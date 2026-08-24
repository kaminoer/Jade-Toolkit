#include "Viewer3D.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QPushButton>
#include <QSlider>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>

using viewer3d::AnimKey;
using viewer3d::AnimTrack;
using viewer3d::Mat4;
using viewer3d::MeshData;
using viewer3d::SkinData;
using viewer3d::V3;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// ── Math helpers ──

V3 operator+(const V3& a, const V3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
V3 operator-(const V3& a, const V3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V3 operator*(const V3& a, double s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
V3& operator+=(V3& a, const V3& b) {
    a[0] += b[0]; a[1] += b[1]; a[2] += b[2];
    return a;
}
V3& operator-=(V3& a, const V3& b) {
    a[0] -= b[0]; a[1] -= b[1]; a[2] -= b[2];
    return a;
}
double dot(const V3& a, const V3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
double norm(const V3& a) { return std::sqrt(dot(a, a)); }

// Row-major 4x4 multiply (numpy `@` layout).
Mat4 matmul(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    return out;
}

Mat4 identity4() {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

// Invert a general 4x4 (double precision, Gauss-Jordan). Returns false if
// singular (numpy raises LinAlgError there).
bool invert4(const Mat4& in, std::array<double, 16>& out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) a[r][c] = in[r * 4 + c];
        for (int c = 0; c < 4; ++c) a[r][4 + c] = (r == c) ? 1.0 : 0.0;
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::abs(a[r][col]) > std::abs(a[piv][col])) piv = r;
        if (std::abs(a[piv][col]) < 1e-12) return false;
        if (piv != col)
            for (int c = 0; c < 8; ++c) std::swap(a[piv][c], a[col][c]);
        const double d = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][4 + c];
    return true;
}

using Quat = std::array<double, 4>;  // (x, y, z, w)

// Spherical linear interpolation between two quaternions (x,y,z,w).
Quat slerp(Quat q0, Quat q1, double t) {
    double d = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
    if (d < 0) {
        for (double& v : q1) v = -v;
        d = -d;
    }
    if (d > 0.9995) {
        Quat result;
        for (int i = 0; i < 4; ++i)
            result[i] = q0[i] + t * (q1[i] - q0[i]);
        double n = std::sqrt(result[0] * result[0] + result[1] * result[1]
                             + result[2] * result[2] + result[3] * result[3]);
        for (double& v : result) v /= n;
        return result;
    }
    const double theta = std::acos(std::clamp(d, -1.0, 1.0));
    const double sin_theta = std::sin(theta);
    const double w0 = std::sin((1 - t) * theta) / sin_theta;
    const double w1 = std::sin(t * theta) / sin_theta;
    Quat out;
    for (int i = 0; i < 4; ++i) out[i] = w0 * q0[i] + w1 * q1[i];
    return out;
}

// Convert quaternion (x,y,z,w) to 3x3 rotation matrix (row-major, 9 doubles).
std::array<double, 9> quat_to_mat3(const Quat& q) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
            2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
            2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
}

// Interpolate a keyframe track at the given time.
std::optional<std::array<double, 4>> interp_keyframes(
    const std::vector<AnimKey>& keyframes, double time, bool is_rotation) {
    if (keyframes.empty()) return std::nullopt;
    if (time <= keyframes.front().t) return keyframes.front().v;
    if (time >= keyframes.back().t) return keyframes.back().v;
    for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
        const double t0 = keyframes[i].t, t1 = keyframes[i + 1].t;
        if (t0 <= time && time <= t1) {
            const double frac = (time - t0) / std::max(t1 - t0, 1e-9);
            if (is_rotation) {
                Quat q0{keyframes[i].v[0], keyframes[i].v[1],
                        keyframes[i].v[2], keyframes[i].v[3]};
                Quat q1{keyframes[i + 1].v[0], keyframes[i + 1].v[1],
                        keyframes[i + 1].v[2], keyframes[i + 1].v[3]};
                const Quat q = slerp(q0, q1, frac);
                return std::array<double, 4>{q[0], q[1], q[2], q[3]};
            }
            std::array<double, 4> out{};
            for (int k = 0; k < 4; ++k)
                out[k] = (1 - frac) * keyframes[i].v[k]
                         + frac * keyframes[i + 1].v[k];
            return out;
        }
    }
    return keyframes.back().v;
}

// Convert Jade Z-up position to Y-up: (x,y,z) → (x,z,-y).
inline void jade_to_yup(float x, float y, float z, float* out) {
    out[0] = x;
    out[1] = z;
    out[2] = -y;
}

const char* VERT_SHADER = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat3 uNormalMat;

out vec3 vNormal;
out vec2 vUV;
out vec3 vWorldPos;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = normalize(uNormalMat * aNormal);
    vUV = aUV;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
}
)";

const char* FRAG_SHADER = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec3 vWorldPos;

uniform sampler2D uTexture;
uniform bool uHasTexture;
uniform vec4 uBaseColor;
uniform vec3 uLightDir;

out vec4 FragColor;

void main() {
    vec4 color = uBaseColor;
    if (uHasTexture) {
        color = texture(uTexture, vUV) * uBaseColor;
    }
    float NdotL = max(dot(vNormal, uLightDir), 0.0);
    float ambient = 0.25;
    float diffuse = NdotL * 0.75;
    FragColor = vec4(color.rgb * (ambient + diffuse), color.a);
}
)";

// Distance from `point` to segment [start, end] in screen space.
double distance_to_segment(const QPointF& point, const QPointF& start,
                           const QPointF& end) {
    const double vx = end.x() - start.x();
    const double vy = end.y() - start.y();
    const double wx = point.x() - start.x();
    const double wy = point.y() - start.y();
    const double length_sq = vx * vx + vy * vy;
    if (length_sq < 1e-8) return std::hypot(wx, wy);
    const double t =
        std::clamp((wx * vx + wy * vy) / length_sq, 0.0, 1.0);
    const double px = start.x() + t * vx;
    const double py = start.y() + t * vy;
    return std::hypot(point.x() - px, point.y() - py);
}

void draw_arrow_head(QPainter& painter, const QPointF& start,
                     const QPointF& end, const QColor& color) {
    const QPointF vec = end - start;
    const double length = std::hypot(vec.x(), vec.y());
    if (length < 1.0) return;
    const double ux = vec.x() / length;
    const double uy = vec.y() / length;
    const double px = -uy;
    const double py = ux;
    const double size = 12.0;
    const QPointF back(end.x() - ux * size, end.y() - uy * size);
    const QPointF left(back.x() + px * size * 0.45,
                       back.y() + py * size * 0.45);
    const QPointF right(back.x() - px * size * 0.45,
                        back.y() - py * size * 0.45);
    painter.setBrush(color);
    painter.drawPolygon(QPolygonF({end, left, right}));
}

Mat4 look_at(const V3& eye, const V3& target, const V3& up) {
    V3 f = target - eye;
    const double fl = norm(f);
    f = {f[0] / fl, f[1] / fl, f[2] / fl};
    V3 r = cross(f, up);
    const double rl = norm(r);
    r = {r[0] / rl, r[1] / rl, r[2] / rl};
    const V3 u = cross(r, f);
    Mat4 m = identity4();
    m[0] = float(r[0]); m[1] = float(r[1]); m[2] = float(r[2]);
    m[4] = float(u[0]); m[5] = float(u[1]); m[6] = float(u[2]);
    m[8] = float(-f[0]); m[9] = float(-f[1]); m[10] = float(-f[2]);
    m[3] = float(-dot(r, eye));
    m[7] = float(-dot(u, eye));
    m[11] = float(dot(f, eye));
    return m;
}

