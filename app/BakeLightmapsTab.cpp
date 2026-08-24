// Bake Lightmaps tab: export a zone, bake a per-pixel lightmap atlas in Blender,
// reimport the 2nd UV + atlas into the BF. SEPARATE from the vertex-colour RLI bake
// ("Bake lights" tab) — this writes the lightmap uv1 (RLI extra block +4) and a shared
// lightmap texture, leaving the RLI vertex colours untouched. See
// io_ops/lightmap_bake.py and docs/jade_lightmap_baking.md.
#include "BakeLightmapsTab.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

#include "jade/BigFile.hpp"

#include "GuiUtil.hpp"
#include "Theme.hpp"

// ── PORT GAP: worker classes not ported ─────────────────────────────────────
//
// The Python tab runs both actions on a moved-worker QThread (_Signals with
// log/done/fail, _ExportWorker, _ReimportWorker, plus the _run/_finish/
// _on_done/_on_fail plumbing). Neither worker body has a native equivalent,
// so the whole thread scaffold is omitted and the action slots below show a
// "not ported yet" message box instead. For the future port, the Python
// success strings were:
//
// _ExportWorker (decompress_lzo + parse_sub_entries + build_texture_resolver
//   + export_level_to_glb(include_lights=True, lightmap_atlas=size or 4096,
//   lightmap_mode='blender'); keeps man['lmplan'] to pre-fill the reimport
//   step in _on_done):
//   "Exported {objects} meshes + {lights} lights to {basename} (TEXCOORD_1 =
//   cooked vertex index). In Blender: import the GLB (tick Custom Properties),
//   then setup_lightmaps(4096) → add_daylight() → blender_lightmap_bake.py →
//   blender_export_lightmap.py (writes baked_lightmap_uv.json +
//   baked_lightmap.png). Reimport both here."
//
// _ReimportWorker (apply_lightmap_to_bf(bf_path, uv_json, atlas_png,
//   target_size=size, neutralize_rli=neutralize, district_filter=district)):
//   "Lightmap applied: uv1 into {uv1_gaos} GAOs ({uv1_bins} bins) +
//   {atlas_size}² atlas (key 0x{lightmap_key:08X}) into {atlas_bins} bins.
//   Now patch POP3.EXE:  python scripts/lightmap_patch.py lightmapauto
//   {atlas_size}  (self-finds the atlas; no texIdx/sweep)."
//
// _on_done logged "✓ {msg}", _on_fail logged "✗ {msg}".
// ────────────────────────────────────────────────────────────────────────────

