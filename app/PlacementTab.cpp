// PlacementTab.cpp — Level Editor tab (port of gui/placement_tab.py).
//
// Layout:
//
//     ┌─ Zone tree ┬─ Viewport ──────────────────────────┐
//     │ filter     │                                     │
//     │ entries…   │                                     │
//     │            ├─ Tabs ──────────────────────────────┤
//     │            │ [Object] [Create]                   │
//     │            │   tab body                          │
//     │            │                                     │
//     │            ├─ Log ───────────────────────────────┤
//     └────────────┴─────────────────────────────────────┘
//
// The Object tab is the Inspector. Click a GAO in the viewport → its
// name / key / source-entry surface here; the Position/Rotation/Scale
// spinboxes are bound bidirectionally to the viewport's gizmo so
// dragging the gizmo updates the numbers and editing the numbers moves
// the mesh. The Create tab keeps the legacy new-object workflow
// (clone / primitive / imported model / replace mesh / replace bin).
//
// The anonymous namespace re-implements the GUI half of
// io_ops/object_placer.py (load_zone_info, build_preview_meshes,
// collision previews, texture resolution) on the jade read cores —
// see PlacementTab.hpp for the port-gap summary.
#include "PlacementTab.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "jade/AssetIndex.hpp"
#include "jade/BigFile.hpp"
#include "jade/Collision.hpp"
#include "jade/CollisionFollow.hpp"
#include "jade/Compression.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/Json.hpp"
#include "jade/Jgao.hpp"
#include "jade/Light.hpp"
#include "jade/Material.hpp"
#include "jade/ObjectKinds.hpp"
#include "jade/ObjectPlacer.hpp"
#include "jade/Rli.hpp"
#include "jade/Texture.hpp"

#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"

using placementtab::CommittedState;
using placementtab::PendingLightEdit;
using placementtab::Placement;
using placementtab::XbinBin;
using placementtab::ZoneInfo;
using placementtab::ZoneObject;

namespace {

using placement::MeshDict;
using placement::Quat;
using placement::V3;
using Mat16 = std::array<double, 16>;

constexpr quint32 INVALID_KEY = 0xFFFFFFFFu;
constexpr quint32 FLAG_COLMAP = 0x00000100u;
constexpr quint32 FLAG_ODE = 0x10000000u;

std::string dedicated_link_id(quint32 key) {
    char text[16];
    std::snprintf(text, sizeof text, "d:%08x", key);
    return text;
}

std::string carve_link_id(quint32 key) {
    char text[16];
    std::snprintf(text, sizeof text, "c:%08x", key);
    return text;
}

jade::json::Value collision_links_json(
    const jade::collision_follow::CollisionLinks& links) {
    jade::json::Value out = jade::json::make_obj();
    out.obj["same_gao"] = jade::json::make_bool(links.same_gao);
    jade::json::Value dedicated = jade::json::make_arr();
    for (const auto& link : links.dedicated) {
        jade::json::Value row = jade::json::make_obj();
        row.obj["gao_key"] = jade::json::make_str(hex_key_lower(link.gao_key));
        row.obj["name"] = jade::json::make_str(link.name);
        jade::json::Value keys = jade::json::make_arr();
        for (quint32 key : link.cob_keys)
            keys.arr.push_back(jade::json::make_str(hex_key_lower(key)));
        row.obj["cob_keys"] = std::move(keys);
        dedicated.arr.push_back(std::move(row));
    }
    out.obj["dedicated"] = std::move(dedicated);
    jade::json::Value carves = jade::json::make_arr();
    for (const auto& link : links.carves) {
        jade::json::Value row = jade::json::make_obj();
        row.obj["cob_key"] = jade::json::make_str(hex_key_lower(link.cob_key));
        row.obj["owner_gao_key"] =
            jade::json::make_str(hex_key_lower(link.owner_gao_key));
        row.obj["owner_name"] = jade::json::make_str(link.owner_name);
        jade::json::Value faces = jade::json::make_arr();
        for (quint32 index : link.face_indices)
            faces.arr.push_back(jade::json::make_num(index));
        row.obj["face_indices"] = std::move(faces);
        row.obj["n_total_faces"] = jade::json::make_num(link.n_total_faces);
        carves.arr.push_back(std::move(row));
    }
    out.obj["carves"] = std::move(carves);
    return out;
}

// ── Euler ↔ quaternion helpers (XYZ intrinsic, degrees) ──────────────

// (deg_x, deg_y, deg_z) → (qx, qy, qz, qw) (XYZ intrinsic). Matches
// ops_transform._euler_xyz_deg_to_quat so the viewport visual and the
// eventual baked matrix agree.
Quat euler_xyz_deg_to_quat(const V3& deg) {
    return jade::placer::euler_xyz_deg_to_quat(deg);
}

// Inverse of euler_xyz_deg_to_quat. Result has gimbal-lock ambiguity at
// ±90° pitch — fine for an inspector display.
V3 quat_to_euler_xyz_deg(const Quat& q) {
    const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    // Y (pitch) first to detect singularity.
    double sinp = 2.0 * (qw * qy - qz * qx);
    sinp = std::max(-1.0, std::min(1.0, sinp));
    const double pitch = std::asin(sinp);
    double roll, yaw;
    if (std::abs(sinp) < 0.999999) {
        roll = std::atan2(2.0 * (qw * qx + qy * qz),
                          1.0 - 2.0 * (qx * qx + qy * qy));
        yaw = std::atan2(2.0 * (qw * qz + qx * qy),
                         1.0 - 2.0 * (qy * qy + qz * qz));
    } else {
        // Gimbal lock — bias all rotation to roll, set yaw = 0.
        roll = 2.0 * std::atan2(qx, qw);
        yaw = 0.0;
    }
    const double r2d = 180.0 / M_PI;
    return {roll * r2d, pitch * r2d, yaw * r2d};
}

// Inverse of object_placer._jade_to_display.
// Mapping: display = (x, z, -y) ⇒ jade = (x, -z, y).
V3 display_to_jade(const V3& v) { return {v[0], -v[2], v[1]}; }

V3 jade_to_display(double x, double y, double z) { return {x, z, -y}; }

// Change-of-basis matrices between Jade engine space (Z-up) and display
// space (Y-up). J_jd maps a Jade column vector to its display
// counterpart; J_dj is the inverse (row-major flat, numpy layout).
constexpr Mat16 J_JD_4x4 = {
    1.0, 0.0,  0.0, 0.0,
    0.0, 0.0,  1.0, 0.0,
    0.0, -1.0, 0.0, 0.0,
    0.0, 0.0,  0.0, 1.0,
};
constexpr Mat16 J_DJ_4x4 = {
    1.0, 0.0, 0.0,  0.0,
    0.0, 0.0, -1.0, 0.0,
    0.0, 1.0, 0.0,  0.0,
    0.0, 0.0, 0.0,  1.0,
};

constexpr Mat16 IDENTITY_MATRIX_16 = {
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
};

// Row-major standard 4x4 multiply a·b (numpy @).
Mat16 mat4_mul(const Mat16& a, const Mat16& b) {
    Mat16 out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    return out;
}

// The viewport group's local matrix (pygfx group.local.matrix): the
// row-major math 4x4 T(p)·R(q)·S(s) the GaoTransform composes to.
Mat16 group_local_matrix(const placement::GaoTransform& g) {
    const double x = g.rotation[0], y = g.rotation[1], z = g.rotation[2],
                 w = g.rotation[3];
    const double R[3][3] = {
        {1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w),
         2.0 * (x * z + y * w)},
        {2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z),
         2.0 * (y * z - x * w)},
        {2.0 * (x * z - y * w), 2.0 * (y * z + x * w),
         1.0 - 2.0 * (x * x + y * y)},
    };
    Mat16 m{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m[r * 4 + c] = R[r][c] * g.scale[c];
        m[r * 4 + 3] = g.position[r];
    }
    m[15] = 1.0;
    return m;
}

// Combine a display-space group transform with a Jade-space base matrix,
// returning Jade's native matrix layout (the type-flag word is derived
// at the write site).
//
// Math: M_jade = J_dj · group_display · J_jd · base_jade, then DECOMPOSE
// the result into rotation (unit columns) + per-axis scale + translation
// and pack into Jade's struct layout:
//
//     float Ix, Iy, Iz, Sx;    // [0..3]: rot col 0 + scale X
//     float Jx, Jy, Jz, Sy;    // [4..7]: rot col 1 + scale Y
//     float Kx, Ky, Kz, Sz;    // [8..11]: rot col 2 + scale Z
//     float Tx, Ty, Tz, w;     // [12..15]: translation + w
//
// The S? slots are NOT homogeneous-row padding — they're per-axis scale
// read by MATH_GetScale ONLY when lType's scale bit is set. Shipped GAOs
// have *garbage* in slot [15] (w); we force a clean (0,0,0,1) bottom row
// during multiplication and restore the original w on output so the
// on-disk byte pattern matches the engine's expectation.
Mat16 compose_world_matrix_jade(const placement::GaoTransform& group,
                                const Mat16& base_flat) {
    const Mat16 g = group_local_matrix(group);
    // Rebuild a clean math 4×4 from the rotation columns + translation
    // of the base, ignoring any junk in the scale/w slots (they're NOT
    // standard-matrix bottom-row values).
    Mat16 base{};
    for (int r = 0; r < 3; ++r) {
        base[r * 4 + 0] = base_flat[0 + r];
        base[r * 4 + 1] = base_flat[4 + r];
        base[r * 4 + 2] = base_flat[8 + r];
        base[r * 4 + 3] = base_flat[12 + r];
    }
    base[15] = 1.0;

    const Mat16 result =
        mat4_mul(J_DJ_4x4, mat4_mul(g, mat4_mul(J_JD_4x4, base)));

    // Decompose top-left 3×3 into rotation (orthonormal columns) +
    // per-axis scale (column magnitudes). Assumes no shear — which holds
    // for any TRS the gizmo can produce.
    auto col = [&](int j) -> std::array<double, 3> {
        return {result[0 * 4 + j], result[1 * 4 + j], result[2 * 4 + j]};
    };
    auto norm3 = [](const std::array<double, 3>& v) {
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    };
    const std::array<double, 3> c0 = col(0), c1 = col(1), c2 = col(2);
    const double Sx = norm3(c0), Sy = norm3(c1), Sz = norm3(c2);
    // Build orthonormal rotation columns by dividing out each column's
    // magnitude; degenerate zero columns fall back to the identity axis.
    auto norm_col = [](const std::array<double, 3>& c, double s,
                       int j) -> std::array<double, 3> {
        if (s > 1e-9) return {c[0] / s, c[1] / s, c[2] / s};
        std::array<double, 3> out{0.0, 0.0, 0.0};
        out[size_t(j)] = 1.0;
        return out;
    };
    const std::array<double, 3> I_col = norm_col(c0, Sx, 0);
    const std::array<double, 3> J_col = norm_col(c1, Sy, 1);
    const std::array<double, 3> K_col = norm_col(c2, Sz, 2);
    const std::array<double, 3> T = {result[3], result[7], result[11]};

    const bool is_scale_identity = std::abs(Sx - 1) < 1e-5
                                   && std::abs(Sy - 1) < 1e-5
                                   && std::abs(Sz - 1) < 1e-5;

    Mat16 out{};
    out[0] = I_col[0]; out[1] = I_col[1]; out[2] = I_col[2];
    out[3] = is_scale_identity ? 0.0 : Sx;
    out[4] = J_col[0]; out[5] = J_col[1]; out[6] = J_col[2];
    out[7] = is_scale_identity ? 0.0 : Sy;
    out[8] = K_col[0]; out[9] = K_col[1]; out[10] = K_col[2];
    out[11] = is_scale_identity ? 0.0 : Sz;
    out[12] = T[0]; out[13] = T[1]; out[14] = T[2];
    out[15] = base_flat[15];
    return out;
}

// ── GAO identity flags (from OBJconst.h; gao.py GAO_FLAGS) ───────────
struct GaoFlagName { quint32 bit; const char* name; };
constexpr GaoFlagName GAO_FLAG_NAMES[] = {
    {0x00000001, "Bone"},        {0x00000002, "Anim"},
    {0x00000100, "ColMap"},      {0x00000200, "ZDM"},
    {0x00000400, "ZDE"},         {0x00001000, "Base"},
    {0x00002000, "Extended"},    {0x00004000, "Visual"},
    {0x00010000, "InitialPos"},  {0x00040000, "Links"},
    {0x00080000, "OBBox"},       {0x00200000, "AddMatrix"},
    {0x00400000, "Hierarchy"},   {0x00800000, "Group"},
    {0x02000000, "Events"},      {0x04000000, "FlashMatrix"},
    {0x08000000, "Sound"},       {0x10000000, "ODE"},
};

QStringList gao_flag_names(quint32 identity) {
    QStringList out;
    for (const GaoFlagName& f : GAO_FLAG_NAMES)
        if (identity & f.bit) out << QLatin1String(f.name);
    return out;
}

// object_kinds.category_label / category_color (GUI-side tables).
QString category_label(const QString& cat) {
    static const std::map<QString, QString> labels = {
        {QStringLiteral("camera"), QStringLiteral("Camera")},
        {QStringLiteral("light"), QStringLiteral("Light")},
        {QStringLiteral("sound"), QStringLiteral("Sound")},
        {QStringLiteral("fx"), QStringLiteral("FX / Particle")},
        {QStringLiteral("trigger"), QStringLiteral("Trigger / Sector")},
        {QStringLiteral("trap"), QStringLiteral("Trap")},
        {QStringLiteral("actor"), QStringLiteral("Actor / Enemy")},
        {QStringLiteral("spawner"), QStringLiteral("Spawner")},
        {QStringLiteral("waypoint"), QStringLiteral("Waypoint / Path")},
        {QStringLiteral("logic"), QStringLiteral("Logic / Manager")},
        {QStringLiteral("other"), QStringLiteral("Other")},
    };
    auto it = labels.find(cat);
    return it != labels.end() ? it->second : QStringLiteral("Other");
}

std::array<float, 3> category_color3(const QString& cat) {
    struct Row { const char* name; float r, g, b; };
    static const Row rows[] = {
        {"camera", 0.30f, 0.55f, 1.00f},  {"light", 1.00f, 0.85f, 0.20f},
        {"sound", 0.20f, 0.90f, 0.90f},   {"fx", 1.00f, 0.40f, 0.90f},
        {"trigger", 0.30f, 0.90f, 0.40f}, {"trap", 1.00f, 0.25f, 0.20f},
        {"actor", 1.00f, 0.55f, 0.10f},   {"spawner", 1.00f, 0.35f, 0.10f},
        {"waypoint", 0.55f, 1.00f, 0.45f},{"logic", 0.60f, 0.60f, 0.65f},
        {"other", 0.75f, 0.75f, 0.78f},
    };
    for (const Row& r : rows)
        if (cat == QLatin1String(r.name)) return {r.r, r.g, r.b};
    return {0.75f, 0.75f, 0.78f};
}

// ── Colours (object_placer._object_color / _preview_color / …) ───────

std::array<float, 3> hsv_to_rgb(double h, double s, double v) {
    // colorsys.hsv_to_rgb.
    if (s == 0.0) return {float(v), float(v), float(v)};
    const int i = int(h * 6.0) % 6;
    const double f = h * 6.0 - std::floor(h * 6.0);
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));
    switch (i) {
        case 0: return {float(v), float(t), float(p)};
        case 1: return {float(q), float(v), float(p)};
        case 2: return {float(p), float(v), float(t)};
        case 3: return {float(p), float(q), float(v)};
        case 4: return {float(t), float(p), float(v)};
        default: return {float(v), float(p), float(q)};
    }
}

std::array<float, 4> object_color(int index, quint32 key, bool has_collision) {
    const double hue = std::fmod(
        double(key & 0xFFFF) * 0.00006103515625 + index * 0.61803398875, 1.0);
    const double sat = has_collision ? 0.36 : 0.42;
    const double val = has_collision ? 0.82 : 0.74;
    std::array<float, 3> rgb = hsv_to_rgb(hue, sat, val);
    if (has_collision) {
        rgb[0] = std::min(1.0f, rgb[0] + 0.08f);
        rgb[1] = std::min(1.0f, rgb[1] + 0.05f);
    }
    return {rgb[0], rgb[1], rgb[2], 1.0f};
}

std::array<float, 4> preview_color(int index) {
    static const std::array<float, 4> palette[] = {
        {0.10f, 0.85f, 0.62f, 1.0f},
        {0.95f, 0.52f, 0.26f, 1.0f},
        {0.30f, 0.68f, 0.96f, 1.0f},
        {0.90f, 0.74f, 0.22f, 1.0f},
    };
    return palette[size_t(index) % 4];
}

// Hand-picked palette of 20 maximally-distinct colours for collision-
// source wireframes (Kelly-derived; see the Python for provenance).
constexpr float COLLISION_DISTINCT_PALETTE[][3] = {
    {1.00f, 0.70f, 0.00f}, {0.76f, 0.00f, 0.13f}, {0.00f, 0.49f, 0.20f},
    {0.00f, 0.33f, 0.54f}, {1.00f, 0.41f, 0.00f}, {0.50f, 0.24f, 0.46f},
    {0.95f, 0.46f, 0.55f}, {1.00f, 0.56f, 0.00f}, {0.33f, 0.22f, 0.48f},
    {0.96f, 0.78f, 0.00f}, {0.70f, 0.16f, 0.32f}, {0.94f, 0.48f, 0.36f},
    {0.50f, 0.13f, 0.05f}, {0.58f, 0.67f, 0.00f}, {0.95f, 0.23f, 0.07f},
    {0.13f, 0.70f, 0.67f}, {0.81f, 0.74f, 0.43f}, {0.65f, 0.74f, 0.84f},
    {0.00f, 0.62f, 0.45f}, {0.90f, 0.62f, 0.00f},
};

std::array<float, 4> collision_color(int index, bool staged = false) {
    if (staged) {
        static const std::array<float, 4> staged_palette[] = {
            {1.0f, 0.78f, 0.18f, 0.82f},
            {0.24f, 0.95f, 1.0f, 0.82f},
            {0.92f, 0.52f, 1.0f, 0.82f},
        };
        return staged_palette[size_t(index) % 3];
    }
    constexpr int N = int(sizeof(COLLISION_DISTINCT_PALETTE)
                          / sizeof(COLLISION_DISTINCT_PALETTE[0]));
    if (index < N) {
        const float* c = COLLISION_DISTINCT_PALETTE[index];
        return {c[0], c[1], c[2], 0.78f};
    }
    // Golden-angle fallback for very dense scenes.
    const int overflow = index - N;
    const double hue = std::fmod(overflow * 0.61803398875, 1.0);
    const double sat = (overflow & 1) == 0 ? 0.85 : 0.55;
    const double val = (overflow & 2) == 0 ? 0.95 : 0.70;
    const std::array<float, 3> rgb = hsv_to_rgb(hue, sat, val);
    return {rgb[0], rgb[1], rgb[2], 0.78f};
}

// ── little parsing helpers ───────────────────────────────────────────

quint32 rd_u32(const uint8_t* d, size_t off) {
    quint32 v;
    std::memcpy(&v, d + off, 4);
    return v;
}
quint16 rd_u16(const uint8_t* d, size_t off) {
    quint16 v;
    std::memcpy(&v, d + off, 2);
    return v;
}
float rd_f32(const uint8_t* d, size_t off) {
    float v;
    std::memcpy(&v, d + off, 4);
    return v;
}
float bits_to_f32(quint32 bits) {
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

// "0x%08x" string → u32 (0 on parse failure).
quint32 parse_hex_key(const std::string& s) {
    if (s.empty()) return 0;
    return quint32(strtoul(s.c_str(), nullptr, 16));
}

// LightField bits → (r,g,b) from 0x00BBGGRR.
std::array<int, 3> light_rgb(quint32 bits) {
    return {int(bits & 0xFF), int((bits >> 8) & 0xFF),
            int((bits >> 16) & 0xFF)};
}

// ── zone-object collection (object_placer load_zone_info half) ───────

// [gro_type:u32][payload], trimmed to the declared size (Python
// _sub_full_content).
std::vector<uint8_t> sub_full_content(const jade::SubEntry& sub) {
    std::vector<uint8_t> full(4 + sub.data.size());
    const quint32 gt = sub.gro_null ? 0 : sub.gro_type;
    std::memcpy(full.data(), &gt, 4);
    if (!sub.data.empty())
        std::memcpy(full.data() + 4, sub.data.data(), sub.data.size());
    const size_t size = sub.size ? sub.size : full.size();
    if (size > 0 && size <= full.size()) full.resize(size);
    return full;
}

using SubPtrMap = std::unordered_map<uint32_t, const jade::SubEntry*>;

SubPtrMap build_sub_ptr_map(const std::vector<jade::SubEntry>& subs) {
    SubPtrMap out;
    out.reserve(subs.size());
    for (const jade::SubEntry& s : subs) out.emplace(s.key, &s);
    return out;
}

// ── preview-geometry containers + transforms ─────────────────────────

// Simple triangle soup for collision previews (_collision_geo_from_*).
struct ColGeo {
    std::vector<std::array<double, 3>> verts;
    std::vector<std::array<quint32, 3>> faces;
};

// arr @ mat[:3,:3] + mat[3,:3] with mat = flat 16 reshaped row-major —
// the Jade col-major layout transforms exactly as M·v with columns
// I/J/K + translation at [12..14]. Output is display space (Y-up).
void transform_point_display(double x, double y, double z, const Mat16& m,
                             float* out3) {
    const double tx = x * m[0] + y * m[4] + z * m[8] + m[12];
    const double ty = x * m[1] + y * m[5] + z * m[9] + m[13];
    const double tz = x * m[2] + y * m[6] + z * m[10] + m[14];
    // jade → display: (x, z, -y).
    out3[0] = float(tx);
    out3[1] = float(tz);
    out3[2] = float(-ty);
}

void transform_normal_display(double x, double y, double z, const Mat16& m,
                              float* out3) {
    const double tx = x * m[0] + y * m[4] + z * m[8];
    const double ty = x * m[1] + y * m[5] + z * m[9];
    const double tz = x * m[2] + y * m[6] + z * m[10];
    double len = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (len < 1e-8) len = 1.0;
    out3[0] = float(tx / len);
    out3[1] = float(tz / len);
    out3[2] = float(-ty / len);
}

// object_placer._mesh_from_geo for a parsed zone GEO (jade::GeoInfo).
// faces stride 7: (i0,i1,i2,uv0,uv1,uv2,elem).
MeshDict mesh_from_geoinfo(const jade::GeoInfo& geo,
                           const std::vector<float>* rli /*4n or null*/,
                           const Mat16& matrix, const QString& name,
                           const std::array<float, 4>& color, bool pickable) {
    MeshDict md;
    md.name = name;
    md.base_color = color;
    md.pickable = pickable;
    md.depth_offset = !pickable;

    const size_t n = geo.vertices.size() / 3;
    md.vertices.resize(n * 3);
    for (size_t i = 0; i < n; ++i)
        transform_point_display(geo.vertices[i * 3], geo.vertices[i * 3 + 1],
                                geo.vertices[i * 3 + 2], matrix,
                                md.vertices.data() + i * 3);
    if (geo.normals.size() == n * 3) {
        md.normals.resize(n * 3);
        for (size_t i = 0; i < n; ++i)
            transform_normal_display(geo.normals[i * 3],
                                     geo.normals[i * 3 + 1],
                                     geo.normals[i * 3 + 2], matrix,
                                     md.normals.data() + i * 3);
    }
    md.uvs.assign(geo.uvs.begin(), geo.uvs.end());
    const size_t nb_tris = geo.faces.size() / 7;
    md.faces.reserve(nb_tris * 3);
    md.face_uvs.reserve(nb_tris * 3);
    for (size_t t = 0; t < nb_tris; ++t) {
        const uint16_t* f = geo.faces.data() + t * 7;
        md.faces.push_back(f[0]);
        md.faces.push_back(f[1]);
        md.faces.push_back(f[2]);
        md.face_uvs.push_back(f[3]);
        md.face_uvs.push_back(f[4]);
        md.face_uvs.push_back(f[5]);
    }
    for (size_t e = 0; e + 1 < geo.elements.size(); e += 2) {
        placement::ElementInfo ei;
        ei.nTri = int(geo.elements[e]);
        md.elements.push_back(ei);
    }
    if (rli && rli->size() == n * 4) {
        md.rli_colors = *rli;
        md.rli_comps = 4;
    }
    return md;
}

// object_placer._mesh_from_geo for a generated primitive
// (jade::placer::PlacerGeo — the _geo_dict shape).
MeshDict mesh_from_placer_geo(const jade::placer::PlacerGeo& geo,
                              const Mat16& matrix, const QString& name,
                              const std::array<float, 4>& color,
                              bool pickable) {
    MeshDict md;
    md.name = name;
    md.base_color = color;
    md.pickable = pickable;
    md.depth_offset = !pickable;

    const size_t n = geo.vertices.size();
    md.vertices.resize(n * 3);
    for (size_t i = 0; i < n; ++i)
        transform_point_display(geo.vertices[i][0], geo.vertices[i][1],
                                geo.vertices[i][2], matrix,
                                md.vertices.data() + i * 3);
    if (geo.normals.size() == n) {
        md.normals.resize(n * 3);
        for (size_t i = 0; i < n; ++i)
            transform_normal_display(geo.normals[i][0], geo.normals[i][1],
                                     geo.normals[i][2], matrix,
                                     md.normals.data() + i * 3);
    }
    md.uvs.reserve(geo.uvs.size() * 2);
    for (const auto& uv : geo.uvs) {
        md.uvs.push_back(float(uv[0]));
        md.uvs.push_back(float(uv[1]));
    }
    md.faces.reserve(geo.faces.size() * 3);
    for (const auto& f : geo.faces) {
        md.faces.push_back(f[0]);
        md.faces.push_back(f[1]);
        md.faces.push_back(f[2]);
    }
    md.face_uvs.reserve(geo.face_uvs.size() * 3);
    for (const auto& f : geo.face_uvs) {
        md.face_uvs.push_back(f[0]);
        md.face_uvs.push_back(f[1]);
        md.face_uvs.push_back(f[2]);
    }
    for (const auto& e : geo.elements) {
        placement::ElementInfo ei;
        ei.nTri = int(e.n_tri);
        md.elements.push_back(ei);
    }
    if (md.elements.empty() && !geo.faces.empty()) {
        placement::ElementInfo ei;
        ei.nTri = int(geo.faces.size());
        md.elements.push_back(ei);
    }
    return md;
}

// Replacement of an existing GAO can resize a parsed source GEO. NumPy's
// implementation scales float32 vertices and, when source normals exist,
// regenerates smooth vertex normals from the resized triangles.
jade::GeoInfo scaled_replacement_geo(const jade::GeoInfo& source,
                                     const V3& scale) {
    jade::GeoInfo geo = source;
    const V3 dimensions = jade::placer::vec3_min(scale, 0.001);
    bool identity = true;
    for (double value : dimensions)
        if (std::abs(value - 1.0) >= 1e-6) identity = false;
    if (identity) return geo;

    for (size_t i = 0; i + 2 < geo.vertices.size(); i += 3) {
        geo.vertices[i] = float(geo.vertices[i] * float(dimensions[0]));
        geo.vertices[i + 1] =
            float(geo.vertices[i + 1] * float(dimensions[1]));
        geo.vertices[i + 2] =
            float(geo.vertices[i + 2] * float(dimensions[2]));
    }
    if (geo.normals.empty()) return geo;

    const size_t n = geo.vertices.size() / 3;
    std::vector<std::array<float, 3>> normals(n, {0.0f, 0.0f, 0.0f});
    for (size_t t = 0; t + 6 < geo.faces.size(); t += 7) {
        const uint16_t ia = geo.faces[t], ib = geo.faces[t + 1],
                       ic = geo.faces[t + 2];
        if (ia >= n || ib >= n || ic >= n) continue;
        const float ax = geo.vertices[size_t(ia) * 3],
                    ay = geo.vertices[size_t(ia) * 3 + 1],
                    az = geo.vertices[size_t(ia) * 3 + 2];
        const float bx = geo.vertices[size_t(ib) * 3],
                    by = geo.vertices[size_t(ib) * 3 + 1],
                    bz = geo.vertices[size_t(ib) * 3 + 2];
        const float cx = geo.vertices[size_t(ic) * 3],
                    cy = geo.vertices[size_t(ic) * 3 + 1],
                    cz = geo.vertices[size_t(ic) * 3 + 2];
        const float ux = bx - ax, uy = by - ay, uz = bz - az;
        const float vx = cx - ax, vy = cy - ay, vz = cz - az;
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length <= 1e-8f) continue;
        nx /= length;
        ny /= length;
        nz /= length;
        for (uint16_t index : {ia, ib, ic}) {
            normals[index][0] += nx;
            normals[index][1] += ny;
            normals[index][2] += nz;
        }
    }
    geo.normals.assign(n * 3, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        float nx = normals[i][0], ny = normals[i][1], nz = normals[i][2];
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 1e-8f) {
            nx /= length;
            ny /= length;
            nz /= length;
        } else {
            nx = ny = 0.0f;
            nz = 1.0f;
        }
        geo.normals[i * 3] = nx;
        geo.normals[i * 3 + 1] = ny;
        geo.normals[i * 3 + 2] = nz;
    }
    return geo;
}

// object_placer._collision_mesh_from_geo (wireframe overlay flags).
MeshDict collision_mesh_from_colgeo(const ColGeo& geo, const Mat16& matrix,
                                    const QString& name,
                                    const std::array<float, 4>& color) {
    MeshDict md;
    md.name = name;
    md.base_color = color;
    md.pickable = false;
    md.wireframe = true;
    md.xray = true;
    md.line_width = 1.6f;
    md.depth_offset = false;

    const size_t n = geo.verts.size();
    md.vertices.resize(n * 3);
    for (size_t i = 0; i < n; ++i)
        transform_point_display(geo.verts[i][0], geo.verts[i][1],
                                geo.verts[i][2], matrix,
                                md.vertices.data() + i * 3);
    md.faces.reserve(geo.faces.size() * 3);
    for (const auto& f : geo.faces) {
        md.faces.push_back(f[0]);
        md.faces.push_back(f[1]);
        md.faces.push_back(f[2]);
    }
    return md;
}

ColGeo colgeo_from_placer_geo(const jade::placer::PlacerGeo& g,
                              const std::array<double, 3>& offset) {
    ColGeo out;
    out.verts.reserve(g.vertices.size());
    for (const auto& v : g.vertices)
        out.verts.push_back(
            {v[0] + offset[0], v[1] + offset[1], v[2] + offset[2]});
    out.faces = g.faces;
    return out;
}

// object_placer._collision_box_geo.
ColGeo collision_box_geo(const std::array<double, 3>& mn,
                         const std::array<double, 3>& mx) {
    ColGeo g;
    g.verts = {
        {mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]},
        {mn[0], mx[1], mn[2]}, {mx[0], mx[1], mn[2]},
        {mn[0], mn[1], mx[2]}, {mx[0], mn[1], mx[2]},
        {mn[0], mx[1], mx[2]}, {mx[0], mx[1], mx[2]},
    };
    g.faces = {
        {0, 2, 3}, {3, 1, 0}, {4, 5, 7}, {7, 6, 4},
        {0, 1, 5}, {5, 4, 0}, {1, 3, 7}, {7, 5, 1},
        {3, 2, 6}, {6, 7, 3}, {2, 0, 4}, {4, 6, 2},
    };
    return g;
}

