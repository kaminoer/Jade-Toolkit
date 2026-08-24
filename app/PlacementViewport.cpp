// PlacementViewport.cpp — level-editor viewport for the placement tab
// (port of gui/placement_viewport.py).
//
// The Python module was built on pygfx (WebGPU scene graph); this port
// re-implements the same outward behaviour on QOpenGLWidget following
// Viewer3DWidget: GL 3.3 core for solid meshes and line buffers,
// QPainter overlays for everything pygfx drew with depth_test=False
// (transform gizmos, selection outline, axes helper, billboarded
// non-visual-object markers).
//
// Deliberate simplifications vs pygfx (documented per site too):
//   - Marker icons are QPainter pictographs instead of WGSL SDF point
//     sprites; they draw over geometry (no depth test) and pick by
//     screen distance.
//   - Orbit mode's target initialises to the scene centre on mode
//     switch (pygfx's OrbitController derives it from internal state).
//   - The scale-gizmo tip cube is a fixed-size screen square.
//   - GPU pick buffer (renderer.get_pick_info) → CPU Möller–Trumbore
//     raycast, like Viewer3DWidget::pick_scene_point.

#include "PlacementViewport.hpp"

#include <QApplication>
#include <QAbstractSpinBox>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using placement::ElementInfo;
using placement::GaoTransform;
using placement::Mat4d;
using placement::MeshDict;
using placement::Quat;
using placement::V3;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// ── Real-time physical key state ─────────────────────────────────────
// Port of the Python GetAsyncKeyState path: reads the physical keyboard
// regardless of focus or drag capture, so WASD keeps moving during an
// RMB look-drag. Non-Windows platforms fall back to the Qt-tracked
// held-set (see WasdFilter).
#ifdef Q_OS_WIN
constexpr int VK_KEY_W = 0x57, VK_KEY_A = 0x41, VK_KEY_S = 0x53,
              VK_KEY_D = 0x44;
constexpr int VK_KEY_Q = 0x51, VK_KEY_E = 0x45;

bool vk_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// True when OUR process owns the foreground window. GetAsyncKeyState is
// global — we must not fly the camera while the user is typing in
// another program.
bool app_is_foreground() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}
#else
bool app_is_foreground() { return true; }
#endif

// ── Math helpers (row-major, numpy layout — same as Viewer3D.cpp) ────

V3 operator+(const V3& a, const V3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
V3 operator-(const V3& a, const V3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V3 operator*(const V3& a, double s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
double dot(const V3& a, const V3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
double norm(const V3& a) { return std::sqrt(dot(a, a)); }

Mat4d matmul(const Mat4d& a, const Mat4d& b) {
    Mat4d out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    return out;
}

Mat4d identity4() {
    Mat4d m{};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    return m;
}

// Invert a general 4x4 (Gauss-Jordan). Returns false if singular.
bool invert4(const Mat4d& in, Mat4d& out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) a[r][c] = in[size_t(r) * 4 + size_t(c)];
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
        for (int c = 0; c < 4; ++c) out[size_t(r) * 4 + size_t(c)] = a[r][4 + c];
    return true;
}

// Quaternion (x, y, z, w) helpers — pylinalg conventions.
Quat quat_from_axis_angle(const V3& axis, double angle) {
    const double n = norm(axis);
    const double s = std::sin(angle * 0.5);
    const double c = std::cos(angle * 0.5);
    if (n < 1e-12) return {0.0, 0.0, 0.0, 1.0};
    return {axis[0] / n * s, axis[1] / n * s, axis[2] / n * s, c};
}

Quat quat_mul(const Quat& a, const Quat& b) {
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}

// Rotate vector v by quaternion q (la.vec_transform_quat).
V3 vec_transform_quat(const V3& v, const Quat& q) {
    const V3 qv{q[0], q[1], q[2]};
    const V3 t = cross(qv, v) * 2.0;
    return v + t * q[3] + cross(qv, t);
}

// Compose T·R·S into a row-major 4x4 (pygfx Group.local.matrix).
Mat4d mat_trs(const V3& pos, const Quat& q, const V3& scale) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double r[9] = {
        1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
        2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
        2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
    Mat4d m = identity4();
    for (int rr = 0; rr < 3; ++rr)
        for (int cc = 0; cc < 3; ++cc)
            m[size_t(rr) * 4 + size_t(cc)] = r[rr * 3 + cc] * scale[size_t(cc)];
    m[3] = pos[0];
    m[7] = pos[1];
    m[11] = pos[2];
    return m;
}

// la.vec_transform: matrix @ [p, 1].
V3 vec_transform(const V3& p, const Mat4d& m) {
    return {
        m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
        m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
        m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11],
    };
}

Mat4d look_at(const V3& eye, const V3& target, const V3& up) {
    V3 f = target - eye;
    const double fl = norm(f);
    if (fl > 1e-12) f = f * (1.0 / fl);
    V3 r = cross(f, up);
    const double rl = norm(r);
    if (rl > 1e-12)
        r = r * (1.0 / rl);
    else
        r = {1.0, 0.0, 0.0};
    const V3 u = cross(r, f);
    Mat4d m = identity4();
    m[0] = r[0]; m[1] = r[1]; m[2] = r[2];
    m[4] = u[0]; m[5] = u[1]; m[6] = u[2];
    m[8] = -f[0]; m[9] = -f[1]; m[10] = -f[2];
    m[3] = -dot(r, eye);
    m[7] = -dot(u, eye);
    m[11] = dot(f, eye);
    return m;
}

Mat4d perspective(double fov_deg, double aspect, double near_p,
                  double far_p) {
    const double f = 1.0 / std::tan(fov_deg * M_PI / 180.0 / 2.0);
    Mat4d m{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far_p + near_p) / (near_p - far_p);
    m[11] = (2 * far_p * near_p) / (near_p - far_p);
    m[14] = -1.0;
    return m;
}

double dist_to_segment_2d(const QPointF& p, const QPointF& a,
                          const QPointF& b) {
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double L2 = dx * dx + dy * dy;
    if (L2 < 1e-8) return std::hypot(p.x() - a.x(), p.y() - a.y());
    const double t = std::clamp(
        ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / L2, 0.0, 1.0);
    return std::hypot(p.x() - (a.x() + t * dx), p.y() - (a.y() + t * dy));
}

// numpy-style linear-interpolation percentile of a value list.
// Sorts in place — callers pass scratch vectors they own.
double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * double(v.size() - 1);
    const size_t lo = size_t(std::floor(idx));
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - double(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch())
        .count();
}

// ── Category metadata (core/object_kinds.py CATEGORY_INFO) ───────────
struct CategoryColor {
    const char* name;
    double r, g, b;
};
const CategoryColor CATEGORY_COLORS[] = {
    {"camera", 0.30, 0.55, 1.00},  {"light", 1.00, 0.85, 0.20},
    {"sound", 0.20, 0.90, 0.90},   {"fx", 1.00, 0.40, 0.90},
    {"trigger", 0.30, 0.90, 0.40}, {"trap", 1.00, 0.25, 0.20},
    {"actor", 1.00, 0.55, 0.10},   {"spawner", 1.00, 0.35, 0.10},
    {"waypoint", 0.55, 1.00, 0.45},{"logic", 0.60, 0.60, 0.65},
    {"other", 0.75, 0.75, 0.78},
};

QColor category_color(const QString& category) {
    for (const CategoryColor& c : CATEGORY_COLORS)
        if (category == QLatin1String(c.name))
            return QColor::fromRgbF(float(c.r), float(c.g), float(c.b));
    return QColor::fromRgbF(0.75f, 0.75f, 0.78f);  // "other"
}

// Axis colours are constant across tools (R=X, G=Y, B=Z) so users can
// build muscle memory.
const QColor AXIS_COLORS[3] = {
    QColor::fromRgbF(1.0f, 0.25f, 0.25f),
    QColor::fromRgbF(0.25f, 1.0f, 0.25f),
    QColor::fromRgbF(0.25f, 0.45f, 1.0f),
};

// ── Shaders ──────────────────────────────────────────────────────────
// Unlit "MeshBasicMaterial" equivalent: texture × baked per-vertex RLI
// × base colour, no real-time lighting (2003-era look).
const char* MESH_VERT = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vUV;
out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

const char* MESH_FRAG = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;

uniform sampler2D uTexture;
uniform bool uUseTexture;
uniform bool uUseVertexColor;
uniform bool uAlphaTest;
uniform vec4 uBaseColor;

out vec4 FragColor;

void main() {
    vec4 color = uBaseColor;
    if (uUseTexture) color *= texture(uTexture, vUV);
    if (uUseVertexColor) color *= vColor;
    if (uAlphaTest && color.a < 0.5) discard;
    FragColor = color;
}
)";

const char* LINE_VERT = R"(
#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";

const char* LINE_FRAG = R"(
#version 330 core
uniform vec4 uColor;
out vec4 FragColor;
void main() { FragColor = uColor; }
)";

// ── UV-seam vertex split (_split_for_uv) ─────────────────────────────
// Jade GEOs index UVs separately from positions: each face has 3 vertex
// indices and 3 UV indices. A vertex shared by faces with different UV
// indices needs duplicating so each instance carries its own UV —
// otherwise the texture lands on the wrong region.
struct SplitResult {
    std::vector<float> verts;    // 3*n
    std::vector<float> norms;    // 3*n
    std::vector<float> uvs;      // 2*n
    std::vector<uint32_t> faces; // 3*m
    std::vector<float> colors;   // 4*n (empty when no RLI supplied)
};

SplitResult split_for_uv(const MeshDict& md) {
    SplitResult out;
    const size_t n_verts = md.vertices.size() / 3;
    const size_t n_faces = md.faces.size() / 3;
    const bool has_norms = md.normals.size() >= n_verts * 3;
    const bool has_cols =
        md.rli_comps > 0 && md.rli_colors.size() >= n_verts * size_t(md.rli_comps);
    auto push_color = [&](size_t vi) {
        if (!has_cols) return;
        const size_t comps = size_t(md.rli_comps);
        if (vi < n_verts) {
            for (size_t c = 0; c < 4; ++c)
                out.colors.push_back(
                    c < comps ? md.rli_colors[vi * comps + c] : 1.0f);
        } else {
            for (int c = 0; c < 4; ++c) out.colors.push_back(1.0f);
        }
    };
    const bool no_face_uvs = md.face_uvs.size() != n_faces * 3
                             || md.uvs.empty();
    if (no_face_uvs) {
        // No per-face UVs available — return as-is with zeroed UVs.
        out.verts = md.vertices;
        if (has_norms) {
            out.norms.assign(md.normals.begin(),
                             md.normals.begin() + long(n_verts * 3));
        } else {
            out.norms.assign(n_verts * 3, 0.0f);
            for (size_t i = 0; i < n_verts; ++i) out.norms[i * 3 + 1] = 1.0f;
        }
        out.uvs.assign(n_verts * 2, 0.0f);
        out.faces.reserve(md.faces.size());
        // Defensive: drop faces with out-of-range indices (would be UB
        // in the GL draw; numpy just carried them).
        for (size_t f = 0; f + 2 < md.faces.size(); f += 3) {
            const int64_t a = md.faces[f], b = md.faces[f + 1],
                          c = md.faces[f + 2];
            if (a >= 0 && size_t(a) < n_verts && b >= 0
                && size_t(b) < n_verts && c >= 0 && size_t(c) < n_verts) {
                out.faces.push_back(uint32_t(a));
                out.faces.push_back(uint32_t(b));
                out.faces.push_back(uint32_t(c));
            }
        }
        if (has_cols)
            for (size_t i = 0; i < n_verts; ++i) push_color(i);
        return out;
    }

    const size_t n_uvs = md.uvs.size() / 2;
    std::unordered_map<uint64_t, uint32_t> cache;
    cache.reserve(n_faces * 2);
    out.faces.resize(n_faces * 3);
    for (size_t fi = 0; fi < n_faces; ++fi) {
        for (int col = 0; col < 3; ++col) {
            const int64_t vi = md.faces[fi * 3 + size_t(col)];
            const int64_t ui = md.face_uvs[fi * 3 + size_t(col)];
            const uint64_t key =
                (uint64_t(uint32_t(vi)) << 32) | uint64_t(uint32_t(ui));
            auto it = cache.find(key);
            uint32_t ix;
            if (it == cache.end()) {
                ix = uint32_t(out.verts.size() / 3);
                cache.emplace(key, ix);
                if (vi >= 0 && size_t(vi) < n_verts) {
                    out.verts.push_back(md.vertices[size_t(vi) * 3]);
                    out.verts.push_back(md.vertices[size_t(vi) * 3 + 1]);
                    out.verts.push_back(md.vertices[size_t(vi) * 3 + 2]);
                } else {
                    out.verts.insert(out.verts.end(), {0.0f, 0.0f, 0.0f});
                }
                if (has_norms && vi >= 0 && size_t(vi) < n_verts) {
                    out.norms.push_back(md.normals[size_t(vi) * 3]);
                    out.norms.push_back(md.normals[size_t(vi) * 3 + 1]);
                    out.norms.push_back(md.normals[size_t(vi) * 3 + 2]);
                } else {
                    out.norms.insert(out.norms.end(), {0.0f, 1.0f, 0.0f});
                }
                if (ui >= 0 && size_t(ui) < n_uvs) {
                    out.uvs.push_back(md.uvs[size_t(ui) * 2]);
                    out.uvs.push_back(md.uvs[size_t(ui) * 2 + 1]);
                } else {
                    out.uvs.insert(out.uvs.end(), {0.0f, 0.0f});
                }
                push_color(vi >= 0 ? size_t(vi) : n_verts);
            } else {
                ix = it->second;
            }
            out.faces[fi * 3 + size_t(col)] = ix;
        }
    }
    return out;
}

