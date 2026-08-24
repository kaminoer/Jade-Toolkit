// MeshSwapTab.hpp — Mesh Swap tab (port of gui/mesh_swap_tab.py): replace a
// single .geo sub-entry from a GLB file.
//
// The workflow:
//   1. The active BigFile from the main window is shown.
//   2. The user picks a BF entry index (typed in, or pulled from the View
//      tab via context) and clicks "List meshes" to populate the geo combo.
//   3. The user picks a target .geo key, picks a source GLB, and applies.
//   4. The replacement runs on a worker thread, makes a .bak the first
//      time, recompresses, and writes the BF back in place.
#pragma once

#include <QPixmap>
#include <QThread>
#include <QWidget>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "jade/AssetIndex.hpp"
#include "jade/Geometry.hpp"
#include "jade/Material.hpp"
#include "jade/SubEntry.hpp"

namespace jade { class BigFile; }
class MeshSwapResolver;   // cross-bin material/texture resolver (MeshSwapTab.cpp)
class ProjectDoc;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;

// _ExportGlbWorker — run single-mesh GLB export on a worker thread.
class MeshSwapExportGlbWorker : public QObject {
    Q_OBJECT
public:
    MeshSwapExportGlbWorker(const QString& bf_path, quint32 entry_idx,
                            quint32 geo_key, const QString& out_path)
        : bf_path_(bf_path), entry_idx_(entry_idx), geo_key_(geo_key),
          out_path_(out_path) {}

public slots:
    void run();

signals:
    void log(const QString& msg);
    void finished(bool ok, const QString& message);

private:
    QString bf_path_;
    quint32 entry_idx_;
    quint32 geo_key_;
    QString out_path_;
};

// _ExportGlbHierWorker — run single-mesh GLB export WITH bone hierarchy on a
// worker thread.
class MeshSwapExportGlbHierWorker : public QObject {
    Q_OBJECT
public:
    MeshSwapExportGlbHierWorker(const QString& bf_path, quint32 entry_idx,
                                quint32 geo_key, const QString& out_path)
        : bf_path_(bf_path), entry_idx_(entry_idx), geo_key_(geo_key),
          out_path_(out_path) {}

public slots:
    void run();

signals:
    void log(const QString& msg);
    void finished(bool ok, const QString& message);

private:
    QString bf_path_;
    quint32 entry_idx_;
    quint32 geo_key_;
    QString out_path_;
};

// NOTE (legacy workers): the Python module also carries _SwapWorker,
// _CrossCopyWorker, _CharacterSwapWorker and _StubWorker for the LEGACY
// direct-BF-write paths (cross-entry copy + bundled character pack + direct
// swap/stub). Their UI was removed upstream (gated behind `if False:`) and the
// handlers are unreachable dead code kept only as a conversion reference, so
// they are not ported — the ops-based paths below replace them.

// Pick an entry, list its .geo sub-entries, swap one from a GLB.
class MeshSwapTab : public QWidget {
    Q_OBJECT
public:
    explicit MeshSwapTab(QWidget* parent = nullptr);
    ~MeshSwapTab() override;

    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& path);
    // Receive the archive's AssetIndex (built once by the Asset Browser).
    // Enables cross-bin material/texture resolution so the table and the
    // replace/retarget ops target the bin a material actually lives in (the
    // Shape/MAT split — see resolve_mesh_materials).
    void set_asset_index(std::shared_ptr<jade::AssetIndex> idx);
    // Bind the tab to the current ModProject (or nullptr).
    void set_project(ProjectDoc* proj);
    // Optional hook so other tabs can preselect an entry.
    void set_entry(quint32 entry_idx);
    // Programmatic entry: open `parent_index`, list geometries, and
    // pre-select the row whose key matches `sub_key`. Used by the Asset
    // Browser's "Send to Mesh Swap" right-click.
    void receive_asset(quint32 parent_index, quint32 sub_key);

    // Jade GEOs index vertices with a 16-bit cooked index buffer, so a single
    // mesh maxes out at this many RENDER vertices (post UV/normal/material
    // seam splitting — higher than Blender's welded count). The build refuses
    // above it (patcher: nb_pts > 0xFFFF). See [[jade-ww-skinned-vert-ceiling]].
    static constexpr long long U16_VERT_LIMIT = 0xFFFF;

