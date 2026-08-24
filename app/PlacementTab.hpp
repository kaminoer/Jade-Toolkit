// PlacementTab.hpp — Level Editor tab (port of gui/placement_tab.py):
// viewport on top, Object + Create tabs in a right-hand inspector dock.
//
// Bidirectional inspector for the zone's existing GAOs and a creation
// workflow for adding new ones. Both routes produce project operations
// (modify_transform / add_object / …) that the user queues in the Project
// tab and builds via the normal pipeline — there is no local plan or
// per-tab BF write path.
//
// PORT NOTES (deviations from the Python, documented in GUI_PORT.md):
//  * The zone loader / preview-mesh builders (io_ops/object_placer.py's
//    GUI half: load_zone_info, build_preview_meshes, …) have no core
//    port — they are re-implemented here on the jade read APIs
//    (Gao/Geometry/Material/Texture/Collision/ObjectKinds/Light).
//  * Collision-follow detection, user confirmation, ghost preview, operation
//    serialization, and build-time replay use the native CollisionFollow core.
//  * .jgao import/export and the JGAO↔GLB converters use the native Jgao
//    core. Replace previews cover model, existing-GAO, and imported-JGAO
//    sources; replace-kind submission remains rejected, as in the Python UI.
//  * Imported-model placement and preview use the native static-model
//    importer. DAE, binary FBX, glTF/GLB, OBJ, STL, and PLY are supported.
//  * Baked-RLI vertex colours are not baked into zone preview meshes yet
//    (the viewport's "Baked" toggle shows unlit texture shading).
#pragma once

#include <QTimer>
#include <QWidget>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "jade/Geometry.hpp"
#include "jade/CollisionFollow.hpp"
#include "jade/Json.hpp"
#include "jade/Jgao.hpp"
#include "jade/Light.hpp"
#include "jade/SubEntry.hpp"

#include "PlacementViewport.hpp"