// Cumulative nTri → [(face_start, face_end), ...] index ranges. One
// range covering all faces when there are no elements. Clamps the final
// range if element nTri sums disagree with the actual face count.
std::vector<std::pair<int, int>> element_face_ranges(
    const std::vector<ElementInfo>& elements, int total_faces) {
    std::vector<std::pair<int, int>> ranges;
    if (elements.empty()) {
        if (total_faces) ranges.emplace_back(0, total_faces);
        return ranges;
    }
    int cursor = 0;
    for (const ElementInfo& el : elements) {
        const int n = std::max(0, el.nTri);
        const int start = cursor;
        const int end = std::min(total_faces, cursor + n);
        ranges.emplace_back(start, end);
        cursor = end;
        if (cursor >= total_faces) break;
    }
    return ranges;
}

}  // namespace

// GL texture wrapper — deletion is queued on the owning canvas so
// shared_ptr owners (aliased texture lists) can drop it anywhere.
struct PlacementCanvas::GlTexture {
    unsigned id = 0;
    PlacementCanvas* owner = nullptr;
    ~GlTexture() {
        if (owner && id) owner->pending_tex_delete_.push_back(id);
    }
};

// ── WasdFilter ───────────────────────────────────────────────────────
// App-level event filter that tracks held WASD/QE/Space/Shift keys, so
// WASD works right after a zone load (before the canvas is clicked).
// On Windows the movement axes read GetAsyncKeyState instead; this
// held-set is the non-Windows fallback (and mirrors the Python filter).
class WasdFilter : public QObject {
public:
    explicit WasdFilter(PlacementCanvas* canvas)
        : QObject(canvas), canvas_(canvas) {}

    QSet<int> held;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        const QEvent::Type et = event->type();
        if (et == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (!ke->isAutoRepeat()) held.insert(ke->key());
        } else if (et == QEvent::KeyRelease) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (!ke->isAutoRepeat()) held.remove(ke->key());
        } else if (et == QEvent::WindowDeactivate) {
            // Alt-tabbing away mid-press would otherwise leave a sticky
            // key flying the camera off-world.
            held.clear();
        }
        Q_UNUSED(obj);
        return false;  // never consume keys
    }

private:
    PlacementCanvas* canvas_;
};

// ── PlacementCanvas ──────────────────────────────────────────────────

PlacementCanvas::PlacementCanvas(PlacementViewport* panel, QWidget* parent)
    : QOpenGLWidget(parent), panel_(panel) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(4);
    setFormat(fmt);

    setMinimumHeight(280);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);  // _last_cursor_xy for Place Object

    // On-screen FPS counter — floats above the render surface.
    fps_label_ = new QLabel(QString(), this);
    fps_label_->setStyleSheet(QStringLiteral(
        "color: #c8ffc8; background: rgba(0,0,0,140); "
        "padding: 2px 6px; font-family: 'Consolas','Menlo',monospace; "
        "font-size: 11px;"));
    fps_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    fps_label_->hide();
    fps_label_->adjustSize();

    // Collision legend — maps each wireframe colour to the GAO/COB it
    // came from so the user can tell which host owns which collision
    // mesh. Populated by rebuild_collision_legend.
    collision_legend_label_ = new QLabel(QString(), this);
    collision_legend_label_->setStyleSheet(QStringLiteral(
        "color: #f0f0f0; background: rgba(0,0,0,170); "
        "padding: 6px 10px; font-family: 'Consolas','Menlo',monospace; "
        "font-size: 11px; border: 1px solid rgba(255,255,255,40);"));
    collision_legend_label_->setTextFormat(Qt::RichText);
    collision_legend_label_->setAttribute(Qt::WA_TransparentForMouseEvents,
                                          true);
    collision_legend_label_->hide();
    collision_legend_label_->adjustSize();

    fps_sample_timer_.setParent(this);
    fps_sample_timer_.setInterval(500);
    connect(&fps_sample_timer_, &QTimer::timeout, this,
            &PlacementCanvas::sample_fps);

    // WASD is driven by a ~60 Hz integration tick, independent of the
    // render loop; forward/right come from the camera's current
    // orientation, up is world-up so flight stays level.
    wasd_timer_.setParent(this);
    wasd_timer_.setInterval(16);
    connect(&wasd_timer_, &QTimer::timeout, this,
            &PlacementCanvas::tick_wasd);
    wasd_timer_.start();

    show_pos({0.0, 0.0, 0.0});
}

PlacementCanvas::~PlacementCanvas() { cleanup_gl(); }

// -- Camera basics --

V3 PlacementCanvas::cam_forward() const {
    const double cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
    const double cy = std::cos(cam_yaw_), sy = std::sin(cam_yaw_);
    return {-cp * sy, sp, -cp * cy};
}

V3 PlacementCanvas::cam_right() const {
    const double cy = std::cos(cam_yaw_), sy = std::sin(cam_yaw_);
    return {cy, 0.0, -sy};
}

// camera.show_pos(target): orient the camera to look at a world point.
void PlacementCanvas::show_pos(const V3& target) {
    V3 f = target - cam_pos_;
    const double fl = norm(f);
    if (fl < 1e-9) return;
    f = f * (1.0 / fl);
    cam_pitch_ = std::asin(std::clamp(f[1], -1.0, 1.0));
    cam_yaw_ = std::atan2(-f[0], -f[2]);
}

Mat4d PlacementCanvas::proj_view() const {
    const double aspect =
        std::max(double(width()), 1.0) / std::max(double(height()), 1.0);
    // PerspectiveCamera(50, …, depth_range=(0.01, 10000.0))
    const Mat4d proj = perspective(50.0, aspect, 0.01, 10000.0);
    const Mat4d view =
        look_at(cam_pos_, cam_pos_ + cam_forward(), {0.0, 1.0, 0.0});
    return matmul(proj, view);
}

// Scale Fly / WASD speed to the loaded scene — large outdoor zones need
// orders of magnitude more units/sec than a single GAO.
double PlacementCanvas::fly_speed_for_scene() const {
    return std::max(2.0, scene_radius_ * 0.6);
}

// View-distance-adaptive WASD speed, recomputed each tick: proportional
// to how far the camera is from the scene focus — floored by a fraction
// of the scene radius, capped so flying far out doesn't teleport.
double PlacementCanvas::current_fly_speed() const {
    const double dist = norm(cam_pos_ - scene_center_);
    const double base = std::max(dist, 0.35 * scene_radius_);
    return std::clamp(base * 0.8 * user_speed_mult, 1.0, 8000.0);
}

// True when the mouse cursor is over the (visible) viewport canvas —
// the Unity/Unreal model: WASD flies while you're looking at the view.
bool PlacementCanvas::cursor_over_canvas() const {
    if (!isVisible()) return false;
    return rect().contains(mapFromGlobal(QCursor::pos()));
}

// True when a text / number entry has keyboard focus, so WASD keys
// meant for that field don't also fly the camera.
static bool text_input_focused() {
    QWidget* fw = QApplication::focusWidget();
    return qobject_cast<QLineEdit*>(fw) != nullptr
           || qobject_cast<QAbstractSpinBox*>(fw) != nullptr
           || qobject_cast<QPlainTextEdit*>(fw) != nullptr;
}

void PlacementCanvas::tick_wasd() {
    const double now = now_seconds();
    if (!wasd_last_t_) {
        wasd_last_t_ = now;
        return;
    }
    const double dt = std::clamp(now - *wasd_last_t_, 0.0, 0.1);
    wasd_last_t_ = now;

    // Movement axes from the live keyboard state (physical keys on
    // Windows — works during an RMB look-drag; Qt held-set elsewhere).
    double dz = 0.0, dx = 0.0, dy = 0.0;
    bool fast = false;
    bool rmb_down = rmb_look_;
#ifdef Q_OS_WIN
    dz = (vk_down(VK_KEY_W) ? 1.0 : 0.0) - (vk_down(VK_KEY_S) ? 1.0 : 0.0);
    dx = (vk_down(VK_KEY_D) ? 1.0 : 0.0) - (vk_down(VK_KEY_A) ? 1.0 : 0.0);
    dy = ((vk_down(VK_SPACE) || vk_down(VK_KEY_E)) ? 1.0 : 0.0)
         - (vk_down(VK_KEY_Q) ? 1.0 : 0.0);
    fast = vk_down(VK_SHIFT);
    rmb_down = vk_down(VK_RBUTTON);
#else
    auto* filter = static_cast<WasdFilter*>(panel_->wasd_filter_);
    if (filter) {
        const QSet<int>& held = filter->held;
        dz = (held.contains(Qt::Key_W) ? 1.0 : 0.0)
             - (held.contains(Qt::Key_S) ? 1.0 : 0.0);
        dx = (held.contains(Qt::Key_D) ? 1.0 : 0.0)
             - (held.contains(Qt::Key_A) ? 1.0 : 0.0);
        dy = ((held.contains(Qt::Key_Space) || held.contains(Qt::Key_E))
                  ? 1.0 : 0.0)
             - (held.contains(Qt::Key_Q) ? 1.0 : 0.0);
        fast = held.contains(Qt::Key_Shift);
    }
#endif
    const bool keys_held = (dz != 0.0 || dx != 0.0 || dy != 0.0);
    const bool over = cursor_over_canvas();
    const bool fg = app_is_foreground();

    // Gate (in priority order). RMB held = the user is looking at OUR
    // viewport → always allow movement.
    const char* reason = nullptr;
    if (text_input_focused())
        reason = "text-field";
    else if (!rmb_down && !fg)
        reason = "not-foreground";
    else if (!rmb_down && !over)
        reason = "cursor-off-view";
    else if (!keys_held)
        reason = "no-keys";

    // On-screen diagnostic (toggle with wasd_debug_). Shows live in the
    // viewport info bar whenever movement keys are held.
    if (wasd_debug_ && keys_held && (now - wasd_dbg_t_) > 0.25) {
        wasd_dbg_t_ = now;
        panel_->set_info(QString::asprintf(
            "[wasd] %s  rmb=%d over=%d fg=%d  axes=(%+.0f,%+.0f,%+.0f)  "
            "pos=(%.0f,%.0f,%.0f)",
            reason ? reason : "MOVING", rmb_down ? 1 : 0, over ? 1 : 0,
            fg ? 1 : 0, dz, dx, dy, cam_pos_[0], cam_pos_[1], cam_pos_[2]));
    }

    if (reason) return;

    const double speed = current_fly_speed() * (fast ? 4.0 : 1.0);
    const V3 forward = cam_forward();
    const V3 right = cam_right();
    const V3 up{0.0, 1.0, 0.0};
    const V3 delta = forward * dz + right * dx + up * dy;
    const double step = speed * dt;
    cam_pos_ = cam_pos_ + delta * step;
    // Keep orbit target in sync so a subsequent mouse-orbit pivots
    // around where the user just flew to instead of snapping back.
    if (cam_mode_ == QLatin1String("orbit"))
        orbit_target_ = orbit_target_ + delta * step;
    update();
}

// -- Scene load / clear --

void PlacementCanvas::load_meshes(const std::vector<MeshDict>& mesh_list,
                                  const placement::TexturesByKey& textures,
                                  bool preserve_camera) {
    if (!gl_initialized_) {
        // GL not up yet — defer to the first paintGL (Viewer3D's
        // pending-load pattern), where the context is current.
        pending_scene_ = std::make_unique<PendingScene>(
            PendingScene{mesh_list, textures, preserve_camera});
        update();
        return;
    }
    makeCurrent();
    load_scene(mesh_list, textures, preserve_camera);
    doneCurrent();
    update();
}

// Scene-rebuild body — requires a current GL context.
void PlacementCanvas::load_scene(const std::vector<MeshDict>& mesh_list,
                                 const placement::TexturesByKey& textures,
                                 bool preserve_camera) {
    // Selection survives the rebuild: if the previously-selected GAO
    // key reappears in the new mesh list we re-select it.
    const long long prev_selection = selected_gao_;
    clear_meshes();
    // Cache per-element textures by gao_key. Each entry is
    // (texture | null, alpha_class) aligned with the GAO's elements.
    textures_by_key_.clear();
    for (const auto& [gk, per_el] : textures) {
        std::vector<TexEntry> tex_list;
        bool any = false;
        for (const QImage& img : per_el) {
            if (img.isNull()) {
                tex_list.push_back(TexEntry{});
            } else {
                tex_list.push_back(make_texture(img));
                if (tex_list.back().tex) any = true;
            }
        }
        if (any) textures_by_key_[gk] = std::move(tex_list);
    }

    for (const MeshDict& md : mesh_list) add_scene_object(md);

    recompute_bounds();
    if (!preserve_camera) auto_frame();
    if (prev_selection >= 0
        && gao_groups_.count(uint32_t(prev_selection)))
        select_gao(prev_selection);
    if (show_collision_) {
        rebuild_collision_legend();
        position_collision_legend();
    }
}

void PlacementCanvas::set_point_picking(bool enabled, double plane_y) {
    point_picking_ = enabled;
    pick_plane_y_ = plane_y;
}

// Register the source GAO's per-element texture list under the
// destination key (clone preview: synthetic key reuses the source's
// geometry + materials). Shared by reference — entries are never
// mutated after creation.
bool PlacementCanvas::alias_textures(uint32_t dest_gao_key,
                                     uint32_t source_gao_key) {
    auto it = textures_by_key_.find(source_gao_key);
    if (it == textures_by_key_.end()) return false;
    textures_by_key_[dest_gao_key] = it->second;
    return true;
}