// object_placer._collision_geo_from_cob_full (all four COB shapes).
bool collision_geo_from_cob_full(const std::vector<uint8_t>& full,
                                 ColGeo& out) {
    if (full.size() < 6) return false;
    const uint8_t* d = full.data();
    const size_t n = full.size();
    const int cob_type = d[4];
    if (cob_type == jade::COL_ZONE_TRIANGLES) {
        size_t off = 6;
        if (off + 4 > n) return false;
        const quint32 point_count = rd_u32(d, off);
        off += 4;
        if (point_count == 0 || point_count > 200000
            || off + size_t(point_count) * 12 > n)
            return false;
        out.verts.resize(point_count);
        for (quint32 i = 0; i < point_count; ++i)
            out.verts[i] = {rd_f32(d, off + i * 12),
                            rd_f32(d, off + i * 12 + 4),
                            rd_f32(d, off + i * 12 + 8)};
        off += size_t(point_count) * 12;
        if (off + 4 > n) return false;
        const quint32 face_count = rd_u32(d, off);
        off += 4;
        if (face_count > 500000 || off + size_t(face_count) * 12 > n)
            return false;
        off += size_t(face_count) * 12;
        if (off + 4 > n) return false;
        const quint32 element_count = rd_u32(d, off);
        off += 4;
        if (element_count == 0 || element_count > 65535) return false;
        for (quint32 e = 0; e < element_count; ++e) {
            if (off + 8 > n) return false;
            const quint16 tri_count = rd_u16(d, off);
            off += 8;
            if (tri_count > 500000 || off + size_t(tri_count) * 6 > n)
                return false;
            for (quint16 t = 0; t < tri_count; ++t) {
                const quint16 a = rd_u16(d, off), b = rd_u16(d, off + 2),
                              c = rd_u16(d, off + 4);
                off += 6;
                if (a < point_count && b < point_count && c < point_count
                    && a != b && a != c && b != c)
                    out.faces.push_back({a, b, c});
            }
        }
        return !out.faces.empty();
    }
    if (cob_type == 1) {
        const size_t off = 6 + 4;
        if (off + 24 > n) return false;
        std::array<double, 3> bmax = {rd_f32(d, off), rd_f32(d, off + 4),
                                      rd_f32(d, off + 8)};
        std::array<double, 3> bmin = {rd_f32(d, off + 12),
                                      rd_f32(d, off + 16),
                                      rd_f32(d, off + 20)};
        std::array<double, 3> mn, mx;
        for (int i = 0; i < 3; ++i) {
            mn[size_t(i)] = std::min(bmin[size_t(i)], bmax[size_t(i)]);
            mx[size_t(i)] = std::max(bmin[size_t(i)], bmax[size_t(i)]);
        }
        out = collision_box_geo(mn, mx);
        return true;
    }
    if (cob_type == 2) {
        const size_t off = 6 + 4;
        if (off + 16 > n) return false;
        const std::array<double, 3> center = {
            rd_f32(d, off), rd_f32(d, off + 4), rd_f32(d, off + 8)};
        const double radius = std::abs(double(rd_f32(d, off + 12)));
        if (radius <= 1e-6) return false;
        out = colgeo_from_placer_geo(
            jade::placer::build_sphere_geometry(radius * 2.0, radius * 2.0,
                                                radius * 2.0, quint32(-1)),
            center);
        return true;
    }
    if (cob_type == 3) {
        const size_t off = 6 + 4;
        if (off + 20 > n) return false;
        const std::array<double, 3> center = {
            rd_f32(d, off), rd_f32(d, off + 4), rd_f32(d, off + 8)};
        const double radius = std::abs(double(rd_f32(d, off + 12)));
        const double height = std::abs(double(rd_f32(d, off + 16)));
        if (radius <= 1e-6 || height <= 1e-6) return false;
        out = colgeo_from_placer_geo(
            jade::placer::build_cylinder_geometry(
                radius * 2.0, radius * 2.0, height, quint32(-1), 20),
            center);
        return true;
    }
    return false;
}

bool collision_geo_from_cob_sub(const jade::SubEntry* sub, ColGeo& out) {
    if (!jade::looks_like_cob_sub(sub)) return false;
    return collision_geo_from_cob_full(sub_full_content(*sub), out);
}

// object_placer._collision_meshes_for_gao_payload.
std::vector<MeshDict> collision_meshes_for_gao_payload(
    const std::vector<uint8_t>& gao_payload, const SubPtrMap& by_key,
    const Mat16& matrix_values, const QString& name,
    const std::array<float, 4>& color, long long owner_gao_key = -1) {
    std::vector<MeshDict> meshes;
    if (gao_payload.empty()) return meshes;
    std::unordered_set<quint32> seen_colmaps;
    const auto offsets =
        jade::gao_colmap_key_offsets(gao_payload, by_key);
    for (const auto& [offset, colmap_key_raw] : offsets) {
        (void)offset;
        const quint32 colmap_key = colmap_key_raw;
        if (!seen_colmaps.insert(colmap_key).second) continue;
        auto it = by_key.find(colmap_key);
        const jade::SubEntry* colmap_sub =
            it == by_key.end() ? nullptr : it->second;
        int cob_index = 0;
        for (quint32 cob_key : jade::colmap_cob_keys(colmap_sub, by_key)) {
            ColGeo cob_geo;
            auto cit = by_key.find(cob_key);
            if (cit == by_key.end()
                || !collision_geo_from_cob_sub(cit->second, cob_geo)) {
                ++cob_index;
                continue;
            }
            QString label = QStringLiteral("%1 %2/%3").arg(
                name, hex_key(colmap_key), hex_key(cob_key));
            if (cob_index)
                label = QStringLiteral("%1 [%2]").arg(label).arg(cob_index + 1);
            MeshDict mesh =
                collision_mesh_from_colgeo(cob_geo, matrix_values, label,
                                           color);
            // gao_key doubles as the Python's collision_owner_key tag so
            // collision-follow can hide a dedicated host's old-position
            // wireframe while its orange moved-position ghost is shown.
            mesh.gao_key = owner_gao_key;
            meshes.push_back(std::move(mesh));
            ++cob_index;
        }
    }
    return meshes;
}

// _bounds_from_points over a PlacerGeo.
void bounds_from_placer_geo(const jade::placer::PlacerGeo& geo,
                            std::array<double, 3>& mn,
                            std::array<double, 3>& mx) {
    mn = {0.0, 0.0, 0.0};
    mx = {0.0, 0.0, 0.0};
    bool first = true;
    for (const auto& v : geo.vertices) {
        if (first) {
            mn = v;
            mx = v;
            first = false;
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            mn[size_t(i)] = std::min(mn[size_t(i)], v[size_t(i)]);
            mx[size_t(i)] = std::max(mx[size_t(i)], v[size_t(i)]);
        }
    }
}

// ── texture decode + per-element resolution ──────────────────────────

// Decode every non-placeholder texture in the entry (QImage RGBA copy).
std::map<quint32, QImage> decode_zone_textures(
    const std::vector<jade::SubEntry>& subs) {
    std::map<quint32, QImage> out;
    for (const jade::SubEntry& sub : subs) {
        const uint8_t* d = sub.data.data();
        const size_t n = sub.data.size();
        if (!n || !jade::is_texture_entry(d, n)) continue;
        const jade::TexInfo ti = jade::parse_texture(d, n);
        if (!ti.valid) continue;
        const size_t pixdata_len = n > ti.pix_start ? n - ti.pix_start : 0;
        if (jade::is_placeholder(ti, pixdata_len)) continue;
        const std::vector<uint8_t>* pal = jade::palette_for_texture(ti, subs);
        const std::vector<uint8_t> rgba = jade::decode_texture(
            d, n, ti, pal ? pal->data() : nullptr, pal ? pal->size() : 0);
        if (rgba.size() < size_t(ti.width) * ti.height * 4) continue;
        QImage img(int(ti.width), int(ti.height), QImage::Format_RGBA8888);
        for (uint32_t row = 0; row < ti.height; ++row)
            std::memcpy(img.scanLine(int(row)),
                        rgba.data() + size_t(row) * ti.width * 4,
                        size_t(ti.width) * 4);
        out.emplace(sub.key, std::move(img));
    }
    return out;
}

// tex_for_submat: sub-material key → diffuse texture (null QImage = None).
QImage tex_for_submat(quint32 sub_key, const SubPtrMap& by_key,
                      const std::map<quint32, QImage>& tex_by_key) {
    auto it = by_key.find(sub_key);
    if (it == by_key.end()) return QImage();
    const jade::SubEntry* sub = it->second;
    const quint32 tk =
        jade::resolve_texture_key(sub->data.data(), sub->data.size());
    if (!tk) return QImage();
    auto tit = tex_by_key.find(tk);
    return tit == tex_by_key.end() ? QImage() : tit->second;
}

// object_placer.resolve_textures_for_material (per-element diffuse for
// one GAO under a given material reference).
std::vector<QImage> resolve_textures_for_material_impl(
    quint32 material_key, const std::vector<int>& element_matids,
    const SubPtrMap& by_key, const std::map<quint32, QImage>& tex_by_key) {
    const size_t n_el = std::max<size_t>(1, element_matids.size());
    std::vector<QImage> per_el(n_el);
    if (!material_key || material_key == INVALID_KEY) return per_el;
    auto it = by_key.find(material_key);
    if (it == by_key.end()) return per_el;
    const jade::SubEntry* mat_sub = it->second;
    if (mat_sub->gro_null) return per_el;
    const int gro_type = int(mat_sub->gro_type);
    if (gro_type == 4) {
        const jade::MatInfo mm = jade::parse_material(
            mat_sub->data.data(), mat_sub->data.size(), 4);
        if (!mm.ok) return per_el;
        for (size_t i = 0; i < element_matids.size(); ++i) {
            const int mi = element_matids[i];
            if (mi >= 0 && size_t(mi) < mm.sub_material_keys.size())
                per_el[i] = tex_for_submat(mm.sub_material_keys[size_t(mi)],
                                           by_key, tex_by_key);
        }
    } else if (gro_type == 3 || gro_type == 5) {
        const quint32 tk = jade::resolve_texture_key(mat_sub->data.data(),
                                                     mat_sub->data.size());
        QImage rgba;
        if (tk) {
            auto tit = tex_by_key.find(tk);
            if (tit != tex_by_key.end()) rgba = tit->second;
        }
        for (size_t i = 0; i < n_el; ++i) per_el[i] = rgba;
    }
    return per_el;
}

// element matIds of a parsed GEO (flat (nTri, matId) pairs).
std::vector<int> geoinfo_matids(const jade::GeoInfo& geo) {
    std::vector<int> out;
    for (size_t e = 0; e + 1 < geo.elements.size(); e += 2)
        out.push_back(int(geo.elements[e + 1]));
    return out;
}

// object_placer._resolve_textures_for_objects.
placement::TexturesByKey resolve_textures_for_objects(
    const std::vector<ZoneObject>& objects, const SubPtrMap& by_key,
    const std::map<quint32, QImage>& tex_by_key) {
    placement::TexturesByKey out;
    if (tex_by_key.empty()) return out;
    for (const ZoneObject& obj : objects) {
        if (obj.is_marker || !obj.geo) continue;
        if (obj.material_key <= 0
            || quint32(obj.material_key) == INVALID_KEY)
            continue;
        std::vector<QImage> per_el = resolve_textures_for_material_impl(
            quint32(obj.material_key), geoinfo_matids(*obj.geo), by_key,
            tex_by_key);
        bool any = false;
        for (const QImage& t : per_el)
            if (!t.isNull()) { any = true; break; }
        if (any) out[obj.key] = std::move(per_el);
    }
    return out;
}

// Material combo label: the Python shows parse_material's 'type' string.
QString material_type_label(const jade::MatInfo& mat) {
    if (!mat.ok) return QStringLiteral("material");
    switch (mat.type) {
        case 0: return QStringLiteral("single");
        case 1: return QStringLiteral("multitexture");
        case 2: return QStringLiteral("multi");
        default: return QStringLiteral("material");
    }
}

// approx-equality over fixed-size tuples (Python _approx_eq, eps 1e-5).
template <size_t N>
bool approx_eq(const std::array<double, N>& a, const std::array<double, N>& b,
               double eps = 1e-5) {
    for (size_t i = 0; i < N; ++i)
        if (std::abs(a[i] - b[i]) > eps) return false;
    return true;
}

// gao.visual_block_offset — offset of the [gro_key][grm_key] pair, or -1.
long long visual_block_offset(const std::vector<uint8_t>& payload) {
    if (payload.size() < 16) return -1;
    const quint32 identity = rd_u32(payload.data(), 8);
    if (!(identity & 0x00004000u)) return -1;
    const quint32 name_size = rd_u32(payload.data(), 12);
    const size_t mat_off = 16 + size_t(name_size) + 10;
    const size_t bv_size = (identity & 0x00080000u) ? 48 : 24;
    const size_t vis_off = mat_off + 68 + bv_size;
    if (vis_off + 8 > payload.size()) return -1;
    return (long long)vis_off;
}

}  // namespace

// ── construction ─────────────────────────────────────────────────────

PlacementTab::PlacementTab(QWidget* parent) : QWidget(parent) {
    // Debounce ghost re-bake during a gizmo drag: rebuilding the scene
    // every tick would lag a big zone, so coalesce to one refresh once
    // the drag settles.
    ghost_refresh_timer_ = new QTimer(this);
    ghost_refresh_timer_->setSingleShot(true);
    ghost_refresh_timer_->setInterval(140);
    connect(ghost_refresh_timer_, &QTimer::timeout, this,
            [this] { refresh_preview(nullptr, true); });

    draft_refresh_timer_ = new QTimer(this);
    draft_refresh_timer_->setSingleShot(true);
    connect(draft_refresh_timer_, &QTimer::timeout, this,
            &PlacementTab::refresh_current_draft_now);

    auto* root = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal);

    // ── Left: zone tree ─────────────────────────────────────────
    auto* left = new QWidget;
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(4, 4, 4, 4);

    left_layout->addWidget(new QLabel(tr("Zones")));
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter by entry name, index, or key…"));
    connect(filter_, &QLineEdit::textChanged, this,
            &PlacementTab::apply_filter);
    left_layout->addWidget(filter_);

    entry_tree_ = new QTreeWidget;
    entry_tree_->setHeaderLabels({tr("Entry"), tr("Index"), tr("Key")});
    entry_tree_->setColumnWidth(0, 260);
    entry_tree_->setColumnWidth(1, 70);
    connect(entry_tree_, &QTreeWidget::itemClicked, this,
            &PlacementTab::on_entry_clicked);
    left_layout->addWidget(entry_tree_, 1);
    splitter->addWidget(left);

    // ── Centre: the viewport fills the space, with a short log
    // console beneath it (engine-style: big central view, docks at the
    // sides). The inspector controls move to a right-hand dock so the
    // viewport gets the full height instead of being squeezed above a
    // tall stack of settings.
    auto* center = new QWidget;
    auto* center_layout = new QVBoxLayout(center);
    center_layout->setContentsMargins(2, 2, 2, 2);
    center_layout->setSpacing(2);

    viewer_panel_ = new PlacementViewport;
    viewer_panel_->set_collision_toggle_visible(true);
    center_layout->addWidget(viewer_panel_, 1);

    log_ = new QTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumHeight(90);
    center_layout->addWidget(log_);
    splitter->addWidget(center);

    // ── Right: inspector dock (commit bar + Object/Create tabs) ──
    auto* inspector = new QWidget;
    auto* insp_layout = new QVBoxLayout(inspector);
    insp_layout->setContentsMargins(4, 4, 4, 4);
    insp_layout->setSpacing(4);

    // Map-wide commit bar — top of the dock so it's always visible
    // regardless of which tab the user is on. Tracks edits across
    // every GAO in the loaded zone, not just the selected one.
    auto* commit_bar = new QHBoxLayout;
    commit_bar->setContentsMargins(0, 0, 0, 0);
    map_dirty_label_ = new QLabel(QString());
    map_dirty_label_->setStyleSheet(
        QStringLiteral("color: #e6c34a; font-style: italic;"));
    commit_bar->addWidget(map_dirty_label_, 1);
    commit_map_btn_ = new QPushButton(tr("Commit Changes"));
    commit_map_btn_->setToolTip(
        tr("Send all edits across the loaded zone to the project as "
           "modify_transform / modify_gao_flags operations. Replaces any "
           "prior op for each edited GAO — no duplicates."));
    connect(commit_map_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_commit_map);
    commit_bar->addWidget(commit_map_btn_);
    discard_map_btn_ = new QPushButton(tr("Discard Changes"));
    discard_map_btn_->setToolTip(
        tr("Roll back every uncommitted edit in the loaded zone — "
           "restores the viewport to the last-committed state."));
    connect(discard_map_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_discard_map);
    commit_bar->addWidget(discard_map_btn_);
    insp_layout->addLayout(commit_bar);

    // Tabbed controls — each page scrolls so a tall inspector never
    // clips on smaller screens (and never forces the window taller).
    tabs_ = new QTabWidget;
    tabs_->addTab(wrap_scroll(build_object_tab()), tr("Object"));
    tabs_->addTab(wrap_scroll(build_create_tab()), tr("Create"));
    insp_layout->addWidget(tabs_, 1);

    inspector->setMinimumWidth(380);
    inspector->setMaximumWidth(720);
    splitter->addWidget(inspector);

    // zones | viewport(grows) | inspector(fixed-ish)
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setCollapsible(1, false);
    splitter->setSizes({260, 1500, 560});
    root->addWidget(splitter);

    // ── Viewport signal wiring ──────────────────────────────────
    PlacementViewport* v = viewer_panel_->viewer();
    connect(v, &PlacementViewport::point_picked, this,
            &PlacementTab::on_viewer_point_picked);
    connect(v, &PlacementViewport::gizmo_moved, this,
            &PlacementTab::on_viewer_gizmo_moved);
    connect(v, &PlacementViewport::object_selected, this,
            &PlacementTab::on_viewer_object_selected);
    // object_transformed fires for ANY gizmo (move/rotate/scale); the
    // inspector pulls full TRS via get_gao_transform.
    connect(v, &PlacementViewport::object_transformed, this,
            &PlacementTab::on_viewer_object_transformed);
    connect(viewer_panel_, &PlacementViewport::collision_toggled, this,
            &PlacementTab::on_collision_preview_toggled);
    connect(viewer_panel_, &PlacementViewport::markers_toggled, this,
            &PlacementTab::on_markers_toggled);

    set_create_controls_enabled(false);
    update_object_tab_for_selection(std::nullopt);
    on_kind_changed();
}

// ── Tab builders ─────────────────────────────────────────────────────

// Wrap a tab page in a vertical scroll area so a tall inspector scrolls
// instead of clipping / forcing the whole window taller.
QScrollArea* PlacementTab::wrap_scroll(QWidget* widget) {
    auto* sa = new QScrollArea;
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    sa->setWidget(widget);
    return sa;
}

QWidget* PlacementTab::build_object_tab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    inspector_header_ =
        new QLabel(tr("(no selection — click a mesh in the viewport)"));
    inspector_header_->setWordWrap(true);
    inspector_header_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    layout->addWidget(inspector_header_);

    inspector_meta_ = new QLabel(QString());
    inspector_meta_->setStyleSheet(QStringLiteral("color: #8FA89C;"));
    inspector_meta_->setWordWrap(true);
    layout->addWidget(inspector_meta_);

    // ── Details (read-only) ────────────────────────────────────
    // Laid out in THREE columns (Object | Geometry | Material/render)
    // so surfacing everything we can read about a GAO doesn't push the
    // window taller. Each cell is a right-aligned dim label + a
    // selectable monospace value.
    auto* details_group = new QGroupBox(tr("Details"));
    auto* details_grid = new QGridLayout(details_group);
    details_grid->setHorizontalSpacing(12);
    details_grid->setVerticalSpacing(2);
    details_grid->setContentsMargins(8, 4, 8, 4);
    const QString mono = QStringLiteral(
        "font-family: 'Consolas','Menlo',monospace; color: #D9E6DF;");
    d_col_rows_[0] = d_col_rows_[1] = d_col_rows_[2] = 0;

    auto cell = [&](int colgroup, const QString& name) -> QLabel* {
        const int r = d_col_rows_[colgroup];
        d_col_rows_[colgroup] += 1;
        auto* cap = new QLabel(name);
        cap->setStyleSheet(QStringLiteral("color:#8FA89C;"));
        cap->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* val = new QLabel(QStringLiteral("—"));
        val->setStyleSheet(mono);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        details_grid->addWidget(cap, r, colgroup * 2);
        details_grid->addWidget(val, r, colgroup * 2 + 1);
        return val;
    };

    // Column 0 — Object / transform identity
    d_gao_key_ = cell(0, tr("GAO key"));
    d_identity_ = cell(0, tr("Identity"));
    d_editor_ = cell(0, tr("Editor"));
    d_father_ = cell(0, tr("Father"));
    d_mat_type_ = cell(0, tr("Matrix type"));
    d_position_ = cell(0, tr("Baked pos"));
    // Column 1 — Geometry (GEO)
    d_geo_key_ = cell(1, tr("GEO key"));
    d_geo_hdr_ = cell(1, tr("GEO hdr"));
    d_geo_counts_ = cell(1, tr("Verts/Tris"));
    d_geo_counts2_ = cell(1, tr("UVs/Elem"));
    d_skin_ = cell(1, tr("Skin"));
    d_vtxcol_ = cell(1, tr("RLI verts"));
    // Column 2 — Material / render state
    d_mat_key_ = cell(2, tr("Material"));
    d_mat_kind_ = cell(2, tr("Mat kind"));
    d_texture_ = cell(2, tr("Texture"));
    d_drawmask_ = cell(2, tr("Draw mask"));
    d_field3_ = cell(2, tr("Vis flags"));
    d_collision_ = cell(2, tr("Collision"));
    // Full-width rows spanning all columns.
    const int full = std::max({d_col_rows_[0], d_col_rows_[1],
                               d_col_rows_[2]});
    auto* bv_cap = new QLabel(tr("Bounds"));
    bv_cap->setStyleSheet(QStringLiteral("color:#8FA89C;"));
    bv_cap->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d_bv_ = new QLabel(QStringLiteral("—"));
    d_bv_->setStyleSheet(mono);
    d_bv_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details_grid->addWidget(bv_cap, full, 0);
    details_grid->addWidget(d_bv_, full, 1, 1, 5);
    auto* fl_cap = new QLabel(tr("Flag names"));
    fl_cap->setStyleSheet(QStringLiteral("color:#8FA89C;"));
    fl_cap->setAlignment(Qt::AlignRight | Qt::AlignTop);
    d_flag_list_ = new QLabel(QStringLiteral("—"));
    d_flag_list_->setStyleSheet(mono);
    d_flag_list_->setWordWrap(true);
    d_flag_list_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details_grid->addWidget(fl_cap, full + 1, 0);
    details_grid->addWidget(d_flag_list_, full + 1, 1, 1, 5);
    for (int value_col : {1, 3, 5}) details_grid->setColumnStretch(value_col, 1);
    layout->addWidget(details_group);

    // ── Transform spinboxes ────────────────────────────────────
    auto* xform_group = new QGroupBox(tr("Transform"));
    auto* xform_form = new QFormLayout(xform_group);
    xform_form->setLabelAlignment(Qt::AlignRight);
    for (int i = 0; i < 3; ++i)
        insp_pos_.push_back(spin(-100000.0, 100000.0, 0.0, 0.1));
    for (int i = 0; i < 3; ++i)
        insp_rot_.push_back(spin(-720.0, 720.0, 0.0, 1.0, 2));
    for (int i = 0; i < 3; ++i)
        insp_scale_.push_back(spin(0.001, 1000.0, 1.0, 0.05, 4));
    for (QDoubleSpinBox* s : insp_pos_)
        connect(s, &QDoubleSpinBox::valueChanged, this,
                &PlacementTab::on_inspector_edited);
    for (QDoubleSpinBox* s : insp_rot_)
        connect(s, &QDoubleSpinBox::valueChanged, this,
                &PlacementTab::on_inspector_edited);
    for (QDoubleSpinBox* s : insp_scale_)
        connect(s, &QDoubleSpinBox::valueChanged, this,
                &PlacementTab::on_inspector_edited);
    xform_form->addRow(tr("Position XYZ"),
                       row({insp_pos_[0], insp_pos_[1], insp_pos_[2]}));
    xform_form->addRow(tr("Rotation XYZ°"),
                       row({insp_rot_[0], insp_rot_[1], insp_rot_[2]}));
    xform_form->addRow(tr("Scale XYZ"),
                       row({insp_scale_[0], insp_scale_[1], insp_scale_[2]}));

    auto* xform_actions = new QHBoxLayout;
    frame_sel_btn_ = new QPushButton(tr("Frame"));
    frame_sel_btn_->setToolTip(
        tr("Centre the camera on the selected object."));
    connect(frame_sel_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_inspector_frame);
    xform_actions->addWidget(frame_sel_btn_);
    xform_actions->addStretch();
    auto* actions_holder = new QWidget;
    actions_holder->setLayout(xform_actions);
    xform_form->addRow(QString(), actions_holder);
    layout->addWidget(xform_group);

    // ── Material picker ───────────────────────────────────────
    // Swaps the selected GAO's grm_key (material reference) to a
    // different one from the zone's material list. Applies live to the
    // viewport AND saves a ModifyGaoMaterial op on Commit (existing
    // GAOs) or updates placement.material_key in-place (pending
    // creates) so the BF build picks it up.
    auto* mat_group = new QGroupBox(tr("Material"));
    auto* mat_form = new QFormLayout(mat_group);
    mat_form->setLabelAlignment(Qt::AlignRight);
    insp_material_combo_ = new QComboBox;
    insp_material_combo_->setToolTip(
        tr("Repoints the GAO at a different material from the zone's "
           "material list. Live-previews in the viewport."));
    connect(insp_material_combo_, &QComboBox::activated, this,
            &PlacementTab::on_inspector_material_picked);
    mat_form->addRow(tr("Material"), insp_material_combo_);
    layout->addWidget(mat_group);

    // ── Light tuning (non-visual light markers only) ───────────
    // Shown only when the selected marker resolves to a GRO_Light
    // resource. Edits stage an EditLight op on Commit (in-place field
    // patch — see core.light / ops_light). Hidden otherwise.
    light_group_ = new QGroupBox(tr("Light"));
    light_group_->setToolTip(
        tr("Tune this light's type, colour, range and spot cone. "
           "Applied to the light resource on Commit."));
    auto* light_form = new QFormLayout(light_group_);
    light_form->setLabelAlignment(Qt::AlignRight);

    light_type_combo_ = new QComboBox;
    // light.LIGHT_TYPE_ORDER / LIGHT_TYPES.
    const std::pair<int, QString> light_types[] = {
        {0, tr("Omni")},        {1, tr("Directional")}, {2, tr("Spot")},
        {3, tr("Fog")},         {5, tr("Ambient")},
    };
    for (const auto& [tv, label] : light_types)
        light_type_combo_->addItem(label, tv);
    connect(light_type_combo_, &QComboBox::activated, this,
            &PlacementTab::on_light_edited);
    light_form->addRow(tr("Type"), light_type_combo_);

    // Colour swatches open a QColorDialog; the button background
    // previews the current colour. Diffuse drives hue + brightness.
    light_diffuse_btn_ = new QPushButton(QString());
    light_diffuse_btn_->setToolTip(
        tr("Diffuse colour. Brighter colour = brighter light."));
    connect(light_diffuse_btn_, &QPushButton::clicked, this,
            [this] { pick_light_color(QStringLiteral("diffuse")); });
    light_form->addRow(tr("Color"), light_diffuse_btn_);

    light_specular_btn_ = new QPushButton(QString());
    light_specular_btn_->setToolTip(
        tr("Specular highlight colour (WW / T2T lights only)."));
    connect(light_specular_btn_, &QPushButton::clicked, this,
            [this] { pick_light_color(QStringLiteral("specular")); });
    light_specular_row_label_ = new QLabel(tr("Specular"));
    light_form->addRow(light_specular_row_label_, light_specular_btn_);

    light_near_ = spin(0.0, 1.0e6, 0.0, 0.5, 3);
    light_far_ = spin(0.0, 1.0e6, 0.0, 0.5, 3);
    connect(light_near_, &QDoubleSpinBox::valueChanged, this,
            &PlacementTab::on_light_edited);
    connect(light_far_, &QDoubleSpinBox::valueChanged, this,
            &PlacementTab::on_light_edited);
    light_form->addRow(tr("Range near / far"), row({light_near_, light_far_}));

    // Spot cone angles shown in degrees (stored as radians on disk).
    light_inner_ = spin(0.0, 180.0, 0.0, 1.0, 2);
    light_outer_ = spin(0.0, 180.0, 0.0, 1.0, 2);
    connect(light_inner_, &QDoubleSpinBox::valueChanged, this,
            &PlacementTab::on_light_edited);
    connect(light_outer_, &QDoubleSpinBox::valueChanged, this,
            &PlacementTab::on_light_edited);
    light_angle_label_ = new QLabel(tr("Cone in / out °"));
    light_form->addRow(light_angle_label_, row({light_inner_, light_outer_}));

    light_intensity_ = spin(0.0, 100.0, 1.0, 0.1, 3);
    connect(light_intensity_, &QDoubleSpinBox::valueChanged, this,
            &PlacementTab::on_light_edited);
    light_intensity_label_ = new QLabel(tr("Intensity"));
    light_form->addRow(light_intensity_label_, light_intensity_);

    light_group_->setVisible(false);
    layout->addWidget(light_group_);

    // ── Identity-flag toggles ──────────────────────────────────
    auto* flags_group = new QGroupBox(tr("Flags"));
    auto* flags_layout = new QVBoxLayout(flags_group);
    flags_layout->setSpacing(2);
    // Only expose runtime-side flags that are safe to toggle without
    // corrupting parsing — ColMap (collision presence) and ODE (physics
    // body). Other identity bits change the payload's layout and can't
    // be flipped post-hoc.
    const std::tuple<QString, quint32, QString> flag_rows[] = {
        {tr("Collision (ColMap)"), 0x00000100u,
         tr("Whether this object participates in character/world collision. "
            "Clearing this lets the player walk through the mesh.")},
        {tr("Physics body (ODE)"), 0x10000000u,
         tr("Whether this object is a rigid body in the ODE physics world. "
            "Clearing this disables physics-driven behaviour.")},
    };
    for (const auto& [label, bit, tip] : flag_rows) {
        auto* cb = new QCheckBox(label);
        cb->setToolTip(tip);
        connect(cb, &QCheckBox::toggled, this, &PlacementTab::on_flag_toggled);
        flags_layout->addWidget(cb);
        flag_checks_.push_back({cb, bit});
    }
    layout->addWidget(flags_group);

    // ── Collision-follow ───────────────────────────────────────
    // When the selected object's collision is stored separately (a
    // dedicated *collision.gao, or baked into a shared room COB), the
    // editor lists it here so the user can confirm/override which
    // collision rides along on a move. Dedicated links default on; shared
    // room-carve candidates are intentionally opt-in.
    colfollow_group_ = new QGroupBox(tr("Move collision with object"));
    colfollow_group_->setToolTip(
        tr("Collision this object relies on that is NOT stored in its own "
           "GAO. Checked entries move/carve to follow the object when you "
           "Commit. Enable the viewport's Collision toggle to preview "
           "them."));
    colfollow_vbox_ = new QVBoxLayout(colfollow_group_);
    colfollow_vbox_->setSpacing(2);
    colfollow_info_ = new QLabel(tr("(no selection)"));
    colfollow_info_->setWordWrap(true);
    colfollow_vbox_->addWidget(colfollow_info_);
    layout->addWidget(colfollow_group_);

    // ── Add collision (for objects that have none) ──────────────
    // Gives a decorative GAO a collision box at its current pose by
    // extending the host room COB — no move needed. Queues an
    // AddObjectCollision op; previewed as a green box.
    addcol_group_ = new QGroupBox(tr("Add collision box"));
    addcol_group_->setToolTip(
        tr("Add a collision box around this object (into the room's "
           "collision mesh) so the player can stand on / be blocked by it. "
           "Use for decorative objects the player currently falls "
           "through."));
    auto* addcol_form = new QFormLayout(addcol_group_);
    addcol_form->setLabelAlignment(Qt::AlignRight);
    addcol_profile_ = new QComboBox;
    // Mesh = shape-accurate (keeps doorways/openings); box = solid AABB
    // (seals openings — only for solid blockers). Item data encodes
    // "shape[/profile]".
    addcol_profile_->addItem(tr("Match object mesh (recommended)"),
                             QStringLiteral("mesh"));
    addcol_profile_->addItem(tr("Bounding box — solid"),
                             QStringLiteral("box/simple_box"));
    addcol_profile_->addItem(tr("Bounding box — climbable ledge"),
                             QStringLiteral("box/ledge_openbox"));
    addcol_profile_->setToolTip(
        tr("Match object mesh: collision follows the object's actual "
           "triangles, so hollow shapes (corridors, archways) keep their "
           "openings. Bounding box: one solid box — seals openings, use "
           "only for solid blockers."));
    addcol_form->addRow(tr("Collision"), addcol_profile_);
    addcol_btn_ = new QPushButton(tr("Add collision box"));
    connect(addcol_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_add_collision_clicked);
    addcol_form->addRow(QString(), addcol_btn_);
    layout->addWidget(addcol_group_);

    layout->addStretch();
    set_inspector_enabled(false);
    return page;
}

