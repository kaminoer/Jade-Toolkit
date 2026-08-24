// Viewer3D.hpp — OpenGL 3D viewer widget for meshes with textures and
// animations (port of gui/viewer_3d.py).
#pragma once

#include <QImage>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPointF>
#include <QTimer>
#include <QWidget>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

namespace viewer3d {

// One mesh to upload — mirrors the Python mesh dict handed to load_meshes.
struct MeshData {
    std::vector<float> vertices;   // 3*n (display / Y-up space)
    std::vector<float> normals;    // 3*n (may be empty)
    std::vector<float> uvs;        // 2*n (may be empty)
    std::vector<int64_t> faces;    // 3*m vertex indices
    std::array<float, 4> base_color{0.8f, 0.8f, 0.8f, 1.0f};
    int texture_idx = -1;          // index into the images list, or -1
    bool pickable = true;
    bool depth_offset = false;
    bool wireframe = false;
    bool xray = false;
    float line_width = 1.0f;
};

// Skin data — mirrors parse_skin_data's dict ('bones' list).
struct SkinBone {
    int bone_idx = 0;
    std::array<float, 16> bind_matrix{};  // inverse bind matrix, row-major
    bool has_bind = false;
    std::vector<std::pair<int, float>> weights;  // (orig vert idx, weight)
};

struct SkinData {
    std::vector<SkinBone> bones;
};

// One keyframe: time + up to 4 values (xyz / xyzw).
struct AnimKey {
    double t = 0.0;
    std::array<double, 4> v{};
};

// One track: {'gizmo': int, 'type': 'rotation'|'translation', 'keyframes'}.
struct AnimTrack {
    int gizmo = 0;
    bool is_rotation = false;
    std::vector<AnimKey> keyframes;
};

using V3 = std::array<double, 3>;
using Mat4 = std::array<float, 16>;  // row-major (numpy layout)

}  // namespace viewer3d

// OpenGL widget with orbit camera, mesh rendering, and animation playback.
class Viewer3DWidget : public QOpenGLWidget,
                       protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit Viewer3DWidget(QWidget* parent = nullptr);
    ~Viewer3DWidget() override;

    // -- Public API (mirrors the Python) --
    void load_meshes(const std::vector<viewer3d::MeshData>& mesh_list,
                     const std::vector<QImage>& images = {},
                     bool preserve_camera = false);
    void load_animated_mesh(std::vector<float> verts_jade,
                            std::vector<float> norms_jade,
                            std::vector<float> uvs,
                            std::vector<uint32_t> faces,
                            std::vector<int32_t> orig_indices,
                            std::shared_ptr<viewer3d::SkinData> skin = nullptr,
                            std::vector<QImage> images = {});
    void set_anim_tracks(const std::vector<viewer3d::AnimTrack>& tracks,
                         double duration);
    void play_animation();
    void stop_animation();
    void set_anim_time(double time);
    void toggle_textures(bool visible);
    void set_grid_visible(bool visible);
    void set_point_picking(bool enabled, double plane_y = 0.0);
    void set_transform_gizmo(bool enabled);
    void set_transform_gizmo(bool enabled, const viewer3d::V3& position);
    void set_gizmo_position(double x, double y, double z);
    void reset_camera();
    void home_camera();
    void focus_selection();
    void frame_all();
    void clear();
    void cleanup_gl();

    double anim_duration() const { return anim_duration_; }
    bool has_rest_pose() const { return !rest_jade_.empty(); }
    void clear_anim_tracks();

