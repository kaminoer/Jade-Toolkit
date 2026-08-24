#include "LevelBlenderTab.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/LevelBlender.hpp"
#include "jade/SubEntry.hpp"

#include "GuiUtil.hpp"
#include "Theme.hpp"

namespace {

// _ExportWorker.
class ExportWorker : public LevelBlenderWorker {
public:
    ExportWorker(std::shared_ptr<jade::BigFile> bf, quint32 bin_idx,
                 QString out_path, bool include_lights, bool srgb_lighting)
        : bf_(std::move(bf)),
          bin_idx_(bin_idx),
          out_path_(std::move(out_path)),
          include_lights_(include_lights),
          srgb_lighting_(srgb_lighting) {}

    void run() override {
        try {
            const jade::LzoResult r =
                jade::decompress_lzo(bf_->read_data(bin_idx_));
            const std::vector<jade::SubEntry> subs =
                jade::parse_sub_entries(r.data);
            const jade::levelblend::ExportManifest man =
                jade::levelblend::export_level_to_glb(
                    subs, out_path_.toStdString(), include_lights_,
                    bf_.get(), srgb_lighting_,
                    [this](const std::string& line) { emit log(qs(line)); });
            if (!man.ok) {
                emit fail(tr("export failed: %1").arg(qs(man.error)));
                return;
            }
            emit done(tr("Exported %1 lit meshes + %2 lights to %3")
                          .arg(man.objects)
                          .arg(man.lights)
                          .arg(QFileInfo(out_path_).fileName()));
        } catch (const std::exception& e) {
            emit fail(tr("export failed: %1").arg(e.what()));
        }
    }

private:
    std::shared_ptr<jade::BigFile> bf_;
    quint32 bin_idx_;
    QString out_path_;
    bool include_lights_, srgb_lighting_;
};

// _ReimportWorker.
class ReimportWorker : public LevelBlenderWorker {
public:
    ReimportWorker(std::shared_ptr<jade::BigFile> bf, QString bf_path,
                   quint32 bin_idx, QString glb_path, QString district,
                   bool refresh_primary, bool srgb_lighting)
        : bf_(std::move(bf)),
          bf_path_(std::move(bf_path)),
          bin_idx_(bin_idx),
          glb_path_(std::move(glb_path)),
          district_(std::move(district)),
          refresh_primary_(refresh_primary),
          srgb_lighting_(srgb_lighting) {}

    void run() override {
        try {
            const jade::LzoResult r =
                jade::decompress_lzo(bf_->read_data(bin_idx_));
            const std::vector<jade::SubEntry> subs =
                jade::parse_sub_entries(r.data);
            const jade::levelblend::BakedImport imp =
                jade::levelblend::import_baked_rli(
                    glb_path_.toStdString(), subs, refresh_primary_,
                    srgb_lighting_,
                    [this](const std::string& line) { emit log(qs(line)); });
            if (!imp.ok) {
                emit fail(tr("reimport failed: %1").arg(qs(imp.error)));
                return;
            }
            if (imp.updates.empty()) {
                emit done(tr("No changed RLI found in the baked GLB (was it "
                             "baked + exported with vertex colors?)."));
                return;
            }
            const jade::levelblend::ApplyStats st =
                jade::levelblend::apply_baked_rli_to_bf(
                    bf_path_.toStdString(), imp.updates,
                    district_.toStdString(),
                    [this](const std::string& line) { emit log(qs(line)); });
            if (!st.ok) {
                emit fail(tr("reimport failed: %1").arg(qs(st.error)));
                return;
            }
            emit done(tr("Applied baked RLI: %1 GAO copies across %2 bins. "
                         "Relaunch the game to see it.")
                          .arg(st.gaos)
                          .arg(st.bins));
        } catch (const std::exception& e) {
            emit fail(tr("reimport failed: %1").arg(e.what()));
        }
    }

private:
    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    quint32 bin_idx_;
    QString glb_path_, district_;
    bool refresh_primary_, srgb_lighting_;
};

// _SampleWorker — read the selected zone bin and summarise its baked-RLI
// palette.
class SampleWorker : public LevelBlenderWorker {
public:
    SampleWorker(std::shared_ptr<jade::BigFile> bf, quint32 bin_idx)
        : bf_(std::move(bf)), bin_idx_(bin_idx) {}