namespace jade { class BigFile; }
class ProjectDoc;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace placementtab {

// One zone GAO as the inspector sees it (load_zone_info's object dict).
struct ZoneObject {
    quint32 key = 0;
    QString name;
    bool is_root = true;
    bool is_marker = false;
    bool has_colmap = false, has_ode = false, has_obbox = false;
    quint32 identity = 0, editor_flags = 0;
    long long father_key = -1;               // -1 == root/none
    bool has_matrix = false;
    std::array<double, 16> matrix{};         // Jade-native layout
    int matrix_type = -1;                    // -1 == unknown
    placement::V3 position{0, 0, 0};         // Jade space
    QStringList flag_names;
    long long geo_key = -1;
    long long material_key = -1;             // owning grm
    std::shared_ptr<jade::GeoInfo> geo;      // parsed GEO (visuals only)
    int sub_index = -1;                      // index into ZoneInfo::subs
    // Markers only:
    QString category;                        // "camera"/"light"/…
    long long light_key = -1;
    std::optional<jade::LightInfo> light;
};

// The loaded zone (load_zone_info's dict).
struct ZoneInfo {
    bool ok = false;
    QString error;
    quint32 entry_index = 0;
    quint32 entry_key = 0;
    QString entry_name;
    size_t dec_size = 0;
    std::vector<jade::SubEntry> subs;
    std::map<quint32, int> sub_by_key;       // key -> index into subs
    // Visual objects first ([0, n_visual)), then non-visual markers —
    // the Python keeps two lists merged into one objects_by_key.
    std::vector<ZoneObject> objects;
    size_t n_visual = 0;
    std::map<quint32, int> objects_by_key;   // key -> index into objects
    // Decoded zone textures by texture key (QImage RGBA), reused by the
    // inspector's live material swap so it doesn't re-decode the zone.
    std::map<quint32, QImage> tex_by_key;
    struct MatEntry {
        quint32 key = 0;
        QString label;
    };
    std::vector<MatEntry> materials;
    // Prebuilt viewport scene (display space).
    std::vector<placement::MeshDict> scene_meshes;
    std::vector<placement::MeshDict> marker_meshes;
    std::vector<placement::MeshDict> collision_meshes;
    placement::TexturesByKey textures_by_gao_key;
};

// A pending / committed Create placement (the placement-op dict).
struct Placement {
    QString kind;                            // clone|model|cube|sphere|cylinder|replace|replace_bin
    QString name;
    placement::V3 position{0, 0, 0};         // Jade space
    placement::Quat rotation_quat{0, 0, 0, 1};
    placement::V3 scale_xform{1, 1, 1};
    placement::V3 size{1, 1, 1};
    long long source_key = -1;               // clone (in-zone)
    long long source_entry_key = -1;         // cross-bin clone (BF FAT key)
    long long source_entry_index = -1;       // replacement source (BF index)
    long long target_key = -1;               // replace target GAO
    long long material_key = -1;
    bool collision = true;
    bool clone_with_collision = false;
    QString collision_profile = QStringLiteral("simple_box");
    long long room_cob_key = -1;
    QString model_path;
    std::shared_ptr<const jade::jgao::File> imported_jgao;
    bool import_vertex_colors = false;
    std::optional<std::array<int, 4>> vertex_color;
    // Session state:
    quint32 synthetic_key = 0;
    QString op_id;                           // committed AddObject's id
    bool has_build_position = false;
    placement::V3 build_position{0, 0, 0};
};

// Committed viewport state per GAO ({pos, rot_quat, scale, identity}).
struct CommittedState {
    placement::V3 pos{0, 0, 0};
    placement::Quat rot_quat{0, 0, 0, 1};
    placement::V3 scale{1, 1, 1};
    quint32 identity = 0;
};

// One cached cross-bin clone source bin (object_placer._xbin_source_for_op's
// per-FAT-key cache entry).
struct XbinBin {
    std::vector<jade::SubEntry> subs;
    std::map<quint32, int> sub_by_key;
    std::vector<ZoneObject> objects;
    std::map<quint32, int> objects_by_key;
    placement::TexturesByKey textures;       // per-GAO per-element rgba
};

// Pending light edit (changed-only field subset of EditLight).
struct PendingLightEdit {
    quint32 light_key = 0;
    std::optional<int> light_type;
    std::optional<std::array<int, 3>> diffuse, specular;
    std::optional<double> near_, far_, inner_angle, outer_angle, intensity;
    bool empty() const {
        return !light_type && !diffuse && !specular && !near_ && !far_
               && !inner_angle && !outer_angle && !intensity;
    }
};

}  // namespace placementtab

// Level editor: viewport + Object inspector + Create workflow.
class PlacementTab : public QWidget {
    Q_OBJECT
public:
    explicit PlacementTab(QWidget* parent = nullptr);

    // ── Public hooks (called by main_window) ──
    void set_bigfile(std::shared_ptr<jade::BigFile> bf,
                     const QString& bf_path);
    void set_project(ProjectDoc* project);
    void receive_asset(quint32 parent_index, quint32 key);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void apply_filter();
    void on_entry_clicked(QTreeWidgetItem* item, int column);
    void on_kind_changed();
    void on_pick_prim_color();
    void on_clear_prim_color();
    void on_collision_check_changed(bool checked);
    void on_add_pending_object();
    void on_inspector_edited();
    void on_inspector_frame();
    void on_inspector_material_picked(int index);
    void on_flag_toggled(bool checked);
    void on_light_edited();
    void on_add_collision_clicked();
    void on_commit_map();
    void on_discard_map();
    void on_viewer_object_selected(qlonglong gao_key);
    void on_viewer_object_transformed(qlonglong gao_key);
    void on_viewer_point_picked(double x, double y, double z);
    void on_viewer_gizmo_moved(double x, double y, double z);
    void on_collision_preview_toggled(bool checked);
    void on_markers_toggled(bool checked);
    void on_replace_from_changed();
    void on_load_source_entry();
    void on_browse_model();
    void on_source_filter_changed(const QString& text);
    void on_source_combo_user_picked(int index);
    void on_draft_control_changed();
    void on_export_gao();
    void on_import_gao();
    void on_jgao_to_glb();
    void on_glb_to_jgao();

private:
    // ── Tab builders ──
    static QScrollArea* wrap_scroll(QWidget* widget);
    QWidget* build_object_tab();
    QWidget* build_create_tab();
    static QDoubleSpinBox* spin(double minimum, double maximum, double value,
                                double step, int decimals = 4);
    static QWidget* row(const std::vector<QWidget*>& widgets);