Mat4 perspective(double fov_deg, double aspect, double near_p, double far_p) {
    const double f = 1.0 / std::tan(fov_deg * M_PI / 180.0 / 2.0);
    Mat4 m{};
    m[0] = float(f / aspect);
    m[5] = float(f);
    m[10] = float((far_p + near_p) / (near_p - far_p));
    m[11] = float((2 * far_p * near_p) / (near_p - far_p));
    m[14] = -1.0f;
    return m;
}

}  // namespace

// ── Viewer3DWidget ──

Viewer3DWidget::Viewer3DWidget(QWidget* parent) : QOpenGLWidget(parent) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(4);
    setFormat(fmt);

    nav_timer_.setParent(this);
    connect(&nav_timer_, &QTimer::timeout, this,
            &Viewer3DWidget::on_nav_tick);
    connect(&anim_timer_, &QTimer::timeout, this,
            &Viewer3DWidget::on_anim_tick);
    setFocusPolicy(Qt::StrongFocus);
}

Viewer3DWidget::~Viewer3DWidget() { cleanup_gl(); }

// -- Public API --

// Upload meshes to GPU. Each MeshData has vertices, normals, uvs, faces,
// base_color; images are textures indexed by MeshData::texture_idx.
void Viewer3DWidget::load_meshes(const std::vector<MeshData>& mesh_list,
                                 const std::vector<QImage>& images,
                                 bool preserve_camera) {
    makeCurrent();
    clear_meshes();
    rest_jade_.clear();
    rest_norms_jade_.clear();
    skin_data_.reset();
    anim_rot_.clear();
    anim_trans_.clear();
    std::map<int, unsigned> tex_ids;
    for (int i = 0; i < int(images.size()); ++i)
        tex_ids[i] = upload_texture(images[size_t(i)]);

    for (const MeshData& md : mesh_list) {
        GLMesh gm;
        if (upload_mesh(md, tex_ids, gm)) meshes_.push_back(std::move(gm));
    }

    if (!preserve_camera) auto_frame();
    doneCurrent();
    update();
}

// Load a mesh with optional skinned animation support.
//
// All positions/normals are in Jade Z-up coords — converted to Y-up
// internally. orig_indices maps split vertex indices back to original Jade
// vertex indices.
void Viewer3DWidget::load_animated_mesh(
    std::vector<float> verts_jade, std::vector<float> norms_jade,
    std::vector<float> uvs, std::vector<uint32_t> faces,
    std::vector<int32_t> orig_indices, std::shared_ptr<SkinData> skin,
    std::vector<QImage> images) {
    pending_load_ = std::make_unique<PendingLoad>(PendingLoad{
        std::move(verts_jade), std::move(norms_jade), std::move(uvs),
        std::move(faces), std::move(orig_indices), std::move(skin),
        std::move(images)});
    stop_animation();
    if (gl_initialized_)
        update();  // triggers paintGL which will process the pending load
    // otherwise initializeGL will trigger it
}

// Upload pending mesh data to GPU. Must be called with GL context active.
void Viewer3DWidget::process_pending_load() {
    if (!pending_load_) return;
    PendingLoad load = std::move(*pending_load_);
    pending_load_.reset();

    clear_meshes();

    const int n = int(load.verts_jade.size() / 3);
    rest_jade_ = std::move(load.verts_jade);
    rest_norms_jade_ = std::move(load.norms_jade);
    mesh_uvs_ = std::move(load.uvs);
    mesh_uvs_.resize(size_t(n) * 2, 0.0f);
    orig_indices_ = std::move(load.orig_indices);

    // Interleaved data: pos(3)+normal(3)+uv(2), converted to Y-up.
    std::vector<float> data(size_t(n) * 8, 0.0f);
    for (int i = 0; i < n; ++i) {
        jade_to_yup(rest_jade_[size_t(i) * 3], rest_jade_[size_t(i) * 3 + 1],
                    rest_jade_[size_t(i) * 3 + 2], &data[size_t(i) * 8]);
        if (size_t(i) * 3 + 2 < rest_norms_jade_.size())
            jade_to_yup(rest_norms_jade_[size_t(i) * 3],
                        rest_norms_jade_[size_t(i) * 3 + 1],
                        rest_norms_jade_[size_t(i) * 3 + 2],
                        &data[size_t(i) * 8 + 3]);
        data[size_t(i) * 8 + 6] = mesh_uvs_[size_t(i) * 2];
        data[size_t(i) * 8 + 7] = mesh_uvs_[size_t(i) * 2 + 1];
    }

    GLMesh gm;
    gm.num_indices = int(load.faces.size());
    gm.base_color = {0.8f, 0.8f, 0.8f, 1.0f};

    glGenVertexArrays(1, &gm.vao);
    glBindVertexArray(gm.vao);

    glGenBuffers(1, &gm.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.size() * sizeof(float)),
                 data.data(), GL_DYNAMIC_DRAW);

    const int stride = 8 * 4;
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(24));

    glGenBuffers(1, &gm.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 GLsizeiptr(load.faces.size() * sizeof(uint32_t)),
                 load.faces.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);

    // Upload textures
    if (!load.images.empty()) gm.texture_id = upload_texture(load.images[0]);

    meshes_.push_back(std::move(gm));

    // Setup skin data
    if (load.skin) setup_skin(load.skin);

    auto_frame();
}

// Pre-compute skin data structures for CPU animation.
void Viewer3DWidget::setup_skin(const std::shared_ptr<SkinData>& skin) {
    skin_data_ = skin;
    bone_ibms_.clear();
    for (const auto& bone : skin->bones) {
        Mat4 ibm = identity4();
        if (bone.has_bind)
            for (int i = 0; i < 16; ++i)
                ibm[size_t(i)] = bone.bind_matrix[size_t(i)];
        bone_ibms_.push_back(ibm);
    }

    // Inverse split→orig map, so apply_skinning finds split vertices for an
    // original vertex without a per-weight scan (same result as the numpy
    // mask, linear-time).
    int num_orig = 0;
    for (int32_t v : orig_indices_) num_orig = std::max(num_orig, int(v) + 1);
    splits_of_orig_.assign(size_t(num_orig), {});
    for (int i = 0; i < int(orig_indices_.size()); ++i) {
        const int32_t ov = orig_indices_[size_t(i)];
        if (ov >= 0 && int(ov) < num_orig)
            splits_of_orig_[size_t(ov)].push_back(i);
    }
}

// Set animation tracks for playback.
void Viewer3DWidget::set_anim_tracks(const std::vector<AnimTrack>& tracks,
                                     double duration) {
    anim_rot_.clear();
    anim_trans_.clear();
    for (const AnimTrack& trk : tracks) {
        if (trk.is_rotation)
            anim_rot_[trk.gizmo] = trk.keyframes;
        else
            anim_trans_[trk.gizmo] = trk.keyframes;
    }
    anim_duration_ = duration;
    anim_time_ = 0.0;
}

void Viewer3DWidget::clear_anim_tracks() {
    anim_rot_.clear();
    anim_trans_.clear();
}

void Viewer3DWidget::play_animation() {
    if ((!anim_rot_.empty() || !anim_trans_.empty()) && !anim_playing_) {
        anim_playing_ = true;
        anim_timer_.start(16);
    }
}

void Viewer3DWidget::stop_animation() {
    anim_playing_ = false;
    anim_timer_.stop();
}

// Seek to specific time and render one frame.
void Viewer3DWidget::set_anim_time(double time) {
    anim_time_ = time;
    if (skin_data_ && (!anim_rot_.empty() || !anim_trans_.empty())) {
        makeCurrent();
        apply_skinning(time);
        doneCurrent();
        update();
    }
}

// Show or hide textures on the mesh.
void Viewer3DWidget::toggle_textures(bool visible) {
    textures_visible_ = visible;
    update();
}