    void run() override {
        try {
            const jade::LzoResult r =
                jade::decompress_lzo(bf_->read_data(bin_idx_));
            const std::vector<jade::SubEntry> subs =
                jade::parse_sub_entries(r.data);
            stats_ = jade::sample_zone(subs);
            emit result();
            emit done(stats_.ok
                          ? tr("Sampled %1 lit vertices.").arg(stats_.count)
                          : tr("No baked lighting found in this zone."));
        } catch (const std::exception& e) {
            emit fail(tr("sample failed: %1").arg(e.what()));
        }
    }

private:
    std::shared_ptr<jade::BigFile> bf_;
    quint32 bin_idx_;
};

// _RescaleWorker — apply a uniform atmosphere grade to the district's
// baked RLI (and optionally the dynamic lights and unlit textures) in the
// BF.
class RescaleWorker : public LevelBlenderWorker {
public:
    RescaleWorker(QString bf_path, double brightness,
                  std::array<int, 3> tint, double contrast,
                  QString district, bool grade_lights, bool grade_textures)
        : bf_path_(std::move(bf_path)),
          brightness_(brightness),
          tint_(tint),
          contrast_(contrast),
          district_(std::move(district)),
          grade_lights_(grade_lights),
          grade_textures_(grade_textures) {}

    void run() override {
        const jade::RescaleStats r = jade::rescale_zone_in_bf(
            bf_path_.toStdString(), brightness_, tint_, contrast_,
            district_.toStdString(), grade_lights_, grade_textures_);
        if (!r.ok) {
            emit fail(tr("rescale failed: %1").arg(qs(r.error)));
            return;
        }
        if (!(r.gaos || r.lights || r.textures)) {
            emit done(tr("No data changed (identity grade?)."));
        } else {
            QStringList parts{tr("%1 RLI GAOs").arg(r.gaos)};
            if (grade_lights_) parts << tr("%1 lights").arg(r.lights);
            if (grade_textures_) parts << tr("%1 textures").arg(r.textures);
            emit done(tr("Graded %1 across %2 bins. Relaunch the game to "
                         "see it.")
                          .arg(parts.join(QStringLiteral(", ")))
                          .arg(r.bins));
        }
    }

private:
    QString bf_path_;
    double brightness_;
    std::array<int, 3> tint_;
    double contrast_;
    QString district_;
    bool grade_lights_, grade_textures_;
};

}  // namespace

// ── PalettePreview ──

PalettePreview::PalettePreview(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(118);
}

void PalettePreview::set_data(const jade::PaletteStats& stats,
                              jade::RliColorFn fn) {
    stats_ = stats;
    fn_ = std::move(fn);
    update();
}

void PalettePreview::draw_row(QPainter& p, int y, int rowH,
                              const QString& title,
                              const jade::PaletteStats& st) {
    const int w = width();
    const int m = 8, labelW = 46, avgW = 64, gap = 10;
    p.setPen(QColor(205, 205, 205));
    p.drawText(QRect(m, y, labelW, rowH), Qt::AlignVCenter | Qt::AlignLeft,
               title);
    const int x = m + labelW;
    const auto& a = st.average;
    const QColor ac{int(a[0]), int(a[1]), int(a[2])};
    const QRect ar(x, y, avgW, rowH);
    p.fillRect(ar, ac);
    p.setPen(QColor(90, 90, 90));
    p.drawRect(ar);
    p.setPen(a[0] + a[1] + a[2] < 330 ? QColor(235, 235, 235)
                                      : QColor(15, 15, 15));
    p.drawText(ar, Qt::AlignCenter,
               QStringLiteral("%1,%2,%3")
                   .arg(int(a[0]))
                   .arg(int(a[1]))
                   .arg(int(a[2])));
    const int sx = x + avgW + gap;
    const int sw = w - m - sx;
    const auto& pct = st.percentiles;
    const double bw = double(sw) / std::max<size_t>(1, pct.size());
    for (size_t i = 0; i < pct.size(); ++i)
        p.fillRect(QRect(int(sx + double(i) * bw), y, int(bw) + 1, rowH),
                   QColor(pct[i][0], pct[i][1], pct[i][2]));
    p.setPen(QColor(90, 90, 90));
    p.drawRect(QRect(sx, y, sw, rowH));
}