    // ── Zone loading / preview (object_placer's GUI half, local port) ──
    void populate_entries();
    void load_zone(quint32 entry_index);
    placementtab::ZoneInfo load_zone_info(quint32 entry_index);
    void populate_create_controls();
    void populate_source_entry_combo();
    // The GAO's geometry-element matIds (single 0 for pending creates).
    std::vector<int> gao_element_matids(quint32 gao_key);
    // Cross-bin clone source resolution (cached per source BF FAT key).
    const placementtab::ZoneObject* xbin_source_for_op(
        const placementtab::Placement& placement,
        std::shared_ptr<placementtab::XbinBin>* out_bin = nullptr);
    void rebuild_source_combo();
    void sync_clone_source_to_selection();
    void set_create_controls_enabled(bool enabled);
    // Build one placement's preview meshes (display space, TRS baked).
    std::vector<placement::MeshDict> build_preview_meshes(
        const placementtab::Placement& placement);
    std::vector<placement::MeshDict> build_replacement_preview_meshes(
        const std::vector<const placementtab::Placement*>& placements);
    std::vector<placement::MeshDict> build_collision_preview_meshes(
        const placementtab::Placement& placement);
    std::vector<placement::MeshDict> build_added_collision_meshes();
    const jade::collision_follow::CollisionLinks* detect_collision_links(
        quint32 gao_key);
    std::optional<jade::collision_follow::CollisionLinks>
    enabled_collision_links(quint32 gao_key);
    std::vector<placement::MeshDict> build_collision_follow_ghosts(
        std::set<quint32>& hide_owner_keys);
    void refresh_preview(const placementtab::Placement* extra_operation
                         = nullptr,
                         bool preserve_camera = false);
    void rebuild_pending_object(quint32 syn_key);
    void refresh_current_draft(int delay_ms = 120);
    void refresh_current_draft_now();
    void sync_transform_gizmo(std::optional<bool> enabled = std::nullopt);
    void set_pick_mode(bool enabled);

    // ── Draft (Create) construction ──
    // ok == false + error mirrors the Python PlacementError.
    struct MakeOpResult {
        bool ok = false;
        QString error;
        placementtab::Placement placement;
    };
    MakeOpResult make_operation();

    // ── Inspector ──
    void set_inspector_enabled(bool enabled);
    void update_object_tab_for_selection(std::optional<quint32> gao_key);
    void populate_marker_inspector(const placementtab::ZoneObject& meta);
    void populate_light_editor(const placementtab::ZoneObject& meta);
    static void set_light_swatch(QPushButton* btn,
                                 const std::optional<std::array<int, 3>>& rgb);
    void pick_light_color(const QString& which);
    void populate_collision_follow(std::optional<quint32> gao_key);
    void update_addcollision_ui(std::optional<quint32> gao_key);
    void sync_inspector_material_combo(quint32 gao_key);
    std::vector<QLabel*> detail_widgets();
    void clear_details();
    void populate_inspector_for_pending(quint32 syn_key);
    void populate_details(const placementtab::ZoneObject& meta);
    void populate_geo_material_details(const placementtab::ZoneObject& meta);
    void write_inspector_values(const placement::V3& pos,
                                const placement::V3& rot_deg,
                                const placement::V3& scale);
    quint32 current_identity_from_checks() const;

