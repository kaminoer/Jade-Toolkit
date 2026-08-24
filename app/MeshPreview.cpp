// MeshPreview.cpp — 3D mesh preview panel (port of gui/mesh_preview.py).
//
// The Python original rendered via PyVista / VTK; this port re-implements
// the same outward panel on a QOpenGLWidget viewport whose GL patterns are
// lifted from Viewer3DWidget (app/Viewer3D.cpp). See MeshPreview.hpp for
// the architectural notes and PORT GAPs.

#include "MeshPreview.hpp"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "Theme.hpp"

using viewer3d::Mat4;
using viewer3d::V3;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// ── Math helpers (lifted from Viewer3D.cpp) ──

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

// Convert Jade Z-up position to Y-up: (x,y,z) → (x,z,-y).
inline void jade_to_yup(float x, float y, float z, float* out) {
    out[0] = x;
    out[1] = z;
    out[2] = -y;
}

// The Python actor shading (ambient=0.35, diffuse=0.75) with the material
// texture, plus a discard of fully-transparent texels — the GL analogue of
// routing cutout textures through the alpha pipeline.
const char* VERT_SHADER = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;

out vec3 vNormal;
out vec2 vUV;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = normalize(aNormal);
    vUV = aUV;
}
)";

const char* FRAG_SHADER = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;

uniform sampler2D uTexture;
uniform bool uHasTexture;
uniform vec4 uBaseColor;
uniform vec3 uLightDir;
uniform bool uUnlit;

out vec4 FragColor;

void main() {
    vec4 color = uBaseColor;
    if (uHasTexture) {
        color = texture(uTexture, vUV) * uBaseColor;
    }
    // Discard only fully-transparent texels — the "floored at 1" cutout
    // trick from _alpha_mode_for (clears black squares without holes).
    if (color.a < 0.004) discard;
    if (uUnlit) {
        FragColor = color;
        return;
    }
    float NdotL = max(dot(normalize(vNormal), uLightDir), 0.0);
    FragColor = vec4(color.rgb * (0.35 + NdotL * 0.75), color.a);
}
)";

}  // namespace

namespace meshpreview {

// _alpha_mode_for: map a material's render flags to a
// (alpha_mode, threshold) pair.
//
// - real (non-Copy) blend → Auto (graded translucency).
// - alpha-test → Cutout at the material's threshold, FLOORED AT 1. The
//   floor is the whole trick: it discards only fully-transparent texels
//   (alpha 0), which clears the black squares an opaque draw shows around
//   cutout textures, while keeping every visible texel — so no holes.
// - opaque → Opaque (alpha ignored, solid).
// - None → Auto heuristic (only when no material was resolved).
std::pair<AlphaMode, int> alpha_mode_for(const ElementRenderMode& rf) {
    if (!rf.ok) return {AlphaMode::Auto, 128};
    if (rf.blend) return {AlphaMode::Auto, 128};
    if (rf.alpha_test)
        return {AlphaMode::Cutout, std::max(rf.alpha_thresh, 1)};
    return {AlphaMode::Opaque, 128};
}

// _pil_to_texture: normalize the image to RGBA and apply the alpha mode.
// Returns the processed image; *translucent tells the caller whether the
// surface must be routed through the translucent draw pass.
QImage process_texture_alpha(const QImage& image, AlphaMode mode,
                             int /*threshold*/, bool* translucent) {
    if (translucent) *translucent = false;
    if (image.isNull()) return QImage();
    QImage arr = image.convertToFormat(QImage::Format_RGBA8888);

    const int w = arr.width(), h = arr.height();
    if (mode == AlphaMode::Opaque) {
        // Force alpha to 255; the surface is solid regardless of what the
        // texture carries (an opaque material whose texture happens to have
        // an alpha channel must NOT go see-through).
        for (int y = 0; y < h; ++y) {
            uint8_t* row = arr.scanLine(y);
            for (int x = 0; x < w; ++x) row[x * 4 + 3] = 255;
        }
        return arr;
    }

    if (mode == AlphaMode::Cutout) {
        // Jade overloads a texture's alpha three ways; decide from the
        // texture itself which it is:
        //  * REAL TRANSPARENCY — the COLORED content is predominantly
        //    opaque (alpha high), so the low-alpha texels are genuine
        //    transparency. Keep the graded alpha so it renders smooth, not
        //    a hard blocky mask (e.g. a WW sword's soft edges).
        //  * BOGUS / LIGHTING — colored content instead sits at LOW alpha
        //    (Jade authors alpha-0 on solid PAL8 geometry; the engine
        //    ignores it) or at a mid band of baked RLI light (DXT5).
        //    Render SOLID, cutting only genuinely EMPTY texels —
        //    near-black AND low-alpha = the transparent background.
        int64_t n_colored = 0, n_colored_hi_a = 0;
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = arr.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                const uint8_t r = row[x * 4], g = row[x * 4 + 1],
                              b = row[x * 4 + 2], a = row[x * 4 + 3];
                const uint8_t rgb_max = std::max({r, g, b});
                if (rgb_max > 32) {
                    ++n_colored;
                    if (a >= 192) ++n_colored_hi_a;
                }
            }
        }
        if (n_colored > 0 && double(n_colored_hi_a) / double(n_colored) > 0.5) {
            bool any_alpha = false;
            for (int y = 0; y < h && !any_alpha; ++y) {
                const uint8_t* row = arr.constScanLine(y);
                for (int x = 0; x < w; ++x)
                    if (row[x * 4 + 3] < 255) { any_alpha = true; break; }
            }
            if (translucent) *translucent = any_alpha;
            return arr;
        }
        bool any_cut = false;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = arr.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const uint8_t r = row[x * 4], g = row[x * 4 + 1],
                              b = row[x * 4 + 2], a = row[x * 4 + 3];
                const bool cut = std::max({r, g, b}) < 32 && a < 128;
                row[x * 4 + 3] = cut ? 0 : 255;
                any_cut = any_cut || cut;
            }
        }
        if (translucent) *translucent = any_cut;
        return arr;
    }

    // Auto: keep the texture's graded alpha (real alpha-blend).
    bool has_alpha = false;
    for (int y = 0; y < h && !has_alpha; ++y) {
        const uint8_t* row = arr.constScanLine(y);
        for (int x = 0; x < w; ++x)
            if (row[x * 4 + 3] < 255) { has_alpha = true; break; }
    }
    if (translucent) *translucent = has_alpha;
    return arr;
}

}  // namespace meshpreview