QWidget* PlacementTab::build_create_tab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* form = new QFormLayout;
    kind_combo_ = new QComboBox;
    kind_combo_->addItem(tr("Clone Existing"), QStringLiteral("clone"));
    kind_combo_->addItem(tr("Imported Model"), QStringLiteral("model"));
    kind_combo_->addItem(tr("Cube"), QStringLiteral("cube"));
    kind_combo_->addItem(tr("Sphere"), QStringLiteral("sphere"));
    kind_combo_->addItem(tr("Cylinder"), QStringLiteral("cylinder"));
    kind_combo_->addItem(tr("Replace Mesh"), QStringLiteral("replace"));
    kind_combo_->addItem(tr("Replace Entire Bin"),
                         QStringLiteral("replace_bin"));
    connect(kind_combo_, &QComboBox::currentIndexChanged, this,
            &PlacementTab::on_kind_changed);
    form->addRow(tr("Type"), kind_combo_);

    source_combo_ = new QComboBox;
    connect(source_combo_, &QComboBox::currentIndexChanged, this,
            &PlacementTab::on_draft_control_changed);
    // `activated` only fires on USER selection (not programmatic
    // setCurrentIndex) — that's what we use to detect a manual override
    // and stop auto-syncing from the viewport selection.
    connect(source_combo_, &QComboBox::activated, this,
            &PlacementTab::on_source_combo_user_picked);
    // Inline filter above the combo so users can find a GAO by name in
    // a 700+ object hall without scrolling.
    source_filter_ = new QLineEdit;
    source_filter_->setPlaceholderText(
        tr("Filter source GAOs by name or key…"));
    connect(source_filter_, &QLineEdit::textChanged, this,
            &PlacementTab::on_source_filter_changed);
    form->addRow(tr("Source filter"), source_filter_);
    form->addRow(tr("Source"), source_combo_);

    material_combo_ = new QComboBox;
    connect(material_combo_, &QComboBox::currentIndexChanged, this,
            &PlacementTab::on_draft_control_changed);
    form->addRow(tr("Material"), material_combo_);

    auto* model_row = new QHBoxLayout;
    model_path_edit_ = new QLineEdit;
    model_path_edit_->setPlaceholderText(tr("DAE, FBX, OBJ, glTF, STL, PLY…"));
    connect(model_path_edit_, &QLineEdit::textChanged, this,
            &PlacementTab::on_draft_control_changed);
    model_row->addWidget(model_path_edit_, 1);
    model_browse_button_ = new QPushButton(tr("Browse"));
    connect(model_browse_button_, &QPushButton::clicked, this,
            &PlacementTab::on_browse_model);
    model_row->addWidget(model_browse_button_);
    form->addRow(tr("Model"), model_row);

    // Vertex-color controls. For imported models: carry the file's
    // per-vertex COLOR_0 into the GEO. For primitives: an optional flat
    // tint. See [[jade-geo-vertex-colors]].
    vcolor_check_ =
        new QCheckBox(tr("Use the model's vertex colors (GLB COLOR_0 / PLY)"));
    vcolor_check_->setToolTip(
        tr("Write the imported model's per-vertex colors into the new GEO's\n"
           "vertex-color array (D3DCOLOR ARGB) so the engine renders them.\n"
           "Only meaningful for models that actually carry vertex colors."));
    connect(vcolor_check_, &QCheckBox::toggled, this,
            &PlacementTab::on_draft_control_changed);
    form->addRow(tr("Vertex colors"), vcolor_check_);

    prim_color_ = std::nullopt;  // (r,g,b,a) flat tint for primitives
    auto* prim_color_row = new QHBoxLayout;
    prim_color_btn_ = new QPushButton(tr("No tint"));
    prim_color_btn_->setToolTip(
        tr("Optional flat vertex tint applied to every vertex of a "
           "generated\ncube / sphere / cylinder (modulates the chosen "
           "material)."));
    connect(prim_color_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_pick_prim_color);
    prim_color_row->addWidget(prim_color_btn_, 1);
    prim_color_clear_ = new QPushButton(tr("Clear"));
    connect(prim_color_clear_, &QPushButton::clicked, this,
            &PlacementTab::on_clear_prim_color);
    prim_color_row->addWidget(prim_color_clear_);
    form->addRow(tr("Primitive tint"), prim_color_row);

    replace_from_combo_ = new QComboBox;
    replace_from_combo_->addItem(tr("From Model File"),
                                 QStringLiteral("model_file"));
    replace_from_combo_->addItem(tr("From Existing GAO"),
                                 QStringLiteral("existing_gao"));
    replace_from_combo_->addItem(tr("From Imported .jgao File"),
                                 QStringLiteral("imported_file"));
    connect(replace_from_combo_, &QComboBox::currentIndexChanged, this,
            &PlacementTab::on_replace_from_changed);
    form->addRow(tr("Replace From"), replace_from_combo_);

    auto* source_entry_row = new QHBoxLayout;
    source_entry_combo_ = new QComboBox;
    source_entry_combo_->setMinimumWidth(200);
    source_entry_row->addWidget(source_entry_combo_, 1);
    load_source_btn_ = new QPushButton(tr("Load"));
    load_source_btn_->setToolTip(
        tr("Load visual objects from the selected source entry"));
    connect(load_source_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_load_source_entry);
    source_entry_row->addWidget(load_source_btn_);
    form->addRow(tr("Source Entry"), source_entry_row);

    source_gao_combo_ = new QComboBox;
    connect(source_gao_combo_, &QComboBox::currentIndexChanged, this,
            &PlacementTab::on_draft_control_changed);
    form->addRow(tr("Source GAO"), source_gao_combo_);

    replace_from_combo_->setVisible(false);
    source_entry_combo_->setVisible(false);
    load_source_btn_->setVisible(false);
    source_gao_combo_->setVisible(false);

    name_edit_ = new QLineEdit;
    name_edit_->setPlaceholderText(tr("Object name (optional)"));
    form->addRow(tr("Name"), name_edit_);

    // Position is no longer configured here — Place spawns at the
    // camera's focus point (where the user is looking) and the Object
    // tab is where the user nudges from there. Spinboxes kept as hidden
    // members so the rest of the file's draft / pick / preview paths
    // don't need to change.
    pos_x_ = spin(-100000.0, 100000.0, 0.0, 0.25);
    pos_y_ = spin(-100000.0, 100000.0, 0.0, 0.25);
    pos_z_ = spin(-100000.0, 100000.0, 0.0, 0.25);
    pick_check_ = new QCheckBox(tr("(unused)"));
    pick_check_->setVisible(false);

    size_x_ = spin(0.001, 10000.0, 1.0, 0.25);
    size_y_ = spin(0.001, 10000.0, 1.0, 0.25);
    size_z_ = spin(0.001, 10000.0, 1.0, 0.25);
    form->addRow(tr("Size XYZ"), row({size_x_, size_y_, size_z_}));

    collision_check_ = new QCheckBox(tr("Add ColMap collision box"));
    collision_check_->setChecked(true);
    collision_check_->setToolTip(
        tr("Creates a Jade ColMap/COB box so static primitives participate "
           "in character collision."));
    connect(collision_check_, &QCheckBox::toggled, this,
            &PlacementTab::on_collision_check_changed);
    form->addRow(tr("Collision"), collision_check_);

    // Collision profile — Solid is the regular blocker, Climbable Ledge
    // emits the open-front-box COB recipe (mat 67 / design 0 / 4
    // pre-baked climb-edge records on the +Z perimeter) the engine
    // recognises as a grabbable surface. Calibrated against retail
    // 0x4D000E9A in 1401_Cour_wow. Works for ANY kind — cube/sphere/
    // cylinder/imported model/clone — the COB is the box approximation
    // of the visual's bounds either way.
    collision_profile_combo_ = new QComboBox;
    collision_profile_combo_->addItem(tr("Solid (blocker)"),
                                      QStringLiteral("simple_box"));
    collision_profile_combo_->addItem(tr("Climbable Ledge"),
                                      QStringLiteral("ledge_openbox"));
    collision_profile_combo_->setToolTip(
        tr("Solid: standard collision blocker.  "
           "Climbable Ledge: pre-baked grab-edges on the +Z face "
           "perimeter — Prince grabs from the +Z side. Rotate the "
           "object so its local +Z faces the approach direction."));
    connect(collision_profile_combo_, &QComboBox::currentIndexChanged, this,
            &PlacementTab::on_draft_control_changed);
    form->addRow(tr("Collision profile"), collision_profile_combo_);

    // Host COB picker — clone-with-collision appends new triangles into
    // a shipped room COB. Auto picks the largest containing COB; users
    // override here when several overlap.
    host_cob_combo_ = new QComboBox;
    host_cob_combo_->addItem(tr("Auto (largest containing)"), QVariant());
    host_cob_combo_->setToolTip(
        tr("Which existing room COB to extend with the clone's collision "
           "triangles. Auto picks the largest triangle COB whose host "
           "GAO's local OBBox contains the clone position. Override when "
           "the wrong host gets the new triangles."));
    connect(host_cob_combo_, &QComboBox::currentIndexChanged, this,
            &PlacementTab::on_draft_control_changed);
    form->addRow(tr("Host COB"), host_cob_combo_);

    layout->addLayout(form);

    auto* button_row = new QHBoxLayout;
    auto* place = new QPushButton(tr("Place Object"));
    place->setToolTip(
        tr("Spawn the configured object at the camera's focus point, "
           "auto-select it, and switch to the Object tab for editing. "
           "The object joins the map-wide change set and is committed "
           "alongside transform / flag edits via Commit Changes."));
    connect(place, &QPushButton::clicked, this,
            &PlacementTab::on_add_pending_object);
    button_row->addWidget(place);
    layout->addLayout(button_row);

    auto* gao_io_row = new QHBoxLayout;
    export_gao_btn_ = new QPushButton(tr("Export GAO"));
    export_gao_btn_->setToolTip(
        tr("Export the selected source GAO to a .jgao file (geometry + "
           "material + setup)"));
    connect(export_gao_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_export_gao);
    gao_io_row->addWidget(export_gao_btn_);
    import_gao_btn_ = new QPushButton(tr("Import GAO"));
    import_gao_btn_->setToolTip(
        tr("Import a .jgao file as the replacement source"));
    connect(import_gao_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_import_gao);
    gao_io_row->addWidget(import_gao_btn_);
    layout->addLayout(gao_io_row);

    auto* glb_io_row = new QHBoxLayout;
    jgao_to_glb_btn_ = new QPushButton(tr("JGAO → GLB"));
    jgao_to_glb_btn_->setToolTip(
        tr("Convert a .jgao file to editable .glb (for Blender)"));
    connect(jgao_to_glb_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_jgao_to_glb);
    glb_io_row->addWidget(jgao_to_glb_btn_);
    glb_to_jgao_btn_ = new QPushButton(tr("GLB → JGAO"));
    glb_to_jgao_btn_->setToolTip(
        tr("Convert edited .glb back to .jgao (uses original .jgao as "
           "template)"));
    connect(glb_to_jgao_btn_, &QPushButton::clicked, this,
            &PlacementTab::on_glb_to_jgao);
    glb_io_row->addWidget(glb_to_jgao_btn_);
    layout->addLayout(glb_io_row);

    layout->addStretch();
    return page;
}

// ── Tiny widget builders ─────────────────────────────────────────────

QDoubleSpinBox* PlacementTab::spin(double minimum, double maximum,
                                   double value, double step, int decimals) {
    auto* s = new QDoubleSpinBox;
    s->setRange(minimum, maximum);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setValue(value);
    return s;
}

QWidget* PlacementTab::row(const std::vector<QWidget*>& widgets) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    for (QWidget* x : widgets) h->addWidget(x);
    return w;
}

// ── Public hooks (called by main_window) ─────────────────────────────

void PlacementTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                               const QString& bf_path) {
    bf_ = std::move(bf);
    bf_path_ = bf_path;
    zone_info_.reset();
    replace_source_objects_.clear();
    imported_jgao_.reset();
    imported_jgao_path_.clear();
    // New BF → drop any cached cross-bin sources from the previous one.
    xbin_source_cache_.clear();
    draft_preview_active_ = false;
    selected_gao_key_.reset();
    set_pick_mode(false);
    sync_transform_gizmo(false);
    populate_entries();
    log_->clear();
    log_->append(tr("Loaded BF: %1").arg(bf_path));
    update_object_tab_for_selection(std::nullopt);
}

void PlacementTab::set_project(ProjectDoc* project) {
    // Receive the active ModProject from the main window.
    project_ = project;
    if (project == nullptr)
        log_->append(tr(
            "[project] no active project — Stage/Add buttons will warn"));
}

void PlacementTab::receive_asset(quint32 parent_index, quint32 key) {
    // The Python PlacementTab has no receive_asset — main_window's
    // send-to-tab helper resolves getattr(tab, "receive_asset", None)
    // to None and skips it. Kept as a no-op for the same behaviour.
    Q_UNUSED(parent_index);
    Q_UNUSED(key);
}

// ── Zone tree ────────────────────────────────────────────────────────

void PlacementTab::populate_entries() {
    entry_tree_->clear();
    if (!bf_) return;
    // Only show `_wow_` bins — those carry the actual placeable
    // geometry. `_wol_` bins are the small dependency lists and have
    // nothing for the placement editor to act on.
    std::vector<const jade::BFFile*> files;
    for (const auto& [idx, fi] : bf_->files) {
        (void)idx;
        if (fi.name.empty() || fi.key == INVALID_KEY || fi.length == 0)
            continue;
        const QString lname = qs(fi.name).toLower();
        if (!lname.contains(QStringLiteral("_wow_"))) continue;
        files.push_back(&fi);
    }
    std::sort(files.begin(), files.end(),
              [](const jade::BFFile* a, const jade::BFFile* b) {
                  const QString an = qs(a->name).toLower();
                  const QString bn = qs(b->name).toLower();
                  if (an != bn) return an < bn;
                  return a->index < b->index;
              });
    for (const jade::BFFile* fi : files) {
        auto* item = new QTreeWidgetItem(entry_tree_);
        item->setText(0, qs(fi->name));
        item->setText(1, QString::number(fi->index));
        item->setText(2, hex_key(fi->key));
        item->setData(0, Qt::UserRole, fi->index);
    }
    entry_tree_->resizeColumnToContents(1);
}

void PlacementTab::apply_filter() {
    const QString needle = filter_->text().trimmed().toLower();
    for (int i = 0; i < entry_tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = entry_tree_->topLevelItem(i);
        QStringList hay_parts;
        for (int c = 0; c < 3; ++c) hay_parts << item->text(c).toLower();
        const QString hay = hay_parts.join(QLatin1Char(' '));
        item->setHidden(!needle.isEmpty() && !hay.contains(needle));
    }
}

void PlacementTab::on_entry_clicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    const QVariant entry_index = item->data(0, Qt::UserRole);
    if (!entry_index.isValid()) return;
    load_zone(entry_index.toUInt());
}

void PlacementTab::load_zone(quint32 entry_index) {
    if (!bf_) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    ZoneInfo zone = load_zone_info(entry_index);
    QApplication::restoreOverrideCursor();
    if (!zone.ok) {
        QMessageBox::warning(this, tr("Zone Load Error"), zone.error);
        return;
    }

    zone_info_ = std::move(zone);
    draft_preview_active_ = false;
    populate_create_controls();
    set_create_controls_enabled(true);
    on_kind_changed();
    log_->append(tr("Loaded entry %1: %2 objects, %L3 decompressed bytes")
                     .arg(entry_index)
                     .arg(zone_info_->n_visual)
                     .arg(qulonglong(zone_info_->dec_size)));
    refresh_preview();
    sync_transform_gizmo(false);
    // Map-wide edit-tracking is scoped to one zone — wipe the dirty set
    // on every load and replay any project ops onto the viewport so
    // previously-committed edits show up.
    build_committed_cache();
    update_object_tab_for_selection(std::nullopt);
}

// ── Zone loading (object_placer.load_zone_info, local port) ──────────

namespace {

// object_placer._collect_visual_objects + _collect_marker_objects over
// one decompressed entry. Visual objects come first, then markers;
// n_visual marks the split (the Python keeps two lists).
void collect_zone_objects(const std::vector<jade::SubEntry>& subs,
                          std::vector<ZoneObject>& out, size_t& n_visual) {
    const SubPtrMap by_key = build_sub_ptr_map(subs);
    std::unordered_set<quint32> all_gao_keys;
    for (const jade::SubEntry& s : subs)
        if (s.ext == ".gao") all_gao_keys.insert(s.key);

    std::vector<ZoneObject> markers;
    for (size_t si = 0; si < subs.size(); ++si) {
        const jade::SubEntry& sub = subs[si];
        if (sub.ext != ".gao") continue;
        const jade::GaoInfo info =
            jade::parse_gao_full(sub.data.data(), sub.data.size());
        if (!info.ok || !info.gmat_present || info.gmat_raw.size() < 68)
            continue;

        ZoneObject obj;
        obj.key = sub.key;
        obj.sub_index = int(si);
        obj.name = !info.name.empty() ? qs(info.name)
                                      : QStringLiteral("gao_") + hex_key(sub.key);
        obj.identity = info.identity;
        obj.editor_flags = info.editor_flags;
        obj.flag_names = gao_flag_names(info.identity);
        obj.has_colmap = (info.identity & FLAG_COLMAP) != 0;
        obj.has_ode = (info.identity & FLAG_ODE) != 0;
        obj.has_obbox = (info.identity & 0x00080000u) != 0;
        obj.has_matrix = true;
        for (int i = 0; i < 16; ++i)
            obj.matrix[size_t(i)] =
                rd_f32(info.gmat_raw.data(), size_t(i) * 4);
        obj.matrix_type = int(info.gmat_type);
        obj.position = {obj.matrix[12], obj.matrix[13], obj.matrix[14]};
        const quint32 father =
            info.hier_read ? info.father_key : INVALID_KEY;
        obj.father_key = (long long)father;
        obj.is_root = father == INVALID_KEY || !all_gao_keys.count(father);

        // Visual object: visual block + a parseable gro_type-1 GEO.
        int gro_type = -1;
        std::shared_ptr<jade::GeoInfo> geo;
        if (info.vis_read) {
            auto git = by_key.find(info.gro_key);
            if (git != by_key.end() && !git->second->gro_null) {
                gro_type = int(git->second->gro_type);
                if (gro_type == 1) {
                    auto parsed = std::make_shared<jade::GeoInfo>(
                        jade::parse_geometry(git->second->data.data(),
                                             git->second->data.size()));
                    if (parsed->ok && !parsed->ps2 && parsed->nb_tris > 0)
                        geo = std::move(parsed);
                }
            }
        }
        if (geo) {
            obj.geo = std::move(geo);
            obj.geo_key = (long long)info.gro_key;
            obj.material_key = (long long)info.grm_key;
            out.push_back(std::move(obj));
            continue;
        }

        // Non-visual marker (camera / light / sound / trigger / …).
        // Skeleton bones are skipped (joints, not placeable objects).
        const bool father_in_bin = all_gao_keys.count(father) != 0;
        if (jade::is_bone(obj.name.toStdString(), info.identity,
                          father_in_bin))
            continue;
        obj.is_marker = true;
        obj.category = qs(jade::classify_object(
            obj.name.toStdString(), info.identity, gro_type, father_in_bin));
        // Light markers carry their GRO_Light resource elsewhere in the
        // bin: the light key is the last u32 of the GAO payload.
        // Presence of a real light resource is authoritative — promote
        // the category to "light" even if the name heuristic missed.
        const jade::LightKeyOpt cand = jade::light_key_from_gao_payload(
            sub.data.data(), sub.data.size());
        if (cand.have) {
            auto lit = by_key.find(cand.key);
            if (lit != by_key.end() && !lit->second->gro_null
                && lit->second->gro_type == jade::GRO_LIGHT) {
                const jade::LightInfo parsed = jade::parse_light(
                    lit->second->data.data(), lit->second->data.size());
                if (parsed.ok) {
                    obj.light_key = (long long)cand.key;
                    obj.light = parsed;
                    obj.category = QStringLiteral("light");
                }
            }
        }
        markers.push_back(std::move(obj));
    }
    n_visual = out.size();
    for (ZoneObject& m : markers) out.push_back(std::move(m));
}

}  // namespace

ZoneInfo PlacementTab::load_zone_info(quint32 entry_index) {
    ZoneInfo zone;
    auto fit = bf_->files.find(entry_index);
    if (fit == bf_->files.end()) {
        zone.error = tr("Entry %1 not found").arg(entry_index);
        return zone;
    }
    const std::vector<uint8_t> raw = bf_->read_data(entry_index);
    const jade::LzoResult dec = jade::decompress_lzo(raw.data(), raw.size());
    if (!dec.ok || dec.data.empty()) {
        zone.error = tr("Entry %1 did not decompress").arg(entry_index);
        return zone;
    }

    zone.entry_index = entry_index;
    zone.entry_key = fit->second.key;
    zone.entry_name = qs(fit->second.name);
    zone.dec_size = dec.data.size();
    zone.subs = jade::walk_sub_entries(dec.data);
    for (size_t i = 0; i < zone.subs.size(); ++i)
        zone.sub_by_key[zone.subs[i].key] = int(i);

    collect_zone_objects(zone.subs, zone.objects, zone.n_visual);
    for (size_t i = 0; i < zone.objects.size(); ++i)
        zone.objects_by_key[zone.objects[i].key] = int(i);

    const SubPtrMap by_key = build_sub_ptr_map(zone.subs);

    // Materials (_collect_materials): gro_type 3/4/5, 5 only when a
    // static shipped GAO references it, sorted by (-visual_refs,
    // gro_type != 4, stream order).
    {
        std::map<quint32, int> visual_refs, static_refs;
        for (const jade::SubEntry& s : zone.subs) {
            if (s.ext != ".gao") continue;
            const jade::GaoInfo info =
                jade::parse_gao_full(s.data.data(), s.data.size());
            if (!info.ok || !info.vis_read) continue;
            const quint32 mk = info.grm_key;
            if (mk == INVALID_KEY) continue;
            visual_refs[mk] += 1;
            // Static referrer: name not Fake_/GFX_/B_/M_, key not in the
            // toolkit's 0x7Axxxxxx modded range.
            if ((s.key & 0xFF000000u) == 0x7A000000u) continue;
            const QString name = qs(info.name);
            if (name.isEmpty()
                || name.startsWith(QStringLiteral("Fake_"))
                || name.startsWith(QStringLiteral("GFX_"))
                || name.startsWith(QStringLiteral("B_"))
                || name.startsWith(QStringLiteral("M_")))
                continue;
            static_refs[mk] += 1;
        }
        struct MatRow {
            ZoneInfo::MatEntry entry;
            int visual_refs = 0;
            int not_multi = 0;
            int order = 0;
        };
        std::vector<MatRow> rows;
        int order = 0;
        for (const jade::SubEntry& s : zone.subs) {
            const int gt = s.gro_null ? -1 : int(s.gro_type);
            ++order;
            if (gt != 3 && gt != 4 && gt != 5) continue;
            // gro_type=5 covers both real static single-materials AND
            // tiny GFX/particle stubs that crash a static visual block
            // in-game — keep a 5 only when a static zone GAO already
            // references it.
            if (gt == 5 && static_refs[s.key] == 0) continue;
            const jade::MatInfo mat =
                jade::parse_material(s.data.data(), s.data.size(), gt);
            MatRow row;
            row.entry.key = s.key;
            row.entry.label = material_type_label(mat);
            row.visual_refs = visual_refs.count(s.key)
                                  ? visual_refs[s.key] : 0;
            row.not_multi = gt != 4 ? 1 : 0;
            row.order = order;
            rows.push_back(std::move(row));
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](const MatRow& a, const MatRow& b) {
                             if (a.visual_refs != b.visual_refs)
                                 return a.visual_refs > b.visual_refs;
                             if (a.not_multi != b.not_multi)
                                 return a.not_multi < b.not_multi;
                             return a.order < b.order;
                         });
        for (MatRow& r : rows) zone.materials.push_back(std::move(r.entry));
    }

    // Prebuilt viewport scene: zone meshes (with baked RLI), markers,
    // collision wireframes, per-GAO per-element textures.
    zone.tex_by_key = decode_zone_textures(zone.subs);
    for (size_t i = 0; i < zone.n_visual; ++i) {
        const ZoneObject& obj = zone.objects[i];
        // Baked per-vertex lighting (RLI): the GAO's PRIMARY
        // dul_VertexColors table is base-vertex order, 1:1 with the
        // GEO's vertices. An ALL-ZERO table marks an UNLIT surface —
        // treat as "no RLI" so the viewport falls back to texture-only
        // (the engine's unlit look) instead of solid black.
        std::vector<float> rli;
        if (obj.geo && obj.sub_index >= 0) {
            const jade::SubEntry& sub = zone.subs[size_t(obj.sub_index)];
            const jade::PrimaryColors prim = jade::read_primary_colors(
                sub.data.data(), sub.data.size(), obj.geo->nb_points);
            if (prim.ok) {
                bool any = false;
                for (const jade::RgbaColor& c : prim.colors)
                    if (c.r || c.g || c.b) { any = true; break; }
                if (any) {
                    rli.reserve(prim.colors.size() * 4);
                    for (const jade::RgbaColor& c : prim.colors) {
                        rli.push_back(float(c.r) / 255.0f);
                        rli.push_back(float(c.g) / 255.0f);
                        rli.push_back(float(c.b) / 255.0f);
                        rli.push_back(1.0f);
                    }
                }
            }
        }
        const std::array<float, 4> color = object_color(
            int(i), obj.key, obj.has_colmap || obj.has_ode);
        MeshDict mesh = mesh_from_geoinfo(*obj.geo,
                                          rli.empty() ? nullptr : &rli,
                                          obj.matrix, obj.name, color, true);
        mesh.gao_key = (long long)obj.key;
        zone.scene_meshes.push_back(std::move(mesh));
    }
    for (size_t i = zone.n_visual; i < zone.objects.size(); ++i) {
        const ZoneObject& m = zone.objects[i];
        // Marker: a billboarded category icon at the object's world
        // position, pickable + movable exactly like a mesh.
        MeshDict md;
        md.name = m.name;
        md.is_marker = true;
        md.marker_category = m.category;
        md.gao_key = (long long)m.key;
        md.position = jade_to_display(m.position[0], m.position[1],
                                      m.position[2]);
        const std::array<float, 3> col = category_color3(m.category);
        md.base_color = {col[0], col[1], col[2], 1.0f};
        md.pickable = true;
        zone.marker_meshes.push_back(std::move(md));
    }
    // Collision wireframes (_build_collision_scene_meshes).
    {
        int index = 0;
        for (const jade::SubEntry& sub : zone.subs) {
            const int idx_now = index++;
            if (sub.ext != ".gao") continue;
            const jade::GaoInfo info =
                jade::parse_gao_full(sub.data.data(), sub.data.size());
            if (!info.ok || !(info.identity & FLAG_COLMAP)) continue;
            if (!info.gmat_present || info.gmat_raw.size() < 68) continue;
            Mat16 matrix;
            for (int i = 0; i < 16; ++i)
                matrix[size_t(i)] =
                    rd_f32(info.gmat_raw.data(), size_t(i) * 4);
            const QString name = !info.name.empty()
                                     ? qs(info.name)
                                     : QStringLiteral("gao_") + hex_key(sub.key);
            std::vector<MeshDict> meshes = collision_meshes_for_gao_payload(
                sub.data, by_key, matrix,
                QStringLiteral("%1 collision").arg(name),
                collision_color(idx_now, false), (long long)sub.key);
            for (MeshDict& m : meshes)
                zone.collision_meshes.push_back(std::move(m));
        }
    }
    {
        std::vector<ZoneObject> visuals(zone.objects.begin(),
                                        zone.objects.begin()
                                            + long(zone.n_visual));
        zone.textures_by_gao_key =
            resolve_textures_for_objects(visuals, by_key, zone.tex_by_key);
    }

    zone.ok = true;
    return zone;
}

// ── Create-tab control population ────────────────────────────────────