    // ── Map-wide dirty tracking / commit / discard ──
    placementtab::ZoneObject* object_meta(quint32 gao_key);
    std::optional<std::array<double, 16>>
    compute_existing_gao_world_matrix(quint32 gao_key);
    std::optional<std::array<double, 16>>
    compute_pending_gao_world_matrix(quint32 syn_key,
                                     const placementtab::Placement& p);
    bool state_matches_committed(quint32 gao_key, const placement::V3& pos,
                                 const placement::Quat& q,
                                 const placement::V3& scale,
                                 quint32 identity) const;
    quint32 loaded_identity_for(quint32 gao_key) const;
    void mark_dirty_from_inspector();
    void update_map_dirty_indicator();
    void mark_dirty_gao_from_viewport(quint32 gao_key);
    // Existing op of op_type targeting gao_key in the current zone entry;
    // returns its id, or empty.
    QString find_op_for_gao(const QString& op_type, quint32 gao_key) const;
    void build_committed_cache();
    // AddObject op dict from a placement (empty Value.obj == failure).
    bool add_object_op_from_placement(const placementtab::Placement& p,
                                      quint32 entry_key,
                                      jade::json::Value& out);
    void refresh_inspector_from_committed();
    std::vector<placementtab::ZoneObject*> zone_objects();

    // ── State ──
    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    std::optional<placementtab::ZoneInfo> zone_info_;
    ProjectDoc* project_ = nullptr;
    std::vector<std::pair<quint32, QString>> replace_source_objects_;
    bool draft_preview_active_ = false;
    std::optional<quint32> selected_gao_key_;
    bool inspector_updating_ = false;
    quint32 loaded_identity_ = 0;
    std::map<quint32, QString> added_collision_shape_;    // gk -> shape
    std::map<quint32, QString> added_collision_profile_;  // gk -> profile
    std::map<quint32, jade::collision_follow::CollisionLinks>
        collision_links_cache_;
    std::map<quint32, std::map<std::string, bool>> colfollow_enabled_;
    std::vector<QCheckBox*> colfollow_rows_;
    QTimer* ghost_refresh_timer_ = nullptr;
    std::map<quint32, placementtab::CommittedState> committed_cache_;
    std::set<quint32> dirty_gaos_;
    std::map<quint32, quint32> pending_identity_;
    std::vector<quint32> pending_new_objects_;    // synthetic keys, in order
    std::vector<quint32> committed_new_objects_;  // synthetic keys
    std::set<quint32> dirty_committed_creates_;
    std::map<quint32, quint32> pending_material_;
    std::map<quint32, placementtab::PendingLightEdit> pending_light_edits_;
    std::optional<jade::LightInfo> cur_light_;
    long long cur_light_key_ = -1;
    std::map<quint32, placementtab::Placement> pending_objects_by_key_;
    quint32 next_synthetic_key_ = 0xFE000000u;
    // Cross-bin clone source bins, keyed by source BF FAT key. Reset
    // when the user opens a new BF.
    std::map<quint32, std::shared_ptr<placementtab::XbinBin>>
        xbin_source_cache_;
    std::vector<std::pair<QString, quint32>> all_source_objects_;
    bool clone_source_user_overridden_ = false;
    QTimer* draft_refresh_timer_ = nullptr;
    std::optional<std::array<int, 4>> prim_color_;
    std::shared_ptr<jade::jgao::File> imported_jgao_;
    QString imported_jgao_path_;