// Live-swap a GAO's per-element textures and rebuild the materials on
// its existing meshes — the inspector's material picker path.
void PlacementCanvas::set_gao_textures(
    uint32_t gao_key, const std::vector<QImage>& rgba_per_element) {
    if (!gl_initialized_) return;
    makeCurrent();
    std::vector<TexEntry> tex_list;
    bool any = false;
    for (const QImage& img : rgba_per_element) {
        if (img.isNull()) {
            tex_list.push_back(TexEntry{});
        } else {
            tex_list.push_back(make_texture(img));
            if (tex_list.back().tex) any = true;
        }
    }
    if (any) {
        textures_by_key_[gao_key] = tex_list;
    } else {
        // No usable textures — fall back to flat base_color.
        textures_by_key_.erase(gao_key);
        tex_list.clear();
    }
    for (SolidMesh& sm : solid_meshes_) {
        if (sm.gao_key < 0 || uint32_t(sm.gao_key) != gao_key) continue;
        const size_t el = size_t(sm.element_index);
        sm.texture = (el < tex_list.size()) ? tex_list[el] : TexEntry{};
    }
    drain_texture_deletes();
    doneCurrent();
    update();
}

// Swap the meshes for a single GAO without touching the rest of the
// scene — live-edit feedback path (full load_meshes rebuilds cost
// seconds on an 842-mesh hall zone).
void PlacementCanvas::replace_object_meshes(long long gao_key,
                                            std::vector<MeshDict> mesh_dicts) {
    if (!gl_initialized_) return;
    const uint32_t key = uint32_t(uint64_t(gao_key) & 0xFFFFFFFFull);
    makeCurrent();
    // Drop the old group + bookkeeping.
    gao_groups_.erase(key);
    gao_bounds_.erase(key);
    markers_.erase(std::remove_if(markers_.begin(), markers_.end(),
                                  [&](const Marker& m) {
                                      return m.gao_key >= 0
                                             && uint32_t(m.gao_key) == key;
                                  }),
                   markers_.end());
    for (auto it = solid_meshes_.begin(); it != solid_meshes_.end();) {
        if (it->gao_key >= 0 && uint32_t(it->gao_key) == key) {
            free_solid_mesh(*it);
            it = solid_meshes_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = collision_lines_.begin(); it != collision_lines_.end();) {
        if (it->gao_key >= 0 && uint32_t(it->gao_key) == key) {
            free_collision_line(*it);
            it = collision_lines_.erase(it);
        } else {
            ++it;
        }
    }

    for (MeshDict& md : mesh_dicts) {
        if (md.gao_key < 0) md.gao_key = (long long)key;
        add_scene_object(md);
    }
    drain_texture_deletes();
    doneCurrent();

    if (show_collision_) rebuild_collision_legend();
    // If this GAO was selected, re-position the gizmo to the new bounds.
    if (selected_gao_ >= 0 && uint32_t(selected_gao_) == key)
        reposition_gizmo_to_selection();
    update();
}

PlacementCanvas::GroupXform& PlacementCanvas::ensure_group(uint32_t key) {
    return gao_groups_[key];  // default-constructs identity transform
}

void PlacementCanvas::add_scene_object(const MeshDict& md) {
    if (md.is_marker)
        add_marker(md);
    else
        add_mesh(md);
}

// Render one non-visual object as a category icon. Markers stay out of
// the solid-mesh bookkeeping but share the GAO group transform so
// click-select + gizmo-move work exactly like a visual object.
void PlacementCanvas::add_marker(const MeshDict& md) {
    Marker mk;
    mk.local_pos = md.position;
    mk.category = md.marker_category.isEmpty() ? QStringLiteral("other")
                                               : md.marker_category;
    mk.gao_key = md.gao_key;
    markers_.push_back(mk);
    if (md.gao_key < 0) return;  // loose draft preview: render-only
    const uint32_t key = uint32_t(uint64_t(md.gao_key) & 0xFFFFFFFFull);
    ensure_group(key);
    // A small world-space box around the point so the selection gizmo
    // has a sane size/position (markers carry no real geometry bounds).
    const V3 half{1.0, 1.0, 1.0};
    gao_bounds_[key] = {md.position - half, md.position + half};
}

void PlacementCanvas::add_mesh(const MeshDict& md) {
    if (md.vertices.empty()) return;
    SplitResult sr = split_for_uv(md);
    const size_t n = sr.verts.size() / 3;
    if (n == 0) return;
    const long long gao_key = md.gao_key;
    const uint32_t key32 =
        gao_key >= 0 ? uint32_t(uint64_t(gao_key) & 0xFFFFFFFFull) : 0;

    if (md.wireframe) {
        // Collision overlay: draw as line segments, x-ray when toggled.
        if (sr.faces.empty()) return;
        std::vector<float> segs;
        segs.reserve(sr.faces.size() * 6);
        for (size_t f = 0; f + 2 < sr.faces.size(); f += 3) {
            const uint32_t idx[3] = {sr.faces[f], sr.faces[f + 1],
                                     sr.faces[f + 2]};
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = idx[e], b = idx[(e + 1) % 3];
                segs.insert(segs.end(), {sr.verts[a * 3], sr.verts[a * 3 + 1],
                                         sr.verts[a * 3 + 2]});
                segs.insert(segs.end(), {sr.verts[b * 3], sr.verts[b * 3 + 1],
                                         sr.verts[b * 3 + 2]});
            }
        }
        CollisionLine cl;
        cl.name = md.name;
        cl.gao_key = gao_key;
        cl.color = md.base_color;
        cl.line_width = md.line_width > 0.0f ? md.line_width : 1.6f;
        cl.xray = md.xray;
        cl.count = int(segs.size() / 3);
        glGenVertexArrays(1, &cl.vao);
        glBindVertexArray(cl.vao);
        glGenBuffers(1, &cl.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, cl.vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(segs.size() * sizeof(float)),
                     segs.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * 4, nullptr);
        glBindVertexArray(0);
        // Parent under the GAO's group when there is one so the
        // wireframe inherits gizmo edits.
        if (gao_key >= 0) ensure_group(key32);
        collision_lines_.push_back(std::move(cl));
        return;
    }

    // Per-element split: Jade multi-mats route each element to its own
    // sub-material/texture via matId. One draw per element with its own
    // diffuse map is what makes textures land on the right surfaces.
    const std::vector<TexEntry>* tex_list = nullptr;
    if (gao_key >= 0) {
        auto it = textures_by_key_.find(key32);
        if (it != textures_by_key_.end()) tex_list = &it->second;
    }
    const int total_faces = int(sr.faces.size() / 3);
    const auto face_ranges = element_face_ranges(md.elements, total_faces);
    const bool has_rli = !sr.colors.empty();

    // One interleaved vertex buffer shared across every element of this
    // GAO — pos(3) + uv(2) + color(4).
    std::vector<float> data(n * 9, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        data[i * 9 + 0] = sr.verts[i * 3 + 0];
        data[i * 9 + 1] = sr.verts[i * 3 + 1];
        data[i * 9 + 2] = sr.verts[i * 3 + 2];
        data[i * 9 + 3] = sr.uvs[i * 2 + 0];
        data[i * 9 + 4] = sr.uvs[i * 2 + 1];
        if (has_rli) {
            data[i * 9 + 5] = sr.colors[i * 4 + 0];
            data[i * 9 + 6] = sr.colors[i * 4 + 1];
            data[i * 9 + 7] = sr.colors[i * 4 + 2];
            data[i * 9 + 8] = sr.colors[i * 4 + 3];
        } else {
            data[i * 9 + 5] = data[i * 9 + 6] = data[i * 9 + 7] =
                data[i * 9 + 8] = 1.0f;
        }
    }
    unsigned vbo_id = 0;
    glGenBuffers(1, &vbo_id);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.size() * sizeof(float)),
                 data.data(), GL_STATIC_DRAW);
    // The GL buffer itself is freed by free_solid_mesh when the last
    // element-mesh owning this shared_ptr is torn down.
    auto shared_vbo = std::shared_ptr<unsigned>(new unsigned(vbo_id));

    // Per-GAO group: all element-meshes share it so click-to-move can
    // translate the whole object by writing the group's transform.
    if (gao_key >= 0) ensure_group(key32);

    auto verts_shared =
        std::make_shared<std::vector<float>>(std::move(sr.verts));
    auto norms_shared =
        std::make_shared<std::vector<float>>(std::move(sr.norms));

    for (size_t el_idx = 0; el_idx < face_ranges.size(); ++el_idx) {
        const auto [start, end] = face_ranges[el_idx];
        if (end <= start) continue;
        std::vector<uint32_t> sub_faces(
            sr.faces.begin() + long(start) * 3,
            sr.faces.begin() + long(end) * 3);
        SolidMesh sm;
        sm.name = md.name
                  + (face_ranges.size() > 1
                         ? QStringLiteral("#el%1").arg(el_idx)
                         : QString());
        sm.gao_key = gao_key;
        sm.element_index = int(el_idx);
        sm.base_color = md.base_color;
        sm.texture = (tex_list && el_idx < tex_list->size())
                         ? (*tex_list)[el_idx]
                         : TexEntry{};
        sm.has_rli = has_rli;
        sm.pickable = md.pickable;
        sm.depth_offset = md.depth_offset;
        sm.vertices = verts_shared;
        sm.normals = norms_shared;
        sm.num_indices = int(sub_faces.size());
        sm.shared_vbo = shared_vbo;

        glGenVertexArrays(1, &sm.vao);
        glBindVertexArray(sm.vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
        const int stride = 9 * 4;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(12));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(20));
        glGenBuffers(1, &sm.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sm.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     GLsizeiptr(sub_faces.size() * sizeof(uint32_t)),
                     sub_faces.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        sm.faces = std::move(sub_faces);
        solid_meshes_.push_back(std::move(sm));
    }

    // Cache bounds for outline + framing.
    if (!verts_shared->empty() && gao_key >= 0) {
        V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        const std::vector<float>& v = *verts_shared;
        for (size_t i = 0; i + 2 < v.size(); i += 3)
            for (int c = 0; c < 3; ++c) {
                lo[size_t(c)] = std::min(lo[size_t(c)], double(v[i + size_t(c)]));
                hi[size_t(c)] = std::max(hi[size_t(c)], double(v[i + size_t(c)]));
            }
        gao_bounds_[key32] = {lo, hi};
    }
}

// Wrap a decoded RGBA image as a GL texture + its alpha class.
// Jade's DXT5 diffuse textures sometimes ship "all zero" alpha; forcing
// those opaque ('broken') is the only way to see the texture at all.
PlacementCanvas::TexEntry PlacementCanvas::make_texture(const QImage& img) {
    TexEntry entry;
    if (img.isNull()) return entry;
    QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    // _classify_alpha: 'opaque' | 'cutout' | 'blend' | 'broken'.
    const uchar* bits = rgba.constBits();
    const qsizetype stride = rgba.bytesPerLine();
    const int w = rgba.width(), h = rgba.height();
    qint64 n = qint64(w) * h;
    qint64 c255 = 0, c0 = 0, sum = 0;
    for (int y = 0; y < h; ++y) {
        const uchar* row = bits + qsizetype(y) * stride;
        for (int x = 0; x < w; ++x) {
            const uchar a = row[x * 4 + 3];
            if (a == 255) ++c255;
            if (a == 0) ++c0;
            sum += a;
        }
    }
    QString alpha_class = QStringLiteral("opaque");
    if (n > 0) {
        const double r255 = double(c255) / double(n);
        const double r0 = double(c0) / double(n);
        const double mean = double(sum) / double(n);
        if (r255 > 0.99)
            alpha_class = QStringLiteral("opaque");
        else if (r0 > 0.99 || mean < 5)
            alpha_class = QStringLiteral("broken");
        else if (r0 + r255 > 0.85)
            alpha_class = QStringLiteral("cutout");
        else
            alpha_class = QStringLiteral("blend");
    }
    if (alpha_class == QLatin1String("broken")) {
        // Overwrite alpha to 255 so the surface is at least visible.
        uchar* wbits = rgba.bits();
        for (int y = 0; y < h; ++y) {
            uchar* row = wbits + qsizetype(y) * rgba.bytesPerLine();
            for (int x = 0; x < w; ++x) row[x * 4 + 3] = 255;
        }
        alpha_class = QStringLiteral("opaque");
    }

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

    auto gt = std::make_shared<GlTexture>();
    gt->id = tid;
    gt->owner = this;
    entry.tex = gt;
    entry.alpha_class = alpha_class == QLatin1String("cutout")
                            ? AlphaClass::Cutout
                            : (alpha_class == QLatin1String("blend")
                                   ? AlphaClass::Blend
                                   : AlphaClass::Opaque);
    return entry;
}

void PlacementCanvas::free_solid_mesh(SolidMesh& sm) {
    if (sm.vao) glDeleteVertexArrays(1, &sm.vao);
    if (sm.ebo) glDeleteBuffers(1, &sm.ebo);
    if (sm.normals_vao) glDeleteVertexArrays(1, &sm.normals_vao);
    if (sm.normals_vbo) glDeleteBuffers(1, &sm.normals_vbo);
    sm.vao = sm.ebo = sm.normals_vao = sm.normals_vbo = 0;
    // The shared vertex buffer is freed when its last element drops it.
    if (sm.shared_vbo && sm.shared_vbo.use_count() == 1 && *sm.shared_vbo)
        glDeleteBuffers(1, sm.shared_vbo.get());
    sm.shared_vbo.reset();
    sm.texture = TexEntry{};
}

void PlacementCanvas::free_collision_line(CollisionLine& cl) {
    if (cl.vao) glDeleteVertexArrays(1, &cl.vao);
    if (cl.vbo) glDeleteBuffers(1, &cl.vbo);
    cl.vao = cl.vbo = 0;
}

void PlacementCanvas::drain_texture_deletes() {
    for (unsigned id : pending_tex_delete_) glDeleteTextures(1, &id);
    pending_tex_delete_.clear();
}

// Assumes a current GL context.
void PlacementCanvas::clear_meshes() {
    for (SolidMesh& sm : solid_meshes_) free_solid_mesh(sm);
    solid_meshes_.clear();
    for (CollisionLine& cl : collision_lines_) free_collision_line(cl);
    collision_lines_.clear();
    markers_.clear();
    gao_groups_.clear();
    gao_bounds_.clear();
    selected_gao_ = -1;
    drain_texture_deletes();
}