void PalettePreview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int w = width(), h = height();
    p.fillRect(0, 0, w, h, QColor(32, 32, 32));
    if (!stats_.ok) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(QRect(0, 0, w, h), Qt::AlignCenter,
                   tr("Click \"Sample zone\" to read the current baked "
                      "lighting."));
        return;
    }
    const int m = 8, gap = 12;
    const int rowH = (h - 2 * m - gap) / 2;
    draw_row(p, m, rowH, tr("Now"), stats_);
    const jade::PaletteStats aft =
        fn_ ? jade::preview_after(stats_, fn_) : stats_;
    draw_row(p, m + rowH + gap, rowH, tr("After"), aft);
}

// ── LevelBlenderTab ──

LevelBlenderTab::LevelBlenderTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Re-light a zone's static geometry with Blender Cycles.\n"
           "1) Export the zone.  2) In Blender: File > Import glTF, then run "
           "scripts/blender_level_setup.py — preview_baked_lighting() shows "
           "the current engine look (the daytime is baked RLI, not the "
           "accent lights); prepare_for_relight() sets up materials/radii so "
           "you can add a Sun + sky and bake with blender_bake_rli.py.  "
           "3) Reimport the baked vertex colours."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color:%1; font-style:italic;")
                             .arg(theme::DIM_TEXT));
    root->addWidget(intro);

    auto* zbox = new QGroupBox(tr("Zone"));
    auto* zl = new QHBoxLayout(zbox);
    zl->addWidget(new QLabel(tr("Zone bin:")));
    zone_combo_ = new QComboBox();
    zone_combo_->setMinimumWidth(360);
    zl->addWidget(zone_combo_, 1);
    root->addWidget(zbox);

    auto* ebox = new QGroupBox(tr("1 — Export to Blender"));
    auto* el = new QHBoxLayout(ebox);
    lights_chk_ = new QCheckBox(tr("Include zone lights"));
    lights_chk_->setChecked(true);
    el->addWidget(lights_chk_);
    // Experimental: convert the engine's display-space RLI to linear on
    // export and back on reimport so Blender DISPLAYS/BAKES lighting at the
    // right brightness. Must match between export and reimport. Off =
    // byte-exact raw.
    srgb_chk_ = new QCheckBox(tr("sRGB-correct lighting (experimental)"));
    srgb_chk_->setChecked(false);
    srgb_chk_->setToolTip(
        tr("Convert RLI <-> linear so Blender shows/bakes the right "
           "brightness.\nUse the SAME setting for export and reimport. "
           "Validate in-game — the\nengine's exact RLI colour space is "
           "inferred, not yet confirmed."));
    el->addWidget(srgb_chk_);
    export_btn_ = new QPushButton(tr("Export zone → .glb"));
    connect(export_btn_, &QPushButton::clicked, this,
            &LevelBlenderTab::on_export);
    el->addWidget(export_btn_);
    el->addStretch(1);
    root->addWidget(ebox);

    auto* rbox = new QGroupBox(tr("2 — Reimport baked lighting"));
    auto* rl = new QHBoxLayout(rbox);
    primary_chk_ = new QCheckBox(tr("Refresh primary table too"));
    primary_chk_->setChecked(true);
    rl->addWidget(primary_chk_);
    reimport_btn_ = new QPushButton(tr("Reimport baked .glb → BF"));
    connect(reimport_btn_, &QPushButton::clicked, this,
            &LevelBlenderTab::on_reimport);
    rl->addWidget(reimport_btn_);
    rl->addStretch(1);
    root->addWidget(rbox);

    // ── Rescale existing baked lighting (no Blender) ──
    auto* gbox = new QGroupBox(tr("Rescale baked lighting"));
    auto* gl = new QGridLayout(gbox);
    auto* hint = new QLabel(
        tr("Pull the zone's existing baked lighting toward a mood (dusk, "
           "night, warm/cold) by scaling the vertex colors directly. Sample "
           "the zone, dial brightness / tint / contrast and watch the "
           "Now→After swatches, then apply to the zone bin."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1; font-style:italic;")
                            .arg(theme::DIM_TEXT));
    gl->addWidget(hint, 0, 0, 1, 4);

    preview_ = new PalettePreview();
    gl->addWidget(preview_, 1, 0, 1, 4);

    // brightness
    gl->addWidget(new QLabel(tr("Brightness")), 2, 0);
    bright_ = new QSlider(Qt::Horizontal);
    bright_->setRange(0, 300);
    bright_->setValue(100);
    connect(bright_, &QSlider::valueChanged, this,
            &LevelBlenderTab::refresh_preview);
    gl->addWidget(bright_, 2, 1, 1, 2);
    bright_lbl_ = new QLabel(QStringLiteral("100%"));
    bright_lbl_->setMinimumWidth(46);
    gl->addWidget(bright_lbl_, 2, 3);

    // contrast
    gl->addWidget(new QLabel(tr("Contrast")), 3, 0);
    contrast_ = new QSlider(Qt::Horizontal);
    contrast_->setRange(50, 200);
    contrast_->setValue(100);
    connect(contrast_, &QSlider::valueChanged, this,
            &LevelBlenderTab::refresh_preview);
    gl->addWidget(contrast_, 3, 1, 1, 2);
    contrast_lbl_ = new QLabel(QStringLiteral("1.00"));
    gl->addWidget(contrast_lbl_, 3, 3);

    // also-grade toggles
    lights_grade_chk_ = new QCheckBox(tr("Also tint dynamic (GRO) lights"));
    lights_grade_chk_->setToolTip(
        tr("Apply the same grade to the zone's GRO_Light colours so "
           "Prince/NPCs and other dynamically-lit objects match the new "
           "mood."));
    gl->addWidget(lights_grade_chk_, 4, 0, 1, 2);
    tex_grade_chk_ =
        new QCheckBox(tr("Also shade unlit textures (skybox/background)"));
    tex_grade_chk_->setToolTip(
        tr("Unlit STATIC background surfaces (skydome, backdrops, glow "
           "planes — cooked geometry with an all-zero RLI table) show their "
           "texture directly. Scale those to the same shade so they don't "
           "stay daytime-bright. Skinned meshes and FX/particle emitters "
           "(fire, smoke, sparks) are NOT touched — they're dynamic-lit / "
           "emissive."));
    gl->addWidget(tex_grade_chk_, 4, 2, 1, 2);

    // tint + sample + reset + apply
    auto* row = new QHBoxLayout();
    tint_btn_ = new QPushButton(tr("Tint color…"));
    connect(tint_btn_, &QPushButton::clicked, this,
            &LevelBlenderTab::pick_tint);
    row->addWidget(tint_btn_);
    tint_swatch_ = new QLabel();
    tint_swatch_->setFixedSize(28, 20);
    update_tint_swatch();
    row->addWidget(tint_swatch_);
    reset_btn_ = new QPushButton(tr("Reset"));
    connect(reset_btn_, &QPushButton::clicked, this,
            &LevelBlenderTab::reset_grade);
    row->addWidget(reset_btn_);
    row->addStretch(1);
    sample_btn_ = new QPushButton(tr("Sample zone"));
    connect(sample_btn_, &QPushButton::clicked, this,
            &LevelBlenderTab::on_sample);
    row->addWidget(sample_btn_);
    apply_btn_ = new QPushButton(tr("Apply grade → BF"));
    connect(apply_btn_, &QPushButton::clicked, this,
            &LevelBlenderTab::on_rescale);
    row->addWidget(apply_btn_);
    gl->addLayout(row, 5, 0, 1, 4);
    root->addWidget(gbox);

    log_ = new QTextEdit();
    log_->setReadOnly(true);
    log_->setFont(QFont(QStringLiteral("Consolas"), 9));
    root->addWidget(log_, 1);

    connect(zone_combo_, &QComboBox::currentIndexChanged, this,
            &LevelBlenderTab::on_zone_changed);
    refresh_preview();
    set_enabled(false);
}