    // ── Widgets ──
    QLineEdit* filter_ = nullptr;
    QTreeWidget* entry_tree_ = nullptr;
    PlacementViewport* viewer_panel_ = nullptr;
    QTextEdit* log_ = nullptr;
    QLabel* map_dirty_label_ = nullptr;
    QPushButton* commit_map_btn_ = nullptr;
    QPushButton* discard_map_btn_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    // Object tab
    QLabel* inspector_header_ = nullptr;
    QLabel* inspector_meta_ = nullptr;
    int d_col_rows_[3] = {0, 0, 0};
    QLabel* d_gao_key_ = nullptr;
    QLabel* d_identity_ = nullptr;
    QLabel* d_editor_ = nullptr;
    QLabel* d_father_ = nullptr;
    QLabel* d_mat_type_ = nullptr;
    QLabel* d_position_ = nullptr;
    QLabel* d_geo_key_ = nullptr;
    QLabel* d_geo_hdr_ = nullptr;
    QLabel* d_geo_counts_ = nullptr;
    QLabel* d_geo_counts2_ = nullptr;
    QLabel* d_skin_ = nullptr;
    QLabel* d_vtxcol_ = nullptr;
    QLabel* d_mat_key_ = nullptr;
    QLabel* d_mat_kind_ = nullptr;
    QLabel* d_texture_ = nullptr;
    QLabel* d_drawmask_ = nullptr;
    QLabel* d_field3_ = nullptr;
    QLabel* d_collision_ = nullptr;
    QLabel* d_bv_ = nullptr;
    QLabel* d_flag_list_ = nullptr;
    std::vector<QDoubleSpinBox*> insp_pos_, insp_rot_, insp_scale_;
    QPushButton* frame_sel_btn_ = nullptr;
    QComboBox* insp_material_combo_ = nullptr;
    QGroupBox* light_group_ = nullptr;
    QComboBox* light_type_combo_ = nullptr;
    QPushButton* light_diffuse_btn_ = nullptr;
    QPushButton* light_specular_btn_ = nullptr;
    QLabel* light_specular_row_label_ = nullptr;
    QDoubleSpinBox* light_near_ = nullptr;
    QDoubleSpinBox* light_far_ = nullptr;
    QDoubleSpinBox* light_inner_ = nullptr;
    QDoubleSpinBox* light_outer_ = nullptr;
    QLabel* light_angle_label_ = nullptr;
    QDoubleSpinBox* light_intensity_ = nullptr;
    QLabel* light_intensity_label_ = nullptr;
    std::vector<std::pair<QCheckBox*, quint32>> flag_checks_;
    QGroupBox* colfollow_group_ = nullptr;
    QVBoxLayout* colfollow_vbox_ = nullptr;
    QLabel* colfollow_info_ = nullptr;
    QGroupBox* addcol_group_ = nullptr;
    QComboBox* addcol_profile_ = nullptr;
    QPushButton* addcol_btn_ = nullptr;
    // Create tab
    QComboBox* kind_combo_ = nullptr;
    QComboBox* source_combo_ = nullptr;
    QLineEdit* source_filter_ = nullptr;
    QComboBox* material_combo_ = nullptr;
    QLineEdit* model_path_edit_ = nullptr;
    QPushButton* model_browse_button_ = nullptr;
    QCheckBox* vcolor_check_ = nullptr;
    QPushButton* prim_color_btn_ = nullptr;
    QPushButton* prim_color_clear_ = nullptr;
    QComboBox* replace_from_combo_ = nullptr;
    QComboBox* source_entry_combo_ = nullptr;
    QPushButton* load_source_btn_ = nullptr;
    QComboBox* source_gao_combo_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QDoubleSpinBox* pos_x_ = nullptr;
    QDoubleSpinBox* pos_y_ = nullptr;
    QDoubleSpinBox* pos_z_ = nullptr;
    QCheckBox* pick_check_ = nullptr;
    QDoubleSpinBox* size_x_ = nullptr;
    QDoubleSpinBox* size_y_ = nullptr;
    QDoubleSpinBox* size_z_ = nullptr;
    QCheckBox* collision_check_ = nullptr;
    QComboBox* collision_profile_combo_ = nullptr;
    QComboBox* host_cob_combo_ = nullptr;
    QPushButton* export_gao_btn_ = nullptr;
    QPushButton* import_gao_btn_ = nullptr;
    QPushButton* jgao_to_glb_btn_ = nullptr;
    QPushButton* glb_to_jgao_btn_ = nullptr;
};