void Viewer3DWidget::set_grid_visible(bool visible) {
    grid_visible_ = visible;
    update();
}

// Enable click/drag picking against mesh triangles, falling back to a Y
// plane.
void Viewer3DWidget::set_point_picking(bool enabled, double plane_y) {
    point_picking_ = enabled;
    pick_plane_y_ = plane_y;
    update_cursor();
}

// Show a draggable XYZ transform gizmo at a display-space position.
void Viewer3DWidget::set_transform_gizmo(bool enabled) {
    transform_gizmo_ = enabled;
    update_cursor();
    update();
}

void Viewer3DWidget::set_transform_gizmo(bool enabled, const V3& position) {
    transform_gizmo_ = enabled;
    set_gizmo_position(position[0], position[1], position[2]);
    update_cursor();
    update();
}

void Viewer3DWidget::set_gizmo_position(double x, double y, double z) {
    gizmo_pos_ = {x, y, z};
    update();
}

void Viewer3DWidget::reset_camera() {
    cam_yaw_ = 45.0;
    cam_pitch_ = 30.0;
    auto_frame();
    update();
}

void Viewer3DWidget::home_camera() {
    cam_yaw_ = 45.0;
    cam_pitch_ = 30.0;
    cam_target_ = {0.0, 0.0, 0.0};
    cam_dist_ = std::max(scene_radius_ * 2.5, 5.0);
    update();
}

void Viewer3DWidget::focus_selection() {
    if (transform_gizmo_) {
        cam_target_ = gizmo_pos_;
        cam_dist_ = std::max(
            0.5, std::min(cam_dist_, std::max(scene_radius_ * 0.08, 2.0)));
    } else {
        auto_frame();
    }
    update();
}

void Viewer3DWidget::frame_all() {
    auto_frame();
    update();
}

void Viewer3DWidget::clear() {
    stop_animation();
    nav_timer_.stop();
    nav_keys_.clear();
    if (gl_initialized_) {
        makeCurrent();
        clear_meshes();
        doneCurrent();
    }
    pending_load_.reset();
    rest_jade_.clear();
    skin_data_.reset();
    anim_rot_.clear();
    anim_trans_.clear();
    transform_gizmo_ = false;
    scene_center_ = {0.0, 0.0, 0.0};
    scene_radius_ = 5.0;
    update();
}

void Viewer3DWidget::cleanup_gl() {
    stop_animation();
    nav_timer_.stop();
    nav_keys_.clear();
    if (!gl_initialized_) return;
    makeCurrent();
    clear_meshes();
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    doneCurrent();
    gl_initialized_ = false;
}

// -- OpenGL callbacks --

void Viewer3DWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.18f, 0.18f, 0.22f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    GLfloat line_width_range[2] = {1.0f, 1.0f};
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, line_width_range);
    max_line_width_ = std::max(1.0f, line_width_range[1]);

    auto compile = [this](const char* src, GLenum type) -> unsigned {
        unsigned shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(shader, sizeof log, nullptr, log);
            glDeleteShader(shader);
            std::fprintf(stderr, "Shader compile error: %s\n", log);
            return 0;
        }
        return shader;
    };
    const unsigned vs = compile(VERT_SHADER, GL_VERTEX_SHADER);
    const unsigned fs = compile(FRAG_SHADER, GL_FRAGMENT_SHADER);
    if (vs && fs) {
        unsigned prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(prog, sizeof log, nullptr, log);
            glDeleteProgram(prog);
            std::fprintf(stderr, "Program link error: %s\n", log);
        } else {
            program_ = prog;
        }
        glDeleteShader(vs);
        glDeleteShader(fs);
    }
    gl_initialized_ = true;
    // Process any mesh that was queued before GL was ready
    if (pending_load_) process_pending_load();
}

void Viewer3DWidget::resizeGL(int w, int h) { glViewport(0, 0, w, h); }

void Viewer3DWidget::paintGL() {
    if (!program_) return;
    // Process any deferred mesh upload (GL context is active here)
    if (pending_load_) process_pending_load();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program_);

    const int w = std::max(width(), 1);
    const int h = std::max(height(), 1);
    const Mat4 proj = projection_matrix(w, h);
    const Mat4 view = orbit_view();
    const Mat4 model = identity4();
    const Mat4 mvp = matmul(matmul(proj, view), model);

    // normal matrix = inv(model[:3,:3]).T — model is identity here.
    const float normal_mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    const int loc_mvp = glGetUniformLocation(program_, "uMVP");
    const int loc_model = glGetUniformLocation(program_, "uModel");
    const int loc_nmat = glGetUniformLocation(program_, "uNormalMat");
    const int loc_tex = glGetUniformLocation(program_, "uTexture");
    const int loc_has_tex = glGetUniformLocation(program_, "uHasTexture");
    const int loc_base = glGetUniformLocation(program_, "uBaseColor");
    const int loc_light = glGetUniformLocation(program_, "uLightDir");

    // Row-major matrices → transpose=GL_TRUE, matching the numpy upload.
    glUniformMatrix4fv(loc_mvp, 1, GL_TRUE, mvp.data());
    glUniformMatrix4fv(loc_model, 1, GL_TRUE, model.data());
    glUniformMatrix3fv(loc_nmat, 1, GL_TRUE, normal_mat);
    const float light_dir[3] = {0.5f, 0.8f, 0.6f};
    const float ll = std::sqrt(light_dir[0] * light_dir[0]
                               + light_dir[1] * light_dir[1]
                               + light_dir[2] * light_dir[2]);
    glUniform3f(loc_light, light_dir[0] / ll, light_dir[1] / ll,
                light_dir[2] / ll);
    glUniform1i(loc_tex, 0);

    for (const GLMesh& gm : meshes_) {
        glBindVertexArray(gm.vao);
        if (gm.texture_id && textures_visible_ && !gm.wireframe) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gm.texture_id);
            glUniform1i(loc_has_tex, 1);
        } else {
            glUniform1i(loc_has_tex, 0);
        }
        glUniform4f(loc_base, gm.base_color[0], gm.base_color[1],
                    gm.base_color[2], gm.base_color[3]);
        bool depth_was_enabled = false;
        if (gm.wireframe) {
            depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
            if (gm.xray) glDisable(GL_DEPTH_TEST);
            glDisable(GL_POLYGON_OFFSET_FILL);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        } else if (gm.depth_offset) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -1.0f);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
        glDrawElements(GL_TRIANGLES, gm.num_indices, GL_UNSIGNED_INT,
                       nullptr);
        if (gm.wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            if (gm.xray && depth_was_enabled) glEnable(GL_DEPTH_TEST);
        }
        glBindVertexArray(0);
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glUseProgram(0);
    draw_grid(mvp);
}

// -- Mouse interaction --