// ── MeshPreviewViewport ──

MeshPreviewViewport::MeshPreviewViewport(QWidget* parent)
    : QOpenGLWidget(parent) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(4);  // the Python enable_anti_aliasing('msaa')
    setFormat(fmt);
    setFocusPolicy(Qt::ClickFocus);
}

MeshPreviewViewport::~MeshPreviewViewport() { cleanup_gl(); }

void MeshPreviewViewport::load_mesh(
    std::vector<float> verts_jade, std::vector<float> norms_jade,
    std::vector<float> uvs, std::vector<meshpreview::RenderGroup> groups) {
    pending_load_ = std::make_unique<PendingLoad>(PendingLoad{
        std::move(verts_jade), std::move(norms_jade), std::move(uvs),
        std::move(groups)});
    if (gl_initialized_) update();  // paintGL processes the pending load
    // otherwise initializeGL will trigger it
}

void MeshPreviewViewport::set_textures_visible(bool visible) {
    textures_visible_ = visible;
    update();
}

void MeshPreviewViewport::set_wireframe(bool enabled) {
    wireframe_ = enabled;
    update();
}

void MeshPreviewViewport::clear() {
    pending_load_.reset();
    if (gl_initialized_) {
        makeCurrent();
        clear_gl_objects();
        doneCurrent();
    }
    update();
}

void MeshPreviewViewport::cleanup_gl() {
    if (!gl_initialized_) return;
    makeCurrent();
    clear_gl_objects();
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    doneCurrent();
    gl_initialized_ = false;
}

void MeshPreviewViewport::initializeGL() {
    initializeOpenGLFunctions();
    // Background: the Python set_background('#1e1e1e'), adapted to the
    // theme palette.
    const QColor bg(theme::BASE_BG);
    glClearColor(float(bg.redF()), float(bg.greenF()), float(bg.blueF()),
                 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

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
            std::fprintf(stderr, "MeshPreview shader compile error: %s\n",
                         log);
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
            std::fprintf(stderr, "MeshPreview program link error: %s\n", log);
        } else {
            program_ = prog;
        }
        glDeleteShader(vs);
        glDeleteShader(fs);
    }
    gl_initialized_ = true;
    if (pending_load_) process_pending_load();
}

void MeshPreviewViewport::resizeGL(int w, int h) { glViewport(0, 0, w, h); }

