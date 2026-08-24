// PreviewPanel.cpp — multi-content preview for selected BF entries (port of
// gui/preview_panel.py).
//
// Shows a content summary header listing all sub-entry types, with a
// clickable sub-entry tree for entries that contain multiple resources
// (e.g. costumes with GAOs + geometry + materials + textures + animations).
// Individual previews: texture images, geometry 3D view, GAO info,
// animation details. Includes export helpers (PNG, DDS, GLB, OBJ, raw).
//
// PORT GAPS (each also marked at its call site):
//  * core/animation.py (parse_trl) — no C++ port: animation track listing,
//    animated preview, the playback combo and the animation JSON/GLB
//    exports are unavailable.
//  * core/ai_script.py / ai_enums.py — no C++ port: AI scripts show a hex
//    peek instead of decoded logic.
//  * io_ops/scene_export.py (find_skeleton_donor / build_scene /
//    glb_to_trl_payload) — no C++ port: donor-based animation preview and
//    "Import Anim from GLB…" are disabled.
// Static cross-bin material/texture resolution is ported through
// MeshSwapResolver and shared with MeshSwapTab. The remaining scene gaps
// above are specifically animation/AI work.

#include "PreviewPanel.hpp"

#include <QFile>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "GuiUtil.hpp"
#include "MeshPreview.hpp"
#include "MeshSwapResolver.hpp"
#include "Theme.hpp"
#include "jade/BigFile.hpp"
#include "jade/Gao.hpp"
#include "jade/GltfBuilder.hpp"
#include "jade/Material.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/ObjectKinds.hpp"

namespace {

using jade::SubEntry;

// Category labels and colors for sub-entries (_SUB_CATEGORIES).
struct SubCat {
    QString label;
    QColor color;
};

SubCat sub_category(const QString& key) {
    static const std::map<QString, SubCat> cats = {
        {QStringLiteral("texture"),
         {QStringLiteral("Texture"), QColor(100, 180, 100)}},
        {QStringLiteral("geometry"),
         {QStringLiteral("Geometry"), QColor(100, 150, 220)}},
        {QStringLiteral("material"),
         {QStringLiteral("Material"), QColor(200, 160, 80)}},
        {QStringLiteral("gao"),
         {QStringLiteral("Game Object"), QColor(180, 130, 200)}},
        {QStringLiteral("light"),
         {QStringLiteral("Light"), QColor(240, 210, 110)}},
        {QStringLiteral("palette"),
         {QStringLiteral("Palette"), QColor(100, 200, 160)}},
        {QStringLiteral("animation"),
         {QStringLiteral("Animation"), QColor(200, 100, 100)}},
        {QStringLiteral("ai_script"),
         {QStringLiteral("AI Script"), QColor(180, 180, 100)}},
        {QStringLiteral("reference"),
         {QStringLiteral("Resource index"), QColor(150, 160, 180)}},
        {QStringLiteral("unknown"),
         {QStringLiteral("Resource"), QColor(160, 160, 160)}},
    };
    auto it = cats.find(key);
    if (it != cats.end()) return it->second;
    return {QStringLiteral("Resource"), QColor(160, 160, 160)};
}

// f"{n:,}" — thousands separators.
QString fmt_thousands(qulonglong v) {
    return QLocale(QLocale::English).toString(v);
}

float f32_from_bits(uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// True when the payload begins with an ASCII '.ext' tag ('.' + 3 alpha).
bool starts_with_ext_tag(const std::vector<uint8_t>& d) {
    if (d.size() < 4 || d[0] != '.') return false;
    for (int i = 1; i < 4; ++i)
        if (!std::isalpha(int(d[size_t(i)]))) return false;
    return true;
}

// Format names shown on the texture page.
QString tex_format_name(uint32_t fmt) {
    switch (fmt) {
        case 0: return QStringLiteral("BGRA");
        case 1: return QStringLiteral("PAL8");
        case 5: return QStringLiteral("DXT1");
        case 6: return QStringLiteral("DXT3");
        case 7: return QStringLiteral("DXT5");
        case 11: return QStringLiteral("4bpp");
        default: return QStringLiteral("?");
    }
}

// GAO identity flags (from OBJconst.h) — gao.GAO_FLAGS.
const std::map<uint32_t, const char*>& gao_flag_names() {
    static const std::map<uint32_t, const char*> flags = {
        {0x00000001, "Bone"},       {0x00000002, "Anim"},
        {0x00000100, "ColMap"},     {0x00000200, "ZDM"},
        {0x00000400, "ZDE"},        {0x00001000, "Base"},
        {0x00002000, "Extended"},   {0x00004000, "Visual"},
        {0x00010000, "InitialPos"}, {0x00040000, "Links"},
        {0x00080000, "OBBox"},      {0x00200000, "AddMatrix"},
        {0x00400000, "Hierarchy"},  {0x00800000, "Group"},
        {0x02000000, "Events"},     {0x04000000, "FlashMatrix"},
        {0x08000000, "Sound"},      {0x10000000, "ODE"},
    };
    return flags;
}

// object_kinds.category_label — editor-kind display labels.
QString marker_kind_label(const std::string& kind) {
    static const std::map<std::string, QString> labels = {
        {"camera", QStringLiteral("Camera")},
        {"light", QStringLiteral("Light")},
        {"sound", QStringLiteral("Sound")},
        {"fx", QStringLiteral("FX / Particle")},
        {"trigger", QStringLiteral("Trigger / Sector")},
        {"trap", QStringLiteral("Trap")},
        {"actor", QStringLiteral("Actor / Enemy")},
        {"spawner", QStringLiteral("Spawner")},
        {"waypoint", QStringLiteral("Waypoint / Path")},
        {"logic", QStringLiteral("Logic / Manager")},
        {"other", QStringLiteral("Other")},
    };
    auto it = labels.find(kind);
    return it != labels.end() ? it->second : QStringLiteral("Other");
}

// asset_index.EXT_TYPE_INFO — declared extension → friendly type name.
QString ext_type_label(const std::string& ext) {
    static const std::map<std::string, QString> info = {
        {".ofc", QStringLiteral("AI function (.ofc)")},
        {".ova", QStringLiteral("AI vars (.ova)")},
        {".txi", QStringLiteral("Texture info (.txi)")},
        {".txs", QStringLiteral("Texture surface (.txs)")},
        {".txg", QStringLiteral("Text/lang group (.txg)")},
        {".gri", QStringLiteral("Grid (.gri)")},
        {".ttt", QStringLiteral("Data table (.ttt)")},
        {".bra", QStringLiteral("BRA resource (.bra)")},
        {".nova", QStringLiteral("NOVA resource (.nova)")},
    };
    auto it = info.find(ext);
    if (it != info.end()) return it->second;
    return QString::fromStdString(ext) + QStringLiteral(" resource");
}

// (category_key, label, detail) triple for a sub-entry
// (_classify_sub_entry).
struct ClassifyResult {
    QString cat;
    QString label;
    QString detail;
};

ClassifyResult classify_sub(const SubEntry& s) {
    const uint8_t* d = s.data.data();
    const size_t n = s.data.size();
    const uint32_t gro = s.gro_null ? 0u : s.gro_type;  // gro_type or 0

    // Texture: detected by magic markers in payload.
    if (n && jade::is_texture_entry(d, n)) {
        jade::TexInfo ti = jade::parse_texture(d, n);
        if (ti.valid)
            return {QStringLiteral("texture"), QStringLiteral("Texture"),
                    QStringLiteral("%1×%2 fmt%3")
                        .arg(ti.width)
                        .arg(ti.height)
                        .arg(ti.format)};
        return {QStringLiteral("texture"), QStringLiteral("Texture"),
                QString()};
    }

    // Palette: 256-colour BGRA LUT (1024B, not a texture, not a ref-list,
    // and not a small-GRO-typed resource — guards a 1024B
    // geo/light/material).
    if (n == 1024 && !(gro >= 1 && gro <= 5) && !starts_with_ext_tag(s.data))
        return {QStringLiteral("palette"), QStringLiteral("Palette"),
                QStringLiteral("256 colours")};

    // Geometry: GRO type == 1, GEO v7.
    if (!s.gro_null && s.gro_type == 1 && n) {
        const uint32_t one = 1;
        if (jade::is_geometry_entry(d, n, &one)) {
            jade::GeoInfo geo = jade::parse_geometry(d, n);
            if (geo.ok) {
                QString detail = QStringLiteral("%1 verts, %2 tris")
                                     .arg(geo.nb_points)
                                     .arg(geo.nb_tris);
                if (geo.skin_present) detail += QStringLiteral(" (skinned)");
                return {QStringLiteral("geometry"),
                        QStringLiteral("Geometry"), detail};
            }
            return {QStringLiteral("geometry"), QStringLiteral("Geometry"),
                    QString()};
        }
    }

    // Light: GRO type 2 (verified SoT v9 / WW+T2T v10).
    if (!s.gro_null && s.gro_type == jade::GRO_LIGHT
        && jade::is_light_payload(d, n)) {
        jade::LightInfo li = jade::parse_light(d, n);
        return {QStringLiteral("light"), QStringLiteral("Light"),
                li.ok ? QString::fromStdString(li.type_name) : QString()};
    }

    // GAO: identified by .gao/.wol extension.
    if (s.ext == ".gao" || s.ext == ".wol") {
        jade::GaoInfo hdr = jade::parse_gao_full(d, n);
        return {QStringLiteral("gao"), QStringLiteral("Game Object"),
                hdr.ok ? QString::fromStdString(hdr.name) : QString()};
    }

    // Material: GRO type 3 (single), 4 (multi), 5 (multitexture).
    if (!s.gro_null
        && (s.gro_type == 3 || s.gro_type == 4 || s.gro_type == 5)) {
        jade::MatInfo mat =
            jade::parse_material(d, n, int(s.gro_type));
        QString tp;
        if (mat.ok) {
            tp = mat.type == 0 ? QStringLiteral("single")
                 : mat.type == 1 ? QStringLiteral("multitexture")
                 : mat.type == 2 ? QStringLiteral("multi")
                                 : QString();
        }
        return {QStringLiteral("material"), QStringLiteral("Material"), tp};
    }

    // Animation: upper 16 bits of gro_type == 0x0002 (TRL track list).
    // Exclude low byte 0x30 (AI script) so 0x00020030 isn't stolen.
    // PORT GAP: core/animation.py parse_trl is unported — the Python
    // showed the parsed track count here.
    if (!s.gro_null && (s.gro_type >> 16) == 0x0002
        && (s.gro_type & 0xFF) != 0x30)
        return {QStringLiteral("animation"), QStringLiteral("Animation"),
                QString()};

    // AI script: low byte 0x30 (function / proclist / model list).
    // PORT GAP: core/ai_script.py classify_ai_script is unported — the
    // Python showed the decoded kind (model/function/bytecode) here.
    if (!s.gro_null && (s.gro_type & 0xFF) == 0x30 && n)
        return {QStringLiteral("ai_script"), QStringLiteral("AI Script"),
                QStringLiteral("%1B").arg(fmt_thousands(n))};

    // Resource index: a packed [ext][key] dependency list (engine
    // streaming metadata).
    if (s.ext.empty() && n >= 8 && starts_with_ext_tag(s.data))
        return {QStringLiteral("reference"),
                QStringLiteral("Resource index"),
                QStringLiteral("%1 refs").arg(n / 8)};

    QString type_label;
    if (!s.ext.empty())
        type_label = QString::fromStdString(s.ext);
    else if (gro)
        type_label = QStringLiteral("type ") + hex_key(gro);
    else
        type_label = QStringLiteral("raw");
    return {QStringLiteral("unknown"), QStringLiteral("Resource"),
            type_label};
}

const SubEntry* find_sub_by_key(const std::vector<SubEntry>& subs,
                                uint32_t key) {
    for (const SubEntry& s : subs)
        if (s.key == key) return &s;
    return nullptr;
}

// A hex dump ("OFFSET  HEX  ASCII") of at most max_bytes, with a trailing
// "more bytes" note. `off_width` = 8 (sub hex) or 4 (AI peek).
QStringList hex_dump_lines(const std::vector<uint8_t>& payload,
                           size_t max_bytes, int off_width) {
    QStringList lines;
    const size_t show = std::min(payload.size(), max_bytes);
    for (size_t off = 0; off < show; off += 16) {
        QString hex_part, ascii_part;
        for (size_t i = off; i < std::min(off + 16, payload.size()); ++i) {
            hex_part += QStringLiteral("%1 ")
                            .arg(payload[i], 2, 16, QLatin1Char('0'))
                            .toUpper();
            const uint8_t b = payload[i];
            ascii_part += (b >= 32 && b < 127) ? QChar(b) : QChar('.');
        }
        hex_part = hex_part.trimmed();
        lines.append(QStringLiteral("%1  %2  %3")
                         .arg(off, off_width, 16, QLatin1Char('0'))
                         .arg(hex_part, -47)
                         .arg(ascii_part));
    }
    return lines;
}

// Export format options per content type (_EXPORT_FORMATS). Textures are
// PNG-only and geometry GLB-only — those go through the Texture Swap /
// Mesh Swap pipelines; the other types keep their exporters.
QList<QPair<QString, QString>> export_formats(const QString& cat) {
    if (cat == QLatin1String("texture"))
        return {{QStringLiteral("PNG Image"), QStringLiteral(".png")}};
    if (cat == QLatin1String("geometry"))
        return {{QStringLiteral("glTF Binary"), QStringLiteral(".glb")}};
    if (cat == QLatin1String("gao"))
        return {{QStringLiteral("JSON Dump"), QStringLiteral(".json")},
                {QStringLiteral("Raw Binary"), QStringLiteral(".bin")}};
    if (cat == QLatin1String("animation"))
        return {{QStringLiteral("glTF Binary"), QStringLiteral(".glb")},
                {QStringLiteral("Raw TRL"), QStringLiteral(".trl")},
                {QStringLiteral("JSON Dump"), QStringLiteral(".json")}};
    if (cat == QLatin1String("material"))
        return {{QStringLiteral("JSON Dump"), QStringLiteral(".json")},
                {QStringLiteral("Raw Binary"), QStringLiteral(".bin")}};
    return {{QStringLiteral("Raw Binary"), QStringLiteral(".bin")}};
}

// Translation T (floats 12..14) of a 68-byte Jade matrix blob, or zeros.
std::array<float, 3> matrix_t(const std::vector<uint8_t>& raw) {
    std::array<float, 3> t{0.0f, 0.0f, 0.0f};
    if (raw.size() >= 68)
        for (int i = 0; i < 3; ++i)
            std::memcpy(&t[size_t(i)], raw.data() + 48 + i * 4, 4);
    return t;
}

}  // namespace

// ── ZoomScrollArea ──

void ZoomScrollArea::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        emit zoomRequested(event->angleDelta().y() > 0 ? 1 : -1);
        event->accept();
    } else {
        QScrollArea::wheelEvent(event);
    }
}