BakeLightmapsTab::BakeLightmapsTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(tr(
        "Per-pixel LIGHTMAP for a zone's static geometry (texture × baked lighting), "
        "separate from the vertex-colour RLI bake.\n"
        "1) Export the zone — the GLB carries TEXCOORD_1 = each cooked vertex's INDEX "
        "(so Blender's unwrap maps back exactly, no position matching). "
        "2) In Blender: File ▸ Import glTF (tick Custom Properties), then "
        "blender_lightmap_setup.py (Smart UV Project + Pack Islands) → add_daylight() → "
        "blender_lightmap_bake.py (DIFFUSE) → blender_export_lightmap.py (writes "
        "baked_lightmap_uv.json + baked_lightmap.png). "
        "3) Reimport here: that .json + the PNG.  4) Patch POP3.EXE: "
        "`python scripts/lightmap_patch.py lightmapauto` (self-finds the atlas). "
        "See docs/jade_lightmap_baking.md."));
    intro->setWordWrap(true);
    // color:#aaa → theme dim text (same muted-hint semantics).
    intro->setStyleSheet(
        QStringLiteral("color:%1; font-style:italic;").arg(theme::DIM_TEXT));
    root->addWidget(intro);

    auto* zbox = new QGroupBox(tr("Zone"));
    auto* zl = new QHBoxLayout(zbox);
    zl->addWidget(new QLabel(tr("Zone bin:")));
    zone_combo_ = new QComboBox();
    zone_combo_->setMinimumWidth(360);
    zl->addWidget(zone_combo_, 1);
    root->addWidget(zbox);

    auto* ebox = new QGroupBox(
        tr("1 — Export to Blender (Blender does the per-mesh unwrap)"));
    auto* el = new QHBoxLayout(ebox);
    export_btn_ = new QPushButton(tr("Export zone → .glb"));
    connect(export_btn_, &QPushButton::clicked, this,
            &BakeLightmapsTab::on_export);
    el->addWidget(export_btn_);
    el->addStretch(1);
    root->addWidget(ebox);
    // self._exp_size_combo = None — atlas size is decided in Blender
    // (setup_lightmaps).

    auto* rbox = new QGroupBox(tr("2 — Reimport baked lightmap (uv plan + atlas PNG)"));
    auto* rl = new QVBoxLayout(rbox);
    auto* r1 = new QHBoxLayout();
    uv_btn_ = new QPushButton(tr("Pick baked_lightmap_uv.json…"));
    connect(uv_btn_, &QPushButton::clicked, this, &BakeLightmapsTab::pick_uv);
    r1->addWidget(uv_btn_);
    uv_lbl_ = new QLabel(tr("(none)"));
    // color:#888 → theme dim text.
    uv_lbl_->setStyleSheet(QStringLiteral("color:%1;").arg(theme::DIM_TEXT));
    r1->addWidget(uv_lbl_, 1);
    rl->addLayout(r1);
    auto* r2 = new QHBoxLayout();
    png_btn_ = new QPushButton(tr("Pick baked_lightmap.png…"));
    connect(png_btn_, &QPushButton::clicked, this, &BakeLightmapsTab::pick_png);
    r2->addWidget(png_btn_);
    png_lbl_ = new QLabel(tr("(none)"));
    png_lbl_->setStyleSheet(QStringLiteral("color:%1;").arg(theme::DIM_TEXT));
    r2->addWidget(png_lbl_, 1);
    rl->addLayout(r2);
    auto* r3 = new QHBoxLayout();
    r3->addWidget(new QLabel(tr("Atlas resolution:")));
    size_combo_ = new QComboBox();
    size_combo_->addItem(tr("From plan (auto)"));  // data None → invalid QVariant
    for (int s : {2048, 1024, 256})
        size_combo_->addItem(QStringLiteral("%1 × %2").arg(s).arg(s), s);
    r3->addWidget(size_combo_);
    neutralize_chk_ = new QCheckBox(tr("Neutralize vertex-colour RLI"));
    neutralize_chk_->setChecked(true);
    neutralize_chk_->setToolTip(tr(
        "Set the daytime RLI vertex colours to neutral (128) so the lightmap is the "
        "SOLE lighting (texture × lightmap × 2). Engine does texture × RLI × 2, so 128 "
        "= pass-through (NOT 0 = black). Turn OFF for an AO-only bake — then the AO "
        "multiplies the existing RLI lighting. Destroys the RLI bake; re-bake to restore."));
    r3->addWidget(neutralize_chk_);
    r3->addStretch(1);
    apply_btn_ = new QPushButton(tr("Apply lightmap → BF"));
    connect(apply_btn_, &QPushButton::clicked, this,
            &BakeLightmapsTab::on_reimport);
    r3->addWidget(apply_btn_);
    rl->addLayout(r3);
    root->addWidget(rbox);

    log_ = new QTextEdit();
    log_->setReadOnly(true);
    log_->setFont(QFont(QStringLiteral("Consolas"), 9));
    root->addWidget(log_, 1);

    set_enabled(false);
}

// ── wiring ──

void BakeLightmapsTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                                   const QString& path) {
    bf_ = std::move(bf);
    bf_path_ = path;
    zone_combo_->clear();
    if (bf_ && !bf_->files.empty()) {
        std::vector<std::pair<uint32_t, QString>> zones;
        for (const auto& [idx, fi] : bf_->files)
            if (fi.name.find("_wow_") != std::string::npos)
                zones.emplace_back(idx, qs(fi.name));
        // zones.sort(key=lambda t: t[1] or '') — stable, name-keyed.
        std::stable_sort(
            zones.begin(), zones.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        for (const auto& [idx, name] : zones) zone_combo_->addItem(name, idx);
    }
    set_enabled(zone_combo_->count() > 0);
    log_->append(tr("Loaded %1 zone bins.").arg(zone_combo_->count()));
}

void BakeLightmapsTab::set_enabled(bool on) {
    for (QWidget* w : {static_cast<QWidget*>(export_btn_),
                       static_cast<QWidget*>(zone_combo_),
                       static_cast<QWidget*>(uv_btn_),
                       static_cast<QWidget*>(png_btn_),
                       static_cast<QWidget*>(apply_btn_),
                       static_cast<QWidget*>(size_combo_),
                       static_cast<QWidget*>(neutralize_chk_)})
        w->setEnabled(on);
}

long long BakeLightmapsTab::current_bin() const {
    const QVariant v = zone_combo_->currentData();
    return v.isValid() ? v.toLongLong() : -1;  // None → -1
}

QString BakeLightmapsTab::district_prefix() const {
    // re.match(r'(\d{3})', ...) — anchored at the start.
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral("^(\\d{3})"))
            .match(zone_combo_->currentText());
    return m.hasMatch() ? m.captured(1) : QString();  // null = None
}