// Upload pending mesh data to GPU. Must be called with GL context active.
void MeshPreviewViewport::process_pending_load() {
    if (!pending_load_) return;
    PendingLoad load = std::move(*pending_load_);
    pending_load_.reset();

    clear_gl_objects();

    const int n = int(load.verts_jade.size() / 3);
    num_verts_ = n;
    if (n == 0) {
        auto_frame();
        return;
    }
    load.norms_jade.resize(size_t(n) * 3, 0.0f);
    load.uvs.resize(size_t(n) * 2, 0.0f);

    // Interleaved data: pos(3)+normal(3)+uv(2), converted to Y-up.
    std::vector<float> data(size_t(n) * 8, 0.0f);
    V3 mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
    for (int i = 0; i < n; ++i) {
        float* dst = &data[size_t(i) * 8];
        jade_to_yup(load.verts_jade[size_t(i) * 3],
                    load.verts_jade[size_t(i) * 3 + 1],
                    load.verts_jade[size_t(i) * 3 + 2], dst);
        jade_to_yup(load.norms_jade[size_t(i) * 3],
                    load.norms_jade[size_t(i) * 3 + 1],
                    load.norms_jade[size_t(i) * 3 + 2], dst + 3);
        dst[6] = load.uvs[size_t(i) * 2];
        dst[7] = load.uvs[size_t(i) * 2 + 1];
        for (int c = 0; c < 3; ++c) {
            mn[size_t(c)] = std::min(mn[size_t(c)], double(dst[c]));
            mx[size_t(c)] = std::max(mx[size_t(c)], double(dst[c]));
        }
    }

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
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
    glBindVertexArray(0);

    for (const meshpreview::RenderGroup& rg : load.groups) {
        // Drop faces with out-of-range indices (the numpy valid_faces
        // filter of the old viewer).
        std::vector<uint32_t> indices;
        indices.reserve(rg.indices.size());
        for (size_t f = 0; f + 2 < rg.indices.size(); f += 3) {
            const uint32_t a = rg.indices[f], b = rg.indices[f + 1],
                           c = rg.indices[f + 2];
            if (a < uint32_t(n) && b < uint32_t(n) && c < uint32_t(n)) {
                indices.push_back(a);
                indices.push_back(b);
                indices.push_back(c);
            }
        }
        if (indices.empty()) continue;
        GLGroup g;
        g.num_indices = int(indices.size());
        g.translucent = rg.translucent;
        glGenBuffers(1, &g.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     GLsizeiptr(indices.size() * sizeof(uint32_t)),
                     indices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        if (!rg.texture.isNull()) g.texture_id = upload_texture(rg.texture);
        // Translucent groups keep CPU indices + per-triangle centroids
        // (Y-up) so the translucent pass can depth-sort back-to-front —
        // the plotter-side enable_depth_peeling(8) equivalent.
        if (g.translucent) {
            g.centroids.reserve(indices.size());
            for (size_t f = 0; f + 2 < indices.size(); f += 3) {
                const float* p0 = &data[size_t(indices[f]) * 8];
                const float* p1 = &data[size_t(indices[f + 1]) * 8];
                const float* p2 = &data[size_t(indices[f + 2]) * 8];
                for (int c = 0; c < 3; ++c)
                    g.centroids.push_back((p0[c] + p1[c] + p2[c])
                                          * (1.0f / 3.0f));
            }
            g.cpu_indices = std::move(indices);
        }
        groups_.push_back(std::move(g));
    }

    // Camera: reset to fit bounds, then a stable isometric view — does the
    // right thing regardless of mesh scale (small bone-local Prince geos vs
    // huge level meshes).
    if (mn[0] <= mx[0]) {
        const V3 center = (mn + mx) * 0.5;
        const double radius = std::max(norm(mx - mn) * 0.5, 0.25);
        cam_target_ = center;
        scene_radius_ = std::max(radius, 0.5);
        cam_yaw_ = 45.0;
        cam_pitch_ = 30.0;
        cam_dist_ = std::max(
            radius / std::tan(45.0 * M_PI / 180.0 * 0.5) * 1.15, 0.5);
    } else {
        auto_frame();
    }
}

void MeshPreviewViewport::clear_gl_objects() {
    for (GLGroup& g : groups_) {
        if (g.ebo) glDeleteBuffers(1, &g.ebo);
        if (g.texture_id) glDeleteTextures(1, &g.texture_id);
    }
    groups_.clear();
    if (sort_ebo_) {
        glDeleteBuffers(1, &sort_ebo_);
        sort_ebo_ = 0;
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    num_verts_ = 0;
}

unsigned MeshPreviewViewport::upload_texture(const QImage& img) {
    // VERTICAL FLIP is load-bearing: the Python viewer wraps the image in
    // pv.Texture, and pyvista's _from_array flips the array vertically
    // (np.flip(..., axis=1) after swapaxes) so the image's TOP row lands at
    // texture v=1. The split UVs are authored against that orientation
    // (v' = 1 - v_jade), so GL must upload bottom-row-first to sample the
    // same texels — without this every texture renders upside down.
    const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888)
                            .mirrored(false, true);
    unsigned tid = 0;
    glGenTextures(1, &tid);
    glBindTexture(GL_TEXTURE_2D, tid);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    // NEAREST, NO MIPMAPS — vtkTexture defaults to InterpolateOff with no
    // mipmapping, and this is load-bearing for cutout textures: their
    // binarized alpha averages toward 0 in deeper mip levels, so a
    // mipmapped sampler drops below the discard threshold at glancing
    // angles and whole triangles vanish as the mesh rotates.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return tid;
}

void MeshPreviewViewport::auto_frame() {
    cam_target_ = {0.0, 0.0, 0.0};
    cam_dist_ = 5.0;
    scene_radius_ = 5.0;
    cam_yaw_ = 45.0;
    cam_pitch_ = 30.0;
}

Mat4 MeshPreviewViewport::projection_matrix(int w, int h) const {
    const double aspect = std::max(double(w), 1.0) / std::max(double(h), 1.0);
    const double radius = std::max(scene_radius_, 0.5);
    const double near_p = std::max(0.02, std::min(50.0, cam_dist_ / 1000.0));
    double far_p = std::max({near_p + 100.0, cam_dist_ + radius * 4.0,
                             radius * 8.0});
    if (far_p <= near_p) far_p = near_p + 1000.0;
    return perspective(45.0, aspect, near_p, far_p);
}

Mat4 MeshPreviewViewport::orbit_view() const {
    V3 eye, forward, right, up;
    camera_basis(eye, forward, right, up);
    return look_at(eye, cam_target_, {0.0, 1.0, 0.0});
}

void MeshPreviewViewport::camera_basis(V3& eye, V3& forward, V3& right,
                                       V3& up) const {
    const double yaw_r = cam_yaw_ * M_PI / 180.0;
    const double pitch_r = cam_pitch_ * M_PI / 180.0;
    eye = cam_target_
          + V3{std::cos(pitch_r) * std::sin(yaw_r), std::sin(pitch_r),
               std::cos(pitch_r) * std::cos(yaw_r)}
                * cam_dist_;
    forward = cam_target_ - eye;
    const double fl = norm(forward);
    forward = fl < 1e-8 ? V3{0.0, 0.0, -1.0} : forward * (1.0 / fl);
    const V3 world_up{0.0, 1.0, 0.0};
    right = cross(forward, world_up);
    const double rl = norm(right);
    right = rl < 1e-8 ? V3{1.0, 0.0, 0.0} : right * (1.0 / rl);
    up = cross(right, forward);
    const double ul = norm(up);
    up = ul < 1e-8 ? world_up : up * (1.0 / ul);
}

void MeshPreviewViewport::paintGL() {
    if (pending_load_) process_pending_load();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!program_ || !vao_ || groups_.empty()) return;

    glUseProgram(program_);
    const int w = std::max(width(), 1);
    const int h = std::max(height(), 1);
    const Mat4 mvp = matmul(projection_matrix(w, h), orbit_view());

    const int loc_mvp = glGetUniformLocation(program_, "uMVP");
    const int loc_tex = glGetUniformLocation(program_, "uTexture");
    const int loc_has_tex = glGetUniformLocation(program_, "uHasTexture");
    const int loc_base = glGetUniformLocation(program_, "uBaseColor");
    const int loc_light = glGetUniformLocation(program_, "uLightDir");
    const int loc_unlit = glGetUniformLocation(program_, "uUnlit");

    glUniformMatrix4fv(loc_mvp, 1, GL_TRUE, mvp.data());
    const float light_dir[3] = {0.5f, 0.8f, 0.6f};
    const float ll = std::sqrt(light_dir[0] * light_dir[0]
                               + light_dir[1] * light_dir[1]
                               + light_dir[2] * light_dir[2]);
    glUniform3f(loc_light, light_dir[0] / ll, light_dir[1] / ll,
                light_dir[2] / ll);
    glUniform1i(loc_tex, 0);
    glUniform1i(loc_unlit, 0);

    glBindVertexArray(vao_);

    // Untextured gray: the Python color='#bdbdbd'.
    auto set_group_state = [&](const GLGroup& g, bool with_texture) {
        if (with_texture && g.texture_id && textures_visible_) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g.texture_id);
            glUniform1i(loc_has_tex, 1);
            glUniform4f(loc_base, 1.0f, 1.0f, 1.0f, 1.0f);
        } else {
            glUniform1i(loc_has_tex, 0);
            glUniform4f(loc_base, 0.741f, 0.741f, 0.741f, 1.0f);
        }
    };
    auto bind_group = [&](const GLGroup& g, bool with_texture) {
        set_group_state(g, with_texture);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
        glDrawElements(GL_TRIANGLES, g.num_indices, GL_UNSIGNED_INT,
                       nullptr);
    };

    // Fill passes get a small polygon offset so the wireframe overlay
    // doesn't z-fight (the Python show_edges edge rendering).
    if (wireframe_) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
    }

    // Pass 1: opaque groups (or everything when textures are hidden — an
    // untextured gray surface is always opaque).
    for (const GLGroup& g : groups_)
        if (!g.translucent || !textures_visible_) bind_group(g, true);

    // Pass 2: translucent geometry last, with depth writes off and every
    // translucent TRIANGLE (across all groups) depth-sorted back-to-front
    // per frame. This is the visual equivalent of the Python plotter's
    // enable_depth_peeling(8): per-pixel alpha layers correctly when one
    // transparent surface covers another (eyelashes over face, decals) —
    // an unsorted blend pass is what made the transparency z-order wrong.
    if (textures_visible_) {
        // Collect (view depth, packed group|tri) for every translucent tri.
        sort_keys_.clear();
        viewer3d::V3 eye, forward, right, up;
        camera_basis(eye, forward, right, up);
        constexpr uint32_t TRI_BITS = 22;  // 4M tris/group, 1024 groups
        for (size_t gi = 0; gi < groups_.size() && gi < 1024; ++gi) {
            const GLGroup& g = groups_[gi];
            if (!g.translucent) continue;
            const size_t n_tris = g.centroids.size() / 3;
            for (size_t t = 0; t < n_tris; ++t) {
                const float* c = &g.centroids[t * 3];
                const double d = (c[0] - eye[0]) * forward[0]
                                 + (c[1] - eye[1]) * forward[1]
                                 + (c[2] - eye[2]) * forward[2];
                sort_keys_.push_back(
                    {float(d), uint32_t((gi << TRI_BITS) | t)});
            }
        }
        if (!sort_keys_.empty()) {
            // Farthest first (back-to-front).
            std::sort(sort_keys_.begin(), sort_keys_.end(),
                      [](const auto& a, const auto& b) {
                          return a.first > b.first;
                      });
            // Emit the sorted index stream, batching consecutive
            // same-group runs into single draws.
            struct Run { size_t gi; size_t first; GLsizei count; };
            std::vector<Run> runs;
            sort_indices_.clear();
            sort_indices_.reserve(sort_keys_.size() * 3);
            for (const auto& [depth, packed] : sort_keys_) {
                (void)depth;
                const size_t gi = packed >> TRI_BITS;
                const size_t t = packed & ((1u << TRI_BITS) - 1);
                if (runs.empty() || runs.back().gi != gi)
                    runs.push_back({gi, sort_indices_.size(), 0});
                const GLGroup& g = groups_[gi];
                sort_indices_.push_back(g.cpu_indices[t * 3]);
                sort_indices_.push_back(g.cpu_indices[t * 3 + 1]);
                sort_indices_.push_back(g.cpu_indices[t * 3 + 2]);
                runs.back().count += 3;
            }
            if (!sort_ebo_) glGenBuffers(1, &sort_ebo_);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sort_ebo_);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         GLsizeiptr(sort_indices_.size() * sizeof(uint32_t)),
                         sort_indices_.data(), GL_STREAM_DRAW);
            glDepthMask(GL_FALSE);
            for (const Run& r : runs) {
                set_group_state(groups_[r.gi], true);
                glDrawElements(
                    GL_TRIANGLES, r.count, GL_UNSIGNED_INT,
                    reinterpret_cast<void*>(r.first * sizeof(uint32_t)));
            }
            glDepthMask(GL_TRUE);
        }
    }

    if (wireframe_) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        // Wireframe overlay: edge_color='#6a6a6a', line_width 1.
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glUniform1i(loc_has_tex, 0);
        glUniform1i(loc_unlit, 1);
        glUniform4f(loc_base, 0.415f, 0.415f, 0.415f, 1.0f);
        for (const GLGroup& g : groups_) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
            glDrawElements(GL_TRIANGLES, g.num_indices, GL_UNSIGNED_INT,
                           nullptr);
        }
        glUniform1i(loc_unlit, 0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

// ── Mouse interaction (the QtInteractor trackball equivalents) ──

void MeshPreviewViewport::mousePressEvent(QMouseEvent* event) {
    setFocus();
    last_mouse_ = event->position();
}

void MeshPreviewViewport::mouseMoveEvent(QMouseEvent* event) {
    if (!last_mouse_) return;
    const double dx = event->position().x() - last_mouse_->x();
    const double dy = event->position().y() - last_mouse_->y();

    if ((event->buttons() & Qt::LeftButton)
        && !(event->modifiers() & Qt::ShiftModifier)) {
        // VTK trackball signs: Rotate() calls Azimuth(-200·dx/size) —
        // drag right moves the camera LEFT around the focal point, so the
        // object follows the drag ("grab the object"). Pitch keeps its
        // sign: VTK's bottom-origin Y flip and its negative
        // delta_elevation cancel against Qt's top-origin dy.
        cam_yaw_ -= dx * 0.45;
        cam_pitch_ = std::clamp(cam_pitch_ + dy * 0.45, -89.0, 89.0);
        update();
    } else if ((event->buttons() & Qt::MiddleButton)
               || ((event->buttons() & Qt::LeftButton)
                   && (event->modifiers() & Qt::ShiftModifier))) {
        const double speed = cam_dist_ * 0.002;
        V3 eye, forward, right, up;
        camera_basis(eye, forward, right, up);
        cam_target_ -= right * (dx * speed);
        cam_target_ += up * (dy * speed);
        update();
    }
    last_mouse_ = event->position();
}

void MeshPreviewViewport::mouseReleaseEvent(QMouseEvent*) {
    last_mouse_.reset();
}

void MeshPreviewViewport::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y();
    const double factor = delta > 0 ? 0.9 : 1.1;
    cam_dist_ = std::max(0.1, cam_dist_ * factor);
    update();
}