// Overlays (wireframe / normals) draw straight off the mesh buffers —
// nothing to rebuild; the toggle handlers just repaint.
void PlacementCanvas::rebuild_overlays() { update(); }

// -- Transform API --

// Return a sensible gizmo size for a target at world_pos: constant
// screen size (dist·0.15) grown to the selection's longest half-extent
// ×1.25 (folding in the group's accumulated scale) so every arm sticks
// out past the mesh's furthest face.
double PlacementCanvas::gizmo_world_scale(const V3& world_pos,
                                          long long gao_key) const {
    const double dist = norm(cam_pos_ - world_pos);
    double scale = std::max(0.5, dist * 0.15);
    if (gao_key >= 0) {
        const uint32_t gk = uint32_t(uint64_t(gao_key) & 0xFFFFFFFFull);
        auto bit = gao_bounds_.find(gk);
        if (bit != gao_bounds_.end()) {
            const auto& [lo, hi] = bit->second;
            double extents[3];
            for (int i = 0; i < 3; ++i)
                extents[i] = std::abs(hi[size_t(i)] - lo[size_t(i)]);
            auto git = gao_groups_.find(gk);
            if (git != gao_groups_.end())
                for (int i = 0; i < 3; ++i)
                    extents[i] *= std::abs(git->second.scale[size_t(i)]);
            const double max_half =
                std::max({extents[0], extents[1], extents[2]}) * 0.5;
            scale = std::max(scale, max_half * 1.25);
        }
    }
    return scale;
}

// Best-effort world-space spawn point for a new object:
//   1. selected GAO's bounds centre → 2. surface pick under cursor →
//   3. surface pick at canvas centre → 4. focus_point() fallback.
V3 PlacementCanvas::focus_point_under_cursor() {
    if (selected_gao_ >= 0) {
        const uint32_t gk = uint32_t(selected_gao_);
        auto bit = gao_bounds_.find(gk);
        auto git = gao_groups_.find(gk);
        if (bit != gao_bounds_.end() && git != gao_groups_.end()) {
            const V3 centre_local =
                (bit->second.first + bit->second.second) * 0.5;
            const Mat4d m = mat_trs(git->second.position,
                                    git->second.rotation, git->second.scale);
            const V3 cw = vec_transform(centre_local, m);
            if (std::isfinite(cw[0]) && std::isfinite(cw[1])
                && std::isfinite(cw[2]))
                return cw;
        }
    }
    if (last_cursor_xy_) {
        auto hit = pick_world_hit(last_cursor_xy_->x(), last_cursor_xy_->y());
        if (hit) return *hit;
    }
    // Centre-of-canvas pick — what the camera is actually looking at.
    auto hit = pick_world_hit(double(width()) * 0.5, double(height()) * 0.5);
    if (hit) return *hit;
    return focus_point();
}

// Spawn point when nothing pickable is on screen: orbit target when set,
// else the camera position pushed forward a fraction of the scene radius.
V3 PlacementCanvas::focus_point() {
    if (cam_mode_ == QLatin1String("orbit") && orbit_target_set_) {
        const V3& t = orbit_target_;
        if (std::abs(t[0]) > 1e-6 || std::abs(t[1]) > 1e-6
            || std::abs(t[2]) > 1e-6)
            return t;
    }
    const double dist = std::max(2.0, scene_radius_ * 0.25);
    return cam_pos_ + cam_forward() * dist;
}

// Spawn point for the geometry under (sx, sy), or nullopt. The hit mesh
// resolves back to its GAO; ITS bounds centre is the spawn point —
// "spawn where I clicked" == "spawn on the thing I clicked".
std::optional<V3> PlacementCanvas::pick_world_hit(double sx, double sy) {
    const long long gao_key = raycast_gao(sx, sy);
    if (gao_key < 0) return std::nullopt;
    const uint32_t gk = uint32_t(gao_key);
    auto bit = gao_bounds_.find(gk);
    auto git = gao_groups_.find(gk);
    if (bit == gao_bounds_.end() || git == gao_groups_.end())
        return std::nullopt;
    const V3 centre_local = (bit->second.first + bit->second.second) * 0.5;
    const Mat4d m = mat_trs(git->second.position, git->second.rotation,
                            git->second.scale);
    const V3 cw = vec_transform(centre_local, m);
    if (!std::isfinite(cw[0]) || !std::isfinite(cw[1])
        || !std::isfinite(cw[2]))
        return std::nullopt;
    return cw;
}

// Return (position_offset, rotation_quat, scale) — position is the
// translation of the mesh's bounds centre from its load position, NOT
// the literal group position (which is a pivot-correction artefact).
std::optional<GaoTransform> PlacementCanvas::get_gao_transform(
    uint32_t gao_key) {
    auto git = gao_groups_.find(gao_key);
    if (git == gao_groups_.end()) return std::nullopt;
    GaoTransform out;
    out.rotation = git->second.rotation;
    out.scale = git->second.scale;
    auto bit = gao_bounds_.find(gao_key);
    if (bit == gao_bounds_.end()) {
        out.position = git->second.position;
        return out;
    }
    const V3 pivot_local = (bit->second.first + bit->second.second) * 0.5;
    const Mat4d m = mat_trs(git->second.position, git->second.rotation,
                            git->second.scale);
    out.position = vec_transform(pivot_local, m) - pivot_local;
    return out;
}

// Drive a GAO's group transform from the Inspector. Rotation and scale
// pivot around the mesh centre:
//   group.position = pivot_target − R·(S⊙pivot_local)
void PlacementCanvas::set_gao_transform(
    uint32_t gao_key, const std::optional<V3>& position,
    const std::optional<Quat>& rotation_quat,
    const std::optional<V3>& scale) {
    auto git = gao_groups_.find(gao_key);
    if (git == gao_groups_.end()) return;
    GroupXform& grp = git->second;

    V3 p{0.0, 0.0, 0.0};
    if (position) {
        p = *position;
    } else {
        auto cur = get_gao_transform(gao_key);
        if (cur) p = cur->position;
    }
    const Quat q = rotation_quat ? *rotation_quat : grp.rotation;
    const V3 s = scale ? *scale : grp.scale;

    grp.rotation = q;
    grp.scale = s;
    auto bit = gao_bounds_.find(gao_key);
    if (bit != gao_bounds_.end()) {
        const V3 pivot_local = (bit->second.first + bit->second.second) * 0.5;
        const V3 scaled{pivot_local[0] * s[0], pivot_local[1] * s[1],
                        pivot_local[2] * s[2]};
        const V3 rotated = vec_transform_quat(scaled, q);
        grp.position = pivot_local + p - rotated;
    } else {
        grp.position = p;
    }

    if (selected_gao_ >= 0 && uint32_t(selected_gao_) == gao_key)
        reposition_gizmo_to_selection();
    update();
}

// Re-centre the gizmo on the selected GAO's current world position.
void PlacementCanvas::reposition_gizmo_to_selection() {
    if (selected_gao_ < 0) return;
    const uint32_t gk = uint32_t(selected_gao_);
    auto bit = gao_bounds_.find(gk);
    auto git = gao_groups_.find(gk);
    if (bit == gao_bounds_.end() || git == gao_groups_.end()) return;
    const V3 center_local = (bit->second.first + bit->second.second) * 0.5;
    const Mat4d m = mat_trs(git->second.position, git->second.rotation,
                            git->second.scale);
    gizmo_pos_ = vec_transform(center_local, m);
}

void PlacementCanvas::set_transform_gizmo(
    bool enabled, const std::optional<V3>& position) {
    gizmo_visible_ = enabled;
    if (position) gizmo_pos_ = *position;
    update();
}

// -- Selection --

// Set the current selection (-1 clears). Shows the active tool's gizmo
// at the GAO's current world-space bounds centre (post-transform).
void PlacementCanvas::select_gao(long long gao_key) {
    if (gao_key < 0) {
        selected_gao_ = -1;
        gizmo_visible_ = false;
        emit object_selected(-1);
        update();
        return;
    }
    const uint32_t key = uint32_t(uint64_t(gao_key) & 0xFFFFFFFFull);
    selected_gao_ = (long long)key;
    auto bit = gao_bounds_.find(key);
    auto git = gao_groups_.find(key);
    if (bit == gao_bounds_.end() || git == gao_groups_.end()) {
        selected_gao_ = -1;
        gizmo_visible_ = false;
        emit object_selected(-1);
        update();
        return;
    }
    // Place gizmo at the mesh's current world-space bounds centre —
    // through the group's FULL matrix, not just its translation (the
    // pivot-corrected translation alone drops it "under the map" after
    // a rotate/scale edit).
    reposition_gizmo_to_selection();
    gizmo_visible_ = true;
    emit object_selected(qlonglong(key));
    update();
}

// Click-pick: select the GAO under the cursor (or deselect on empty
// space). Markers pick first (they draw over the geometry), then a CPU
// raycast over the solid meshes.
void PlacementCanvas::handle_selection(double sx, double sy) {
    long long gao_key = -1;
    // Marker hit: screen distance to the projected icon centre.
    double best_marker = 13.0;  // half the 26-px icon
    for (const Marker& mk : markers_) {
        if (mk.gao_key < 0) continue;
        const uint32_t gk = uint32_t(mk.gao_key);
        auto git = gao_groups_.find(gk);
        V3 world = mk.local_pos;
        if (git != gao_groups_.end())
            world = vec_transform(mk.local_pos,
                                  mat_trs(git->second.position,
                                          git->second.rotation,
                                          git->second.scale));
        const auto scr = world_to_screen(world);
        if (!scr) continue;
        const double d = std::hypot(scr->x() - sx, scr->y() - sy);
        if (d < best_marker) {
            best_marker = d;
            gao_key = mk.gao_key;
        }
    }
    if (gao_key < 0) gao_key = raycast_gao(sx, sy);
    select_gao(gao_key);
}

// CPU Möller–Trumbore raycast over every solid mesh (group transforms
// applied); returns the nearest hit's gao_key or -1.
long long PlacementCanvas::raycast_gao(double sx, double sy) {
    V3 origin, direction;
    if (!ray_from_screen(sx, sy, origin, direction)) return -1;
    long long best_key = -1;
    double best_dist = 0.0;
    bool have_best = false;
    for (const SolidMesh& sm : solid_meshes_) {
        if (!sm.vertices || sm.faces.empty()) continue;
        // Transform the ray into mesh-local space.
        V3 lo = origin, ld = direction;
        if (sm.gao_key >= 0) {
            auto git = gao_groups_.find(uint32_t(sm.gao_key));
            if (git != gao_groups_.end()) {
                const Mat4d m = mat_trs(git->second.position,
                                        git->second.rotation,
                                        git->second.scale);
                Mat4d inv;
                if (!invert4(m, inv)) continue;
                const V3 p0 = vec_transform(origin, inv);
                const V3 p1 = vec_transform(origin + direction, inv);
                lo = p0;
                ld = p1 - p0;  // unnormalised: t maps back linearly
            }
        }
        const std::vector<float>& verts = *sm.vertices;
        for (size_t f = 0; f + 2 < sm.faces.size(); f += 3) {
            const float* p0 = &verts[size_t(sm.faces[f]) * 3];
            const float* p1 = &verts[size_t(sm.faces[f + 1]) * 3];
            const float* p2 = &verts[size_t(sm.faces[f + 2]) * 3];
            const V3 v0{p0[0], p0[1], p0[2]};
            const V3 v1{p1[0], p1[1], p1[2]};
            const V3 v2{p2[0], p2[1], p2[2]};
            const V3 edge1 = v1 - v0;
            const V3 edge2 = v2 - v0;
            const V3 h = cross(ld, edge2);
            const double a = dot(edge1, h);
            if (std::abs(a) <= 1e-12) continue;
            const double fi = 1.0 / a;
            const V3 s = lo - v0;
            const double u = fi * dot(s, h);
            if (u < 0.0 || u > 1.0) continue;
            const V3 q = cross(s, edge1);
            const double v = fi * dot(ld, q);
            if (v < 0.0 || u + v > 1.0) continue;
            const double t = fi * dot(edge2, q);
            if (t <= 1e-9) continue;
            // World-space distance so hits compare across groups with
            // different scales.
            V3 hit_local = lo + ld * t;
            V3 hit_world = hit_local;
            if (sm.gao_key >= 0) {
                auto git = gao_groups_.find(uint32_t(sm.gao_key));
                if (git != gao_groups_.end())
                    hit_world = vec_transform(
                        hit_local, mat_trs(git->second.position,
                                           git->second.rotation,
                                           git->second.scale));
            }
            const double d = norm(hit_world - origin);
            if (!have_best || d < best_dist) {
                have_best = true;
                best_dist = d;
                best_key = sm.gao_key;
            }
        }
    }
    return have_best ? best_key : -1;
}

// -- Picking rays --

bool PlacementCanvas::ray_from_screen(double sx, double sy, V3& origin,
                                      V3& direction) const {
    const double w = std::max(width(), 1);
    const double h = std::max(height(), 1);
    const double ndc_x = (sx / w) * 2.0 - 1.0;
    const double ndc_y = 1.0 - (sy / h) * 2.0;
    Mat4d inv;
    if (!invert4(proj_view(), inv)) return false;
    auto unproject = [&inv, ndc_x, ndc_y](double z, V3& out) -> bool {
        double v[4];
        for (int r = 0; r < 4; ++r)
            v[r] = inv[size_t(r) * 4 + 0] * ndc_x
                   + inv[size_t(r) * 4 + 1] * ndc_y
                   + inv[size_t(r) * 4 + 2] * z + inv[size_t(r) * 4 + 3];
        if (std::abs(v[3]) < 1e-12) return false;
        out = {v[0] / v[3], v[1] / v[3], v[2] / v[3]};
        return true;
    };
    V3 near_p, far_p;
    if (!unproject(-1.0, near_p) || !unproject(1.0, far_p)) return false;
    V3 dir = far_p - near_p;
    const double n = norm(dir);
    if (n < 1e-8) return false;
    origin = near_p;
    direction = dir * (1.0 / n);
    return true;
}