void Viewer3DWidget::mousePressEvent(QMouseEvent* event) {
    setFocus();
    last_mouse_ = event->position();
    if (event->button() == Qt::RightButton) {
        right_mouse_look_ = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (transform_gizmo_ && event->button() == Qt::LeftButton) {
        const auto hit_axis = gizmo_axis_hit(event->position());
        if (hit_axis) {
            gizmo_drag_axis_ = hit_axis;
            gizmo_drag_start_pos_ = gizmo_pos_;
            gizmo_drag_start_mouse_ = event->position();
            gizmo_drag_scale_ = gizmo_scale();
            return;
        }
    }
    if (point_picking_ && event->button() == Qt::LeftButton
        && event->modifiers() == Qt::NoModifier) {
        emit_point_pick(event->position());
        return;
    }
}

void Viewer3DWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!last_mouse_) return;

    if (gizmo_drag_axis_ && (event->buttons() & Qt::LeftButton)) {
        drag_gizmo_to(event->position());
        last_mouse_ = event->position();
        return;
    }

    if (point_picking_ && (event->buttons() & Qt::LeftButton)
        && event->modifiers() == Qt::NoModifier) {
        emit_point_pick(event->position());
        last_mouse_ = event->position();
        return;
    }

    const double dx = event->position().x() - last_mouse_->x();
    const double dy = event->position().y() - last_mouse_->y();

    if ((event->buttons() & Qt::RightButton) || right_mouse_look_) {
        cam_yaw_ += dx * 0.35;
        cam_pitch_ = std::clamp(cam_pitch_ + dy * 0.35, -89.0, 89.0);
        update();
    } else if ((event->buttons() & Qt::LeftButton)
               && !(event->modifiers() & Qt::ShiftModifier)) {
        cam_yaw_ += dx * 0.45;
        cam_pitch_ = std::clamp(cam_pitch_ + dy * 0.45, -89.0, 89.0);
        update();
    } else if ((event->buttons() & Qt::MiddleButton)
               || ((event->buttons() & Qt::LeftButton)
                   && (event->modifiers() & Qt::ShiftModifier))) {
        const double speed = cam_dist_ * 0.002;
        const V3 right = cam_right();
        const V3 up = cam_up();
        cam_target_ -= right * (dx * speed);
        cam_target_ += up * (dy * speed);
        update();
    }

    last_mouse_ = event->position();
}

void Viewer3DWidget::mouseReleaseEvent(QMouseEvent* event) {
    last_mouse_.reset();
    if (event->button() == Qt::RightButton) {
        right_mouse_look_ = false;
        update_cursor();
    }
    gizmo_drag_axis_.reset();
    gizmo_drag_start_mouse_.reset();
}

void Viewer3DWidget::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y();
    if (event->buttons() & Qt::RightButton) {
        nav_speed_scalar_ = std::clamp(
            nav_speed_scalar_ * (delta > 0 ? 1.15 : 0.87), 0.1, 20.0);
        event->accept();
        return;
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        cam_target_ +=
            cam_up() * (cam_dist_ * 0.08 * (delta > 0 ? 1.0 : -1.0));
        update();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        cam_target_ +=
            cam_forward() * (cam_dist_ * 0.08 * (delta > 0 ? 1.0 : -1.0));
        update();
        return;
    }
    const double factor = delta > 0 ? 0.9 : 1.1;
    cam_dist_ = std::max(0.1, cam_dist_ * factor);
    update();
}

void Viewer3DWidget::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (key == Qt::Key_A || key == Qt::Key_D || key == Qt::Key_W
        || key == Qt::Key_S || key == Qt::Key_Q || key == Qt::Key_E) {
        nav_keys_.insert(key);
        nav_fast_ = event->modifiers() & Qt::ShiftModifier;
        step_navigation(1.0 / 30.0);
        if (!nav_timer_.isActive()) nav_timer_.start(16);
        return;
    }
    if (key == Qt::Key_F) {
        focus_selection();
        return;
    }
    if (key == Qt::Key_H) {
        home_camera();
        return;
    }
    if (key == Qt::Key_R) {
        reset_camera();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void Viewer3DWidget::keyReleaseEvent(QKeyEvent* event) {
    const int key = event->key();
    if (nav_keys_.count(key)) {
        nav_keys_.erase(key);
        nav_fast_ = event->modifiers() & Qt::ShiftModifier;
        if (nav_keys_.empty()) nav_timer_.stop();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

void Viewer3DWidget::emit_point_pick(const QPointF& pos) {
    const auto hit = pick_scene_point(pos);
    if (hit) emit point_picked((*hit)[0], (*hit)[1], (*hit)[2]);
}

void Viewer3DWidget::update_cursor() {
    if (right_mouse_look_)
        setCursor(Qt::ClosedHandCursor);
    else if (transform_gizmo_)
        setCursor(Qt::ArrowCursor);
    else if (point_picking_)
        setCursor(Qt::CrossCursor);
    else
        unsetCursor();
}

void Viewer3DWidget::on_nav_tick() {
    if (nav_keys_.empty()) {
        nav_timer_.stop();
        return;
    }
    step_navigation(1.0 / 60.0);
}

void Viewer3DWidget::step_navigation(double dt) {
    double speed = std::max(0.05, cam_dist_ * 0.9) * dt * nav_speed_scalar_;
    if (nav_fast_) speed *= 3.0;
    const V3 forward = cam_forward();
    const V3 right = cam_right();
    const V3 world_up{0.0, 1.0, 0.0};
    V3 delta{0.0, 0.0, 0.0};
    if (nav_keys_.count(Qt::Key_W)) delta += forward * speed;
    if (nav_keys_.count(Qt::Key_S)) delta -= forward * speed;
    if (nav_keys_.count(Qt::Key_D)) delta += right * speed;
    if (nav_keys_.count(Qt::Key_A)) delta -= right * speed;
    if (nav_keys_.count(Qt::Key_E)) delta += world_up * speed;
    if (nav_keys_.count(Qt::Key_Q)) delta -= world_up * speed;
    if (delta[0] != 0.0 || delta[1] != 0.0 || delta[2] != 0.0) {
        cam_target_ += delta;
        update();
    }
}

// -- Internals --

Mat4 Viewer3DWidget::projection_matrix(int w, int h) const {
    const double aspect = std::max(double(w), 1.0) / std::max(double(h), 1.0);
    const double radius = std::max(scene_radius_, 0.5);
    const double near_p = std::max(0.02, std::min(50.0, cam_dist_ / 1000.0));
    double far_p = std::max({near_p + 100.0, cam_dist_ + radius * 4.0,
                             radius * 8.0});
    if (far_p <= near_p) far_p = near_p + 1000.0;
    return perspective(45.0, aspect, near_p, far_p);
}

Mat4 Viewer3DWidget::orbit_view() const {
    V3 eye, forward, right, up;
    camera_basis(eye, forward, right, up);
    return look_at(eye, cam_target_, {0.0, 1.0, 0.0});
}

void Viewer3DWidget::camera_basis(V3& eye, V3& forward, V3& right,
                                  V3& up) const {
    const double yaw_r = cam_yaw_ * M_PI / 180.0;
    const double pitch_r = cam_pitch_ * M_PI / 180.0;
    eye = cam_target_
          + V3{std::cos(pitch_r) * std::sin(yaw_r), std::sin(pitch_r),
               std::cos(pitch_r) * std::cos(yaw_r)}
                * cam_dist_;
    forward = cam_target_ - eye;
    const double forward_len = norm(forward);
    if (forward_len < 1e-8)
        forward = {0.0, 0.0, -1.0};
    else
        forward = forward * (1.0 / forward_len);
    const V3 world_up{0.0, 1.0, 0.0};
    right = cross(forward, world_up);
    const double right_len = norm(right);
    if (right_len < 1e-8)
        right = {1.0, 0.0, 0.0};
    else
        right = right * (1.0 / right_len);
    up = cross(right, forward);
    const double up_len = norm(up);
    if (up_len < 1e-8)
        up = world_up;
    else
        up = up * (1.0 / up_len);
}

V3 Viewer3DWidget::cam_right() const {
    V3 e, f, r, u;
    camera_basis(e, f, r, u);
    return r;
}

V3 Viewer3DWidget::cam_up() const {
    V3 e, f, r, u;
    camera_basis(e, f, r, u);
    return u;
}

V3 Viewer3DWidget::cam_forward() const {
    V3 e, f, r, u;
    camera_basis(e, f, r, u);
    return f;
}

bool Viewer3DWidget::upload_mesh(const MeshData& md,
                                 const std::map<int, unsigned>& tex_ids,
                                 GLMesh& gm) {
    const int n = int(md.vertices.size() / 3);
    if (n == 0 || md.faces.empty()) return false;
    for (float v : md.vertices)
        if (!std::isfinite(v)) return false;

    std::vector<float> norms;
    bool norms_ok = int(md.normals.size() / 3) == n;
    if (norms_ok)
        for (float v : md.normals)
            if (!std::isfinite(v)) { norms_ok = false; break; }
    if (norms_ok) {
        norms = md.normals;
    } else {
        norms.assign(size_t(n) * 3, 0.0f);
        for (int i = 0; i < n; ++i) norms[size_t(i) * 3 + 2] = 1.0f;
    }

    std::vector<float> uvs;
    bool uvs_ok = int(md.uvs.size() / 2) == n;
    if (uvs_ok)
        for (float v : md.uvs)
            if (!std::isfinite(v)) { uvs_ok = false; break; }
    if (uvs_ok)
        uvs = md.uvs;
    else
        uvs.assign(size_t(n) * 2, 0.0f);

    // Drop faces with out-of-range indices (the numpy valid_faces filter).
    std::vector<uint32_t> indices;
    indices.reserve(md.faces.size());
    for (size_t f = 0; f + 2 < md.faces.size(); f += 3) {
        const int64_t a = md.faces[f], b = md.faces[f + 1],
                      c = md.faces[f + 2];
        if (a >= 0 && a < n && b >= 0 && b < n && c >= 0 && c < n) {
            indices.push_back(uint32_t(a));
            indices.push_back(uint32_t(b));
            indices.push_back(uint32_t(c));
        }
    }
    if (indices.empty()) return false;

    // Interleave: pos(3) + normal(3) + uv(2) = 8 floats per vertex
    std::vector<float> data(size_t(n) * 8, 0.0f);
    for (int i = 0; i < n; ++i) {
        data[size_t(i) * 8 + 0] = md.vertices[size_t(i) * 3 + 0];
        data[size_t(i) * 8 + 1] = md.vertices[size_t(i) * 3 + 1];
        data[size_t(i) * 8 + 2] = md.vertices[size_t(i) * 3 + 2];
        data[size_t(i) * 8 + 3] = norms[size_t(i) * 3 + 0];
        data[size_t(i) * 8 + 4] = norms[size_t(i) * 3 + 1];
        data[size_t(i) * 8 + 5] = norms[size_t(i) * 3 + 2];
        data[size_t(i) * 8 + 6] = uvs[size_t(i) * 2 + 0];
        data[size_t(i) * 8 + 7] = uvs[size_t(i) * 2 + 1];
    }

    gm.num_indices = int(indices.size());
    gm.base_color = md.base_color;
    gm.min_bound = {md.vertices[0], md.vertices[1], md.vertices[2]};
    gm.max_bound = gm.min_bound;
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            const double v = md.vertices[size_t(i) * 3 + size_t(c)];
            gm.min_bound[size_t(c)] = std::min(gm.min_bound[size_t(c)], v);
            gm.max_bound[size_t(c)] = std::max(gm.max_bound[size_t(c)], v);
        }
    gm.has_bounds = true;
    gm.vertices = md.vertices;
    gm.triangles = indices;
    gm.pickable = md.pickable;
    gm.depth_offset = md.depth_offset;
    gm.wireframe = md.wireframe;
    gm.xray = md.xray;
    gm.line_width = md.line_width;

    glGenVertexArrays(1, &gm.vao);
    glBindVertexArray(gm.vao);

    glGenBuffers(1, &gm.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.size() * sizeof(float)),
                 data.data(), GL_STATIC_DRAW);

    const int stride = 8 * 4;
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(24));

    glGenBuffers(1, &gm.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 GLsizeiptr(indices.size() * sizeof(uint32_t)),
                 indices.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);

    // Texture
    if (md.texture_idx >= 0) {
        auto it = tex_ids.find(md.texture_idx);
        if (it != tex_ids.end()) gm.texture_id = it->second;
    }
    return true;
}