signals:
    // Emitted (entry_index, texture_key) when the user right-clicks a
    // texture in the materials table and chooses "Replace this texture".
    // (The Python signal used `object` for the key because PS2 keys set the
    // high bit and overflow signed-int marshalling — quint32 carries them.)
    void send_texture_to_tab(quint32 entry_idx, quint32 texture_key);

private slots:
    void on_list();
    void refresh_materials();
    void on_mat_cell_hover(int row, int col);
    void on_mat_context_menu(const QPoint& pos);
    void on_browse_glb();
    void on_apply();
    void on_stub();
    void on_export_glb();
    void on_export_glb_hier();
    void on_log(const QString& msg);
    void on_done(bool success, const QString& message);

private:
    // ── row/data shapes (the Python dicts) ─────────────────────────────

    // One list_geo_subs() summary row.
    struct GeoRow {
        quint32 key = 0;
        QString name;                     // '' when nothing references the geo
        quint32 nb_points = 0, nb_uvs = 0, nb_elements = 0, nb_tris = 0;
        bool    has_skin = false;
        quint32 bones = 0;
        qulonglong payload_bytes = 0;
        bool geo_colors = false;          // GEO body dul_PointColors present
        bool rli_colors = false;          // host GAO RLI lighting present
    };

    // One texture layer of a material (base + detail/effect chain).
    struct TexLayer {
        int       layer = 0;
        quint32   texture_key = 0;
        long long texture_entry_idx = -1;              // -1 == None
        long long texture_width = -1, texture_height = -1, texture_format = -1;
    };

    // One resolve_mesh_materials() element row.
    struct MatRow {
        int element = 0;
        int matId = 0;
        quint32 nTri = 0;
        long long material_key = -1;                   // -1 == None
        long long material_entry_idx = -1;
        long long texture_key = -1;                    // base layer (layer 0)
        long long texture_entry_idx = -1;
        long long texture_width = -1, texture_height = -1, texture_format = -1;
        std::vector<TexLayer> texture_layers;          // ALL layers
        jade::MatRenderFlags render_flags;             // .ok == false == None
        std::vector<quint32> shared_with_geos;         // other GEOs sharing it
    };

    // resolve_mesh_materials() result.
    struct MeshMatInfo {
        bool ok = false;
        QString error;                    // the Python exception text
        long long grm_key = -1;           // -1 == None
        long long grm_entry_idx = -1;     // bin holding the container used
        bool grm_is_crossbin = false;     // True if a sibling bin's material
        std::vector<MatRow> elements;
    };

    struct ListGeoResult {
        bool ok = false;
        QString error;
        std::vector<GeoRow> rows;
    };

    // ── ports of the io_ops/mesh_swap.py read helpers (see MeshSwapTab.cpp
    //    for the per-function provenance notes) ─────────────────────────
    ListGeoResult list_geo_subs(quint32 entry_idx);
    MeshMatInfo resolve_mesh_materials(quint32 entry_idx, quint32 geo_key);
    MeshSwapResolver* resolver_for(quint32 entry_idx,
                                   std::vector<jade::SubEntry> local_subs);

    long long current_entry_idx() const;              // -1 == None
    void populate_entry_combo();
    void update_enabled();
    long long glb_render_vert_count(const QString& path);  // -1 == None

    QPixmap texture_pixmap(quint32 texture_key, long long entry_idx = -1);
    static QString pixmap_tooltip_html(const QString& title, QPixmap pix,
                                       int max_side = 192);
    void show_cell_tooltip(int row, int col, const QString& html);
    void show_element_hover(int row, int col);
    const jade::GeoInfo* parsed_geo();
    QPixmap element_thumb(int elem_idx);

    // ── edit guardrails (cross-bin / clamp hazards) ────────────────────
    std::optional<std::set<quint32>> container_slot_keys();
    bool guard_crossbin_edit(const QString& action_desc);
    bool guard_append_clamp(quint32 new_material_key);

    void retarget_material(quint32 sub_material_key,
                           long long current_texture_key,
                           long long mat_entry_idx, int layer = 0);
    void new_texture_for_material(quint32 material_key, long long mat_entry_idx,
                                  int row, int layer = 0);
    void edit_material_flags(quint32 material_key, long long mat_entry_idx,
                             const jade::MatRenderFlags& rf);
    void set_element_material(int element, long long current_material_key);
    std::set<quint32> project_minted_keys() const;
    std::set<quint32> bin_keys(quint32 entry_idx) const;
    void detach_geo_material();

    const std::map<quint32, std::vector<quint32>>& geo_key_entry_index();
    std::vector<quint32> find_entries_with_geo_key(quint32 geo_key);

    // ── state (the Python instance attributes) ─────────────────────────
    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    std::shared_ptr<jade::AssetIndex> asset_index_;   // cross-bin resolution
    std::vector<std::pair<quint32, GeoRow>> geo_rows_;
    QThread* worker_thread_ = nullptr;   // only used by Export-to-GLB paths
    QObject* worker_ = nullptr;
    ProjectDoc* project_ = nullptr;
    // texture preview cache: (entry_idx, texture_key) -> QPixmap (null == None)
    std::map<std::pair<long long, quint32>, QPixmap> tex_preview_cache_;
    long long current_grm_key_ = -1;       // multi-material of the listed GEO
    std::vector<MatRow> current_mat_rows_; // resolve_mesh_materials() rows
    long long current_grm_entry_idx_ = -1; // bin holding the container in use
    bool current_grm_is_crossbin_ = false; // resolved from a sibling bin
    // (entry, geo_key) -> parsed geo (nullptr == unparseable)
    std::map<std::pair<quint32, quint32>, std::shared_ptr<jade::GeoInfo>>
        geo_parse_cache_;
    // (entry, geo_key, elem) -> QPixmap (null == None)
    std::map<std::tuple<quint32, quint32, int>, QPixmap> elem_thumb_cache_;
    // path -> (mtime_ms, vert count) — the over-u16 guard's cache
    std::map<QString, std::pair<qint64, long long>> glb_vcount_cache_;
    // cross-bin geo-key scan cache (per BF): geo_key -> [entry_idx, …]
    bool geo_key_idx_cache_valid_ = false;
    std::map<quint32, std::vector<quint32>> geo_key_idx_cache_;
    // per-entry resolver cache (the Python get_cached_resolver, bounded 8)
    std::vector<std::pair<quint32, std::shared_ptr<MeshSwapResolver>>>
        resolver_cache_;

    QString apply_tip_default_;

    // ── widgets ────────────────────────────────────────────────────────
    QLabel* bf_label_ = nullptr;
    QComboBox* entry_combo_ = nullptr;
    QPushButton* list_btn_ = nullptr;
    QComboBox* geo_combo_ = nullptr;
    QPushButton* export_glb_btn_ = nullptr;
    QPushButton* export_glb_hier_btn_ = nullptr;
    QTableWidget* mat_table_ = nullptr;
    QLineEdit* glb_edit_ = nullptr;
    QCheckBox* vcolor_check_ = nullptr;
    QLabel* project_hint_ = nullptr;
    QPushButton* disable_btn_ = nullptr;
    QPushButton* apply_btn_ = nullptr;
    QTextEdit* log_ = nullptr;
};