std::optional<V3> PlacementCanvas::ray_to_plane(double sx, double sy,
                                                double plane_y) const {
    V3 origin, direction;
    if (!ray_from_screen(sx, sy, origin, direction)) return std::nullopt;
    if (std::abs(direction[1]) < 1e-8) return std::nullopt;
    const double t = (plane_y - origin[1]) / direction[1];
    if (t < 0) return std::nullopt;
    return origin + direction * t;
}

std::optional<QPointF> PlacementCanvas::world_to_screen(const V3& p) const {
    const Mat4d pv = proj_view();
    double clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = pv[size_t(r) * 4 + 0] * p[0] + pv[size_t(r) * 4 + 1] * p[1]
                  + pv[size_t(r) * 4 + 2] * p[2] + pv[size_t(r) * 4 + 3];
    if (clip[3] <= 1e-6) return std::nullopt;
    const double nx = clip[0] / clip[3];
    const double ny = clip[1] / clip[3];
    const double nz = clip[2] / clip[3];
    if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz)
        || nz < -1.0 || nz > 1.0)
        return std::nullopt;
    return QPointF((nx + 1.0) * 0.5 * std::max(width(), 1),
                   (1.0 - ny) * 0.5 * std::max(height(), 1));
}

// -- Gizmo hit tests / drag math --

std::optional<int> PlacementCanvas::tool_axis_at(double sx, double sy) const {
    if (active_tool_ == QLatin1String("rotate")) return ring_axis_at(sx, sy);
    return gizmo_axis_at(sx, sy);  // move + scale share the axis-line test
}

// Hit-test the three gizmo axes in screen space (14 px tolerance).
std::optional<int> PlacementCanvas::gizmo_axis_at(double sx, double sy) const {
    if (!gizmo_visible_) return std::nullopt;
    const double scale = gizmo_world_scale(gizmo_pos_, selected_gao_);
    const auto o_scr = world_to_screen(gizmo_pos_);
    if (!o_scr) return std::nullopt;
    std::optional<int> best;
    double best_dist = 14.0;
    for (int axis = 0; axis < 3; ++axis) {
        V3 end = gizmo_pos_;
        end[size_t(axis)] += scale;
        const auto e_scr = world_to_screen(end);
        if (!e_scr) continue;
        const double d = dist_to_segment_2d(QPointF(sx, sy), *o_scr, *e_scr);
        if (d < best_dist) {
            best_dist = d;
            best = axis;
        }
    }
    return best;
}

// Pick the rotate ring closest to the mouse: intersect the picking ray
// with each ring's plane; the hit must land near the ring radius.
std::optional<int> PlacementCanvas::ring_axis_at(double sx, double sy) const {
    V3 origin, direction;
    if (!ray_from_screen(sx, sy, origin, direction)) return std::nullopt;
    const double radius = gizmo_world_scale(gizmo_pos_, selected_gao_);
    const double tol = radius * 0.10;
    std::optional<int> best;
    double best_err = tol;
    for (int axis = 0; axis < 3; ++axis) {
        V3 n{0.0, 0.0, 0.0};
        n[size_t(axis)] = 1.0;
        const double denom = dot(direction, n);
        if (std::abs(denom) < 1e-6) continue;
        const double t = dot(gizmo_pos_ - origin, n) / denom;
        if (t < 0) continue;
        const V3 hit = origin + direction * t;
        const double err = std::abs(norm(hit - gizmo_pos_) - radius);
        if (err < best_err) {
            best_err = err;
            best = axis;
        }
    }
    return best;
}

// Angle (radians) of the picking-ray's intersection with the ring's
// plane, measured CCW from the ring's local +X axis.
std::optional<double> PlacementCanvas::mouse_angle_on_ring(int axis, double sx,
                                                           double sy) const {
    V3 origin, direction;
    if (!ray_from_screen(sx, sy, origin, direction)) return std::nullopt;
    V3 n{0.0, 0.0, 0.0};
    n[size_t(axis)] = 1.0;
    const double denom = dot(direction, n);
    if (std::abs(denom) < 1e-6) return std::nullopt;
    const double t = dot(gizmo_pos_ - origin, n) / denom;
    if (t < 0) return std::nullopt;
    const V3 local = origin + direction * t - gizmo_pos_;
    const int ax2 = (axis + 1) % 3;
    const int ax3 = (axis + 2) % 3;
    return std::atan2(local[size_t(ax3)], local[size_t(ax2)]);
}

// Signed distance from gizmo origin along the picked axis, by
// projecting the picking ray onto the axis line.
std::optional<double> PlacementCanvas::mouse_axis_distance(int axis, double sx,
                                                           double sy) const {
    V3 origin, direction;
    if (!ray_from_screen(sx, sy, origin, direction)) return std::nullopt;
    V3 d1{0.0, 0.0, 0.0};
    d1[size_t(axis)] = 1.0;
    const V3 n = cross(d1, direction);
    const double denom = dot(n, n);
    if (denom < 1e-12) return std::nullopt;
    return dot(cross(origin - gizmo_pos_, direction), n) / denom;
}

// Capture pre-drag transform state and per-tool start values.
void PlacementCanvas::begin_tool_drag(int axis, double sx, double sy) {
    gizmo_drag_axis_ = axis;
    gizmo_drag_start_world_ = gizmo_pos_;
    gizmo_drag_start_mouse_ = QPointF(sx, sy);
    gizmo_drag_target_ = selected_gao_;
    if (selected_gao_ >= 0) {
        auto git = gao_groups_.find(uint32_t(selected_gao_));
        if (git != gao_groups_.end()) {
            drag_start_rot_ = git->second.rotation;
            drag_start_scale_ = git->second.scale;
        }
    }
    if (active_tool_ == QLatin1String("rotate"))
        drag_start_angle_ = mouse_angle_on_ring(axis, sx, sy);
    else if (active_tool_ == QLatin1String("scale"))
        drag_start_axis_dist_ = mouse_axis_distance(axis, sx, sy);
}

// Closest-point between the picking ray and the axis line at the
// gizmo's drag-start position.
std::optional<V3> PlacementCanvas::drag_gizmo_to(double sx, double sy) const {
    if (!gizmo_drag_axis_) return std::nullopt;
    V3 origin, direction;
    if (!ray_from_screen(sx, sy, origin, direction)) return std::nullopt;
    const V3& p1 = gizmo_drag_start_world_;
    V3 d1{0.0, 0.0, 0.0};
    d1[size_t(*gizmo_drag_axis_)] = 1.0;
    const V3 n = cross(d1, direction);
    const double denom = dot(n, n);
    if (denom < 1e-12) return std::nullopt;
    const double t = dot(cross(origin - p1, direction), n) / denom;
    return p1 + d1 * t;
}

void PlacementCanvas::drag_move(double sx, double sy) {
    const auto new_pos = drag_gizmo_to(sx, sy);
    if (!new_pos) return;
    const V3 delta = *new_pos - gizmo_pos_;
    gizmo_pos_ = *new_pos;
    if (gizmo_drag_target_ >= 0) {
        auto git = gao_groups_.find(uint32_t(gizmo_drag_target_));
        if (git != gao_groups_.end()) {
            git->second.position = git->second.position + delta;
            // Emit the mesh-centre offset (NOT the literal group
            // position — meaningless after a rotate-pivot adjustment).
            const auto offset =
                gao_centre_offset(uint32_t(gizmo_drag_target_));
            if (offset)
                emit object_moved(qlonglong(gizmo_drag_target_),
                                  (*offset)[0], (*offset)[1], (*offset)[2]);
            emit object_transformed(qlonglong(gizmo_drag_target_));
        }
    } else {
        // Placement-draft gizmo path (no GAO selected)
        emit gizmo_moved((*new_pos)[0], (*new_pos)[1], (*new_pos)[2]);
    }
    update();
}

// Translation of the GAO's mesh centre from its load position — same
// value get_gao_transform returns as position.
std::optional<V3> PlacementCanvas::gao_centre_offset(uint32_t gao_key) const {
    auto git = gao_groups_.find(gao_key);
    auto bit = gao_bounds_.find(gao_key);
    if (git == gao_groups_.end() || bit == gao_bounds_.end())
        return std::nullopt;
    const V3 pivot_local = (bit->second.first + bit->second.second) * 0.5;
    const Mat4d m = mat_trs(git->second.position, git->second.rotation,
                            git->second.scale);
    return vec_transform(pivot_local, m) - pivot_local;
}

void PlacementCanvas::drag_rotate(double sx, double sy) {
    if (!gizmo_drag_axis_ || !drag_start_angle_) return;
    const auto ang_now = mouse_angle_on_ring(*gizmo_drag_axis_, sx, sy);
    if (!ang_now) return;
    const double delta = *ang_now - *drag_start_angle_;
    V3 axis_vec{0.0, 0.0, 0.0};
    axis_vec[size_t(*gizmo_drag_axis_)] = 1.0;
    const Quat q_delta = quat_from_axis_angle(axis_vec, delta);
    if (gizmo_drag_target_ < 0 || !drag_start_rot_) return;
    auto git = gao_groups_.find(uint32_t(gizmo_drag_target_));
    if (git == gao_groups_.end()) return;
    const Quat q_new = quat_mul(q_delta, *drag_start_rot_);
    git->second.rotation = q_new;
    // Pivot-around-mesh-center: without this fix the mesh would swing
    // around the world origin instead of spinning in place.
    adjust_position_for_pivot(uint32_t(gizmo_drag_target_), q_new,
                              drag_start_scale_);
    emit object_transformed(qlonglong(gizmo_drag_target_));
    update();
}

void PlacementCanvas::drag_scale(double sx, double sy) {
    if (!gizmo_drag_axis_ || !drag_start_axis_dist_) return;
    const auto d_now = mouse_axis_distance(*gizmo_drag_axis_, sx, sy);
    if (!d_now) return;
    const double d0 = *drag_start_axis_dist_;
    // Clamp so a tiny drag near the origin doesn't blow up to 1000×.
    double factor = 1.0;
    if (std::abs(d0) >= 1e-3)
        factor = std::clamp(*d_now / d0, 0.01, 100.0);
    if (gizmo_drag_target_ < 0 || !drag_start_scale_) return;
    auto git = gao_groups_.find(uint32_t(gizmo_drag_target_));
    if (git == gao_groups_.end()) return;
    V3 new_scale = *drag_start_scale_;
    new_scale[size_t(*gizmo_drag_axis_)] =
        (*drag_start_scale_)[size_t(*gizmo_drag_axis_)] * factor;
    git->second.scale = new_scale;
    // Same pivot-around-mesh-center fix as for rotate.
    adjust_position_for_pivot(uint32_t(gizmo_drag_target_), drag_start_rot_,
                              new_scale);
    emit object_transformed(qlonglong(gizmo_drag_target_));
    update();
}

// Set group.position so the mesh's bounds centre stays pinned at the
// drag-start world position while rotation q and per-axis scale change:
//   position = pivot_world − R(q)·(scale ⊙ p_local)
void PlacementCanvas::adjust_position_for_pivot(
    uint32_t gao_key, const std::optional<Quat>& q,
    const std::optional<V3>& scale) {
    auto bit = gao_bounds_.find(gao_key);
    auto git = gao_groups_.find(gao_key);
    if (bit == gao_bounds_.end() || git == gao_groups_.end()) return;
    const V3 p_local = (bit->second.first + bit->second.second) * 0.5;
    const V3 s = scale ? *scale : V3{1.0, 1.0, 1.0};
    const Quat qq = q ? *q : Quat{0.0, 0.0, 0.0, 1.0};
    const V3 p_scaled{p_local[0] * s[0], p_local[1] * s[1],
                      p_local[2] * s[2]};
    const V3 p_rot = vec_transform_quat(p_scaled, qq);
    git->second.position = gizmo_drag_start_world_ - p_rot;
}

// Activate one of 'move' | 'rotate' | 'scale' (Unity-style W/E/R).
void PlacementCanvas::set_tool(const QString& tool) {
    if (tool != QLatin1String("move") && tool != QLatin1String("rotate")
        && tool != QLatin1String("scale"))
        return;
    active_tool_ = tool;
    // Keep the toolbar button group in sync (covers the keyboard path).
    if (panel_) panel_->sync_tool_button(tool);
    update();
}

// -- Bounds / framing --

// Robust scene bounds for auto-framing: anchor on the MEDIAN object
// centre (ignores outliers like ±1800 skydomes), keep the inner ~90% of
// objects by distance, and frame the true bounding box of what's kept.
void PlacementCanvas::recompute_bounds() {
    std::vector<V3> los, his, centers;
    for (const SolidMesh& sm : solid_meshes_) {
        if (!sm.vertices || sm.vertices->empty()) continue;
        const std::vector<float>& v = *sm.vertices;
        V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (size_t i = 0; i + 2 < v.size(); i += 3)
            for (int c = 0; c < 3; ++c) {
                lo[size_t(c)] = std::min(lo[size_t(c)], double(v[i + size_t(c)]));
                hi[size_t(c)] = std::max(hi[size_t(c)], double(v[i + size_t(c)]));
            }
        los.push_back(lo);
        his.push_back(hi);
        centers.push_back((lo + hi) * 0.5);
    }
    if (centers.empty()) {
        scene_center_ = {0.0, 0.0, 0.0};
        scene_radius_ = 5.0;
        return;
    }
    V3 med;
    for (int c = 0; c < 3; ++c) {
        std::vector<double> axis;
        axis.reserve(centers.size());
        for (const V3& p : centers) axis.push_back(p[size_t(c)]);
        med[size_t(c)] = percentile(axis, 50.0);
    }
    std::vector<double> d;
    d.reserve(centers.size());
    for (const V3& p : centers) d.push_back(norm(p - med));
    const double cap =
        d.size() >= 6 ? percentile(d, 90.0)
                      : *std::max_element(d.begin(), d.end());
    const double keep_cap = std::max(cap, 1e-6);
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (size_t i = 0; i < centers.size(); ++i) {
        if (d[i] > keep_cap) continue;
        for (int c = 0; c < 3; ++c) {
            lo[size_t(c)] = std::min(lo[size_t(c)], los[i][size_t(c)]);
            hi[size_t(c)] = std::max(hi[size_t(c)], his[i][size_t(c)]);
        }
    }
    scene_center_ = (lo + hi) * 0.5;
    scene_radius_ = std::max(norm(hi - lo) * 0.5, 1.0);
}