std::optional<V3> Viewer3DWidget::pick_scene_point(
    const QPointF& screen_pos) {
    V3 origin, direction;
    if (!ray_from_screen(screen_pos, origin, direction)) return std::nullopt;

    bool have_best = false;
    double best_t = 0.0;
    for (const GLMesh& gm : meshes_) {
        if (!gm.pickable || gm.vertices.empty() || gm.triangles.empty())
            continue;
        // Möller–Trumbore over every triangle (numpy _raycast_triangles).
        for (size_t f = 0; f + 2 < gm.triangles.size(); f += 3) {
            const float* p0 = &gm.vertices[size_t(gm.triangles[f]) * 3];
            const float* p1 = &gm.vertices[size_t(gm.triangles[f + 1]) * 3];
            const float* p2 = &gm.vertices[size_t(gm.triangles[f + 2]) * 3];
            const V3 v0{p0[0], p0[1], p0[2]};
            const V3 v1{p1[0], p1[1], p1[2]};
            const V3 v2{p2[0], p2[1], p2[2]};
            const V3 edge1 = v1 - v0;
            const V3 edge2 = v2 - v0;
            const V3 h = cross(direction, edge2);
            const double a = dot(edge1, h);
            if (std::abs(a) <= 1e-8) continue;
            const double fi = 1.0 / a;
            const V3 s = origin - v0;
            const double u = fi * dot(s, h);
            if (u < 0.0 || u > 1.0) continue;
            const V3 q = cross(s, edge1);
            const double v = fi * dot(direction, q);
            if (v < 0.0 || u + v > 1.0) continue;
            const double t = fi * dot(edge2, q);
            if (t > 1e-6 && (!have_best || t < best_t)) {
                have_best = true;
                best_t = t;
            }
        }
    }
    if (have_best) return origin + direction * best_t;

    if (std::abs(direction[1]) < 1e-8) return std::nullopt;
    const double plane_t = (pick_plane_y_ - origin[1]) / direction[1];
    if (plane_t <= 0) return std::nullopt;
    return origin + direction * plane_t;
}

bool Viewer3DWidget::ray_from_screen(const QPointF& screen_pos, V3& origin,
                                     V3& direction) {
    const int w = std::max(width(), 1);
    const int h = std::max(height(), 1);
    const double x = (2.0 * screen_pos.x() / double(w)) - 1.0;
    const double y = 1.0 - (2.0 * screen_pos.y() / double(h));
    const Mat4 pv = matmul(projection_matrix(w, h), orbit_view());
    std::array<double, 16> inv;
    if (!invert4(pv, inv)) return false;

    auto apply = [&inv](double px, double py, double pz, double pw,
                        double* out) {
        for (int r = 0; r < 4; ++r)
            out[r] = inv[size_t(r) * 4 + 0] * px
                     + inv[size_t(r) * 4 + 1] * py
                     + inv[size_t(r) * 4 + 2] * pz
                     + inv[size_t(r) * 4 + 3] * pw;
    };
    double near_v[4], far_v[4];
    apply(x, y, -1.0, 1.0, near_v);
    apply(x, y, 1.0, 1.0, far_v);
    if (std::abs(near_v[3]) < 1e-8 || std::abs(far_v[3]) < 1e-8) return false;
    const V3 near_p{near_v[0] / near_v[3], near_v[1] / near_v[3],
                    near_v[2] / near_v[3]};
    const V3 far_p{far_v[0] / far_v[3], far_v[1] / far_v[3],
                   far_v[2] / far_v[3]};
    V3 dir = far_p - near_p;
    const double length = norm(dir);
    if (length < 1e-8) return false;
    origin = near_p;
    direction = dir * (1.0 / length);
    return true;
}