void PlacementTab::populate_create_controls() {
    const ZoneInfo& zone = *zone_info_;
    // Cache the full source-GAO list (display label + key) so the
    // filter LineEdit can rebuild the combo from it on every keystroke
    // without re-touching zone.objects.
    all_source_objects_.clear();
    auto add_visual = [&](const ZoneObject& obj) {
        const QString suffix =
            obj.is_root ? QString() : QStringLiteral(" (child)");
        QString collision;
        if (obj.has_colmap)
            collision = QStringLiteral(" ColMap");
        else if (obj.has_ode)
            collision = QStringLiteral(" ODE");
        const QString label = QStringLiteral("%1 %2%3%4").arg(
            hex_key(obj.key), obj.name, suffix, collision);
        all_source_objects_.push_back({label, obj.key});
    };
    for (size_t i = 0; i < zone.n_visual; ++i)
        if (zone.objects[i].is_root) add_visual(zone.objects[i]);
    for (size_t i = 0; i < zone.n_visual; ++i)
        if (!zone.objects[i].is_root) add_visual(zone.objects[i]);
    // Non-visual objects (cameras/lights/triggers/…) are clonable too —
    // same code path; they just have no geometry. Listed after the
    // visual objects, tagged with their category so they're easy to spot.
    for (size_t i = zone.n_visual; i < zone.objects.size(); ++i) {
        const ZoneObject& m = zone.objects[i];
        const QString label = QStringLiteral("%1 %2  · %3").arg(
            hex_key(m.key), m.name, category_label(m.category));
        all_source_objects_.push_back({label, m.key});
    }
    // Reset filter + override on every zone load — new bin = new set of
    // GAOs, old override no longer makes sense.
    clone_source_user_overridden_ = false;
    source_filter_->blockSignals(true);
    source_filter_->clear();
    source_filter_->blockSignals(false);
    rebuild_source_combo();

    material_combo_->clear();
    for (const ZoneInfo::MatEntry& mat : zone.materials)
        material_combo_->addItem(
            QStringLiteral("%1 %2").arg(hex_key(mat.key), mat.label),
            mat.key);
    if (material_combo_->count() == 0)
        material_combo_->addItem(tr("No material found"), INVALID_KEY);
    // The Inspector's material picker reuses the same material list.
    insp_material_combo_->blockSignals(true);
    insp_material_combo_->clear();
    for (const ZoneInfo::MatEntry& mat : zone.materials)
        insp_material_combo_->addItem(
            QStringLiteral("%1 %2").arg(hex_key(mat.key), mat.label),
            mat.key);
    insp_material_combo_->blockSignals(false);
    populate_source_entry_combo();

    // Host COB dropdown (object_placer.enumerate_triangle_cob_hosts):
    // every triangle-shape COB with the name of the GAO that owns it
    // (via ColMap chain), sorted by size descending.
    host_cob_combo_->clear();
    host_cob_combo_->addItem(tr("Auto (largest containing)"), QVariant());
    {
        const SubPtrMap by_key = build_sub_ptr_map(zone.subs);
        struct CobRow { quint32 key; QString host_name; quint32 size; };
        std::vector<CobRow> cobs;
        for (const jade::SubEntry& s : zone.subs) {
            if (!jade::looks_like_cob_sub(&s)) continue;
            if (s.data.empty() || s.data[0] != jade::COL_ZONE_TRIANGLES)
                continue;
            // Find the GAO whose ColMap chain reaches this COB.
            QString host_name = tr("(no host)");
            for (const jade::SubEntry& g : zone.subs) {
                if (g.ext != ".gao") continue;
                const jade::GaoInfo info =
                    jade::parse_gao_full(g.data.data(), g.data.size());
                if (!info.ok || !(info.identity & FLAG_COLMAP)) continue;
                bool found = false;
                for (const auto& [off, cm_key] :
                     jade::gao_colmap_key_offsets(g.data, by_key)) {
                    (void)off;
                    auto cit = by_key.find(cm_key);
                    const jade::SubEntry* cm_sub =
                        cit == by_key.end() ? nullptr : cit->second;
                    for (quint32 ck : jade::colmap_cob_keys(cm_sub, by_key))
                        if (ck == s.key) { found = true; break; }
                    if (found) break;
                }
                if (found) {
                    if (!info.name.empty()) host_name = qs(info.name);
                    break;
                }
            }
            cobs.push_back({s.key, host_name, s.size});
        }
        std::stable_sort(cobs.begin(), cobs.end(),
                         [](const CobRow& a, const CobRow& b) {
                             return a.size > b.size;
                         });
        for (const CobRow& cob : cobs)
            host_cob_combo_->addItem(
                QStringLiteral("%1 %2 (%L3 B)")
                    .arg(hex_key(cob.key), cob.host_name)
                    .arg(cob.size),
                cob.key);
    }
}

// Repopulate the source combo from the cached object list, honouring
// the current filter substring. Preserves the currently-selected key
// when possible so a stray keystroke in the filter doesn't drop the
// user's pick.
void PlacementTab::rebuild_source_combo() {
    const QString needle = source_filter_->text().trimmed().toLower();
    const QVariant prev_key = source_combo_->currentData();
    source_combo_->blockSignals(true);
    source_combo_->clear();
    for (const auto& [label, key] : all_source_objects_) {
        if (!needle.isEmpty() && !label.toLower().contains(needle)) continue;
        source_combo_->addItem(label, key);
    }
    // Restore previous selection if it survived the filter.
    if (prev_key.isValid()) {
        const int idx = source_combo_->findData(prev_key);
        if (idx >= 0) source_combo_->setCurrentIndex(idx);
    }
    source_combo_->blockSignals(false);
}

void PlacementTab::on_source_filter_changed(const QString& text) {
    Q_UNUSED(text);
    rebuild_source_combo();
}

void PlacementTab::on_source_combo_user_picked(int index) {
    // Manual selection from the dropdown — the user has overridden the
    // auto-default. Don't re-sync from the viewport selection until the
    // next zone load or kind-switch.
    Q_UNUSED(index);
    clone_source_user_overridden_ = true;
}

// When the kind is `clone` and the user hasn't manually overridden the
// source combo, set the combo to the viewport's currently-selected GAO.
// No-op for synthetic pending keys (those aren't real zone GAOs and
// can't be a clone source).
void PlacementTab::sync_clone_source_to_selection() {
    if (kind_combo_->currentData().toString() != QStringLiteral("clone"))
        return;
    if (clone_source_user_overridden_) return;
    if (!selected_gao_key_) return;
    const quint32 gk = *selected_gao_key_;
    // Pending creates live above 0xFE000000 — they aren't in the source
    // combo and can't be cloned.
    if (pending_objects_by_key_.count(gk)) return;
    int idx = source_combo_->findData(gk);
    if (idx < 0) {
        // The selected GAO is filtered out by the search box; clear the
        // filter so it appears, then re-find.
        if (!source_filter_->text().isEmpty()) {
            source_filter_->blockSignals(true);
            source_filter_->clear();
            source_filter_->blockSignals(false);
            rebuild_source_combo();
            idx = source_combo_->findData(gk);
        }
    }
    if (idx >= 0) {
        source_combo_->blockSignals(true);
        source_combo_->setCurrentIndex(idx);
        source_combo_->blockSignals(false);
        on_draft_control_changed();
    }
}

void PlacementTab::set_create_controls_enabled(bool enabled) {
    for (QWidget* widget : std::initializer_list<QWidget*>{
             kind_combo_, source_combo_, material_combo_, name_edit_,
             model_path_edit_, model_browse_button_, replace_from_combo_,
             source_entry_combo_, load_source_btn_, source_gao_combo_,
             pick_check_, pos_x_, pos_y_, pos_z_, size_x_, size_y_, size_z_,
             collision_check_})
        widget->setEnabled(enabled);
}

void PlacementTab::on_kind_changed() {
    const QString kind = kind_combo_->currentData().toString();
    const bool is_clone = kind == QStringLiteral("clone");
    const bool is_model = kind == QStringLiteral("model");
    const bool is_replace = kind == QStringLiteral("replace");
    const bool is_replace_bin = kind == QStringLiteral("replace_bin");
    const bool needs_source = is_clone || is_replace;
    const QString replace_from =
        is_replace ? replace_from_combo_->currentData().toString()
                   : QString();
    const bool replace_from_gao =
        replace_from == QStringLiteral("existing_gao");
    const bool replace_from_import =
        replace_from == QStringLiteral("imported_file");
    const bool replace_from_model =
        is_replace && !replace_from_gao && !replace_from_import;
    const bool needs_model = is_model || replace_from_model;
    const bool needs_size =
        !is_clone && !is_replace_bin && !replace_from_import;
    const bool loaded = zone_info_.has_value();
    source_combo_->setEnabled(needs_source && loaded);
    material_combo_->setEnabled((!needs_source && !is_replace_bin) && loaded);
    model_path_edit_->setEnabled(needs_model && loaded);
    model_browse_button_->setEnabled(needs_model && loaded);
    // Vertex-color controls: the COLOR_0 checkbox is for imported
    // models; the flat tint is for generated primitives.
    const bool is_primitive = kind == QStringLiteral("cube")
                              || kind == QStringLiteral("sphere")
                              || kind == QStringLiteral("cylinder");
    vcolor_check_->setEnabled(needs_model && loaded);
    prim_color_btn_->setEnabled(is_primitive && loaded);
    prim_color_clear_->setEnabled(is_primitive && loaded);
    replace_from_combo_->setVisible(is_replace);
    // Show the cross-bin Entry+GAO pickers for Clone too — Clone
    // Existing supports cloning from any BF entry, not just the current
    // zone. The intra-bin source_combo above stays as the primary
    // picker; the cross-bin combos override when the user picks a
    // non-current source entry.
    source_entry_combo_->setVisible(replace_from_gao || is_replace_bin
                                    || is_clone);
    load_source_btn_->setVisible(replace_from_gao || is_clone);
    source_gao_combo_->setVisible(replace_from_gao || is_clone);
    size_x_->setEnabled(needs_size && loaded);
    size_y_->setEnabled(needs_size && loaded);
    size_z_->setEnabled(needs_size && loaded);
    pos_x_->setEnabled(!is_replace && !is_replace_bin);
    pos_y_->setEnabled(!is_replace && !is_replace_bin);
    pos_z_->setEnabled(!is_replace && !is_replace_bin);
    // Clones can also synthesise a collision box from the source GAO's
    // OBBox bounds when the source itself has no ColMap — covers the
    // "alcove that used to clone with collision" workflow.
    const bool coll_enabled = !is_replace && !is_replace_bin && loaded;
    collision_check_->setEnabled(coll_enabled);
    // Profile combo only matters when collision is actually being
    // added; mirror the collision checkbox.
    collision_profile_combo_->setEnabled(coll_enabled
                                         && collision_check_->isChecked());
    // Host COB picker is only meaningful for clone-with-collision
    // (other kinds don't extend an existing room COB).
    host_cob_combo_->setEnabled(is_clone && coll_enabled
                                && collision_check_->isChecked());
    pick_check_->setEnabled(!is_replace && !is_replace_bin);
    name_edit_->setEnabled(!is_replace_bin);
    export_gao_btn_->setVisible(is_replace || is_clone);
    import_gao_btn_->setVisible(is_replace);
    // Switching INTO clone-kind resets the manual-override flag so the
    // next sync defaults to the current viewport selection; then
    // perform that sync.
    if (is_clone) {
        clone_source_user_overridden_ = false;
        sync_clone_source_to_selection();
    }
    on_draft_control_changed();
}

void PlacementTab::on_pick_prim_color() {
    // Pick a flat vertex tint for a generated primitive.
    const QColor initial = prim_color_
                               ? QColor{(*prim_color_)[0], (*prim_color_)[1],
                                        (*prim_color_)[2]}
                               : QColor{255, 255, 255};
    const QColor col =
        QColorDialog::getColor(initial, this, tr("Primitive vertex tint"));
    if (col.isValid()) {
        prim_color_ = {{col.red(), col.green(), col.blue(), 255}};
        prim_color_btn_->setText(tr("RGB %1,%2,%3")
                                     .arg(col.red())
                                     .arg(col.green())
                                     .arg(col.blue()));
        prim_color_btn_->setStyleSheet(
            QStringLiteral("background-color: %1; color: %2;")
                .arg(col.name(), col.lightness() > 128
                                     ? QStringLiteral("#000")
                                     : QStringLiteral("#fff")));
        on_draft_control_changed();
    }
}

void PlacementTab::on_clear_prim_color() {
    prim_color_.reset();
    prim_color_btn_->setText(tr("No tint"));
    prim_color_btn_->setStyleSheet(QString());
    on_draft_control_changed();
}

void PlacementTab::on_collision_check_changed(bool checked) {
    // Keep the profile and host-COB combos grayed when collision is off.
    Q_UNUSED(checked);
    const bool active =
        collision_check_->isEnabled() && collision_check_->isChecked();
    collision_profile_combo_->setEnabled(active);
    const bool is_clone =
        kind_combo_->currentData().toString() == QStringLiteral("clone");
    host_cob_combo_->setEnabled(active && is_clone);
    on_draft_control_changed();
}

// ── Draft (Create) operation construction ────────────────────────────

PlacementTab::MakeOpResult PlacementTab::make_operation() {
    MakeOpResult res;
    if (!zone_info_) {
        res.error = tr("Select a zone first");
        return res;
    }
    QString kind = kind_combo_->currentData().toString();
    if (kind.isEmpty()) kind = QStringLiteral("cube");
    const V3 pos = {pos_x_->value(), pos_y_->value(), pos_z_->value()};
    const V3 size = {size_x_->value(), size_y_->value(), size_z_->value()};
    QString name = name_edit_->text().trimmed();

    if (kind == QStringLiteral("clone")) {
        // Cross-bin source takes priority when the user picked a
        // non-current entry from the Source Entry combo AND loaded GAOs
        // from it. Otherwise fall back to the intra-bin source_combo
        // (the current zone's GAOs).
        const quint32 current_entry_idx = zone_info_->entry_index;
        const QVariant picked_src_entry = source_entry_combo_->currentData();
        const QVariant picked_src_gao = source_gao_combo_->currentData();
        const bool cross_bin =
            picked_src_entry.isValid() && picked_src_gao.isValid()
            && picked_src_entry.toUInt() != current_entry_idx
            && source_gao_combo_->currentIndex() >= 0;
        quint32 source_key = 0;
        long long source_entry_idx = -1;
        if (cross_bin) {
            source_key = picked_src_gao.toUInt();
            source_entry_idx = picked_src_entry.toUInt();
            const QString text = source_gao_combo_->currentText();
            const QString src_name_hint =
                text.isEmpty() ? QString()
                               : text.section(QLatin1Char(' '), 1);
            if (name.isEmpty()) {
                QString base = src_name_hint;
                base.replace(QStringLiteral(".gao"), QString());
                if (base.isEmpty())
                    base = QStringLiteral("gao_") + hex_key(source_key);
                name = base + QStringLiteral("_xclone.gao");
            }
        } else {
            if (source_combo_->currentIndex() < 0) {
                res.error = tr(
                    "Select a source GAO to clone (intra-bin or pick "
                    "a Source Entry + Source GAO for cross-bin)");
                return res;
            }
            source_key = source_combo_->currentData().toUInt();
            const ZoneObject* source = nullptr;
            auto oit = zone_info_->objects_by_key.find(source_key);
            if (oit != zone_info_->objects_by_key.end())
                source = &zone_info_->objects[size_t(oit->second)];
            if (name.isEmpty()) {
                QString base = source
                                   ? source->name
                                   : QStringLiteral("gao_") + hex_key(source_key);
                base.replace(QStringLiteral(".gao"), QString());
                name = base + QStringLiteral("_copy.gao");
            }
        }
        // Non-visual objects (markers) have no geometry and often a
        // room-sized OBBox — adding clone-collision would spawn a huge
        // collision box. Never add collision when cloning a marker.
        bool src_is_marker = false;
        if (source_entry_idx < 0) {
            auto oit = zone_info_->objects_by_key.find(source_key);
            if (oit != zone_info_->objects_by_key.end())
                src_is_marker =
                    zone_info_->objects[size_t(oit->second)].is_marker;
        }
        const bool want_collision =
            collision_check_->isChecked() && !src_is_marker;
        Placement op;
        op.kind = QStringLiteral("clone");
        op.source_key = (long long)source_key;
        op.name = name;
        op.position = pos;
        op.collision = want_collision;
        if (source_entry_idx >= 0) {
            // Map BF entry index -> BF FAT key (what AddObject stores).
            auto fit = bf_->files.find(quint32(source_entry_idx));
            if (fit != bf_->files.end() && fit->second.key != 0xFFFFFFFFu)
                op.source_entry_key = (long long)fit->second.key;
        }
        // Pass clone_with_collision through too so ProjectPanel's
        // AddObject reconstruction picks it up; the placer reads
        // 'collision' but the project layer uses clone_with_collision
        // to keep the semantic distinct from non-clone collision.
        op.clone_with_collision = op.collision;
        if (op.collision) {
            const QString profile =
                collision_profile_combo_->currentData().toString();
            if (!profile.isEmpty() && profile != QStringLiteral("simple_box"))
                op.collision_profile = profile;
            const QVariant host_cob = host_cob_combo_->currentData();
            if (host_cob.isValid())
                op.room_cob_key = (long long)host_cob.toUInt();
        }
        res.ok = true;
        res.placement = std::move(op);
        return res;
    }

    if (kind == QStringLiteral("model")) {
        const QString model_path = model_path_edit_->text().trimmed();
        if (model_path.isEmpty()) {
            res.error = tr("Choose an imported model file");
            return res;
        }
        if (!QFileInfo::exists(model_path)
            || !QFileInfo(model_path).isFile()) {
            res.error =
                tr("Model file was not found: %1").arg(model_path);
            return res;
        }
        if (name.isEmpty()) {
            QString base = QFileInfo(model_path).completeBaseName();
            if (base.isEmpty()) base = QStringLiteral("model");
            name = QStringLiteral("Placed_%1.gao").arg(base);
        }
        Placement op;
        op.kind = QStringLiteral("model");
        op.model_path = model_path;
        op.name = name;
        op.position = pos;
        op.size = size;
        op.material_key = (long long)material_combo_->currentData().toUInt();
        op.collision = collision_check_->isChecked();
        op.collision_profile =
            collision_profile_combo_->currentData().toString();
        op.import_vertex_colors = vcolor_check_->isChecked();
        res.ok = true;
        res.placement = std::move(op);
        return res;
    }

    if (kind == QStringLiteral("replace")
        || kind == QStringLiteral("replace_bin")) {
        if (kind == QStringLiteral("replace_bin")) {
            if (source_entry_combo_->currentIndex() < 0) {
                res.error = tr("Select a source entry to copy from");
                return res;
            }
            Placement op;
            op.kind = kind;
            op.source_entry_index =
                (long long)source_entry_combo_->currentData().toUInt();
            op.name = tr("BinReplace from %1")
                          .arg(source_entry_combo_->currentText());
            res.ok = true;
            res.placement = std::move(op);
            return res;
        }

        if (source_combo_->currentIndex() < 0) {
            res.error = tr("Select a target GAO to replace");
            return res;
        }
        const quint32 target_key = source_combo_->currentData().toUInt();
        const ZoneObject* target = nullptr;
        auto target_it = zone_info_->objects_by_key.find(target_key);
        if (target_it != zone_info_->objects_by_key.end())
            target = &zone_info_->objects[size_t(target_it->second)];
        if (name.isEmpty()) {
            const QString target_name =
                target ? target->name
                       : QStringLiteral("0x%1")
                             .arg(target_key, 8, 16, QLatin1Char('0'));
            name = QStringLiteral("Replace_%1").arg(target_name);
        }

        Placement op;
        op.kind = kind;
        op.target_key = (long long)target_key;
        op.name = name;
        op.size = size;
        const QString replace_from =
            replace_from_combo_->currentData().toString();
        if (replace_from == QStringLiteral("existing_gao")) {
            if (source_gao_combo_->currentIndex() < 0) {
                res.error = tr("Select a source GAO to copy geometry from");
                return res;
            }
            if (source_entry_combo_->currentIndex() < 0) {
                res.error = tr("Select a source entry to copy from");
                return res;
            }
            op.source_key =
                (long long)source_gao_combo_->currentData().toUInt();
            op.source_entry_index =
                (long long)source_entry_combo_->currentData().toUInt();
            if (!bf_) {
                res.error = tr(
                    "BigFile reference is required for GAO-source replacement");
                return res;
            }
            auto fit = bf_->files.find(quint32(op.source_entry_index));
            if (fit != bf_->files.end() && fit->second.key != INVALID_KEY)
                op.source_entry_key = (long long)fit->second.key;
        } else if (replace_from == QStringLiteral("imported_file")) {
            if (!imported_jgao_) {
                res.error = tr(
                    "No .jgao file imported. Click 'Import GAO' first.");
                return res;
            }
            op.imported_jgao = imported_jgao_;
            op.size = {1.0, 1.0, 1.0};
        } else {
            const QString model_path = model_path_edit_->text().trimmed();
            if (model_path.isEmpty() || !QFileInfo::exists(model_path)
                || !QFileInfo(model_path).isFile()) {
                res.error = tr("Choose an imported model file for replacement");
                return res;
            }
            op.model_path = model_path;
        }
        res.ok = true;
        res.placement = std::move(op);
        return res;
    }

    if (name.isEmpty()) name = QStringLiteral("Placed_%1.gao").arg(kind);
    Placement op;
    op.kind = kind;
    op.name = name;
    op.position = pos;
    op.size = size;
    op.material_key = (long long)material_combo_->currentData().toUInt();
    op.collision = collision_check_->isChecked();
    op.collision_profile = collision_profile_combo_->currentData().toString();
    if (prim_color_) op.vertex_color = prim_color_;
    res.ok = true;
    res.placement = std::move(op);
    return res;
}

// Place the configured object at the viewport cursor's ray-cast hit
// point and pop it into the Inspector.
//
// Spawn location priority: pick under the last cursor position over the
// canvas → plane at scene-centre height → camera forward-projection.
// That way clicking Place always lands the object somewhere visible on
// the surface the user was looking at.
void PlacementTab::on_add_pending_object() {
    if (!zone_info_) {
        QMessageBox::warning(this, tr("Place Object"),
                             tr("Select a zone first."));
        return;
    }
    // Cursor-based spawn point — falls through to camera focus if the
    // user has never moved the mouse over the canvas. The viewport
    // returns display coords (Y-up); placement.position stores Jade
    // coords so the AddObject op writes the right global-matrix
    // translation. Convert before storing.
    const V3 focus_display = viewer_panel_->viewer()->focus_point_under_cursor();
    const V3 focus_jade = display_to_jade(focus_display);
    QDoubleSpinBox* spins[3] = {pos_x_, pos_y_, pos_z_};
    for (int i = 0; i < 3; ++i) {
        spins[i]->blockSignals(true);
        spins[i]->setValue(focus_jade[size_t(i)]);
        spins[i]->blockSignals(false);
    }
    MakeOpResult made = make_operation();
    if (!made.ok) {
        QMessageBox::warning(this, tr("Place Object"), made.error);
        return;
    }
    const QString kind = made.placement.kind;
    if (kind == QStringLiteral("replace")
        || kind == QStringLiteral("replace_bin")) {
        QMessageBox::information(
            this, tr("Place Object"),
            tr("'%1' isn't yet routed through the Project ops system. "
               "Use the legacy direct-write workflow for now.")
                .arg(kind));
        return;
    }
    draft_preview_active_ = false;
    const quint32 syn_key = next_synthetic_key_++;
    made.placement.synthetic_key = syn_key;
    pending_new_objects_.push_back(syn_key);
    pending_objects_by_key_[syn_key] = made.placement;
    // Incremental: build + insert just this object's meshes, leave the
    // rest of the scene untouched. Avoids the multi-second load_meshes
    // rebuild for ~840-mesh zones.
    rebuild_pending_object(syn_key);
    // Auto-select + switch tabs so the gizmo appears immediately.
    viewer_panel_->viewer()->select_gao((long long)syn_key);
    tabs_->setCurrentIndex(0);
    update_map_dirty_indicator();
    const Placement& p = pending_objects_by_key_[syn_key];
    log_->append(
        tr("+ placed %1 '%2' at (%3, %4, %5) jade [profile=%6]")
            .arg(kind, p.name)
            .arg(p.position[0], 0, 'f', 1)
            .arg(p.position[1], 0, 'f', 1)
            .arg(p.position[2], 0, 'f', 1)
            .arg(p.collision_profile.isEmpty()
                     ? QStringLiteral("simple_box")
                     : p.collision_profile));
}

// Rebuild a single pending object's preview meshes via the incremental
// viewport path. Used when the placement's *geometry* changes —
// inspector edits to position, size, source, collision settings, etc.
//
// NOT called from the gizmo-drag transform handler — that wipes
// rotation/scale because replace_object_meshes recreates the group at
// identity transform. The handler just persists the TRS values; the
// viewport already shows the correct visual via the live group
// transform.
void PlacementTab::rebuild_pending_object(quint32 syn_key) {
    auto pit = pending_objects_by_key_.find(syn_key);
    if (pit == pending_objects_by_key_.end()) return;
    Placement& placement = pit->second;
    // Alias the source GAO's textures onto the clone's synthetic key
    // BEFORE replace_object_meshes runs, which reads the viewport's
    // textures_by_key. Otherwise the cloned door renders as flat grey
    // instead of with the source door's diffuse map.
    if (placement.kind == QStringLiteral("clone")) {
        const long long src_key = placement.source_key;
        if (src_key >= 0) {
            // Cross-bin: the source GAO's textures aren't in the
            // current zone's texture map, so alias_textures would
            // return false. Pre-register the source bin's per-element
            // textures under the source key first, then the alias call
            // succeeds.
            if (placement.source_entry_key >= 0) {
                std::shared_ptr<XbinBin> bin;
                const ZoneObject* src_obj = xbin_source_for_op(placement, &bin);
                if (src_obj != nullptr && bin) {
                    auto tit = bin->textures.find(quint32(src_key));
                    if (tit != bin->textures.end())
                        viewer_panel_->viewer()->set_gao_textures(
                            quint32(src_key), tit->second);
                }
            }
            viewer_panel_->viewer()->alias_textures(syn_key,
                                                    quint32(src_key));
        }
    }
    std::vector<MeshDict> op_meshes = build_preview_meshes(placement);
    for (MeshDict& m : op_meshes) {
        m.gao_key = (long long)syn_key;
        m.pickable = true;
        m.depth_offset = false;  // render with normal depth ordering
    }
    if (viewer_panel_->collision_visible()) {
        std::vector<MeshDict> col_meshes =
            build_collision_preview_meshes(placement);
        for (MeshDict& m : col_meshes) m.gao_key = (long long)syn_key;
        for (MeshDict& m : col_meshes) op_meshes.push_back(std::move(m));
    }
    viewer_panel_->viewer()->replace_object_meshes((long long)syn_key,
                                                   std::move(op_meshes));
    // Snapshot the build-time position so subsequent gizmo drags can
    // convert cumulative offsets back to absolute placement positions
    // correctly. build_preview_meshes bakes the full TRS into the mesh,
    // so the rebuilt group is at identity — the rotation/scale are
    // already in the mesh vertices.
    placement.has_build_position = true;
    placement.build_position = placement.position;
}

// ── Cross-bin clone source resolution (object_placer._xbin_source_for_op) ──

const ZoneObject* PlacementTab::xbin_source_for_op(
    const Placement& placement, std::shared_ptr<XbinBin>* out_bin) {
    if (!bf_) return nullptr;
    if (placement.source_entry_key < 0) return nullptr;
    const quint32 src_entry_key = quint32(placement.source_entry_key);
    const quint32 src_gao_key = quint32(placement.source_key);
    auto cit = xbin_source_cache_.find(src_entry_key);
    std::shared_ptr<XbinBin> bin;
    if (cit != xbin_source_cache_.end()) {
        bin = cit->second;
    } else {
        const jade::BFFile* src_fi = nullptr;
        for (const auto& [idx, fi] : bf_->files) {
            (void)idx;
            if (fi.key == src_entry_key) { src_fi = &fi; break; }
        }
        if (src_fi == nullptr) return nullptr;
        const std::vector<uint8_t> src_raw = bf_->read_data(src_fi->index);
        const jade::LzoResult src_dec =
            jade::decompress_lzo(src_raw.data(), src_raw.size());
        if (!src_dec.ok || src_dec.data.empty()) return nullptr;
        bin = std::make_shared<XbinBin>();
        bin->subs = jade::walk_sub_entries(src_dec.data);
        for (size_t i = 0; i < bin->subs.size(); ++i)
            bin->sub_by_key[bin->subs[i].key] = int(i);
        size_t n_visual = 0;
        collect_zone_objects(bin->subs, bin->objects, n_visual);
        // Only the visual objects matter as clone sources; markers get
        // filtered out by geometry-less handling downstream anyway.
        for (size_t i = 0; i < bin->objects.size(); ++i)
            bin->objects_by_key[bin->objects[i].key] = int(i);
        // Decode the source bin's per-element textures once so the
        // viewport's texture aliasing path can register them under the
        // clone's synthetic key.
        const SubPtrMap by_key = build_sub_ptr_map(bin->subs);
        const std::map<quint32, QImage> tex_by_key =
            decode_zone_textures(bin->subs);
        std::vector<ZoneObject> visuals(bin->objects.begin(),
                                        bin->objects.begin()
                                            + long(n_visual));
        bin->textures =
            resolve_textures_for_objects(visuals, by_key, tex_by_key);
        xbin_source_cache_[src_entry_key] = bin;
    }
    if (out_bin) *out_bin = bin;
    auto oit = bin->objects_by_key.find(src_gao_key);
    if (oit == bin->objects_by_key.end()) return nullptr;
    return &bin->objects[size_t(oit->second)];
}

// ── Preview builders (object_placer.build_preview_meshes & co) ───────

namespace {

// _placement_rotation_euler_deg: pending placements carry a quat from
// the viewport gizmo (committed AddObjects reload through the same
// field after an euler→quat conversion at load).
V3 placement_rotation_euler_deg(const Placement& op) {
    return quat_to_euler_xyz_deg(op.rotation_quat);
}

// _compose_clone_matrix: default rotation/scale → keep source matrix,
// only patch translation. Otherwise compose T·R·S onto the source's
// rotation/scale (with source translation stripped).
Mat16 compose_clone_matrix(const Mat16& source_matrix, const V3& position,
                           const V3& rotation_euler_deg,
                           const V3& scale_xform) {
    bool rot_id = true, scale_id = true;
    for (double v : rotation_euler_deg)
        if (std::abs(v) >= 1e-6) rot_id = false;
    for (double v : scale_xform)
        if (std::abs(v - 1.0) >= 1e-6) scale_id = false;
    if (rot_id && scale_id) {
        Mat16 matrix = source_matrix;
        matrix[12] = position[0];
        matrix[13] = position[1];
        matrix[14] = position[2];
        return matrix;
    }
    Mat16 src_no_t = source_matrix;
    src_no_t[12] = src_no_t[13] = src_no_t[14] = 0.0;
    const Mat16 user_trs = jade::placer::trs_to_matrix(
        position, rotation_euler_deg, scale_xform);
    return jade::placer::col_major_mul(user_trs, src_no_t);
}

}  // namespace