// Frame the scene so the loaded bounds fill ~75% of the view.
void PlacementCanvas::auto_frame() {
    recompute_bounds();
    const double fov_rad = 50.0 * M_PI / 180.0;
    const double dist = std::max(
        {scene_radius_ / std::max(std::tan(fov_rad * 0.5), 1e-3) * 0.75,
         scene_radius_ * 0.6, 2.0});
    cam_pos_ = scene_center_ + V3{dist * 0.7, dist * 0.5, dist * 0.7};
    show_pos(scene_center_);
    orbit_target_ = scene_center_;
    orbit_target_set_ = true;
    update();
}

void PlacementCanvas::reset_camera() { auto_frame(); }

// -- Toolbar-driven state --

void PlacementCanvas::set_textures_visible(bool on) {
    textures_visible_ = on;
    update();
}

void PlacementCanvas::set_show_wireframe(bool on) {
    show_wireframe_ = on;
    rebuild_overlays();
}

void PlacementCanvas::set_show_baked(bool on) {
    show_baked_ = on;
    update();
}

void PlacementCanvas::set_show_normals(bool on) {
    show_normals_ = on;
    rebuild_overlays();
}

void PlacementCanvas::set_show_collision(bool on) {
    show_collision_ = on;
    if (show_collision_) {
        rebuild_collision_legend();
        collision_legend_label_->show();
        position_collision_legend();
    } else {
        collision_legend_label_->hide();
    }
    update();
}

void PlacementCanvas::set_collision_xray(bool on) {
    show_collision_xray_ = on;
    update();
}

void PlacementCanvas::set_fps_visible(bool on) {
    if (on) {
        fps_frame_count_ = 0;
        fps_last_t_ = now_seconds();
        fps_label_->setText(QStringLiteral(" measuring… "));
        fps_label_->adjustSize();
        position_fps_label();
        fps_label_->show();
        fps_sample_timer_.start();
    } else {
        fps_sample_timer_.stop();
        fps_label_->hide();
    }
}

// Swap mouse-handling behaviour. WASD always works regardless; the mode
// only chooses whether the mouse orbits a target or mouse-looks
// first-person style.
void PlacementCanvas::set_camera_mode(const QString& mode) {
    cam_mode_ = mode;
    if (mode == QLatin1String("orbit") && !orbit_target_set_) {
        // pygfx's OrbitController derives its target from internal
        // controller state; we initialise to the scene centre.
        orbit_target_ = scene_center_;
        orbit_target_set_ = true;
    }
}

void PlacementCanvas::sample_fps() {
    const double now = now_seconds();
    const double dt = std::max(1e-6, now - fps_last_t_);
    const double fps = double(fps_frame_count_) / dt;
    fps_label_->setText(QString::asprintf(" %5.1f FPS · %d dc ", fps,
                                          int(solid_meshes_.size())));
    fps_label_->adjustSize();
    position_fps_label();
    fps_frame_count_ = 0;
    fps_last_t_ = now;
}

void PlacementCanvas::position_fps_label() {
    // Top-right corner of the canvas, 6px margin.
    if (!fps_label_->isVisible()) return;
    fps_label_->move(std::max(0, width() - fps_label_->width() - 6), 6);
}

// Colour-keyed HTML table: one row per unique (gao_key, name) collision
// wireframe. Swatches render opaque so the hue reads against the dark
// legend background.
void PlacementCanvas::rebuild_collision_legend() {
    QStringList rows;
    QSet<QString> seen;
    for (const CollisionLine& cl : collision_lines_) {
        const QString key =
            QStringLiteral("%1|%2").arg(cl.gao_key).arg(cl.name);
        if (seen.contains(key)) continue;
        seen.insert(key);
        const int r = std::clamp(int(std::lround(cl.color[0] * 255)), 0, 255);
        const int g = std::clamp(int(std::lround(cl.color[1] * 255)), 0, 255);
        const int b = std::clamp(int(std::lround(cl.color[2] * 255)), 0, 255);
        QString safe_name = cl.name;
        safe_name.replace(QLatin1Char('<'), QLatin1String("&lt;"));
        safe_name.replace(QLatin1Char('>'), QLatin1String("&gt;"));
        rows.append(QStringLiteral(
            "<tr>"
            "<td style='padding:2px 6px 2px 0;'>"
            "<span style='display:inline-block; width:24px; height:14px; "
            "background-color:rgb(%1,%2,%3); border:1px solid #ddd;'>"
            "&nbsp;</span></td>"
            "<td style='padding:2px 0;'>%4</td>"
            "</tr>").arg(r).arg(g).arg(b).arg(safe_name));
    }
    if (rows.isEmpty()) {
        collision_legend_label_->setText(
            QStringLiteral("<b>Collision</b><br>(no ColMap GAOs in scene)"));
    } else {
        collision_legend_label_->setText(
            QStringLiteral("<div style='font-size:12px;'>"
                           "<b>Collision sources</b></div>"
                           "<table cellspacing='0' cellpadding='0'>")
            + rows.join(QString()) + QStringLiteral("</table>"));
    }
    collision_legend_label_->adjustSize();
}

void PlacementCanvas::position_collision_legend() {
    // Top-left corner of the canvas, 6px margin.
    if (!collision_legend_label_->isVisible()) return;
    collision_legend_label_->move(6, 6);
}

bool PlacementCanvas::eventFilter(QObject* obj, QEvent* event) {
    return QOpenGLWidget::eventFilter(obj, event);
}

// -- GL lifecycle --

void PlacementCanvas::initializeGL() {
    initializeOpenGLFunctions();
    // Background: near-black green from the app theme (the Python scene
    // used the pygfx default black).
    glClearColor(0.059f, 0.090f, 0.075f, 1.0f);
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
    auto link = [this, &compile](const char* vs_src,
                                 const char* fs_src) -> unsigned {
        const unsigned vs = compile(vs_src, GL_VERTEX_SHADER);
        const unsigned fs = compile(fs_src, GL_FRAGMENT_SHADER);
        unsigned prog = 0;
        if (vs && fs) {
            prog = glCreateProgram();
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
                prog = 0;
            }
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return prog;
    };
    mesh_program_ = link(MESH_VERT, MESH_FRAG);
    line_program_ = link(LINE_VERT, LINE_FRAG);
    gl_initialized_ = true;
    // Any scene queued before GL was ready is processed in paintGL.
}

void PlacementCanvas::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    position_fps_label();
    position_collision_legend();
}

void PlacementCanvas::cleanup_gl() {
    wasd_timer_.stop();
    fps_sample_timer_.stop();
    if (!gl_initialized_) return;
    makeCurrent();
    clear_meshes();
    textures_by_key_.clear();
    drain_texture_deletes();
    if (mesh_program_) {
        glDeleteProgram(mesh_program_);
        mesh_program_ = 0;
    }
    if (line_program_) {
        glDeleteProgram(line_program_);
        line_program_ = 0;
    }
    doneCurrent();
    gl_initialized_ = false;
}