// Upload a QImage as an OpenGL texture.
unsigned Viewer3DWidget::upload_texture(const QImage& img) {
    const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    unsigned tid = 0;
    glGenTextures(1, &tid);
    glBindTexture(GL_TEXTURE_2D, tid);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);
    return tid;
}

void Viewer3DWidget::clear_meshes() {
    for (GLMesh& gm : meshes_) {
        if (gm.vao) glDeleteVertexArrays(1, &gm.vao);
        if (gm.vbo) glDeleteBuffers(1, &gm.vbo);
        if (gm.ebo) glDeleteBuffers(1, &gm.ebo);
        if (gm.texture_id) glDeleteTextures(1, &gm.texture_id);
    }
    meshes_.clear();
}

// Adjust camera to fit loaded meshes using bounding box.
void Viewer3DWidget::auto_frame() {
    update_scene_bounds();
    if (!rest_jade_.empty()) {
        V3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
        for (size_t i = 0; i + 2 < rest_jade_.size(); i += 3) {
            float yup[3];
            jade_to_yup(rest_jade_[i], rest_jade_[i + 1], rest_jade_[i + 2],
                        yup);
            for (int c = 0; c < 3; ++c) {
                mn[size_t(c)] = std::min(mn[size_t(c)], double(yup[c]));
                mx[size_t(c)] = std::max(mx[size_t(c)], double(yup[c]));
            }
        }
        const V3 center = (mn + mx) * 0.5;
        const double radius = std::max(norm(mx - mn) * 0.5, 0.25);
        cam_target_ = center;
        cam_dist_ = std::max(
            radius / std::tan(45.0 * M_PI / 180.0 * 0.5) * 1.15, 0.5);
    } else if (meshes_.empty()) {
        cam_target_ = {0.0, 0.0, 0.0};
        cam_dist_ = 5.0;
    } else {
        bool any = false;
        V3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
        for (const GLMesh& gm : meshes_) {
            if (!gm.has_bounds) continue;
            any = true;
            for (int c = 0; c < 3; ++c) {
                mn[size_t(c)] =
                    std::min(mn[size_t(c)], gm.min_bound[size_t(c)]);
                mx[size_t(c)] =
                    std::max(mx[size_t(c)], gm.max_bound[size_t(c)]);
            }
        }
        if (any) {
            const V3 center = (mn + mx) * 0.5;
            const double radius = std::max(norm(mx - mn) * 0.5, 0.25);
            cam_target_ = center;
            cam_dist_ = std::max(
                radius / std::tan(45.0 * M_PI / 180.0 * 0.5) * 1.15, 0.5);
        } else {
            cam_target_ = {0.0, 0.0, 0.0};
            cam_dist_ = 5.0;
        }
    }
}

void Viewer3DWidget::update_scene_bounds() {
    V3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
    bool any = false;
    if (!rest_jade_.empty()) {
        for (size_t i = 0; i + 2 < rest_jade_.size(); i += 3) {
            float yup[3];
            jade_to_yup(rest_jade_[i], rest_jade_[i + 1], rest_jade_[i + 2],
                        yup);
            for (int c = 0; c < 3; ++c) {
                mn[size_t(c)] = std::min(mn[size_t(c)], double(yup[c]));
                mx[size_t(c)] = std::max(mx[size_t(c)], double(yup[c]));
            }
        }
        any = true;
    } else {
        for (const GLMesh& gm : meshes_) {
            if (!gm.has_bounds) continue;
            any = true;
            for (int c = 0; c < 3; ++c) {
                mn[size_t(c)] =
                    std::min(mn[size_t(c)], gm.min_bound[size_t(c)]);
                mx[size_t(c)] =
                    std::max(mx[size_t(c)], gm.max_bound[size_t(c)]);
            }
        }
    }
    if (!any) {
        scene_center_ = {0.0, 0.0, 0.0};
        scene_radius_ = 5.0;
        return;
    }
    scene_center_ = (mn + mx) * 0.5;
    scene_radius_ = std::max(norm(mx - mn) * 0.5, 0.5);
}

// Draw projected grid, world axes, and transform gizmo as a 2D overlay.
void Viewer3DWidget::draw_grid(const Mat4& mvp) {
    if (!grid_visible_ && !transform_gizmo_) return;
    QPainter painter(this);
    if (!painter.isActive()) return;
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (grid_visible_) draw_grid_overlay(painter, mvp);
    if (transform_gizmo_) draw_gizmo_overlay(painter, mvp);
    painter.end();
}

void Viewer3DWidget::draw_grid_overlay(QPainter& painter, const Mat4& mvp) {
    const double span = std::max(4.0, cam_dist_ * 1.5);
    double step =
        std::pow(10.0, std::floor(std::log10(std::max(span / 8.0, 0.001))));
    if (span / step > 18) step *= 2.0;
    const double center_x = std::round(cam_target_[0] / step) * step;
    const double center_z = std::round(cam_target_[2] / step) * step;
    const int count = 12;

    const QPen thin(QColor(76, 80, 86, 130), 1);
    const QPen x_axis(QColor(210, 75, 70, 210), 2);
    const QPen z_axis(QColor(70, 130, 230, 210), 2);
    const QPen y_axis(QColor(80, 180, 95, 210), 2);

    for (int i = -count; i <= count; ++i) {
        const double x = center_x + i * step;
        const QPen& pen_x = std::abs(x) < step * 0.5 ? x_axis : thin;
        draw_projected_line(painter, mvp, {x, 0.0, center_z - count * step},
                            {x, 0.0, center_z + count * step}, pen_x);

        const double z = center_z + i * step;
        const QPen& pen_z = std::abs(z) < step * 0.5 ? z_axis : thin;
        draw_projected_line(painter, mvp, {center_x - count * step, 0.0, z},
                            {center_x + count * step, 0.0, z}, pen_z);
    }

    double axis_len = std::max(step * 3.0, cam_dist_ * 0.3);
    axis_len = std::min(axis_len, step * 10.0);
    draw_projected_line(painter, mvp, {0.0, 0.0, 0.0},
                        {axis_len, 0.0, 0.0}, x_axis);
    draw_projected_line(painter, mvp, {0.0, 0.0, 0.0},
                        {0.0, axis_len, 0.0}, y_axis);
    draw_projected_line(painter, mvp, {0.0, 0.0, 0.0},
                        {0.0, 0.0, axis_len}, z_axis);
}

void Viewer3DWidget::draw_gizmo_overlay(QPainter& painter, const Mat4& mvp) {
    const auto origin = project_point(gizmo_pos_, mvp);
    if (!origin) return;
    const double scale = gizmo_scale();
    struct Axis {
        V3 dir;
        QColor color;
        const char* label;
    };
    const Axis axes[3] = {
        {{1.0, 0.0, 0.0}, QColor(225, 68, 62), "X"},
        {{0.0, 1.0, 0.0}, QColor(74, 190, 88), "Y"},
        {{0.0, 0.0, 1.0}, QColor(78, 138, 238), "Z"},
    };
    painter.setPen(QPen(QColor(245, 245, 245, 220), 1));
    painter.setBrush(QColor(245, 245, 245, 210));
    painter.drawEllipse(*origin, 4.0, 4.0);
    for (const Axis& ax : axes) {
        const auto end = project_point(gizmo_pos_ + ax.dir * scale, mvp);
        if (!end) continue;
        QPen pen(ax.color, 4);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(ax.color);
        painter.drawLine(*origin, *end);
        draw_arrow_head(painter, *origin, *end, ax.color);
        painter.drawText(*end + QPointF(5.0, -5.0), ax.label);
    }
}

