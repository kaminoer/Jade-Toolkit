// PlacementViewport.hpp — level-editor viewport for the placement tab
// (port of gui/placement_viewport.py).
//
// The Python module is built on pygfx (WebGPU scene graph), which has no
// C++ equivalent here — this port re-implements the same outward
// behaviour on QOpenGLWidget in the style of Viewer3DWidget (GL 3.3
// core for meshes/lines, QPainter for the 2D overlays: axes helper,
// selection outline, transform gizmos, non-visual object markers).
//
// Drop-in replacement for viewer_3d.Viewer3DPanel — same outward API
// (viewer()->load_meshes, set_transform_gizmo, point_picked /
// gizmo_moved signals) so the placement tab swaps it in with one
// include change.
//
// Targets a 2003-game look: unlit textured surfaces (MeshBasicMaterial
// equivalent) with optional baked per-vertex RLI multiply. No PBR.
#pragma once

#include <QImage>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "Viewer3D.hpp"  // Viewer3DPanel::AnimEntry (set_animations parity)

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

class PlacementViewport;

namespace placement {

using V3 = std::array<double, 3>;
using Quat = std::array<double, 4>;   // (x, y, z, w) — pylinalg order
using Mat4d = std::array<double, 16>; // row-major (numpy layout)

// One geometry element of a multi-material GAO ({'nTri': int}).
struct ElementInfo {
    int nTri = 0;
};

// One scene dict handed to load_meshes — mirrors the Python mesh dict.
// `gao_key` -1 == Python None. Markers set `is_marker` + position +
// marker_category and ignore the geometry fields.
struct MeshDict {
    std::vector<float> vertices;    // 3*n (display / Y-up space)
    std::vector<float> normals;     // 3*n (may be empty)
    std::vector<float> uvs;         // 2*k UV pool (indexed by face_uvs)
    std::vector<int64_t> faces;     // 3*m vertex indices
    std::vector<int64_t> face_uvs;  // 3*m per-face UV indices (may be empty)
    std::vector<float> rli_colors;  // rli_comps*n baked per-vertex RLI
    int rli_comps = 0;              // 3 or 4 (0 == no RLI)
    std::array<float, 4> base_color{0.8f, 0.8f, 0.8f, 1.0f};
    long long gao_key = -1;         // None → -1
    QString name;
    std::vector<ElementInfo> elements;
    bool wireframe = false;         // collision overlay (line rendering)
    bool xray = false;
    bool depth_offset = false;
    bool pickable = true;
    float line_width = 1.6f;
    // Non-visual object marker fields
    bool is_marker = false;
    V3 position{0.0, 0.0, 0.0};
    QString marker_category;        // "camera"/"light"/… ("" → "other")
};

// get_gao_transform result: (position_offset, rotation_quat, scale).
struct GaoTransform {
    V3 position{0.0, 0.0, 0.0};
    Quat rotation{0.0, 0.0, 0.0, 1.0};
    V3 scale{1.0, 1.0, 1.0};
};

// {gao_key: [rgba_per_element, ...]} — a null QImage is Python None.
using TexturesByKey = std::map<uint32_t, std::vector<QImage>>;

}  // namespace placement

// OpenGL canvas: scene storage, camera controllers, picking, gizmos.
// Internal to the panel — the outward API lives on PlacementViewport.
class PlacementCanvas : public QOpenGLWidget,
                        protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit PlacementCanvas(PlacementViewport* panel,
                             QWidget* parent = nullptr);
    ~PlacementCanvas() override;

    // -- Scene API (called by the panel) --
    void load_meshes(const std::vector<placement::MeshDict>& mesh_list,
                     const placement::TexturesByKey& textures,
                     bool preserve_camera);
    void set_point_picking(bool enabled, double plane_y);
    bool alias_textures(uint32_t dest_gao_key, uint32_t source_gao_key);
    void set_gao_textures(uint32_t gao_key,
                          const std::vector<QImage>& rgba_per_element);
    void replace_object_meshes(long long gao_key,
                               std::vector<placement::MeshDict> mesh_dicts);
    placement::V3 focus_point_under_cursor();
    placement::V3 focus_point();
    std::optional<placement::GaoTransform> get_gao_transform(uint32_t gao_key);
    void set_gao_transform(uint32_t gao_key,
                           const std::optional<placement::V3>& position,
                           const std::optional<placement::Quat>& rotation_quat,
                           const std::optional<placement::V3>& scale);
    void set_transform_gizmo(bool enabled,
                             const std::optional<placement::V3>& position);
    void select_gao(long long gao_key);  // Python _select_gao (-1 clears)
    void auto_frame();                   // Python _auto_frame
    void set_tool(const QString& tool);  // "move" | "rotate" | "scale"
    void cleanup_gl();

    // -- Toolbar-driven state --
    void set_textures_visible(bool on);
    void set_show_wireframe(bool on);
    void set_show_baked(bool on);
    void set_show_normals(bool on);
    void set_show_collision(bool on);
    void set_collision_xray(bool on);
    void set_fps_visible(bool on);
    void set_camera_mode(const QString& mode);  // "fly" | "orbit"

    // Scroll-wheel fly-speed multiplier (Unity-style; see WasdFilter).
    double user_speed_mult = 1.0;