// Build display-space meshes for one staged operation (the Python takes
// a list; every call site passes exactly one op, so the index used for
// _preview_color is always 0 here).
std::vector<MeshDict> PlacementTab::build_preview_meshes(
    const Placement& placement) {
    std::vector<MeshDict> meshes;
    if (!zone_info_) return meshes;
    const ZoneInfo& zone = *zone_info_;
    const quint32 default_mat =
        zone.materials.empty() ? INVALID_KEY : zone.materials.front().key;

    const std::array<float, 4> color = preview_color(0);
    const QString kind = placement.kind;
    const V3 position = placement.position;
    const V3 rotation_euler_deg = placement_rotation_euler_deg(placement);
    const V3 scale_xform = placement.scale_xform;
    const QString name = !placement.name.isEmpty()
                             ? placement.name
                             : QStringLiteral("placement_1");

    if (kind == QStringLiteral("clone")) {
        const ZoneObject* source = nullptr;
        auto oit = zone.objects_by_key.find(quint32(placement.source_key));
        if (oit != zone.objects_by_key.end())
            source = &zone.objects[size_t(oit->second)];
        if (source == nullptr) {
            // Cross-bin fallback: pull the GAO from another BF entry.
            source = xbin_source_for_op(placement);
            if (source == nullptr) return meshes;
        }
        // Cloning a non-visual object (camera / light / trigger / …):
        // no geometry to preview, so emit a marker icon at the clone
        // position, in the source's category.
        if (source->is_marker) {
            MeshDict md;
            md.name = name;
            md.is_marker = true;
            md.marker_category = source->category;
            md.gao_key = -1;
            md.position =
                jade_to_display(position[0], position[1], position[2]);
            const std::array<float, 3> col = category_color3(source->category);
            md.base_color = {col[0], col[1], col[2], 1.0f};
            md.pickable = false;
            meshes.push_back(std::move(md));
            return meshes;
        }
        if (!source->geo) return meshes;
        // Mirror the placer's clone matrix: user T·R·S composed onto
        // the source's rotation/scale (with source translation
        // stripped). Default rotation/scale → only translation is
        // patched, preserving the source matrix byte-for-byte.
        const Mat16 matrix = compose_clone_matrix(
            source->matrix, position, rotation_euler_deg, scale_xform);
        meshes.push_back(mesh_from_geoinfo(*source->geo, nullptr, matrix,
                                           name, color, false));
        return meshes;
    }

    const V3 size = jade::placer::vec3_min(placement.size, 0.001);
    jade::placer::PlacerGeo geo;
    if (kind == QStringLiteral("model")) {
        // Use the same importer as the build path so preview geometry and
        // ordinary collision bounds agree with the committed object.
        try {
            geo = jade::placer::load_model_geometry(
                placement.model_path.toStdString(), size, default_mat,
                /*import_vertex_colors=*/false);
        } catch (const std::exception&) {
            // Draft refreshes also run while the path is being typed. An
            // invalid/incomplete path simply has no preview; commit/build
            // surfaces the importer error to the user.
            return meshes;
        }
    } else {
        geo = jade::placer::build_primitive_geometry(
            kind.toStdString(), size, default_mat);
    }
    const Mat16 matrix = jade::placer::trs_to_matrix(
        position, rotation_euler_deg, scale_xform);
    meshes.push_back(
        mesh_from_placer_geo(geo, matrix, name, color, false));
    return meshes;
}

// Build display-space overlays for Replace Mesh drafts. The target keeps its
// original world matrix; only its geometry is swapped. This is the native
// counterpart of object_placer.build_replacement_preview_meshes.
std::vector<MeshDict> PlacementTab::build_replacement_preview_meshes(
    const std::vector<const Placement*>& placements) {
    std::vector<MeshDict> meshes;
    if (!zone_info_) return meshes;
    const ZoneInfo& zone = *zone_info_;

    for (size_t index = 0; index < placements.size(); ++index) {
        const Placement& op = *placements[index];
        if (op.kind != QStringLiteral("replace") || op.target_key < 0)
            continue;
        auto target_it = zone.objects_by_key.find(quint32(op.target_key));
        if (target_it == zone.objects_by_key.end()) continue;
        const ZoneObject& target = zone.objects[size_t(target_it->second)];
        if (!target.has_matrix) continue;
        const V3 size = jade::placer::vec3_min(op.size, 0.001);
        const quint32 material_key = target.material_key < 0
                                         ? INVALID_KEY
                                         : quint32(target.material_key);
        const std::array<float, 4> color =
            index % 2 == 0
                ? std::array<float, 4>{0.15f, 0.92f, 0.80f, 1.0f}
                : std::array<float, 4>{0.80f, 0.42f, 0.95f, 1.0f};
        const QString name = QStringLiteral("Replace: %1").arg(target.name);

        MeshDict mesh;
        bool have_mesh = false;
        if (op.imported_jgao) {
            const std::vector<uint8_t>& payload = op.imported_jgao->geo_data;
            const jade::GeoInfo geo =
                jade::parse_geometry(payload.data(), payload.size());
            if (!geo.ok)
                throw std::runtime_error(
                    "Could not parse geometry from imported .jgao file");
            mesh = mesh_from_geoinfo(geo, nullptr, target.matrix, name,
                                     color, false);
            have_mesh = true;
        } else if (op.source_key >= 0 && op.source_entry_index >= 0) {
            const ZoneObject* source = nullptr;
            if (quint32(op.source_entry_index) == zone.entry_index) {
                auto source_it =
                    zone.objects_by_key.find(quint32(op.source_key));
                if (source_it != zone.objects_by_key.end())
                    source = &zone.objects[size_t(source_it->second)];
            } else {
                source = xbin_source_for_op(op);
            }
            if (!source || !source->geo)
                throw std::runtime_error(
                    "Source GAO geometry could not be loaded");
            const jade::GeoInfo geo =
                scaled_replacement_geo(*source->geo, size);
            mesh = mesh_from_geoinfo(geo, nullptr, target.matrix, name,
                                     color, false);
            have_mesh = true;
        } else if (!op.model_path.isEmpty()) {
            const jade::placer::PlacerGeo geo =
                jade::placer::load_model_geometry(
                    op.model_path.toStdString(), size, material_key,
                    /*import_vertex_colors=*/false);
            mesh = mesh_from_placer_geo(geo, target.matrix, name, color,
                                        false);
            have_mesh = true;
        }
        if (have_mesh) {
            mesh.depth_offset = true;
            meshes.push_back(std::move(mesh));
        }
    }
    return meshes;
}

// Build wireframe ColMap/COB preview meshes for one staged operation
// (object_placer.build_collision_preview_meshes).
std::vector<MeshDict> PlacementTab::build_collision_preview_meshes(
    const Placement& placement) {
    std::vector<MeshDict> meshes;
    if (!zone_info_) return meshes;
    const ZoneInfo& zone = *zone_info_;
    const quint32 default_mat =
        zone.materials.empty() ? INVALID_KEY : zone.materials.front().key;
    const SubPtrMap by_key = build_sub_ptr_map(zone.subs);

    const QString kind = placement.kind;
    const V3 position = placement.position;
    const QString name = !placement.name.isEmpty()
                             ? placement.name
                             : QStringLiteral("placement_1");
    const std::array<float, 4> color = collision_color(0, true);

    if (kind == QStringLiteral("clone")) {
        const ZoneObject* source = nullptr;
        auto oit = zone.objects_by_key.find(quint32(placement.source_key));
        if (oit != zone.objects_by_key.end())
            source = &zone.objects[size_t(oit->second)];
        std::shared_ptr<XbinBin> xbin;
        if (source == nullptr) {
            source = xbin_source_for_op(placement, &xbin);
            if (source == nullptr) return meshes;
        }
        Mat16 matrix = source->matrix;
        matrix[12] = position[0];
        matrix[13] = position[1];
        matrix[14] = position[2];
        // Cross-bin: the source's ColMap/COB live in the source bin's
        // sub-entries, not in our zone.
        const std::vector<uint8_t>* payload = nullptr;
        SubPtrMap xbin_map;
        const SubPtrMap* map = &by_key;
        if (xbin) {
            xbin_map = build_sub_ptr_map(xbin->subs);
            map = &xbin_map;
            if (source->sub_index >= 0)
                payload = &xbin->subs[size_t(source->sub_index)].data;
        } else if (source->sub_index >= 0) {
            payload = &zone.subs[size_t(source->sub_index)].data;
        }
        if (payload != nullptr)
            meshes = collision_meshes_for_gao_payload(
                *payload, *map, matrix,
                QStringLiteral("%1 collision").arg(name), color);
        return meshes;
    }

    if (!placement.collision) return meshes;

    const V3 size = jade::placer::vec3_min(placement.size, 0.001);
    jade::placer::PlacerGeo geo;
    if (kind == QStringLiteral("model")) {
        try {
            geo = jade::placer::load_model_geometry(
                placement.model_path.toStdString(), size, default_mat,
                /*import_vertex_colors=*/false);
        } catch (const std::exception&) {
            return meshes;
        }
    } else {
        geo = jade::placer::build_primitive_geometry(kind.toStdString(),
                                                     size, default_mat);
    }
    const quint32 gmat_key = jade::pick_cob_gamemat_key(by_key);
    // Climbable profile previews the open-front-box collision recipe at
    // the visual's bounding box (the placer always boxes the collision
    // regardless of visual kind).
    std::array<double, 3> lb_min{}, lb_max{};
    jade::CobProfile profile;
    if (placement.collision_profile == QStringLiteral("ledge_openbox")) {
        const jade::CobProfileLookup look =
            jade::get_cob_profile("ledge_openbox");
        if (look.ok) profile = look.profile;
        lb_min = {-size[0] * 0.5, -size[1] * 0.5, -size[2] * 0.5};
        lb_max = {size[0] * 0.5, size[1] * 0.5, size[2] * 0.5};
    } else {
        const jade::CobProfileLookup look = jade::get_cob_profile(
            placement.collision_profile.toStdString());
        if (look.ok) profile = look.profile;
        bounds_from_placer_geo(geo, lb_min, lb_max);
    }
    const std::vector<uint8_t> cob_payload =
        jade::build_cob_triangle_box(lb_min, lb_max, gmat_key, profile);
    if (cob_payload.empty()) return meshes;
    std::vector<uint8_t> cob_full(4 + cob_payload.size());
    std::memcpy(cob_full.data(), &gmat_key, 4);
    std::memcpy(cob_full.data() + 4, cob_payload.data(), cob_payload.size());
    ColGeo cob_geo;
    if (collision_geo_from_cob_full(cob_full, cob_geo))
        meshes.push_back(collision_mesh_from_colgeo(
            cob_geo, jade::placer::matrix_values(position),
            QStringLiteral("%1 collision").arg(name), color));
    return meshes;
}

// Wireframe preview of collision queued via the editor's "Add
// collision" button (green = newly-added). Mesh shape previews the
// object's own visual triangles; box shape previews the bounding box.
std::vector<MeshDict> PlacementTab::build_added_collision_meshes() {
    std::vector<MeshDict> meshes;
    if (!zone_info_) return meshes;
    const ZoneInfo& zone = *zone_info_;
    const SubPtrMap by_key = build_sub_ptr_map(zone.subs);
    const std::array<float, 4> color = {0.20f, 1.0f, 0.40f, 1.0f};
    for (const auto& [gk, shape] : added_collision_shape_) {
        auto pit = added_collision_profile_.find(gk);
        const QString profile = pit != added_collision_profile_.end()
                                    ? pit->second
                                    : QStringLiteral("simple_box");
        auto oit = zone.objects_by_key.find(gk);
        if (oit == zone.objects_by_key.end()) continue;
        const ZoneObject& obj = zone.objects[size_t(oit->second)];
        if (!obj.has_matrix) continue;
        if (shape != QStringLiteral("box")) {
            // Shape-accurate: the object's own visual mesh becomes
            // collision.
            if (obj.geo && obj.geo->nb_tris > 0) {
                MeshDict m = mesh_from_geoinfo(
                    *obj.geo, nullptr, obj.matrix,
                    QStringLiteral("%1 +collision (mesh)").arg(obj.name),
                    color, false);
                m.wireframe = true;
                m.xray = true;
                m.line_width = 1.6f;
                m.depth_offset = false;
                meshes.push_back(std::move(m));
            }
            continue;
        }
        const jade::SubEntry* sub =
            obj.sub_index >= 0 ? &zone.subs[size_t(obj.sub_index)] : nullptr;
        std::array<double, 3> mn{}, mx{};
        bool have_bounds = false;
        if (sub) {
            const jade::ObboxBounds b = jade::obbox_local_bounds(
                sub->data.data(), sub->data.size());
            if (b.ok) {
                mn = b.mn;
                mx = b.mx;
                have_bounds = true;
            }
        }
        if (!have_bounds && obj.geo && !obj.geo->vertices.empty()) {
            mn = {obj.geo->vertices[0], obj.geo->vertices[1],
                  obj.geo->vertices[2]};
            mx = mn;
            const size_t n = obj.geo->vertices.size() / 3;
            for (size_t i = 1; i < n; ++i)
                for (int c = 0; c < 3; ++c) {
                    const double v = obj.geo->vertices[i * 3 + size_t(c)];
                    mn[size_t(c)] = std::min(mn[size_t(c)], v);
                    mx[size_t(c)] = std::max(mx[size_t(c)], v);
                }
            have_bounds = true;
        }
        if (!have_bounds) continue;
        const quint32 gmat_key = jade::pick_cob_gamemat_key(by_key);
        const jade::CobProfileLookup look =
            jade::get_cob_profile(profile.toStdString());
        const std::vector<uint8_t> cob_payload = jade::build_cob_triangle_box(
            mn, mx, gmat_key,
            look.ok ? look.profile : jade::CobProfile{});
        if (cob_payload.empty()) continue;
        std::vector<uint8_t> cob_full(4 + cob_payload.size());
        std::memcpy(cob_full.data(), &gmat_key, 4);
        std::memcpy(cob_full.data() + 4, cob_payload.data(),
                    cob_payload.size());
        ColGeo geo;
        if (collision_geo_from_cob_full(cob_full, geo))
            meshes.push_back(collision_mesh_from_colgeo(
                geo, obj.matrix,
                QStringLiteral("%1 +collision (box)").arg(obj.name), color));
    }
    return meshes;
}

// ── Inspector (Object tab) ───────────────────────────────────────────

ZoneObject* PlacementTab::object_meta(quint32 gao_key) {
    if (!zone_info_) return nullptr;
    auto it = zone_info_->objects_by_key.find(gao_key);
    if (it == zone_info_->objects_by_key.end()) return nullptr;
    return &zone_info_->objects[size_t(it->second)];
}

void PlacementTab::set_inspector_enabled(bool enabled) {
    for (QDoubleSpinBox* s : insp_pos_) s->setEnabled(enabled);
    for (QDoubleSpinBox* s : insp_rot_) s->setEnabled(enabled);
    for (QDoubleSpinBox* s : insp_scale_) s->setEnabled(enabled);
    frame_sel_btn_->setEnabled(enabled);
    insp_material_combo_->setEnabled(enabled);
    for (auto& [cb, bit] : flag_checks_) {
        Q_UNUSED(bit);
        cb->setEnabled(enabled);
    }
}

// Repopulate the Object tab from the current viewport state.
//
// Note: the viewport's group transform is the canonical "current" state
// for *existing* GAOs (it carries any pending edits across selection
// switches). For pending creates we read from the placement in
// pending_objects_by_key_.
void PlacementTab::update_object_tab_for_selection(
    std::optional<quint32> gao_key) {
    selected_gao_key_ = gao_key;
    // Light editor is light-marker-specific — hide for every other
    // selection; the marker path re-shows + fills it when applicable.
    cur_light_.reset();
    cur_light_key_ = -1;
    light_group_->setVisible(false);
    // Pending-create selection: synthetic key in the reserved range.
    if (gao_key && pending_objects_by_key_.count(*gao_key)) {
        populate_inspector_for_pending(*gao_key);
        populate_collision_follow(gao_key);
        return;
    }
    if (!gao_key || !zone_info_) {
        inspector_header_->setText(
            tr("(no selection — click a mesh in the viewport)"));
        inspector_meta_->setText(QString());
        set_inspector_enabled(false);
        write_inspector_values({0, 0, 0}, {0, 0, 0}, {1, 1, 1});
        clear_details();
        loaded_identity_ = 0;
        populate_collision_follow(std::nullopt);
        update_map_dirty_indicator();
        return;
    }

    const ZoneObject* meta = object_meta(*gao_key);
    // Non-visual object marker: movable, but flags / material /
    // collision editing don't apply — show category + position only.
    if (meta != nullptr && meta->is_marker) {
        populate_marker_inspector(*meta);
        return;
    }
    const QString name = meta && !meta->name.isEmpty()
                             ? meta->name
                             : QStringLiteral("gao_") + hex_key(*gao_key);
    inspector_header_->setText(
        QStringLiteral("%1    [%2]   entry: %3")
            .arg(name, hex_key(*gao_key), zone_info_->entry_name));
    QStringList flag_short;
    if (meta && meta->has_colmap) flag_short << QStringLiteral("ColMap");
    if (meta && meta->has_ode) flag_short << QStringLiteral("ODE");
    if (meta && meta->is_root) flag_short << QStringLiteral("root");
    size_t n_el = 0;
    if (meta && meta->geo) n_el = meta->geo->elements.size() / 2;
    if (n_el == 0) n_el = 1;
    inspector_meta_->setText(
        QStringLiteral("elements %1 · %2")
            .arg(n_el)
            .arg(flag_short.isEmpty() ? tr("no special flags")
                                      : flag_short.join(QStringLiteral(", "))));
    if (meta) populate_details(*meta);
    loaded_identity_ = meta ? meta->identity : 0;

    // Read viewport state — this already reflects committed + any
    // pending edits made to this GAO before its selection switch.
    const std::optional<placement::GaoTransform> xform =
        viewer_panel_->viewer()->get_gao_transform(*gao_key);
    V3 pos{0, 0, 0}, scale{1, 1, 1};
    Quat q{0, 0, 0, 1};
    if (xform) {
        pos = xform->position;
        q = xform->rotation;
        scale = xform->scale;
    }
    // Identity: prefer the user's pending flag-toggle for this GAO
    // (survives selection switches via pending_identity_), then the
    // committed value, then loaded.
    quint32 identity;
    auto pit = pending_identity_.find(*gao_key);
    if (pit != pending_identity_.end()) {
        identity = pit->second;
    } else {
        auto cit = committed_cache_.find(*gao_key);
        identity = cit != committed_cache_.end() ? cit->second.identity
                                                 : loaded_identity_;
    }

    set_inspector_enabled(true);
    write_inspector_values(pos, quat_to_euler_xyz_deg(q), scale);
    inspector_updating_ = true;
    for (auto& [cb, bit] : flag_checks_) cb->setChecked((identity & bit) != 0);
    inspector_updating_ = false;
    d_identity_->setText(hex_key(identity));
    // Sync material combo to the current effective material key:
    // pending edit > shipped value. Done after populate_details so the
    // asterisk on the read-only detail row is consistent.
    sync_inspector_material_combo(*gao_key);
    populate_collision_follow(gao_key);
    update_map_dirty_indicator();
}

// Inspector for a non-visual object (camera / light / trigger / …).
//
// These have no geometry or material, so only the transform is
// editable. Moving one emits a ModifyTransform on Commit, exactly like
// a visual object. Light markers additionally show the Light editor,
// which stages an EditLight op on Commit.
void PlacementTab::populate_marker_inspector(const ZoneObject& meta) {
    const QString label = category_label(meta.category);
    const QString name = !meta.name.isEmpty()
                             ? meta.name
                             : QStringLiteral("gao_") + hex_key(meta.key);
    inspector_header_->setText(QStringLiteral("%1    [%2]   %3")
                                   .arg(name, hex_key(meta.key), label));
    const QString flags = meta.flag_names.isEmpty()
                              ? tr("no special flags")
                              : meta.flag_names.join(QStringLiteral(", "));
    inspector_meta_->setText(
        QStringLiteral("Non-visual · %1 · %2").arg(label, flags));
    populate_details(meta);
    loaded_identity_ = meta.identity;

    const std::optional<placement::GaoTransform> xform =
        viewer_panel_->viewer()->get_gao_transform(meta.key);
    V3 pos{0, 0, 0}, scale{1, 1, 1};
    Quat q{0, 0, 0, 1};
    if (xform) {
        pos = xform->position;
        q = xform->rotation;
        scale = xform->scale;
    }
    set_inspector_enabled(true);
    write_inspector_values(pos, quat_to_euler_xyz_deg(q), scale);
    // Flags + material aren't meaningful for a markerless object — show
    // the identity bits read-only, disable editing.
    inspector_updating_ = true;
    for (auto& [cb, bit] : flag_checks_) {
        cb->setChecked((loaded_identity_ & bit) != 0);
        cb->setEnabled(false);
    }
    inspector_updating_ = false;
    insp_material_combo_->setEnabled(false);
    d_identity_->setText(hex_key(loaded_identity_));
    // Collision-follow / add-collision don't apply to markers.
    populate_collision_follow(std::nullopt);
    // Light editor — only when this marker resolves to a GRO_Light.
    populate_light_editor(meta);
    update_map_dirty_indicator();
}

// ── Light editor ─────────────────────────────────────────────────────

// Fill + show the Light group for a light marker, applying any pending
// (uncommitted) field edits over the shipped values. Hides the group
// for non-light markers.
void PlacementTab::populate_light_editor(const ZoneObject& meta) {
    if (!meta.light || meta.light_key < 0) {
        cur_light_.reset();
        cur_light_key_ = -1;
        light_group_->setVisible(false);
        return;
    }
    // Effective values = shipped light + any pending edit for this GAO.
    const jade::LightInfo& info = *meta.light;
    cur_light_ = info;
    cur_light_key_ = meta.light_key;
    const PendingLightEdit* pend = nullptr;
    auto pit = pending_light_edits_.find(meta.key);
    if (pit != pending_light_edits_.end()) pend = &pit->second;

    const int eff_type = pend && pend->light_type ? *pend->light_type
                                                  : int(info.type);
    std::optional<std::array<int, 3>> eff_diffuse;
    if (pend && pend->diffuse)
        eff_diffuse = pend->diffuse;
    else if (info.diffuse.present)
        eff_diffuse = light_rgb(info.diffuse.bits);
    std::optional<std::array<int, 3>> eff_specular;
    if (pend && pend->specular)
        eff_specular = pend->specular;
    else if (info.specular.present)
        eff_specular = light_rgb(info.specular.bits);
    const double eff_near =
        pend && pend->near_ ? *pend->near_
                            : (info.near_.present
                                   ? double(bits_to_f32(info.near_.bits))
                                   : 0.0);
    const double eff_far =
        pend && pend->far_ ? *pend->far_
                           : (info.far_.present
                                  ? double(bits_to_f32(info.far_.bits))
                                  : 0.0);
    const double eff_inner =
        pend && pend->inner_angle
            ? *pend->inner_angle
            : (info.inner.present ? double(bits_to_f32(info.inner.bits))
                                  : 0.0);
    const double eff_outer =
        pend && pend->outer_angle
            ? *pend->outer_angle
            : (info.outer.present ? double(bits_to_f32(info.outer.bits))
                                  : 0.0);
    const double eff_intensity =
        pend && pend->intensity
            ? *pend->intensity
            : (info.intensity.present
                   ? double(bits_to_f32(info.intensity.bits))
                   : 1.0);

    inspector_updating_ = true;
    const int ti = light_type_combo_->findData(eff_type & 0x7);
    light_type_combo_->setCurrentIndex(ti >= 0 ? ti : 0);
    set_light_swatch(light_diffuse_btn_, eff_diffuse);
    const bool has_spec = info.has_specular;
    light_specular_btn_->setVisible(has_spec);
    light_specular_row_label_->setVisible(has_spec);
    if (has_spec) set_light_swatch(light_specular_btn_, eff_specular);
    light_near_->setValue(eff_near);
    light_far_->setValue(eff_far);
    // Stored radians → display degrees.
    light_inner_->setValue(eff_inner * 180.0 / M_PI);
    light_outer_->setValue(eff_outer * 180.0 / M_PI);
    // Cone angles only matter for spots — disable for other types.
    const bool is_spot = (eff_type & 0x7) == 2;
    light_inner_->setEnabled(is_spot);
    light_outer_->setEnabled(is_spot);
    light_angle_label_->setEnabled(is_spot);
    const bool has_int = info.has_intensity;
    light_intensity_->setVisible(has_int);
    light_intensity_label_->setVisible(has_int);
    if (has_int) light_intensity_->setValue(eff_intensity);
    inspector_updating_ = false;
    light_group_->setVisible(true);
}

// Paint a colour-picker button's face with rgb (0–255) and label it
// with the hex value.
void PlacementTab::set_light_swatch(
    QPushButton* btn, const std::optional<std::array<int, 3>>& rgb) {
    if (!rgb) {
        btn->setText(QStringLiteral("—"));
        btn->setStyleSheet(QString());
        return;
    }
    const int r = (*rgb)[0] & 0xFF, g = (*rgb)[1] & 0xFF,
              b = (*rgb)[2] & 0xFF;
    // Readable text colour over the swatch.
    const QString txt = (0.299 * r + 0.587 * g + 0.114 * b) > 140
                            ? QStringLiteral("#000")
                            : QStringLiteral("#fff");
    btn->setText(QStringLiteral("#%1%2%3")
                     .arg(r, 2, 16, QLatin1Char('0'))
                     .arg(g, 2, 16, QLatin1Char('0'))
                     .arg(b, 2, 16, QLatin1Char('0'))
                     .toUpper());
    btn->setStyleSheet(
        QStringLiteral(
            "background-color: rgb(%1,%2,%3); color: %4; padding: 3px;")
            .arg(r)
            .arg(g)
            .arg(b)
            .arg(txt));
}

namespace {

// Parse "#RRGGBB" swatch labels back into (r,g,b) — Python swatch_rgb.
std::optional<std::array<int, 3>> swatch_rgb(const QPushButton* btn) {
    QString t = btn->text();
    if (t.startsWith(QLatin1Char('#'))) t = t.mid(1);
    if (t.size() != 6) return std::nullopt;
    bool ok1 = false, ok2 = false, ok3 = false;
    const int r = t.mid(0, 2).toInt(&ok1, 16);
    const int g = t.mid(2, 2).toInt(&ok2, 16);
    const int b = t.mid(4, 2).toInt(&ok3, 16);
    if (!ok1 || !ok2 || !ok3) return std::nullopt;
    return std::array<int, 3>{r, g, b};
}

}  // namespace

// Open a colour dialog for the diffuse/specular swatch and stage the
// edit.
void PlacementTab::pick_light_color(const QString& which) {
    if (!cur_light_) return;
    QPushButton* btn = which == QStringLiteral("diffuse")
                           ? light_diffuse_btn_
                           : light_specular_btn_;
    // Seed from the button's current swatch.
    const std::optional<std::array<int, 3>> cur = swatch_rgb(btn);
    const QColor initial = cur ? QColor{(*cur)[0], (*cur)[1], (*cur)[2]}
                               : QColor{255, 255, 255};
    QString title = which;
    if (!title.isEmpty()) title[0] = title[0].toUpper();
    const QColor chosen = QColorDialog::getColor(
        initial, this, tr("%1 colour").arg(title));
    if (!chosen.isValid()) return;
    set_light_swatch(btn,
                     std::array<int, 3>{chosen.red(), chosen.green(),
                                        chosen.blue()});
    on_light_edited();
}

// Read the light controls, diff against the shipped light, and store
// the changed-only field set in pending_light_edits_.
void PlacementTab::on_light_edited() {
    if (inspector_updating_ || !cur_light_) return;
    if (!selected_gao_key_ || cur_light_key_ < 0) return;
    const quint32 gk = *selected_gao_key_;
    const jade::LightInfo& base = *cur_light_;
    PendingLightEdit fields;
    fields.light_key = quint32(cur_light_key_);

    // Type.
    const int new_type = light_type_combo_->currentData().toInt() & 0x7;
    if (new_type != int(base.type & 0x7)) fields.light_type = new_type;

    // Colours (parse from the swatch hex labels).
    const std::optional<std::array<int, 3>> d_rgb =
        swatch_rgb(light_diffuse_btn_);
    if (d_rgb && base.diffuse.present
        && *d_rgb != light_rgb(base.diffuse.bits))
        fields.diffuse = d_rgb;
    if (base.has_specular) {
        const std::optional<std::array<int, 3>> s_rgb =
            swatch_rgb(light_specular_btn_);
        if (s_rgb && base.specular.present
            && *s_rgb != light_rgb(base.specular.bits))
            fields.specular = s_rgb;
    }

    // Range. Compare with a tolerance matching the spinbox precision (3
    // decimals) — the box rounds the shipped float32 on display, so a
    // tighter eps would flag spurious edits on plain selection.
    auto changed = [](double spin_val, double base_val, double tol) {
        return std::abs(spin_val - base_val) > tol;
    };
    const double base_near =
        base.near_.present ? double(bits_to_f32(base.near_.bits)) : 0.0;
    const double base_far =
        base.far_.present ? double(bits_to_f32(base.far_.bits)) : 0.0;
    if (changed(light_near_->value(), base_near, 1e-3))
        fields.near_ = light_near_->value();
    if (changed(light_far_->value(), base_far, 1e-3))
        fields.far_ = light_far_->value();

    // Cone angles. Compared in degree-space (the UI unit) so the
    // rad↔deg round-trip doesn't manufacture a diff; written as
    // radians. Only meaningful for spots.
    if (new_type == 2) {
        const double base_inner_deg =
            (base.inner.present ? double(bits_to_f32(base.inner.bits)) : 0.0)
            * 180.0 / M_PI;
        const double base_outer_deg =
            (base.outer.present ? double(bits_to_f32(base.outer.bits)) : 0.0)
            * 180.0 / M_PI;
        if (changed(light_inner_->value(), base_inner_deg, 1e-2))
            fields.inner_angle = light_inner_->value() * M_PI / 180.0;
        if (changed(light_outer_->value(), base_outer_deg, 1e-2))
            fields.outer_angle = light_outer_->value() * M_PI / 180.0;
    }

    // Intensity (base-size lights only).
    if (base.has_intensity) {
        const double base_int = base.intensity.present
                                    ? double(bits_to_f32(base.intensity.bits))
                                    : 0.0;
        if (changed(light_intensity_->value(), base_int, 1e-3))
            fields.intensity = light_intensity_->value();
    }

    // Toggling spot on/off changes which angle controls are live.
    light_inner_->setEnabled(new_type == 2);
    light_outer_->setEnabled(new_type == 2);
    light_angle_label_->setEnabled(new_type == 2);

    if (!fields.empty()) {
        pending_light_edits_[gk] = fields;
        dirty_gaos_.insert(gk);
    } else {
        pending_light_edits_.erase(gk);
        // Only drop from dirty if there's no transform edit either.
        mark_dirty_from_inspector();
    }
    update_map_dirty_indicator();
}

// ── Collision-follow (preview-confirm) ───────────────────────────────

const jade::collision_follow::CollisionLinks*
PlacementTab::detect_collision_links(quint32 gao_key) {
    if (!zone_info_) return nullptr;
    auto found = collision_links_cache_.find(gao_key);
    if (found != collision_links_cache_.end()) return &found->second;
    try {
        auto inserted = collision_links_cache_.emplace(
            gao_key, jade::collision_follow::detect_collision_links(
                         gao_key, zone_info_->subs));
        return &inserted.first->second;
    } catch (const std::exception& exception) {
        log_->append(QStringLiteral("[collision-follow] detect failed: %1")
                         .arg(QString::fromUtf8(exception.what())));
        return nullptr;
    }
}