// ── PreviewPanel ──

PreviewPanel::PreviewPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    // ── Content summary header ──
    summary_ = new QLabel(tr("Select a file entry to preview"));
    summary_->setWordWrap(true);
    summary_->setStyleSheet(QStringLiteral("font-size: 11pt;"));
    layout->addWidget(summary_);

    // ── Toolbar ──
    // Export controls now live on the Asset Browser's main table
    // (multi-select rows + "Export Selected…"); the panel keeps only the
    // animation re-import action, which is animation-specific.
    auto* export_row = new QHBoxLayout();
    import_anim_btn_ = new QPushButton(tr("Import Anim from GLB…"));
    import_anim_btn_->setToolTip(tr(
        "Re-encode the matching animation from a GLB file back to TRL "
        "bytes, sized to fit this sub-entry's slot for in-place "
        "replacement."));
    connect(import_anim_btn_, &QPushButton::clicked, this,
            &PreviewPanel::on_import_animation_glb);
    import_anim_btn_->setVisible(false);
    // PORT GAP: io_ops/scene_export.glb_to_trl_payload is unported — the
    // action stays disabled.
    import_anim_btn_->setEnabled(false);
    import_anim_btn_->setToolTip(import_anim_btn_->toolTip()
                                 + tr("\n\nnot ported yet: GLB→TRL "
                                      "re-encoding (io_ops/scene_export)"));
    export_row->addWidget(import_anim_btn_);
    export_row->addStretch();
    layout->addLayout(export_row);

    // ── Main area: sub-entry list (left) + detail view (right) ──
    splitter_ = new QSplitter(Qt::Horizontal);

    // Sub-entry tree (grouped by category).
    sub_tree_ = new QTreeWidget();
    sub_tree_->setMaximumWidth(320);
    sub_tree_->setMinimumWidth(160);
    sub_tree_->setHeaderHidden(true);
    sub_tree_->setIndentation(16);
    connect(sub_tree_, &QTreeWidget::currentItemChanged, this,
            &PreviewPanel::on_tree_item_changed);
    splitter_->addWidget(sub_tree_);

    // Detail stack.
    detail_stack_ = new QStackedWidget();

    // Page 0: placeholder.
    auto* ph = new QLabel(tr("Click a sub-entry to preview"));
    ph->setAlignment(Qt::AlignCenter);
    detail_stack_->addWidget(ph);

    // Page 1: texture preview.
    tex_widget_ = new QWidget();
    auto* tex_lay = new QVBoxLayout(tex_widget_);
    tex_lay->setContentsMargins(0, 0, 0, 0);
    tex_info_ = new QLabel();
    tex_info_->setWordWrap(true);
    tex_lay->addWidget(tex_info_);

    // Zoom toolbar.
    auto* zoom_row = new QHBoxLayout();
    zoom_row->addWidget(new QLabel(tr("Zoom:")));
    auto* zoom_out_btn = new QPushButton(QStringLiteral("−"));
    zoom_out_btn->setFixedWidth(28);
    connect(zoom_out_btn, &QPushButton::clicked, this,
            [this] { zoom_tex(-1); });
    zoom_row->addWidget(zoom_out_btn);
    auto* zoom_in_btn = new QPushButton(QStringLiteral("+"));
    zoom_in_btn->setFixedWidth(28);
    connect(zoom_in_btn, &QPushButton::clicked, this,
            [this] { zoom_tex(1); });
    zoom_row->addWidget(zoom_in_btn);
    tex_zoom_reset_btn_ = new QPushButton(QStringLiteral("100%"));
    tex_zoom_reset_btn_->setFixedWidth(56);
    connect(tex_zoom_reset_btn_, &QPushButton::clicked, this,
            [this] { set_tex_zoom(1.0); });
    zoom_row->addWidget(tex_zoom_reset_btn_);
    zoom_row->addWidget(new QLabel(tr("(Ctrl + scroll)")));
    zoom_row->addStretch();
    tex_lay->addLayout(zoom_row);

    tex_label_ = new QLabel();
    tex_label_->setAlignment(Qt::AlignCenter);
    tex_scroll_ = new ZoomScrollArea();
    tex_scroll_->setWidget(tex_label_);
    tex_scroll_->setWidgetResizable(false);
    tex_scroll_->setAlignment(Qt::AlignCenter);
    connect(tex_scroll_, &ZoomScrollArea::zoomRequested, this,
            &PreviewPanel::zoom_tex);
    tex_lay->addWidget(tex_scroll_, 1);
    detail_stack_->addWidget(tex_widget_);

    // Page 2: info / text.
    info_widget_ = new QWidget();
    auto* info_lay = new QVBoxLayout(info_widget_);
    info_lay->setContentsMargins(0, 0, 0, 0);
    info_header_ = new QLabel();
    info_header_->setWordWrap(true);
    info_lay->addWidget(info_header_);
    info_text_ = new QTextEdit();
    info_text_->setReadOnly(true);
    info_text_->setFont(QFont(QStringLiteral("Consolas"), 9));
    info_lay->addWidget(info_text_, 1);
    detail_stack_->addWidget(info_widget_);

    splitter_->addWidget(detail_stack_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 3);
    layout->addWidget(splitter_, 1);

    // Page 3: 3D viewer.
    viewer_panel_ = new MeshPreviewPanel();
    detail_stack_->addWidget(viewer_panel_);

    // Page 4: material breakdown — render flags + colour params, plus a
    // thumbnail row per texture the material references. Mirrors the Mesh
    // Swap tab's materials table for a single selected material.
    mat_widget_ = new QWidget();
    auto* mat_lay = new QVBoxLayout(mat_widget_);
    mat_lay->setContentsMargins(0, 0, 0, 0);
    mat_header_ = new QLabel();
    mat_header_->setTextFormat(Qt::RichText);
    mat_header_->setWordWrap(true);
    mat_lay->addWidget(mat_header_);
    mat_scroll_ = new QScrollArea();
    mat_scroll_->setWidgetResizable(true);
    mat_body_ = new QWidget();
    mat_body_lay_ = new QVBoxLayout(mat_body_);
    mat_body_lay_->setContentsMargins(0, 0, 0, 0);
    mat_body_lay_->setAlignment(Qt::AlignTop);
    mat_scroll_->setWidget(mat_body_);
    mat_lay->addWidget(mat_scroll_, 1);
    MAT_PAGE_ = detail_stack_->addWidget(mat_widget_);

    detail_stack_->setCurrentIndex(0);
}

void PreviewPanel::set_asset_index(std::shared_ptr<jade::AssetIndex> idx) {
    asset_index_ = std::move(idx);
    // Resolver is index-backed — force a rebuild so cross-bin lookups
    // pick the new index up (or stop, on nullptr for a new archive).
    resolver_.reset();
    resolver_index_ = -2;
    xbin_tex_cache_.clear();
}

// A MeshSwapResolver over the open archive, seeded with the current
// entry. Rebuilt when the selected entry changes; the same resolver (and
// its bin cache) is reused across the per-element texture lookups of one
// geometry preview. (The Python additionally shared one resolver per
// entry with the Mesh-Swap table via get_cached_resolver — a cache-warm
// optimisation, not a behaviour difference.)
MeshSwapResolver* PreviewPanel::crossbin_resolver() {
    const long long cur = current_result_.index;
    if (!resolver_ || resolver_index_ != cur) {
        resolver_ = std::make_shared<MeshSwapResolver>(
            bf_.get(), asset_index_, quint32(cur < 0 ? 0 : cur),
            current_subs_);
        resolver_index_ = cur;
    }
    return resolver_.get();
}

// Decode a texture by key from wherever it lives, cached. Used for
// per-element textures that resolve into a sibling bin.
QImage PreviewPanel::decode_texture_crossbin(uint32_t tex_key) {
    auto hit = xbin_tex_cache_.find(tex_key);
    if (hit != xbin_tex_cache_.end()) return hit->second;
    QImage img;
    if (bf_) {
        auto [sub, pidx] = crossbin_resolver()->lookup(tex_key);
        if (sub != nullptr
            && jade::is_texture_entry(sub->data.data(), sub->data.size())) {
            const jade::TexInfo ti =
                jade::parse_texture(sub->data.data(), sub->data.size());
            const size_t pixlen = sub->data.size() > ti.pix_start
                                      ? sub->data.size() - ti.pix_start
                                      : 0;
            if (ti.valid && !jade::is_placeholder(ti, pixlen)) {
                // Palettes (PAL8) live beside the texture in its own bin.
                const std::vector<SubEntry>& host =
                    pidx >= 0 ? crossbin_resolver()->subs_of(quint32(pidx))
                              : current_subs_;
                const std::vector<uint8_t>* pal =
                    jade::palette_for_texture(ti, host);
                const std::vector<uint8_t> rgba = jade::decode_texture(
                    sub->data.data(), sub->data.size(), ti,
                    pal ? pal->data() : nullptr, pal ? pal->size() : 0);
                if (!rgba.empty())
                    img = QImage(rgba.data(), int(ti.width), int(ti.height),
                                 int(ti.width) * 4, QImage::Format_RGBA8888)
                              .copy();
            }
        }
    }
    xbin_tex_cache_[tex_key] = img;
    return img;
}

void PreviewPanel::show_error(const QString& msg) {
    summary_->setText(
        QStringLiteral("<b style='color:red'>Error:</b> ") + msg);
    sub_tree_->clear();
    detail_stack_->setCurrentIndex(0);
}

