#include "MeshSwapTab.hpp"

#include "MeshSwapResolver.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/GltfBuilder.hpp"
#include "jade/Material.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/PatcherModel.hpp"
#include "jade/Rli.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

#include "BoneRemapDialog.hpp"
#include "ElementThumb.hpp"
#include "GuiUtil.hpp"
#include "MaterialFlagsDialog.hpp"
#include "MaterialPickerDialog.hpp"
#include "ProjectDoc.hpp"
#include "TexturePickerDialog.hpp"
#include "Theme.hpp"
#include "Tooltips.hpp"

using jade::json::Value;
using jade::json::make_bool;
using jade::json::make_num;
using jade::json::make_obj;
using jade::json::make_str;

namespace {

// Short per-vertex-colour tag for a geo-list label (_vcol_tag). RLI = host
// GAO's baked lighting (what renders on static meshes); GEO = the GEO body
// dul_PointColors.
QString vcol_tag(bool geo_colors, bool rli_colors) {
    if (geo_colors && rli_colors) return QStringLiteral("vcol: GEO+RLI");
    if (rli_colors) return QStringLiteral("vcol: RLI");
    if (geo_colors) return QStringLiteral("vcol: GEO");
    return QStringLiteral("vcol: none");
}

// asset_add.allocate_modded_key: a fresh 0x7A-range key not in used_keys.
quint32 allocate_modded_key(const std::set<quint32>& used, quint32 seed = 0) {
    QRandomGenerator rng =
        seed ? QRandomGenerator(seed) : QRandomGenerator::securelySeeded();
    for (int i = 0; i < (1 << 16); ++i) {
        const quint32 key =
            0x7A000000u | rng.bounded(1u, 0x00FFFFFFu);
        if (!used.count(key)) return key;
    }
    return 0x7A000001u;  // modded key space exhausted (?)
}

// _shared_prefix_bits is unused by the ported resolver (the override is
// gated to the single TT Prince-loading stub) — kept out deliberately.

// hex-string field of an op's params (0 when absent).
quint32 params_key(const Value& op, const char* field) {
    const Value* prm = op.find("params");
    const Value* v = prm ? prm->find(field) : nullptr;
    if (!v) return 0;
    if (v->is_str())
        return QString::fromStdString(v->str).toUInt(nullptr, 16);
    if (v->is_num()) return quint32(v->num);
    return 0;
}

}  // namespace


// ── workers ──

void MeshSwapExportGlbWorker::run() {
    try {
        jade::BigFile bf;
        bf.open(bf_path_.toStdString());
        const jade::LzoResult r =
            jade::decompress_lzo(bf.read_data(entry_idx_));
        if (!r.ok) throw std::runtime_error("Could not decompress BF entry");
        const std::vector<jade::SubEntry> subs =
            jade::walk_sub_entries(r.data);
        const jade::SubEntry* sub = jade::pick_geo_sub(subs, geo_key_);
        if (!sub)
            throw std::runtime_error("No .geo sub-entry with that key");
        const jade::GeoInfo geo =
            jade::parse_geometry(sub->data.data(), sub->data.size());
        if (!geo.ok) throw std::runtime_error("Could not parse geometry");
        const std::string hex = hex_key_lower(geo_key_);
        const std::vector<uint8_t> glb =
            jade::gltfbuild::build_geo_model_glb(
                geo, "geo_" + hex, hex, "geo_" + hex);
        if (glb.empty())
            throw std::runtime_error("GEO has no vertices/faces to export");
        QFile f(out_path_);
        if (!f.open(QIODevice::WriteOnly)
            || f.write(reinterpret_cast<const char*>(glb.data()),
                       qint64(glb.size()))
                   != qint64(glb.size()))
            throw std::runtime_error("could not write GLB file");
        emit finished(true,
                      tr("OK — exported %1v/%2tri (%3 bones) to %4")
                          .arg(geo.nb_points)
                          .arg(geo.nb_tris)
                          .arg(geo.skin_nbones)
                          .arg(out_path_));
    } catch (const std::exception& e) {
        emit finished(false, QString::fromUtf8(e.what()));
    }
}

void MeshSwapExportGlbHierWorker::run() {
    try {
        jade::BigFile bf;
        bf.open(bf_path_.toStdString());
        const jade::LzoResult r =
            jade::decompress_lzo(bf.read_data(entry_idx_));
        if (!r.ok) throw std::runtime_error("Could not decompress BF entry");
        const std::vector<jade::SubEntry> subs =
            jade::walk_sub_entries(r.data);
        const jade::gltfbuild::HierExportResult res =
            jade::gltfbuild::build_hierarchical_geo_glb(subs, geo_key_);
        if (!res.ok) throw std::runtime_error(res.error);
        QFile f(out_path_);
        if (!f.open(QIODevice::WriteOnly)
            || f.write(reinterpret_cast<const char*>(res.glb.data()),
                       qint64(res.glb.size()))
                   != qint64(res.glb.size()))
            throw std::runtime_error("could not write GLB file");
        emit finished(
            true, tr("OK — exported %1v/%2tri (%3 bones, %4 root) with "
                     "hierarchy to %5")
                      .arg(res.points)
                      .arg(res.tris)
                      .arg(res.bones)
                      .arg(res.roots)
                      .arg(out_path_));
    } catch (const std::exception& e) {
        emit finished(false, QString::fromUtf8(e.what()));
    }
}

// ── MeshSwapTab ──

MeshSwapTab::MeshSwapTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    // Header
    bf_label_ = new QLabel(tr("No BigFile loaded."));
    root->addWidget(bf_label_);

    // Entry selection
    auto* entry_group = new QGroupBox(
        tr("Step 1 — Pick a BF entry (filtered to *wow* files)"));
    auto* entry_form = new QFormLayout(entry_group);
    auto* row = new QHBoxLayout();
    entry_combo_ = new QComboBox();
    entry_combo_->setMinimumWidth(420);
    row->addWidget(entry_combo_, 1);
    list_btn_ = new QPushButton(tr("List meshes"));
    connect(list_btn_, &QPushButton::clicked, this, &MeshSwapTab::on_list);
    row->addWidget(list_btn_);
    entry_form->addRow(tr("Entry:"), row);
    root->addWidget(entry_group);

    // Geo selection
    auto* geo_group = new QGroupBox(tr("Step 2 — Pick a target .geo"));
    auto* geo_form = new QFormLayout(geo_group);
    auto* geo_row = new QHBoxLayout();
    geo_combo_ = new QComboBox();
    geo_combo_->setMinimumWidth(420);
    geo_row->addWidget(geo_combo_, 1);
    export_glb_btn_ = new QPushButton(tr("Export to GLB…"));
    export_glb_btn_->setToolTip(
        tr("Save the selected .geo as a standalone GLB file (with "
           "skeleton)\nso you can edit it in Blender / Maya as a starting "
           "point."));
    connect(export_glb_btn_, &QPushButton::clicked, this,
            &MeshSwapTab::on_export_glb);
    geo_row->addWidget(export_glb_btn_);
    export_glb_hier_btn_ = new QPushButton(tr("Export to GLB (hierarchy)…"));
    export_glb_hier_btn_->setToolTip(
        tr("Save the selected SKINNED .geo as a GLB with the real bone\n"
           "hierarchy (parent-child tree recovered from the GAO father "
           "links),\nrest-posed at the origin so it matches the plain "
           "export's placement.\nBetter base for editing an animated "
           "character's rig in Blender.\nStatic (unskinned) meshes: use "
           "the plain 'Export to GLB…' instead."));
    connect(export_glb_hier_btn_, &QPushButton::clicked, this,
            &MeshSwapTab::on_export_glb_hier);
    geo_row->addWidget(export_glb_hier_btn_);
    geo_form->addRow(tr("Target geo:"), geo_row);
    root->addWidget(geo_group);

    // Material / texture breakdown of the selected geo
    auto* mat_group =
        new QGroupBox(tr("Materials & textures of the selected geo"));
    auto* mat_lay = new QVBoxLayout(mat_group);
    mat_table_ = new QTableWidget(0, 6);
    mat_table_->setHorizontalHeaderLabels({tr("Element"), tr("matId"),
                                           tr("Tris"), tr("Material"),
                                           tr("Texture"), tr("Render")});
    mat_table_->verticalHeader()->setVisible(false);
    mat_table_->setEditTriggers(QTableWidget::NoEditTriggers);
    mat_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    mat_table_->setMaximumHeight(190);
    QHeaderView* mh = mat_table_->horizontalHeader();
    // User-resizable columns; seed sensible default widths; the last
    // section stretches to fill the table.
    for (int c = 0; c < 6; ++c)
        mh->setSectionResizeMode(c, QHeaderView::Interactive);
    const int widths[6] = {60, 50, 55, 190, 240, 120};
    for (int c = 0; c < 6; ++c) mat_table_->setColumnWidth(c, widths[c]);
    mh->setStretchLastSection(true);
    mat_table_->setMouseTracking(true);
    connect(mat_table_, &QTableWidget::cellEntered, this,
            &MeshSwapTab::on_mat_cell_hover);
    mat_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mat_table_, &QTableWidget::customContextMenuRequested, this,
            &MeshSwapTab::on_mat_context_menu);
    mat_lay->addWidget(mat_table_);
    mat_lay->addWidget(new QLabel(
        tr("<i>Hover a texture for a preview (every layer) · hover Render "
           "for blend/alpha-test flags · right-click for per-layer "
           "replace/retarget.</i>")));
    root->addWidget(mat_group);
    connect(geo_combo_, &QComboBox::currentIndexChanged, this,
            &MeshSwapTab::refresh_materials);

    // Source GLB
    auto* glb_group = new QGroupBox(tr("Step 3 — Source GLB"));
    auto* glb_lay = new QHBoxLayout(glb_group);
    glb_edit_ = new QLineEdit();
    glb_edit_->setPlaceholderText(
        tr("Path to GLB file (first mesh is used)"));
    glb_lay->addWidget(glb_edit_, 1);
    auto* browse_btn = new QPushButton(tr("Browse…"));
    connect(browse_btn, &QPushButton::clicked, this,
            &MeshSwapTab::on_browse_glb);
    glb_lay->addWidget(browse_btn);
    root->addWidget(glb_group);

    // Options
    vcolor_check_ = new QCheckBox(
        tr("Import vertex colors from GLB (COLOR_0 → dul_PointColors)"));
    vcolor_check_->setToolTip(
        tr("Write the GLB's per-vertex COLOR_0 into the GEO's vertex-color\n"
           "array (D3DCOLOR ARGB). These drive the engine's pVB_Color and\n"
           "modulate the material (baked lighting / tint on static "
           "meshes).\nOff by default: a swap without this writes no colors, "
           "so a mesh\nthat originally had baked vertex lighting renders "
           "flat.\nIf the GLB has no COLOR_0, the original colors are kept "
           "when the\nvertex count is unchanged."));
    root->addWidget(vcolor_check_);

    // Add-to-project row
    auto* apply_row = new QHBoxLayout();
    project_hint_ = new QLabel(
        tr("<i>Open or create a Mod Project (File menu) to record "
           "edits.</i>"));
    project_hint_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    apply_row->addWidget(project_hint_, 1);
    disable_btn_ = new QPushButton(tr("Add stub-out op"));
    disable_btn_->setMinimumWidth(160);
    disable_btn_->setToolTip(
        tr("Append a stub_mesh operation: replace the selected geo with "
           "the\n48-byte null template at build time. The mesh becomes "
           "invisible\nin-game without breaking the BF — non-destructive."));
    connect(disable_btn_, &QPushButton::clicked, this,
            &MeshSwapTab::on_stub);
    apply_row->addWidget(disable_btn_);
    apply_btn_ = new QPushButton(tr("Add mesh-swap op"));
    apply_btn_->setMinimumWidth(160);
    apply_tip_default_ = tr(
        "Append a replace_mesh operation: rebuild the selected GEO from "
        "the GLB at build time. The GLB is copied into the project's asset "
        "store. Bone-mapping warnings appear during build.");
    apply_btn_->setToolTip(apply_tip_default_);
    connect(apply_btn_, &QPushButton::clicked, this,
            &MeshSwapTab::on_apply);
    apply_row->addWidget(apply_btn_);
    root->addLayout(apply_row);
    // Re-evaluate the over-u16 guard (see update_enabled) whenever the GLB
    // path changes — typed or set via Browse.
    connect(glb_edit_, &QLineEdit::textChanged, this,
            [this] { update_enabled(); });

    // Log
    log_ = new QTextEdit();
    log_->setReadOnly(true);
    log_->setFont(QFont(QStringLiteral("Consolas"), 9));
    root->addWidget(log_, 1);

    update_enabled();
}

MeshSwapTab::~MeshSwapTab() {
    if (worker_thread_) {
        worker_thread_->quit();
        worker_thread_->wait();
    }
}

void MeshSwapTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                              const QString& path) {
    bf_ = std::move(bf);
    bf_path_ = path;
    asset_index_.reset();  // re-supplied when the new scan finishes
    geo_key_idx_cache_valid_ = false;
    geo_key_idx_cache_.clear();
    resolver_cache_.clear();
    bf_label_->setText(tr("BigFile: %1").arg(path));
    geo_combo_->clear();
    geo_rows_.clear();
    populate_entry_combo();
    log_->append(tr("Loaded BigFile: %1").arg(path));
    update_enabled();
}

void MeshSwapTab::set_asset_index(std::shared_ptr<jade::AssetIndex> idx) {
    asset_index_ = std::move(idx);
    resolver_cache_.clear();
    // Re-resolve the visible table now that cross-bin lookup is possible.
    if (bf_ && geo_combo_->count()) refresh_materials();
}

void MeshSwapTab::set_project(ProjectDoc* proj) {
    project_ = proj;
    if (!proj)
        project_hint_->setText(
            tr("<i>Open or create a Mod Project (File menu) to record "
               "edits.</i>"));
    else
        project_hint_->setText(
            tr("<i>Recording into project: <b>%1</b></i>")
                .arg(proj->name.isEmpty() ? tr("(unnamed)") : proj->name));
    update_enabled();
}

// Fill the entry combo with files whose name contains 'wow' — the actor
// shape files that hold character/prop meshes. If a BF has none, the combo
// falls back to showing every entry so the user isn't stuck.
void MeshSwapTab::populate_entry_combo() {
    entry_combo_->blockSignals(true);
    entry_combo_->clear();
    if (!bf_ || bf_->files.empty()) {
        entry_combo_->blockSignals(false);
        return;
    }

    std::vector<std::pair<quint32, QString>> rows;
    for (const auto& [idx, fi] : bf_->files) {
        if (fi.name.empty()) continue;
        if (qs(fi.name).toLower().contains(QLatin1String("wow")))
            rows.push_back({idx, qs(fi.name)});
    }
    bool used_fallback = false;
    if (rows.empty()) {
        used_fallback = true;
        for (const auto& [idx, fi] : bf_->files)
            if (!fi.name.empty()) rows.push_back({idx, qs(fi.name)});
    }

    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) {
                         return a.second.toLower() < b.second.toLower();
                     });
    for (const auto& [idx, name] : rows)
        entry_combo_->addItem(QStringLiteral("%1  (#%2)").arg(name).arg(idx),
                              idx);

    // Prefer the Dark Prince shape if present.
    for (int i = 0; i < entry_combo_->count(); ++i)
        if (entry_combo_->itemData(i).toUInt() == 1912u) {
            entry_combo_->setCurrentIndex(i);
            break;
        }

    entry_combo_->blockSignals(false);
    if (used_fallback)
        log_->append(tr("  no entries matching 'wow' — showing all %1 "
                        "named entries")
                         .arg(rows.size()));
    else
        log_->append(
            tr("  found %1 BF entries matching 'wow'").arg(rows.size()));
}

long long MeshSwapTab::current_entry_idx() const {
    const QVariant v = entry_combo_->currentData();
    return v.isValid() ? v.toUInt() : -1;
}

void MeshSwapTab::set_entry(quint32 entry_idx) {
    for (int i = 0; i < entry_combo_->count(); ++i)
        if (entry_combo_->itemData(i).toUInt() == entry_idx) {
            entry_combo_->setCurrentIndex(i);
            return;
        }
}

void MeshSwapTab::receive_asset(quint32 parent_index, quint32 sub_key) {
    set_entry(parent_index);
    on_list();
    for (int i = 0; i < geo_combo_->count(); ++i)
        if (geo_combo_->itemData(i).toUInt() == sub_key) {
            geo_combo_->setCurrentIndex(i);
            return;
        }
}

// Render-vertex count of the GLB's first mesh — matches exactly what the
// build checks. Cached by (path, mtime). -1 == unknown (callers do NOT
// disable on unknown).
long long MeshSwapTab::glb_render_vert_count(const QString& path) {
    if (path.isEmpty() || !QFileInfo(path).isFile()) return -1;
    const qint64 mtime =
        QFileInfo(path).lastModified().toMSecsSinceEpoch();
    auto it = glb_vcount_cache_.find(path);
    if (it != glb_vcount_cache_.end() && it->second.first == mtime)
        return it->second.second;
    long long n = -1;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray raw = f.readAll();
        const std::vector<jade::patcher::GlbPatchMesh> meshes =
            jade::patcher::parse_glb_meshes(std::vector<uint8_t>(
                raw.constData(), raw.constData() + raw.size()));
        if (!meshes.empty()) n = (long long)meshes[0].vertices.size();
    }
    glb_vcount_cache_[path] = {mtime, n};
    return n;
}

void MeshSwapTab::update_enabled() {
    const bool has_bf = bf_ != nullptr;
    const bool busy = worker_thread_ && worker_thread_->isRunning();
    const bool has_geo = !geo_rows_.empty();
    list_btn_->setEnabled(has_bf && !busy);
    geo_combo_->setEnabled(has_geo && !busy);
    const QString glb_path = glb_edit_->text().trimmed();
    const long long nverts =
        glb_path.isEmpty() ? -1 : glb_render_vert_count(glb_path);
    const bool over_u16 = nverts >= 0 && nverts > U16_VERT_LIMIT;
    apply_btn_->setEnabled(has_bf && has_geo && !glb_path.isEmpty()
                           && !busy && !over_u16);
    if (over_u16) {
        apply_btn_->setToolTip(
            tr("Disabled — this GLB has %L1 render vertices, over the %L2-"
               "vertex limit for one Jade GEO (its cooked index buffer is "
               "16-bit).\n\nRender vertices run higher than Blender's "
               "vertex count because UV seams, hard/shading edges, and "
               "each extra material split vertices on export.\n\nWhat to "
               "do: merge materials, use smooth shading, reduce UV seams, "
               "and/or decimate until the GLB is under %L2 vertices.")
                .arg(nverts)
                .arg(qlonglong(U16_VERT_LIMIT)));
    } else {
        apply_btn_->setToolTip(apply_tip_default_);
    }
    disable_btn_->setEnabled(has_bf && has_geo && !busy);
    export_glb_btn_->setEnabled(has_bf && has_geo && !busy);
    export_glb_hier_btn_->setEnabled(has_bf && has_geo && !busy);
}

// ── list_geo_subs (port of io_ops/mesh_swap.py list_geo_subs) ──

