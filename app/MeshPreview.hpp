// MeshPreview.hpp — 3D mesh preview panel (port of gui/mesh_preview.py).
//
// The Python module rendered through PyVista / VTK (QtInteractor). PyVista
// has no native equivalent, so this port re-implements the same outward
// panel on a QOpenGLWidget viewport modeled on Viewer3DWidget
// (app/Viewer3D.cpp) — same toolbar (Apply material / Wireframe), same info
// bar, same animation playback row, and the same MeshPreviewPanel API that
// PreviewPanel consumes (`viewer()->load_animated_mesh`, `set_info`,
// `show_mesh`, `set_animations`, `load_glb_scene`, `load_animated_scene`).
//
// The per-element split (face_elems + element_images + element_render_modes)
// renders one GL draw group per element with its own texture and render
// mode, porting _pil_to_texture / _alpha_mode_for to QImage-based alpha
// processing.
//
// PORT GAP: load_glb_scene (vtkGLTFImporter) and load_animated_scene
// (core.animator.SceneAnimator) have no C++ backend — they are stubs that
// report "not ported yet" (see the .cpp).
#pragma once

#include <QImage>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPointF>
#include <QTimer>
#include <QWidget>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Viewer3D.hpp"  // viewer3d::SkinData / AnimTrack / V3 / Mat4

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

namespace meshpreview {

// Compat alias used by the rest of the GUI to gate 3D code paths. The
// Python value depended on the PyVista import; natively GL is always
// available.
inline constexpr bool HAS_GL = true;

// Per-element render mode decoded from a material's ul_Flags — the C++
// shape of the render-flags dict _alpha_mode_for consumes. ok == false is
// the Python None (no material resolved).
struct ElementRenderMode {
    bool ok = false;
    bool blend = false;       // 1..n = real blend mode
    bool alpha_test = false;
    int  alpha_thresh = 0;
};

enum class AlphaMode { Auto, Cutout, Opaque };

// _alpha_mode_for: map a material's render flags to a (alpha_mode,
// threshold) pair for process_texture_alpha.
std::pair<AlphaMode, int> alpha_mode_for(const ElementRenderMode& rf);

// _pil_to_texture: normalize to RGBA and apply the alpha mode; *translucent
// tells the caller whether the surface must go through the translucent draw
// pass (else per-pixel alpha is ignored and it renders solid). A null input
// returns a null image with translucent=false.
QImage process_texture_alpha(const QImage& image, AlphaMode mode,
                             int threshold, bool* translucent);

// One render group: a triangle-index subset of the shared vertex buffer,
// with its own (already alpha-processed) texture and draw mode.
struct RenderGroup {
    std::vector<uint32_t> indices;   // 3*m vertex indices
    QImage texture;                  // processed RGBA; null = untextured gray
    bool translucent = false;
};

}  // namespace meshpreview

// The GL viewport: one shared interleaved vertex buffer, one draw group per
// element. Orbit camera (left-drag rotate, middle / Shift+left pan, wheel
// zoom), auto-framed on load. GL patterns lifted from Viewer3DWidget.
class MeshPreviewViewport : public QOpenGLWidget,
                            protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit MeshPreviewViewport(QWidget* parent = nullptr);
    ~MeshPreviewViewport() override;

    // Upload a mesh (Jade Z-up coords; converted to Y-up internally) split
    // into per-element render groups. Resets the camera to an isometric fit
    // (the Python reset_camera + view_isometric).
    void load_mesh(std::vector<float> verts_jade,
                   std::vector<float> norms_jade,
                   std::vector<float> uvs,
                   std::vector<meshpreview::RenderGroup> groups);

    // The "Apply material" toggle: textures on/off (off = untextured gray).
    void set_textures_visible(bool visible);
    // The "Wireframe" toggle: overlay the mesh wireframe over the shading.
    void set_wireframe(bool enabled);

    void clear();
    void cleanup_gl();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    // GPU-side draw group (indices into the shared VBO). Translucent
    // groups keep CPU copies (indices + per-triangle centroids) so the
    // translucent pass can depth-sort triangles back-to-front each frame
    // — the visual equivalent of the Python plotter's
    // enable_depth_peeling(8) (per-pixel alpha ordering when one
    // transparent surface covers another: eyelashes over face, decals).
    struct GLGroup {
        unsigned ebo = 0;
        int num_indices = 0;
        unsigned texture_id = 0;
        bool translucent = false;
        std::vector<uint32_t> cpu_indices;  // translucent only
        std::vector<float> centroids;       // 3 per tri, Y-up (translucent)
    };

    struct PendingLoad {
        std::vector<float> verts_jade, norms_jade, uvs;
        std::vector<meshpreview::RenderGroup> groups;
    };

    void process_pending_load();
    void clear_gl_objects();
    unsigned upload_texture(const QImage& img);
    void auto_frame();
    viewer3d::Mat4 projection_matrix(int w, int h) const;
    viewer3d::Mat4 orbit_view() const;
    void camera_basis(viewer3d::V3& eye, viewer3d::V3& forward,
                      viewer3d::V3& right, viewer3d::V3& up) const;

    unsigned program_ = 0;
    unsigned vao_ = 0, vbo_ = 0;
    std::vector<GLGroup> groups_;
    int num_verts_ = 0;
    // Streaming index buffer + scratch for the sorted translucent pass.
    unsigned sort_ebo_ = 0;
    std::vector<std::pair<float, uint32_t>> sort_keys_;  // (depth, packed id)
    std::vector<uint32_t> sort_indices_;

    double cam_yaw_ = 45.0, cam_pitch_ = 30.0, cam_dist_ = 5.0;
    viewer3d::V3 cam_target_{0.0, 0.0, 0.0};
    std::optional<QPointF> last_mouse_;
    double scene_radius_ = 5.0;

    bool textures_visible_ = true;
    bool wireframe_ = false;
    bool gl_initialized_ = false;
    std::unique_ptr<PendingLoad> pending_load_;
};