void PreviewPanel::show_entry(const PreviewEntryResult& result,
                              std::shared_ptr<jade::BigFile> bf,
                              const QString& bf_path,
                              long long prefer_sub_index, int compact) {
    const bool compact_b =
        compact < 0 ? (prefer_sub_index >= 0) : (compact != 0);
    sub_tree_->setVisible(!compact_b);
    const std::vector<SubEntry>& subs = result.subs;
    current_subs_ = subs;
    current_result_ = result;

    // Per-entry key → declared-extension map (from this entry's dependency
    // lists). Names records whose body 'type' word is 0 — AI .ofc/.ova,
    // texture .txi/.txs, … — so the hex view labels them instead of
    // showing an anonymous "Resource".
    entry_ref_ext_.clear();
    for (const SubEntry& s : current_subs_) {
        if (s.ext.empty() && !s.gro_null && s.gro_type > 0xFFFF
            && starts_with_ext_tag(s.data)) {
            for (const jade::RefEntry& re :
                 jade::decode_ref_list(s.data.data(), s.data.size()))
                entry_ref_ext_.emplace(re.key, re.ext);
        }
    }
    bf_ = std::move(bf);
    bf_path_ = bf_path;
    // New entry: drop any donor we resolved for the previous one, and the
    // cross-bin resolver (its bin cache is seeded with the entry's subs).
    donor_searched_ = false;
    resolver_.reset();
    resolver_index_ = -2;
    sub_tree_->clear();
    entry_textures_.clear();

    if (current_subs_.empty()) {
        summary_->setText(
            QStringLiteral("<b>Empty entry</b> — Raw %1B → Dec %2B")
                .arg(fmt_thousands(result.raw_size))
                .arg(fmt_thousands(result.dec_size)));
        detail_stack_->setCurrentIndex(0);
        return;
    }

    // Classify all sub-entries.
    std::map<QString, int> cat_counts;
    classified_.clear();
    std::set<uint32_t> tex_keys;  // distinct texture keys for the summary
    for (const SubEntry& s : current_subs_) {
        const ClassifyResult c = classify_sub(s);
        classified_.push_back({c.cat, c.label, c.detail});
        cat_counts[c.cat] += 1;
        if (c.cat == QLatin1String("texture")) tex_keys.insert(s.key);
    }

    // Build summary line. These counts describe ONE BF entry — the
    // selected asset's parent container inside the .bf — not the whole
    // archive. The texture count is reported as DISTINCT keys: Jade stores
    // every texture twice in an entry (a header-only placeholder stub then
    // the real pixel record, both under the same key), so the raw
    // sub-entry tally is ~2× the real texture count.
    const int n_tex_subs = cat_counts.count(QStringLiteral("texture"))
                               ? cat_counts[QStringLiteral("texture")]
                               : 0;
    std::map<QString, int> display_counts = cat_counts;
    if (!tex_keys.empty())
        display_counts[QStringLiteral("texture")] = int(tex_keys.size());
    QStringList summary_parts;
    const QStringList cat_order = {
        QStringLiteral("gao"),       QStringLiteral("geometry"),
        QStringLiteral("material"),  QStringLiteral("texture"),
        QStringLiteral("animation"), QStringLiteral("ai_script"),
        QStringLiteral("unknown")};
    for (const QString& cat_key : cat_order) {
        auto it = display_counts.find(cat_key);
        const int n = it != display_counts.end() ? it->second : 0;
        if (n > 0) {
            const SubCat sc = sub_category(cat_key);
            summary_parts.append(
                QStringLiteral("<span style='color:%1'>%2 %3%4</span>")
                    .arg(sc.color.name())
                    .arg(n)
                    .arg(sc.label)
                    .arg(n > 1 ? QStringLiteral("s") : QString()));
        }
    }

    // Name the parent entry so the scope is unambiguous.
    QString ent_label = QStringLiteral("Entry");
    if (bf_ && result.index >= 0) {
        auto it = bf_->files.find(uint32_t(result.index));
        if (it != bf_->files.end() && !it->second.name.empty())
            ent_label = QString::fromStdString(it->second.name);
    }
    summary_->setText(
        QStringLiteral("<b>%1</b>: %2 sub-entries — %3 — Raw %4B → Dec %5B")
            .arg(ent_label)
            .arg(current_subs_.size())
            .arg(summary_parts.join(QStringLiteral(", ")))
            .arg(fmt_thousands(result.raw_size))
            .arg(fmt_thousands(result.dec_size)));
    summary_->setToolTip(tr(
        "Counts are for the selected asset's parent BF entry only (one "
        "container inside the .bf), not the whole archive.\n"
        "Textures are shown as distinct keys (%1); this entry holds %2 "
        "texture sub-entries because Jade stores each texture twice — a "
        "placeholder stub plus the real pixel record under the same key.")
                             .arg(tex_keys.size())
                             .arg(n_tex_subs));

    // Build category tree.
    std::map<QString, std::vector<std::pair<long long, QString>>> cat_groups;
    for (long long i = 0; i < (long long)classified_.size(); ++i)
        cat_groups[classified_[size_t(i)].cat].push_back(
            {i, classified_[size_t(i)].detail});

    QTreeWidgetItem* first_item = nullptr;
    const QStringList previewable = {
        QStringLiteral("texture"), QStringLiteral("geometry"),
        QStringLiteral("gao"), QStringLiteral("animation"),
        QStringLiteral("ai_script")};
    for (const QString& cat_key : cat_order) {
        auto git = cat_groups.find(cat_key);
        if (git == cat_groups.end() || git->second.empty()) continue;
        const auto& items = git->second;
        const SubCat sc = sub_category(cat_key);
        auto* group = new QTreeWidgetItem(sub_tree_);
        group->setText(0, items.size() > 1
                              ? QStringLiteral("%1s (%2)")
                                    .arg(sc.label)
                                    .arg(items.size())
                              : sc.label);
        group->setForeground(0, sc.color);
        group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
        QFont font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
        group->setExpanded(true);

        for (const auto& [sub_idx, detail] : items) {
            const SubEntry& s = current_subs_[size_t(sub_idx)];
            QString text = hex_key(s.key);
            if (!detail.isEmpty())
                text += QStringLiteral("  ") + detail;
            auto* child = new QTreeWidgetItem(group);
            child->setText(0, text);
            child->setForeground(0, sc.color);
            child->setData(0, Qt::UserRole, qlonglong(sub_idx));
            if (!first_item && previewable.contains(cat_key))
                first_item = child;
        }
    }

    // Auto-select: caller's preferred sub_index wins, else first
    // previewable (texture/geo/gao/anim), else first leaf.
    QTreeWidgetItem* preferred = nullptr;
    if (prefer_sub_index >= 0
        && prefer_sub_index < (long long)current_subs_.size())
        preferred = find_sub_leaf(prefer_sub_index);
    if (preferred) {
        sub_tree_->setCurrentItem(preferred);
    } else if (first_item) {
        sub_tree_->setCurrentItem(first_item);
    } else if (sub_tree_->topLevelItemCount() > 0) {
        QTreeWidgetItem* top = sub_tree_->topLevelItem(0);
        if (top->childCount() > 0) sub_tree_->setCurrentItem(top->child(0));
    }
}

// Locate the tree leaf whose UserRole == sub_idx, or nullptr.
QTreeWidgetItem* PreviewPanel::find_sub_leaf(long long sub_idx) {
    for (int ti = 0; ti < sub_tree_->topLevelItemCount(); ++ti) {
        QTreeWidgetItem* group = sub_tree_->topLevelItem(ti);
        for (int ci = 0; ci < group->childCount(); ++ci) {
            QTreeWidgetItem* leaf = group->child(ci);
            const QVariant v = leaf->data(0, Qt::UserRole);
            if (v.isValid() && v.toLongLong() == sub_idx) return leaf;
        }
    }
    return nullptr;
}

// Handle tree item selection change.
void PreviewPanel::on_tree_item_changed(QTreeWidgetItem* current,
                                        QTreeWidgetItem* /*previous*/) {
    if (!current) {
        detail_stack_->setCurrentIndex(0);
        current_sub_idx_ = -1;
        current_cat_ = QStringLiteral("unknown");
        return;
    }
    const QVariant v = current->data(0, Qt::UserRole);
    if (!v.isValid()) return;  // category header clicked
    on_sub_selected(v.toLongLong());
}

// Preview the selected sub-entry.
void PreviewPanel::on_sub_selected(long long row) {
    if (row < 0 || row >= (long long)current_subs_.size()) {
        detail_stack_->setCurrentIndex(0);
        current_sub_idx_ = -1;
        current_cat_ = QStringLiteral("unknown");
        return;
    }
    current_sub_idx_ = row;
    const SubEntry& s = current_subs_[size_t(row)];
    const ClassifyResult c = classify_sub(s);
    current_cat_ = c.cat;

    // The animation re-import action is only meaningful for anims.
    import_anim_btn_->setVisible(c.cat == QLatin1String("animation"));

    if (c.cat == QLatin1String("texture"))
        show_texture(s);
    else if (c.cat == QLatin1String("palette"))
        show_palette(s);
    else if (c.cat == QLatin1String("geometry"))
        show_geometry(s);
    else if (c.cat == QLatin1String("light"))
        show_light(s);
    else if (c.cat == QLatin1String("gao"))
        show_gao(s);
    else if (c.cat == QLatin1String("animation"))
        show_animation(s);
    else if (c.cat == QLatin1String("material"))
        show_material(s);
    else if (c.cat == QLatin1String("ai_script"))
        show_ai_script(s);
    else if (c.cat == QLatin1String("reference"))
        show_reference(s);
    else
        show_sub_hex(s);
}

// ── Individual preview methods ──

void PreviewPanel::show_texture(const SubEntry& sub) {
    const uint8_t* d = sub.data.data();
    const size_t n = sub.data.size();
    jade::TexInfo ti = jade::parse_texture(d, n);
    if (!ti.valid) {
        show_detail_text(QStringLiteral("Texture"),
                         tr("Could not parse texture header."));
        return;
    }

    const std::vector<uint8_t>* pal =
        jade::palette_for_texture(ti, current_subs_);
    const std::vector<uint8_t> rgba = jade::decode_texture(
        d, n, ti, pal ? pal->data() : nullptr, pal ? pal->size() : 0);
    if (rgba.empty()) {
        show_detail_text(
            QStringLiteral("Texture ") + hex_key(sub.key),
            tr("Could not decode texture.\nFormat: %1, Size: %2x%3")
                .arg(ti.format)
                .arg(ti.width)
                .arg(ti.height));
        return;
    }
    const QImage img(rgba.data(), int(ti.width), int(ti.height),
                     int(ti.width) * 4, QImage::Format_RGBA8888);
    const QPixmap pixmap = QPixmap::fromImage(img.copy());

    const size_t pix_size = n > ti.pix_start ? n - ti.pix_start : 0;
    tex_info_->setText(
        QStringLiteral("<b>Texture</b> %1<br>"
                       "Size: %2 × %3 | Format: %4 (%5) | "
                       "Mipmaps: %6 | Pixel data: %7B")
            .arg(hex_key(sub.key))
            .arg(ti.width)
            .arg(ti.height)
            .arg(ti.format)
            .arg(tex_format_name(ti.format))
            .arg(ti.mip_count)
            .arg(fmt_thousands(pix_size)));
    tex_src_pixmap_ = pixmap;
    tex_zoom_ = 1.0;
    apply_tex_zoom();
    detail_stack_->setCurrentIndex(1);
}

// Step the texture zoom in (direction > 0) or out (direction < 0).
void PreviewPanel::zoom_tex(int direction) {
    if (tex_src_pixmap_.isNull()) return;
    const double factor =
        direction > 0 ? TEX_ZOOM_STEP : 1.0 / TEX_ZOOM_STEP;
    set_tex_zoom(tex_zoom_ * factor);
}

void PreviewPanel::set_tex_zoom(double zoom) {
    tex_zoom_ = std::clamp(zoom, TEX_ZOOM_MIN, TEX_ZOOM_MAX);
    apply_tex_zoom();
}

void PreviewPanel::apply_tex_zoom() {
    if (tex_src_pixmap_.isNull()) return;
    const QPixmap& src = tex_src_pixmap_;
    const int sw = std::max(1, int(std::lround(src.width() * tex_zoom_)));
    const int sh = std::max(1, int(std::lround(src.height() * tex_zoom_)));
    // Crisp texels when zoomed in, smooth when shrinking.
    const Qt::TransformationMode mode = tex_zoom_ >= 1.0
                                            ? Qt::FastTransformation
                                            : Qt::SmoothTransformation;
    const QPixmap scaled =
        src.scaled(sw, sh, Qt::KeepAspectRatio, mode);
    tex_label_->setPixmap(scaled);
    tex_label_->resize(scaled.size());
    tex_zoom_reset_btn_->setText(
        QStringLiteral("%1%").arg(tex_zoom_ * 100.0, 0, 'f', 0));
}