MeshSwapTab::ListGeoResult MeshSwapTab::list_geo_subs(quint32 entry_idx) {
    ListGeoResult out;
    try {
        jade::BigFile bf;
        bf.open(bf_path_.toStdString());
        if (!bf.files.count(entry_idx)) {
            out.error = tr("BF entry %1 not found").arg(entry_idx);
            return out;
        }
        const jade::LzoResult r =
            jade::decompress_lzo(bf.read_data(entry_idx));
        if (!r.ok) {
            out.error = tr("Could not decompress BF entry");
            return out;
        }
        const std::vector<jade::SubEntry> subs =
            jade::walk_sub_entries(r.data);

        // _build_geo_name_index: geo_key -> human-readable name from the
        // .gao sub-entries that reference each geo via visual.gro_key.
        std::map<quint32, QString> name_by_geo;
        std::map<quint32, int> ref_count;
        std::unordered_map<uint32_t, const jade::SubEntry*> by_key;
        for (const jade::SubEntry& s : subs) by_key[s.key] = &s;
        std::unordered_set<uint32_t> geo_keys;
        bool geo_keys_built = false;
        for (const jade::SubEntry& s : subs) {
            if (s.ext != ".gao") continue;
            const jade::GaoInfo info =
                jade::parse_gao_full(s.data.data(), s.data.size());
            if (!info.ok || !info.vis_read) continue;
            const quint32 gro_key = info.gro_key;
            if (!gro_key || gro_key == 0xFFFFFFFFu) continue;
            QString gao_name = qs(info.name).trimmed();
            gao_name.remove(QChar(0));
            if (gao_name.endsWith(QStringLiteral(".gao")))
                gao_name.chop(4);
            // A GAO's gro_key may name a GEO directly or a gro_type-8
            // geometry group; name every member GEO.
            std::vector<uint32_t> targets;
            auto gk_it = by_key.find(gro_key);
            if (gk_it != by_key.end()
                && (gk_it->second->gro_null
                    || gk_it->second->gro_type != 1)) {
                if (!geo_keys_built) {
                    geo_keys_built = true;
                    for (const auto& [k, sp] : by_key)
                        if (!sp->gro_null && sp->gro_type == 1)
                            geo_keys.insert(k);
                }
                targets =
                    jade::geo_group_members(gro_key, by_key, &geo_keys);
            } else {
                targets = {gro_key};
            }
            for (uint32_t target : targets) {
                ref_count[target] += 1;
                if (!gao_name.isEmpty() && !name_by_geo.count(target))
                    name_by_geo[target] = gao_name;
            }
        }
        // Decorate names with the reference count when >1.
        for (auto& [k, name] : name_by_geo) {
            const int n = ref_count.count(k) ? ref_count[k] : 1;
            if (n > 1) name += QStringLiteral(" (+%1)").arg(n - 1);
        }

        // Largest parseable gro_type-1 candidate per key.
        std::map<quint32, const jade::SubEntry*> seen;
        for (const jade::SubEntry& s : subs) {
            if (s.gro_null || s.gro_type != 1) continue;
            const uint32_t gt = 1;
            if (!jade::is_geometry_entry(s.data.data(), s.data.size(), &gt))
                continue;
            auto it = seen.find(s.key);
            if (it == seen.end() || s.data.size() > it->second->data.size())
                seen[s.key] = &s;
        }

        for (const auto& [key, s] : seen) {
            const jade::GeoInfo g =
                jade::parse_geometry(s->data.data(), s->data.size());
            if (!g.ok) continue;
            // Vertex-colour sources: GEO body dul_PointColors, and the
            // host GAO RLI (what actually renders on static meshes).
            const jade::SubEntry* host = jade::host_gao_sub(subs, key);
            const bool rli_cols =
                host && jade::has_rli(host->data.data(), host->data.size(),
                                      g.nb_points);
            GeoRow rowv;
            rowv.key = key;
            rowv.name = name_by_geo.count(key) ? name_by_geo[key]
                                               : QString();
            rowv.nb_points = g.nb_points;
            rowv.nb_uvs = g.nb_uvs;
            rowv.nb_elements = g.nb_elements;
            rowv.nb_tris = g.nb_tris;
            rowv.has_skin = g.skin_present;
            rowv.bones = quint32(g.skin_bones.size());
            rowv.payload_bytes = s->data.size();
            rowv.geo_colors = !g.colors.empty();
            rowv.rli_colors = rli_cols;
            out.rows.push_back(rowv);
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = QString::fromUtf8(e.what());
    }
    return out;
}

void MeshSwapTab::on_list() {
    if (!bf_) return;
    const long long idx = current_entry_idx();
    if (idx < 0) {
        QMessageBox::warning(this, tr("Mesh Swap"),
                             tr("Pick a BF entry first."));
        return;
    }
    auto fit = bf_->files.find(uint32_t(idx));
    if (fit == bf_->files.end()) {
        QMessageBox::warning(
            this, tr("Mesh Swap"),
            tr("Entry index %1 not found in BigFile.").arg(idx));
        return;
    }
    log_->append(tr("\n=== Listing .geo sub-entries in entry %1 (%2) ===")
                     .arg(idx)
                     .arg(qs(fit->second.name)));
    geo_combo_->clear();
    geo_rows_.clear();

    ListGeoResult res = list_geo_subs(quint32(idx));
    if (!res.ok) {
        log_->append(tr("  ERROR: %1").arg(res.error));
        update_enabled();
        return;
    }
    std::stable_sort(res.rows.begin(), res.rows.end(),
                     [](const GeoRow& a, const GeoRow& b) {
                         return a.payload_bytes > b.payload_bytes;
                     });
    for (const GeoRow& info : res.rows) {
        geo_rows_.push_back({info.key, info});
        const QString name =
            info.name.isEmpty() ? tr("<unnamed>") : info.name;
        const QString label =
            QStringLiteral("%1  —  %2  (%3v/%4uv/%5tri, %6, %7 bones, %8, "
                           "%9 B)")
                .arg(name, qs(hex_key_lower(info.key)))
                .arg(info.nb_points)
                .arg(info.nb_uvs)
                .arg(info.nb_tris)
                .arg(info.has_skin ? tr("skinned") : tr("unskinned"))
                .arg(info.bones)
                .arg(vcol_tag(info.geo_colors, info.rli_colors))
                .arg(info.payload_bytes);
        geo_combo_->addItem(label, info.key);
    }
    log_->append(
        tr("  found %1 parseable .geo sub-entries").arg(res.rows.size()));
    update_enabled();
}

// ── resolver / materials table ──

// A MeshSwapResolver for entry_idx, reused across calls (the Python
// get_cached_resolver, bounded to the 8 most-recent entries).
MeshSwapResolver* MeshSwapTab::resolver_for(
    quint32 entry_idx, std::vector<jade::SubEntry> local_subs) {
    for (auto& [idx, r] : resolver_cache_)
        if (idx == entry_idx) return r.get();
    auto r = std::make_shared<MeshSwapResolver>(
        bf_.get(), asset_index_, entry_idx, std::move(local_subs));
    resolver_cache_.push_back({entry_idx, r});
    if (resolver_cache_.size() > 8)
        resolver_cache_.erase(resolver_cache_.begin());
    return resolver_cache_.back().second.get();
}

// resolve_mesh_materials (port of io_ops/mesh_swap.py): resolve a GEO's
// element -> material -> texture chain, across bins when an AssetIndex is
// available (the Shape/MAT split).
MeshSwapTab::MeshMatInfo MeshSwapTab::resolve_mesh_materials(
    quint32 entry_idx, quint32 geo_key) {
    MeshMatInfo out;
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bf_->read_data(entry_idx));
        if (!r.ok) {
            out.error = tr("Could not decompress BF entry");
            return out;
        }
        const std::vector<jade::SubEntry> subs =
            jade::walk_sub_entries(r.data);
        std::unordered_map<uint32_t, const jade::SubEntry*> by_key;
        for (const jade::SubEntry& s : subs) by_key[s.key] = &s;
        const jade::SubEntry* geo_sub = jade::pick_geo_sub(subs, geo_key);
        if (!geo_sub) {
            out.error = tr("No .geo sub-entry %1 in entry %2")
                            .arg(qs(hex_key_lower(geo_key)))
                            .arg(entry_idx);
            return out;
        }
        const jade::GeoInfo geo =
            jade::parse_geometry(geo_sub->data.data(), geo_sub->data.size());
        if (!geo.ok) {
            out.error = tr("Could not parse geometry");
            return out;
        }

        MeshSwapResolver* resolver = resolver_for(entry_idx, subs);

        // The owning GAO names the container its matIds index — but if
        // that's a Shape-bin STUB, the engine substitutes a MAT-bin
        // material at load (the wow merge; proven via x64dbg).
        uint32_t grm_key = 0;
        const bool have_grm = jade::owning_grm_key(subs, geo_key, grm_key);
        qint64 max_mat = 0;
        for (size_t ei = 0; ei * 2 + 1 < geo.elements.size(); ++ei)
            max_mat = std::max<qint64>(max_mat, geo.elements[ei * 2 + 1]);

        std::vector<quint32> sub_mat_keys;
        if (have_grm)
            for (uint32_t k : jade::grm_sub_material_keys(
                     by_key.count(grm_key) ? by_key[grm_key] : nullptr))
                sub_mat_keys.push_back(k);

        long long grm_entry_idx =
            (have_grm && by_key.count(grm_key)) ? (long long)entry_idx : -1;
        bool grm_is_crossbin = false;
        {
            quint32 ckey = 0;
            long long cpidx = -1;
            std::vector<quint32> ckeys;
            if (resolver->find_material_override(have_grm ? grm_key : 0,
                                                 max_mat, geo_key, ckey,
                                                 cpidx, ckeys)
                && !ckeys.empty()) {
                grm_key = ckey;
                grm_entry_idx = cpidx;
                sub_mat_keys = ckeys;
                grm_is_crossbin = true;
            }
        }

        // Texture parse + the bin it lives in, resolved cross-bin.
        auto texture_info =
            [resolver](quint32 texk) -> std::pair<jade::TexInfo, long long> {
            if (!texk) return {jade::TexInfo{}, -1};
            auto [s, pidx] = resolver->lookup(texk);
            if (!s
                || !jade::is_texture_entry(s->data.data(), s->data.size()))
                return {jade::TexInfo{}, -1};
            return {jade::parse_texture(s->data.data(), s->data.size()),
                    pidx};
        };

        // Per-sub-material usage across every other GEO in the same wow
        // entry — lets the UI warn that a retarget also affects siblings.
        std::map<quint32, std::set<quint32>> shared_uses_by_matkey;
        for (const jade::SubEntry& other : subs) {
            if (other.ext != ".gao") continue;
            const jade::GaoInfo oinfo =
                jade::parse_gao_full(other.data.data(), other.data.size());
            if (!oinfo.ok || !oinfo.vis_read) continue;
            const quint32 other_gro = oinfo.gro_key;
            const quint32 other_grm = oinfo.grm_key;
            if (!other_gro || other_gro == geo_key) continue;
            std::vector<uint32_t> other_sub_keys;
            if (by_key.count(other_grm)) {
                const jade::MatInfo omm = jade::parse_material(
                    by_key[other_grm]->data.data(),
                    by_key[other_grm]->data.size(),
                    jade::GRO_TYPE_MAT_MULTI);
                if (omm.ok) other_sub_keys = omm.sub_material_keys;
            }
            const jade::SubEntry* other_geo_sub =
                jade::pick_geo_sub(subs, other_gro);
            if (!other_geo_sub) continue;
            const jade::GeoInfo other_geo = jade::parse_geometry(
                other_geo_sub->data.data(), other_geo_sub->data.size());
            if (!other_geo.ok) continue;
            for (size_t ei = 0; ei * 2 + 1 < other_geo.elements.size();
                 ++ei) {
                const qint64 mid = other_geo.elements[ei * 2 + 1];
                if (mid >= 0 && size_t(mid) < other_sub_keys.size())
                    shared_uses_by_matkey[other_sub_keys[size_t(mid)]]
                        .insert(other_gro);
            }
        }

        const qint64 n_sub = qint64(sub_mat_keys.size());
        for (size_t ei = 0; ei * 2 + 1 < geo.elements.size(); ++ei) {
            const quint32 ntri = geo.elements[ei * 2];
            const qint64 mid = geo.elements[ei * 2 + 1];
            // Engine clamps an out-of-range matId to the last slot.
            const qint64 slot = jade::clamp_matid(mid, n_sub);
            long long matk = -1;
            if (slot >= 0 && slot < n_sub) {
                const quint32 k = sub_mat_keys[size_t(slot)];
                if (k != 0 && k != 0xFFFFFFFFu) matk = k;
            }
            MatRow rowv;
            rowv.element = int(ei);
            rowv.matId = int(mid);
            rowv.nTri = ntri;
            rowv.material_key = matk;
            if (matk >= 0) {
                auto [msub, mat_pidx] = resolver->lookup(quint32(matk));
                rowv.material_entry_idx = mat_pidx;
                if (msub) {
                    const quint32 texk = jade::resolve_texture_key(
                        msub->data.data(), msub->data.size());
                    if (texk) rowv.texture_key = texk;
                    rowv.render_flags = jade::parse_material_render_flags(
                        msub->data.data(), msub->data.size());
                    // Every texture LAYER the material references.
                    for (const jade::TexLayer& tl :
                         jade::resolve_texture_keys(msub->data.data(),
                                                    msub->data.size())) {
                        auto [ltp, ltex_pidx] = texture_info(tl.key);
                        TexLayer lyr;
                        lyr.layer = int(tl.layer_index);
                        lyr.texture_key = tl.key;
                        lyr.texture_entry_idx = ltex_pidx;
                        if (ltp.valid) {
                            lyr.texture_width = ltp.width;
                            lyr.texture_height = ltp.height;
                            lyr.texture_format = ltp.format;
                        }
                        rowv.texture_layers.push_back(lyr);
                    }
                }
                auto sh = shared_uses_by_matkey.find(quint32(matk));
                if (sh != shared_uses_by_matkey.end())
                    rowv.shared_with_geos.assign(sh->second.begin(),
                                                 sh->second.end());
            }
            if (rowv.texture_key >= 0) {
                auto [tp, tex_pidx] =
                    texture_info(quint32(rowv.texture_key));
                rowv.texture_entry_idx = tex_pidx;
                if (tp.valid) {
                    rowv.texture_width = tp.width;
                    rowv.texture_height = tp.height;
                    rowv.texture_format = tp.format;
                }
            }
            out.elements.push_back(std::move(rowv));
        }
        out.grm_key = have_grm ? (long long)grm_key : -1;
        out.grm_entry_idx = grm_entry_idx;
        out.grm_is_crossbin = grm_is_crossbin;
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = QString::fromUtf8(e.what());
    }
    return out;
}

// Repopulate the element → material → texture table for the geo currently
// selected in the Step 2 combo.
void MeshSwapTab::refresh_materials() {
    mat_table_->setRowCount(0);
    tex_preview_cache_.clear();
    geo_parse_cache_.clear();
    elem_thumb_cache_.clear();
    current_grm_key_ = -1;
    current_grm_entry_idx_ = -1;
    current_grm_is_crossbin_ = false;
    current_mat_rows_.clear();
    if (!bf_ || geo_rows_.empty()) return;
    const QVariant keyv = geo_combo_->currentData();
    const long long idx = current_entry_idx();
    if (!keyv.isValid() || idx < 0) return;
    const quint32 key = keyv.toUInt();
    MeshMatInfo info = resolve_mesh_materials(quint32(idx), key);
    if (!info.ok) {
        log_->append(tr("  material lookup skipped: %1").arg(info.error));
        return;
    }
    // The multi-material container the elements' matIds index into.
    current_grm_key_ = info.grm_key;
    current_grm_entry_idx_ = info.grm_entry_idx;
    current_grm_is_crossbin_ = info.grm_is_crossbin;
    current_mat_rows_ = info.elements;
    if (info.grm_is_crossbin && asset_index_) {
        QString gname = QStringLiteral("?");
        if (info.grm_entry_idx >= 0
            && bf_->files.count(uint32_t(info.grm_entry_idx)))
            gname = qs(bf_->files.at(uint32_t(info.grm_entry_idx)).name);
        log_->append(
            tr("  ⚠ materials resolved CROSS-BIN (heuristic) from %1 "
               "in '%2' — this may not be the mesh's real material; edits "
               "here are risky (the table is a best-guess preview, not the "
               "engine's runtime choice)")
                .arg(qs(hex_key_lower(
                         quint32(info.grm_key < 0 ? 0 : info.grm_key))),
                     gname));
    }
    mat_table_->setRowCount(int(info.elements.size()));
    for (int r = 0; r < int(info.elements.size()); ++r) {
        const MatRow& el = info.elements[size_t(r)];
        const long long mk = el.material_key;
        const long long tk = el.texture_key;
        std::vector<TexLayer> extra;
        for (const TexLayer& ly : el.texture_layers)
            if (ly.layer != 0) extra.push_back(ly);
        QString tex_text;
        if (tk >= 0) {
            QString dims;
            if (el.texture_width > 0)
                dims = QStringLiteral("   %1x%2 fmt%3")
                           .arg(el.texture_width)
                           .arg(el.texture_height)
                           .arg(el.texture_format);
            tex_text = qs(hex_key_lower(quint32(tk))) + dims;
        } else if (!extra.empty()) {
            tex_text.clear();
        } else {
            tex_text = QStringLiteral("—");
        }
        // Surface detail/effect layers the material modulates on top of
        // the base — the usual reason a retexture "looks wrong".
        if (!extra.empty()) {
            const QString sep =
                (tex_text.isEmpty() || tex_text == QStringLiteral("—"))
                    ? QString()
                    : QStringLiteral("  +  ");
            QStringList parts;
            for (const TexLayer& ly : extra)
                parts << QStringLiteral("L%1·%2")
                             .arg(ly.layer)
                             .arg(qs(hex_key_lower(ly.texture_key)));
            tex_text += sep + parts.join(QStringLiteral("  "));
        }
        // Tag shared materials so the user knows a retarget will also
        // change every other mesh that references the same sub-material.
        QString mat_text;
        if (mk >= 0) {
            mat_text = qs(hex_key_lower(quint32(mk)));
            if (!el.shared_with_geos.empty()) {
                mat_text += tr("  (shared with %1 other mesh%2)")
                                .arg(el.shared_with_geos.size())
                                .arg(el.shared_with_geos.size() != 1
                                         ? QStringLiteral("es")
                                         : QString());
            }
        } else {
            mat_text = QStringLiteral("—");
        }
        // Render-state label (opaque / alpha-test / blend) + detail tip.
        const QString render_text =
            el.render_flags.ok
                ? qs(el.render_flags.mode)
                : (mk >= 0 ? QStringLiteral("—") : QString());
        const QString cells[6] = {QString::number(el.element),
                                  QString::number(el.matId),
                                  QString::number(el.nTri),
                                  mat_text,
                                  tex_text,
                                  render_text};
        for (int c = 0; c < 6; ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            if (c == 0) item->setData(Qt::UserRole, el.element);
            if (c == 1) item->setData(Qt::UserRole, el.matId);
            if (c == 3) {
                if (mk >= 0) item->setData(Qt::UserRole, quint32(mk));
                if (mk >= 0 && !el.shared_with_geos.empty()) {
                    QStringList geos;
                    for (quint32 g : el.shared_with_geos)
                        geos << qs(hex_key_lower(g));
                    item->setToolTip(
                        tr("Sub-material %1 is also used by these GEOs in "
                           "the same wow entry:\n  %2\n\nRetargeting it "
                           "will change the texture for every mesh in "
                           "this list as well.")
                            .arg(qs(hex_key_lower(quint32(mk))),
                                 geos.join(QStringLiteral("\n  "))));
                }
            }
            if (c == 4 && tk >= 0)
                item->setData(Qt::UserRole, quint32(tk));
            if (c == 5 && el.render_flags.ok) {
                QStringList flag_list;
                for (const std::string& f : el.render_flags.flags)
                    flag_list << qs(f);
                item->setToolTip(
                    tr("ul_Flags %1\nblend: %2\ncolorOp: %3\nalpha test: "
                       "%4 (threshold %5)\nflags: %6")
                        .arg(qs(hex_key_lower(el.render_flags.ul_flags)),
                             qs(el.render_flags.blend_name),
                             qs(el.render_flags.colorop_name),
                             el.render_flags.alpha_test ? tr("yes")
                                                        : tr("no"))
                        .arg(el.render_flags.alpha_thresh)
                        .arg(flag_list.isEmpty()
                                 ? tr("(none)")
                                 : flag_list.join(QStringLiteral(", "))));
            }
            mat_table_->setItem(r, c, item);
        }
    }
}