// ── wiring ──

void LevelBlenderTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                                  const QString& path) {
    bf_ = std::move(bf);
    bf_path_ = path;
    zone_combo_->clear();
    if (bf_ && !bf_->files.empty()) {
        std::vector<std::pair<quint32, QString>> zones;
        for (const auto& kv : bf_->files)
            if (kv.second.name.find("_wow_") != std::string::npos)
                zones.push_back({kv.first, qs(kv.second.name)});
        std::stable_sort(zones.begin(), zones.end(),
                         [](const auto& a, const auto& b) {
                             return a.second < b.second;
                         });
        for (const auto& [idx, name] : zones)
            zone_combo_->addItem(name, idx);
    }
    set_enabled(zone_combo_->count() > 0);
    log_->append(tr("Loaded %1 zone bins.").arg(zone_combo_->count()));
}

void LevelBlenderTab::set_project(ProjectDoc* proj) { project_ = proj; }

void LevelBlenderTab::receive_asset(quint32, quint32) {}

void LevelBlenderTab::set_enabled(bool on) {
    for (QWidget* w : std::initializer_list<QWidget*>{
             export_btn_, reimport_btn_, zone_combo_, sample_btn_,
             apply_btn_, tint_btn_, reset_btn_, bright_, contrast_,
             lights_grade_chk_, tex_grade_chk_})
        w->setEnabled(on);
}