signals:
    void point_picked(double x, double y, double z);   // display-space XYZ
    void gizmo_moved(double x, double y, double z);    // display-space XYZ
    // qlonglong (not int) carries the gao_key because synthetic selection
    // keys for pending creates live above 0x7FFFFFFF and would overflow
    // a C-int signal payload.
    void object_moved(qlonglong gao_key, double x, double y, double z);
    void object_transformed(qlonglong gao_key);
    void object_selected(qlonglong gao_key);           // gao_key, or -1

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void tick_wasd();
    void sample_fps();

private:
    // Scene-rebuild body — requires a current GL context (load_meshes
    // defers to it via the pending-scene path when GL isn't up yet).
    void load_scene(const std::vector<placement::MeshDict>& mesh_list,
                    const placement::TexturesByKey& textures,
                    bool preserve_camera);

    // GL texture wrapper: deletion is queued so shared_ptr owners can
    // drop it without a current context (drained in paintGL/cleanup).
    struct GlTexture;
    // (texture | null, alpha_class) aligned with a GAO's elements.
    enum class AlphaClass { Opaque, Cutout, Blend };
    struct TexEntry {
        std::shared_ptr<GlTexture> tex;  // null == no texture
        AlphaClass alpha_class = AlphaClass::Opaque;
    };

    // Per-GAO group transform (pygfx Group.local: T·R·S).
    struct GroupXform {
        placement::V3 position{0.0, 0.0, 0.0};
        placement::Quat rotation{0.0, 0.0, 0.0, 1.0};
        placement::V3 scale{1.0, 1.0, 1.0};
    };

    // One solid element-mesh (mesh node + its Python _mesh_meta row).
    struct SolidMesh {
        unsigned vao = 0, ebo = 0;
        std::shared_ptr<unsigned> shared_vbo;  // shared across a GAO's elements
        int num_indices = 0;
        QString name;
        long long gao_key = -1;
        int element_index = 0;
        std::array<float, 4> base_color{0.8f, 0.8f, 0.8f, 1.0f};
        TexEntry texture;
        bool has_rli = false;
        bool pickable = true;
        bool depth_offset = false;
        // CPU copies for picking / overlays / bounds (mesh-local space).
        std::shared_ptr<std::vector<float>> vertices;   // 3*n (split)
        std::shared_ptr<std::vector<float>> normals;    // 3*n (split)
        std::vector<uint32_t> faces;                    // this element's tris
        // Normals-overlay line buffer (built on demand).
        unsigned normals_vao = 0, normals_vbo = 0;
        int normals_count = 0;
    };

    // One collision wireframe (Python kind=='collision' meta + gfx.Line).
    struct CollisionLine {
        unsigned vao = 0, vbo = 0;
        int count = 0;
        QString name;
        long long gao_key = -1;
        std::array<float, 4> color{0.8f, 0.8f, 0.8f, 1.0f};
        float line_width = 1.6f;
        bool xray = false;
    };

    // One non-visual object marker (billboarded category icon).
    struct Marker {
        placement::V3 local_pos{0.0, 0.0, 0.0};
        QString category;
        long long gao_key = -1;  // -1 == loose draft preview (not selectable)
    };

    struct PendingScene {
        std::vector<placement::MeshDict> mesh_list;
        placement::TexturesByKey textures;
        bool preserve_camera = false;
    };

    // -- scene build --
    void add_scene_object(const placement::MeshDict& md);
    void add_mesh(const placement::MeshDict& md);
    void add_marker(const placement::MeshDict& md);
    TexEntry make_texture(const QImage& rgba);
    void clear_meshes();
    void rebuild_overlays();
    void free_solid_mesh(SolidMesh& sm);
    void free_collision_line(CollisionLine& cl);
    void drain_texture_deletes();
    GroupXform& ensure_group(uint32_t key);

    // -- camera --
    placement::V3 cam_forward() const;
    placement::V3 cam_right() const;
    void show_pos(const placement::V3& target);
    placement::Mat4d proj_view() const;
    double current_fly_speed() const;
    double fly_speed_for_scene() const;
    bool cursor_over_canvas() const;
    void recompute_bounds();
    void reset_camera();

    // -- picking / gizmo --
    bool ray_from_screen(double sx, double sy, placement::V3& origin,
                         placement::V3& direction) const;
    std::optional<placement::V3> ray_to_plane(double sx, double sy,
                                              double plane_y) const;
    std::optional<QPointF> world_to_screen(const placement::V3& p) const;
    void handle_selection(double sx, double sy);
    std::optional<placement::V3> pick_world_hit(double sx, double sy);
    long long raycast_gao(double sx, double sy);
    double gizmo_world_scale(const placement::V3& world_pos,
                             long long gao_key) const;
    std::optional<int> tool_axis_at(double sx, double sy) const;
    std::optional<int> gizmo_axis_at(double sx, double sy) const;
    std::optional<int> ring_axis_at(double sx, double sy) const;
    std::optional<double> mouse_angle_on_ring(int axis, double sx,
                                              double sy) const;
    std::optional<double> mouse_axis_distance(int axis, double sx,
                                              double sy) const;
    void begin_tool_drag(int axis, double sx, double sy);
    void drag_move(double sx, double sy);
    void drag_rotate(double sx, double sy);
    void drag_scale(double sx, double sy);
    std::optional<placement::V3> drag_gizmo_to(double sx, double sy) const;
    void adjust_position_for_pivot(uint32_t gao_key,
                                   const std::optional<placement::Quat>& q,
                                   const std::optional<placement::V3>& scale);
    std::optional<placement::V3> gao_centre_offset(uint32_t gao_key) const;
    void reposition_gizmo_to_selection();

    // -- overlays (QPainter) --
    void draw_overlays();
    void draw_axes_helper(QPainter& painter);
    void draw_markers(QPainter& painter);
    void draw_selection_outline(QPainter& painter);
    void draw_gizmo(QPainter& painter);
    void draw_marker_icon(QPainter& painter, const QPointF& c,
                          const QString& category);

    // -- fps / legend labels --
    void position_fps_label();
    void rebuild_collision_legend();
    void position_collision_legend();

    PlacementViewport* panel_ = nullptr;

    // Scene
    std::vector<SolidMesh> solid_meshes_;
    std::vector<CollisionLine> collision_lines_;
    std::vector<Marker> markers_;
    std::map<uint32_t, GroupXform> gao_groups_;
    std::map<uint32_t, std::pair<placement::V3, placement::V3>> gao_bounds_;
    std::map<uint32_t, std::vector<TexEntry>> textures_by_key_;
    std::vector<unsigned> pending_tex_delete_;
    bool wireframe_built_ = false;  // normals-overlay buffers valid

    placement::V3 scene_center_{0.0, 0.0, 0.0};
    double scene_radius_ = 5.0;

    // Camera (fly-style: position + FPS yaw/pitch; orbit reuses it)
    placement::V3 cam_pos_{5.0, 5.0, 5.0};
    double cam_yaw_ = 0.0;    // radians, around world +Y
    double cam_pitch_ = 0.0;  // radians, around local +X
    QString cam_mode_ = QStringLiteral("fly");
    placement::V3 orbit_target_{0.0, 0.0, 0.0};
    bool orbit_target_set_ = false;

    // Interaction state
    bool point_picking_ = false;
    double pick_plane_y_ = 0.0;
    bool gizmo_visible_ = false;
    placement::V3 gizmo_pos_{0.0, 0.0, 0.0};
    QString active_tool_ = QStringLiteral("move");
    std::optional<int> gizmo_drag_axis_;
    placement::V3 gizmo_drag_start_world_{};
    std::optional<QPointF> gizmo_drag_start_mouse_;
    long long gizmo_drag_target_ = -1;  // gao_key if dragging selection
    std::optional<placement::Quat> drag_start_rot_;
    std::optional<placement::V3> drag_start_scale_;
    std::optional<double> drag_start_angle_;      // rotate tool
    std::optional<double> drag_start_axis_dist_;  // scale tool
    long long selected_gao_ = -1;
    bool rmb_look_ = false;
    std::optional<QPointF> last_mouse_;
    std::optional<QPointF> last_cursor_xy_;

    // Display toggles
    bool show_normals_ = false;
    bool show_wireframe_ = false;
    bool show_collision_ = false;
    bool show_baked_ = true;
    bool textures_visible_ = true;
    bool show_collision_xray_ = true;

    // WASD fly tick
    QTimer wasd_timer_;
    std::optional<double> wasd_last_t_;
    bool wasd_debug_ = true;  // live WASD-gate readout in info bar
    double wasd_dbg_t_ = 0.0;

    // FPS counter overlay
    QLabel* fps_label_ = nullptr;
    QLabel* collision_legend_label_ = nullptr;
    QTimer fps_sample_timer_;
    int fps_frame_count_ = 0;
    double fps_last_t_ = 0.0;

    unsigned mesh_program_ = 0;
    unsigned line_program_ = 0;
    float max_line_width_ = 1.0f;
    bool gl_initialized_ = false;
    std::unique_ptr<PendingScene> pending_scene_;

    friend class PlacementViewport;
    friend class WasdFilter;
};