// ── MeshPreviewViewerShim ──

void MeshPreviewViewerShim::load_animated_mesh(
    const std::vector<float>& verts, const std::vector<float>& norms,
    const std::vector<float>& uvs, const std::vector<uint32_t>& faces,
    const std::vector<int32_t>& /*orig_idx*/,
    std::shared_ptr<viewer3d::SkinData> skin,
    const std::vector<QImage>& images,
    const std::vector<int32_t>& face_elems,
    const std::vector<QImage>& element_images,
    const std::vector<meshpreview::ElementRenderMode>&
        element_render_modes) {
    const QImage tex = images.empty() ? QImage() : images.front();
    QString info = QStringLiteral("%1 verts, %2 tris")
                       .arg(verts.size() / 3)
                       .arg(faces.size() / 3);
    if (!element_images.empty()) {
        int textured = 0;
        for (const QImage& t : element_images)
            if (!t.isNull()) ++textured;
        info += QStringLiteral(" | %1 textured parts").arg(textured);
    }
    if (skin && !skin->bones.empty())
        info += QStringLiteral(" | %1 bones (rest pose)")
                    .arg(skin->bones.size());
    panel_->show_mesh(verts, norms, uvs, faces, tex, info, face_elems,
                      element_images, element_render_modes);
}

void MeshPreviewViewerShim::set_anim_tracks(
    const std::vector<viewer3d::AnimTrack>& tracks, double max_time) {
    // No skinned playback yet — surface the data in the info bar so the
    // user at least sees what's there.
    QString cur = panel_->info_text();
    const QString extra = QStringLiteral(" | %1 tracks, %2s")
                              .arg(tracks.size())
                              .arg(max_time, 0, 'f', 2);
    QString combined = cur + extra;
    while (combined.startsWith(QLatin1Char(' '))
           || combined.startsWith(QLatin1Char('|')))
        combined.remove(0, 1);
    while (combined.endsWith(QLatin1Char(' '))
           || combined.endsWith(QLatin1Char('|')))
        combined.chop(1);
    panel_->set_info(combined);
}