signals:
    void time_updated(double current_time, double duration);
    void point_picked(double x, double y, double z);   // display-space XYZ
    void gizmo_moved(double x, double y, double z);    // display-space XYZ

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    // GPU-side mesh data (_GLMesh).
    struct GLMesh {
        unsigned vao = 0, vbo = 0, ebo = 0;
        int num_indices = 0;
        unsigned texture_id = 0;
        std::array<float, 4> base_color{0.8f, 0.8f, 0.8f, 1.0f};
        bool has_bounds = false;
        viewer3d::V3 min_bound{}, max_bound{};
        std::vector<float> vertices;      // CPU copy for picking (3*n)
        std::vector<uint32_t> triangles;  // CPU copy for picking (3*m)
        bool pickable = true;
        bool depth_offset = false;
        bool wireframe = false;
        bool xray = false;
        float line_width = 1.0f;
    };

    struct PendingLoad {
        std::vector<float> verts_jade, norms_jade, uvs;
        std::vector<uint32_t> faces;
        std::vector<int32_t> orig_indices;
        std::shared_ptr<viewer3d::SkinData> skin;
        std::vector<QImage> images;
    };

    void process_pending_load();
    void setup_skin(const std::shared_ptr<viewer3d::SkinData>& skin);
    void emit_point_pick(const QPointF& pos);
    void update_cursor();
    void on_nav_tick();
    void step_navigation(double dt);
    viewer3d::Mat4 projection_matrix(int w, int h) const;
    viewer3d::Mat4 orbit_view() const;
    void camera_basis(viewer3d::V3& eye, viewer3d::V3& forward,
                      viewer3d::V3& right, viewer3d::V3& up) const;
    viewer3d::V3 cam_right() const;
    viewer3d::V3 cam_up() const;
    viewer3d::V3 cam_forward() const;
    bool upload_mesh(const viewer3d::MeshData& md,
                     const std::map<int, unsigned>& tex_ids, GLMesh& out);
    std::optional<viewer3d::V3> pick_scene_point(const QPointF& screen_pos);
    bool ray_from_screen(const QPointF& screen_pos, viewer3d::V3& origin,
                         viewer3d::V3& direction);
    unsigned upload_texture(const QImage& img);
    void clear_meshes();
    void auto_frame();
    void update_scene_bounds();
    void draw_grid(const viewer3d::Mat4& mvp);
    void draw_grid_overlay(QPainter& painter, const viewer3d::Mat4& mvp);
    void draw_gizmo_overlay(QPainter& painter, const viewer3d::Mat4& mvp);
    void draw_projected_line(QPainter& painter, const viewer3d::Mat4& mvp,
                             const viewer3d::V3& start,
                             const viewer3d::V3& end, const QPen& pen);
    std::optional<QPointF> project_point(const viewer3d::V3& point,
                                         const viewer3d::Mat4& mvp) const;
    double gizmo_scale() const;
    std::optional<viewer3d::V3> gizmo_axis_hit(const QPointF& screen_pos);
    void drag_gizmo_to(const QPointF& screen_pos);
    void on_anim_tick();
    void apply_skinning(double time);

    unsigned program_ = 0;
    std::vector<GLMesh> meshes_;
    double cam_yaw_ = 45.0, cam_pitch_ = 30.0, cam_dist_ = 5.0;
    viewer3d::V3 cam_target_{0.0, 0.0, 0.0};
    std::optional<QPointF> last_mouse_;
    bool point_picking_ = false;
    double pick_plane_y_ = 0.0;
    bool transform_gizmo_ = false;
    viewer3d::V3 gizmo_pos_{0.0, 0.0, 0.0};
    std::optional<viewer3d::V3> gizmo_drag_axis_;
    viewer3d::V3 gizmo_drag_start_pos_{};
    std::optional<QPointF> gizmo_drag_start_mouse_;
    double gizmo_drag_scale_ = 1.0;
    bool grid_visible_ = true;
    viewer3d::V3 scene_center_{0.0, 0.0, 0.0};
    double scene_radius_ = 5.0;
    bool right_mouse_look_ = false;
    std::set<int> nav_keys_;
    bool nav_fast_ = false;
    double nav_speed_scalar_ = 1.0;
    QTimer nav_timer_;

    QTimer anim_timer_;
    double anim_time_ = 0.0;
    bool anim_playing_ = false;
    double anim_duration_ = 0.0;

    bool gl_initialized_ = false;
    std::unique_ptr<PendingLoad> pending_load_;

    // Skinned animation data (all in Jade Z-up coords).
    std::vector<float> rest_jade_;        // 3*n
    std::vector<float> rest_norms_jade_;  // 3*n
    std::vector<float> mesh_uvs_;         // 2*n
    std::vector<int32_t> orig_indices_;   // split → original vertex map
    std::vector<std::vector<int>> splits_of_orig_;  // inverse of the above
    std::shared_ptr<viewer3d::SkinData> skin_data_;
    std::vector<viewer3d::Mat4> bone_ibms_;
    std::map<int, std::vector<viewer3d::AnimKey>> anim_rot_;
    std::map<int, std::vector<viewer3d::AnimKey>> anim_trans_;
    bool textures_visible_ = true;
    float max_line_width_ = 1.0f;
};

// Container with 3D viewer, playback controls, texture toggle, and
// animation selector (Viewer3DPanel).
class Viewer3DPanel : public QWidget {
    Q_OBJECT
public:
    struct AnimEntry {
        QString label;
        std::vector<viewer3d::AnimTrack> tracks;
        double duration = 0.0;
    };

    explicit Viewer3DPanel(QWidget* parent = nullptr);

    Viewer3DWidget* viewer() const { return viewer_; }
    void set_info(const QString& text);
    bool collision_visible() const;
    void set_collision_toggle_visible(bool visible);
    void set_animations(const std::vector<AnimEntry>& anim_list);

signals:
    void anim_selected(int index);  // index in animation combo
    void collision_toggled(bool on);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_play();
    void on_stop();
    void on_tex_toggle(bool checked);
    void on_grid_toggle(bool checked);
    void on_frame();
    void on_reset_view();
    void on_home();
    void on_anim_combo_changed(int idx);
    void on_slider(int value);
    void on_time_updated(double current, double duration);

private:
    Viewer3DWidget* viewer_ = nullptr;
    QCheckBox* tex_check_ = nullptr;
    QCheckBox* grid_check_ = nullptr;
    QCheckBox* collision_check_ = nullptr;
    QComboBox* anim_combo_ = nullptr;
    QLabel* info_label_ = nullptr;
    QPushButton* play_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QSlider* time_slider_ = nullptr;
    QLabel* time_label_ = nullptr;
    std::vector<AnimEntry> anim_data_;
};