void PlacementCanvas::paintGL() {
    drain_texture_deletes();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ++fps_frame_count_;
    if (!mesh_program_ || !line_program_) return;

    const Mat4d pv = proj_view();
    auto upload_mvp = [this](unsigned prog, const Mat4d& mvp) {
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = float(mvp[size_t(i)]);
        // Row-major matrices → transpose=GL_TRUE (numpy layout).
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_TRUE, m);
    };
    auto group_matrix = [this](long long gao_key) -> Mat4d {
        if (gao_key < 0) return identity4();
        auto git = gao_groups_.find(uint32_t(gao_key));
        if (git == gao_groups_.end()) return identity4();
        return mat_trs(git->second.position, git->second.rotation,
                       git->second.scale);
    };

    // ── Solid meshes: opaque + cutout first, blend after (render_order
    // +100 in the Python) with depth-write off.
    glUseProgram(mesh_program_);
    const int loc_tex = glGetUniformLocation(mesh_program_, "uTexture");
    const int loc_use_tex = glGetUniformLocation(mesh_program_, "uUseTexture");
    const int loc_use_vc =
        glGetUniformLocation(mesh_program_, "uUseVertexColor");
    const int loc_atest = glGetUniformLocation(mesh_program_, "uAlphaTest");
    const int loc_base = glGetUniformLocation(mesh_program_, "uBaseColor");
    glUniform1i(loc_tex, 0);

    auto draw_solid = [&](const SolidMesh& sm) {
        const bool textured = sm.texture.tex && textures_visible_;
        const bool baked = show_baked_ && sm.has_rli;
        // MeshBasicMaterial mode table: colour forced white when
        // textured or baked so the diffuse map / RLI stay true.
        if (textured || baked)
            glUniform4f(loc_base, 1.0f, 1.0f, 1.0f, 1.0f);
        else
            glUniform4f(loc_base, sm.base_color[0], sm.base_color[1],
                        sm.base_color[2], sm.base_color[3]);
        glUniform1i(loc_use_tex, textured ? 1 : 0);
        glUniform1i(loc_use_vc, baked ? 1 : 0);
        glUniform1i(loc_atest,
                    (textured && sm.texture.alpha_class == AlphaClass::Cutout)
                        ? 1 : 0);
        if (textured) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sm.texture.tex->id);
        }
        if (sm.depth_offset) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -1.0f);
        }
        upload_mvp(mesh_program_, matmul(pv, group_matrix(sm.gao_key)));
        glBindVertexArray(sm.vao);
        glDrawElements(GL_TRIANGLES, sm.num_indices, GL_UNSIGNED_INT,
                       nullptr);
        glBindVertexArray(0);
        if (sm.depth_offset) glDisable(GL_POLYGON_OFFSET_FILL);
    };

    // Two-sided rendering (material.side='both'): no face culling.
    glDisable(GL_CULL_FACE);

    // Match pygfx's alpha modes exactly.  Its opaque and alpha-test
    // materials write depth with blending disabled; only alpha_mode='blend'
    // enables source-alpha blending and disables depth writes.  Leaving
    // GL_BLEND enabled for cutouts makes surviving edge texels translucent,
    // which exposes the rectangular texture card around dirt/foliage.
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    for (const SolidMesh& sm : solid_meshes_) {
        const bool blend = sm.texture.tex && textures_visible_
                           && sm.texture.alpha_class == AlphaClass::Blend;
        if (!blend) draw_solid(sm);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (const SolidMesh& sm : solid_meshes_) {
        const bool blend = sm.texture.tex && textures_visible_
                           && sm.texture.alpha_class == AlphaClass::Blend;
        if (blend) draw_solid(sm);
    }
    glDepthMask(GL_TRUE);
    // Keep blending enabled for the translucent wireframe, collision, marker,
    // selection, and gizmo overlays below.

    // ── Wireframe overlay: the meshes again in line polygon mode.
    if (show_wireframe_) {
        glUseProgram(line_program_);
        const int loc_col = glGetUniformLocation(line_program_, "uColor");
        glUniform4f(loc_col, 0.0f, 0.0f, 0.0f, 0.55f);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(1.0f);
        for (const SolidMesh& sm : solid_meshes_) {
            upload_mvp(line_program_, matmul(pv, group_matrix(sm.gao_key)));
            glBindVertexArray(sm.vao);
            glDrawElements(GL_TRIANGLES, sm.num_indices, GL_UNSIGNED_INT,
                           nullptr);
            glBindVertexArray(0);
        }
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // ── Normals overlay: short line at each vertex along its normal
    // (debug aid). Buffers are built lazily per mesh.
    if (show_normals_) {
        glUseProgram(line_program_);
        const int loc_col = glGetUniformLocation(line_program_, "uColor");
        glUniform4f(loc_col, 0.2f, 0.9f, 1.0f, 1.0f);
        glLineWidth(1.0f);
        for (SolidMesh& sm : solid_meshes_) {
            if (!sm.normals_vao) {
                if (!sm.vertices || !sm.normals || sm.vertices->empty())
                    continue;
                const std::vector<float>& v = *sm.vertices;
                const std::vector<float>& nn = *sm.normals;
                V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
                for (size_t i = 0; i + 2 < v.size(); i += 3)
                    for (int c = 0; c < 3; ++c) {
                        lo[size_t(c)] = std::min(lo[size_t(c)],
                                                 double(v[i + size_t(c)]));
                        hi[size_t(c)] = std::max(hi[size_t(c)],
                                                 double(v[i + size_t(c)]));
                    }
                double radius = norm(hi - lo);
                if (radius <= 0.0) radius = 1.0;
                const float len = float(radius * 0.02);
                const size_t count = std::min(v.size(), nn.size()) / 3;
                std::vector<float> segs;
                segs.reserve(count * 6);
                for (size_t i = 0; i < count; ++i) {
                    segs.insert(segs.end(),
                                {v[i * 3], v[i * 3 + 1], v[i * 3 + 2]});
                    segs.insert(segs.end(),
                                {v[i * 3] + nn[i * 3] * len,
                                 v[i * 3 + 1] + nn[i * 3 + 1] * len,
                                 v[i * 3 + 2] + nn[i * 3 + 2] * len});
                }
                glGenVertexArrays(1, &sm.normals_vao);
                glBindVertexArray(sm.normals_vao);
                glGenBuffers(1, &sm.normals_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, sm.normals_vbo);
                glBufferData(GL_ARRAY_BUFFER,
                             GLsizeiptr(segs.size() * sizeof(float)),
                             segs.data(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * 4,
                                      nullptr);
                glBindVertexArray(0);
                sm.normals_count = int(segs.size() / 3);
            }
            upload_mvp(line_program_, matmul(pv, group_matrix(sm.gao_key)));
            glBindVertexArray(sm.normals_vao);
            glDrawArrays(GL_LINES, 0, sm.normals_count);
            glBindVertexArray(0);
        }
    }

    // ── Collision wireframes (x-ray optional).
    if (show_collision_ && !collision_lines_.empty()) {
        glUseProgram(line_program_);
        const int loc_col = glGetUniformLocation(line_program_, "uColor");
        for (const CollisionLine& cl : collision_lines_) {
            const bool no_depth = cl.xray && show_collision_xray_;
            if (no_depth) glDisable(GL_DEPTH_TEST);
            glLineWidth(std::min(std::max(cl.line_width, 1.0f),
                                 max_line_width_));
            glUniform4f(loc_col, cl.color[0], cl.color[1], cl.color[2],
                        cl.color[3]);
            upload_mvp(line_program_, matmul(pv, group_matrix(cl.gao_key)));
            glBindVertexArray(cl.vao);
            glDrawArrays(GL_LINES, 0, cl.count);
            glBindVertexArray(0);
            if (no_depth) glEnable(GL_DEPTH_TEST);
        }
        glLineWidth(1.0f);
    }

    glUseProgram(0);
    draw_overlays();
}

// ── QPainter overlays (everything pygfx drew with depth_test=False) ──

void PlacementCanvas::draw_overlays() {
    QPainter painter(this);
    if (!painter.isActive()) return;
    painter.setRenderHint(QPainter::Antialiasing, true);
    draw_axes_helper(painter);
    draw_markers(painter);
    draw_selection_outline(painter);
    if (gizmo_visible_) draw_gizmo(painter);
    painter.end();
}

// gfx.AxesHelper(size=1.5, thickness=2) — world axes at the origin.
// Simplification: drawn as an overlay (no depth test).
void PlacementCanvas::draw_axes_helper(QPainter& painter) {
    const auto origin = world_to_screen({0.0, 0.0, 0.0});
    if (!origin) return;
    const QColor colors[3] = {QColor(220, 60, 60), QColor(60, 200, 80),
                              QColor(70, 120, 240)};
    for (int axis = 0; axis < 3; ++axis) {
        V3 end{0.0, 0.0, 0.0};
        end[size_t(axis)] = 1.5;
        const auto e = world_to_screen(end);
        if (!e) continue;
        painter.setPen(QPen(colors[axis], 2));
        painter.drawLine(*origin, *e);
    }
}

void PlacementCanvas::draw_markers(QPainter& painter) {
    for (const Marker& mk : markers_) {
        V3 world = mk.local_pos;
        if (mk.gao_key >= 0) {
            auto git = gao_groups_.find(uint32_t(mk.gao_key));
            if (git != gao_groups_.end())
                world = vec_transform(mk.local_pos,
                                      mat_trs(git->second.position,
                                              git->second.rotation,
                                              git->second.scale));
        }
        const auto scr = world_to_screen(world);
        if (!scr) continue;
        draw_marker_icon(painter, *scr, mk.category);
    }
}

// Category icon: QPainter pictograph approximating the Python's SDF /
// built-in point markers (26 px, black edge, category colour).
void PlacementCanvas::draw_marker_icon(QPainter& painter, const QPointF& c,
                                       const QString& category) {
    const double s = 26.0;
    const QColor col = category_color(category);
    const QPen edge(QColor(0, 0, 0), 1.5);
    painter.setPen(edge);
    painter.setBrush(col);
    if (category == QLatin1String("camera")) {
        // Rounded body + viewfinder bump, lens hole punched out.
        QPainterPath path;
        path.addRoundedRect(c.x() - s * 0.32, c.y() - s * 0.17, s * 0.64,
                            s * 0.38, 2.0, 2.0);
        path.addRect(c.x() + s * 0.05, c.y() - s * 0.25, s * 0.20, s * 0.10);
        painter.drawPath(path.simplified());
        painter.setBrush(QColor(0, 0, 0));
        painter.drawEllipse(c, s * 0.085, s * 0.085);
    } else if (category == QLatin1String("light")) {
        // Sun: solid core + 8-point burst.
        for (int i = 0; i < 8; ++i) {
            const double a = 2.0 * M_PI * i / 8.0;
            painter.setPen(QPen(col, 2.0));
            painter.drawLine(
                c + QPointF(std::cos(a) * s * 0.22, std::sin(a) * s * 0.22),
                c + QPointF(std::cos(a) * s * 0.44, std::sin(a) * s * 0.44));
        }
        painter.setPen(edge);
        painter.drawEllipse(c, s * 0.18, s * 0.18);
    } else if (category == QLatin1String("sound")) {
        // Speech bubble: rounded body + tail.
        painter.drawRoundedRect(
            QRectF(c.x() - s * 0.30, c.y() - s * 0.27, s * 0.60, s * 0.40),
            3.0, 3.0);
        const QPointF tail[3] = {
            c + QPointF(-s * 0.20, s * 0.10),
            c + QPointF(-s * 0.05, s * 0.10),
            c + QPointF(-s * 0.16, s * 0.34),
        };
        painter.drawPolygon(tail, 3);
    } else if (category == QLatin1String("logic")) {
        // Gear: ringed disc with 8 teeth and a central hole.
        for (int i = 0; i < 8; ++i) {
            const double a = 2.0 * M_PI * i / 8.0;
            painter.setPen(QPen(col, 3.0));
            painter.drawLine(
                c + QPointF(std::cos(a) * s * 0.24, std::sin(a) * s * 0.24),
                c + QPointF(std::cos(a) * s * 0.38, std::sin(a) * s * 0.38));
        }
        painter.setPen(edge);
        painter.drawEllipse(c, s * 0.26, s * 0.26);
        painter.setBrush(QColor(0, 0, 0));
        painter.drawEllipse(c, s * 0.12, s * 0.12);
    } else if (category == QLatin1String("fx")) {
        // asterisk8 (sparkle)
        painter.setPen(QPen(col, 2.5));
        for (int i = 0; i < 8; ++i) {
            const double a = M_PI * i / 4.0;
            painter.drawLine(
                c - QPointF(std::cos(a) * s * 0.4, std::sin(a) * s * 0.4),
                c + QPointF(std::cos(a) * s * 0.4, std::sin(a) * s * 0.4));
        }
    } else if (category == QLatin1String("trigger")) {
        // ring (gate / zone)
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0), 5.0));
        painter.drawEllipse(c, s * 0.30, s * 0.30);
        painter.setPen(QPen(col, 3.0));
        painter.drawEllipse(c, s * 0.30, s * 0.30);
    } else if (category == QLatin1String("trap")) {
        // triangle_up (warning)
        const QPointF tri[3] = {c + QPointF(0.0, -s * 0.38),
                                c + QPointF(-s * 0.35, s * 0.28),
                                c + QPointF(s * 0.35, s * 0.28)};
        painter.drawPolygon(tri, 3);
    } else if (category == QLatin1String("spawner")) {
        // triangle_down (spawn arrow)
        const QPointF tri[3] = {c + QPointF(0.0, s * 0.38),
                                c + QPointF(-s * 0.35, -s * 0.28),
                                c + QPointF(s * 0.35, -s * 0.28)};
        painter.drawPolygon(tri, 3);
    } else if (category == QLatin1String("waypoint")) {
        // pin (map pin: disc + tail to a point)
        const QPointF tail[3] = {c + QPointF(-s * 0.16, s * 0.02),
                                 c + QPointF(s * 0.16, s * 0.02),
                                 c + QPointF(0.0, s * 0.42)};
        painter.drawPolygon(tail, 3);
        painter.drawEllipse(c + QPointF(0.0, -s * 0.10), s * 0.20, s * 0.20);
    } else if (category == QLatin1String("actor")) {
        // spade-ish figure: head + shoulders silhouette.
        const QPointF body[3] = {c + QPointF(0.0, -s * 0.34),
                                 c + QPointF(-s * 0.28, s * 0.20),
                                 c + QPointF(s * 0.28, s * 0.20)};
        painter.drawPolygon(body, 3);
        painter.drawRect(QRectF(c.x() - s * 0.05, c.y() + s * 0.16, s * 0.10,
                                s * 0.18));
    } else {
        painter.drawEllipse(c, s * 0.30, s * 0.30);
    }
}

// Yellow world-space bounds box around the selected GAO — tracks the
// group's full transform (rotation + scale, not just translation).
void PlacementCanvas::draw_selection_outline(QPainter& painter) {
    if (selected_gao_ < 0) return;
    const uint32_t gk = uint32_t(selected_gao_);
    auto bit = gao_bounds_.find(gk);
    auto git = gao_groups_.find(gk);
    if (bit == gao_bounds_.end() || git == gao_groups_.end()) return;
    const V3& lo = bit->second.first;
    const V3& hi = bit->second.second;
    const Mat4d m = mat_trs(git->second.position, git->second.rotation,
                            git->second.scale);
    const V3 corners_local[8] = {
        {lo[0], lo[1], lo[2]}, {hi[0], lo[1], lo[2]},
        {lo[0], hi[1], lo[2]}, {hi[0], hi[1], lo[2]},
        {lo[0], lo[1], hi[2]}, {hi[0], lo[1], hi[2]},
        {lo[0], hi[1], hi[2]}, {hi[0], hi[1], hi[2]},
    };
    std::optional<QPointF> proj[8];
    for (int i = 0; i < 8; ++i)
        proj[i] = world_to_screen(vec_transform(corners_local[i], m));
    static const int edges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    QPen pen(QColor::fromRgbF(1.0f, 0.85f, 0.1f), 2.5);
    painter.setPen(pen);
    for (const auto& e : edges) {
        if (!proj[e[0]] || !proj[e[1]]) continue;
        painter.drawLine(*proj[e[0]], *proj[e[1]]);
    }
}

// Draw the active transform tool's gizmo (move / rotate / scale) with
// constant axis colours (R=X, G=Y, B=Z).
void PlacementCanvas::draw_gizmo(QPainter& painter) {
    const double scale = gizmo_world_scale(gizmo_pos_, selected_gao_);
    if (active_tool_ == QLatin1String("rotate")) {
        // Three 48-segment rings, each in the plane ⟂ its axis.
        const int ring_segs = 48;
        for (int axis = 0; axis < 3; ++axis) {
            const int ax2 = (axis + 1) % 3;
            const int ax3 = (axis + 2) % 3;
            QPolygonF poly;
            bool complete = true;
            for (int i = 0; i <= ring_segs; ++i) {
                const double a = 2.0 * M_PI * i / ring_segs;
                V3 p = gizmo_pos_;
                p[size_t(ax2)] += std::cos(a) * scale;
                p[size_t(ax3)] += std::sin(a) * scale;
                const auto scr = world_to_screen(p);
                if (!scr) {
                    complete = false;
                    break;
                }
                poly << *scr;
            }
            if (!complete) continue;
            painter.setPen(QPen(AXIS_COLORS[axis], 3));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(poly);
        }
        return;
    }
    // Move / scale: three axis lines from the origin.
    const auto origin = world_to_screen(gizmo_pos_);
    if (!origin) return;
    for (int axis = 0; axis < 3; ++axis) {
        V3 end = gizmo_pos_;
        end[size_t(axis)] += scale;
        const auto e = world_to_screen(end);
        if (!e) continue;
        QPen pen(AXIS_COLORS[axis], 4);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(*origin, *e);
        if (active_tool_ == QLatin1String("scale")) {
            // Cube handle at the tip — fixed screen-size square
            // (simplification of the pygfx 0.12-unit box).
            painter.setBrush(AXIS_COLORS[axis]);
            painter.setPen(QPen(QColor(0, 0, 0), 1));
            painter.drawRect(QRectF(e->x() - 4.5, e->y() - 4.5, 9.0, 9.0));
        }
    }
}

// -- Mouse / key events --

void PlacementCanvas::mousePressEvent(QMouseEvent* event) {
    // Steal keyboard focus on any canvas click so WASD / F / Home work
    // without the user having to tab around.
    if (!hasFocus()) setFocus(Qt::MouseFocusReason);
    last_mouse_ = event->position();
    if (event->button() == Qt::RightButton) {
        // RMB = look around. Remember it so WASD keeps moving even if
        // the cursor wanders to the canvas edge mid-drag.
        rmb_look_ = true;
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const double x = event->position().x();
        const double y = event->position().y();
        // Tool gizmo grab takes precedence (only when a GAO is selected
        // — otherwise there's nothing to transform).
        if (gizmo_visible_ && selected_gao_ >= 0) {
            const auto axis = tool_axis_at(x, y);
            if (axis) {
                begin_tool_drag(*axis, x, y);
                return;
            }
        }
        // Otherwise: pick-on-plane / mesh select
        if (point_picking_) {
            const auto pt = ray_to_plane(x, y, pick_plane_y_);
            if (pt) {
                emit point_picked((*pt)[0], (*pt)[1], (*pt)[2]);
                return;
            }
        }
        handle_selection(x, y);
    }
}