// ── MeshPreviewPanel ──

MeshPreviewPanel::MeshPreviewPanel(QWidget* parent)
    : QWidget(parent), viewer_shim_(this) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    // Toolbar (toggles that drive re-render via GL state).
    auto* tb = new QHBoxLayout();
    tb->setContentsMargins(6, 2, 6, 0);
    show_tex_ = new QCheckBox(tr("Apply material"));
    show_tex_->setChecked(true);
    connect(show_tex_, &QCheckBox::toggled, this,
            &MeshPreviewPanel::on_toggle);
    tb->addWidget(show_tex_);
    show_wire_ = new QCheckBox(tr("Wireframe"));
    show_wire_->setChecked(false);
    connect(show_wire_, &QCheckBox::toggled, this,
            &MeshPreviewPanel::on_toggle);
    tb->addWidget(show_wire_);
    tb->addStretch();
    info_ = new QLabel(QString());
    info_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    tb->addWidget(info_, 1);
    layout->addLayout(tb);

    // Animation playback row — built once, hidden until a GLB scene with
    // animations is loaded. PORT GAP: the scene loaders are stubs (no
    // vtkGLTFImporter / SceneAnimator backend), so the row stays hidden;
    // the slots keep the Python's guard-out behaviour.
    anim_row_ = new QWidget();
    auto* ar = new QHBoxLayout(anim_row_);
    ar->setContentsMargins(6, 0, 6, 2);
    anim_combo_ = new QComboBox();
    anim_combo_->setMinimumWidth(220);
    connect(anim_combo_, &QComboBox::currentIndexChanged, this,
            &MeshPreviewPanel::on_anim_combo_changed);
    ar->addWidget(new QLabel(tr("Animation:")));
    ar->addWidget(anim_combo_, 1);
    play_btn_ = new QPushButton(QStringLiteral("▶"));
    play_btn_->setFixedWidth(28);
    play_btn_->setToolTip(tr("Play"));
    connect(play_btn_, &QPushButton::clicked, this,
            &MeshPreviewPanel::on_play);
    ar->addWidget(play_btn_);
    stop_btn_ = new QPushButton(QStringLiteral("■"));
    stop_btn_->setFixedWidth(28);
    stop_btn_->setToolTip(tr("Stop / rewind to start"));
    connect(stop_btn_, &QPushButton::clicked, this,
            &MeshPreviewPanel::on_stop);
    ar->addWidget(stop_btn_);
    time_slider_ = new QSlider(Qt::Horizontal);
    time_slider_->setRange(0, 1000);
    connect(time_slider_, &QSlider::sliderMoved, this,
            &MeshPreviewPanel::on_time_slider);
    ar->addWidget(time_slider_, 2);
    time_lbl_ = new QLabel(QStringLiteral("0.00 / 0.00 s"));
    time_lbl_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    ar->addWidget(time_lbl_);
    anim_row_->setVisible(false);
    layout->addWidget(anim_row_);

    viewport_ = new MeshPreviewViewport();
    layout->addWidget(viewport_, 1);

    anim_timer_.setParent(this);
    anim_timer_.setInterval(33);  // ~30 fps
    connect(&anim_timer_, &QTimer::timeout, this,
            &MeshPreviewPanel::on_anim_tick);
}

