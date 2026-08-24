// PreviewPanel.hpp — multi-content preview for selected BF entries (port of
// gui/preview_panel.py).
//
// Shows a content summary header listing all sub-entry types, with a
// clickable sub-entry tree for entries that contain multiple resources
// (e.g. costumes with GAOs + geometry + materials + textures + animations).
// Individual previews: texture images, geometry 3D view, GAO info,
// animation details. Includes export helpers (PNG, DDS, GLB, OBJ, JSON,
// raw binary).
#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QMap>
#include <QPair>
#include <QPixmap>
#include <QScrollArea>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "jade/AssetIndex.hpp"
#include "jade/Geometry.hpp"
#include "jade/Light.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

namespace jade { class BigFile; }

class MeshPreviewPanel;
class MeshSwapResolver;
class QBoxLayout;
class QFrame;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

// Map AssetBrowser category names → the keys used by the export-format
// table (the Python module-level EXPORT_CATEGORY_MAP, imported by
// asset_browser_tab). Used by the right-click "Export…" entrypoint.
inline const QMap<QString, QString> EXPORT_CATEGORY_MAP = {
    {QStringLiteral("Textures"), QStringLiteral("texture")},
    {QStringLiteral("Geometry"), QStringLiteral("geometry")},
    {QStringLiteral("Materials"), QStringLiteral("material")},
    {QStringLiteral("Game Objects"), QStringLiteral("gao")},
    {QStringLiteral("Animations"), QStringLiteral("animation")},
};

// The `result` dict handed to show_entry (subs / raw_size / dec_size /
// index). `index` is the BF entry index, -1 = the Python None.
struct PreviewEntryResult {
    std::vector<jade::SubEntry> subs;
    size_t raw_size = 0;
    size_t dec_size = 0;
    long long index = -1;
};

// Scroll area that emits a zoom request on Ctrl + mouse wheel
// (_ZoomScrollArea).
class ZoomScrollArea : public QScrollArea {
    Q_OBJECT
public:
    using QScrollArea::QScrollArea;

signals:
    void zoomRequested(int direction);

protected:
    void wheelEvent(QWheelEvent* event) override;
};

// Right-side panel: content summary + sub-entry list + individual previews.
class PreviewPanel : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPanel(QWidget* parent = nullptr);

    // Hand the panel the open archive's AssetIndex — enables cross-bin
    // material/texture resolution for the 3D preview (the Shape/MAT merge
    // + foreign-bin texture decode, via MeshSwapResolver). Called by the
    // Asset Browser when its background scan completes, and with nullptr
    // when a new archive starts loading.
    void set_asset_index(std::shared_ptr<jade::AssetIndex> idx);

    void show_error(const QString& msg);

    // Analyze all sub-entries and build the content summary + clickable
    // tree. `prefer_sub_index` (the Asset Browser uses this): pre-select
    // that sub-entry's leaf (-1 = default behaviour). `compact` -1 = auto
    // (True whenever prefer_sub_index is set), 0/1 explicit.
    void show_entry(const PreviewEntryResult& result,
                    std::shared_ptr<jade::BigFile> bf = nullptr,
                    const QString& bf_path = QString(),
                    long long prefer_sub_index = -1, int compact = -1);

    // Public export entry-point — drives the same exporter functions
    // without depending on the panel's UI state (used by AssetBrowserTab's
    // right-click "Export…"). Returns false + *err on failure (the Python
    // raised).
    bool export_sub(const jade::SubEntry& sub,
                    const std::vector<jade::SubEntry>& all_subs,
                    const QString& cat_key, const QString& path,
                    const QString& fmt_ext, QString* err = nullptr);

    // [(label, ext), …] for a category key, for file-dialog filters.
    static QList<QPair<QString, QString>> formats_for(const QString& cat_key);

private slots:
    void on_tree_item_changed(QTreeWidgetItem* current,
                              QTreeWidgetItem* previous);
    void on_import_animation_glb();
    void zoom_tex(int direction);