void Viewer3DWidget::draw_projected_line(QPainter& painter, const Mat4& mvp,
                                         const V3& start, const V3& end,
                                         const QPen& pen) {
    const auto p0 = project_point(start, mvp);
    const auto p1 = project_point(end, mvp);
    if (!p0 || !p1) return;
    const double limit = std::max({width(), height(), 1}) * 8.0;
    const bool outside_x = (p0->x() < -limit && p1->x() < -limit)
                           || (p0->x() > limit && p1->x() > limit);
    const bool outside_y = (p0->y() < -limit && p1->y() < -limit)
                           || (p0->y() > limit && p1->y() > limit);
    if (outside_x || outside_y) return;
    painter.setPen(pen);
    painter.drawLine(*p0, *p1);
}

std::optional<QPointF> Viewer3DWidget::project_point(
    const V3& point, const Mat4& mvp) const {
    double clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = double(mvp[size_t(r) * 4 + 0]) * point[0]
                  + double(mvp[size_t(r) * 4 + 1]) * point[1]
                  + double(mvp[size_t(r) * 4 + 2]) * point[2]
                  + double(mvp[size_t(r) * 4 + 3]);
    if (clip[3] <= 1e-6) return std::nullopt;
    const double nx = clip[0] / clip[3];
    const double ny = clip[1] / clip[3];
    const double nz = clip[2] / clip[3];
    if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz)
        || nz < -1.05 || nz > 1.05)
        return std::nullopt;
    const double x = (nx * 0.5 + 0.5) * std::max(width(), 1);
    const double y = (1.0 - (ny * 0.5 + 0.5)) * std::max(height(), 1);
    return QPointF(x, y);
}

double Viewer3DWidget::gizmo_scale() const {
    return std::max(0.25, cam_dist_ * 0.18);
}