// Render a 256-colour BGRA palette as a 16×16 swatch grid on the texture
// page (so the zoom controls work).
void PreviewPanel::show_palette(const SubEntry& sub) {
    const std::vector<uint8_t>& payload = sub.data;
    if (payload.size() < 1024) {
        show_detail_text(QStringLiteral("Palette"),
                         tr("Palette payload too short."));
        return;
    }
    // 256 entries × BGRA → RGBA, laid out 16×16.
    QImage img(16, 16, QImage::Format_RGBA8888);
    for (int i = 0; i < 256; ++i) {
        const uint8_t b = payload[size_t(i) * 4];
        const uint8_t g = payload[size_t(i) * 4 + 1];
        const uint8_t r = payload[size_t(i) * 4 + 2];
        uint8_t* px = img.scanLine(i / 16) + (i % 16) * 4;
        px[0] = r;
        px[1] = g;
        px[2] = b;
        px[3] = 255;  // show opaque; many LUT slots store a=0
    }
    const QImage big =
        img.scaled(256, 256, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    tex_info_->setText(
        QStringLiteral("<b>Palette</b> %1<br>"
                       "256 colours · BGRA LUT (used by PAL8 / 4-bit "
                       "textures)")
            .arg(hex_key(sub.key)));
    tex_src_pixmap_ = QPixmap::fromImage(big);
    tex_zoom_ = 1.0;
    apply_tex_zoom();
    detail_stack_->setCurrentIndex(1);
}

namespace {

// (r,g,b) from a light colour word (0x00BBGGRR, low byte = R).
std::array<int, 3> light_rgb(uint32_t raw) {
    return {int(raw & 0xFF), int((raw >> 8) & 0xFF),
            int((raw >> 16) & 0xFF)};
}

// A 256×96 diffuse(+specular) colour swatch for a parsed light
// (_light_swatch).
QImage light_swatch(const jade::LightInfo& li) {
    const std::array<int, 3> dif =
        li.diffuse.present ? light_rgb(li.diffuse.bits)
                           : std::array<int, 3>{0, 0, 0};
    constexpr int W = 256, H = 96;
    QImage img(W, H, QImage::Format_RGBA8888);
    img.fill(QColor(30, 30, 30));
    QPainter* painter = nullptr;
    (void)painter;
    if (li.specular.present) {
        const std::array<int, 3> spec = light_rgb(li.specular.bits);
        for (int y = 0; y < H; ++y) {
            uint8_t* row = img.scanLine(y);
            for (int x = 0; x < W; ++x) {
                const auto& c = x < W / 2 ? dif : spec;
                row[x * 4] = uint8_t(c[0]);
                row[x * 4 + 1] = uint8_t(c[1]);
                row[x * 4 + 2] = uint8_t(c[2]);
                row[x * 4 + 3] = 255;
            }
        }
    } else {
        img.fill(QColor(dif[0], dif[1], dif[2]));
    }
    return img;
}

// HTML lines describing a parsed light's parameters (_light_lines).
QStringList light_lines(const jade::LightInfo& li) {
    auto rgb = [](uint32_t raw, bool present) {
        if (!present) return QStringLiteral("—");
        const auto c = light_rgb(raw);
        return QStringLiteral("(%1, %2, %3)").arg(c[0]).arg(c[1]).arg(c[2]);
    };
    auto hex6 = [](uint32_t raw) {
        return QStringLiteral("0x%1")
            .arg(raw, 6, 16, QLatin1Char('0'))
            .toUpper()
            .replace(QStringLiteral("0X"), QStringLiteral("0x"));
    };
    QStringList lines;
    lines << QStringLiteral("Type: <b>%1</b>  ·  version %2")
                 .arg(QString::fromStdString(li.type_name))
                 .arg(li.version);
    lines << QStringLiteral("Diffuse: %1  rgb %2")
                 .arg(hex6(li.diffuse.present ? li.diffuse.bits : 0))
                 .arg(rgb(li.diffuse.bits, li.diffuse.present));
    if (li.has_specular)
        lines << QStringLiteral(
                     "Specular: %1  rgb %2  <i>(left = diffuse, right = "
                     "specular)</i>")
                     .arg(hex6(li.specular.present ? li.specular.bits : 0))
                     .arg(rgb(li.specular.bits, li.specular.present));
    QStringList rng;
    if (li.near_.present)
        rng << QStringLiteral("near %1").arg(
            double(f32_from_bits(li.near_.bits)), 0, 'f', 3);
    if (li.far_.present)
        rng << QStringLiteral("far %1").arg(
            double(f32_from_bits(li.far_.bits)), 0, 'f', 3);
    if (!rng.isEmpty())
        lines << QStringLiteral("Range: ") + rng.join(QStringLiteral(", "));
    if (li.type == 2)  // Spot
        lines << QStringLiteral("Cone: inner %1, outer %2 rad")
                     .arg(double(f32_from_bits(
                              li.inner.present ? li.inner.bits : 0)),
                          0, 'f', 3)
                     .arg(double(f32_from_bits(
                              li.outer.present ? li.outer.bits : 0)),
                          0, 'f', 3);
    if (li.has_intensity && li.intensity.present)
        lines << QStringLiteral("Intensity: %1").arg(
            double(f32_from_bits(li.intensity.bits)), 0, 'f', 3);
    return lines;
}

}  // namespace

// Render a light (swatch + param lines) on the texture page.
void PreviewPanel::show_light_on_tex_page(const jade::LightInfo& li,
                                          const QStringList& header_lines,
                                          const QString& footer) {
    QStringList lines = header_lines;
    lines += light_lines(li);
    if (!footer.isEmpty()) lines.append(footer);
    tex_info_->setText(lines.join(QStringLiteral("<br>")));
    tex_src_pixmap_ = QPixmap::fromImage(light_swatch(li));
    tex_zoom_ = 1.0;
    apply_tex_zoom();
    detail_stack_->setCurrentIndex(1);
}

// Show a GRO_Light resource's parameters plus colour swatches.
void PreviewPanel::show_light(const SubEntry& sub) {
    const jade::LightInfo li =
        jade::parse_light(sub.data.data(), sub.data.size());
    if (!li.ok) {
        show_sub_hex(sub);
        return;
    }
    show_light_on_tex_page(
        li, {QStringLiteral("<b>Light</b> ") + hex_key(sub.key)},
        QStringLiteral("<i>Position + editing live on the light-marker GAO "
                       "(Place Objects → Edit Light).</i>"));
}

namespace {

// Split vertices at UV seams for per-vertex UV rendering
// (_split_verts_for_viewer). Returns flat arrays:
//  - verts/norms in Jade Z-up coords (conversion done by the viewer)
//  - uvs with V flipped for OpenGL
//  - orig maps each split vertex to its original Jade vertex index.
struct SplitResult {
    std::vector<float> verts, norms, uvs;
    std::vector<uint32_t> faces;
    std::vector<int32_t> orig;
    std::vector<int32_t> face_elems;
};

SplitResult split_verts_for_viewer(const jade::GeoInfo& geo) {
    SplitResult out;
    const size_t n = geo.nb_points;
    const size_t n_tris = geo.faces.size() / 7;
    out.face_elems.reserve(n_tris);
    for (size_t f = 0; f < n_tris; ++f)
        out.face_elems.push_back(int32_t(geo.faces[f * 7 + 6]));

    const bool have_norms = geo.normals.size() == n * 3;
    if (geo.uvs.empty()) {
        out.verts = geo.vertices;
        if (have_norms) {
            out.norms = geo.normals;
        } else {
            out.norms.assign(n * 3, 0.0f);
            for (size_t i = 0; i < n; ++i) out.norms[i * 3 + 2] = 1.0f;
        }
        out.uvs.assign(n * 2, 0.0f);
        out.orig.reserve(n);
        for (size_t i = 0; i < n; ++i) out.orig.push_back(int32_t(i));
        out.faces.reserve(n_tris * 3);
        for (size_t f = 0; f < n_tris; ++f)
            for (int k = 0; k < 3; ++k)
                out.faces.push_back(geo.faces[f * 7 + size_t(k)]);
        return out;
    }

    const size_t n_uvs = geo.uvs.size() / 2;
    std::unordered_map<uint64_t, uint32_t> cache;
    cache.reserve(n_tris * 3);
    out.faces.reserve(n_tris * 3);
    for (size_t f = 0; f < n_tris; ++f) {
        for (int k = 0; k < 3; ++k) {
            const uint16_t vi = geo.faces[f * 7 + size_t(k)];
            const uint16_t ui = geo.faces[f * 7 + 3 + size_t(k)];
            const uint64_t key = (uint64_t(vi) << 32) | ui;
            auto it = cache.find(key);
            uint32_t ix;
            if (it == cache.end()) {
                ix = uint32_t(out.verts.size() / 3);
                cache.emplace(key, ix);
                if (size_t(vi) < n) {
                    out.verts.push_back(geo.vertices[size_t(vi) * 3]);
                    out.verts.push_back(geo.vertices[size_t(vi) * 3 + 1]);
                    out.verts.push_back(geo.vertices[size_t(vi) * 3 + 2]);
                } else {
                    out.verts.insert(out.verts.end(), {0.0f, 0.0f, 0.0f});
                }
                if (have_norms && size_t(vi) < n) {
                    out.norms.push_back(geo.normals[size_t(vi) * 3]);
                    out.norms.push_back(geo.normals[size_t(vi) * 3 + 1]);
                    out.norms.push_back(geo.normals[size_t(vi) * 3 + 2]);
                } else {
                    out.norms.insert(out.norms.end(), {0.0f, 0.0f, 1.0f});
                }
                if (size_t(ui) < n_uvs) {
                    out.uvs.push_back(geo.uvs[size_t(ui) * 2]);
                    out.uvs.push_back(1.0f - geo.uvs[size_t(ui) * 2 + 1]);
                } else {
                    out.uvs.insert(out.uvs.end(), {0.0f, 0.0f});
                }
                out.orig.push_back(int32_t(vi));
            } else {
                ix = it->second;
            }
            out.faces.push_back(ix);
        }
    }
    return out;
}

// A GEO-retained skin → the 3D viewer's SkinData shape (weight =
// pond / 65535.0).
std::shared_ptr<viewer3d::SkinData> geo_skin_to_viewer(
    const jade::GeoInfo& geo) {
    if (!geo.skin_present) return nullptr;
    auto skin = std::make_shared<viewer3d::SkinData>();
    skin->bones.reserve(geo.skin_bones.size());
    for (const jade::GeoBone& b : geo.skin_bones) {
        viewer3d::SkinBone sb;
        sb.bone_idx = int(b.bone_idx);
        sb.bind_matrix = b.bind_matrix;
        sb.has_bind = true;
        sb.weights.reserve(b.weights.size());
        for (const auto& [vi, pond] : b.weights)
            sb.weights.push_back({int(vi), float(pond) / 65535.0f});
        skin->bones.push_back(std::move(sb));
    }
    return skin;
}

}  // namespace

void PreviewPanel::show_geometry(const SubEntry& sub) {
    const jade::GeoInfo geo =
        jade::parse_geometry(sub.data.data(), sub.data.size());
    if (!geo.ok) {
        show_detail_text(QStringLiteral("Geometry"),
                         tr("Could not parse geometry data"));
        return;
    }

    if (!meshpreview::HAS_GL) {
        show_geometry_text(sub, geo);
        return;
    }

    // Split vertices at UV seams.
    SplitResult split = split_verts_for_viewer(geo);
    if (split.verts.empty() || split.faces.empty()) {
        show_geometry_text(sub, geo);
        return;
    }

    // Show the 3D viewer page FIRST so the widget gets realized before we
    // upload GL resources.
    detail_stack_->setCurrentIndex(3);

    // Find textures from sibling sub-entries.
    if (entry_textures_.empty()) entry_textures_ = find_entry_textures();
    // PORT GAP: _find_entry_animations needs core/animation.py parse_trl —
    // the animation combo never populates natively.

    // Load into the 3D viewer. Pick the texture this geometry's *material*
    // points at, rather than blindly using textures[0] — for
    // multi-material entries (e.g. Prince costume) the body / head /
    // weapons textures are all present and only one is correct here.
    const QImage best = texture_for_geometry(geo);
    const std::vector<QImage> elem_tex = element_textures(geo, sub.key);
    int have_elem = 0;
    for (const QImage& t : elem_tex)
        if (!t.isNull()) ++have_elem;
    // Per-element render flags so opaque / alpha-test / alpha-blend each
    // draw correctly (instead of blanket translucency). Walks the exact
    // same element→matId→sub-material chain as element_textures (incl.
    // the Shape/MAT merge override + cross-bin sub-material lookup) —
    // io_ops/mesh_swap.resolve_element_render_modes_xbin.
    std::vector<meshpreview::ElementRenderMode> elem_modes;
    {
        const std::vector<uint32_t> esubs = element_sub_keys(geo, sub.key);
        elem_modes.resize(esubs.size());
        for (size_t ei = 0; ei < esubs.size(); ++ei) {
            const uint32_t sk = esubs[ei];
            if (!sk || sk == 0xFFFFFFFFu) continue;
            const SubEntry* ms = nullptr;
            if (bf_)
                ms = crossbin_resolver()->lookup(sk).first;
            else
                ms = find_sub_by_key(current_subs_, sk);
            if (!ms) continue;
            const jade::MatRenderFlags rf =
                jade::parse_material_render_flags(ms->data.data(),
                                                  ms->data.size());
            if (rf.ok)
                elem_modes[ei] = {true, rf.blend != 0, rf.alpha_test,
                                  rf.alpha_thresh};
        }
    }
    std::vector<QImage> images;
    if (!best.isNull()) images.push_back(best);
    viewer_panel_->viewer()->load_animated_mesh(
        split.verts, split.norms, split.uvs, split.faces, split.orig,
        geo_skin_to_viewer(geo), images,
        have_elem > 0 ? split.face_elems : std::vector<int32_t>{},
        have_elem > 0 ? elem_tex : std::vector<QImage>{},
        have_elem > 0 ? elem_modes
                      : std::vector<meshpreview::ElementRenderMode>{});

    // PORT GAP: set_animations for skinned geos needs the (unported)
    // parsed-animation cache.

    // Info text. The animation count is fixed at 0 natively (see above).
    QString info = QStringLiteral("%1 verts, %2 tris | %3 tex | %4 anims")
                       .arg(geo.nb_points)
                       .arg(geo.nb_tris)
                       .arg(entry_textures_.size())
                       .arg(0);
    if (geo.skin_present)
        info += QStringLiteral(" | %1 bones").arg(geo.skin_bones.size());
    viewer_panel_->set_info(info);
}

// Fallback text display for geometry when OpenGL is unavailable.
void PreviewPanel::show_geometry_text(const SubEntry& sub,
                                      const jade::GeoInfo& geo) {
    QStringList lines;
    lines << QStringLiteral("Vertices: %1").arg(geo.nb_points);
    lines << QStringLiteral("Faces: %1").arg(geo.nb_tris);
    lines << QStringLiteral("Elements: %1").arg(geo.elements.size() / 2);
    lines << QStringLiteral("UVs: %1").arg(geo.uvs.size() / 2);
    lines << QStringLiteral("Normals: %1").arg(geo.normals.size() / 3);
    lines << QStringLiteral("Has Skin: %1")
                 .arg(geo.skin_present ? QStringLiteral("Yes")
                                       : QStringLiteral("No"));
    if (geo.skin_present) {
        const auto& bones = geo.skin_bones;
        lines << QStringLiteral("Bones: %1").arg(bones.size());
        for (size_t bi = 0; bi < bones.size() && bi < 20; ++bi)
            lines << QStringLiteral("  Bone %1: idx=%2, verts=%3")
                         .arg(bi)
                         .arg(bones[bi].bone_idx)
                         .arg(bones[bi].weights.size());
        if (bones.size() > 20)
            lines << QStringLiteral("  ... +%1 more").arg(bones.size() - 20);
    }
    if (!geo.elements.empty()) {
        lines << QString();
        lines << QStringLiteral("Element groups:");
        const size_t n_elems = geo.elements.size() / 2;
        for (size_t ei = 0; ei < n_elems && ei < 10; ++ei)
            lines << QStringLiteral("  Element %1: (nTri %2, matId %3)")
                         .arg(ei)
                         .arg(geo.elements[ei * 2])
                         .arg(geo.elements[ei * 2 + 1]);
    }
    show_detail_text(QStringLiteral("<b>Geometry</b> ") + hex_key(sub.key),
                     lines.join(QLatin1Char('\n')));
}

void PreviewPanel::show_gao(const SubEntry& sub) {
    const std::vector<uint8_t>& payload = sub.data;

    // Sibling context lets us classify the GAO the same way the level
    // editor and the browser list do (marker kind / visual / bone) and
    // resolve a light marker's GRO_Light resource.
    const std::vector<SubEntry>* subs_p = &current_subs_;
    std::vector<SubEntry> lone;
    if (current_subs_.empty()) {
        lone.push_back(sub);
        subs_p = &lone;
    }
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    std::unordered_set<uint32_t> all_gao_keys;
    for (const SubEntry& s : *subs_p) {
        by_key.emplace(s.key, &s);
        if (s.ext == ".gao" || s.ext == ".wol") all_gao_keys.insert(s.key);
    }
    const jade::GaoInfo full =
        jade::parse_gao_full(payload.data(), payload.size());

    // Marker classification (the Python classify_gao_marker), composed
    // from the exported ObjectKinds / AssetIndex primitives.
    std::string kind;         // empty = mk None (non-.gao / unparsed)
    uint32_t light_key = 0;
    jade::LightInfo light_info;
    if (full.ok && sub.ext == ".gao") {
        int gro_type = -1;  // Python None
        kind = "other";
        bool is_visual = false;
        if (full.vis_flag && full.vis_read) {
            auto it = by_key.find(full.gro_key);
            if (it != by_key.end()) {
                const SubEntry* gsub = it->second;
                gro_type = gsub->gro_null ? -1 : int(gsub->gro_type);
                if (gro_type == 1) {
                    const jade::GeoInfo g = jade::parse_geometry(
                        gsub->data.data(), gsub->data.size());
                    if (g.ok && g.nb_tris > 0) {
                        kind = "visual";
                        is_visual = true;
                    }
                }
            }
        }
        if (!is_visual) {
            const uint32_t father =
                full.hier_flag ? full.father_key : 0xFFFFFFFFu;
            const bool father_in_bin = all_gao_keys.count(father) != 0;
            if (jade::is_bone(full.name, full.identity, father_in_bin)) {
                kind = "bone";
            } else {
                kind = jade::classify_object(full.name, full.identity,
                                             gro_type, father_in_bin);
                // Light-marker promotion: the GAO owns a GRO_Light keyed
                // by its last u32.
                const jade::LightKeyOpt cand =
                    jade::light_key_from_gao_payload(payload.data(),
                                                     payload.size());
                if (cand.have) {
                    auto it = by_key.find(cand.key);
                    if (it != by_key.end() && !it->second->gro_null
                        && it->second->gro_type == jade::GRO_LIGHT) {
                        const jade::LightInfo li = jade::parse_light(
                            it->second->data.data(),
                            it->second->data.size());
                        if (li.ok) {
                            kind = "light";
                            light_key = cand.key;
                            light_info = li;
                        }
                    }
                }
            }
        }
    }

    // A light marker → render its light on the texture page (swatch +
    // params), prefixed with the marker's own header.
    if (kind == "light" && light_info.ok) {
        const std::array<float, 3> pos =
            full.gmat_present ? matrix_t(full.gmat_raw)
                              : std::array<float, 3>{0, 0, 0};
        const QStringList head = {
            QStringLiteral("<b>Light marker</b> %1  ·  %2")
                .arg(hex_key(sub.key))
                .arg(QString::fromStdString(full.name)),
            QStringLiteral(
                "Position: (%1, %2, %3)  ·  light resource %4")
                .arg(double(pos[0]), 0, 'f', 2)
                .arg(double(pos[1]), 0, 'f', 2)
                .arg(double(pos[2]), 0, 'f', 2)
                .arg(hex_key(light_key)),
        };
        show_light_on_tex_page(
            light_info, head,
            QStringLiteral(
                "<i>Edit via Place Objects → Edit Light.</i>"));
        return;
    }

    // Header conveys the editor kind (Spawner / Trigger / Camera / …) for
    // markers, or "Game Object" for a visual / generic object.
    QString head_label;
    if (!kind.empty() && kind != "visual" && kind != "bone"
        && kind != "other")
        head_label = marker_kind_label(kind) + QStringLiteral(" marker");
    else if (kind == "bone")
        head_label = QStringLiteral("Bone");
    else
        head_label = QStringLiteral("Game Object");

    QStringList lines;
    if (full.ok) {
        lines << QStringLiteral("Name: %1")
                     .arg(full.name.empty()
                              ? QStringLiteral("N/A")
                              : QString::fromStdString(full.name));
        lines << QStringLiteral("Version: %1").arg(full.version);
        lines << QStringLiteral("Identity: %1").arg(hex_key(full.identity));
        QStringList flags;
        for (const auto& [bit, name] : gao_flag_names())
            if (full.identity & bit) flags << QString::fromLatin1(name);
        if (!flags.isEmpty())
            lines << QStringLiteral("Flags: %1")
                         .arg(flags.join(QStringLiteral(", ")));

        if (full.gmat_present) {
            const std::array<float, 3> t = matrix_t(full.gmat_raw);
            lines << QStringLiteral("Position: (%1, %2, %3)")
                         .arg(double(t[0]), 0, 'f', 3)
                         .arg(double(t[1]), 0, 'f', 3)
                         .arg(double(t[2]), 0, 'f', 3);
        }

        if (full.vis_flag && full.vis_read) {
            lines << QString();
            lines << QStringLiteral("Visual references:");
            lines << QStringLiteral("  GRO (geometry): %1")
                         .arg(hex_key(full.gro_key));
            lines << QStringLiteral("  GRM (material): %1")
                         .arg(hex_key(full.grm_key));
        }

        if (full.hier_flag && full.hier_read && full.father_key
            && full.father_key != 0xFFFFFFFFu)
            lines << QStringLiteral("Parent GAO: %1")
                         .arg(hex_key(full.father_key));

        if (full.lmat_present) {
            const std::array<float, 3> lt = matrix_t(full.lmat_raw);
            lines << QStringLiteral("Local offset: (%1, %2, %3)")
                         .arg(double(lt[0]), 0, 'f', 3)
                         .arg(double(lt[1]), 0, 'f', 3)
                         .arg(double(lt[2]), 0, 'f', 3);
        }

        const size_t n_giz = full.gizmo_flat.size() / 2;
        if (n_giz) {
            lines << QString();
            lines << QStringLiteral("Gizmo pointers (%1):").arg(n_giz);
            for (size_t gi = 0; gi < n_giz && gi < 20; ++gi)
                lines << QStringLiteral("  [%1] GAO=%2")
                             .arg(gi)
                             .arg(hex_key(full.gizmo_flat[gi * 2]));
            if (n_giz > 20)
                lines << QStringLiteral("  ... +%1 more").arg(n_giz - 20);
        }
    }

    show_detail_text(QStringLiteral("<b>%1</b> %2")
                         .arg(head_label)
                         .arg(hex_key(sub.key)),
                     lines.join(QLatin1Char('\n')));
}

void PreviewPanel::show_animation(const SubEntry& sub) {
    // PORT GAP: the Python parsed the TRL (core/animation.py parse_trl),
    // resolved a skeleton donor (io_ops/scene_export.find_skeleton_donor)
    // and played the merged GLB scene. None of those cores are ported —
    // only the raw facts are shown.
    const uint32_t gro_type = sub.gro_null ? 0 : sub.gro_type;
    QStringList lines;
    lines << QStringLiteral("GRO type: %1").arg(hex_key(gro_type));
    lines << QStringLiteral("Payload size: %1B")
                 .arg(fmt_thousands(sub.data.size()));
    lines << QString();
    lines << tr("not ported yet: TRL track decoding (core/animation.py) — "
                "track listing and animated preview are unavailable.");
    show_detail_text(QStringLiteral("<b>Animation</b> ") + hex_key(sub.key),
                     lines.join(QLatin1Char('\n')));
}

// Material breakdown: render flags, colour params, and a thumbnail per
// referenced texture — the same information the Mesh Swap tab's materials
// table shows, for a single material selected in the browser.
//
// Three shapes are handled:
//  * Leaf (gro_type 3 single / 5 multitexture) — its own render state +
//    colours, with one row per texture *layer* it points at.
//  * Container (gro_type 4 multi) — the positional sub-material list;
//    each non-empty slot resolves to its sub-material's texture.
//  * Unparseable — falls back to a size note.
void PreviewPanel::show_material(const SubEntry& sub) {
    const uint8_t* d = sub.data.data();
    const size_t n = sub.data.size();
    const uint32_t gro_type = sub.gro_null ? 0 : sub.gro_type;
    const uint32_t key = sub.key;

    clear_layout(mat_body_lay_);

    const bool is_container =
        int(gro_type) == jade::GRO_TYPE_MAT_MULTI;
    jade::MatRenderFlags rf;
    if (!is_container) rf = jade::parse_material_render_flags(d, n);
    jade::MatHeader hdr;
    if (!is_container) hdr = jade::parse_material_header(d, n);

    QString type_label;
    switch (gro_type) {
        case 3: type_label = QStringLiteral("single"); break;
        case 4: type_label = QStringLiteral("multi (container)"); break;
        case 5: type_label = QStringLiteral("multitexture"); break;
        default: type_label = QStringLiteral("gro ") + hex_key(gro_type);
    }

    // ── Header line ──
    QStringList head_bits;
    head_bits << QStringLiteral("<b>Material</b> ") + hex_key(key);
    head_bits << QStringLiteral("type: %1").arg(type_label);
    if (hdr.ok) head_bits << QStringLiteral("kind %1").arg(hdr.kind);
    if (rf.ok)
        head_bits << QStringLiteral("<b style='color:#d8a85a'>%1</b>")
                         .arg(QString::fromStdString(rf.mode));
    mat_header_->setText(
        head_bits.join(QStringLiteral("&nbsp;&nbsp;·&nbsp;&nbsp;")));

    // ── Parameters block ──
    QStringList lines;
    if (rf.ok) {
        lines << QStringLiteral("Render state");
        lines << QStringLiteral("  ul_Flags      %1").arg(hex_key(rf.ul_flags));
        lines << QStringLiteral("  Mode          %1")
                     .arg(QString::fromStdString(rf.mode));
        lines << QStringLiteral("  Blend         %1 (%2)")
                     .arg(QString::fromStdString(rf.blend_name))
                     .arg(rf.blend);
        lines << QStringLiteral("  Colour op     %1 (%2)")
                     .arg(QString::fromStdString(rf.colorop_name))
                     .arg(rf.colorop);
        lines << QStringLiteral("  Alpha test    %1  (threshold %2)")
                     .arg(rf.alpha_test ? QStringLiteral("yes")
                                        : QStringLiteral("no"))
                     .arg(rf.alpha_thresh);
        QStringList fl;
        for (const std::string& f : rf.flags)
            fl << QString::fromStdString(f);
        lines << QStringLiteral("  Flags         %1")
                     .arg(fl.isEmpty() ? QStringLiteral("(none)")
                                       : fl.join(QStringLiteral(", ")));
        lines << QString();
    }
    if (hdr.ok) {
        lines << QStringLiteral("Colours / lighting");
        lines << QStringLiteral("  Ambient       %1").arg(hex_key(hdr.ambient));
        lines << QStringLiteral("  Diffuse       %1").arg(hex_key(hdr.diffuse));
        lines << QStringLiteral("  Specular      %1").arg(hex_key(hdr.specular));
        lines << QStringLiteral("  Specular exp  %1")
                     .arg(fmt_mat_float(hdr.spec_exp_raw,
                                        f32_from_bits(hdr.spec_exp_raw)));
        lines << QStringLiteral("  Opacity       %1")
                     .arg(fmt_mat_float(hdr.opacity_raw,
                                        f32_from_bits(hdr.opacity_raw)));
        lines << QString();
    }
    if (!rf.ok && !hdr.ok && !is_container) {
        lines << QStringLiteral(
                     "Could not decode material header (%1B payload).")
                     .arg(fmt_thousands(n));
        lines << QString();
    }

    QString params_text = lines.join(QLatin1Char('\n'));
    while (params_text.endsWith(QLatin1Char('\n'))) params_text.chop(1);
    auto* params = new QLabel(params_text);
    params->setFont(QFont(QStringLiteral("Consolas"), 9));
    params->setTextInteractionFlags(Qt::TextSelectableByMouse);
    params->setWordWrap(false);
    mat_body_lay_->addWidget(params);

    // ── Texture rows ──
    const std::vector<MatTexRow> rows =
        material_texture_rows(sub, gro_type, is_container);
    int n_real = 0;
    for (const MatTexRow& r : rows)
        if (!r.img.isNull() || r.tex_key) ++n_real;
    auto* hdr_lbl = new QLabel(
        rows.empty() ? QStringLiteral("<b>Textures</b> — none referenced")
                     : QStringLiteral("<b>Textures (%1)</b>").arg(n_real));
    mat_body_lay_->addWidget(hdr_lbl);

    const size_t shown =
        std::min(rows.size(), size_t(MAT_MAX_TEX_ROWS));
    for (size_t i = 0; i < shown; ++i)
        mat_body_lay_->addWidget(
            make_texture_row(rows[i].title, rows[i].tex_key, rows[i].img));
    if (rows.size() > shown) {
        auto* more = new QLabel(QStringLiteral("… +%1 more slots")
                                    .arg(rows.size() - shown));
        more->setStyleSheet(
            QStringLiteral("color:%1;").arg(theme::DIM_TEXT));
        mat_body_lay_->addWidget(more);
    }

    mat_body_lay_->addStretch(1);
    detail_stack_->setCurrentIndex(MAT_PAGE_);
}

// Build [(title, tex_key, image), …] for a material. Leaf materials list
// one entry per texture layer; containers list one entry per non-empty
// sub-material slot (resolved to its texture).
std::vector<PreviewPanel::MatTexRow> PreviewPanel::material_texture_rows(
    const SubEntry& sub, uint32_t gro_type, bool is_container) {
    std::vector<MatTexRow> rows;
    const uint8_t* d = sub.data.data();
    const size_t n = sub.data.size();
    if (is_container) {
        const jade::MatInfo mm =
            jade::parse_material(d, n, int(gro_type));
        for (size_t slot = 0; slot < mm.sub_material_keys.size(); ++slot) {
            const uint32_t sk = mm.sub_material_keys[slot];
            if (!sk || sk == 0xFFFFFFFFu) continue;
            const uint32_t tk = submaterial_texture_key(sk);
            QString title = QStringLiteral("slot %1: sub-mat %2")
                                .arg(slot)
                                .arg(hex_key(sk));
            if (tk)
                title += QStringLiteral("  →  tex ") + hex_key(tk);
            rows.push_back({title, tk, resolve_texture_image(tk)});
        }
        return rows;
    }

    // Leaf material — every texture layer it references.
    for (const jade::TexLayer& tl : jade::resolve_texture_keys(d, n)) {
        const QString title =
            tl.layer_index == 0
                ? QStringLiteral("base layer: ") + hex_key(tl.key)
                : QStringLiteral("layer %1: %2")
                      .arg(tl.layer_index)
                      .arg(hex_key(tl.key));
        rows.push_back({title, tl.key, resolve_texture_image(tl.key)});
    }
    return rows;
}

// Texture key the sub-material `sub_key` points at (base layer), looked
// up across bins when an archive handle is present. 0 when unresolved
// (_submaterial_texture_key).
uint32_t PreviewPanel::submaterial_texture_key(uint32_t sub_key) {
    const SubEntry* s = nullptr;
    if (bf_)
        s = crossbin_resolver()->lookup(sub_key).first;
    else
        s = find_sub_by_key(current_subs_, sub_key);
    if (!s) return 0;
    return jade::resolve_texture_key(s->data.data(), s->data.size());
}

// Image for a texture key — from this entry's decoded textures first,
// else decoded from whichever (possibly sibling) bin holds it. Null when
// the key is empty / not found / undecodable (_resolve_texture_image).
QImage PreviewPanel::resolve_texture_image(uint32_t tex_key) {
    if (!tex_key || tex_key == 0xFFFFFFFFu) return QImage();
    for (const EntryTexture& t : entry_textures_)
        if (t.key == tex_key) return t.image;
    // With an archive handle: the cross-bin decoder (which also covers
    // the local entry) — cached per key.
    if (bf_) return decode_texture_crossbin(tex_key);
    // No archive handle → in-entry only; decode straight from the subs.
    for (const SubEntry& s : current_subs_) {
        if (s.key != tex_key) continue;
        const uint8_t* d = s.data.data();
        const size_t n = s.data.size();
        if (!jade::is_texture_entry(d, n)) continue;
        const jade::TexInfo ti = jade::parse_texture(d, n);
        const size_t pixlen = n > ti.pix_start ? n - ti.pix_start : 0;
        if (ti.valid && !jade::is_placeholder(ti, pixlen)) {
            const std::vector<uint8_t>* pal =
                jade::palette_for_texture(ti, current_subs_);
            const std::vector<uint8_t> rgba = jade::decode_texture(
                d, n, ti, pal ? pal->data() : nullptr,
                pal ? pal->size() : 0);
            if (!rgba.empty())
                return QImage(rgba.data(), int(ti.width), int(ti.height),
                              int(ti.width) * 4, QImage::Format_RGBA8888)
                    .copy();
        }
    }
    return QImage();
}

// Format a material float field. Shipped f_SpecularExp / f_Opacity often
// carry sentinel bit patterns (e.g. opacity 0xFFFFFFFF → nan, or a
// denormal near zero), so show a clean decimal only when the value is
// finite and in a sane range; otherwise fall back to the raw hex so the
// user sees the truth, not "nan".
QString PreviewPanel::fmt_mat_float(uint32_t raw, float fval) {
    if (std::isfinite(fval)
        && (fval == 0.0f
            || (std::abs(fval) >= 1e-4f && std::abs(fval) <= 1e7f)))
        return QStringLiteral("%1").arg(double(fval), 0, 'f', 3);
    return hex_key(raw) + QStringLiteral(" (raw)");
}

// Convert an RGBA image to a QPixmap, optionally fit to a box
// (_pil_to_pixmap).
QPixmap PreviewPanel::image_to_pixmap(const QImage& img, int max_size) {
    QPixmap pm = QPixmap::fromImage(img);
    if (max_size)
        pm = pm.scaled(max_size, max_size, Qt::KeepAspectRatio,
                       Qt::SmoothTransformation);
    return pm;
}

// One material-texture row: thumbnail + key/dimension caption.
QWidget* PreviewPanel::make_texture_row(const QString& title,
                                        uint32_t tex_key,
                                        const QImage& img) {
    auto* row = new QFrame();
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(2, 2, 2, 2);
    rl->setSpacing(8);

    auto* thumb = new QLabel();
    thumb->setFixedSize(72, 72);
    thumb->setAlignment(Qt::AlignCenter);
    thumb->setStyleSheet(
        QStringLiteral("border:1px solid %1; background:%2;")
            .arg(theme::BORDER)
            .arg(theme::PANEL_BG));
    QString caption;
    if (!img.isNull()) {
        thumb->setPixmap(image_to_pixmap(img, 70));
        caption = QStringLiteral("%1\n%2 × %3")
                      .arg(title)
                      .arg(img.width())
                      .arg(img.height());
    } else {
        thumb->setText(!tex_key ? QStringLiteral("—")
                                : QStringLiteral("not\nfound"));
        thumb->setStyleSheet(
            QStringLiteral(
                "border:1px solid %1; background:%2; color:%3;")
                .arg(theme::BORDER)
                .arg(theme::PANEL_BG)
                .arg(theme::DIM_TEXT));
        caption = title
                  + (tex_key ? QStringLiteral("\n(texture not found)")
                             : QString());
    }
    rl->addWidget(thumb, 0);

    auto* cap = new QLabel(caption);
    cap->setWordWrap(true);
    cap->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cap->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    rl->addWidget(cap, 1);
    return row;
}

// Remove and delete every widget currently in `lay`.
void PreviewPanel::clear_layout(QBoxLayout* lay) {
    while (lay->count()) {
        QLayoutItem* item = lay->takeAt(0);
        if (QWidget* w = item->widget()) {
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
}

// Decoded view for an AI-script resource.
//
// PORT GAP: core/ai_script.py (classify_ai_script / parse_node_stream) and
// core/ai_enums.py have no C++ port — the Python rendered model-list
// reference tables and decoded instruction listings here. Natively we show
// the labelled note plus the short hex peek the Python's bytecode
// fallback used.
void PreviewPanel::show_ai_script(const SubEntry& sub) {
    const std::vector<uint8_t>& payload = sub.data;
    const uint32_t gro_type = sub.gro_null ? 0 : sub.gro_type;
    QStringList lines;
    lines << QStringLiteral("Format: AI script  ·  %1 bytes")
                 .arg(fmt_thousands(payload.size()));
    lines << QStringLiteral("GRO type %1").arg(hex_key(gro_type));
    lines << QString();
    lines << tr("not ported yet: AI-script decoding (core/ai_script.py) — "
                "model lists and instruction streams show as raw bytes.");
    lines << QString();
    lines << QStringLiteral("Raw header bytes:");
    const size_t peek = std::min(payload.size(), size_t(256));
    for (const QString& l : hex_dump_lines(payload, peek, 4))
        lines << QStringLiteral("  ") + l;
    if (payload.size() > peek)
        lines << QStringLiteral("  … (+%1 more bytes)")
                     .arg(fmt_thousands(payload.size() - peek));
    show_detail_text(QStringLiteral("<b>AI Script</b> %1  <i>bytecode</i>")
                         .arg(hex_key(sub.key)),
                     lines.join(QLatin1Char('\n')));
}

// Decode a resource-index *dependency list* — a packed array of
// [4-byte ASCII ext][4-byte key] entries naming the resources this record
// pulls in. This is engine streaming metadata, not an editable asset —
// shown read-only.
void PreviewPanel::show_reference(const SubEntry& sub) {
    const std::vector<jade::RefEntry> pairs =
        jade::decode_ref_list(sub.data.data(), sub.data.size());

    QStringList lines;
    lines << QStringLiteral("Resource dependency list — %1 reference(s).")
                 .arg(pairs.size());
    lines << QStringLiteral("(engine streaming index; read-only)");
    lines << QString();
    lines << QStringLiteral("%1 %2 target")
                 .arg(QStringLiteral("ext"), -6)
                 .arg(QStringLiteral("key"), -12);
    for (const jade::RefEntry& re : pairs) {
        QString target;
        if (asset_index_ != nullptr) {
            const jade::AssetRecord* rec =
                asset_index_->find_first_by_key(re.key);
            if (rec != nullptr) target = QString::fromStdString(rec->name);
        }
        lines << QStringLiteral("%1 %2  %3")
                     .arg(QString::fromStdString(re.ext), -6)
                     .arg(hex_key(re.key), -12)
                     .arg(target);
    }
    if (sub.data.size() % 8)
        lines << QStringLiteral("\n(+%1 trailing bytes)")
                     .arg(sub.data.size() % 8);
    show_detail_text(QStringLiteral("<b>Resource index</b> ")
                         + hex_key(sub.key),
                     lines.join(QLatin1Char('\n')));
}

// Hex dump for an unrecognized sub-entry.
//
// When the record's body 'type' word is 0 but it's *declared* under an
// extension in this entry's dependency lists (AI .ofc / .ova, texture
// .txi / .txs, …), name it by that real type instead of the anonymous
// "Resource".
void PreviewPanel::show_sub_hex(const SubEntry& sub) {
    const std::vector<uint8_t>& payload = sub.data;
    const uint32_t key = sub.key;
    const uint32_t gro_type = sub.gro_null ? 0 : sub.gro_type;

    QString label = QStringLiteral("Resource");
    auto decl_it = entry_ref_ext_.find(key);
    const bool has_decl = decl_it != entry_ref_ext_.end();
    if (has_decl) label = ext_type_label(decl_it->second);

    QStringList hdr_lines;
    hdr_lines << QStringLiteral("Key: %1").arg(hex_key(key));
    hdr_lines << (!sub.ext.empty()
                      ? QStringLiteral("Extension: %1")
                            .arg(QString::fromStdString(sub.ext))
                      : QStringLiteral("GRO type: %1").arg(hex_key(gro_type)));
    hdr_lines << QStringLiteral("Size: %1B")
                     .arg(fmt_thousands(payload.size()));
    if (has_decl)
        hdr_lines << QStringLiteral("Declared as: %1")
                         .arg(QString::fromStdString(decl_it->second));

    const size_t show_bytes = std::min(payload.size(), size_t(2048));
    QStringList hex_lines = hex_dump_lines(payload, show_bytes, 8);
    if (payload.size() > show_bytes)
        hex_lines << QStringLiteral("... (%1 more bytes)")
                         .arg(fmt_thousands(payload.size() - show_bytes));

    show_detail_text(QStringLiteral("<b>%1</b> %2")
                         .arg(label)
                         .arg(hex_key(key)),
                     hdr_lines.join(QLatin1Char('\n')) + QStringLiteral("\n\n")
                         + hex_lines.join(QLatin1Char('\n')));
}

// ── Detail page helpers ──

void PreviewPanel::show_detail_text(const QString& header_html,
                                    const QString& body_text) {
    info_header_->setText(header_html);
    info_text_->setPlainText(body_text);
    detail_stack_->setCurrentIndex(2);
}

// ── Entry-level helpers ──

// Find and decode all textures in the current entry's sub-entries.
//
// Jade stores every texture twice in the same entry: a 52-byte
// *placeholder* header with empty pixdata first, then the same key again
// with full pixel data later. Decoding the placeholder gives an all-zero
// RGBA image — that's the root cause of the "Prince mesh renders
// pitch-black" bug. We filter placeholders here so downstream code only
// ever sees textures that actually carry image data.
std::vector<PreviewPanel::EntryTexture> PreviewPanel::find_entry_textures() {
    std::vector<EntryTexture> textures;
    for (const SubEntry& s : current_subs_) {
        const uint8_t* d = s.data.data();
        const size_t n = s.data.size();
        if (!n || !jade::is_texture_entry(d, n)) continue;
        const jade::TexInfo ti = jade::parse_texture(d, n);
        if (!ti.valid) continue;
        // Skip placeholder stubs — pixdata too small for the base mip
        // level (decode to all-zero RGBA → black-mesh bug).
        const size_t pixlen = n > ti.pix_start ? n - ti.pix_start : 0;
        if (jade::is_placeholder(ti, pixlen)) continue;
        const std::vector<uint8_t>* pal =
            jade::palette_for_texture(ti, current_subs_);
        const std::vector<uint8_t> rgba = jade::decode_texture(
            d, n, ti, pal ? pal->data() : nullptr, pal ? pal->size() : 0);
        if (rgba.empty()) continue;
        EntryTexture t;
        t.key = s.key;
        t.info = ti;
        t.image = QImage(rgba.data(), int(ti.width), int(ti.height),
                         int(ti.width) * 4, QImage::Format_RGBA8888)
                      .copy();
        textures.push_back(std::move(t));
    }
    return textures;
}

// Pick the best-matching texture in the current entry for `geo`.
//
// Strategy:
//  1. Walk material sub-entries; follow their texture_keys to a decoded
//     texture in the entry. First match wins.
//  2. Fallback: return the *largest* real texture by pixel area — the
//     diffuse map is almost always the right call when no direct
//     material→texture link exists.
QImage PreviewPanel::texture_for_geometry(const jade::GeoInfo& /*geo*/) {
    if (entry_textures_.empty()) return QImage();
    // Dict-comprehension semantics: the LAST record with a key wins (Jade
    // ships some textures twice; the later record is the real one).
    std::map<uint32_t, const QImage*> by_key;
    for (const EntryTexture& t : entry_textures_) by_key[t.key] = &t.image;

    for (const SubEntry& s : current_subs_) {
        const uint32_t gt = s.gro_null ? 0 : s.gro_type;
        const bool is_mat = gt == 3 || gt == 4 || gt == 5
                            || (gt && (gt & 0xFF) == 0x07);
        if (!is_mat) continue;
        const jade::MatInfo mat =
            jade::parse_material(s.data.data(), s.data.size(), int(gt));
        if (!mat.ok) continue;
        for (uint32_t tk : mat.texture_keys) {
            if (!tk || tk == 0xFFFFFFFFu) continue;
            auto it = by_key.find(tk);
            if (it != by_key.end()) return *it->second;
        }
    }

    // Fallback: largest texture by pixel area.
    const EntryTexture* largest = &entry_textures_.front();
    for (const EntryTexture& t : entry_textures_)
        if (t.info.width * t.info.height
            > largest->info.width * largest->info.height)
            largest = &t;
    return largest->image;
}

// Per-element resolved sub-material key (0 = unresolved), reproducing the
// engine's Shape/MAT merge — the shared spine of
// mesh_swap.resolve_element_texture_keys_xbin and
// resolve_element_render_modes_xbin. Same chain as the in-bin
// jade::resolve_element_texture_keys, but the geo's own GAO-linked
// container can be a Shape-bin *stub* that the engine replaces with a
// MAT-bin material at load (proven via x64dbg), so the resolver's
// find_material_override supplies the real container's positional
// sub_material_keys when that applies.
std::vector<uint32_t> PreviewPanel::element_sub_keys(const jade::GeoInfo& geo,
                                                     uint32_t geo_key) {
    const size_t n_elems = geo.elements.size() / 2;
    if (!n_elems) return {};

    qint64 max_mat = 0;
    for (size_t ei = 0; ei < n_elems; ++ei)
        max_mat = std::max(max_mat, qint64(geo.elements[ei * 2 + 1]));

    uint32_t grm_key = 0;
    jade::owning_grm_key(current_subs_, geo_key, grm_key);
    std::vector<uint32_t> sub_keys;
    if (grm_key)
        sub_keys = jade::grm_sub_material_keys(
            find_sub_by_key(current_subs_, grm_key));

    // The MAT-bin override (wow merge) — a no-op unless the stub is a
    // degenerate placeholder (or can't index every matId) and a richer
    // counterpart exists, so correctly-materialed meshes are untouched.
    if (bf_) {
        quint32 cont_key = 0;
        long long pidx = -1;
        std::vector<quint32> alt;
        if (crossbin_resolver()->find_material_override(
                grm_key, max_mat, geo_key, cont_key, pidx, alt)
            && !alt.empty())
            sub_keys.assign(alt.begin(), alt.end());
    }

    std::vector<uint32_t> out(n_elems, 0);
    if (sub_keys.empty()) return out;
    for (size_t ei = 0; ei < n_elems; ++ei) {
        const uint32_t mat_id = geo.elements[ei * 2 + 1];
        const int64_t slot =
            jade::clamp_matid(int64_t(mat_id), int64_t(sub_keys.size()));
        out[ei] = sub_keys[size_t(slot)];
    }
    return out;
}

// Resolve a per-element texture list for `geo`, aligned with its element
// list (null image = unresolved).
//
// Resolution is the exact element→matId→sub-material→texture chain the
// Mesh Swap material table uses (including the Shape/MAT merge override),
// so the preview and that table never disagree about which texture covers
// which element. In-bin keys come from the already-decoded entry
// textures; foreign keys are decoded from their own bin and cached
// (_element_textures + resolve_element_texture_keys_xbin).
std::vector<QImage> PreviewPanel::element_textures(const jade::GeoInfo& geo,
                                                   uint32_t geo_key) {
    const std::vector<uint32_t> esubs = element_sub_keys(geo, geo_key);
    if (esubs.empty()) return {};

    // Per unique sub-material key → its base-layer texture key.
    std::map<uint32_t, uint32_t> texk_by_sub;
    for (uint32_t sk : esubs) {
        if (!sk || sk == 0xFFFFFFFFu || texk_by_sub.count(sk)) continue;
        const SubEntry* s = nullptr;
        if (bf_)
            s = crossbin_resolver()->lookup(sk).first;
        else
            s = find_sub_by_key(current_subs_, sk);
        texk_by_sub[sk] =
            s ? jade::resolve_texture_key(s->data.data(), s->data.size())
              : 0;
    }

    // LAST record with a key wins (dict-comprehension semantics).
    std::map<uint32_t, const QImage*> by_img;
    for (const EntryTexture& t : entry_textures_) by_img[t.key] = &t.image;
    std::vector<QImage> out;
    out.reserve(esubs.size());
    for (uint32_t sk : esubs) {
        uint32_t tk = 0;
        if (sk && sk != 0xFFFFFFFFu) {
            auto it = texk_by_sub.find(sk);
            if (it != texk_by_sub.end()) tk = it->second;
        }
        if (!tk) {
            out.push_back(QImage());
            continue;
        }
        auto it = by_img.find(tk);
        if (it != by_img.end())
            out.push_back(*it->second);
        else
            out.push_back(bf_ ? decode_texture_crossbin(tk)
                              : resolve_texture_image(tk));
    }
    return out;
}

// ── Export ──

// Public export entry-point — drives the same exporter functions the
// panel's own paths use, but without depending on the panel's UI state.
// Used by AssetBrowserTab's right-click "Export…".
bool PreviewPanel::export_sub(const SubEntry& sub,
                              const std::vector<SubEntry>& all_subs,
                              const QString& cat_key, const QString& path,
                              const QString& fmt_ext, QString* err) {
    std::vector<SubEntry> single;
    const std::vector<SubEntry>* ctx = &all_subs;
    if (all_subs.empty()) {
        single.push_back(sub);
        ctx = &single;
    }
    try {
        export_sub_entry(sub, *ctx, cat_key, path, fmt_ext);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = QString::fromUtf8(e.what());
        return false;
    }
}

// Return [(label, ext), …] for a category key, for file-dialog filters.
QList<QPair<QString, QString>> PreviewPanel::formats_for(
    const QString& cat_key) {
    return export_formats(cat_key);
}

// Re-encode a GLB's animation back to TRL bytes for the selected sub.
// PORT GAP: io_ops/scene_export.glb_to_trl_payload (and the skeleton-donor
// resolution it needs) has no C++ port — the button is permanently
// disabled; this slot only reports the gap if ever reached.
void PreviewPanel::on_import_animation_glb() {
    QMessageBox::information(
        this, tr("Import Anim from GLB"),
        tr("not ported yet: GLB→TRL re-encoding (io_ops/scene_export)"));
}

namespace {

void write_bytes_or_throw(const QString& path,
                          const uint8_t* data, size_t n) {
    std::ofstream f(std::filesystem::path(path.toStdWString()),
                    std::ios::binary);
    if (!f)
        throw std::runtime_error("could not open output file: "
                                 + path.toStdString());
    f.write(reinterpret_cast<const char*>(data), std::streamsize(n));
    if (!f) throw std::runtime_error("write failed: " + path.toStdString());
}

void write_text_or_throw(const QString& path, const QByteArray& text) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw std::runtime_error("could not open output file: "
                                 + path.toStdString());
    if (f.write(text) != text.size())
        throw std::runtime_error("write failed: " + path.toStdString());
}

}  // namespace

// Export a single sub-entry to a file.
void PreviewPanel::export_sub_entry(const SubEntry& sub,
                                    const std::vector<SubEntry>& ctx_subs,
                                    const QString& cat_key,
                                    const QString& path,
                                    const QString& fmt_ext) {
    if (cat_key == QLatin1String("texture"))
        export_texture(sub, ctx_subs, path, fmt_ext);
    else if (cat_key == QLatin1String("geometry"))
        export_geometry(sub, path, fmt_ext);
    else if (cat_key == QLatin1String("animation"))
        export_animation(sub, path, fmt_ext);
    else if (cat_key == QLatin1String("gao")
             || cat_key == QLatin1String("material"))
        export_structured(sub, cat_key, path, fmt_ext);
    else
        write_bytes_or_throw(path, sub.data.data(), sub.data.size());
}

// Export texture as PNG, DDS, TGA, or raw.
void PreviewPanel::export_texture(const SubEntry& sub,
                                  const std::vector<SubEntry>& ctx_subs,
                                  const QString& path,
                                  const QString& fmt_ext) {
    const uint8_t* d = sub.data.data();
    const size_t n = sub.data.size();
    const jade::TexInfo ti = jade::parse_texture(d, n);
    if (!ti.valid) throw std::runtime_error("Could not parse texture");

    const std::vector<uint8_t>* pal =
        jade::palette_for_texture(ti, ctx_subs);

    if (fmt_ext == QLatin1String(".dds")) {
        // Lossless raw DDS export.
        const std::vector<uint8_t> dds = jade::write_dds_raw(
            d, n, ti, pal ? pal->data() : nullptr, pal ? pal->size() : 0);
        if (dds.empty())
            throw std::runtime_error("Could not export DDS");
        write_bytes_or_throw(path, dds.data(), dds.size());
        return;
    }

    const std::vector<uint8_t> rgba = jade::decode_texture(
        d, n, ti, pal ? pal->data() : nullptr, pal ? pal->size() : 0);
    if (rgba.empty())
        throw std::runtime_error("Could not decode texture pixels");

    const QImage img =
        QImage(rgba.data(), int(ti.width), int(ti.height),
               int(ti.width) * 4, QImage::Format_RGBA8888)
            .copy();

    if (fmt_ext == QLatin1String(".png")) {
        if (!img.save(path, "PNG"))
            throw std::runtime_error("PNG save failed");
    } else if (fmt_ext == QLatin1String(".tga")) {
        if (!img.save(path, "TGA"))
            throw std::runtime_error("TGA save failed");
    } else if (fmt_ext == QLatin1String(".raw")) {
        write_bytes_or_throw(path, rgba.data(), rgba.size());
    } else {
        if (!img.save(path)) throw std::runtime_error("image save failed");
    }
}

// Export geometry as GLB, OBJ, or raw binary.
void PreviewPanel::export_geometry(const SubEntry& sub, const QString& path,
                                   const QString& fmt_ext) {
    const jade::GeoInfo geo =
        jade::parse_geometry(sub.data.data(), sub.data.size());
    if (!geo.ok) throw std::runtime_error("Could not parse geometry");

    if (fmt_ext == QLatin1String(".obj"))
        write_obj(geo, path, sub.key);
    else if (fmt_ext == QLatin1String(".glb"))
        write_geo_glb(geo, path, sub.key);
    else
        write_bytes_or_throw(path, sub.data.data(), sub.data.size());
}

// Write geometry as Wavefront OBJ.
void PreviewPanel::write_obj(const jade::GeoInfo& geo, const QString& path,
                             uint32_t key) {
    std::ofstream f(std::filesystem::path(path.toStdWString()));
    if (!f)
        throw std::runtime_error("could not open output file: "
                                 + path.toStdString());
    char buf[128];
    f << "# Jade Engine geometry export\n";
    std::snprintf(buf, sizeof buf, "# Key: 0x%08X\n", key);
    f << buf;
    f << "# Vertices: " << geo.nb_points << "\n\n";

    for (size_t i = 0; i < size_t(geo.nb_points); ++i) {
        std::snprintf(buf, sizeof buf, "v %.6f %.6f %.6f\n",
                      double(geo.vertices[i * 3]),
                      double(geo.vertices[i * 3 + 1]),
                      double(geo.vertices[i * 3 + 2]));
        f << buf;
    }
    const size_t n_norms = geo.normals.size() / 3;
    for (size_t i = 0; i < n_norms; ++i) {
        std::snprintf(buf, sizeof buf, "vn %.6f %.6f %.6f\n",
                      double(geo.normals[i * 3]),
                      double(geo.normals[i * 3 + 1]),
                      double(geo.normals[i * 3 + 2]));
        f << buf;
    }
    const size_t n_uvs = geo.uvs.size() / 2;
    for (size_t i = 0; i < n_uvs; ++i) {
        std::snprintf(buf, sizeof buf, "vt %.6f %.6f\n",
                      double(geo.uvs[i * 2]),
                      1.0 - double(geo.uvs[i * 2 + 1]));
        f << buf;
    }

    const size_t n_tris = geo.faces.size() / 7;
    const bool has_uvs = n_uvs > 0;
    const bool has_normals = n_norms > 0;
    for (size_t fi = 0; fi < n_tris; ++fi) {
        const unsigned i0 = geo.faces[fi * 7] + 1;
        const unsigned i1 = geo.faces[fi * 7 + 1] + 1;
        const unsigned i2 = geo.faces[fi * 7 + 2] + 1;
        const unsigned uv0 = geo.faces[fi * 7 + 3] + 1;
        const unsigned uv1 = geo.faces[fi * 7 + 4] + 1;
        const unsigned uv2 = geo.faces[fi * 7 + 5] + 1;
        if (has_uvs && has_normals)
            std::snprintf(buf, sizeof buf,
                          "f %u/%u/%u %u/%u/%u %u/%u/%u\n", i0, uv0, i0, i1,
                          uv1, i1, i2, uv2, i2);
        else if (has_uvs)
            std::snprintf(buf, sizeof buf, "f %u/%u %u/%u %u/%u\n", i0, uv0,
                          i1, uv1, i2, uv2);
        else if (has_normals)
            std::snprintf(buf, sizeof buf, "f %u//%u %u//%u %u//%u\n", i0,
                          i0, i1, i1, i2, i2);
        else
            std::snprintf(buf, sizeof buf, "f %u %u %u\n", i0, i1, i2);
        f << buf;
    }
    if (!f) throw std::runtime_error("write failed: " + path.toStdString());
}

// Write geometry as a simple glTF binary (.glb).
void PreviewPanel::write_geo_glb(const jade::GeoInfo& geo,
                                 const QString& path, uint32_t key) {
    char name[32];
    std::snprintf(name, sizeof name, "geo_0x%08X", key);
    char key_hex[16];
    std::snprintf(key_hex, sizeof key_hex, "0x%08X", key);
    const std::vector<uint8_t> glb = jade::gltfbuild::build_geo_model_glb(
        geo, name, key_hex, name);
    if (glb.empty())
        throw std::runtime_error("GLB build produced no data");
    write_bytes_or_throw(path, glb.data(), glb.size());
}

// Export animation as GLB, TRL, or JSON.
void PreviewPanel::export_animation(const SubEntry& sub, const QString& path,
                                    const QString& fmt_ext) {
    const uint32_t gro_type = sub.gro_null ? 0 : sub.gro_type;
    const uint32_t key = sub.key;

    if (fmt_ext == QLatin1String(".trl")) {
        write_bytes_or_throw(path, sub.data.data(), sub.data.size());
    } else if (fmt_ext == QLatin1String(".json")) {
        // PORT GAP: parse_trl (core/animation.py) is unported — 'parsed'
        // is null where the Python emitted the decoded track list.
        QJsonObject data;
        data.insert(QStringLiteral("key"), hex_key(key));
        data.insert(QStringLiteral("gro_type"), double(gro_type));
        data.insert(QStringLiteral("parsed"), QJsonValue::Null);
        data.insert(QStringLiteral("payload_size"),
                    double(sub.data.size()));
        write_text_or_throw(
            path, QJsonDocument(data).toJson(QJsonDocument::Indented));
    } else if (fmt_ext == QLatin1String(".glb")) {
        // PORT GAP: the Python built a merged animated scene through
        // io_ops/scene_export.build_scene + core/gltf_builder.build_glb.
        throw std::runtime_error(
            "not ported yet: animated-scene GLB export "
            "(io_ops/scene_export)");
    } else {
        write_bytes_or_throw(path, sub.data.data(), sub.data.size());
    }
}

// Export GAO or material as JSON or raw binary.
void PreviewPanel::export_structured(const SubEntry& sub,
                                     const QString& cat_key,
                                     const QString& path,
                                     const QString& fmt_ext) {
    if (fmt_ext != QLatin1String(".json")) {
        write_bytes_or_throw(path, sub.data.data(), sub.data.size());
        return;
    }

    const uint32_t gro_type = sub.gro_null ? 0 : sub.gro_type;
    QJsonValue parsed = QJsonValue::Null;
    if (cat_key == QLatin1String("gao")) {
        const jade::GaoInfo g =
            jade::parse_gao_full(sub.data.data(), sub.data.size());
        if (g.ok) {
            QJsonObject o;
            o.insert(QStringLiteral("name"),
                     QString::fromStdString(g.name));
            o.insert(QStringLiteral("version"), double(g.version));
            o.insert(QStringLiteral("identity"), hex_key(g.identity));
            QJsonArray flags;
            for (const auto& [bit, fname] : gao_flag_names())
                if (g.identity & bit)
                    flags.append(QString::fromLatin1(fname));
            o.insert(QStringLiteral("flags"), flags);
            if (g.gmat_present) {
                const std::array<float, 3> t = matrix_t(g.gmat_raw);
                QJsonArray pos;
                for (float v : t) pos.append(double(v));
                o.insert(QStringLiteral("position"), pos);
            }
            if (g.vis_flag && g.vis_read) {
                QJsonObject vis;
                vis.insert(QStringLiteral("gro_key"), hex_key(g.gro_key));
                vis.insert(QStringLiteral("grm_key"), hex_key(g.grm_key));
                o.insert(QStringLiteral("visual"), vis);
            }
            if (g.hier_flag && g.hier_read)
                o.insert(QStringLiteral("father_key"),
                         hex_key(g.father_key));
            QJsonArray gizmos;
            for (size_t gi = 0; gi + 1 < g.gizmo_flat.size(); gi += 2) {
                QJsonObject gz;
                gz.insert(QStringLiteral("gao_key"),
                          hex_key(g.gizmo_flat[gi]));
                gz.insert(QStringLiteral("mat_id"),
                          double(g.gizmo_flat[gi + 1]));
                gizmos.append(gz);
            }
            o.insert(QStringLiteral("gizmo_ptrs"), gizmos);
            parsed = o;
        }
    } else {
        const jade::MatInfo m = jade::parse_material(
            sub.data.data(), sub.data.size(), int(gro_type));
        if (m.ok) {
            QJsonObject o;
            o.insert(QStringLiteral("type"),
                     m.type == 0   ? QStringLiteral("single")
                     : m.type == 1 ? QStringLiteral("multitexture")
                                   : QStringLiteral("multi"));
            if (m.type == 2) {
                o.insert(QStringLiteral("n_sub"), double(m.n_sub));
                QJsonArray keys;
                for (uint32_t k : m.sub_material_keys)
                    keys.append(hex_key(k));
                o.insert(QStringLiteral("sub_material_keys"), keys);
            } else {
                o.insert(QStringLiteral("ambient"), hex_key(m.ambient));
                o.insert(QStringLiteral("diffuse"), hex_key(m.diffuse));
                o.insert(QStringLiteral("specular"), hex_key(m.specular));
                o.insert(QStringLiteral("flags"), hex_key(m.flags));
                QJsonArray keys;
                for (uint32_t k : m.texture_keys) keys.append(hex_key(k));
                o.insert(QStringLiteral("texture_keys"), keys);
            }
            parsed = o;
        }
    }

    QJsonObject data;
    data.insert(QStringLiteral("key"), hex_key(sub.key));
    data.insert(QStringLiteral("type"), cat_key);
    data.insert(QStringLiteral("parsed"), parsed);
    data.insert(QStringLiteral("payload_size"), double(sub.data.size()));
    write_text_or_throw(path,
                        QJsonDocument(data).toJson(QJsonDocument::Indented));
}