std::optional<jade::collision_follow::CollisionLinks>
PlacementTab::enabled_collision_links(quint32 gao_key) {
    const auto* links = detect_collision_links(gao_key);
    if (!links || links->empty()) return std::nullopt;
    jade::collision_follow::CollisionLinks enabled;
    enabled.moved_gao_key = gao_key;
    enabled.same_gao = links->same_gao;
    const auto state = colfollow_enabled_.find(gao_key);
    auto is_enabled = [&](const std::string& id, bool fallback) {
        if (state == colfollow_enabled_.end()) return fallback;
        auto found = state->second.find(id);
        return found == state->second.end() ? fallback : found->second;
    };
    for (const auto& link : links->dedicated)
        if (is_enabled(dedicated_link_id(link.gao_key), true))
            enabled.dedicated.push_back(link);
    for (const auto& link : links->carves)
        if (is_enabled(carve_link_id(link.cob_key), false))
            enabled.carves.push_back(link);
    return enabled.empty()
        ? std::optional<jade::collision_follow::CollisionLinks>{}
        : std::optional<jade::collision_follow::CollisionLinks>{std::move(enabled)};
}

void PlacementTab::populate_collision_follow(
    std::optional<quint32> gao_key) {
    for (QCheckBox* checkbox : colfollow_rows_) {
        colfollow_vbox_->removeWidget(checkbox);
        checkbox->deleteLater();
    }
    colfollow_rows_.clear();
    update_addcollision_ui(gao_key);
    // Only meaningful for existing (already-shipped) movable GAOs.
    const bool pending =
        gao_key && pending_objects_by_key_.count(*gao_key) > 0;
    if (!gao_key || !zone_info_ || pending) {
        colfollow_group_->setVisible(!pending);
        colfollow_info_->setText(tr("(no separate collision)"));
        return;
    }
    colfollow_group_->setVisible(true);
    const auto* links = detect_collision_links(*gao_key);
    if (!links || links->empty()) {
        colfollow_info_->setText(
            links && links->same_gao
                ? tr("Collision is in this object's own GAO — it already "
                     "moves with the object.")
                : tr("No separate collision detected for this object."));
        return;
    }
    QString note = tr("Checked collision moves with the object on Commit. "
                      "Toggle the viewport's Collision view to preview "
                      "the moved collision in orange.");
    if (!links->carves.empty() && links->dedicated.empty())
        note += tr("\nShared room/floor triangles are overlap candidates; "
                   "leave them unchecked unless this object is actually "
                   "baked into that mesh.");
    colfollow_info_->setText(note);
    auto& state = colfollow_enabled_[*gao_key];
    auto add_row = [&](const QString& label, const std::string& id,
                       bool fallback) {
        auto found = state.find(id);
        const bool checked = found == state.end() ? fallback : found->second;
        state[id] = checked;
        auto* checkbox = new QCheckBox(label);
        checkbox->setChecked(checked);
        connect(checkbox, &QCheckBox::toggled, this,
                [this, key=*gao_key, id](bool on) {
                    colfollow_enabled_[key][id] = on;
                    if (viewer_panel_->collision_visible())
                        refresh_preview(nullptr, true);
                });
        colfollow_vbox_->addWidget(checkbox);
        colfollow_rows_.push_back(checkbox);
    };
    for (const auto& link : links->dedicated)
        add_row(QStringLiteral("%1 — dedicated collision (whole)")
                    .arg(qs(link.name)),
                dedicated_link_id(link.gao_key), true);
    for (const auto& link : links->carves)
        add_row(QStringLiteral("%1 — %2/%3 overlapping room triangles (carve)")
                    .arg(qs(link.owner_name))
                    .arg(link.face_indices.size())
                    .arg(link.n_total_faces),
                carve_link_id(link.cob_key), false);
}

std::vector<MeshDict> PlacementTab::build_collision_follow_ghosts(
    std::set<quint32>& hide_owner_keys) {
    std::vector<MeshDict> meshes;
    if (!selected_gao_key_ || !zone_info_
        || pending_objects_by_key_.count(*selected_gao_key_)) return meshes;
    const quint32 moved_key = *selected_gao_key_;
    auto object = zone_info_->objects_by_key.find(moved_key);
    if (object == zone_info_->objects_by_key.end()) return meshes;
    const ZoneObject& moved_object = zone_info_->objects[size_t(object->second)];
    if (!moved_object.has_matrix) return meshes;
    const auto enabled = enabled_collision_links(moved_key);
    if (!enabled) return meshes;
    const auto new_matrix = compute_existing_gao_world_matrix(moved_key);
    if (!new_matrix) return meshes;
    const auto delta = jade::collision_follow::world_delta(
        moved_object.matrix, *new_matrix);
    const std::array<float,4> color{{1.0f, 0.55f, 0.10f, 1.0f}};

    auto moved_owner_matrix = [&](quint32 owner_key) -> std::optional<Mat16> {
        auto found = zone_info_->sub_by_key.find(owner_key);
        if (found == zone_info_->sub_by_key.end()) return std::nullopt;
        const jade::SubEntry& sub = zone_info_->subs[size_t(found->second)];
        const long long offset = jade::global_matrix_offset(
            sub.data.data(), sub.data.size());
        if (offset < 0 || size_t(offset) + 64 > sub.data.size())
            return std::nullopt;
        Mat16 matrix{};
        for (size_t i=0; i<16; ++i)
            matrix[i] = rd_f32(sub.data.data(), size_t(offset) + i*4);
        return jade::collision_follow::math_to_jade16(
            jade::collision_follow::matrix_multiply(
                delta, jade::collision_follow::jade16_to_math(matrix)));
    };
    for (const auto& link : enabled->dedicated) {
        const auto matrix = moved_owner_matrix(link.gao_key);
        if (!matrix) continue;
        hide_owner_keys.insert(link.gao_key);
        for (quint32 cob_key : link.cob_keys) {
            auto found = zone_info_->sub_by_key.find(cob_key);
            if (found == zone_info_->sub_by_key.end()) continue;
            ColGeo geometry;
            if (!collision_geo_from_cob_sub(
                    &zone_info_->subs[size_t(found->second)], geometry))
                continue;
            meshes.push_back(collision_mesh_from_colgeo(
                geometry, *matrix,
                QStringLiteral("%1 (moves)").arg(qs(link.name)), color));
        }
    }
    for (const auto& link : enabled->carves) {
        const auto matrix = moved_owner_matrix(link.owner_gao_key);
        auto found = zone_info_->sub_by_key.find(link.cob_key);
        if (!matrix || found == zone_info_->sub_by_key.end()) continue;
        const jade::SubEntry& sub = zone_info_->subs[size_t(found->second)];
        const jade::CobInfo cob = jade::parse_cob(sub.data.data(), sub.data.size());
        if (!cob.ok) continue;
        ColGeo geometry;
        for (size_t i=0; i+2<cob.verts.size(); i+=3)
            geometry.verts.push_back({cob.verts[i], cob.verts[i+1], cob.verts[i+2]});
        uint32_t global_face = 0;
        std::set<uint32_t> selected(link.face_indices.begin(),
                                    link.face_indices.end());
        for (const jade::CobElement& element : cob.elements)
            for (size_t i=0; i+2<element.faces.size(); i+=3, ++global_face)
                if (selected.count(global_face))
                    geometry.faces.push_back({element.faces[i],
                                              element.faces[i+1],
                                              element.faces[i+2]});
        if (!geometry.faces.empty())
            meshes.push_back(collision_mesh_from_colgeo(
                geometry, *matrix,
                QStringLiteral("%1 subset (moves)").arg(qs(link.owner_name)),
                color));
    }
    return meshes;
}

// ── Add collision to an existing object ──────────────────────────────

// Show/refresh the Add-collision button for the selection.
void PlacementTab::update_addcollision_ui(std::optional<quint32> gao_key) {
    const bool existing =
        gao_key && zone_info_
        && pending_objects_by_key_.count(*gao_key) == 0
        && zone_info_->objects_by_key.count(*gao_key) > 0;
    addcol_group_->setVisible(existing);
    if (!existing) return;
    const bool queued = added_collision_shape_.count(*gao_key) > 0;
    addcol_profile_->setEnabled(!queued);
    if (queued) {
        const QString shape = added_collision_shape_[*gao_key];
        const QString profile = added_collision_profile_[*gao_key];
        for (int i = 0; i < addcol_profile_->count(); ++i) {
            const QString d = addcol_profile_->itemData(i).toString();
            const QString d_shape = d.section(QLatin1Char('/'), 0, 0);
            const QString d_profile = d.section(QLatin1Char('/'), 1, 1);
            if (d_shape == shape
                && (d_shape != QStringLiteral("box")
                    || d_profile == profile)) {
                addcol_profile_->setCurrentIndex(i);
                break;
            }
        }
        addcol_btn_->setText(tr("Remove added collision"));
    } else {
        addcol_btn_->setText(tr("Add collision"));
    }
}

void PlacementTab::on_add_collision_clicked() {
    if (!selected_gao_key_ || !zone_info_) return;
    const quint32 gk = *selected_gao_key_;
    if (pending_objects_by_key_.count(gk)) return;
    if (project_ == nullptr) {
        QMessageBox::warning(
            this, tr("Add Collision"),
            tr("No active project. Create or open one in the Project tab "
               "first."));
        return;
    }
    const quint32 entry_key = zone_info_->entry_key;
    const QString existing =
        find_op_for_gao(QStringLiteral("add_object_collision"), gk);
    if (added_collision_shape_.count(gk) || !existing.isEmpty()) {
        // Toggle off — remove the queued op.
        if (!existing.isEmpty()) {
            project_->remove_operation(existing);
            log_->append(
                QStringLiteral("[-] add_object_collision %1").arg(existing));
        }
        added_collision_shape_.erase(gk);
        added_collision_profile_.erase(gk);
    } else {
        const QString choice = addcol_profile_->currentData().toString();
        QString shape = choice.section(QLatin1Char('/'), 0, 0);
        if (shape.isEmpty()) shape = QStringLiteral("mesh");
        QString profile = choice.section(QLatin1Char('/'), 1, 1);
        if (profile.isEmpty()) profile = QStringLiteral("simple_box");
        // AddObjectCollision op dict (project/ops_transform.py):
        //   {"op": "add_object_collision",
        //    "target": {"entry_key", "gao_key"},
        //    "params": {"collision_profile", "collision_shape"}}
        jade::json::Value op = jade::json::make_obj();
        op.obj["op"] = jade::json::make_str("add_object_collision");
        op.obj["label"] = jade::json::make_str(
            QStringLiteral("add collision GAO %1")
                .arg(qs(hex_key_lower(gk)))
                .toStdString());
        jade::json::Value target = jade::json::make_obj();
        target.obj["entry_key"] =
            jade::json::make_str(hex_key_lower(entry_key));
        target.obj["gao_key"] = jade::json::make_str(hex_key_lower(gk));
        op.obj["target"] = std::move(target);
        jade::json::Value params = jade::json::make_obj();
        params.obj["collision_profile"] =
            jade::json::make_str(profile.toStdString());
        params.obj["collision_shape"] =
            jade::json::make_str(shape.toStdString());
        op.obj["params"] = std::move(params);
        const QString op_id = project_->add_operation(std::move(op));
        added_collision_shape_[gk] = shape;
        added_collision_profile_[gk] = profile;
        log_->append(QStringLiteral("[+] add_object_collision %1: GAO %2 (%3%4)")
                         .arg(op_id, hex_key(gk), shape,
                              shape == QStringLiteral("box")
                                  ? QStringLiteral("/") + profile
                                  : QString()));
    }
    update_addcollision_ui(gk);
    update_map_dirty_indicator();
    if (viewer_panel_->collision_visible()) refresh_preview(nullptr, true);
}

// Set the inspector material combo to reflect the current effective
// material for gao_key — pending edit if any, else the GAO's loaded
// material from the BF. Signal-blocked so the change doesn't trigger
// another picker callback.
void PlacementTab::sync_inspector_material_combo(quint32 gao_key) {
    if (!zone_info_) return;
    quint32 cur = 0;
    auto pit = pending_material_.find(gao_key);
    if (pit != pending_material_.end()) {
        cur = pit->second;
    } else if (pending_objects_by_key_.count(gao_key)) {
        const Placement& placement = pending_objects_by_key_[gao_key];
        cur = placement.material_key > 0 ? quint32(placement.material_key)
                                         : 0;
    } else {
        const ZoneObject* meta = object_meta(gao_key);
        cur = meta && meta->material_key > 0 ? quint32(meta->material_key)
                                             : 0;
    }
    if (!cur) return;
    const int idx = insp_material_combo_->findData(cur);
    if (idx < 0) return;
    insp_material_combo_->blockSignals(true);
    insp_material_combo_->setCurrentIndex(idx);
    insp_material_combo_->blockSignals(false);
}

std::vector<QLabel*> PlacementTab::detail_widgets() {
    return {
        d_gao_key_,    d_geo_key_,     d_mat_key_,  d_father_,
        d_identity_,   d_editor_,      d_mat_type_, d_bv_,
        d_position_,   d_flag_list_,   d_geo_hdr_,  d_geo_counts_,
        d_geo_counts2_, d_skin_,       d_vtxcol_,   d_mat_kind_,
        d_texture_,    d_drawmask_,    d_field3_,   d_collision_,
    };
}

void PlacementTab::clear_details() {
    for (QLabel* w : detail_widgets()) w->setText(QStringLiteral("—"));
}

// Populate the Inspector for a pending (not-yet-committed) new object.
// Pending creates have no GAO yet — their state lives in the placement.
// Edits write back there, and commit turns them into AddObject ops.
void PlacementTab::populate_inspector_for_pending(quint32 syn_key) {
    auto pit = pending_objects_by_key_.find(syn_key);
    if (pit == pending_objects_by_key_.end()) return;
    const Placement& placement = pit->second;
    const QString kind = placement.kind.isEmpty() ? QStringLiteral("?")
                                                  : placement.kind;
    const QString name = !placement.name.isEmpty()
                             ? placement.name
                             : QStringLiteral("new_%1").arg(kind);
    inspector_header_->setText(
        QStringLiteral("Pending: %1 '%2'    [syn %3]")
            .arg(kind, name, hex_key(syn_key)));
    const QString coll = placement.collision ? QStringLiteral("ColMap")
                                             : tr("no collision");
    const QString profile = placement.collision_profile.isEmpty()
                                ? QStringLiteral("simple_box")
                                : placement.collision_profile;
    inspector_meta_->setText(
        QStringLiteral("new %1 · %2 (%3) · material %4")
            .arg(kind, coll, profile,
                 hex_key(placement.material_key > 0
                             ? quint32(placement.material_key)
                             : 0)));

    // Read-only details surface what we know about the staged op.
    // Geometry / material / render cells don't exist until the build
    // allocates them, so blank them for a pending create.
    for (QLabel* w : {d_geo_hdr_, d_geo_counts_, d_geo_counts2_, d_skin_,
                      d_vtxcol_, d_mat_kind_, d_texture_, d_drawmask_,
                      d_field3_, d_collision_})
        w->setText(QStringLiteral("—"));
    const V3 size = placement.size;
    const V3 pos = placement.position;
    d_gao_key_->setText(QStringLiteral("syn %1").arg(hex_key(syn_key)));
    d_geo_key_->setText(tr("(allocated on commit)"));
    d_mat_key_->setText(placement.material_key >= 0
                            ? hex_key(quint32(placement.material_key))
                            : QStringLiteral("—"));
    d_father_->setText(QStringLiteral("(root)"));
    d_identity_->setText(tr("(allocated on commit)"));
    d_editor_->setText(QStringLiteral("—"));
    d_mat_type_->setText(QStringLiteral("—"));
    d_bv_->setText(tr("AABB (generated)"));
    d_position_->setText(QStringLiteral("(%1, %2, %3)")
                             .arg(pos[0], 0, 'f', 3)
                             .arg(pos[1], 0, 'f', 3)
                             .arg(pos[2], 0, 'f', 3));
    d_flag_list_->setText(QStringLiteral("size %1 × %2 × %3")
                              .arg(size[0], 0, 'f', 2)
                              .arg(size[1], 0, 'f', 2)
                              .arg(size[2], 0, 'f', 2));

    // Inspector spinboxes show the placement's full TRS — the
    // `Scale XYZ` row drives scale_xform (the matrix scale, uniform
    // across all kinds), not the primitive's box-side `size` (that one
    // is set once at Create time via the Create tab's Size XYZ row).
    write_inspector_values(pos,
                           quat_to_euler_xyz_deg(placement.rotation_quat),
                           placement.scale_xform);
    // Sync the material combo so the user sees which material is
    // currently driving this pending create's render.
    sync_inspector_material_combo(syn_key);

    // Flags don't apply to pending creates (no GAO yet).
    inspector_updating_ = true;
    for (auto& [cb, bit] : flag_checks_) {
        Q_UNUSED(bit);
        cb->setChecked(false);
    }
    inspector_updating_ = false;
    // Enable position + scale spinboxes; disable rotation + flag
    // checkboxes since they have no meaning for a pending create.
    for (QDoubleSpinBox* w : insp_pos_) w->setEnabled(true);
    for (QDoubleSpinBox* w : insp_scale_) w->setEnabled(true);
    frame_sel_btn_->setEnabled(true);
    for (QDoubleSpinBox* w : insp_rot_) w->setEnabled(false);
    for (auto& [cb, bit] : flag_checks_) {
        Q_UNUSED(bit);
        cb->setEnabled(false);
    }
    update_map_dirty_indicator();
}

void PlacementTab::populate_details(const ZoneObject& meta) {
    d_gao_key_->setText(hex_key(meta.key));
    d_geo_key_->setText(meta.geo_key > 0 ? hex_key(quint32(meta.geo_key))
                                         : QStringLiteral("—"));
    d_mat_key_->setText(meta.material_key > 0
                            ? hex_key(quint32(meta.material_key))
                            : QStringLiteral("—"));
    if (meta.father_key < 0
        || quint32(meta.father_key) == 0xFFFFFFFFu)
        d_father_->setText(QStringLiteral("(root)"));
    else
        d_father_->setText(hex_key(quint32(meta.father_key)));
    d_identity_->setText(hex_key(meta.identity));
    d_editor_->setText(hex_key(meta.editor_flags));
    d_mat_type_->setText(meta.matrix_type >= 0
                             ? QString::number(meta.matrix_type)
                             : QStringLiteral("—"));
    d_position_->setText(QStringLiteral("(%1, %2, %3)")
                             .arg(meta.position[0], 0, 'f', 2)
                             .arg(meta.position[1], 0, 'f', 2)
                             .arg(meta.position[2], 0, 'f', 2));
    d_flag_list_->setText(meta.flag_names.isEmpty()
                              ? QStringLiteral("—")
                              : meta.flag_names.join(QStringLiteral(", ")));
    populate_geo_material_details(meta);
}

// Fill the GEO / material / render-state / bounds detail cells from
// everything we can read about this GAO (its parsed GEO, its material
// sub-entry, the visual block's draw mask + RLI vertex-colour count,
// the OBBox bounds, and its collision flags).
void PlacementTab::populate_geo_material_details(const ZoneObject& meta) {
    const jade::GeoInfo* geo = meta.geo.get();

    // ── Geometry column ──
    if (geo) {
        d_geo_hdr_->setText(
            QStringLiteral("v%1 f1=0x%2 pf=0x%3")
                .arg(geo->version)
                .arg(QString::number(geo->flags1, 16).toUpper(),
                     QString::number(geo->flags2, 16).toUpper()));
        d_geo_counts_->setText(QStringLiteral("%1 / %2")
                                   .arg(geo->nb_points)
                                   .arg(geo->nb_tris));
        d_geo_counts2_->setText(QStringLiteral("%1 / %2")
                                    .arg(geo->nb_uvs)
                                    .arg(geo->nb_elements));
        if (geo->skin_present) {
            const size_t nb = !geo->skin_bones.empty()
                                  ? geo->skin_bones.size()
                                  : geo->skin_nbones;
            d_skin_->setText(tr("skinned (%1 bones)").arg(nb));
        } else {
            d_skin_->setText(geo->skin_ok3 ? tr("normals") : tr("static"));
        }
    } else {
        d_geo_hdr_->setText(QStringLiteral("—"));
        d_geo_counts_->setText(QStringLiteral("—"));
        d_geo_counts2_->setText(QStringLiteral("—"));
        d_skin_->setText(QStringLiteral("—"));
    }

    // ── Material column ──
    const jade::SubEntry* mat_sub = nullptr;
    if (zone_info_ && meta.material_key > 0
        && quint32(meta.material_key) != INVALID_KEY) {
        auto it = zone_info_->sub_by_key.find(quint32(meta.material_key));
        if (it != zone_info_->sub_by_key.end())
            mat_sub = &zone_info_->subs[size_t(it->second)];
    }
    if (mat_sub != nullptr) {
        const int gt = mat_sub->gro_null ? -1 : int(mat_sub->gro_type);
        QString kind;
        switch (gt) {
            case 3: kind = QStringLiteral("single"); break;
            case 4: kind = QStringLiteral("multi"); break;
            case 5: kind = QStringLiteral("multitexture"); break;
            default: kind = QStringLiteral("type %1").arg(gt); break;
        }
        if (gt == 4) {
            const jade::MatInfo mm = jade::parse_material(
                mat_sub->data.data(), mat_sub->data.size(), 4);
            if (mm.ok)
                d_mat_kind_->setText(
                    QStringLiteral("%1 (%2 sub)").arg(kind).arg(mm.n_sub));
            else
                d_mat_kind_->setText(kind);
        } else {
            d_mat_kind_->setText(kind);
        }
        const quint32 tk = jade::resolve_texture_key(mat_sub->data.data(),
                                                     mat_sub->data.size());
        d_texture_->setText(tk ? hex_key(tk) : QStringLiteral("—"));
    } else {
        d_mat_kind_->setText(QStringLiteral("—"));
        d_texture_->setText(QStringLiteral("—"));
    }

    // ── Visual block: draw mask + render-flags + RLI vtx-colour count ──
    const std::vector<uint8_t>* payload = nullptr;
    if (zone_info_ && meta.sub_index >= 0)
        payload = &zone_info_->subs[size_t(meta.sub_index)].data;
    const long long voff = payload ? visual_block_offset(*payload) : -1;
    if (payload && voff >= 0 && size_t(voff) + 16 <= payload->size()) {
        const quint32 draw_mask = rd_u32(payload->data(), size_t(voff) + 8);
        const quint32 field3 = rd_u32(payload->data(), size_t(voff) + 12);
        d_drawmask_->setText(hex_key(draw_mask));
        d_field3_->setText(hex_key(field3));
        // After [gro][grm][draw_mask][field3] comes [u16 sep][u32
        // nbColors].
        QString nbc = QStringLiteral("—");
        if (size_t(voff) + 22 <= payload->size()) {
            const quint16 sep = rd_u16(payload->data(), size_t(voff) + 16);
            const quint32 cnt = rd_u32(payload->data(), size_t(voff) + 18);
            if (sep == 0xFFFF && cnt <= 1000000) nbc = QString::number(cnt);
        }
        d_vtxcol_->setText(nbc);
    } else {
        d_drawmask_->setText(QStringLiteral("—"));
        d_field3_->setText(QStringLiteral("—"));
        d_vtxcol_->setText(QStringLiteral("—"));
    }

    // ── Bounds + collision ──
    bool have_bounds = false;
    jade::ObboxBounds bounds;
    if (payload) {
        bounds = jade::obbox_local_bounds(payload->data(), payload->size());
        have_bounds = bounds.ok;
    }
    bool finite = have_bounds;
    if (have_bounds)
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(bounds.mn[size_t(i)])
                || !std::isfinite(bounds.mx[size_t(i)]))
                finite = false;
    if (have_bounds && finite) {
        const QString kind = meta.has_obbox ? QStringLiteral("OBB")
                                            : QStringLiteral("BV");
        d_bv_->setText(QStringLiteral("%1 (%2, %3, %4) → (%5, %6, %7)")
                           .arg(kind)
                           .arg(bounds.mn[0], 0, 'f', 1)
                           .arg(bounds.mn[1], 0, 'f', 1)
                           .arg(bounds.mn[2], 0, 'f', 1)
                           .arg(bounds.mx[0], 0, 'f', 1)
                           .arg(bounds.mx[1], 0, 'f', 1)
                           .arg(bounds.mx[2], 0, 'f', 1));
    } else {
        d_bv_->setText(meta.has_obbox ? QStringLiteral("OBBox")
                                      : QStringLiteral("AABB"));
    }
    QStringList coll;
    if (meta.has_colmap) coll << QStringLiteral("ColMap");
    if (meta.has_ode) coll << QStringLiteral("ODE");
    d_collision_->setText(coll.isEmpty() ? tr("none")
                                         : coll.join(QStringLiteral(", ")));
}

// Populate the spinboxes without firing the edit handler.
void PlacementTab::write_inspector_values(const V3& pos, const V3& rot_deg,
                                          const V3& scale) {
    inspector_updating_ = true;
    for (int i = 0; i < 3; ++i) {
        insp_pos_[size_t(i)]->setValue(pos[size_t(i)]);
        insp_rot_[size_t(i)]->setValue(rot_deg[size_t(i)]);
        insp_scale_[size_t(i)]->setValue(scale[size_t(i)]);
    }
    inspector_updating_ = false;
}

void PlacementTab::on_inspector_edited() {
    if (inspector_updating_ || !selected_gao_key_) return;
    const V3 pos = {insp_pos_[0]->value(), insp_pos_[1]->value(),
                    insp_pos_[2]->value()};
    const V3 rot_deg = {insp_rot_[0]->value(), insp_rot_[1]->value(),
                        insp_rot_[2]->value()};
    const V3 scale = {insp_scale_[0]->value(), insp_scale_[1]->value(),
                      insp_scale_[2]->value()};
    // Pending-create path: edits mutate the placement and we rebuild
    // ONLY the affected GAO's meshes — not the whole scene. A full
    // refresh_preview rebuilds 842 mesh groups for a hall zone and runs
    // in seconds; the incremental path is instantaneous.
    const quint32 gk = *selected_gao_key_;
    auto pit = pending_objects_by_key_.find(gk);
    if (pit != pending_objects_by_key_.end()) {
        Placement& placement = pit->second;
        placement.position = pos;
        placement.rotation_quat = euler_xyz_deg_to_quat(rot_deg);
        placement.scale_xform = scale;
        // Rebuild bakes the new position into the mesh.
        rebuild_pending_object(gk);
        // Same dirty-mark as the gizmo path — inspector edits to
        // previously-committed AddObjects need to flow back into the
        // saved op on next Commit.
        if (!placement.op_id.isEmpty()) dirty_committed_creates_.insert(gk);
        update_map_dirty_indicator();
        return;
    }
    const Quat q = euler_xyz_deg_to_quat(rot_deg);
    viewer_panel_->viewer()->set_gao_transform(gk, pos, q, scale);
    mark_dirty_from_inspector();
}

void PlacementTab::on_inspector_frame() {
    if (!selected_gao_key_) return;
    viewer_panel_->viewer()->auto_frame();
}

// User picked a new material in the inspector combo.
//
// Three things happen: (1) live-preview in the viewport — resolve the
// new material's per-element textures and push them onto the selected
// GAO's existing mesh nodes (no full scene rebuild); (2) track the
// edit — existing shipped GAO → pending_material_, emitted as a
// ModifyGaoMaterial op on Commit; pending create → update
// placement.material_key directly; (3) update the inspector's
// read-only "Material" detail row.
void PlacementTab::on_inspector_material_picked(int index) {
    Q_UNUSED(index);
    if (!zone_info_ || !selected_gao_key_) return;
    const QVariant data = insp_material_combo_->currentData();
    if (!data.isValid()) return;
    const quint32 new_key = data.toUInt();
    const quint32 gk = *selected_gao_key_;

    // Resolve elements + push the new textures live to the viewport.
    const std::vector<int> matids = gao_element_matids(gk);
    const SubPtrMap by_key = build_sub_ptr_map(zone_info_->subs);
    const std::vector<QImage> per_el = resolve_textures_for_material_impl(
        new_key, matids, by_key, zone_info_->tex_by_key);
    viewer_panel_->viewer()->set_gao_textures(gk, per_el);

    // Track the edit + update dirty state.
    auto pit = pending_objects_by_key_.find(gk);
    if (pit != pending_objects_by_key_.end()) {
        pit->second.material_key = (long long)new_key;
        if (!pit->second.op_id.isEmpty())
            dirty_committed_creates_.insert(gk);
    } else {
        pending_material_[gk] = new_key;
        dirty_gaos_.insert(gk);
    }
    d_mat_key_->setText(hex_key(new_key) + QStringLiteral(" *"));
    update_map_dirty_indicator();
}

// The GAO's geometry-element matIds (one entry per face-group sharing a
// sub-material). Used to size the per-element texture list when
// swapping materials live. For shipped GAOs the elements come from the
// parsed GEO; pending creates use a single-element GEO.
std::vector<int> PlacementTab::gao_element_matids(quint32 gao_key) {
    if (!zone_info_) return {};
    const ZoneObject* meta = object_meta(gao_key);
    if (meta != nullptr)
        return meta->geo ? geoinfo_matids(*meta->geo) : std::vector<int>{};
    // Pending create: single-element GEO.
    return {0};
}

void PlacementTab::on_flag_toggled(bool checked) {
    Q_UNUSED(checked);
    if (inspector_updating_ || !selected_gao_key_) return;
    const quint32 gk = *selected_gao_key_;
    const quint32 new_identity = current_identity_from_checks();
    auto cit = committed_cache_.find(gk);
    const quint32 committed_identity = cit != committed_cache_.end()
                                           ? cit->second.identity
                                           : loaded_identity_for(gk);
    const QString suffix = new_identity != committed_identity
                               ? QStringLiteral(" *")
                               : QString();
    d_identity_->setText(hex_key(new_identity) + suffix);
    // Persist the toggled identity so the edit survives a selection
    // switch — committed when the user finally hits Commit.
    if (new_identity == committed_identity)
        pending_identity_.erase(gk);
    else
        pending_identity_[gk] = new_identity;
    mark_dirty_from_inspector();
}

quint32 PlacementTab::current_identity_from_checks() const {
    quint32 identity = loaded_identity_;
    for (const auto& [cb, bit] : flag_checks_) {
        if (cb->isChecked())
            identity |= bit;
        else
            identity &= ~bit;
    }
    return identity;
}

// ── Map-wide dirty tracking / commit / discard ───────────────────────

std::vector<ZoneObject*> PlacementTab::zone_objects() {
    std::vector<ZoneObject*> out;
    if (!zone_info_) return out;
    out.reserve(zone_info_->objects.size());
    for (ZoneObject& o : zone_info_->objects) out.push_back(&o);
    return out;
}

// Return the absolute Jade-space global_matrix for an existing GAO
// under its current viewport transform, or nullopt if any required
// piece is missing.
//
// Composes the gizmo's display-space group matrix onto the GAO's
// shipped Jade matrix and converts the result back into Jade frame —
// exactly what the runtime will read out of the BF after the op
// applies.
std::optional<Mat16> PlacementTab::compute_existing_gao_world_matrix(
    quint32 gao_key) {
    if (!zone_info_) return std::nullopt;
    const std::optional<placement::GaoTransform> group =
        viewer_panel_->viewer()->get_gao_transform(gao_key);
    if (!group) return std::nullopt;
    const ZoneObject* obj = object_meta(gao_key);
    if (obj == nullptr || !obj->has_matrix) return std::nullopt;
    return compose_world_matrix_jade(*group, obj->matrix);
}