private:
    // Decoded in-entry texture cache row (_find_entry_textures results).
    struct EntryTexture {
        uint32_t key = 0;
        jade::TexInfo info;
        QImage image;
    };

    // (cat_key, label, detail) triple from the sub-entry classifier.
    struct Classified {
        QString cat;
        QString label;
        QString detail;
    };

    void on_sub_selected(long long row);
    QTreeWidgetItem* find_sub_leaf(long long sub_idx);

    // ── individual preview methods ──
    void show_texture(const jade::SubEntry& sub);
    void show_palette(const jade::SubEntry& sub);
    void show_light(const jade::SubEntry& sub);
    void show_light_on_tex_page(const jade::LightInfo& li,
                                const QStringList& header_lines,
                                const QString& footer = QString());
    void show_geometry(const jade::SubEntry& sub);
    void show_geometry_text(const jade::SubEntry& sub,
                            const jade::GeoInfo& geo);
    void show_gao(const jade::SubEntry& sub);
    void show_animation(const jade::SubEntry& sub);
    void show_material(const jade::SubEntry& sub);
    void show_ai_script(const jade::SubEntry& sub);
    void show_reference(const jade::SubEntry& sub);
    void show_sub_hex(const jade::SubEntry& sub);
    void show_detail_text(const QString& header_html,
                          const QString& body_text);

    // ── texture zoom ──
    void set_tex_zoom(double zoom);
    void apply_tex_zoom();

    // ── material page helpers ──
    struct MatTexRow {
        QString title;
        uint32_t tex_key = 0;   // 0 = None
        QImage img;
    };
    std::vector<MatTexRow> material_texture_rows(const jade::SubEntry& sub,
                                                 uint32_t gro_type,
                                                 bool is_container);
    uint32_t submaterial_texture_key(uint32_t sub_key);
    QImage resolve_texture_image(uint32_t tex_key);
    QWidget* make_texture_row(const QString& title, uint32_t tex_key,
                              const QImage& img);
    static void clear_layout(QBoxLayout* lay);
    static QString fmt_mat_float(uint32_t raw, float fval);

    // ── entry-level helpers ──
    std::vector<EntryTexture> find_entry_textures();
    QImage texture_for_geometry(const jade::GeoInfo& geo);
    std::vector<QImage> element_textures(const jade::GeoInfo& geo,
                                         uint32_t geo_key);
    // Per-element resolved sub-material key (0 = unresolved), via the
    // element→matId→container chain INCLUDING the Shape/MAT override —
    // the shared spine of element_textures and the render-mode pass
    // (mesh_swap.resolve_element_texture_keys_xbin /
    // resolve_element_render_modes_xbin).
    std::vector<uint32_t> element_sub_keys(const jade::GeoInfo& geo,
                                           uint32_t geo_key);
    // A MeshSwapResolver over the open archive, seeded with the current
    // entry; rebuilt when the selected entry (or index) changes
    // (_crossbin_resolver).
    MeshSwapResolver* crossbin_resolver();
    // Decode a texture by key from wherever it lives, cached
    // (_decode_texture_crossbin).
    QImage decode_texture_crossbin(uint32_t tex_key);

    static QPixmap image_to_pixmap(const QImage& img, int max_size = 0);

    // ── export helpers (ctx_subs = the entry the sub came from) ──
    void export_sub_entry(const jade::SubEntry& sub,
                          const std::vector<jade::SubEntry>& ctx_subs,
                          const QString& cat_key, const QString& path,
                          const QString& fmt_ext);
    void export_texture(const jade::SubEntry& sub,
                        const std::vector<jade::SubEntry>& ctx_subs,
                        const QString& path, const QString& fmt_ext);
    void export_geometry(const jade::SubEntry& sub, const QString& path,
                         const QString& fmt_ext);
    void export_animation(const jade::SubEntry& sub, const QString& path,
                          const QString& fmt_ext);
    void export_structured(const jade::SubEntry& sub, const QString& cat_key,
                           const QString& path, const QString& fmt_ext);
    static void write_obj(const jade::GeoInfo& geo, const QString& path,
                          uint32_t key);
    static void write_geo_glb(const jade::GeoInfo& geo, const QString& path,
                              uint32_t key);

    static constexpr double TEX_ZOOM_MIN = 0.1;
    static constexpr double TEX_ZOOM_MAX = 16.0;
    static constexpr double TEX_ZOOM_STEP = 1.25;

    // Cap on how many texture/sub-material rows we render — multi-material
    // containers (gro_type 4) can declare 100+ slots.
    static constexpr int MAT_MAX_TEX_ROWS = 48;

    // ── widgets ──
    QLabel* summary_ = nullptr;
    QPushButton* import_anim_btn_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTreeWidget* sub_tree_ = nullptr;
    QStackedWidget* detail_stack_ = nullptr;

    // Page 1: texture preview.
    QWidget* tex_widget_ = nullptr;
    QLabel* tex_info_ = nullptr;
    QLabel* tex_label_ = nullptr;
    ZoomScrollArea* tex_scroll_ = nullptr;
    QPushButton* tex_zoom_reset_btn_ = nullptr;
    QPixmap tex_src_pixmap_;      // null = the Python None
    double tex_zoom_ = 1.0;

    // Page 2: info / text.
    QWidget* info_widget_ = nullptr;
    QLabel* info_header_ = nullptr;
    QTextEdit* info_text_ = nullptr;

    // Page 3: 3D viewer.
    MeshPreviewPanel* viewer_panel_ = nullptr;

    // Page 4: material breakdown.
    QWidget* mat_widget_ = nullptr;
    QLabel* mat_header_ = nullptr;
    QScrollArea* mat_scroll_ = nullptr;
    QWidget* mat_body_ = nullptr;
    QVBoxLayout* mat_body_lay_ = nullptr;
    int MAT_PAGE_ = 4;

    // ── current entry state ──
    std::vector<jade::SubEntry> current_subs_;
    PreviewEntryResult current_result_;
    std::map<uint32_t, std::string> entry_ref_ext_;
    long long current_sub_idx_ = -1;
    QString current_cat_ = QStringLiteral("unknown");
    std::vector<Classified> classified_;
    std::vector<EntryTexture> entry_textures_;
    // PORT GAP: parsed animations cache — core/animation.py (parse_trl)
    // has no C++ port, so this list stays empty and the animation preview
    // / playback combo never populates.
    // std::vector<...> entry_animations_;
    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    // PORT GAP: the _wow_ skeleton-donor machinery
    // (io_ops/scene_export.find_skeleton_donor / build_scene) is unported;
    // only the searched flag is kept for flow parity.
    bool donor_searched_ = false;
    // Cross-bin material resolution (MeshSwapResolver — the CrossBinResolver
    // port shared with MeshSwapTab). The resolver is rebuilt when the
    // selected entry changes; the foreign-texture cache is panel-lived like
    // the Python's _xbin_tex_cache (cleared when the archive changes).
    std::shared_ptr<jade::AssetIndex> asset_index_;
    std::shared_ptr<MeshSwapResolver> resolver_;
    long long resolver_index_ = -2;   // entry index the resolver was built for
    std::map<uint32_t, QImage> xbin_tex_cache_;
};