// Decode + cache a QPixmap for a texture key. entry_idx is the bin the
// texture lives in (may be a sibling of the geo's entry under the
// Shape/MAT split); defaults to the geo's entry.
QPixmap MeshSwapTab::texture_pixmap(quint32 texture_key,
                                    long long entry_idx) {
    if (entry_idx < 0) entry_idx = current_entry_idx();
    const auto ckey = std::make_pair(entry_idx, texture_key);
    auto hit = tex_preview_cache_.find(ckey);
    if (hit != tex_preview_cache_.end()) return hit->second;
    QPixmap pix;
    if (entry_idx >= 0) {
        // decode_texture_preview: the real texture occurrence (Jade
        // declares a header stub first), palette-resolved for PAL8.
        try {
            jade::BigFile bf;
            bf.open(bf_path_.toStdString());
            const jade::LzoResult r =
                jade::decompress_lzo(bf.read_data(uint32_t(entry_idx)));
            if (r.ok) {
                const std::vector<jade::SubEntry> subs =
                    jade::walk_sub_entries(r.data);
                const jade::SubEntry* best = nullptr;
                size_t best_pixlen = 0;
                jade::TexInfo best_ti;
                for (const jade::SubEntry& s : subs) {
                    if (s.key != texture_key) continue;
                    if (!jade::is_texture_entry(s.data.data(),
                                                s.data.size()))
                        continue;
                    const jade::TexInfo tp = jade::parse_texture(
                        s.data.data(), s.data.size());
                    if (!tp.valid) continue;
                    const size_t pixlen =
                        s.data.size() > tp.pix_start
                            ? s.data.size() - tp.pix_start
                            : 0;
                    if (!best || pixlen > best_pixlen) {
                        best = &s;
                        best_pixlen = pixlen;
                        best_ti = tp;
                    }
                }
                if (best) {
                    const std::vector<uint8_t>* pal =
                        jade::palette_for_texture(best_ti, subs);
                    const std::vector<uint8_t> rgba =
                        jade::decode_texture(best->data.data(),
                                             best->data.size(), best_ti,
                                             pal ? pal->data() : nullptr,
                                             pal ? pal->size() : 0);
                    if (!rgba.empty()) {
                        QImage img(rgba.data(), int(best_ti.width),
                                   int(best_ti.height),
                                   int(best_ti.width) * 4,
                                   QImage::Format_RGBA8888);
                        pix = QPixmap::fromImage(img.copy());
                    }
                }
            }
        } catch (const std::exception&) {
        }
    }
    tex_preview_cache_[ckey] = pix;
    return pix;
}

QString MeshSwapTab::pixmap_tooltip_html(const QString& title, QPixmap pix,
                                         int max_side) {
    if (pix.width() > max_side || pix.height() > max_side)
        pix = pix.scaled(max_side, max_side, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QBuffer::WriteOnly);
    pix.save(&buf, "PNG");
    return QStringLiteral(
               "<div>%1<br><img src=\"data:image/png;base64,%2\"></div>")
        .arg(title, QString::fromLatin1(ba.toBase64()));
}

// Show a tooltip bound to the cell's rect — it stays up for as long as the
// cursor remains on that cell.
void MeshSwapTab::show_cell_tooltip(int row, int col, const QString& html) {
    QTableWidgetItem* item = mat_table_->item(row, col);
    const QRect rect = item ? mat_table_->visualItemRect(item)
                            : mat_table_->viewport()->rect();
    tooltips::show_persistent_tooltip(QCursor::pos(), html,
                                      mat_table_->viewport(), rect);
}

void MeshSwapTab::on_mat_cell_hover(int row, int col) {
    // Element / matId / Tris columns: render which part of the mesh this
    // element (and so its material + texture) covers.
    if (col == 0 || col == 1 || col == 2) {
        show_element_hover(row, col);
        return;
    }
    if (col != 4) {
        tooltips::hide_persistent_tooltip();
        return;
    }
    // The texture may live in a sibling bin (Shape/MAT split). Preview
    // EVERY layer the material references (base + detail/effect).
    const MatRow* mrow =
        (row >= 0 && size_t(row) < current_mat_rows_.size())
            ? &current_mat_rows_[size_t(row)]
            : nullptr;
    std::vector<TexLayer> layers;
    if (mrow) layers = mrow->texture_layers;
    if (layers.empty()) {
        QTableWidgetItem* item = mat_table_->item(row, col);
        const QVariant tkv = item ? item->data(Qt::UserRole) : QVariant();
        if (!tkv.isValid()) {
            tooltips::hide_persistent_tooltip();
            return;
        }
        TexLayer base;
        base.layer = 0;
        base.texture_key = tkv.toUInt();
        base.texture_entry_idx = mrow ? mrow->texture_entry_idx : -1;
        layers.push_back(base);
    }
    QStringList blocks;
    for (const TexLayer& ly : layers) {
        const QString label =
            qs(hex_key_lower(ly.texture_key))
            + (ly.layer ? QStringLiteral("  · layer %1").arg(ly.layer)
                        : QStringLiteral(" · base"));
        const QPixmap pix =
            texture_pixmap(ly.texture_key, ly.texture_entry_idx);
        if (pix.isNull())
            blocks << QStringLiteral("<div>%1 (preview unavailable)</div>")
                          .arg(label);
        else
            blocks << pixmap_tooltip_html(label, pix);
    }
    show_cell_tooltip(row, col, blocks.join(QStringLiteral("<br>")));
}

// Parsed geometry of the GEO listed in the table (cached).
const jade::GeoInfo* MeshSwapTab::parsed_geo() {
    const long long idx = current_entry_idx();
    const QVariant keyv = geo_combo_->currentData();
    if (idx < 0 || !keyv.isValid()) return nullptr;
    const auto ck = std::make_pair(quint32(idx), keyv.toUInt());
    auto hit = geo_parse_cache_.find(ck);
    if (hit != geo_parse_cache_.end()) return hit->second.get();
    std::shared_ptr<jade::GeoInfo> geo;
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bf_->read_data(uint32_t(idx)));
        if (r.ok) {
            const std::vector<jade::SubEntry> subs =
                jade::walk_sub_entries(r.data);
            const jade::SubEntry* sub =
                jade::pick_geo_sub(subs, keyv.toUInt());
            if (sub) {
                auto g = std::make_shared<jade::GeoInfo>(
                    jade::parse_geometry(sub->data.data(),
                                         sub->data.size()));
                if (g->ok) geo = g;
            }
        }
    } catch (const std::exception&) {
    }
    geo_parse_cache_[ck] = geo;
    return geo_parse_cache_[ck].get();
}

// Element-highlight render for the listed GEO (cached).
QPixmap MeshSwapTab::element_thumb(int elem_idx) {
    const long long idx = current_entry_idx();
    const QVariant keyv = geo_combo_->currentData();
    const auto ck = std::make_tuple(quint32(idx < 0 ? 0 : idx),
                                    keyv.toUInt(), elem_idx);
    auto hit = elem_thumb_cache_.find(ck);
    if (hit != elem_thumb_cache_.end()) return hit->second;
    QPixmap pix;
    const jade::GeoInfo* geo = parsed_geo();
    if (geo) {
        // faces are 7-wide (i0,i1,i2,uv0,uv1,uv2,elem) — split into the
        // triangle list + per-face element index the renderer wants.
        std::vector<uint32_t> faces;
        std::vector<int32_t> face_elems;
        faces.reserve(size_t(geo->nb_tris) * 3);
        face_elems.reserve(geo->nb_tris);
        for (size_t f = 0; f + 6 < geo->faces.size(); f += 7) {
            faces.push_back(geo->faces[f]);
            faces.push_back(geo->faces[f + 1]);
            faces.push_back(geo->faces[f + 2]);
            face_elems.push_back(int32_t(geo->faces[f + 6]));
        }
        pix = element_thumb::render_element_highlight(
            geo->vertices, faces, face_elems, elem_idx);
    }
    elem_thumb_cache_[ck] = pix;
    return pix;
}

void MeshSwapTab::show_element_hover(int row, int col) {
    QTableWidgetItem* el_item = mat_table_->item(row, 0);
    const QVariant eiv =
        el_item ? el_item->data(Qt::UserRole) : QVariant();
    if (!eiv.isValid()) {
        tooltips::hide_persistent_tooltip();
        return;
    }
    const QPixmap pix = element_thumb(eiv.toInt());
    if (pix.isNull()) {
        tooltips::hide_persistent_tooltip();
        return;
    }
    QTableWidgetItem* tris = mat_table_->item(row, 2);
    QTableWidgetItem* mid = mat_table_->item(row, 1);
    const QString title =
        tr("element %1 — %2 tris, matId %3 <font color='#ff8c28'>"
           "(highlighted)</font>")
            .arg(eiv.toInt())
            .arg(tris ? tris->text() : QStringLiteral("?"),
                 mid ? mid->text() : QStringLiteral("?"));
    show_cell_tooltip(row, col, pixmap_tooltip_html(title, pix, 230));
}