// ── actions ──

void BakeLightmapsTab::on_export() {
    const long long idx = current_bin();
    if (idx < 0) return;
    QString base = zone_combo_->currentText();
    if (base.isEmpty()) base = QStringLiteral("zone");
    const QString def =
        base.split(QStringLiteral("_wow_")).first() + QStringLiteral(".glb");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export zone GLB"), def, tr("glTF Binary (*.glb)"));
    if (path.isEmpty()) return;
    // PORT GAP: self._run(_ExportWorker(self._bf, idx, path, None)) —
    // _ExportWorker.run() calls io_ops/level_blender.build_texture_resolver +
    // export_level_to_glb(subs, path, include_lights=True, texture_resolver=...,
    // lightmap_atlas=4096, lightmap_mode='blender', log_fn=...).
    // jade::levelblend::export_level_to_glb (jade/LevelBlender.hpp) exists but
    // explicitly excludes the lightmap_atlas modes (no TEXCOORD_1 =
    // cooked-vertex-index passthrough, no 'lmplan' in the manifest).
    QMessageBox::critical(
        this, tr("Not ported"),
        tr("not ported yet: lightmap-mode zone export "
           "(level_blender.export_level_to_glb with lightmap_mode='blender')"));
}

void BakeLightmapsTab::pick_uv() {
    const QString p = QFileDialog::getOpenFileName(
        this, tr("Lightmap uv plan"), QString(),
        tr("uv plan (*.lmplan.json *.json)"));
    if (!p.isEmpty()) {
        uv_json_ = p;
        uv_lbl_->setText(QFileInfo(p).fileName());
    }
}

void BakeLightmapsTab::pick_png() {
    const QString p = QFileDialog::getOpenFileName(
        this, tr("Baked lightmap atlas"), QString(),
        tr("Image (*.png *.tga *.bmp)"));
    if (!p.isEmpty()) {
        atlas_png_ = p;
        png_lbl_->setText(QFileInfo(p).fileName());
    }
}

void BakeLightmapsTab::on_reimport() {
    if (uv_json_.isEmpty() || atlas_png_.isEmpty()) {
        QMessageBox::information(
            this, tr("Pick both files"),
            tr("Pick the baked uv .json AND the atlas .png first."));
        return;
    }
    const QString dist = district_prefix();
    const QVariant size = size_combo_->currentData();
    const QString size_txt = size.isValid() ? QStringLiteral("%1²").arg(size.toInt())
                                            : tr("plan-sized");
    const bool neutralize = neutralize_chk_->isChecked();
    const QString nz_txt =
        neutralize ? tr(" + neutralize RLI (lightmap = sole lighting)")
                   : tr(" (keep RLI — AO-multiply mode)");
    // The Python f-string renders a None district as 'None'.
    const QString dist_txt = dist.isNull() ? QStringLiteral("None") : dist;
    const QString bf_name = bf_path_.isEmpty()
                                ? QStringLiteral("pop3.bf")
                                : QFileInfo(bf_path_).fileName();
    if (QMessageBox::question(
            this, tr("Apply lightmap"),
            tr("Write the baked lightmap (uv1 + %1 atlas)%2 into every '%3' "
               "district bin?\nThis modifies %4 in place.")
                .arg(size_txt, nz_txt, dist_txt, bf_name)) != QMessageBox::Yes)
        return;
    // PORT GAP: self._run(_ReimportWorker(self._bf_path, self._uv_json,
    // self._atlas_png, dist, size, neutralize)) — _ReimportWorker.run() calls
    // io_ops/lightmap_bake.apply_lightmap_to_bf(bf_path, uv_json, atlas_png,
    // target_size=size, neutralize_rli=neutralize, district_filter=dist,
    // log_fn=...). io_ops/lightmap_bake.py has NO native port (no jade/Lightmap
    // header; jade/LevelBlender.hpp excludes lightmaps from the roadmap).
    QMessageBox::critical(
        this, tr("Not ported"),
        tr("not ported yet: lightmap reimport "
           "(lightmap_bake.apply_lightmap_to_bf)"));
}