// Return the absolute Jade-space matrix for a pending / committed
// AddObject placement under its current viewport transform.
//
// Base matrix = what was baked into the preview mesh at the last
// rebuild; the viewport group transform layered on top captures any
// user gizmo edits since. For primitives + models the bake was a
// translation matrix at the build position; for clones the source's
// full matrix with translation patched to the placement position.
std::optional<Mat16> PlacementTab::compute_pending_gao_world_matrix(
    quint32 syn_key, const Placement& placement) {
    if (!zone_info_) return std::nullopt;
    const std::optional<placement::GaoTransform> group =
        viewer_panel_->viewer()->get_gao_transform(syn_key);
    if (!group) return std::nullopt;
    const V3 build_pos = placement.has_build_position
                             ? placement.build_position
                             : placement.position;
    Mat16 base_matrix = IDENTITY_MATRIX_16;
    if (placement.kind == QStringLiteral("clone")) {
        const ZoneObject* source =
            object_meta(quint32(placement.source_key));
        if (source != nullptr && source->has_matrix)
            base_matrix = source->matrix;
    }
    base_matrix[12] = build_pos[0];
    base_matrix[13] = build_pos[1];
    base_matrix[14] = build_pos[2];
    return compose_world_matrix_jade(*group, base_matrix);
}

bool PlacementTab::state_matches_committed(quint32 gao_key, const V3& pos,
                                           const Quat& q, const V3& scale,
                                           quint32 identity) const {
    auto it = committed_cache_.find(gao_key);
    if (it == committed_cache_.end()) {
        // No prior commit — committed == load identity, identity
        // transform.
        return approx_eq(pos, V3{0, 0, 0}) && approx_eq(q, Quat{0, 0, 0, 1})
               && approx_eq(scale, V3{1, 1, 1})
               && identity == loaded_identity_for(gao_key);
    }
    const CommittedState& committed = it->second;
    return approx_eq(pos, committed.pos)
           && approx_eq(q, committed.rot_quat)
           && approx_eq(scale, committed.scale)
           && identity == committed.identity;
}

quint32 PlacementTab::loaded_identity_for(quint32 gao_key) const {
    if (!zone_info_) return 0;
    auto it = zone_info_->objects_by_key.find(gao_key);
    if (it == zone_info_->objects_by_key.end()) return 0;
    return zone_info_->objects[size_t(it->second)].identity;
}

// Refresh dirty membership for the currently-selected GAO based on the
// inspector's current widgets vs the committed cache. Called from every
// edit handler.
void PlacementTab::mark_dirty_from_inspector() {
    if (!selected_gao_key_) {
        update_map_dirty_indicator();
        return;
    }
    const quint32 gk = *selected_gao_key_;
    const V3 pos = {insp_pos_[0]->value(), insp_pos_[1]->value(),
                    insp_pos_[2]->value()};
    const V3 rot_deg = {insp_rot_[0]->value(), insp_rot_[1]->value(),
                        insp_rot_[2]->value()};
    const V3 scale = {insp_scale_[0]->value(), insp_scale_[1]->value(),
                      insp_scale_[2]->value()};
    const Quat q = euler_xyz_deg_to_quat(rot_deg);
    const quint32 identity = current_identity_from_checks();
    if (state_matches_committed(gk, pos, q, scale, identity))
        dirty_gaos_.erase(gk);
    else
        dirty_gaos_.insert(gk);
    update_map_dirty_indicator();
}

void PlacementTab::update_map_dirty_indicator() {
    const int n_mod = int(dirty_gaos_.size());
    const int n_new = int(pending_new_objects_.size());
    const int n_reissue = int(dirty_committed_creates_.size());
    QStringList parts;
    if (n_mod)
        parts << tr("%1 object%2 modified")
                     .arg(n_mod)
                     .arg(n_mod != 1 ? QStringLiteral("s") : QString());
    if (n_new)
        parts << tr("%1 new object%2 pending")
                     .arg(n_new)
                     .arg(n_new != 1 ? QStringLiteral("s") : QString());
    if (n_reissue)
        parts << tr("%1 placement%2 updated")
                     .arg(n_reissue)
                     .arg(n_reissue != 1 ? QStringLiteral("s") : QString());
    map_dirty_label_->setText(parts.join(QStringLiteral(" · ")));
    const bool enabled = n_mod || n_new || n_reissue;
    commit_map_btn_->setEnabled(enabled);
    discard_map_btn_->setEnabled(enabled);
    // Header asterisk only when the *selected* GAO is dirty.
    const bool header_dirty =
        selected_gao_key_
        && (dirty_gaos_.count(*selected_gao_key_)
            || dirty_committed_creates_.count(*selected_gao_key_));
    const QString header = inspector_header_->text();
    if (header_dirty && !header.endsWith(QStringLiteral(" *")))
        inspector_header_->setText(header + QStringLiteral(" *"));
    else if (!header_dirty && header.endsWith(QStringLiteral(" *")))
        inspector_header_->setText(header.left(header.size() - 2));
}

// Return the existing op of op_type targeting gao_key in the current
// zone entry, or an empty id.
QString PlacementTab::find_op_for_gao(const QString& op_type,
                                      quint32 gao_key) const {
    if (project_ == nullptr || !zone_info_) return QString();
    const std::string entry_hex = hex_key_lower(zone_info_->entry_key);
    const std::string gao_hex = hex_key_lower(gao_key);
    const std::string type = op_type.toStdString();
    for (const jade::json::Value& op : project_->operations) {
        const jade::json::Value* t = op.find("op");
        if (t == nullptr || t->str != type) continue;
        const jade::json::Value* target = op.find("target");
        if (target == nullptr) continue;
        const jade::json::Value* ek = target->find("entry_key");
        const jade::json::Value* gk = target->find("gao_key");
        if (ek == nullptr || gk == nullptr) continue;
        if (parse_hex_key(ek->str) != zone_info_->entry_key) continue;
        if (parse_hex_key(gk->str) != gao_key) continue;
        const jade::json::Value* id = op.find("id");
        if (id != nullptr) return qs(id->str);
    }
    return QString();
}

// On zone load: replay any existing project ops onto the viewport
// per-GAO and seed committed_cache_ from them. GAOs without a queued op
// aren't in the cache — they implicitly equal loaded state.
void PlacementTab::build_committed_cache() {
    committed_cache_.clear();
    dirty_gaos_.clear();
    pending_identity_.clear();
    dirty_committed_creates_.clear();
    pending_material_.clear();
    pending_light_edits_.clear();
    added_collision_shape_.clear();
    added_collision_profile_.clear();
    collision_links_cache_.clear();
    colfollow_enabled_.clear();
    // Pending and committed new-object lists are zone-scoped too: a
    // different zone has a different entry_key, so any pending creates
    // from another zone wouldn't apply here.
    pending_new_objects_.clear();
    committed_new_objects_.clear();
    pending_objects_by_key_.clear();
    next_synthetic_key_ = 0xFE000000u;
    if (project_ == nullptr || !zone_info_) return;
    const quint32 entry_key = zone_info_->entry_key;

    auto op_targets_entry = [&](const jade::json::Value& op,
                                quint32* gao_out) -> bool {
        const jade::json::Value* target = op.find("target");
        if (target == nullptr) return false;
        const jade::json::Value* ek = target->find("entry_key");
        if (ek == nullptr || parse_hex_key(ek->str) != entry_key)
            return false;
        if (gao_out != nullptr) {
            const jade::json::Value* gk = target->find("gao_key");
            if (gk == nullptr) return false;
            *gao_out = parse_hex_key(gk->str);
        }
        return true;
    };
    auto arr3 = [](const jade::json::Value* v, V3 def) -> V3 {
        if (v == nullptr || !v->is_arr() || v->arr.size() < 3) return def;
        return {v->arr[0].num, v->arr[1].num, v->arr[2].num};
    };

    for (const jade::json::Value& op : project_->operations) {
        const jade::json::Value* type = op.find("op");
        if (type == nullptr) continue;
        quint32 gk = 0;
        if (type->str != "modify_transform"
            && type->str != "modify_gao_flags")
            continue;
        if (!op_targets_entry(op, &gk)) continue;
        auto [it, inserted] = committed_cache_.try_emplace(gk);
        if (inserted) it->second.identity = loaded_identity_for(gk);
        const jade::json::Value* params = op.find("params");
        if (params == nullptr) continue;
        if (type->str == "modify_transform") {
            it->second.pos = arr3(params->find("position"), {0, 0, 0});
            it->second.rot_quat = euler_xyz_deg_to_quat(
                arr3(params->find("rotation_euler_deg"), {0, 0, 0}));
            it->second.scale = arr3(params->find("scale"), {1, 1, 1});
        } else {
            const jade::json::Value* sm = params->find("set_mask");
            const jade::json::Value* cm = params->find("clear_mask");
            const quint32 set_mask = sm ? parse_hex_key(sm->str) : 0;
            const quint32 clear_mask = cm ? parse_hex_key(cm->str) : 0;
            const quint32 loaded = loaded_identity_for(gk);
            it->second.identity = (loaded & ~clear_mask) | set_mask;
        }
    }
    // Also pull AddObject ops for this entry so previously-saved
    // creates show up in the viewport preview. Each gets its own
    // synthetic key so it's picking-addressable just like a pending
    // create.
    for (const jade::json::Value& op : project_->operations) {
        const jade::json::Value* type = op.find("op");
        if (type == nullptr || type->str != "add_object") continue;
        if (!op_targets_entry(op, nullptr)) continue;
        const jade::json::Value* params = op.find("params");
        if (params == nullptr) continue;
        Placement placement;
        const jade::json::Value* kind = params->find("kind");
        placement.kind = kind ? qs(kind->str) : QStringLiteral("cube");
        const jade::json::Value* pname = params->find("name");
        if (pname != nullptr) placement.name = qs(pname->str);
        placement.position = arr3(params->find("position"), {0, 0, 0});
        placement.rotation_quat = euler_xyz_deg_to_quat(
            arr3(params->find("rotation_euler_deg"), {0, 0, 0}));
        placement.scale_xform =
            arr3(params->find("scale_xform"), {1, 1, 1});
        if (const auto* v = params->find("size"))
            placement.size = arr3(v, {1, 1, 1});
        if (const auto* v = params->find("collision"))
            placement.collision = v->b;
        if (const auto* v = params->find("collision_profile"))
            placement.collision_profile = qs(v->str);
        if (const auto* v = params->find("clone_with_collision"))
            placement.clone_with_collision = v->b;
        if (const auto* v = params->find("room_cob_key"))
            placement.room_cob_key = (long long)parse_hex_key(v->str);
        if (const auto* v = params->find("source_gao_key"))
            placement.source_key = (long long)parse_hex_key(v->str);
        if (const auto* v = params->find("source_entry_key"))
            placement.source_entry_key = (long long)parse_hex_key(v->str);
        if (const auto* v = params->find("material_key"))
            placement.material_key = (long long)parse_hex_key(v->str);
        if (const auto* v = params->find("model_path"))
            placement.model_path = qs(v->str);
        if (const auto* v = params->find("source");
            v != nullptr && project_ != nullptr)
            placement.model_path = project_->resolve_asset(qs(v->str));
        if (const auto* v = params->find("import_vertex_colors"))
            placement.import_vertex_colors = v->b;
        if (const auto* v = params->find("vertex_color");
            v != nullptr && v->is_arr() && v->arr.size() >= 4)
            placement.vertex_color = std::array<int, 4>{
                int(v->arr[0].num), int(v->arr[1].num), int(v->arr[2].num),
                int(v->arr[3].num)};
        const jade::json::Value* id = op.find("id");
        const quint32 syn_key = next_synthetic_key_++;
        placement.synthetic_key = syn_key;
        placement.op_id =
            id ? qs(id->str) : QString();  // re-commit can replace
        pending_objects_by_key_[syn_key] = placement;
        committed_new_objects_.push_back(syn_key);
    }
    // Re-seed add-collision toggles + green preview from saved ops.
    for (const jade::json::Value& op : project_->operations) {
        const jade::json::Value* type = op.find("op");
        if (type == nullptr || type->str != "add_object_collision") continue;
        quint32 gk = 0;
        if (!op_targets_entry(op, &gk)) continue;
        const jade::json::Value* params = op.find("params");
        QString shape = QStringLiteral("mesh");
        QString profile = QStringLiteral("simple_box");
        if (params != nullptr) {
            if (const auto* v = params->find("collision_shape"))
                shape = qs(v->str);
            if (const auto* v = params->find("collision_profile"))
                profile = qs(v->str);
        }
        added_collision_shape_[gk] = shape;
        added_collision_profile_[gk] = profile;
    }
    // Re-seed light edits from saved ops so the inspector shows the
    // edited state (effective = shipped + this). Not marked dirty — the
    // op already exists, so an untouched light isn't re-committed.
    for (const jade::json::Value& op : project_->operations) {
        const jade::json::Value* type = op.find("op");
        if (type == nullptr || type->str != "edit_light") continue;
        quint32 gk = 0;
        if (!op_targets_entry(op, &gk)) continue;
        if (!gk) continue;
        const jade::json::Value* target = op.find("target");
        const jade::json::Value* lk =
            target ? target->find("light_key") : nullptr;
        const jade::json::Value* params = op.find("params");
        if (lk == nullptr || params == nullptr) continue;
        PendingLightEdit f;
        f.light_key = parse_hex_key(lk->str);
        if (const auto* v = params->find("light_type"))
            f.light_type = int(v->num);
        auto rgb = [](const jade::json::Value* v)
            -> std::optional<std::array<int, 3>> {
            if (v == nullptr || !v->is_arr() || v->arr.size() < 3)
                return std::nullopt;
            return std::array<int, 3>{int(v->arr[0].num), int(v->arr[1].num),
                                      int(v->arr[2].num)};
        };
        f.diffuse = rgb(params->find("diffuse"));
        f.specular = rgb(params->find("specular"));
        if (const auto* v = params->find("near")) f.near_ = v->num;
        if (const auto* v = params->find("far")) f.far_ = v->num;
        if (const auto* v = params->find("inner_angle"))
            f.inner_angle = v->num;
        if (const auto* v = params->find("outer_angle"))
            f.outer_angle = v->num;
        if (const auto* v = params->find("intensity")) f.intensity = v->num;
        if (!f.empty()) pending_light_edits_[gk] = f;
    }
    // Push committed transforms into the viewport so the visible mesh
    // reflects existing project edits.
    for (const auto& [gk, st] : committed_cache_)
        viewer_panel_->viewer()->set_gao_transform(gk, st.pos, st.rot_quat,
                                                   st.scale);
    update_map_dirty_indicator();
}

// Flush every dirty GAO to the project as ops.
void PlacementTab::on_commit_map() {
    if (project_ == nullptr) {
        QMessageBox::warning(
            this, tr("Commit Changes"),
            tr("No active project. Create or open one in the Project tab "
               "first."));
        return;
    }
    if (!zone_info_) return;
    if (dirty_gaos_.empty() && pending_new_objects_.empty()) return;
    const quint32 entry_key = zone_info_->entry_key;
    int committed_any = 0;

    auto num_arr = [](std::initializer_list<double> values) {
        jade::json::Value arr = jade::json::make_arr();
        for (double v : values) arr.arr.push_back(jade::json::make_num(v));
        return arr;
    };
    auto make_target = [&](quint32 gk) {
        jade::json::Value target = jade::json::make_obj();
        target.obj["entry_key"] =
            jade::json::make_str(hex_key_lower(entry_key));
        target.obj["gao_key"] = jade::json::make_str(hex_key_lower(gk));
        return target;
    };

    for (const quint32 gk : dirty_gaos_) {
        const std::optional<placement::GaoTransform> xform =
            viewer_panel_->viewer()->get_gao_transform(gk);
        if (!xform) continue;
        const V3 pos = xform->position;
        const Quat q = xform->rotation;
        const V3 scale = xform->scale;
        const V3 rot_deg = quat_to_euler_xyz_deg(q);
        // Identity priority (matches selection-load path):
        //   1. Selected GAO → live checkbox state
        //   2. Pending flag toggle stored at edit-time
        //   3. Committed cache
        //   4. Loaded identity
        quint32 identity;
        if (selected_gao_key_ && *selected_gao_key_ == gk) {
            identity = current_identity_from_checks();
        } else if (pending_identity_.count(gk)) {
            identity = pending_identity_[gk];
        } else {
            auto cit = committed_cache_.find(gk);
            identity = cit != committed_cache_.end()
                           ? cit->second.identity
                           : loaded_identity_for(gk);
        }
        const quint32 loaded_identity = loaded_identity_for(gk);

        // ── Transform op ────────────────────────────────────────
        const bool is_identity_xform = approx_eq(pos, V3{0, 0, 0})
                                       && approx_eq(rot_deg, V3{0, 0, 0})
                                       && approx_eq(scale, V3{1, 1, 1});
        const QString existing_t =
            find_op_for_gao(QStringLiteral("modify_transform"), gk);
        if (is_identity_xform) {
            if (!existing_t.isEmpty()) {
                project_->remove_operation(existing_t);
                log_->append(QStringLiteral("[-] modify_transform %1")
                                 .arg(existing_t));
            }
        } else {
            if (!existing_t.isEmpty()) project_->remove_operation(existing_t);
            // Compute the absolute Jade-space matrix from the viewport
            // group's current display-space transform composed with the
            // GAO's shipped matrix. Writing this absolute matrix
            // (instead of a decomposed TRS delta) handles both the
            // display↔jade axis swap and the gizmo's pivot-around-
            // bounds-centre semantics which a delta path can't
            // represent.
            const std::optional<Mat16> world_matrix =
                compute_existing_gao_world_matrix(gk);
            jade::json::Value op = jade::json::make_obj();
            op.obj["op"] = jade::json::make_str("modify_transform");
            op.obj["label"] = jade::json::make_str(
                QStringLiteral("transform GAO %1")
                    .arg(qs(hex_key_lower(gk)))
                    .toStdString());
            op.obj["target"] = make_target(gk);
            jade::json::Value params = jade::json::make_obj();
            params.obj["position"] = num_arr({pos[0], pos[1], pos[2]});
            params.obj["rotation_euler_deg"] =
                num_arr({rot_deg[0], rot_deg[1], rot_deg[2]});
            params.obj["scale"] = num_arr({scale[0], scale[1], scale[2]});
            if (world_matrix) {
                jade::json::Value wm = jade::json::make_arr();
                for (double v : *world_matrix)
                    wm.arr.push_back(jade::json::make_num(v));
                params.obj["world_matrix"] = std::move(wm);
            }
            const auto collision_links = enabled_collision_links(gk);
            if (collision_links)
                params.obj["collision_follow"] =
                    collision_links_json(*collision_links);
            op.obj["params"] = std::move(params);
            const QString op_id = project_->add_operation(std::move(op));
            log_->append(QStringLiteral("[+] modify_transform %1: GAO %2%3")
                             .arg(op_id, hex_key(gk),
                                  collision_links
                                      ? QStringLiteral(" (+%1 collision link(s))")
                                            .arg(collision_links->dedicated.size()
                                                 + collision_links->carves.size())
                                      : QString()));
        }

        // ── Flags op ────────────────────────────────────────────
        const QString existing_f =
            find_op_for_gao(QStringLiteral("modify_gao_flags"), gk);
        if (identity == loaded_identity) {
            if (!existing_f.isEmpty()) {
                project_->remove_operation(existing_f);
                log_->append(QStringLiteral("[-] modify_gao_flags %1")
                                 .arg(existing_f));
            }
        } else {
            quint32 exposed_mask = 0;
            for (const auto& [cb, bit] : flag_checks_) {
                Q_UNUSED(cb);
                exposed_mask |= bit;
            }
            const quint32 diff = (identity ^ loaded_identity) & exposed_mask;
            const quint32 set_mask = identity & diff;
            const quint32 clear_mask = ~identity & diff;
            if (!existing_f.isEmpty()) project_->remove_operation(existing_f);
            jade::json::Value op = jade::json::make_obj();
            op.obj["op"] = jade::json::make_str("modify_gao_flags");
            op.obj["label"] = jade::json::make_str(
                QStringLiteral("flags GAO %1")
                    .arg(qs(hex_key_lower(gk)))
                    .toStdString());
            op.obj["target"] = make_target(gk);
            jade::json::Value params = jade::json::make_obj();
            params.obj["set_mask"] =
                jade::json::make_str(hex_key_lower(set_mask));
            params.obj["clear_mask"] =
                jade::json::make_str(hex_key_lower(clear_mask));
            op.obj["params"] = std::move(params);
            const QString op_id = project_->add_operation(std::move(op));
            log_->append(QStringLiteral("[+] modify_gao_flags %1: GAO %2")
                             .arg(op_id, hex_key(gk)));
        }

        // ── Material op ─────────────────────────────────────────
        // pending_material_ only holds entries for SHIPPED GAOs;
        // pending-create material edits live directly on the placement
        // and flow through the AddObject re-issue path below. Always
        // remove the existing op (if any) before emitting a fresh one
        // so the latest pick wins.
        const QString existing_m =
            find_op_for_gao(QStringLiteral("modify_gao_material"), gk);
        auto pmit = pending_material_.find(gk);
        if (pmit != pending_material_.end()) {
            const quint32 new_mat = pmit->second;
            const ZoneObject* loaded_meta = object_meta(gk);
            const quint32 loaded_mat =
                loaded_meta && loaded_meta->material_key > 0
                    ? quint32(loaded_meta->material_key)
                    : 0;
            if (!existing_m.isEmpty()) project_->remove_operation(existing_m);
            if (new_mat != loaded_mat) {
                jade::json::Value op = jade::json::make_obj();
                op.obj["op"] = jade::json::make_str("modify_gao_material");
                op.obj["label"] = jade::json::make_str(
                    QStringLiteral("material GAO %1")
                        .arg(qs(hex_key_lower(gk)))
                        .toStdString());
                op.obj["target"] = make_target(gk);
                jade::json::Value params = jade::json::make_obj();
                params.obj["material_key"] =
                    jade::json::make_str(hex_key_lower(new_mat));
                op.obj["params"] = std::move(params);
                const QString op_id =
                    project_->add_operation(std::move(op));
                log_->append(
                    QStringLiteral("[+] modify_gao_material %1: GAO %2 -> %3")
                        .arg(op_id, hex_key(gk), hex_key(new_mat)));
            } else if (!existing_m.isEmpty()) {
                log_->append(QStringLiteral("[-] modify_gao_material %1")
                                 .arg(existing_m));
            }
        }
        // else: no pending material edit on a previously-committed GAO
        // — leave the existing op in place (an earlier session's edit).

        // ── Light op ────────────────────────────────────────────
        // Light markers stage an EditLight targeting the GRO_Light
        // resource the marker points at. Replace any prior op so the
        // latest field set wins.
        const QString existing_l =
            find_op_for_gao(QStringLiteral("edit_light"), gk);
        auto plit = pending_light_edits_.find(gk);
        if (plit != pending_light_edits_.end()) {
            const PendingLightEdit& pend = plit->second;
            if (!existing_l.isEmpty()) project_->remove_operation(existing_l);
            jade::json::Value op = jade::json::make_obj();
            op.obj["op"] = jade::json::make_str("edit_light");
            op.obj["label"] = jade::json::make_str(
                QStringLiteral("light %1")
                    .arg(qs(hex_key_lower(pend.light_key)))
                    .toStdString());
            jade::json::Value target = make_target(gk);
            target.obj["light_key"] =
                jade::json::make_str(hex_key_lower(pend.light_key));
            op.obj["target"] = std::move(target);
            jade::json::Value params = jade::json::make_obj();
            if (pend.light_type)
                params.obj["light_type"] =
                    jade::json::make_num(*pend.light_type);
            if (pend.diffuse)
                params.obj["diffuse"] =
                    num_arr({double((*pend.diffuse)[0]),
                             double((*pend.diffuse)[1]),
                             double((*pend.diffuse)[2])});
            if (pend.specular)
                params.obj["specular"] =
                    num_arr({double((*pend.specular)[0]),
                             double((*pend.specular)[1]),
                             double((*pend.specular)[2])});
            if (pend.near_)
                params.obj["near"] = jade::json::make_num(*pend.near_);
            if (pend.far_)
                params.obj["far"] = jade::json::make_num(*pend.far_);
            if (pend.inner_angle)
                params.obj["inner_angle"] =
                    jade::json::make_num(*pend.inner_angle);
            if (pend.outer_angle)
                params.obj["outer_angle"] =
                    jade::json::make_num(*pend.outer_angle);
            if (pend.intensity)
                params.obj["intensity"] =
                    jade::json::make_num(*pend.intensity);
            op.obj["params"] = std::move(params);
            const QString op_id = project_->add_operation(std::move(op));
            log_->append(QStringLiteral("[+] edit_light %1: light %2")
                             .arg(op_id, hex_key(pend.light_key)));
        }

        // Update committed cache to reflect the new state.
        CommittedState st;
        st.pos = pos;
        st.rot_quat = q;
        st.scale = scale;
        st.identity = identity;
        committed_cache_[gk] = st;
        ++committed_any;
    }

    // ── Flush pending new-object creations ──────────────────────
    int new_count = 0;
    for (const quint32 syn_key : std::vector<quint32>(
             pending_new_objects_.begin(), pending_new_objects_.end())) {
        auto pit = pending_objects_by_key_.find(syn_key);
        if (pit == pending_objects_by_key_.end()) continue;
        Placement& placement = pit->second;
        jade::json::Value op;
        if (!add_object_op_from_placement(placement, entry_key, op))
            continue;  // already logged + warned
        const QString op_id = project_->add_operation(std::move(op));
        placement.op_id = op_id;
        committed_new_objects_.push_back(syn_key);
        QString body = placement.kind;
        if (placement.kind == QStringLiteral("clone")
            && placement.source_key >= 0)
            body = QStringLiteral("clone %1").arg(
                qs(hex_key_lower(quint32(placement.source_key))));
        log_->append(QStringLiteral("[+] add_object %1: add %2 '%3' -> "
                                    "entry %4")
                         .arg(op_id, body, placement.name,
                              qs(hex_key_lower(entry_key))));
        ++new_count;
    }
    // Pending list emptied; the synthetic-key mapping still points at
    // placements that now live in committed_new_objects_ so the
    // viewport keeps showing them as selectable.
    pending_new_objects_.clear();

    // ── Re-issue dirty committed creates ────────────────────────
    // Gizmo / inspector edits on previously-committed AddObjects update
    // the in-memory placement; the saved project op needs a matching
    // update so the next build picks up the new world_matrix.
    int reissued = 0;
    for (const quint32 syn_key : std::set<quint32>(dirty_committed_creates_)) {
        auto pit = pending_objects_by_key_.find(syn_key);
        if (pit == pending_objects_by_key_.end()) continue;
        Placement& placement = pit->second;
        const QString old_op_id = placement.op_id;
        if (!old_op_id.isEmpty()) project_->remove_operation(old_op_id);
        jade::json::Value op;
        if (!add_object_op_from_placement(placement, entry_key, op))
            continue;
        const QString new_id = project_->add_operation(std::move(op));
        placement.op_id = new_id;
        QString body = placement.kind;
        if (placement.kind == QStringLiteral("clone")
            && placement.source_key >= 0)
            body = QStringLiteral("clone %1").arg(
                qs(hex_key_lower(quint32(placement.source_key))));
        log_->append(
            QStringLiteral("[~] add_object re-issued: add %1 '%2' -> entry "
                           "%3 (%4 → %5)")
                .arg(body, placement.name, qs(hex_key_lower(entry_key)),
                     old_op_id, new_id));
        ++reissued;
    }
    dirty_committed_creates_.clear();

    dirty_gaos_.clear();
    pending_identity_.clear();
    pending_material_.clear();
    pending_light_edits_.clear();
    update_map_dirty_indicator();
    if (selected_gao_key_) {
        // Refresh identity readout (drop the `*`).
        const quint32 sel = *selected_gao_key_;
        auto cit = committed_cache_.find(sel);
        const quint32 committed_identity = cit != committed_cache_.end()
                                               ? cit->second.identity
                                               : loaded_identity_for(sel);
        d_identity_->setText(hex_key(committed_identity));
    }
    log_->append(tr("[*] Commit: %1 modified, %2 new")
                     .arg(committed_any)
                     .arg(new_count));
}