class MeshPreviewPanel;

// Mimics the legacy ``Viewer3DPanel.viewer`` attribute surface
// (_ViewerShim). The old PreviewPanel code calls a few methods on
// ``viewer()`` (load_animated_mesh, set_anim_tracks); keeping a shim lets
// the GUI side stay stable while we rebuild the renderer underneath.
class MeshPreviewViewerShim {
public:
    explicit MeshPreviewViewerShim(MeshPreviewPanel* panel)
        : panel_(panel) {}

    // ``element_images`` + ``face_elems`` drive per-submesh texturing
    // (multi-material costumes). ``images[0]`` is the single-texture
    // fallback when per-element resolution found nothing.
    // ``element_render_modes`` carries each element's decoded material
    // render flags so opaque / alpha-test / alpha-blend each draw right.
    // All positions are Jade Z-up floats (3*n / 2*n flat arrays).
    void load_animated_mesh(
        const std::vector<float>& verts, const std::vector<float>& norms,
        const std::vector<float>& uvs, const std::vector<uint32_t>& faces,
        const std::vector<int32_t>& orig_idx,
        std::shared_ptr<viewer3d::SkinData> skin = nullptr,
        const std::vector<QImage>& images = {},
        const std::vector<int32_t>& face_elems = {},
        const std::vector<QImage>& element_images = {},
        const std::vector<meshpreview::ElementRenderMode>&
            element_render_modes = {});

    // No skinned playback in this panel (the Python behaviour) — surface
    // the data in the info bar so the user at least sees what's there.
    void set_anim_tracks(const std::vector<viewer3d::AnimTrack>& tracks,
                         double max_time);

private:
    MeshPreviewPanel* panel_;
};

// Qt widget showing one mesh in a GL viewport, with a toolbar.
class MeshPreviewPanel : public QWidget {
    Q_OBJECT
public:
    // One entry of the legacy set_animations list: (label, tracks, max_time).
    struct AnimEntry {
        QString label;
        std::vector<viewer3d::AnimTrack> tracks;
        double max_time = 0.0;
    };

    explicit MeshPreviewPanel(QWidget* parent = nullptr);
    ~MeshPreviewPanel() override;

    // ----- public API used by PreviewPanel -----

    void set_info(const QString& text);
    QString info_text() const;

    MeshPreviewViewerShim* viewer() { return &viewer_shim_; }

    // ----- full glTF scene path (multi-mesh + animations) -----
    //
    // PORT GAP: the Python load_glb_scene drives vtkGLTFImporter and
    // load_animated_scene drives core.animator.SceneAnimator; neither has a
    // C++ backend (core/animator.py unported). Both stubs set_info a "not
    // ported yet" note and return false.
    bool load_glb_scene(const QByteArray& glb_bytes_or_path,
                        const std::vector<double>& animation_durations = {},
                        int initial_animation = 0,
                        const QString& info_text = QString());
    bool load_animated_scene(const QString& info_text = QString());

    // Legacy hook: the OpenGL viewer used this to populate a playback
    // combo. We currently don't play animations — surface the count
    // instead of silently dropping the call.
    void set_animations(const std::vector<AnimEntry>& animations);

    // Display one indexed triangle mesh. When ``face_elems`` (per-face
    // element index) and ``element_images`` (per-element texture) are
    // given, the mesh is split into one textured draw group per element so
    // multi-material costumes show the correct texture on each submesh.
    void show_mesh(const std::vector<float>& verts,
                   const std::vector<float>& normals,
                   const std::vector<float>& uvs,
                   const std::vector<uint32_t>& faces,
                   const QImage& texture_image = QImage(),
                   const QString& info_text = QString(),
                   const std::vector<int32_t>& face_elems = {},
                   const std::vector<QImage>& element_images = {},
                   const std::vector<meshpreview::ElementRenderMode>&
                       element_render_modes = {});

protected:
    void closeEvent(QCloseEvent* ev) override;

private slots:
    void on_toggle();
    void on_anim_combo_changed(int idx);
    void on_play();
    void on_stop();
    void on_time_slider(int value);
    void on_anim_tick();

private:
    double duration() const;
    void update_time_label();
    void apply_time();

    QCheckBox* show_tex_ = nullptr;
    QCheckBox* show_wire_ = nullptr;
    QLabel* info_ = nullptr;

    QWidget* anim_row_ = nullptr;
    QComboBox* anim_combo_ = nullptr;
    QPushButton* play_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QSlider* time_slider_ = nullptr;
    QLabel* time_lbl_ = nullptr;

    MeshPreviewViewport* viewport_ = nullptr;
    MeshPreviewViewerShim viewer_shim_;

    // Full-scene animation state. Kept for API parity with the Python; no
    // importer/animator backend exists (PORT GAP), so playback never
    // engages — the slots guard out exactly like the Python does when both
    // its ``_importer`` and ``_animator`` are None.
    std::vector<double> anim_durations_;
    int anim_idx_ = -1;
    double anim_time_ = 0.0;
    bool anim_playing_ = false;
    QTimer anim_timer_;
};