// Editor viewport panel: GL canvas + Qt toolbar + camera controllers.
//
// Public API mirrors the legacy Viewer3DPanel:
//   - viewer() returns this (so panel->viewer()->foo() works)
//   - signals point_picked, gizmo_moved, collision_toggled
//   - load_meshes(meshes, textures, preserve_camera)
//   - set_transform_gizmo(enabled, position)
//   - set_point_picking(enabled, plane_y)
//   - set_info(text)
//   - set_collision_toggle_visible(visible) / collision_visible()
//   - set_animations({}) — no-op
//   - cleanup_gl()
class PlacementViewport : public QWidget {
    Q_OBJECT
public:
    explicit PlacementViewport(QWidget* parent = nullptr);
    ~PlacementViewport() override;

    // Python `.viewer` property returns self, so panel.viewer.foo() works.
    PlacementViewport* viewer() { return this; }

    void set_info(const QString& text);
    void set_collision_toggle_visible(bool visible);
    bool collision_visible() const;
    bool markers_visible() const;
    // Level editor doesn't play animations; tolerated for API parity so
    // callers that bulk-call viewer panel methods don't break.
    void set_animations(const std::vector<Viewer3DPanel::AnimEntry>& = {}) {}
    void cleanup_gl();

    void load_meshes(const std::vector<placement::MeshDict>& mesh_list,
                     const placement::TexturesByKey& textures = {},
                     bool preserve_camera = false);
    void set_point_picking(bool enabled, double plane_y = 0.0);
    bool alias_textures(uint32_t dest_gao_key, uint32_t source_gao_key);
    void set_gao_textures(uint32_t gao_key,
                          const std::vector<QImage>& rgba_per_element);
    void replace_object_meshes(long long gao_key,
                               std::vector<placement::MeshDict> mesh_dicts);
    placement::V3 focus_point_under_cursor();
    placement::V3 focus_point();
    std::optional<placement::GaoTransform> get_gao_transform(uint32_t gao_key);
    void set_gao_transform(
        uint32_t gao_key,
        const std::optional<placement::V3>& position = std::nullopt,
        const std::optional<placement::Quat>& rotation_quat = std::nullopt,
        const std::optional<placement::V3>& scale = std::nullopt);
    void set_transform_gizmo(bool enabled);
    void set_transform_gizmo(bool enabled, const placement::V3& position);
    // Python `_select_gao` / `_auto_frame` — called directly by the tab.
    void select_gao(long long gao_key);
    void auto_frame();

signals:
    void point_picked(double x, double y, double z);
    void gizmo_moved(double x, double y, double z);
    void object_moved(qlonglong gao_key, double x, double y, double z);
    void object_transformed(qlonglong gao_key);
    void object_selected(qlonglong gao_key);  // gao_key, or -1
    void collision_toggled(bool on);
    void markers_toggled(bool on);            // non-visual object markers

private slots:
    void on_collision_toggle(bool checked);
    void on_camera_mode_changed(int idx);

private:
    void sync_tool_button(const QString& tool);

    PlacementCanvas* canvas_ = nullptr;
    QCheckBox* tex_check_ = nullptr;
    QCheckBox* wire_check_ = nullptr;
    QCheckBox* baked_check_ = nullptr;
    QCheckBox* normals_check_ = nullptr;
    QCheckBox* collision_check_ = nullptr;
    QCheckBox* markers_check_ = nullptr;
    QCheckBox* xray_check_ = nullptr;
    QCheckBox* fps_check_ = nullptr;
    QButtonGroup* tool_group_ = nullptr;
    std::map<QString, QPushButton*> tool_buttons_;
    QComboBox* cam_mode_ = nullptr;
    QLabel* info_label_ = nullptr;
    QObject* wasd_filter_ = nullptr;

    friend class PlacementCanvas;
    friend class WasdFilter;
};