std::optional<V3> Viewer3DWidget::gizmo_axis_hit(const QPointF& screen_pos) {
    const int w = std::max(width(), 1);
    const int h = std::max(height(), 1);
    const Mat4 mvp = matmul(projection_matrix(w, h), orbit_view());
    const auto origin = project_point(gizmo_pos_, mvp);
    if (!origin) return std::nullopt;
    const V3 axes[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    std::optional<V3> best_axis;
    double best_dist = 12.0;
    for (const V3& axis : axes) {
        const auto end =
            project_point(gizmo_pos_ + axis * gizmo_scale(), mvp);
        if (!end) continue;
        const double dist = distance_to_segment(screen_pos, *origin, *end);
        if (dist < best_dist) {
            best_dist = dist;
            best_axis = axis;
        }
    }
    return best_axis;
}

void Viewer3DWidget::drag_gizmo_to(const QPointF& screen_pos) {
    if (!gizmo_drag_axis_ || !gizmo_drag_start_mouse_) return;
    const int w = std::max(width(), 1);
    const int h = std::max(height(), 1);
    const Mat4 mvp = matmul(projection_matrix(w, h), orbit_view());
    const auto start_screen = project_point(gizmo_drag_start_pos_, mvp);
    const auto end_screen = project_point(
        gizmo_drag_start_pos_ + *gizmo_drag_axis_ * gizmo_drag_scale_, mvp);
    if (!start_screen || !end_screen) return;
    const QPointF axis_vec = *end_screen - *start_screen;
    const double axis_len_sq =
        axis_vec.x() * axis_vec.x() + axis_vec.y() * axis_vec.y();
    if (axis_len_sq < 1e-6) return;
    const QPointF mouse_delta = screen_pos - *gizmo_drag_start_mouse_;
    const double amount = ((mouse_delta.x() * axis_vec.x()
                            + mouse_delta.y() * axis_vec.y())
                           / axis_len_sq)
                          * gizmo_drag_scale_;
    gizmo_pos_ = gizmo_drag_start_pos_ + *gizmo_drag_axis_ * amount;
    emit gizmo_moved(gizmo_pos_[0], gizmo_pos_[1], gizmo_pos_[2]);
    update();
}

void Viewer3DWidget::on_anim_tick() {
    if (!anim_playing_) return;
    anim_time_ += 0.016;
    if (anim_duration_ > 0 && anim_time_ > anim_duration_) anim_time_ = 0.0;
    emit time_updated(anim_time_, anim_duration_);
    if (skin_data_ && (!anim_rot_.empty() || !anim_trans_.empty())) {
        makeCurrent();
        apply_skinning(anim_time_);
        doneCurrent();
    }
    update();
}

// CPU skinning: compute deformed vertices at given time, upload to GPU.
void Viewer3DWidget::apply_skinning(double time) {
    if (rest_jade_.empty() || bone_ibms_.empty()) return;

    const auto& bones = skin_data_->bones;
    const int n_split = int(rest_jade_.size() / 3);

    // Build per-bone skinning matrix: bone_transform @ IBM
    std::vector<Mat4> bone_matrices;
    bone_matrices.reserve(bones.size());
    for (size_t bi = 0; bi < bones.size(); ++bi) {
        const int bidx = bones[bi].bone_idx;
        const Mat4 ibm =
            bi < bone_ibms_.size() ? bone_ibms_[bi] : identity4();

        // Interpolate rotation
        std::optional<std::array<double, 9>> rot3;
        auto rit = anim_rot_.find(bidx);
        if (rit != anim_rot_.end() && !rit->second.empty()) {
            auto q = interp_keyframes(rit->second, time, true);
            if (q) {
                const double qn =
                    std::sqrt((*q)[0] * (*q)[0] + (*q)[1] * (*q)[1]
                              + (*q)[2] * (*q)[2] + (*q)[3] * (*q)[3]);
                const Quat qq{(*q)[0] / qn, (*q)[1] / qn, (*q)[2] / qn,
                              (*q)[3] / qn};
                rot3 = quat_to_mat3(qq);
            }
        }

        // Interpolate translation
        std::optional<std::array<double, 4>> t;
        auto tit = anim_trans_.find(bidx);
        if (tit != anim_trans_.end() && !tit->second.empty())
            t = interp_keyframes(tit->second, time, false);

        Mat4 skin_mat = identity4();
        if (rot3 || t) {
            // Build 4x4 bone transform from animation
            Mat4 bt = identity4();
            if (rot3)
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        bt[size_t(r) * 4 + size_t(c)] =
                            float((*rot3)[size_t(r) * 3 + size_t(c)]);
            if (t)
                for (int r = 0; r < 3; ++r)
                    bt[size_t(r) * 4 + 3] = float((*t)[size_t(r)]);
            // Skinning matrix = bone_transform @ inverse_bind_matrix
            skin_mat = matmul(bt, ibm);
        }
        // else: no animation for this bone — identity (rest pose).
        bone_matrices.push_back(skin_mat);
    }

    // Apply to vertices using per-bone weights.
    std::vector<float> deformed(size_t(n_split) * 3, 0.0f);
    std::vector<float> total_w(size_t(n_split), 0.0f);

    for (size_t bi = 0; bi < bones.size() && bi < bone_matrices.size();
         ++bi) {
        const auto& weights = bones[bi].weights;
        if (weights.empty()) continue;
        const Mat4& mat = bone_matrices[bi];

        for (const auto& [ov, wt] : weights) {
            if (ov < 0 || size_t(ov) >= splits_of_orig_.size()) continue;
            for (int si : splits_of_orig_[size_t(ov)]) {
                const float* rp = &rest_jade_[size_t(si) * 3];
                for (int r = 0; r < 3; ++r) {
                    const float v = mat[size_t(r) * 4 + 0] * rp[0]
                                    + mat[size_t(r) * 4 + 1] * rp[1]
                                    + mat[size_t(r) * 4 + 2] * rp[2]
                                    + mat[size_t(r) * 4 + 3];
                    deformed[size_t(si) * 3 + size_t(r)] += wt * v;
                }
                total_w[size_t(si)] += wt;
            }
        }
    }

    // Normalize and fill unskinned vertices with rest pose
    for (int i = 0; i < n_split; ++i) {
        if (total_w[size_t(i)] > 0) {
            for (int c = 0; c < 3; ++c)
                deformed[size_t(i) * 3 + size_t(c)] /= total_w[size_t(i)];
        } else {
            for (int c = 0; c < 3; ++c)
                deformed[size_t(i) * 3 + size_t(c)] =
                    rest_jade_[size_t(i) * 3 + size_t(c)];
        }
    }

    // Convert to Y-up and upload
    std::vector<float> data(size_t(n_split) * 8, 0.0f);
    for (int i = 0; i < n_split; ++i) {
        jade_to_yup(deformed[size_t(i) * 3], deformed[size_t(i) * 3 + 1],
                    deformed[size_t(i) * 3 + 2], &data[size_t(i) * 8]);
        if (size_t(i) * 3 + 2 < rest_norms_jade_.size())
            jade_to_yup(rest_norms_jade_[size_t(i) * 3],
                        rest_norms_jade_[size_t(i) * 3 + 1],
                        rest_norms_jade_[size_t(i) * 3 + 2],
                        &data[size_t(i) * 8 + 3]);
        data[size_t(i) * 8 + 6] = mesh_uvs_[size_t(i) * 2];
        data[size_t(i) * 8 + 7] = mesh_uvs_[size_t(i) * 2 + 1];
    }

    if (!meshes_.empty()) {
        const GLMesh& gm = meshes_.front();
        glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        GLsizeiptr(data.size() * sizeof(float)),
                        data.data());
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

// ── Viewer3DPanel ──

Viewer3DPanel::Viewer3DPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    viewer_ = new Viewer3DWidget();
    connect(viewer_, &Viewer3DWidget::time_updated, this,
            &Viewer3DPanel::on_time_updated);
    layout->addWidget(viewer_, 1);

    // Toolbar row
    auto* toolbar = new QHBoxLayout();

    tex_check_ = new QCheckBox(tr("Textures"));
    tex_check_->setChecked(true);
    connect(tex_check_, &QCheckBox::toggled, this,
            &Viewer3DPanel::on_tex_toggle);
    toolbar->addWidget(tex_check_);

    grid_check_ = new QCheckBox(tr("Grid"));
    grid_check_->setChecked(true);
    connect(grid_check_, &QCheckBox::toggled, this,
            &Viewer3DPanel::on_grid_toggle);
    toolbar->addWidget(grid_check_);

    collision_check_ = new QCheckBox(tr("Collision"));
    collision_check_->setToolTip(tr(
        "Show ColMap/COB collision geometry as an x-ray wireframe overlay."));
    connect(collision_check_, &QCheckBox::toggled, this,
            &Viewer3DPanel::collision_toggled);
    collision_check_->setVisible(false);
    toolbar->addWidget(collision_check_);

    auto* frame_btn = new QPushButton(tr("Frame"));
    connect(frame_btn, &QPushButton::clicked, this,
            &Viewer3DPanel::on_frame);
    toolbar->addWidget(frame_btn);

    auto* reset_btn = new QPushButton(tr("Reset View"));
    connect(reset_btn, &QPushButton::clicked, this,
            &Viewer3DPanel::on_reset_view);
    toolbar->addWidget(reset_btn);

    auto* home_btn = new QPushButton(tr("Home"));
    connect(home_btn, &QPushButton::clicked, this, &Viewer3DPanel::on_home);
    toolbar->addWidget(home_btn);

    toolbar->addWidget(new QLabel(tr("Animation:")));
    anim_combo_ = new QComboBox();
    anim_combo_->setMinimumWidth(200);
    anim_combo_->addItem(tr("(none)"));
    connect(anim_combo_, &QComboBox::currentIndexChanged, this,
            &Viewer3DPanel::on_anim_combo_changed);
    toolbar->addWidget(anim_combo_);

    info_label_ = new QLabel();
    toolbar->addWidget(info_label_, 1);

    layout->addLayout(toolbar);

    // Playback controls
    auto* ctrl = new QHBoxLayout();
    play_btn_ = new QPushButton(tr("▶ Play"));
    connect(play_btn_, &QPushButton::clicked, this, &Viewer3DPanel::on_play);
    ctrl->addWidget(play_btn_);

    stop_btn_ = new QPushButton(tr("■ Stop"));
    connect(stop_btn_, &QPushButton::clicked, this, &Viewer3DPanel::on_stop);
    ctrl->addWidget(stop_btn_);

    time_slider_ = new QSlider(Qt::Horizontal);
    time_slider_->setRange(0, 1000);
    connect(time_slider_, &QSlider::valueChanged, this,
            &Viewer3DPanel::on_slider);
    ctrl->addWidget(time_slider_, 1);

    time_label_ = new QLabel(QStringLiteral("0.00s"));
    ctrl->addWidget(time_label_);

    layout->addLayout(ctrl);
}

void Viewer3DPanel::set_info(const QString& text) {
    info_label_->setText(text);
}

bool Viewer3DPanel::collision_visible() const {
    return collision_check_->isChecked();
}

void Viewer3DPanel::set_collision_toggle_visible(bool visible) {
    collision_check_->setVisible(visible);
}

// Populate animation dropdown.
void Viewer3DPanel::set_animations(const std::vector<AnimEntry>& anim_list) {
    anim_combo_->blockSignals(true);
    anim_combo_->clear();
    anim_combo_->addItem(tr("(none)"));
    for (const AnimEntry& e : anim_list) anim_combo_->addItem(e.label);
    anim_combo_->blockSignals(false);
    anim_data_ = anim_list;
}

void Viewer3DPanel::on_play() { viewer_->play_animation(); }

void Viewer3DPanel::on_stop() { viewer_->stop_animation(); }

void Viewer3DPanel::on_tex_toggle(bool checked) {
    viewer_->toggle_textures(checked);
}

void Viewer3DPanel::on_grid_toggle(bool checked) {
    viewer_->set_grid_visible(checked);
}

void Viewer3DPanel::on_frame() { viewer_->frame_all(); }

void Viewer3DPanel::on_reset_view() { viewer_->reset_camera(); }

void Viewer3DPanel::on_home() { viewer_->home_camera(); }

void Viewer3DPanel::on_anim_combo_changed(int idx) {
    if (idx <= 0) {
        viewer_->stop_animation();
        viewer_->clear_anim_tracks();
        // Reset to rest pose
        if (viewer_->has_rest_pose()) viewer_->set_anim_time(0);
        return;
    }
    const int anim_idx = idx - 1;
    if (anim_idx < int(anim_data_.size())) {
        const AnimEntry& e = anim_data_[size_t(anim_idx)];
        viewer_->set_anim_tracks(e.tracks, e.duration);
        emit anim_selected(anim_idx);
    }
}

void Viewer3DPanel::on_slider(int value) {
    if (viewer_->anim_duration() > 0) {
        const double t = (value / 1000.0) * viewer_->anim_duration();
        viewer_->set_anim_time(t);
        time_label_->setText(QStringLiteral("%1s").arg(t, 0, 'f', 2));
    }
}

void Viewer3DPanel::on_time_updated(double current, double duration) {
    time_label_->setText(QStringLiteral("%1s").arg(current, 0, 'f', 2));
    if (duration > 0) {
        time_slider_->blockSignals(true);
        time_slider_->setValue(int((current / duration) * 1000));
        time_slider_->blockSignals(false);
    }
}

void Viewer3DPanel::closeEvent(QCloseEvent* event) {
    viewer_->cleanup_gl();
    QWidget::closeEvent(event);
}