void MeshSwapTab::on_mat_context_menu(const QPoint& pos) {
    QTableWidgetItem* item = mat_table_->itemAt(pos);
    if (!item) return;
    // Both keys are stashed on their cells via Qt::UserRole — the visible
    // text may carry decorations like "(shared with N meshes)".
    const int row = item->row();
    QTableWidgetItem* tex_item = mat_table_->item(row, 4);
    QTableWidgetItem* mat_item = mat_table_->item(row, 3);
    const QVariant tkv =
        tex_item ? tex_item->data(Qt::UserRole) : QVariant();
    const QVariant mkv =
        mat_item ? mat_item->data(Qt::UserRole) : QVariant();
    const long long tk = tkv.isValid() ? tkv.toUInt() : -1;
    const long long mk = mkv.isValid() ? mkv.toUInt() : -1;

    const long long idx = current_entry_idx();
    if (idx < 0) return;
    // The material and texture for this row may live in a sibling bin
    // (Shape/MAT split). Edits must target that bin, not the geo's entry.
    const MatRow* mrow =
        (row >= 0 && size_t(row) < current_mat_rows_.size())
            ? &current_mat_rows_[size_t(row)]
            : nullptr;
    long long tex_idx = mrow && mrow->texture_entry_idx >= 0
                            ? mrow->texture_entry_idx
                            : idx;
    long long mat_idx = mrow && mrow->material_entry_idx >= 0
                            ? mrow->material_entry_idx
                            : idx;

    // Every texture layer this material references. Multi-layer materials
    // (kind-9 detail/effect chains) get a per-layer submenu; a plain
    // single-layer material keeps the old flat actions.
    std::vector<TexLayer> layers;
    if (mrow) layers = mrow->texture_layers;
    if (layers.empty() && tk >= 0) {
        TexLayer base;
        base.layer = 0;
        base.texture_key = quint32(tk);
        base.texture_entry_idx = tex_idx;
        layers.push_back(base);
    }
    const bool multi = layers.size() > 1;

    QMenu menu(this);
    struct LayerAct {
        int kind;  // 0=replace 1=retarget 2=newtex
        int layer;
        quint32 tex_key;
        long long entry;
    };
    std::map<QAction*, LayerAct> layer_acts;
    for (const TexLayer& ly : layers) {
        const int li = ly.layer;
        const quint32 ltk = ly.texture_key;
        const long long ltex_idx =
            ly.texture_entry_idx >= 0 ? ly.texture_entry_idx : idx;
        QMenu* sub = &menu;
        QString rep_lbl, ret_lbl, new_lbl;
        if (multi) {
            const QString lname = li == 0
                                      ? QStringLiteral("base")
                                      : QStringLiteral("layer %1").arg(li);
            sub = menu.addMenu(tr("Texture %1: %2")
                                   .arg(lname, qs(hex_key_lower(ltk))));
            rep_lbl = tr("Replace its pixels in Texture Swap…");
            ret_lbl = tr("Retarget this layer to another texture…");
            new_lbl = tr("Give this layer a NEW texture from an image…");
        } else {
            rep_lbl = tr("Replace texture %1 pixels in Texture Swap…")
                          .arg(qs(hex_key_lower(ltk)));
            if (mk >= 0) {
                ret_lbl =
                    tr("Retarget material %1 to another texture…")
                        .arg(qs(hex_key_lower(quint32(mk))));
                new_lbl = tr("Give material %1 a NEW texture from an "
                             "image (this material only)…")
                              .arg(qs(hex_key_lower(quint32(mk))));
            }
        }
        if (ltex_idx != idx && bf_->files.count(uint32_t(ltex_idx))
            && !multi)
            rep_lbl += QStringLiteral("  [in %1]").arg(
                qs(bf_->files.at(uint32_t(ltex_idx)).name));
        QAction* a_rep = sub->addAction(rep_lbl);
        layer_acts[a_rep] = {0, li, ltk, ltex_idx};
        if (mk >= 0 && !ret_lbl.isEmpty()) {
            QAction* a_ret = sub->addAction(ret_lbl);
            layer_acts[a_ret] = {1, li, ltk, mat_idx};
        }
        // Import a NEW image as this layer's texture — the isolated way
        // to retexture (replace-texture edits shared PIXELS and leaks).
        if (mk >= 0 && !new_lbl.isEmpty()) {
            QAction* a_new = sub->addAction(new_lbl);
            layer_acts[a_new] = {2, li, ltk, mat_idx};
        }
    }
    // Render-flags editor — only offered for an editable (kind-9)
    // material, i.e. when we could decode its render flags.
    QAction* act_flags = nullptr;
    const bool have_rf = mrow && mrow->render_flags.ok;
    if (mk >= 0 && have_rf)
        act_flags = menu.addAction(
            tr("Change render flags (blend / alpha-test)…  [%1]")
                .arg(qs(mrow->render_flags.mode)));
    QAction* act_slot = nullptr;
    QTableWidgetItem* el_item = mat_table_->item(row, 0);
    const QVariant eiv =
        el_item ? el_item->data(Qt::UserRole) : QVariant();
    if (eiv.isValid() && current_grm_key_ >= 0)
        act_slot = menu.addAction(
            tr("Use different material for element %1 (this mesh only)…")
                .arg(eiv.toInt()));
    // Detach is a whole-mesh action: clone the shared container into a
    // private one so later edits stop leaking to sibling meshes.
    QAction* act_detach = nullptr;
    if (current_grm_key_ >= 0) {
        menu.addSeparator();
        bool shared = current_grm_is_crossbin_;
        for (const MatRow& r2 : current_mat_rows_)
            if (!r2.shared_with_geos.empty()) shared = true;
        const QString tag =
            shared ? tr(" (shared — isolates this mesh)") : QString();
        act_detach = menu.addAction(
            tr("Detach this mesh to its own private material…%1").arg(tag));
    }
    if (menu.actions().isEmpty()) return;
    QAction* chosen = menu.exec(mat_table_->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (act_detach && chosen == act_detach) {
        detach_geo_material();
        return;
    }
    if (act_slot && chosen == act_slot) {
        set_element_material(eiv.toInt(), mk);
        return;
    }
    if (act_flags && chosen == act_flags) {
        edit_material_flags(quint32(mk), mat_idx, mrow->render_flags);
        return;
    }
    // Per-layer texture action (replace pixels / retarget / new texture).
    auto la = layer_acts.find(chosen);
    if (la != layer_acts.end()) {
        const LayerAct& a = la->second;
        if (a.kind == 0) {
            if (!guard_crossbin_edit(tr("replace its pixels"))) return;
            emit send_texture_to_tab(quint32(a.entry), a.tex_key);
        } else if (a.kind == 1) {
            retarget_material(quint32(mk), a.tex_key, a.entry, a.layer);
        } else if (a.kind == 2) {
            new_texture_for_material(quint32(mk), a.entry, row, a.layer);
        }
    }
}

// ── Edit guardrails (cross-bin / clamp hazards) ────────────────────

// The current container's positional sub_material_keys as a set, or
// nullopt if it can't be read.
std::optional<std::set<quint32>> MeshSwapTab::container_slot_keys() {
    if (!bf_ || current_grm_key_ < 0 || current_grm_entry_idx_ < 0)
        return std::nullopt;
    try {
        const jade::LzoResult r = jade::decompress_lzo(
            bf_->read_data(uint32_t(current_grm_entry_idx_)));
        if (!r.ok) return std::nullopt;
        for (const jade::SubEntry& s : jade::walk_sub_entries(r.data)) {
            if (s.key == quint32(current_grm_key_)) {
                const jade::MatInfo mm = jade::parse_material(
                    s.data.data(), s.data.size(),
                    jade::GRO_TYPE_MAT_MULTI);
                if (!mm.ok) return std::nullopt;
                return std::set<quint32>(mm.sub_material_keys.begin(),
                                         mm.sub_material_keys.end());
            }
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

// Confirm before an edit when this mesh's material container was resolved
// from a SIBLING bin by the cross-bin heuristic (Shape/MAT split).
bool MeshSwapTab::guard_crossbin_edit(const QString& action_desc) {
    if (!current_grm_is_crossbin_) return true;
    const quint32 grm =
        current_grm_key_ < 0 ? 0 : quint32(current_grm_key_);
    QString binname = QStringLiteral("?");
    if (current_grm_entry_idx_ >= 0
        && bf_->files.count(uint32_t(current_grm_entry_idx_)))
        binname = qs(bf_->files.at(uint32_t(current_grm_entry_idx_)).name);
    const auto r = QMessageBox::warning(
        this, tr("Cross-bin material — proceed with caution"),
        tr("This mesh's material container (%1, in '%2') was resolved "
           "from a DIFFERENT bin by a heuristic, because the mesh's own "
           "in-bin material couldn't index every matId it uses.\n\nThat "
           "container may not be what the engine actually renders this "
           "mesh with — it can be a shared or unrelated material in the "
           "same key family. Editing it to %3 may change other meshes or "
           "have no effect on this one.\n\nProceed anyway?")
            .arg(qs(hex_key_lower(grm)), binname, action_desc),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return r == QMessageBox::Yes;
}

// Confirm before set_element_material APPENDS a new slot — the engine
// clamps any out-of-range matId to the container's LAST slot, so growing
// the container moves that clamp target.
bool MeshSwapTab::guard_append_clamp(quint32 new_material_key) {
    const auto slot_keys = container_slot_keys();
    if (!slot_keys || slot_keys->count(new_material_key)) return true;
    const quint32 grm =
        current_grm_key_ < 0 ? 0 : quint32(current_grm_key_);
    const QString extra =
        current_grm_is_crossbin_
            ? tr(" The container is in another bin, so meshes that share "
                 "it there CANNOT be auto-protected.")
            : QString();
    const auto r = QMessageBox::warning(
        this, tr("Appending a material slot — clamp hazard"),
        tr("This material isn't in container %1 yet, so applying it "
           "APPENDS a new last slot (grows the container).\n\nThe engine "
           "clamps any out-of-range matId to the container's LAST slot, "
           "so growing it changes what every mesh sharing this container "
           "draws when it clamps.%2\n\nPrefer reusing an existing slot. "
           "Append anyway?")
            .arg(qs(hex_key_lower(grm)), extra),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return r == QMessageBox::Yes;
}

// Open a texture picker and append a retarget_material_texture op to the
// open project. mat_entry_idx is the bin the sub-material actually lives
// in; the op edits that bin, and the picker lists textures from it.
// `layer` selects which texture layer of the material to repoint.
void MeshSwapTab::retarget_material(quint32 sub_material_key,
                                    long long current_texture_key,
                                    long long mat_entry_idx, int layer) {
    if (!project_ || project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Retarget material"),
            tr("No project open. Use File → New Project or Open Project "
               "first; the tab records edits into a project rather than "
               "writing to the .bf directly."));
        return;
    }
    const long long idx = current_entry_idx();
    if (idx < 0 || !bf_) return;
    if (!guard_crossbin_edit(tr("retarget it to another texture"))) return;
    // The sub-material's own bin owns the bytes we'll rewrite.
    if (mat_entry_idx < 0) mat_entry_idx = idx;
    if (!bf_->files.count(uint32_t(mat_entry_idx))) return;
    const quint32 entry_key = bf_->files.at(uint32_t(mat_entry_idx)).key;

    // Re-resolve the materials so we have the up-to-date "shared with"
    // list for the confirmation prompt below.
    std::vector<quint32> shared_with;
    {
        const QVariant gv = geo_combo_->currentData();
        if (gv.isValid()) {
            MeshMatInfo info =
                resolve_mesh_materials(quint32(idx), gv.toUInt());
            if (info.ok)
                for (const MatRow& el : info.elements)
                    if (el.material_key == (long long)sub_material_key) {
                        shared_with = el.shared_with_geos;
                        break;
                    }
        }
    }

    TexturePickerDialog dlg(bf_path_, quint32(mat_entry_idx),
                            current_texture_key, project_,
                            (long long)entry_key, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const long long new_key = dlg.selected_key();
    if (new_key < 0) return;
    if (current_texture_key >= 0 && new_key == current_texture_key) {
        QMessageBox::information(
            this, tr("Retarget material"),
            tr("That's the texture this material is already pointing at "
               "— nothing to record."));
        return;
    }

    // Confirm if the sub-material is also used by other meshes in this
    // wow entry — the retarget will change them too.
    if (!shared_with.empty()) {
        QStringList geos;
        for (quint32 g : shared_with) geos << qs(hex_key_lower(g));
        const auto choice = QMessageBox::question(
            this, tr("Material is shared"),
            tr("Sub-material <b>%1</b> is also used by these GEOs in the "
               "same wow entry:<br><br><pre>  %2</pre><br>Retargeting it "
               "will change the texture for those meshes as well. "
               "Continue?")
                .arg(qs(hex_key_lower(sub_material_key)),
                     geos.join(QStringLiteral("\n  "))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
    }

    // RetargetMaterialTexture._params_to_dict shape.
    Value op = make_obj();
    op.obj["op"] = make_str("retarget_material_texture");
    Value tgt = make_obj();
    tgt.obj["entry_key"] = make_str(hex_key_lower(entry_key));
    tgt.obj["sub_key"] = make_str(hex_key_lower(sub_material_key));
    op.obj["target"] = std::move(tgt);
    Value prm = make_obj();
    prm.obj["new_texture_key"] = make_str(hex_key_lower(quint32(new_key)));
    if (layer) prm.obj["layer"] = make_num(layer);
    op.obj["params"] = std::move(prm);
    project_->add_operation(std::move(op));
    project_->save();
    const QString layer_note =
        layer ? tr(" layer %1").arg(layer) : QString();
    log_->append(tr("Recorded: material %1%2 in entry %3 -> texture %4")
                     .arg(qs(hex_key_lower(sub_material_key)), layer_note,
                          qs(hex_key_lower(entry_key)),
                          qs(hex_key_lower(quint32(new_key)))));
    // The table is rebuilt from the BF on disk, so it'll still show the
    // old key until build time.
    log_->append(
        tr("  (table still shows BF state; the change applies at Build)"));
}

// Import an image as a NEW texture and point THIS material at it — the
// isolated way to retexture a detached/private material. Mints a private
// texture (add_texture) in the material's bin and retargets only this
// material's pointer at it, leaving the shared texture untouched.
void MeshSwapTab::new_texture_for_material(quint32 material_key,
                                           long long mat_entry_idx,
                                           int row, int layer) {
    if (!project_ || project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("New texture for material"),
            tr("No project open. Use File → New Project or Open Project "
               "first."));
        return;
    }
    if (!bf_ || mat_entry_idx < 0
        || !bf_->files.count(uint32_t(mat_entry_idx)))
        return;
    const quint32 mat_entry_key = bf_->files.at(uint32_t(mat_entry_idx)).key;

    // Warn if this material is still shared (retarget would leak); a
    // detached private material (0x7A range) is the intended target.
    const MatRow* mrow =
        (row >= 0 && size_t(row) < current_mat_rows_.size())
            ? &current_mat_rows_[size_t(row)]
            : nullptr;
    const std::vector<quint32> shared =
        mrow ? mrow->shared_with_geos : std::vector<quint32>();
    if (!shared.empty() && (material_key >> 24) != 0x7A) {
        QStringList geos;
        for (quint32 g : shared) geos << qs(hex_key_lower(g));
        if (QMessageBox::question(
                this, tr("Material is shared"),
                tr("Material <b>%1</b> is also used by other meshes:<br>"
                   "<pre>  %2</pre>Pointing it at a new texture changes "
                   "them too. To isolate THIS mesh, 'Detach this mesh to "
                   "its own private material' first.<br><br>Continue "
                   "anyway?")
                    .arg(qs(hex_key_lower(material_key)),
                         geos.join(QStringLiteral("\n  "))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Pick an image for this material"), QString(),
        tr("Images (*.png *.dds *.tga *.bmp *.jpg *.jpeg);;All Files "
           "(*)"));
    if (path.isEmpty()) return;
    QString err;
    const QString asset_ref = project_->import_asset(path, &err);
    if (asset_ref.isEmpty()) {
        QMessageBox::critical(this, tr("New texture for material"),
                              tr("Failed to import image:\n%1").arg(err));
        return;
    }

    // DXT5 when the image has alpha, DXT1 otherwise (smaller).
    QString encode = QStringLiteral("7");
    {
        const QImage img(path);
        if (!img.isNull()) {
            encode = img.hasAlphaChannel() ? QStringLiteral("7")
                                           : QStringLiteral("5");
            if (img.hasAlphaChannel()) {
                // hasAlphaChannel is format-level; check the pixels.
                const QImage rgba =
                    img.convertToFormat(QImage::Format_RGBA8888);
                bool any = false;
                for (int y = 0; y < rgba.height() && !any; ++y) {
                    const uchar* line = rgba.constScanLine(y);
                    for (int x = 0; x < rgba.width(); ++x)
                        if (line[x * 4 + 3] < 255) {
                            any = true;
                            break;
                        }
                }
                encode = any ? QStringLiteral("7") : QStringLiteral("5");
            }
        }
    }

    const quint32 new_tex = allocate_modded_key(
        [this, mat_entry_idx] {
            std::set<quint32> used = project_minted_keys();
            for (quint32 k : bin_keys(quint32(mat_entry_idx)))
                used.insert(k);
            return used;
        }());
    const QString base = QFileInfo(path).fileName();
    // 1. add the texture into the material's bin (AddTexture op shape).
    {
        Value op = make_obj();
        op.obj["op"] = make_str("add_texture");
        Value tgt = make_obj();
        tgt.obj["entry_key"] = make_str(hex_key_lower(mat_entry_key));
        op.obj["target"] = std::move(tgt);
        Value prm = make_obj();
        prm.obj["new_key"] = make_str(hex_key_lower(new_tex));
        prm.obj["source"] = make_str(asset_ref.toStdString());
        prm.obj["encode"] = make_str(encode.toStdString());
        op.obj["params"] = std::move(prm);
        op.obj["label"] = make_str(
            tr("new texture %1 in %2 ← %3")
                .arg(qs(hex_key_lower(new_tex)),
                     qs(bf_->files.at(uint32_t(mat_entry_idx)).name), base)
                .toStdString());
        project_->add_operation(std::move(op));
    }
    // 2. point THIS material at it (must run after the add_texture above)
    {
        Value op = make_obj();
        op.obj["op"] = make_str("retarget_material_texture");
        Value tgt = make_obj();
        tgt.obj["entry_key"] = make_str(hex_key_lower(mat_entry_key));
        tgt.obj["sub_key"] = make_str(hex_key_lower(material_key));
        op.obj["target"] = std::move(tgt);
        Value prm = make_obj();
        prm.obj["new_texture_key"] = make_str(hex_key_lower(new_tex));
        if (layer) prm.obj["layer"] = make_num(layer);
        op.obj["params"] = std::move(prm);
        project_->add_operation(std::move(op));
    }
    project_->save();
    const QString layer_note =
        layer ? tr(" layer %1").arg(layer) : QString();
    log_->append(
        tr("Recorded: material %1%2 -> NEW private texture %3 (fmt%4) ← "
           "%5 (this material only; shared texture untouched)")
            .arg(qs(hex_key_lower(material_key)), layer_note,
                 qs(hex_key_lower(new_tex)), encode, base));
    log_->append(tr("  (table still shows BF state; applies at Build)"));
}

// Open the render-flags dialog and record a set_material_flags op.
void MeshSwapTab::edit_material_flags(quint32 material_key,
                                      long long mat_entry_idx,
                                      const jade::MatRenderFlags& rf) {
    if (!project_ || project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Render flags"),
            tr("No project open. Use File → New Project or Open Project "
               "first."));
        return;
    }
    if (!bf_ || mat_entry_idx < 0
        || !bf_->files.count(uint32_t(mat_entry_idx)))
        return;
    if (!guard_crossbin_edit(tr("change its render flags"))) return;
    const quint32 entry_key = bf_->files.at(uint32_t(mat_entry_idx)).key;
    const quint32 orig = rf.ul_flags;
    MaterialFlagsDialog dlg(orig, (long long)material_key, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const quint32 new_flags = dlg.result_flags();
    if (new_flags == orig) {
        QMessageBox::information(this, tr("Render flags"),
                                 tr("No change — flags are unchanged."));
        return;
    }
    // Warn if the sub-material is shared by other meshes.
    std::vector<quint32> shared;
    for (const MatRow& r : current_mat_rows_)
        if (r.material_key == (long long)material_key) {
            shared = r.shared_with_geos;
            break;
        }
    if (!shared.empty()) {
        QStringList geos;
        for (quint32 g : shared) geos << qs(hex_key_lower(g));
        if (QMessageBox::question(
                this, tr("Material is shared"),
                tr("Sub-material <b>%1</b> is also used by:<br><pre>  "
                   "%2</pre>Changing its render flags affects those "
                   "meshes too. Continue?")
                    .arg(qs(hex_key_lower(material_key)),
                         geos.join(QStringLiteral("\n  "))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }
    // SetMaterialFlags._params_to_dict shape.
    Value op = make_obj();
    op.obj["op"] = make_str("set_material_flags");
    Value tgt = make_obj();
    tgt.obj["entry_key"] = make_str(hex_key_lower(entry_key));
    tgt.obj["sub_key"] = make_str(hex_key_lower(material_key));
    op.obj["target"] = std::move(tgt);
    Value prm = make_obj();
    prm.obj["ul_flags"] = make_str(hex_key_lower(new_flags));
    op.obj["params"] = std::move(prm);
    project_->add_operation(std::move(op));
    project_->save();
    log_->append(tr("Recorded: material %1 render flags %2 -> %3")
                     .arg(qs(hex_key_lower(material_key)),
                          qs(hex_key_lower(orig)),
                          qs(hex_key_lower(new_flags))));
    log_->append(tr("  (table still shows BF state; applies at Build)"));
}

// Open a material picker and append a set_element_material op — make THIS
// mesh's element use a different material from the bin.
void MeshSwapTab::set_element_material(int element,
                                       long long current_material_key) {
    if (!project_ || project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Use different material"),
            tr("No project open. Use File → New Project or Open Project "
               "first; the tab records edits into a project rather than "
               "writing to the .bf directly."));
        return;
    }
    const long long idx = current_entry_idx();
    if (idx < 0 || !bf_ || current_grm_key_ < 0) return;
    if (!guard_crossbin_edit(tr("change this element's material"))) return;
    const QVariant gv = geo_combo_->currentData();
    if (!gv.isValid()) return;
    const quint32 geo_key = gv.toUInt();
    const quint32 entry_key = bf_->files.at(uint32_t(idx)).key;
    const quint32 grm_key = quint32(current_grm_key_);
    // The container may live in a sibling bin (Shape/MAT split).
    const long long cont_idx =
        current_grm_entry_idx_ >= 0 ? current_grm_entry_idx_ : idx;
    if (!bf_->files.count(uint32_t(cont_idx))) return;
    const quint32 container_entry_key =
        bf_->files.at(uint32_t(cont_idx)).key;

    // Offer materials from the container's bin so the new slot's material
    // resolves where the container lives.
    MaterialPickerDialog dlg(bf_path_, quint32(cont_idx),
                             current_material_key, project_,
                             (long long)container_entry_key, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const long long new_key = dlg.selected_key();
    if (new_key < 0) return;
    if (current_material_key >= 0 && new_key == current_material_key) {
        QMessageBox::information(
            this, tr("Use different material"),
            tr("That's the material this element already uses — nothing "
               "to record."));
        return;
    }
    // Appending a brand-new material grows the container and moves the
    // engine's clamp-to-last-slot target. Warn unless reusing a slot.
    if (!guard_append_clamp(quint32(new_key))) return;

    // SetElementMaterial._params_to_dict shape.
    Value op = make_obj();
    op.obj["op"] = make_str("set_element_material");
    Value tgt = make_obj();
    tgt.obj["entry_key"] = make_str(hex_key_lower(entry_key));
    tgt.obj["geo_key"] = make_str(hex_key_lower(geo_key));
    tgt.obj["multi_key"] = make_str(hex_key_lower(grm_key));
    if (container_entry_key != entry_key)
        tgt.obj["container_entry_key"] =
            make_str(hex_key_lower(container_entry_key));
    op.obj["target"] = std::move(tgt);
    Value prm = make_obj();
    prm.obj["element"] = make_num(element);
    prm.obj["new_material_key"] = make_str(hex_key_lower(quint32(new_key)));
    op.obj["params"] = std::move(prm);
    project_->add_operation(std::move(op));
    project_->save();
    const QString xb =
        container_entry_key == entry_key
            ? QString()
            : tr(" (container in %1)")
                  .arg(qs(hex_key_lower(container_entry_key)));
    log_->append(
        tr("Recorded: GEO %1 element %2 in entry %3 -> material %4 (this "
           "mesh only)%5")
            .arg(qs(hex_key_lower(geo_key)))
            .arg(element)
            .arg(qs(hex_key_lower(entry_key)),
                 qs(hex_key_lower(quint32(new_key))), xb));
    log_->append(
        tr("  (table still shows BF state; the change applies at Build)"));
}

// Every resource key already minted by ops in the open project, so a
// fresh detach never re-uses one.
std::set<quint32> MeshSwapTab::project_minted_keys() const {
    std::set<quint32> keys;
    if (!project_) return keys;
    for (const Value& op : project_->operations) {
        for (const char* attr : {"new_key", "new_container_key"}) {
            const quint32 v = params_key(op, attr);
            if (v) keys.insert(v);
        }
        const Value* prm = op.find("params");
        const Value* subs = prm ? prm->find("new_sub_keys") : nullptr;
        if (subs && subs->is_arr())
            for (const Value& v : subs->arr) {
                quint32 k = 0;
                if (v.is_str())
                    k = QString::fromStdString(v.str).toUInt(nullptr, 16);
                else if (v.is_num())
                    k = quint32(v.num);
                if (k) keys.insert(k);
            }
    }
    return keys;
}

// All sub-entry keys in a bin (so a minted key never collides).
std::set<quint32> MeshSwapTab::bin_keys(quint32 entry_idx) const {
    std::set<quint32> out;
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bf_->read_data(entry_idx));
        if (r.ok)
            for (const jade::SubEntry& s : jade::walk_sub_entries(r.data))
                out.insert(s.key);
    } catch (const std::exception&) {
    }
    return out;
}

// Give THIS mesh its own private materials: clone each DISTINCT material
// this mesh's elements use and append the clone as a new slot on the SAME
// container the engine already loads, then repoint the elements at the
// clones. Rides the proven, streaming-safe add_material +
// set_element_material path (the private-container design froze the game —
// LOA streaming wall).
void MeshSwapTab::detach_geo_material() {
    if (!project_ || project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Detach material"),
            tr("No project open. Use File → New Project or Open Project "
               "first; the tab records edits into a project rather than "
               "writing to the .bf directly."));
        return;
    }
    const long long idx = current_entry_idx();
    const QVariant gv = geo_combo_->currentData();
    if (idx < 0 || !bf_ || !gv.isValid()) return;
    const quint32 geo_key = gv.toUInt();
    if (!asset_index_) {
        QMessageBox::warning(
            this, tr("Detach material"),
            tr("The asset index isn't ready yet — it's needed to find the "
               "real (cross-bin) material this mesh uses. Try again once "
               "the archive has finished indexing."));
        return;
    }

    MeshMatInfo info = resolve_mesh_materials(quint32(idx), geo_key);
    if (!info.ok) {
        QMessageBox::warning(
            this, tr("Detach material"),
            tr("Couldn't resolve this mesh's material:\n%1")
                .arg(info.error));
        return;
    }
    if (info.grm_key < 0 || info.grm_entry_idx < 0) {
        QMessageBox::information(
            this, tr("Detach material"),
            tr("This mesh has no resolvable multi-material container — "
               "nothing to detach."));
        return;
    }
    const quint32 grm_key = quint32(info.grm_key);
    const quint32 grm_idx = quint32(info.grm_entry_idx);
    const quint32 geo_entry_key = bf_->files.at(uint32_t(idx)).key;
    const quint32 cont_entry_key = bf_->files.at(grm_idx).key;

    // Group the mesh's elements by the DISTINCT (material, texture) they
    // use; clone each once and repoint all elements that use it.
    struct Group {
        quint32 tex = 0;
        std::vector<int> elements;
    };
    std::map<quint32, Group> groups;
    for (const MatRow& rowv : info.elements) {
        if (rowv.material_key < 0 || rowv.texture_key < 0)
            continue;  // unresolvable — leave shared
        Group& g = groups[quint32(rowv.material_key)];
        if (!g.tex) g.tex = quint32(rowv.texture_key);
        g.elements.push_back(rowv.element);
    }
    if (groups.empty()) {
        QMessageBox::information(
            this, tr("Detach material"),
            tr("None of this mesh's elements resolve to an editable "
               "material/texture — nothing to detach."));
        return;
    }

    const QString src_bin = bf_->files.count(grm_idx)
                                ? qs(bf_->files.at(grm_idx).name)
                                : QStringLiteral("?");
    const QString xb =
        cont_entry_key == geo_entry_key
            ? QString()
            : tr(" (in sibling bin '%1')").arg(src_bin);
    size_t n_elems = 0;
    for (const auto& [mk, g] : groups) n_elems += g.elements.size();
    const auto r = QMessageBox::question(
        this, tr("Detach this mesh to private materials"),
        tr("This clones the %1 distinct material(s) this mesh uses and "
           "appends them as new private slots on container %2%3 — the "
           "container the engine already loads — then points this mesh's "
           "%4 element(s) at the clones.<br><br>The mesh looks identical "
           "afterwards (same textures), but it no longer shares its "
           "material with the body/arms: a later retexture or 'use "
           "different material' on it changes nobody else. Other meshes "
           "sharing the container are protected (their matIds are "
           "pinned).<br><br>Record this detach?")
            .arg(groups.size())
            .arg(qs(hex_key_lower(grm_key)), xb)
            .arg(n_elems),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (r != QMessageBox::Yes) return;

    std::set<quint32> used = project_minted_keys();
    for (quint32 k : bin_keys(quint32(idx))) used.insert(k);
    for (quint32 k : bin_keys(grm_idx)) used.insert(k);
    int n_clones = 0;
    for (const auto& [mk, g] : groups) {
        const quint32 new_key = allocate_modded_key(used, mk);
        used.insert(new_key);
        // 1. Clone this material (identical texture) into the container's
        // bin (AddMaterial op shape).
        {
            Value op = make_obj();
            op.obj["op"] = make_str("add_material");
            Value tgt = make_obj();
            tgt.obj["entry_key"] = make_str(hex_key_lower(cont_entry_key));
            op.obj["target"] = std::move(tgt);
            Value prm = make_obj();
            prm.obj["new_key"] = make_str(hex_key_lower(new_key));
            prm.obj["texture_key"] = make_str(hex_key_lower(g.tex));
            prm.obj["donor_key"] = make_str(hex_key_lower(mk));
            op.obj["params"] = std::move(prm);
            project_->add_operation(std::move(op));
        }
        // 2. Point every element that used it at the clone.
        for (int el : g.elements) {
            Value op = make_obj();
            op.obj["op"] = make_str("set_element_material");
            Value tgt = make_obj();
            tgt.obj["entry_key"] = make_str(hex_key_lower(geo_entry_key));
            tgt.obj["geo_key"] = make_str(hex_key_lower(geo_key));
            tgt.obj["multi_key"] = make_str(hex_key_lower(grm_key));
            if (cont_entry_key != geo_entry_key)
                tgt.obj["container_entry_key"] =
                    make_str(hex_key_lower(cont_entry_key));
            op.obj["target"] = std::move(tgt);
            Value prm = make_obj();
            prm.obj["element"] = make_num(el);
            prm.obj["new_material_key"] = make_str(hex_key_lower(new_key));
            op.obj["params"] = std::move(prm);
            project_->add_operation(std::move(op));
        }
        ++n_clones;
    }
    project_->save();
    log_->append(
        tr("Recorded: detach GEO %1 — %2 private material(s) appended to "
           "%3, %4 element(s) repointed (this mesh only)")
            .arg(qs(hex_key_lower(geo_key)))
            .arg(n_clones)
            .arg(qs(hex_key_lower(grm_key)))
            .arg(n_elems));
    log_->append(tr("  Build + reload to retexture the private materials "
                    "in isolation."));
}

void MeshSwapTab::on_browse_glb() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Pick a source GLB"), QString(),
        tr("glTF Binary (*.glb);;All Files (*)"));
    if (!path.isEmpty()) {
        glb_edit_->setText(path);
        update_enabled();
    }
}

// Cached {geo_key: [entry_idx, …]} for the loaded BF, built by
// decompressing every entry once and recording each gro_type-1 sub-entry
// key. Used only when no AssetIndex is available. (A compressed-bytes grep
// misses keys LZO encodes as back-references — the decompress scan is the
// only reliable one.)
const std::map<quint32, std::vector<quint32>>&
MeshSwapTab::geo_key_entry_index() {
    if (geo_key_idx_cache_valid_) return geo_key_idx_cache_;
    geo_key_idx_cache_.clear();
    for (const auto& [eidx, fi] : bf_->files) {
        try {
            const jade::LzoResult r =
                jade::decompress_lzo(bf_->read_data(eidx));
            if (!r.ok) continue;
            for (const jade::SubEntry& s :
                 jade::walk_sub_entries(r.data))
                if (!s.gro_null && s.gro_type == 1)
                    geo_key_idx_cache_[s.key].push_back(eidx);
        } catch (const std::exception&) {
            continue;  // a bad entry just contributes none
        }
    }
    geo_key_idx_cache_valid_ = true;
    return geo_key_idx_cache_;
}

// BF entry indices whose sub-entry table holds geo_key as a PC GEO
// (gro_type 1), in FAT order. Character shape bins are duplicated across
// zones for streaming, so the same mesh key lives in several entries.
std::vector<quint32> MeshSwapTab::find_entries_with_geo_key(
    quint32 geo_key) {
    if (!bf_ || bf_path_.isEmpty()) return {};
    std::set<quint32> entries;
    bool have = false;
    if (asset_index_) {
        auto it = asset_index_->by_key.find(geo_key);
        if (it != asset_index_->by_key.end()) {
            have = true;
            for (size_t ri : it->second) {
                const jade::AssetRecord& rec = asset_index_->records[ri];
                if (rec.gro_type == 1)
                    entries.insert(rec.parent_index);
            }
        }
    }
    if (!have) {
        auto it = geo_key_entry_index().find(geo_key);
        if (it != geo_key_entry_index().end())
            entries.insert(it->second.begin(), it->second.end());
    }
    return std::vector<quint32>(entries.begin(), entries.end());
}

// Append a replace_mesh op to the open project. The splice happens at Build
// time; skin compatibility and bone-mapping warnings are surfaced now.
void MeshSwapTab::on_apply() {
    if (!bf_ || geo_rows_.empty()) return;
    const QVariant keyv = geo_combo_->currentData();
    if (!keyv.isValid()) return;
    const quint32 key = keyv.toUInt();
    const QString glb_path = glb_edit_->text().trimmed();
    if (glb_path.isEmpty() || !QFileInfo::exists(glb_path)) {
        QMessageBox::warning(this, tr("Mesh Swap"),
                             tr("GLB file not found."));
        return;
    }
    const long long idx = current_entry_idx();
    if (idx < 0) {
        QMessageBox::warning(this, tr("Mesh Swap"),
                             tr("Pick a BF entry first."));
        return;
    }
    if (!project_ || project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Mesh Swap"),
            tr("No project open. Use File → New Project or Open Project "
               "first; the tab records edits into a project rather than "
               "writing to the .bf directly."));
        return;
    }

    // Foreign-mesh imports (a model rigged to a different skeleton) need a
    // GLB-joint -> original-bone map. Offer the remap dialog, pre-filled
    // by name match.
    std::map<int, int> bone_map;
    std::map<int, std::string> bone_map_source;
    std::set<int> bone_drops;
    int rigid_bind_bone = 0;
    std::map<int, int> drop_targets;
    bool auto_rig = false;
    bool diagnose_rest_pose = false;
    bool keep_original_skin = false;
    bool have_bone_map = false;
    bool have_skin_report = false;
    bool showed_remap = false;
    jade::SkinValidationResult skin_report;
    jade::patcher::AnalyzeBoneResult bm;
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bf_->read_data(uint32_t(idx)));
        QFile gf(glb_path);
        if (!r.ok || !gf.open(QIODevice::ReadOnly))
            throw std::runtime_error("could not read entry / GLB");
        const QByteArray raw = gf.readAll();
        const std::vector<uint8_t> glb_bytes(
            raw.constData(), raw.constData() + raw.size());
        skin_report = jade::validate_glb_skin(r.data, key, glb_bytes);
        if (skin_report.error.empty()) {
            have_skin_report = true;
            log_->append(
                tr("  Skin check: GLB joints=%1, orig bones=%2, name "
                   "matches=%3")
                    .arg(skin_report.glb_joint_count)
                    .arg(skin_report.orig_bone_count)
                    .arg(skin_report.name_matches.size()));
        } else {
            log_->append(
                tr("  validation skipped: %1").arg(qs(skin_report.error)));
        }
        bm = jade::patcher::analyze_bone_mapping(
            r.data, key, glb_bytes);
        if (!bm.ok) throw std::runtime_error(bm.error);
    } catch (const std::exception& e) {
        log_->append(tr("  bone-map analysis skipped: %1").arg(e.what()));
        bm.ok = false;
    }

    if (bm.ok && !bm.foreign_reason.empty())
        log_->append(tr("  foreign mesh: %1").arg(qs(bm.foreign_reason)));
    if (bm.ok && bm.is_foreign && !bm.dp_bone_names.empty()) {
        showed_remap = true;
        BoneRemapInit init;
        for (const auto& n : bm.glb_joint_names)
            init.glb_joint_names << qs(n);
        for (const auto& n : bm.dp_bone_names)
            init.dp_bone_names << qs(n);
        init.auto_map = bm.auto_map;
        init.auto_map_source = bm.auto_map_source;
        init.joint_stats = bm.joint_stats;
        init.joint_centroids = bm.joint_centroids;
        init.bone_centroids = bm.bone_centroids;
        init.mesh_diagonal = bm.mesh_diagonal;
        init.joint_rest_positions = bm.joint_rest_positions;
        init.bone_rest_positions = bm.bone_rest_positions;
        init.orig_bone_weight_shares = bm.orig_bone_weight_shares;
        BoneRemapDialog dlg(init, this);
        if (dlg.exec() != QDialog::Accepted) {
            log_->append(tr("  swap cancelled at the bone-remap step."));
            return;
        }
        const BoneRemapResult res = dlg.result();
        bone_map = res.bone_map;
        bone_map_source = res.source_map;
        bone_drops = res.drops;
        rigid_bind_bone = res.rigid_bind_bone;
        auto_rig = res.auto_rig;
        diagnose_rest_pose = res.diagnose_rest_pose;
        drop_targets = res.drop_targets;
        have_bone_map = true;
        keep_original_skin = dlg.keep_original_skin();
        if (keep_original_skin) {
            // Geometry-only import: discard the (possibly mangled) GLB
            // skin and keep the original bones/weights, transferred by
            // position. The bone map the dialog returned is irrelevant.
            bone_map.clear();
            bone_map_source.clear();
            bone_drops.clear();
            drop_targets.clear();
            have_bone_map = false;
            log_->append(
                tr("  keep original skinning: importing GLB geometry/UVs/"
                   "materials only; original bones + weights kept "
                   "(transferred onto the new verts by position). The "
                   "bone map is ignored."));
        } else {
            int n_user = 0, n_name = 0, n_geom = 0;
            for (const auto& [j, s] : bone_map_source) {
                if (s == "user") ++n_user;
                else if (s == "name") ++n_name;
                else if (s == "geometric") ++n_geom;
            }
            log_->append(
                tr("  bone map: %1 mapped, %2 dropped (%3 user, %4 name, "
                   "%5 geom)   rigid-bind=bone[%6]%7%8%9")
                    .arg(bone_map.size())
                    .arg(bone_drops.size())
                    .arg(n_user)
                    .arg(n_name)
                    .arg(n_geom)
                    .arg(rigid_bind_bone)
                    .arg(!drop_targets.empty()
                             ? tr("   per-drop targets=%1")
                                   .arg(drop_targets.size())
                             : QString(),
                         auto_rig ? tr("   auto-rig=on") : QString(),
                         diagnose_rest_pose
                             ? tr("   diagnose-rest-pose=on")
                             : QString()));
        }
    }
    if (!showed_remap && have_skin_report
        && !skin_report.warnings.empty()) {
        QStringList lines;
        for (const std::string& warning : skin_report.warnings)
            lines << QStringLiteral("  - %1").arg(qs(warning));
        const QMessageBox::StandardButton proceed = QMessageBox::question(
            this, tr("Skin mapping warnings"),
            tr("The GLB's skin doesn't look fully compatible:\n\n%1\n\n"
               "Record the operation anyway? (Build will revalidate and "
               "may downgrade these to errors with strict_skin=True.)")
                .arg(lines.join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No);
        if (proceed != QMessageBox::Yes) {
            log_->append(tr("  swap cancelled by user."));
            return;
        }
    }

    auto fit = bf_->files.find(uint32_t(idx));
    if (fit == bf_->files.end()) {
        QMessageBox::warning(this, tr("Mesh Swap"),
                             tr("Entry %1 not found in BigFile.").arg(idx));
        return;
    }

    // Cross-bin scope. The same mesh key is duplicated across zone bins
    // for streaming; offer to mirror the swap into all of them.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<quint32> all_entries = find_entries_with_geo_key(key);
    QApplication::restoreOverrideCursor();
    if (std::find(all_entries.begin(), all_entries.end(), quint32(idx))
        == all_entries.end())
        all_entries.insert(all_entries.begin(), quint32(idx));

    std::vector<quint32> target_entries{quint32(idx)};
    if (all_entries.size() > 1) {
        QStringList preview;
        preview << tr("  • entry %1: %2  (the one you selected)")
                       .arg(idx)
                       .arg(qs(fit->second.name));
        int shown = 0;
        int others_total = 0;
        for (quint32 e : all_entries) {
            if (e == quint32(idx)) continue;
            ++others_total;
            if (shown < 8 && bf_->files.count(e)) {
                preview << tr("  • entry %1: %2")
                               .arg(e)
                               .arg(qs(bf_->files.at(e).name));
                ++shown;
            }
        }
        if (others_total > 8)
            preview << tr("  • … and %1 more").arg(others_total - 8);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Mesh in multiple bins"));
        box.setText(tr("Mesh %1 is present in %2 BF entries.")
                        .arg(qs(hex_key_lower(key)))
                        .arg(all_entries.size()));
        box.setInformativeText(
            tr("Character shape bins are duplicated across zones for "
               "streaming, so replacing it in just one bin often won't "
               "show in-game when the engine streams the actor from a "
               "different zone.\n\n%1\n\nRecord this mesh swap in ALL "
               "bins, or only the one you selected?")
                .arg(preview.join(QLatin1Char('\n'))));
        QPushButton* btn_all =
            box.addButton(tr("Replace All (%1)").arg(all_entries.size()),
                          QMessageBox::AcceptRole);
        box.addButton(tr("Only This Bin"), QMessageBox::DestructiveRole);
        QPushButton* btn_cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(btn_all);
        box.exec();
        QAbstractButton* clicked = box.clickedButton();
        if (!clicked || clicked == btn_cancel) {
            log_->append(tr("  swap cancelled at the bin-scope step."));
            return;
        }
        if (clicked == btn_all) target_entries = all_entries;
    }

    QString err;
    const QString asset_ref = project_->import_asset(glb_path, &err);
    if (asset_ref.isEmpty()) {
        QMessageBox::critical(
            this, tr("Mesh Swap"),
            tr("Failed to import GLB into project: %1").arg(err));
        return;
    }

    int added = 0;
    for (quint32 tgt : target_entries) {
        auto tit = bf_->files.find(tgt);
        if (tit == bf_->files.end()) continue;
        // ReplaceMesh._params_to_dict shape.
        Value op = make_obj();
        op.obj["op"] = make_str("replace_mesh");
        Value tv = make_obj();
        tv.obj["entry_key"] = make_str(hex_key_lower(tit->second.key));
        tv.obj["sub_key"] = make_str(hex_key_lower(key));
        op.obj["target"] = std::move(tv);
        Value prm = make_obj();
        prm.obj["source"] = make_str(asset_ref.toStdString());
        prm.obj["strict_skin"] = make_bool(false);  // warn at build
        if (have_bone_map && !bone_map.empty()) {
            Value m = make_obj();
            for (const auto& [j, b] : bone_map)
                m.obj[std::to_string(j)] = make_num(b);
            prm.obj["bone_map"] = std::move(m);
        }
        if (have_bone_map && !bone_map_source.empty()) {
            Value m = make_obj();
            for (const auto& [j, s] : bone_map_source)
                m.obj[std::to_string(j)] = make_str(s);
            prm.obj["bone_map_source"] = std::move(m);
        }
        if (have_bone_map && !bone_drops.empty()) {
            Value arr = jade::json::make_arr();
            for (int j : bone_drops) arr.arr.push_back(make_num(j));
            prm.obj["bone_drops"] = std::move(arr);
        }
        if (rigid_bind_bone != 0)
            prm.obj["rigid_bind_bone"] = make_num(rigid_bind_bone);
        if (have_bone_map && !drop_targets.empty()) {
            Value m = make_obj();
            for (const auto& [j, b] : drop_targets)
                m.obj[std::to_string(j)] = make_num(b);
            prm.obj["drop_targets"] = std::move(m);
        }
        if (auto_rig) prm.obj["auto_rig"] = make_bool(true);
        if (diagnose_rest_pose)
            prm.obj["diagnose_rest_pose"] = make_bool(true);
        if (vcolor_check_->isChecked())
            prm.obj["import_vertex_colors"] = make_bool(true);
        if (keep_original_skin)
            prm.obj["keep_original_skin"] = make_bool(true);
        op.obj["params"] = std::move(prm);
        op.obj["label"] = make_str(
            tr("mesh %1 in %2 ← %3%4")
                .arg(qs(hex_key_lower(key)), qs(tit->second.name),
                     QFileInfo(glb_path).fileName(),
                     keep_original_skin ? tr(" [keep original skin]")
                                        : QString())
                .toStdString());
        const QString op_id = project_->add_operation(std::move(op));
        log_->append(tr("[+] added replace_mesh %1: entry %2 (%3) sub %4 "
                        "← %5…")
                         .arg(op_id, qs(hex_key_lower(tit->second.key)),
                              qs(tit->second.name), qs(hex_key_lower(key)),
                              asset_ref.left(18)));
        ++added;
    }
    if (added > 1)
        log_->append(tr("\n=> queued %1 replace_mesh ops (mesh mirrored "
                        "across %1 bins)")
                         .arg(added));
    update_enabled();
}

void MeshSwapTab::on_stub() {
    if (!bf_ || geo_rows_.empty()) return;
    const QVariant keyv = geo_combo_->currentData();
    if (!keyv.isValid()) return;
    const quint32 key = keyv.toUInt();
    const long long idx = current_entry_idx();
    if (idx < 0) {
        QMessageBox::warning(this, tr("Mesh Swap"),
                             tr("Pick a BF entry first."));
        return;
    }
    if (!project_ || project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Mesh Swap"),
            tr("No project open. Use File → New Project or Open Project "
               "first; the tab records edits into a project rather than "
               "writing to the .bf directly."));
        return;
    }
    auto fit = bf_->files.find(uint32_t(idx));
    if (fit == bf_->files.end()) {
        QMessageBox::warning(this, tr("Mesh Swap"),
                             tr("Entry %1 not found in BigFile.").arg(idx));
        return;
    }
    // StubMesh._params_to_dict shape (target only).
    Value op = make_obj();
    op.obj["op"] = make_str("stub_mesh");
    Value tgt = make_obj();
    tgt.obj["entry_key"] = make_str(hex_key_lower(fit->second.key));
    tgt.obj["sub_key"] = make_str(hex_key_lower(key));
    op.obj["target"] = std::move(tgt);
    op.obj["label"] = make_str(tr("stub mesh %1 in %2")
                                   .arg(qs(hex_key_lower(key)),
                                        qs(fit->second.name))
                                   .toStdString());
    const QString op_id = project_->add_operation(std::move(op));
    log_->append(
        tr("\n[+] added stub_mesh %1: entry %2 sub %3 (48-byte null "
           "template at build time)")
            .arg(op_id, qs(hex_key_lower(fit->second.key)),
                 qs(hex_key_lower(key))));
    update_enabled();
}

void MeshSwapTab::on_export_glb() {
    if (!bf_ || geo_rows_.empty()) return;
    const QVariant keyv = geo_combo_->currentData();
    if (!keyv.isValid()) return;
    const quint32 key = keyv.toUInt();
    const long long idx = current_entry_idx();
    if (idx < 0) {
        QMessageBox::warning(this, tr("Mesh Swap"),
                             tr("Pick a BF entry first."));
        return;
    }
    const QString suggested =
        QStringLiteral("geo_%1.glb").arg(qs(hex_key_lower(key)));
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save mesh as GLB"), suggested,
        tr("glTF Binary (*.glb);;All Files (*)"));
    if (path.isEmpty()) return;
    log_->append(tr("\n=== Exporting GEO %1 from entry %2 to %3 ===")
                     .arg(qs(hex_key_lower(key)))
                     .arg(idx)
                     .arg(path));
    worker_thread_ = new QThread(this);
    auto* worker =
        new MeshSwapExportGlbWorker(bf_path_, quint32(idx), key, path);
    worker_ = worker;
    worker->moveToThread(worker_thread_);
    connect(worker_thread_, &QThread::started, worker,
            &MeshSwapExportGlbWorker::run);
    connect(worker, &MeshSwapExportGlbWorker::log, this,
            &MeshSwapTab::on_log);
    connect(worker, &MeshSwapExportGlbWorker::finished, this,
            &MeshSwapTab::on_done);
    connect(worker, &MeshSwapExportGlbWorker::finished, worker_thread_,
            &QThread::quit);
    connect(worker_thread_, &QThread::finished, worker,
            &QObject::deleteLater);
    worker_thread_->start();
    update_enabled();
}

void MeshSwapTab::on_export_glb_hier() {
    if (!bf_ || geo_rows_.empty()) return;
    const QVariant keyv = geo_combo_->currentData();
    if (!keyv.isValid()) return;
    const quint32 key = keyv.toUInt();
    const long long idx = current_entry_idx();
    if (idx < 0) {
        QMessageBox::warning(this, tr("Mesh Swap"),
                             tr("Pick a BF entry first."));
        return;
    }
    const QString suggested =
        QStringLiteral("geo_%1_hier.glb").arg(qs(hex_key_lower(key)));
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save mesh as GLB (with bone hierarchy)"), suggested,
        tr("glTF Binary (*.glb);;All Files (*)"));
    if (path.isEmpty()) return;
    log_->append(tr("\n=== Exporting GEO %1 (with hierarchy) from entry "
                    "%2 to %3 ===")
                     .arg(qs(hex_key_lower(key)))
                     .arg(idx)
                     .arg(path));
    worker_thread_ = new QThread(this);
    auto* worker = new MeshSwapExportGlbHierWorker(bf_path_, quint32(idx),
                                                   key, path);
    worker_ = worker;
    worker->moveToThread(worker_thread_);
    connect(worker_thread_, &QThread::started, worker,
            &MeshSwapExportGlbHierWorker::run);
    connect(worker, &MeshSwapExportGlbHierWorker::log, this,
            &MeshSwapTab::on_log);
    connect(worker, &MeshSwapExportGlbHierWorker::finished, this,
            &MeshSwapTab::on_done);
    connect(worker, &MeshSwapExportGlbHierWorker::finished, worker_thread_,
            &QThread::quit);
    connect(worker_thread_, &QThread::finished, worker,
            &QObject::deleteLater);
    worker_thread_->start();
    update_enabled();
}

void MeshSwapTab::on_log(const QString& msg) { log_->append(msg); }

void MeshSwapTab::on_done(bool success, const QString& message) {
    if (success) {
        log_->append(message);
    } else {
        log_->append(tr("FAILED: %1").arg(message));
        QMessageBox::critical(this, tr("Mesh Swap"), message);
    }
    update_enabled();
}