long long LevelBlenderTab::current_bin() const {
    const QVariant v = zone_combo_->currentData();
    return v.isValid() ? v.toUInt() : -1;
}

QString LevelBlenderTab::district_prefix() const {
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral("^(\\d{3})"))
            .match(zone_combo_->currentText());
    return m.hasMatch() ? m.captured(1) : QString();
}

// ── actions ──

void LevelBlenderTab::on_export() {
    const long long idx = current_bin();
    if (idx < 0) return;
    const QString cur = zone_combo_->currentText().isEmpty()
                            ? QStringLiteral("zone")
                            : zone_combo_->currentText();
    const QString def = cur.split(QStringLiteral("_wow_")).first()
                        + QStringLiteral(".glb");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export zone GLB"), def, tr("glTF Binary (*.glb)"));
    if (path.isEmpty()) return;
    run(new ExportWorker(bf_, quint32(idx), path, lights_chk_->isChecked(),
                         srgb_chk_->isChecked()));
}

void LevelBlenderTab::on_reimport() {
    const long long idx = current_bin();
    if (idx < 0) return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Baked zone GLB or RLI dump"), QString(),
        tr("Baked RLI (*.glb *.json);;glTF Binary (*.glb);;RLI dump "
           "(*.json)"));
    if (path.isEmpty()) return;
    const QString dist = district_prefix();
    if (QMessageBox::question(
            this, tr("Reimport baked RLI"),
            tr("Write baked lighting into the BF for every '%1' district "
               "bin?\nThis modifies %2 in place.")
                .arg(dist,
                     bf_path_.isEmpty() ? QStringLiteral("pop3.bf")
                                        : QFileInfo(bf_path_).fileName()))
        != QMessageBox::Yes)
        return;
    run(new ReimportWorker(bf_, bf_path_, quint32(idx), path, dist,
                           primary_chk_->isChecked(),
                           srgb_chk_->isChecked()));
}

// ── rescale (atmosphere grade) ──

jade::RliColorFn LevelBlenderTab::current_transform() const {
    return jade::make_transform(bright_->value() / 100.0, tint_,
                                contrast_->value() / 100.0);
}

void LevelBlenderTab::refresh_preview() {
    bright_lbl_->setText(QStringLiteral("%1%").arg(bright_->value()));
    contrast_lbl_->setText(
        QStringLiteral("%1").arg(contrast_->value() / 100.0, 0, 'f', 2));
    preview_->set_data(stats_, current_transform());
}

void LevelBlenderTab::update_tint_swatch() {
    tint_swatch_->setStyleSheet(
        QStringLiteral("background-color: rgb(%1,%2,%3); border:1px solid %4;")
            .arg(tint_[0])
            .arg(tint_[1])
            .arg(tint_[2])
            .arg(theme::BORDER));
}