// Build an AddObject op dict from a placement (AddObject._params_to_dict
// shape). Imports any model file into the project's asset store at this
// moment so the op carries a stable asset:<hash> reference. Returns
// false on failure (warning already shown).
bool PlacementTab::add_object_op_from_placement(const Placement& placement,
                                                quint32 entry_key,
                                                jade::json::Value& out) {
    const QString kind = placement.kind;
    // Decompose the placement's quaternion into Euler degrees for
    // AddObject's serialisation field. Quat (0,0,0,1) → (0,0,0)°, so
    // untouched placements round-trip with no rotation field written to
    // the project JSON.
    const V3 rot_euler_deg = quat_to_euler_xyz_deg(placement.rotation_quat);
    const V3 scale_xform = placement.scale_xform;
    // Absolute Jade-space matrix for the BF write. Computed from the
    // viewport group's current display matrix so the saved transform
    // matches exactly what's visible in the editor.
    const std::optional<Mat16> world_matrix =
        placement.synthetic_key
            ? compute_pending_gao_world_matrix(placement.synthetic_key,
                                               placement)
            : std::nullopt;

    auto num_arr3 = [](const V3& v) {
        jade::json::Value arr = jade::json::make_arr();
        for (double x : v) arr.arr.push_back(jade::json::make_num(x));
        return arr;
    };

    jade::json::Value op = jade::json::make_obj();
    op.obj["op"] = jade::json::make_str("add_object");
    op.obj["label"] = jade::json::make_str(
        QStringLiteral("add %1 '%2' -> entry %3")
            .arg(kind, placement.name, qs(hex_key_lower(entry_key)))
            .toStdString());
    jade::json::Value target = jade::json::make_obj();
    target.obj["entry_key"] = jade::json::make_str(hex_key_lower(entry_key));
    op.obj["target"] = std::move(target);

    jade::json::Value params = jade::json::make_obj();
    params.obj["kind"] = jade::json::make_str(kind.toStdString());
    params.obj["name"] = jade::json::make_str(
        (!placement.name.isEmpty()
             ? placement.name
             : QStringLiteral("JadePlaced_%1").arg(kind))
            .toStdString());
    params.obj["position"] = num_arr3(placement.position);
    // Only round-trip non-default rotation/scale (identity factors in
    // T·R·S — omitting them is a true no-op at apply time).
    bool rot_nondefault = false, scale_nondefault = false;
    for (double v : rot_euler_deg)
        if (std::abs(v) > 1e-6) rot_nondefault = true;
    for (double v : scale_xform)
        if (std::abs(v - 1.0) > 1e-6) scale_nondefault = true;
    if (rot_nondefault)
        params.obj["rotation_euler_deg"] = num_arr3(rot_euler_deg);
    if (scale_nondefault) params.obj["scale_xform"] = num_arr3(scale_xform);
    if (world_matrix) {
        jade::json::Value wm = jade::json::make_arr();
        for (double v : *world_matrix)
            wm.arr.push_back(jade::json::make_num(v));
        params.obj["world_matrix"] = std::move(wm);
    }
    if (kind == QStringLiteral("clone")) {
        if (placement.source_key >= 0)
            params.obj["source_gao_key"] = jade::json::make_str(
                hex_key_lower(quint32(placement.source_key)));
        // Cross-bin clone: BF FAT key of the source entry. The op's
        // apply() pre-reads the GAO bytes from that entry and auto-adds
        // the source bin to the target zone's wol deps.
        if (placement.source_entry_key >= 0)
            params.obj["source_entry_key"] = jade::json::make_str(
                hex_key_lower(quint32(placement.source_entry_key)));
        // Forward the clone-with-collision opt-in + chosen profile.
        if (placement.clone_with_collision) {
            params.obj["clone_with_collision"] = jade::json::make_bool(true);
            if (!placement.collision_profile.isEmpty()
                && placement.collision_profile
                       != QStringLiteral("simple_box"))
                params.obj["collision_profile"] = jade::json::make_str(
                    placement.collision_profile.toStdString());
            if (placement.room_cob_key >= 0)
                params.obj["room_cob_key"] = jade::json::make_str(
                    hex_key_lower(quint32(placement.room_cob_key)));
        }
    } else {
        params.obj["size"] = num_arr3(placement.size);
        params.obj["collision"] = jade::json::make_bool(placement.collision);
        if (placement.collision && !placement.collision_profile.isEmpty()
            && placement.collision_profile != QStringLiteral("simple_box"))
            params.obj["collision_profile"] = jade::json::make_str(
                placement.collision_profile.toStdString());
        if (kind == QStringLiteral("model")) {
            const QString model_path = placement.model_path;
            if (!model_path.isEmpty() && project_ != nullptr) {
                QString err;
                const QString asset_ref =
                    project_->import_asset(model_path, &err);
                if (asset_ref.isEmpty()) {
                    QMessageBox::critical(
                        this, tr("Commit Changes"),
                        tr("Failed to import model asset for '%1': %2")
                            .arg(placement.name, err));
                    return false;
                }
                params.obj["source"] =
                    jade::json::make_str(asset_ref.toStdString());
            } else if (!model_path.isEmpty()) {
                params.obj["model_path"] =
                    jade::json::make_str(model_path.toStdString());
            }
            if (placement.import_vertex_colors)
                params.obj["import_vertex_colors"] =
                    jade::json::make_bool(true);
        } else if (placement.vertex_color) {
            jade::json::Value vc = jade::json::make_arr();
            for (int v : *placement.vertex_color)
                vc.arr.push_back(jade::json::make_num(v));
            params.obj["vertex_color"] = std::move(vc);
        }
        if (placement.material_key >= 0)
            params.obj["material_key"] = jade::json::make_str(
                hex_key_lower(quint32(placement.material_key)));
    }
    op.obj["params"] = std::move(params);
    out = std::move(op);
    return true;
}

// Restore every dirty GAO's viewport to committed state and drop any
// pending new-object creations.
void PlacementTab::on_discard_map() {
    if (dirty_gaos_.empty() && pending_new_objects_.empty()) return;
    // Dropping the pending list refreshes the preview below.
    const int dropped_creates = int(pending_new_objects_.size());
    // Drop the synthetic-key entries for discarded pending objects so
    // subsequent picks don't resolve to dangling placements.
    for (const quint32 syn : pending_new_objects_)
        pending_objects_by_key_.erase(syn);
    pending_new_objects_.clear();
    for (const quint32 gk : std::set<quint32>(dirty_gaos_)) {
        auto cit = committed_cache_.find(gk);
        if (cit != committed_cache_.end()) {
            viewer_panel_->viewer()->set_gao_transform(
                gk, cit->second.pos, cit->second.rot_quat,
                cit->second.scale);
        } else {
            // No prior commit → revert to identity.
            viewer_panel_->viewer()->set_gao_transform(
                gk, V3{0, 0, 0}, Quat{0, 0, 0, 1}, V3{1, 1, 1});
        }
    }
    // Discard pending material edits too. For shipped GAOs, push the
    // original texture list back to the viewport so the preview matches
    // the discarded state.
    if (zone_info_) {
        const SubPtrMap by_key = build_sub_ptr_map(zone_info_->subs);
        for (const auto& [gk, new_mat] : pending_material_) {
            Q_UNUSED(new_mat);
            const ZoneObject* meta = object_meta(gk);
            const quint32 orig_mat =
                meta && meta->material_key > 0
                    ? quint32(meta->material_key)
                    : 0;
            if (orig_mat) {
                const std::vector<QImage> per_el =
                    resolve_textures_for_material_impl(
                        orig_mat, gao_element_matids(gk), by_key,
                        zone_info_->tex_by_key);
                viewer_panel_->viewer()->set_gao_textures(gk, per_el);
            }
        }
    }
    pending_material_.clear();
    pending_light_edits_.clear();
    dirty_gaos_.clear();
    pending_identity_.clear();
    // If the currently-selected GAO was among the discarded, refresh
    // its inspector widgets back to its committed state.
    if (selected_gao_key_) refresh_inspector_from_committed();
    // Pending creates removed → drop their preview meshes.
    if (dropped_creates) {
        refresh_preview(nullptr, true);
        log_->append(tr("[-] Discard: dropped %1 pending object(s)")
                         .arg(dropped_creates));
    }
    update_map_dirty_indicator();
}

// Set inspector widgets to the committed state for the currently-
// selected GAO (used after Discard).
void PlacementTab::refresh_inspector_from_committed() {
    if (!selected_gao_key_) return;
    const quint32 gk = *selected_gao_key_;
    V3 pos{0, 0, 0}, scale{1, 1, 1};
    Quat q{0, 0, 0, 1};
    quint32 identity;
    auto cit = committed_cache_.find(gk);
    if (cit != committed_cache_.end()) {
        pos = cit->second.pos;
        q = cit->second.rot_quat;
        scale = cit->second.scale;
        identity = cit->second.identity;
    } else {
        identity = loaded_identity_for(gk);
    }
    write_inspector_values(pos, quat_to_euler_xyz_deg(q), scale);
    inspector_updating_ = true;
    for (auto& [cb, bit] : flag_checks_)
        cb->setChecked((identity & bit) != 0);
    inspector_updating_ = false;
    d_identity_->setText(hex_key(identity));
}

// ── Viewport signal handlers (bidirectional sync) ────────────────────

void PlacementTab::on_viewer_object_selected(qlonglong gao_key) {
    // gao_key == -1 means deselect.
    const std::optional<quint32> gk =
        gao_key != -1 ? std::optional<quint32>(quint32(gao_key))
                      : std::nullopt;
    update_object_tab_for_selection(gk);
    // Keep the clone-source dropdown tracking the viewport selection
    // until the user manually overrides it. Picking a GAO in the
    // viewport while Clone is the active kind is the natural way to say
    // "I want to clone this".
    sync_clone_source_to_selection();
    if (gk) tabs_->setCurrentIndex(0);  // auto-switch to Object tab
}

// A gizmo drag (move / rotate / scale) finished a tick — pull the fresh
// TRS into the inspector spinboxes and refresh the map-wide dirty set.
void PlacementTab::on_viewer_object_transformed(qlonglong gao_key) {
    const quint32 gk = quint32(gao_key);
    // Pending-create path: persist the gizmo's TRS into the placement.
    // We do NOT rebuild the mesh here — the viewport group already has
    // the gizmo's transform applied, and replace_object_meshes would
    // recreate the group at identity, wiping the rotation/scale the
    // user just dialled in.
    auto pit = pending_objects_by_key_.find(gk);
    if (pit != pending_objects_by_key_.end()) {
        const std::optional<placement::GaoTransform> xform =
            viewer_panel_->viewer()->get_gao_transform(gk);
        if (xform) {
            const V3 offset_jade = display_to_jade(xform->position);
            Placement& placement = pit->second;
            // Lazy-init build-position so the cumulative offset is
            // computed against a STABLE anchor. For freshly placed
            // objects, rebuild_pending_object sets this; for placements
            // loaded from a saved project, we capture the load position
            // the first time the gizmo touches them. Without lazy init
            // the fallback position updates each tick → cumulative
            // drift.
            if (!placement.has_build_position) {
                placement.has_build_position = true;
                placement.build_position = placement.position;
            }
            const V3 build_pos = placement.build_position;
            placement.position = {build_pos[0] + offset_jade[0],
                                  build_pos[1] + offset_jade[1],
                                  build_pos[2] + offset_jade[2]};
            placement.rotation_quat = xform->rotation;
            placement.scale_xform = xform->scale;
            // If this placement was previously committed (loaded from
            // the project's saved AddObject), mark it for re-issue on
            // next Commit. Without this, the gizmo work would update
            // the in-memory placement but never reach the saved op →
            // build keeps using the stale transform.
            if (!placement.op_id.isEmpty()) {
                dirty_committed_creates_.insert(gk);
                update_map_dirty_indicator();
            }
            if (selected_gao_key_ && *selected_gao_key_ == gk)
                populate_inspector_for_pending(gk);
        }
        return;
    }
    if (selected_gao_key_ && *selected_gao_key_ == gk) {
        const std::optional<placement::GaoTransform> xform =
            viewer_panel_->viewer()->get_gao_transform(gk);
        if (xform)
            write_inspector_values(xform->position,
                                   quat_to_euler_xyz_deg(xform->rotation),
                                   xform->scale);
        mark_dirty_from_inspector();
        if (viewer_panel_->collision_visible()
            && enabled_collision_links(gk))
            ghost_refresh_timer_->start();
    } else {
        // Edits to an unselected GAO (rare but possible via the
        // transform gizmo if we ever change selection mid-drag): mark
        // dirty directly from viewport state.
        mark_dirty_gao_from_viewport(gk);
    }
}

// Update dirty-set membership for gao_key from its current viewport
// transform (used when the inspector doesn't have this GAO selected,
// e.g. mid-drag of a different object).
void PlacementTab::mark_dirty_gao_from_viewport(quint32 gao_key) {
    const std::optional<placement::GaoTransform> xform =
        viewer_panel_->viewer()->get_gao_transform(gao_key);
    if (!xform) return;
    // Flags can't change without selection (no checkbox UI), so use the
    // current committed identity.
    auto cit = committed_cache_.find(gao_key);
    const quint32 identity = cit != committed_cache_.end()
                                 ? cit->second.identity
                                 : loaded_identity_for(gao_key);
    if (state_matches_committed(gao_key, xform->position, xform->rotation,
                                xform->scale, identity))
        dirty_gaos_.erase(gao_key);
    else
        dirty_gaos_.insert(gao_key);
    update_map_dirty_indicator();
}

// ── Create-tab handlers (preserved from prior version) ───────────────

void PlacementTab::on_replace_from_changed() { on_kind_changed(); }

void PlacementTab::populate_source_entry_combo() {
    source_entry_combo_->clear();
    if (!bf_) return;
    if (zone_info_) {
        source_entry_combo_->addItem(
            QStringLiteral("[current] %1 (#%2)")
                .arg(zone_info_->entry_name)
                .arg(zone_info_->entry_index),
            zone_info_->entry_index);
    }
    std::vector<const jade::BFFile*> files;
    for (const auto& [idx, fi] : bf_->files) {
        (void)idx;
        if (fi.name.empty() || fi.key == INVALID_KEY || fi.length == 0)
            continue;
        files.push_back(&fi);
    }
    std::sort(files.begin(), files.end(),
              [](const jade::BFFile* a, const jade::BFFile* b) {
                  const QString an = qs(a->name).toLower();
                  const QString bn = qs(b->name).toLower();
                  if (an != bn) return an < bn;
                  return a->index < b->index;
              });
    const quint32 current_idx =
        zone_info_ ? zone_info_->entry_index : 0xFFFFFFFFu;
    for (const jade::BFFile* fi : files) {
        if (fi->index == current_idx) continue;
        source_entry_combo_->addItem(
            QStringLiteral("%1 (#%2)").arg(qs(fi->name)).arg(fi->index),
            fi->index);
    }
}

void PlacementTab::on_load_source_entry() {
    if (!bf_ || source_entry_combo_->currentIndex() < 0) return;
    const quint32 entry_index = source_entry_combo_->currentData().toUInt();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    // object_placer.list_entry_visual_objects: lightweight visual-object
    // info for a BF entry (for cross-entry source picking).
    std::vector<std::pair<quint32, QString>> objects;
    {
        const std::vector<uint8_t> raw = bf_->read_data(entry_index);
        const jade::LzoResult dec =
            jade::decompress_lzo(raw.data(), raw.size());
        if (dec.ok && !dec.data.empty()) {
            const std::vector<jade::SubEntry> subs =
                jade::walk_sub_entries(dec.data);
            std::vector<ZoneObject> objs;
            size_t n_visual = 0;
            collect_zone_objects(subs, objs, n_visual);
            for (size_t i = 0; i < n_visual; ++i)
                objects.push_back({objs[i].key, objs[i].name});
        }
    }
    QApplication::restoreOverrideCursor();
    replace_source_objects_ = objects;
    source_gao_combo_->clear();
    for (const auto& [key, name] : objects)
        source_gao_combo_->addItem(
            QStringLiteral("%1 %2").arg(hex_key(key), name), key);
    log_->append(tr("Loaded %1 visual objects from entry %2")
                     .arg(objects.size())
                     .arg(entry_index));
}

void PlacementTab::on_browse_model() {
    QString start = model_path_edit_->text().trimmed();
    if (start.isEmpty())
        start = QFileInfo(bf_path_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Imported Model"), start,
        tr("Model Files (*.fbx *.obj *.gltf *.glb *.dae *.stl *.ply);;"
           "All Files (*)"));
    if (!path.isEmpty()) model_path_edit_->setText(path);
}

// ── Preview / pick / draft refresh ───────────────────────────────────

void PlacementTab::refresh_preview(const Placement* extra_operation,
                                   bool preserve_camera) {
    if (!zone_info_) return;
    const ZoneInfo& zone = *zone_info_;
    // Order: zone meshes → committed creates (already-saved, kept in
    // preview between session edits) → pending creates → the transient
    // draft operation the user's currently designing. Each create is
    // built individually so we can tag its output meshes with its
    // synthetic gao_key — that's what makes the pending object pickable
    // + selectable like a real loaded GAO.
    std::vector<std::pair<const Placement*, long long>> tagged_creates;
    for (const quint32 syn : committed_new_objects_) {
        auto it = pending_objects_by_key_.find(syn);
        if (it != pending_objects_by_key_.end())
            tagged_creates.push_back({&it->second, (long long)syn});
    }
    for (const quint32 syn : pending_new_objects_) {
        auto it = pending_objects_by_key_.find(syn);
        if (it != pending_objects_by_key_.end())
            tagged_creates.push_back({&it->second, (long long)syn});
    }
    if (extra_operation != nullptr)
        tagged_creates.push_back({extra_operation, -1});

    // Replace drafts hide the target's shipped mesh and add an unpickable
    // overlay built from the selected replacement source below.
    std::vector<MeshDict> meshes = zone.scene_meshes;
    std::vector<const Placement*> replace_ops;
    std::set<quint32> replace_keys;
    for (const auto& [op, syn_key] : tagged_creates) {
        Q_UNUSED(syn_key);
        if (op->kind != QStringLiteral("replace") || op->target_key < 0)
            continue;
        replace_ops.push_back(op);
        replace_keys.insert(quint32(op->target_key));
    }
    if (!replace_keys.empty())
        meshes.erase(
            std::remove_if(meshes.begin(), meshes.end(),
                [&](const MeshDict& mesh) {
                    return mesh.gao_key >= 0
                        && replace_keys.count(quint32(mesh.gao_key));
                }),
            meshes.end());

    // Build per-op so each create's meshes can carry its synthetic key.
    // The transient draft (extra_operation, key -1) stays unpickable.
    std::vector<const Placement*> place_ops_for_collision;
    for (const auto& [op, syn_key] : tagged_creates) {
        if (op->kind == QStringLiteral("replace")
            || op->kind == QStringLiteral("replace_bin"))
            continue;
        std::vector<MeshDict> op_meshes = build_preview_meshes(*op);
        if (syn_key != -1) {
            for (MeshDict& m : op_meshes) {
                m.gao_key = syn_key;
                m.pickable = true;
            }
        }
        for (MeshDict& m : op_meshes) meshes.push_back(std::move(m));
        place_ops_for_collision.push_back(op);
    }
    std::vector<MeshDict> replacement_meshes =
        build_replacement_preview_meshes(replace_ops);
    for (MeshDict& mesh : replacement_meshes)
        meshes.push_back(std::move(mesh));

    // Non-visual object markers (cameras / lights / triggers / …).
    // Opt-in via the viewport "Markers" toggle; each is a small
    // category-coloured icon that's pickable + movable exactly like a
    // mesh (its gao_key is the real shipped key).
    int marker_count = 0;
    if (viewer_panel_->markers_visible()) {
        marker_count = int(zone.marker_meshes.size());
        for (const MeshDict& m : zone.marker_meshes) meshes.push_back(m);
    }

    const bool show_collision = viewer_panel_->collision_visible();
    int collision_mesh_count = 0;
    if (show_collision) {
        std::vector<MeshDict> collision_meshes = zone.collision_meshes;
        for (const Placement* op : place_ops_for_collision) {
            std::vector<MeshDict> extra =
                build_collision_preview_meshes(*op);
            for (MeshDict& m : extra)
                collision_meshes.push_back(std::move(m));
        }
        std::set<quint32> hide_collision_owners;
        std::vector<MeshDict> follow_ghosts =
            build_collision_follow_ghosts(hide_collision_owners);
        if (!hide_collision_owners.empty())
            collision_meshes.erase(
                std::remove_if(collision_meshes.begin(), collision_meshes.end(),
                    [&](const MeshDict& mesh) {
                        return mesh.gao_key >= 0
                            && hide_collision_owners.count(quint32(mesh.gao_key));
                    }),
                collision_meshes.end());
        for (MeshDict& ghost : follow_ghosts)
            collision_meshes.push_back(std::move(ghost));
        // Green boxes for collision queued via "Add collision box".
        if (!added_collision_shape_.empty()) {
            std::vector<MeshDict> added = build_added_collision_meshes();
            for (MeshDict& m : added)
                collision_meshes.push_back(std::move(m));
        }
        collision_mesh_count = int(collision_meshes.size());
        for (MeshDict& m : collision_meshes) meshes.push_back(std::move(m));
    }
    // Alias clones onto their source's texture list: a cloned GAO gets
    // a synthetic key but reuses the source's geometry (same vertices,
    // same UVs, same per-element materials). Without this the viewport
    // falls back to flat base_color because the synthetic key isn't in
    // textures_by_gao_key — door clones rendered as solid grey.
    placement::TexturesByKey textures = zone.textures_by_gao_key;
    for (const auto& [op, syn_key] : tagged_creates) {
        if (syn_key == -1) continue;
        if (op->kind != QStringLiteral("clone")) continue;
        if (op->source_key < 0) continue;
        auto sit = textures.find(quint32(op->source_key));
        if (sit != textures.end())
            textures[quint32(syn_key)] = sit->second;
    }
    viewer_panel_->viewer()->load_meshes(meshes, textures, preserve_camera);
    sync_transform_gizmo();
    viewer_panel_->set_animations({});
    const QString collision_text =
        show_collision
            ? QStringLiteral(" | %1 collision meshes").arg(collision_mesh_count)
            : QString();
    const QString marker_text =
        marker_count ? QStringLiteral(" | %1 non-visual markers")
                           .arg(marker_count)
                     : QString();
    const QString replace_text =
        replace_ops.empty()
            ? QString()
            : QStringLiteral(" | %1 replacement(s)").arg(replace_ops.size());
    viewer_panel_->set_info(QStringLiteral("%1 zone objects | %2 preview%3%4%5")
                                .arg(zone.n_visual)
                                .arg(place_ops_for_collision.size())
                                .arg(replace_text, collision_text, marker_text));
}

void PlacementTab::on_collision_preview_toggled(bool checked) {
    Q_UNUSED(checked);
    refresh_preview(nullptr, true);
}

void PlacementTab::on_markers_toggled(bool checked) {
    // Show/hide non-visual object markers in the viewport.
    Q_UNUSED(checked);
    refresh_preview(nullptr, true);
}

void PlacementTab::set_pick_mode(bool enabled) {
    if (pick_check_ != nullptr && pick_check_->isChecked() != enabled) {
        pick_check_->blockSignals(true);
        pick_check_->setChecked(enabled);
        pick_check_->blockSignals(false);
    }
    if (viewer_panel_ != nullptr) {
        const double plane_y = pos_z_ != nullptr ? pos_z_->value() : 0.0;
        viewer_panel_->viewer()->set_point_picking(
            enabled && zone_info_.has_value(), plane_y);
    }
}

void PlacementTab::on_draft_control_changed() {
    if (pick_check_ != nullptr
        && (pick_check_->isChecked() || draft_preview_active_))
        refresh_current_draft(180);
}

void PlacementTab::on_viewer_point_picked(double display_x, double display_y,
                                          double display_z) {
    if (!zone_info_ || !pick_check_->isChecked()) return;
    // display → Jade conversion was (x, -z, y); preserve that here.
    const double vals[3] = {display_x, -display_z, display_y};
    QDoubleSpinBox* spins[3] = {pos_x_, pos_y_, pos_z_};
    for (int i = 0; i < 3; ++i) {
        spins[i]->blockSignals(true);
        spins[i]->setValue(vals[i]);
        spins[i]->blockSignals(false);
    }
    set_pick_mode(true);
    sync_transform_gizmo(true);
    refresh_current_draft(0);
}

void PlacementTab::on_viewer_gizmo_moved(double display_x, double display_y,
                                         double display_z) {
    if (!zone_info_) return;
    // Same display→Jade mapping as point-pick.
    const double vals[3] = {display_x, -display_z, display_y};
    QDoubleSpinBox* spins[3] = {pos_x_, pos_y_, pos_z_};
    for (int i = 0; i < 3; ++i) {
        spins[i]->blockSignals(true);
        spins[i]->setValue(vals[i]);
        spins[i]->blockSignals(false);
    }
    draft_preview_active_ = true;
    refresh_current_draft(16);
}

void PlacementTab::refresh_current_draft(int delay_ms) {
    if (!zone_info_) return;
    if (delay_ms <= 0) {
        draft_refresh_timer_->stop();
        refresh_current_draft_now();
        return;
    }
    draft_refresh_timer_->start(delay_ms);
}

void PlacementTab::refresh_current_draft_now() {
    if (!zone_info_) return;
    const MakeOpResult made = make_operation();
    if (!made.ok) return;
    try {
        refresh_preview(&made.placement, true);
    } catch (const std::exception&) {
        // Match Python's debounced draft refresh: incomplete or temporarily
        // invalid replacement sources leave the previous preview untouched.
    }
}

void PlacementTab::sync_transform_gizmo(std::optional<bool> enabled) {
    if (viewer_panel_ == nullptr) return;
    PlacementViewport* viewer = viewer_panel_->viewer();
    const bool active =
        !enabled.has_value()
            ? (zone_info_.has_value() && draft_preview_active_)
            : (*enabled && zone_info_.has_value());
    const V3 display_pos = pos_x_ != nullptr
                               ? V3{pos_x_->value(), pos_z_->value(),
                                    -pos_y_->value()}
                               : V3{0.0, 0.0, 0.0};
    // Don't fight the selection-driven gizmo. If the user has an object
    // selected, the viewport owns gizmo visibility.
    if (selected_gao_key_ && !active) return;
    viewer->set_transform_gizmo(active, display_pos);
}

// ── GAO file I/O ─────────────────────────────────────────────────────
// Native object_placer.export_gao/import_gao and JGAO↔GLB front-end.

void PlacementTab::on_export_gao() {
    if (!zone_info_ || !bf_) {
        QMessageBox::information(this, tr("Export GAO"),
                                 tr("Load a zone first."));
        return;
    }
    if (source_combo_->currentIndex() < 0) {
        QMessageBox::information(this, tr("Export GAO"),
                                 tr("Select a GAO to export."));
        return;
    }
    const quint32 gao_key = source_combo_->currentData().toUInt();
    const jade::jgao::ExportResult exported =
        jade::jgao::export_gao(zone_info_->subs, gao_key);
    if (!exported.ok) {
        const QString error = QString::fromStdString(exported.error);
        QMessageBox::warning(this, tr("Export GAO Failed"), error);
        log_->append(tr("Export failed: %1").arg(error));
        return;
    }
    QString suggested = QString::fromStdString(exported.name);
    suggested.replace(QStringLiteral(".gao"), QString());
    suggested += QStringLiteral(".jgao");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export GAO"),
        QFileInfo(bf_path_).absolutePath() + QLatin1Char('/') + suggested,
        tr("Jade GAO Files (*.jgao);;All Files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(reinterpret_cast<const char*>(exported.jgao.data()),
                      qint64(exported.jgao.size())) != qint64(exported.jgao.size())) {
        QMessageBox::warning(this, tr("Export GAO Failed"),
                             tr("Could not write %1").arg(path));
        log_->append(tr("Export failed: could not write %1").arg(path));
        return;
    }
    log_->append(tr("Exported GAO '%1' to %2")
                     .arg(QString::fromStdString(exported.name), path));
    QMessageBox::information(this, tr("Export GAO"),
                             tr("Exported '%1' successfully.")
                                 .arg(QString::fromStdString(exported.name)));
}

void PlacementTab::on_import_gao() {
    if (!zone_info_) {
        QMessageBox::information(this, tr("Import GAO"),
                                 tr("Load a zone first."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import GAO"), QFileInfo(bf_path_).absolutePath(),
        tr("Jade GAO Files (*.jgao);;All Files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import GAO Failed"),
                             tr("Could not open %1").arg(path));
        return;
    }
    const QByteArray bytes = file.readAll();
    jade::jgao::File parsed = jade::jgao::parse(
        reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()));
    if (!parsed.ok) {
        const QString error = QString::fromStdString(parsed.error);
        QMessageBox::warning(this, tr("Import GAO Failed"), error);
        log_->append(tr("Import failed: %1").arg(error));
        return;
    }
    imported_jgao_ =
        std::make_shared<jade::jgao::File>(std::move(parsed));
    imported_jgao_path_ = path;
    log_->append(tr("Imported %1: GAO=0x%2, GEO=%3B, MAT=%4B")
                     .arg(QFileInfo(path).fileName())
                     .arg(imported_jgao_->gao_key, 8, 16, QLatin1Char('0'))
                     .arg(imported_jgao_->geo_data.size())
                     .arg(imported_jgao_->mat_data.size()));
}

void PlacementTab::on_jgao_to_glb() {
    const QString input = QFileDialog::getOpenFileName(
        this, tr("Select JGAO to Convert"), QFileInfo(bf_path_).absolutePath(),
        tr("Jade GAO Files (*.jgao);;All Files (*)"));
    if (input.isEmpty()) return;
    QString suggested = input;
    const int dot = suggested.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0) suggested.resize(dot);
    suggested += QStringLiteral(".glb");
    const QString output = QFileDialog::getSaveFileName(
        this, tr("Save GLB As"), suggested,
        tr("glTF Binary (*.glb);;All Files (*)"));
    if (output.isEmpty()) return;
    QFile in(input);
    if (!in.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("JGAO → GLB Failed"),
                             tr("Could not open %1").arg(input));
        return;
    }
    const QByteArray bytes = in.readAll();
    const std::vector<jade::SubEntry>* context =
        zone_info_ ? &zone_info_->subs : nullptr;
    const jade::jgao::GlbResult converted = jade::jgao::jgao_to_glb(
        reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()),
        context);
    if (!converted.ok) {
        const QString error = QString::fromStdString(converted.error);
        QMessageBox::warning(this, tr("JGAO → GLB Failed"), error);
        log_->append(tr("JGAO→GLB failed: %1").arg(error));
        return;
    }
    QFile out(output);
    if (!out.open(QIODevice::WriteOnly)
        || out.write(reinterpret_cast<const char*>(converted.glb.data()),
                     qint64(converted.glb.size())) != qint64(converted.glb.size())) {
        QMessageBox::warning(this, tr("JGAO → GLB Failed"),
                             tr("Could not write %1").arg(output));
        return;
    }
    log_->append(tr("Exported GLB: %1 verts, %2 faces, %3 elements → %4")
                     .arg(converted.vertices).arg(converted.faces)
                     .arg(converted.elements).arg(QFileInfo(output).fileName()));
}

void PlacementTab::on_glb_to_jgao() {
    const QString glb_path = QFileDialog::getOpenFileName(
        this, tr("Select Edited GLB"), QFileInfo(bf_path_).absolutePath(),
        tr("glTF Binary (*.glb);;All Files (*)"));
    if (glb_path.isEmpty()) return;
    const QString template_path = QFileDialog::getOpenFileName(
        this, tr("Select Original JGAO (template)"), QFileInfo(glb_path).absolutePath(),
        tr("Jade GAO Files (*.jgao);;All Files (*)"));
    if (template_path.isEmpty()) return;
    QString suggested = glb_path;
    const int dot = suggested.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0) suggested.resize(dot);
    suggested += QStringLiteral("_edited.jgao");
    const QString output = QFileDialog::getSaveFileName(
        this, tr("Save New JGAO As"), suggested,
        tr("Jade GAO Files (*.jgao);;All Files (*)"));
    if (output.isEmpty()) return;
    QFile glb_file(glb_path), template_file(template_path);
    if (!glb_file.open(QIODevice::ReadOnly)
        || !template_file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("GLB → JGAO Failed"),
                             tr("Could not open the GLB or template JGAO."));
        return;
    }
    const QByteArray glb = glb_file.readAll();
    const QByteArray templ = template_file.readAll();
    const jade::jgao::ConvertResult converted = jade::jgao::glb_to_jgao(
        reinterpret_cast<const uint8_t*>(glb.constData()), size_t(glb.size()),
        reinterpret_cast<const uint8_t*>(templ.constData()), size_t(templ.size()));
    if (!converted.ok) {
        const QString error = QString::fromStdString(converted.error);
        QMessageBox::warning(this, tr("GLB → JGAO Failed"), error);
        log_->append(tr("GLB→JGAO failed: %1").arg(error));
        return;
    }
    QFile out(output);
    if (!out.open(QIODevice::WriteOnly)
        || out.write(reinterpret_cast<const char*>(converted.jgao.data()),
                     qint64(converted.jgao.size())) != qint64(converted.jgao.size())) {
        QMessageBox::warning(this, tr("GLB → JGAO Failed"),
                             tr("Could not write %1").arg(output));
        return;
    }
    log_->append(tr("Rebuilt JGAO: %1 verts, %2 faces → %3")
                     .arg(converted.vertices).arg(converted.faces)
                     .arg(QFileInfo(output).fileName()));
}

// ── Lifecycle ────────────────────────────────────────────────────────

void PlacementTab::closeEvent(QCloseEvent* event) {
    draft_refresh_timer_->stop();
    if (viewer_panel_ != nullptr) viewer_panel_->viewer()->cleanup_gl();
    QWidget::closeEvent(event);
}