MeshPreviewPanel::~MeshPreviewPanel() = default;

void MeshPreviewPanel::set_info(const QString& text) {
    info_->setText(text);
}

QString MeshPreviewPanel::info_text() const { return info_->text(); }

// ----- full glTF scene path (multi-mesh + animations) -----

bool MeshPreviewPanel::load_glb_scene(
    const QByteArray& /*glb_bytes_or_path*/,
    const std::vector<double>& /*animation_durations*/,
    int /*initial_animation*/, const QString& /*info_text*/) {
    // PORT GAP: the Python path loads a complete glTF binary through VTK's
    // vtkGLTFImporter (multi-mesh, per-material textures, node animation).
    // No C++ backend exists.
    set_info(tr("not ported yet: full-scene animation preview"));
    return false;
}

bool MeshPreviewPanel::load_animated_scene(const QString& /*info_text*/) {
    // PORT GAP: the Python path builds actors from a scene-assets dict and
    // drives core.animator.SceneAnimator for CPU-skinned playback.
    // core/animator.py has no C++ port.
    set_info(tr("not ported yet: full-scene animation preview"));
    return false;
}

void MeshPreviewPanel::set_animations(
    const std::vector<AnimEntry>& animations) {
    if (animations.empty()) return;
    const QString cur = info_->text();
    const QString extra = QStringLiteral(" | %1 anims (playback TBD)")
                              .arg(animations.size());
    if (!cur.contains(extra)) {
        QString combined = cur + extra;
        while (combined.startsWith(QLatin1Char(' '))
               || combined.startsWith(QLatin1Char('|')))
            combined.remove(0, 1);
        while (combined.endsWith(QLatin1Char(' '))
               || combined.endsWith(QLatin1Char('|')))
            combined.chop(1);
        info_->setText(combined);
    }
}