void LevelBlenderTab::pick_tint() {
    const QColor col = QColorDialog::getColor(
        QColor(tint_[0], tint_[1], tint_[2]), this, tr("Tint colour"));
    if (col.isValid()) {
        tint_ = {col.red(), col.green(), col.blue()};
        update_tint_swatch();
        refresh_preview();
    }
}

void LevelBlenderTab::reset_grade() {
    bright_->setValue(100);
    contrast_->setValue(100);
    tint_ = {255, 255, 255};
    update_tint_swatch();
    refresh_preview();
}

void LevelBlenderTab::on_zone_changed() {
    stats_ = jade::PaletteStats();  // palette is per-zone; force a re-sample
    refresh_preview();
}

void LevelBlenderTab::on_sample() {
    const long long idx = current_bin();
    if (idx < 0) return;
    run(new SampleWorker(bf_, quint32(idx)));
}

void LevelBlenderTab::on_sample_result() {
    if (worker_) stats_ = worker_->take_stats();
    refresh_preview();
}

void LevelBlenderTab::on_rescale() {
    const long long idx = current_bin();
    if (idx < 0) return;
    if (!stats_.ok) {
        QMessageBox::information(
            this, tr("Sample first"),
            tr("Click \"Sample zone\" first so you can preview the grade."));
        return;
    }
    const double b = bright_->value() / 100.0;
    const double c = contrast_->value() / 100.0;
    if (std::abs(b - 1.0) < 1e-6 && c == 1.0
        && tint_ == std::array<int, 3>{255, 255, 255}) {
        QMessageBox::information(
            this, tr("Nothing to apply"),
            tr("The grade is identity (100% / white / 1.00) — adjust it "
               "first."));
        return;
    }
    const QString dist = district_prefix();
    const bool gl_lights = lights_grade_chk_->isChecked();
    const bool gl_tex = tex_grade_chk_->isChecked();
    QStringList extra;
    if (gl_lights) extra << tr("dynamic lights");
    if (gl_tex) extra << tr("unlit textures");
    const QString extra_txt =
        extra.isEmpty() ? QString()
                        : QStringLiteral(" + ")
                              + extra.join(QStringLiteral(" + "));
    if (QMessageBox::question(
            this, tr("Apply atmosphere grade"),
            tr("Scale baked lighting%1 (brightness %2%, tint rgb(%3, %4, "
               "%5), contrast %6) for every '%7' district bin?\nThis "
               "modifies %8 in place.")
                .arg(extra_txt)
                .arg(bright_->value())
                .arg(tint_[0])
                .arg(tint_[1])
                .arg(tint_[2])
                .arg(c, 0, 'f', 2)
                .arg(dist,
                     bf_path_.isEmpty() ? QStringLiteral("pop3.bf")
                                        : QFileInfo(bf_path_).fileName()))
        != QMessageBox::Yes)
        return;
    run(new RescaleWorker(bf_path_, b, tint_, c, dist, gl_lights, gl_tex));
}

void LevelBlenderTab::run(LevelBlenderWorker* worker) {
    set_enabled(false);
    worker_ = worker;
    connect(worker, &LevelBlenderWorker::log, log_, &QTextEdit::append);
    connect(worker, &LevelBlenderWorker::result, this,
            &LevelBlenderTab::on_sample_result);
    connect(worker, &LevelBlenderWorker::done, this,
            &LevelBlenderTab::on_done);
    connect(worker, &LevelBlenderWorker::fail, this,
            &LevelBlenderTab::on_fail);
    worker->start();
}

void LevelBlenderTab::finish() {
    if (worker_) {
        worker_->quit();
        worker_->wait();
        worker_->deleteLater();
        worker_ = nullptr;
    }
    set_enabled(true);
}

void LevelBlenderTab::on_done(const QString& msg) {
    log_->append(QStringLiteral("✓ %1").arg(msg));
    finish();
}

void LevelBlenderTab::on_fail(const QString& msg) {
    log_->append(QStringLiteral("✗ %1").arg(msg));
    finish();
}