void PlacementCanvas::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    // Always cache the mouse position so 'Place Object' can spawn under
    // the cursor even if no drag is in progress.
    last_cursor_xy_ = pos;

    if (gizmo_drag_axis_ && (event->buttons() & Qt::LeftButton)) {
        if (active_tool_ == QLatin1String("move"))
            drag_move(pos.x(), pos.y());
        else if (active_tool_ == QLatin1String("rotate"))
            drag_rotate(pos.x(), pos.y());
        else if (active_tool_ == QLatin1String("scale"))
            drag_scale(pos.x(), pos.y());
        last_mouse_ = pos;
        return;
    }

    if (!last_mouse_) {
        last_mouse_ = pos;
        return;
    }
    const double dx = pos.x() - last_mouse_->x();
    const double dy = pos.y() - last_mouse_->y();

    if (cam_mode_ == QLatin1String("fly")) {
        // FPS-style mouse look on RMB: yaw around world-up, pitch
        // around the camera's local X (no roll bake-in).
        if ((event->buttons() & Qt::RightButton) || rmb_look_) {
            cam_yaw_ -= dx * 0.005;
            cam_pitch_ = std::clamp(cam_pitch_ - dy * 0.005,
                                    -89.0 * M_PI / 180.0,
                                    89.0 * M_PI / 180.0);
            update();
        } else if (event->buttons() & Qt::MiddleButton) {
            // MMB pan (Unity/Unreal-style binding).
            const double speed =
                std::max(norm(cam_pos_ - scene_center_), 1.0) * 0.002;
            const V3 right = cam_right();
            const V3 up{0.0, 1.0, 0.0};
            cam_pos_ = cam_pos_ - right * (dx * speed) + up * (dy * speed);
            update();
        }
    } else {  // orbit
        if (event->buttons() & Qt::LeftButton) {
            // Orbit around the target.
            const double dist =
                std::max(norm(cam_pos_ - orbit_target_), 0.1);
            cam_yaw_ -= dx * 0.005;
            cam_pitch_ = std::clamp(cam_pitch_ - dy * 0.005,
                                    -89.0 * M_PI / 180.0,
                                    89.0 * M_PI / 180.0);
            cam_pos_ = orbit_target_ - cam_forward() * dist;
            update();
        } else if ((event->buttons() & Qt::RightButton)
                   || (event->buttons() & Qt::MiddleButton)) {
            // Pan target + camera together.
            const double dist =
                std::max(norm(cam_pos_ - orbit_target_), 1.0);
            const double speed = dist * 0.002;
            const V3 right = cam_right();
            const V3 up{0.0, 1.0, 0.0};
            const V3 delta =
                right * (-dx * speed) + up * (dy * speed);
            cam_pos_ = cam_pos_ + delta;
            orbit_target_ = orbit_target_ + delta;
            update();
        }
    }
    last_mouse_ = pos;
}

void PlacementCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) rmb_look_ = false;
    if (gizmo_drag_axis_) {
        gizmo_drag_axis_.reset();
        gizmo_drag_start_mouse_.reset();
        gizmo_drag_target_ = -1;
        drag_start_rot_.reset();
        drag_start_scale_.reset();
        drag_start_angle_.reset();
        drag_start_axis_dist_.reset();
    }
    last_mouse_.reset();
}

void PlacementCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    // Frame on double-click
    auto_frame();
}

void PlacementCanvas::wheelEvent(QWheelEvent* event) {
    // Scroll wheel dials the fly-speed multiplier up/down (Unity-style)
    // so the user can tune the feel. ~15% per notch, clamped 0.05..20×.
    const int dy = event->angleDelta().y();
    if (dy) {
        const double factor = std::pow(1.15, dy / 120.0);
        user_speed_mult =
            std::min(20.0, std::max(0.05, user_speed_mult * factor));
    }
    event->accept();
}

void PlacementCanvas::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (key == Qt::Key_F) {
        auto_frame();
        return;
    }
    if (key == Qt::Key_Home) {
        reset_camera();
        return;
    }
    // Unity-style transform-tool hotkeys. Skip while a drag is already
    // in progress so the user can't accidentally switch tools mid-move.
    if (!gizmo_drag_axis_) {
        if (key == Qt::Key_W) {
            set_tool(QStringLiteral("move"));
            return;
        }
        if (key == Qt::Key_E) {
            set_tool(QStringLiteral("rotate"));
            return;
        }
        if (key == Qt::Key_R) {
            set_tool(QStringLiteral("scale"));
            return;
        }
    }
    QOpenGLWidget::keyPressEvent(event);
}

// ── PlacementViewport (panel) ────────────────────────────────────────

PlacementViewport::PlacementViewport(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    canvas_ = new PlacementCanvas(this);
    layout->addWidget(canvas_, 1);

    // Signal pass-through (canvas → panel outward API).
    connect(canvas_, &PlacementCanvas::point_picked, this,
            &PlacementViewport::point_picked);
    connect(canvas_, &PlacementCanvas::gizmo_moved, this,
            &PlacementViewport::gizmo_moved);
    connect(canvas_, &PlacementCanvas::object_moved, this,
            &PlacementViewport::object_moved);
    connect(canvas_, &PlacementCanvas::object_transformed, this,
            &PlacementViewport::object_transformed);
    connect(canvas_, &PlacementCanvas::object_selected, this,
            &PlacementViewport::object_selected);

    auto* toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(4, 0, 4, 0);
    toolbar->setSpacing(8);

    auto add_check = [&](const QString& text, bool checked,
                         const QString& tooltip) {
        auto* cb = new QCheckBox(text);
        cb->setChecked(checked);
        if (!tooltip.isEmpty()) cb->setToolTip(tooltip);
        toolbar->addWidget(cb);
        return cb;
    };

    tex_check_ = add_check(
        tr("Textures"), true,
        tr("Apply the zone's diffuse textures resolved via GAO → material "
           "→ texture."));
    connect(tex_check_, &QCheckBox::toggled, canvas_,
            &PlacementCanvas::set_textures_visible);

    wire_check_ = add_check(
        tr("Wireframe"), false,
        tr("Overlay the triangle wireframe over each mesh."));
    connect(wire_check_, &QCheckBox::toggled, canvas_,
            &PlacementCanvas::set_show_wireframe);

    baked_check_ = add_check(
        tr("Show baked lighting"), true,
        tr("Multiply each surface by its baked per-vertex RLI (the "
           "engine's static lighting) — shows the zone as it looks "
           "in-game. Off = full-bright textures for editing."));
    connect(baked_check_, &QCheckBox::toggled, canvas_,
            &PlacementCanvas::set_show_baked);

    normals_check_ = add_check(
        tr("Normals"), false,
        tr("Draw a short line at each vertex along its normal — debug "
           "aid."));
    connect(normals_check_, &QCheckBox::toggled, canvas_,
            &PlacementCanvas::set_show_normals);

    collision_check_ = add_check(
        tr("Collision"), false,
        tr("Show ColMap/COB collision geometry as x-ray wireframe."));
    connect(collision_check_, &QCheckBox::toggled, this,
            &PlacementViewport::on_collision_toggle);
    collision_check_->setVisible(false);

    markers_check_ = add_check(
        tr("Markers"), false,
        tr("Show non-visual objects (cameras, lights, sounds, triggers, "
           "traps, spawners, waypoints, FX) as colour-coded markers you "
           "can select and move."));
    connect(markers_check_, &QCheckBox::toggled, this,
            [this](bool c) { emit markers_toggled(c); });

    xray_check_ = add_check(
        tr("X-ray collision"), true,
        tr("Render collision wireframe through walls (ignore depth "
           "test)."));
    connect(xray_check_, &QCheckBox::toggled, canvas_,
            &PlacementCanvas::set_collision_xray);

    fps_check_ = add_check(
        tr("FPS"), false,
        tr("Show frames-per-second counter in the bottom-right corner."));
    connect(fps_check_, &QCheckBox::toggled, canvas_,
            &PlacementCanvas::set_fps_visible);

    // ── Transform-tool palette (Unity / Blender style) ────────
    toolbar->addWidget(new QLabel(tr("Tool:")));
    tool_group_ = new QButtonGroup(this);
    tool_group_->setExclusive(true);
    const struct {
        const char* name;
        const char* tooltip;
    } tools[] = {
        {"Move", "Translate selected object (W)"},
        {"Rotate", "Rotate selected object around an axis (E)"},
        {"Scale", "Scale selected object along an axis (R)"},
    };
    for (const auto& t : tools) {
        auto* btn = new QPushButton(tr(t.name));
        btn->setCheckable(true);
        btn->setToolTip(tr(t.tooltip));
        btn->setMaximumWidth(64);
        const QString tool_name = QString::fromLatin1(t.name).toLower();
        tool_buttons_[tool_name] = btn;
        tool_group_->addButton(btn);
        toolbar->addWidget(btn);
        connect(btn, &QPushButton::clicked, this,
                [this, tool_name]() { canvas_->set_tool(tool_name); });
    }
    tool_buttons_[QStringLiteral("move")]->setChecked(true);

    toolbar->addWidget(new QLabel(tr("Camera:")));
    cam_mode_ = new QComboBox();
    // Fly first → it's the default. WASD always works regardless; the
    // combo just chooses whether RMB orbits a target (orbit) or
    // mouse-looks first-person style (fly).
    cam_mode_->addItem(tr("Fly (WASD)"), QStringLiteral("fly"));
    cam_mode_->addItem(tr("Orbit"), QStringLiteral("orbit"));
    connect(cam_mode_, &QComboBox::currentIndexChanged, this,
            &PlacementViewport::on_camera_mode_changed);
    toolbar->addWidget(cam_mode_);

    auto* frame_btn = new QPushButton(tr("Frame"));
    frame_btn->setToolTip(tr("Frame all visible meshes (F)"));
    connect(frame_btn, &QPushButton::clicked, this,
            [this]() { canvas_->auto_frame(); });
    toolbar->addWidget(frame_btn);

    auto* reset_btn = new QPushButton(tr("Reset"));
    reset_btn->setToolTip(tr("Reset camera to scene-centered orbit"));
    connect(reset_btn, &QPushButton::clicked, this,
            [this]() { canvas_->reset_camera(); });
    toolbar->addWidget(reset_btn);

    info_label_ = new QLabel();
    toolbar->addWidget(info_label_, 1);

    layout->addLayout(toolbar);

    // App-level (not canvas-level) held-key filter so WASD works right
    // after a zone load (before the canvas is clicked) AND while the
    // right mouse button is held for look-around.
    auto* filter = new WasdFilter(canvas_);
    wasd_filter_ = filter;
    if (QApplication* app = qobject_cast<QApplication*>(
            QCoreApplication::instance()))
        app->installEventFilter(filter);
    else
        canvas_->installEventFilter(filter);
}

PlacementViewport::~PlacementViewport() = default;

void PlacementViewport::set_info(const QString& text) {
    info_label_->setText(text);
}

void PlacementViewport::set_collision_toggle_visible(bool visible) {
    collision_check_->setVisible(visible);
}

bool PlacementViewport::collision_visible() const {
    return collision_check_->isChecked();
}

bool PlacementViewport::markers_visible() const {
    return markers_check_->isChecked();
}

void PlacementViewport::cleanup_gl() { canvas_->cleanup_gl(); }

void PlacementViewport::load_meshes(
    const std::vector<placement::MeshDict>& mesh_list,
    const placement::TexturesByKey& textures, bool preserve_camera) {
    canvas_->load_meshes(mesh_list, textures, preserve_camera);
}

void PlacementViewport::set_point_picking(bool enabled, double plane_y) {
    canvas_->set_point_picking(enabled, plane_y);
}

bool PlacementViewport::alias_textures(uint32_t dest_gao_key,
                                       uint32_t source_gao_key) {
    return canvas_->alias_textures(dest_gao_key, source_gao_key);
}

void PlacementViewport::set_gao_textures(
    uint32_t gao_key, const std::vector<QImage>& rgba_per_element) {
    canvas_->set_gao_textures(gao_key, rgba_per_element);
}

void PlacementViewport::replace_object_meshes(
    long long gao_key, std::vector<placement::MeshDict> mesh_dicts) {
    canvas_->replace_object_meshes(gao_key, std::move(mesh_dicts));
}

placement::V3 PlacementViewport::focus_point_under_cursor() {
    return canvas_->focus_point_under_cursor();
}

placement::V3 PlacementViewport::focus_point() {
    return canvas_->focus_point();
}

std::optional<placement::GaoTransform> PlacementViewport::get_gao_transform(
    uint32_t gao_key) {
    return canvas_->get_gao_transform(gao_key);
}

void PlacementViewport::set_gao_transform(
    uint32_t gao_key, const std::optional<placement::V3>& position,
    const std::optional<placement::Quat>& rotation_quat,
    const std::optional<placement::V3>& scale) {
    canvas_->set_gao_transform(gao_key, position, rotation_quat, scale);
}

void PlacementViewport::set_transform_gizmo(bool enabled) {
    canvas_->set_transform_gizmo(enabled, std::nullopt);
}

void PlacementViewport::set_transform_gizmo(bool enabled,
                                            const placement::V3& position) {
    canvas_->set_transform_gizmo(enabled, position);
}

void PlacementViewport::select_gao(long long gao_key) {
    canvas_->select_gao(gao_key);
}

void PlacementViewport::auto_frame() { canvas_->auto_frame(); }

void PlacementViewport::on_collision_toggle(bool checked) {
    canvas_->set_show_collision(checked);
    emit collision_toggled(checked);
}

void PlacementViewport::on_camera_mode_changed(int idx) {
    Q_UNUSED(idx);
    canvas_->set_camera_mode(cam_mode_->currentData().toString());
    canvas_->setFocus(Qt::OtherFocusReason);
}

// Keep the toolbar button group in sync (covers the keyboard path).
void PlacementViewport::sync_tool_button(const QString& tool) {
    auto it = tool_buttons_.find(tool);
    if (it != tool_buttons_.end() && !it->second->isChecked())
        it->second->setChecked(true);
}