void MeshPreviewPanel::show_mesh(
    const std::vector<float>& verts, const std::vector<float>& normals,
    const std::vector<float>& uvs, const std::vector<uint32_t>& faces,
    const QImage& texture_image, const QString& info_text,
    const std::vector<int32_t>& face_elems,
    const std::vector<QImage>& element_images,
    const std::vector<meshpreview::ElementRenderMode>&
        element_render_modes) {
    using meshpreview::AlphaMode;
    using meshpreview::ElementRenderMode;
    using meshpreview::RenderGroup;

    const size_t n = verts.size() / 3;
    const size_t n_faces = faces.size() / 3;
    if (n == 0 || n_faces == 0) {
        set_info(tr("Mesh has no vertices or faces"));
        viewport_->clear();
        return;
    }

    // Reject NaN / Inf — a single bad coord makes the camera fit land at
    // infinity and the mesh becomes invisible.
    std::vector<float> verts_ok(verts.begin(), verts.begin() + n * 3);
    for (float& v : verts_ok)
        if (!std::isfinite(v)) v = 0.0f;

    std::vector<float> uvs_ok;
    if (uvs.size() == n * 2) uvs_ok = uvs;

    // Per-vertex normals: use the supplied ones, else compute once on the
    // full mesh (area-weighted face-normal accumulation — the
    // compute_normals fallback). Without valid normals the mesh lights
    // with zeros and comes out background-coloured.
    std::vector<float> norms_ok;
    if (normals.size() == n * 3) {
        norms_ok = normals;
        for (float v : norms_ok)
            if (!std::isfinite(v)) { norms_ok.clear(); break; }
    }
    if (norms_ok.empty()) {
        norms_ok.assign(n * 3, 0.0f);
        for (size_t f = 0; f + 2 < faces.size(); f += 3) {
            const uint32_t a = faces[f], b = faces[f + 1], c = faces[f + 2];
            if (a >= n || b >= n || c >= n) continue;
            const float* p0 = &verts_ok[size_t(a) * 3];
            const float* p1 = &verts_ok[size_t(b) * 3];
            const float* p2 = &verts_ok[size_t(c) * 3];
            const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1],
                                 p1[2] - p0[2]};
            const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1],
                                 p2[2] - p0[2]};
            const float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                                 e1[2] * e2[0] - e1[0] * e2[2],
                                 e1[0] * e2[1] - e1[1] * e2[0]};
            for (uint32_t vi : {a, b, c})
                for (int k = 0; k < 3; ++k)
                    norms_ok[size_t(vi) * 3 + size_t(k)] += fn[k];
        }
        for (size_t i = 0; i < n; ++i) {
            float* nv = &norms_ok[i * 3];
            const float l = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1]
                                      + nv[2] * nv[2]);
            if (l > 1e-12f) {
                nv[0] /= l; nv[1] /= l; nv[2] /= l;
            } else {
                nv[2] = 1.0f;
            }
        }
    }

    // Render groups: split by element when per-element textures are
    // available, else one group. The render flags drive
    // opaque / alpha-test / blend.
    std::vector<RenderGroup> groups;
    if (!face_elems.empty() && !element_images.empty()
        && face_elems.size() == n_faces) {
        std::map<int32_t, std::vector<uint32_t>> buckets;
        for (size_t fi = 0; fi < n_faces; ++fi) {
            auto& idxs = buckets[face_elems[fi]];
            idxs.push_back(faces[fi * 3]);
            idxs.push_back(faces[fi * 3 + 1]);
            idxs.push_back(faces[fi * 3 + 2]);
        }
        for (auto& [ei, idxs] : buckets) {
            const QImage img =
                (ei >= 0 && size_t(ei) < element_images.size())
                    ? element_images[size_t(ei)]
                    : QImage();
            ElementRenderMode rf;
            if (ei >= 0 && size_t(ei) < element_render_modes.size())
                rf = element_render_modes[size_t(ei)];
            // Honour the material's render mode: opaque ignores texture
            // alpha entirely, alpha-test binarises to a hard cutout, a
            // real blend keeps graded translucency. With no flags we fall
            // back to the texture-driven heuristic (Auto).
            const auto [mode, thresh] = meshpreview::alpha_mode_for(rf);
            RenderGroup rg;
            rg.indices = std::move(idxs);
            if (!img.isNull())
                rg.texture = meshpreview::process_texture_alpha(
                    img, mode, thresh, &rg.translucent);
            groups.push_back(std::move(rg));
        }
    } else {
        RenderGroup rg;
        rg.indices.assign(faces.begin(), faces.begin() + n_faces * 3);
        if (!texture_image.isNull())
            rg.texture = meshpreview::process_texture_alpha(
                texture_image, meshpreview::AlphaMode::Auto, 128,
                &rg.translucent);
        groups.push_back(std::move(rg));
    }

    viewport_->set_textures_visible(show_tex_->isChecked());
    viewport_->set_wireframe(show_wire_->isChecked());
    viewport_->load_mesh(std::move(verts_ok), std::move(norms_ok),
                         std::move(uvs_ok), std::move(groups));

    if (!info_text.isEmpty()) set_info(info_text);
}

// Re-render the cached mesh with the current toggle state. The GL viewer
// keeps the uploaded geometry, so the toggles are pure draw-state flips —
// no re-upload needed (the Python re-ran its VTK pipeline here).
void MeshPreviewPanel::on_toggle() {
    viewport_->set_textures_visible(show_tex_->isChecked());
    viewport_->set_wireframe(show_wire_->isChecked());
}

// ----- playback slots (guard-out parity; see PORT GAP note in the ctor) --

void MeshPreviewPanel::on_anim_combo_changed(int idx) {
    if (idx < 0 || size_t(idx) >= anim_durations_.size()) return;
    anim_idx_ = idx;
    anim_time_ = 0.0;
    update_time_label();
    apply_time();
}

void MeshPreviewPanel::on_play() {
    if (anim_idx_ < 0) return;
    // PORT GAP: no importer/animator backend — mirrors the Python's
    // "_importer is None and _animator is None" early return.
    return;
}

void MeshPreviewPanel::on_stop() {
    anim_playing_ = false;
    anim_timer_.stop();
    anim_time_ = 0.0;
    update_time_label();
    time_slider_->blockSignals(true);
    time_slider_->setValue(0);
    time_slider_->blockSignals(false);
    apply_time();
}

void MeshPreviewPanel::on_time_slider(int value) {
    if (anim_idx_ < 0) return;
    const double dur = std::max(0.001, duration());
    anim_time_ = (value / 1000.0) * dur;
    update_time_label();
    apply_time();
}

void MeshPreviewPanel::on_anim_tick() {
    if (!anim_playing_) return;
    anim_time_ += anim_timer_.interval() / 1000.0;
    const double dur = duration();
    if (dur > 0 && anim_time_ > dur) anim_time_ = std::fmod(anim_time_, dur);
    update_time_label();
    if (dur > 0) {
        time_slider_->blockSignals(true);
        time_slider_->setValue(int(anim_time_ / dur * 1000));
        time_slider_->blockSignals(false);
    }
    apply_time();
}

void MeshPreviewPanel::apply_time() {
    // PORT GAP: node-transform animation went through vtkGLTFImporter and
    // skinned deformation through core.animator — neither is ported.
}

double MeshPreviewPanel::duration() const {
    if (anim_idx_ >= 0 && size_t(anim_idx_) < anim_durations_.size())
        return anim_durations_[size_t(anim_idx_)];
    return 0.0;
}

void MeshPreviewPanel::update_time_label() {
    time_lbl_->setText(QStringLiteral("%1 / %2 s")
                           .arg(anim_time_, 0, 'f', 2)
                           .arg(duration(), 0, 'f', 2));
}

void MeshPreviewPanel::closeEvent(QCloseEvent* ev) {
    anim_timer_.stop();
    viewport_->cleanup_gl();
    QWidget::closeEvent(ev);
}
