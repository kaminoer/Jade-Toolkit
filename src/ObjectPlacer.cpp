// ObjectPlacer.cpp — stage B2: the byte-builder leaf helpers of
// io_ops/object_placer.py (+ project/ops_transform's TRS math). Faithful
// line-for-line ports; FP paths compute in double and narrow to f32 at pack
// time exactly like struct.pack('<f').
#include "jade/ObjectPlacer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "jade/Collision.hpp"
#include "jade/Compression.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"

namespace jade {
namespace placer {

namespace {

constexpr double PI = 3.141592653589793238462643383279502884;
inline double radians(double deg) { return deg * (PI / 180.0); }

// CPython on Windows uses ucrtbase's sin/cos; mingw's libm differs in the last
// ULP for some inputs, and near-zero rotation residuals keep their (wrong) sign
// through the f32 narrowing. Bind the SAME functions CPython calls so the
// byte output matches. Falls back to std::sin/cos off-Windows.
#if defined(_WIN32)
extern "C" __declspec(dllimport) void* __stdcall LoadLibraryA(const char*);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void*, const char*);
using UnaryMathFn = double (*)(double);
inline UnaryMathFn ucrt_fn(const char* name, UnaryMathFn fallback) {
    static void* dll = LoadLibraryA("ucrtbase.dll");
    if (dll) {
        void* p = GetProcAddress(dll, name);
        if (p) return reinterpret_cast<UnaryMathFn>(p);
    }
    return fallback;
}
inline double py_sin(double x) {
    static UnaryMathFn f = ucrt_fn("sin", static_cast<UnaryMathFn>(std::sin));
    return f(x);
}
inline double py_cos(double x) {
    static UnaryMathFn f = ucrt_fn("cos", static_cast<UnaryMathFn>(std::cos));
    return f(x);
}
#else
inline double py_sin(double x) { return std::sin(x); }
inline double py_cos(double x) { return std::cos(x); }
#endif

inline void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}
inline void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
inline void put_f32(std::vector<uint8_t>& v, double d) {
    float f = static_cast<float>(d);
    uint32_t b;
    std::memcpy(&b, &f, 4);
    put_u32(v, b);
}
inline void set_f32(std::vector<uint8_t>& v, size_t o, double d) {
    float f = static_cast<float>(d);
    uint32_t b;
    std::memcpy(&b, &f, 4);
    v[o] = b & 0xFF; v[o + 1] = (b >> 8) & 0xFF;
    v[o + 2] = (b >> 16) & 0xFF; v[o + 3] = (b >> 24) & 0xFF;
}
inline void set_u32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = x & 0xFF; v[o + 1] = (x >> 8) & 0xFF;
    v[o + 2] = (x >> 16) & 0xFF; v[o + 3] = (x >> 24) & 0xFF;
}
inline uint32_t get_u32(const uint8_t* d, size_t o) {
    return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
           (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}
inline float get_f32(const uint8_t* d, size_t o) {
    uint32_t b = get_u32(d, o);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// _visual_bytes' per-channel op: int(round(c)) & 0xFF (round-half-even, WRAP).
inline uint8_t wrap_round(double c) {
    return static_cast<uint8_t>(static_cast<long long>(std::nearbyint(c)) & 0xFF);
}

const uint8_t DEFAULT_VISUAL_TAIL[26] = {
    0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};

}  // namespace

Vec3 vec3(const Vec3& v) { return v; }

Vec3 vec3_min(const Vec3& v, double minimum) {
    Vec3 out;
    for (int i = 0; i < 3; ++i)
        out[size_t(i)] = std::max(minimum, std::fabs(v[size_t(i)]));
    return out;
}

std::string clean_gao_name(const std::string& name) {
    std::string text = name.empty() ? "JadePlacedObject" : name;
    // .strip() + remove NULs.
    size_t b = text.find_first_not_of(" \t\r\n\f\v");
    size_t e = text.find_last_not_of(" \t\r\n\f\v");
    text = (b == std::string::npos) ? "" : text.substr(b, e - b + 1);
    std::string nonul;
    for (char c : text)
        if (c != '\0') nonul.push_back(c);
    std::string safe;
    for (char c : nonul) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == '@' || c == ' ')
            safe.push_back(c);
        else
            safe.push_back('_');
    }
    b = safe.find_first_not_of(" \t\r\n\f\v");
    e = safe.find_last_not_of(" \t\r\n\f\v");
    safe = (b == std::string::npos) ? "" : safe.substr(b, e - b + 1);
    if (safe.empty()) safe = "JadePlacedObject";
    // .lower().endswith(".gao")
    bool has_gao = false;
    if (safe.size() >= 4) {
        std::string tail = safe.substr(safe.size() - 4);
        for (char& c : tail) c = char(std::tolower(static_cast<unsigned char>(c)));
        has_gao = (tail == ".gao");
    }
    if (!has_gao) safe += ".gao";
    if (safe.size() > 120) safe = safe.substr(0, 120);
    return safe;
}

std::array<double, 4> euler_xyz_deg_to_quat(const Vec3& deg) {
    double rx = radians(deg[0]), ry = radians(deg[1]), rz = radians(deg[2]);
    double cx = py_cos(rx * 0.5), sx = py_sin(rx * 0.5);
    double cy = py_cos(ry * 0.5), sy = py_sin(ry * 0.5);
    double cz = py_cos(rz * 0.5), sz = py_sin(rz * 0.5);
    return {sx * cy * cz + cx * sy * sz,
            cx * sy * cz - sx * cy * sz,
            cx * cy * sz + sx * sy * cz,
            cx * cy * cz - sx * sy * sz};
}

Mat16 trs_to_matrix(const Vec3& position, const Vec3& euler_deg, const Vec3& scale) {
    std::array<double, 4> q = euler_xyz_deg_to_quat(euler_deg);
    double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    double xx = qx * qx, yy = qy * qy, zz = qz * qz;
    double xy = qx * qy, xz = qx * qz, yz = qy * qz;
    double wx = qw * qx, wy = qw * qy, wz = qw * qz;
    double r00 = 1 - 2 * (yy + zz), r01 = 2 * (xy - wz), r02 = 2 * (xz + wy);
    double r10 = 2 * (xy + wz), r11 = 1 - 2 * (xx + zz), r12 = 2 * (yz - wx);
    double r20 = 2 * (xz - wy), r21 = 2 * (yz + wx), r22 = 1 - 2 * (xx + yy);
    double sx = scale[0], sy = scale[1], sz = scale[2];
    return {r00 * sx, r10 * sx, r20 * sx, 0.0,
            r01 * sy, r11 * sy, r21 * sy, 0.0,
            r02 * sz, r12 * sz, r22 * sz, 0.0,
            position[0], position[1], position[2], 1.0};
}

Mat16 col_major_mul(const Mat16& a, const Mat16& b) {
    // Column-major: element (r,c) is m[c*4+r]; c = a·b.
    Mat16 out{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
                s += a[size_t(k * 4 + r)] * b[size_t(c * 4 + k)];
            out[size_t(c * 4 + r)] = s;
        }
    return out;
}

Mat16 matrix_values(const Vec3& position) {
    return {1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            position[0], position[1], position[2], 1.0};
}

std::vector<uint8_t> matrix68(const Vec3& position, const Vec3& rotation_euler_deg,
                              const Vec3& scale) {
    Mat16 vals = trs_to_matrix(position, rotation_euler_deg, scale);
    std::vector<uint8_t> out;
    out.reserve(68);
    for (double v : vals) put_f32(out, v);
    put_u32(out, MATRIX_TYPE);
    return out;
}

std::vector<uint8_t> visual_bytes(uint32_t geometry_key, uint32_t material_key,
                                  int nb_vertices, const VisualColors* colors) {
    std::vector<uint8_t> out;
    put_u32(out, geometry_key);
    put_u32(out, material_key);
    put_u32(out, DEFAULT_VISUAL_DRAW_MASK);
    put_u32(out, 0xFF100000u);
    if (nb_vertices <= 0) {
        out.insert(out.end(), DEFAULT_VISUAL_TAIL, DEFAULT_VISUAL_TAIL + 26);
        return out;
    }
    uint32_t n = static_cast<uint32_t>(nb_vertices);

    auto bgra_of = [](const std::array<double, 3>& c) -> std::array<uint8_t, 4> {
        return {wrap_round(c[2]), wrap_round(c[1]), wrap_round(c[0]), 0xFE};
    };
    const std::array<uint8_t, 4> WHITE = {0xFF, 0xFF, 0xFF, 0xFE};
    std::array<uint8_t, 4> flat{};
    if (colors && colors->flat) flat = bgra_of(colors->tint);
    auto color_at = [&](uint32_t i) -> std::array<uint8_t, 4> {
        if (!colors) return WHITE;
        if (colors->flat) return flat;
        if (colors->per_vertex)
            return i < colors->list.size() ? bgra_of(colors->list[i]) : WHITE;
        return WHITE;
    };

    out.push_back(0xFF);
    out.push_back(0xFF);
    put_u32(out, n);
    for (uint32_t i = 0; i < n; ++i) {
        std::array<uint8_t, 4> c = color_at(i);
        out.insert(out.end(), c.begin(), c.end());
    }
    put_u32(out, 1);
    put_u32(out, INVALID_KEY);
    put_u32(out, INVALID_KEY);
    put_u32(out, INVALID_KEY);
    const uint32_t STRIDE = 12;
    put_u32(out, 1);                              // marker
    put_u32(out, 4 + 4 + n * STRIDE);             // size (count + stride + payload)
    put_u32(out, n);
    put_u32(out, STRIDE);
    for (uint32_t i = 0; i < n; ++i) {
        std::array<uint8_t, 4> c = color_at(i);
        out.insert(out.end(), c.begin(), c.end());
        put_f32(out, -1.0);
        put_f32(out, -1.0);
    }
    return out;
}

std::vector<uint8_t> ode_box_bytes(const Vec3& dimensions) {
    Vec3 d = vec3_min(dimensions, 0.001);
    std::vector<uint8_t> out;
    out.push_back(7); out.push_back(2); out.push_back(0x05); out.push_back(0);
    put_f32(out, 0.0); put_f32(out, 0.0); put_f32(out, 0.0);
    std::vector<uint8_t> m = matrix68({0.0, 0.0, 0.0});
    out.insert(out.end(), m.begin(), m.end());
    put_f32(out, 0.2); put_f32(out, 0.2);
    put_f32(out, 10.0);
    put_f32(out, d[0]); put_f32(out, d[1]); put_f32(out, d[2]);
    put_u32(out, 0);
    const double tail[10] = {500.0, 0.0, 0.0, 0.0, 0.2, 0.00001, 0.0, 0.0, 0.0, 0.0};
    for (double v : tail) put_f32(out, v);
    return out;
}

std::vector<uint8_t> extended_stub_bytes() {
    // II + 5I + I + 4B + HH + BBH — all zeros except the lone 1.
    std::vector<uint8_t> out(44, 0);
    out[28] = 1;
    return out;
}

std::vector<uint8_t> runtime_editor_tail(const std::string& name) {
    std::string nm = clean_gao_name(name);
    std::vector<uint8_t> out;
    put_u32(out, static_cast<uint32_t>(nm.size() + 1));
    out.insert(out.end(), nm.begin(), nm.end());
    out.push_back(0);
    put_u32(out, 1);
    put_u32(out, 0);
    put_u32(out, INVALID_KEY);
    put_u32(out, INVALID_KEY);
    put_u32(out, INVALID_KEY);
    put_u32(out, 0);
    return out;
}

std::vector<uint8_t> obbox_bytes(const Vec3& lmin, const Vec3& lmax,
                                 const Vec3& rotation_euler_deg, const Vec3& scale) {
    double raw_min[3] = {lmin[0], lmin[1], lmin[2]};
    double raw_max[3] = {lmax[0], lmax[1], lmax[2]};
    double sx = scale[0], sy = scale[1], sz = scale[2];
    bool rot_ident = std::fabs(rotation_euler_deg[0]) < 1e-6 &&
                     std::fabs(rotation_euler_deg[1]) < 1e-6 &&
                     std::fabs(rotation_euler_deg[2]) < 1e-6;
    bool scale_ident = std::fabs(sx - 1.0) < 1e-6 && std::fabs(sy - 1.0) < 1e-6 &&
                       std::fabs(sz - 1.0) < 1e-6;
    double gmin[3], gmax[3];
    if (rot_ident && scale_ident) {
        for (int i = 0; i < 3; ++i) { gmin[i] = raw_min[i]; gmax[i] = raw_max[i]; }
    } else {
        double rx = radians(rotation_euler_deg[0]);
        double ry = radians(rotation_euler_deg[1]);
        double rz = radians(rotation_euler_deg[2]);
        double cx = py_cos(rx), cy = py_cos(ry), cz = py_cos(rz);
        double sx_ = py_sin(rx), sy_ = py_sin(ry), sz_ = py_sin(rz);
        // Rotation order matches _matrix68: R = Rz · Ry · Rx. (A variable-
        // shadowing bug here — scale-Z where sin(rz) belongs — was fixed in
        // BOTH implementations together on 2026-07-16.)
        double r00 = cy * cz, r01 = sx_ * sy_ * cz - cx * sz_, r02 = cx * sy_ * cz + sx_ * sz_;
        double r10 = cy * sz_, r11 = sx_ * sy_ * sz_ + cx * cz, r12 = cx * sy_ * sz_ - sx_ * cz;
        double r20 = -sy_, r21 = sx_ * cy, r22 = cx * cy;
        for (int i = 0; i < 3; ++i) {
            gmin[i] = std::numeric_limits<double>::infinity();
            gmax[i] = -std::numeric_limits<double>::infinity();
        }
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b)
                for (int c = 0; c < 2; ++c) {
                    double px = (a ? raw_max[0] : raw_min[0]) * sx;
                    double py = (b ? raw_max[1] : raw_min[1]) * sy;
                    double pz = (c ? raw_max[2] : raw_min[2]) * sz;
                    double wx = r00 * px + r01 * py + r02 * pz;
                    double wy = r10 * px + r11 * py + r12 * pz;
                    double wz = r20 * px + r21 * py + r22 * pz;
                    if (wx < gmin[0]) gmin[0] = wx;
                    if (wy < gmin[1]) gmin[1] = wy;
                    if (wz < gmin[2]) gmin[2] = wz;
                    if (wx > gmax[0]) gmax[0] = wx;
                    if (wy > gmax[1]) gmax[1] = wy;
                    if (wz > gmax[2]) gmax[2] = wz;
                }
    }
    std::vector<uint8_t> out;
    out.reserve(48);
    for (int i = 0; i < 3; ++i) put_f32(out, gmin[i]);
    for (int i = 0; i < 3; ++i) put_f32(out, gmax[i]);
    for (int i = 0; i < 3; ++i) put_f32(out, raw_min[i]);
    for (int i = 0; i < 3; ++i) put_f32(out, raw_max[i]);
    return out;
}

std::vector<uint8_t> replace_gao_name(const uint8_t* payload, size_t n,
                                      const std::string& new_name) {
    if (n < 16) return {};
    uint32_t old_size = get_u32(payload, 12);
    size_t old_end = 16 + static_cast<size_t>(old_size);
    if (old_size == 0 || old_end > n) return {};
    std::string nm = clean_gao_name(new_name);
    std::vector<uint8_t> out;
    out.reserve(n - old_size + nm.size() + 1);
    out.insert(out.end(), payload, payload + 12);
    uint32_t nsz = static_cast<uint32_t>(nm.size() + 1);
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(nsz >> (8 * i)));
    out.insert(out.end(), nm.begin(), nm.end());
    out.push_back(0);
    out.insert(out.end(), payload + old_end, payload + n);
    return out;
}

GaoOffsets gao_offsets(const uint8_t* d, size_t n) {
    GaoOffsets o;
    if (!d || n < 16) return o;
    o.identity = get_u32(d, 8);
    uint32_t name_size = get_u32(d, 12);
    o.status = 16 + static_cast<size_t>(name_size);
    o.global_matrix = o.status + 10;
    o.bv = o.global_matrix + 68;
    if (o.bv > n) return o;
    o.ok = true;
    return o;
}

bool patch_gao_position(std::vector<uint8_t>& data, const Vec3& position) {
    GaoOffsets o = gao_offsets(data.data(), data.size());
    if (!o.ok) return false;
    for (int i = 0; i < 3; ++i)
        set_f32(data, o.global_matrix + 12 * 4 + size_t(i) * 4, position[size_t(i)]);
    if ((o.identity & FLAG_OBBOX) && o.bv + 48 <= data.size()) {
        // GMin/GMax <- LMin/LMax (raw f32 copy).
        for (int i = 0; i < 24; ++i) data[o.bv + size_t(i)] = data[o.bv + 24 + size_t(i)];
    }
    return true;
}

Mat16 jade_matrix_to_math_4x4(const Mat16& m) {
    double Sx = std::fabs(m[3]) > 1e-9 ? m[3] : 1.0;
    double Sy = std::fabs(m[7]) > 1e-9 ? m[7] : 1.0;
    double Sz = std::fabs(m[11]) > 1e-9 ? m[11] : 1.0;
    return {m[0] * Sx, m[1] * Sx, m[2] * Sx, 0.0,
            m[4] * Sy, m[5] * Sy, m[6] * Sy, 0.0,
            m[8] * Sz, m[9] * Sz, m[10] * Sz, 0.0,
            m[12], m[13], m[14], 1.0};
}

uint32_t ltype_from_jade_matrix(const Mat16& m) {
    uint32_t lType = 0;
    if (std::fabs(m[12]) > 1e-5 || std::fabs(m[13]) > 1e-5 || std::fabs(m[14]) > 1e-5)
        lType |= 2;
    bool rot_ident = std::fabs(m[0] - 1) < 1e-5 && std::fabs(m[1]) < 1e-5 &&
                     std::fabs(m[2]) < 1e-5 && std::fabs(m[4]) < 1e-5 &&
                     std::fabs(m[5] - 1) < 1e-5 && std::fabs(m[6]) < 1e-5 &&
                     std::fabs(m[8]) < 1e-5 && std::fabs(m[9]) < 1e-5 &&
                     std::fabs(m[10] - 1) < 1e-5;
    if (!rot_ident) lType |= 4;
    if (std::fabs(m[3]) > 1e-5 || std::fabs(m[7]) > 1e-5 || std::fabs(m[11]) > 1e-5)
        lType |= 8;
    return lType;
}

bool patch_gao_world_matrix(std::vector<uint8_t>& data, const Mat16& world_matrix,
                            const Vec3* lb_min, const Vec3* lb_max) {
    GaoOffsets o = gao_offsets(data.data(), data.size());
    if (!o.ok) return false;
    for (int i = 0; i < 16; ++i)
        set_f32(data, o.global_matrix + size_t(i) * 4, world_matrix[size_t(i)]);
    set_u32(data, o.global_matrix + 64, ltype_from_jade_matrix(world_matrix));
    if (!(o.identity & FLAG_OBBOX) || o.bv + 48 > data.size()) return true;
    if (lb_min == nullptr || lb_max == nullptr) {
        for (int i = 0; i < 24; ++i) data[o.bv + size_t(i)] = data[o.bv + 24 + size_t(i)];
        return true;
    }
    // Orient the 8 local-AABB corners by [I·Sx | J·Sy | K·Sz], no translation.
    const Mat16& m = world_matrix;
    double Ix = m[0], Iy = m[1], Iz = m[2], Sx = m[3];
    double Jx = m[4], Jy = m[5], Jz = m[6], Sy = m[7];
    double Kx = m[8], Ky = m[9], Kz = m[10], Sz = m[11];
    double gmin[3], gmax[3];
    for (int i = 0; i < 3; ++i) {
        gmin[i] = std::numeric_limits<double>::infinity();
        gmax[i] = -std::numeric_limits<double>::infinity();
    }
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 2; ++c) {
                double ax = (a ? (*lb_max)[0] : (*lb_min)[0]) * Sx;
                double ay = (b ? (*lb_max)[1] : (*lb_min)[1]) * Sy;
                double az = (c ? (*lb_max)[2] : (*lb_min)[2]) * Sz;
                double wx = Ix * ax + Jx * ay + Kx * az;
                double wy = Iy * ax + Jy * ay + Ky * az;
                double wz = Iz * ax + Jz * ay + Kz * az;
                if (wx < gmin[0]) gmin[0] = wx;
                if (wy < gmin[1]) gmin[1] = wy;
                if (wz < gmin[2]) gmin[2] = wz;
                if (wx > gmax[0]) gmax[0] = wx;
                if (wy > gmax[1]) gmax[1] = wy;
                if (wz > gmax[2]) gmax[2] = wz;
            }
    for (int i = 0; i < 3; ++i) set_f32(data, o.bv + size_t(i) * 4, gmin[i]);
    for (int i = 0; i < 3; ++i) set_f32(data, o.bv + 12 + size_t(i) * 4, gmax[i]);
    for (int i = 0; i < 3; ++i) set_f32(data, o.bv + 24 + size_t(i) * 4, (*lb_min)[size_t(i)]);
    for (int i = 0; i < 3; ++i) set_f32(data, o.bv + 36 + size_t(i) * 4, (*lb_max)[size_t(i)]);
    return true;
}

std::array<double, 3> norm3(double x, double y, double z) {
    float fx = float(x), fy = float(y), fz = float(z);
    float len = std::sqrt(fx * fx + fy * fy + fz * fz);   // f32 pairwise, no FMA
    if (double(len) < 1e-8) return {0.0, 0.0, 1.0};
    return {double(fx / len), double(fy / len), double(fz / len)};
}

namespace {
constexpr uint32_t GEO_V7_STATIC_FLAGS = 0x00001008;
constexpr uint32_t GEO_V7_PLATFORM_FLAGS = 0x00000000;
constexpr uint32_t GEO_V7_TRIANGLE_FLAGS = 0x00000001;
}  // namespace

PlacerGeo build_cube_geometry(double sx, double sy, double sz, uint32_t material_key) {
    PlacerGeo g;
    g.material_key = material_key;
    double hx = sx * 0.5, hy = sy * 0.5, hz = sz * 0.5;
    g.vertices = {{-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz},
                  {-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {-hx, hy, -hz}};
    g.faces = {{0, 1, 2}, {2, 3, 0}, {0, 3, 7}, {7, 4, 0}, {1, 5, 6}, {6, 2, 1},
               {0, 4, 5}, {5, 1, 0}, {7, 3, 2}, {2, 6, 7}, {4, 7, 6}, {6, 5, 4}};
    g.face_flags = {0x02, 0x02, 0x04, 0x04, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x40, 0x40};
    for (const auto& v : g.vertices) g.normals.push_back(norm3(v[0], v[1], v[2]));
    g.face_uvs.assign(g.faces.size(), {0, 0, 0});
    g.elements = {{static_cast<uint32_t>(g.faces.size()), material_key}};
    return g;
}

PlacerGeo build_cylinder_geometry(double sx, double sy, double sz,
                                  uint32_t material_key, int segments) {
    PlacerGeo g;
    g.material_key = material_key;
    double rx = sx * 0.5, ry = sy * 0.5, hz = sz * 0.5;
    for (int i = 0; i < segments; ++i) {
        double a0 = 2.0 * PI * i / segments;
        double a1 = 2.0 * PI * (i + 1) / segments;
        std::array<double, 3> p0{rx * py_cos(a0), ry * py_sin(a0), -hz};
        std::array<double, 3> p1{rx * py_cos(a1), ry * py_sin(a1), -hz};
        std::array<double, 3> p2{rx * py_cos(a1), ry * py_sin(a1), hz};
        std::array<double, 3> p3{rx * py_cos(a0), ry * py_sin(a0), hz};
        std::array<double, 3> n0 = norm3(py_cos(a0) / std::max(rx, 1e-6),
                                         py_sin(a0) / std::max(ry, 1e-6), 0.0);
        std::array<double, 3> n1 = norm3(py_cos(a1) / std::max(rx, 1e-6),
                                         py_sin(a1) / std::max(ry, 1e-6), 0.0);
        uint32_t base = static_cast<uint32_t>(g.vertices.size());
        g.vertices.insert(g.vertices.end(), {p0, p1, p2, p3});
        g.normals.insert(g.normals.end(), {n0, n1, n1, n0});
        g.uvs.insert(g.uvs.end(), {{0, 0}, {1, 0}, {1, 1}, {0, 1}});
        g.faces.push_back({base, base + 1, base + 2});
        g.faces.push_back({base, base + 2, base + 3});
        g.face_uvs.push_back({base, base + 1, base + 2});
        g.face_uvs.push_back({base, base + 2, base + 3});

        const struct { double z; std::array<double, 3> normal; int order[3]; } caps[2] = {
            {hz, {0, 0, 1}, {0, 1, 2}}, {-hz, {0, 0, -1}, {0, 2, 1}}};
        for (const auto& cap : caps) {
            std::array<double, 3> center{0.0, 0.0, cap.z};
            std::array<double, 3> c0{rx * py_cos(a0), ry * py_sin(a0), cap.z};
            std::array<double, 3> c1{rx * py_cos(a1), ry * py_sin(a1), cap.z};
            uint32_t cb = static_cast<uint32_t>(g.vertices.size());
            g.vertices.insert(g.vertices.end(), {center, c0, c1});
            g.normals.insert(g.normals.end(), {cap.normal, cap.normal, cap.normal});
            g.uvs.insert(g.uvs.end(), {{0.5, 0.5}, {0, 0}, {1, 0}});
            std::array<uint32_t, 3> tri{cb + uint32_t(cap.order[0]),
                                        cb + uint32_t(cap.order[1]),
                                        cb + uint32_t(cap.order[2])};
            g.faces.push_back(tri);
            g.face_uvs.push_back(tri);
        }
    }
    g.elements = {{static_cast<uint32_t>(g.faces.size()), material_key}};
    return g;
}

PlacerGeo build_sphere_geometry(double sx, double sy, double sz,
                                uint32_t material_key, int slices, int stacks) {
    PlacerGeo g;
    g.material_key = material_key;
    double rx = sx * 0.5, ry = sy * 0.5, rz = sz * 0.5;
    for (int stack = 0; stack <= stacks; ++stack) {
        double phi = PI * stack / stacks;
        double z = rz * py_cos(phi);
        double ring = py_sin(phi);
        for (int slc = 0; slc <= slices; ++slc) {
            double theta = 2.0 * PI * slc / slices;
            double x = rx * ring * py_cos(theta);
            double y = ry * ring * py_sin(theta);
            g.vertices.push_back({x, y, z});
            g.normals.push_back(norm3(x / std::max(rx, 1e-6), y / std::max(ry, 1e-6),
                                      z / std::max(rz, 1e-6)));
            g.uvs.push_back({double(slc) / slices, double(stack) / stacks});
        }
    }
    uint32_t stride = static_cast<uint32_t>(slices + 1);
    for (int stack = 0; stack < stacks; ++stack) {
        for (int slc = 0; slc < slices; ++slc) {
            uint32_t a = uint32_t(stack) * stride + uint32_t(slc);
            uint32_t b = a + 1, c = a + stride, d = c + 1;
            if (stack != 0) {
                g.faces.push_back({a, c, b});
                g.face_uvs.push_back({a, c, b});
            }
            if (stack != stacks - 1) {
                g.faces.push_back({b, c, d});
                g.face_uvs.push_back({b, c, d});
            }
        }
    }
    g.elements = {{static_cast<uint32_t>(g.faces.size()), material_key}};
    return g;
}

std::vector<uint8_t> geometry_to_payload(const PlacerGeo& geo, uint32_t geo_version) {
    std::vector<uint32_t> flags = geo.face_flags;
    if (flags.size() != geo.faces.size())
        flags.assign(geo.faces.size(), GEO_V7_TRIANGLE_FLAGS);
    uint32_t static_flags = geo_version >= 8 ? 0x10 : GEO_V7_STATIC_FLAGS;

    std::vector<uint8_t> out;
    const uint32_t hdr[10] = {geo_version, static_flags, GEO_V7_PLATFORM_FLAGS,
                              static_cast<uint32_t>(geo.vertices.size()),
                              0, 0, 0, 1, 0, 0};
    for (uint32_t w : hdr) put_u32(out, w);
    for (const auto& v : geo.vertices) {
        put_f32(out, v[0]); put_f32(out, v[1]); put_f32(out, v[2]);
    }
    put_u32(out, static_cast<uint32_t>(geo.faces.size()));
    put_u32(out, geo.material_key);
    for (size_t f = 0; f < geo.faces.size(); ++f) {
        put_u16(out, uint16_t(geo.faces[f][0]));
        put_u16(out, uint16_t(geo.faces[f][1]));
        put_u16(out, uint16_t(geo.faces[f][2]));
        put_u16(out, 0); put_u16(out, 0); put_u16(out, 0);
        put_u32(out, flags[f]);
    }
    put_u32(out, 0);
    put_u32(out, 0);
    return out;
}

std::vector<uint8_t> geometry_to_payload_with_vb(const PlacerGeo& geo,
                                                 uint32_t geo_version,
                                                 uint32_t platform_flags) {
    std::vector<std::array<double, 2>> uvs = geo.uvs;
    if (uvs.empty()) uvs.assign(geo.vertices.size(), {0.0, 0.0});
    std::vector<std::array<uint32_t, 3>> face_uvs = geo.face_uvs;
    if (face_uvs.size() != geo.faces.size()) face_uvs = geo.faces;
    std::vector<uint32_t> flags = geo.face_flags;
    if (flags.size() != geo.faces.size())
        flags.assign(geo.faces.size(), GEO_V7_TRIANGLE_FLAGS);
    std::vector<PlacerElement> elements = geo.elements;
    if (elements.empty())
        elements = {{static_cast<uint32_t>(geo.faces.size()), geo.material_key}};

    // Optional dul_PointColors (jgao _pack_vertex_colors: nb ARGB dwords,
    // white-padded/truncated; alpha defaults opaque).
    std::vector<uint8_t> color_bytes;
    if (geo.has_colors && !geo.colors.empty()) {
        size_t nn = std::min(geo.colors.size(), geo.vertices.size());
        for (size_t i = 0; i < nn; ++i) {
            uint32_t r = uint32_t(wrap_round(geo.colors[i][0]));
            uint32_t g = uint32_t(wrap_round(geo.colors[i][1]));
            uint32_t b = uint32_t(wrap_round(geo.colors[i][2]));
            uint32_t a = uint32_t(wrap_round(geo.colors[i][3]));
            uint32_t argb = (a << 24) | (r << 16) | (g << 8) | b;
            for (int k = 0; k < 4; ++k) color_bytes.push_back(uint8_t(argb >> (8 * k)));
        }
        for (size_t i = nn; i < geo.vertices.size(); ++i)
            for (int k = 0; k < 4; ++k) color_bytes.push_back(0xFF);
    }
    uint32_t has_abs = color_bytes.empty() ? 0 : 1;

    std::vector<uint8_t> out;
    uint32_t nv = static_cast<uint32_t>(geo.vertices.size());
    const uint32_t hdr[10] = {geo_version, 0x00000004, platform_flags, nv, nv,
                              has_abs,
                              static_cast<uint32_t>(uvs.size()),
                              static_cast<uint32_t>(elements.size()), 0, 0};
    for (uint32_t w : hdr) put_u32(out, w);
    for (const auto& v : geo.vertices) {
        put_f32(out, v[0]); put_f32(out, v[1]); put_f32(out, v[2]);
    }
    out.insert(out.end(), color_bytes.begin(), color_bytes.end());
    for (const auto& uv : uvs) { put_f32(out, uv[0]); put_f32(out, uv[1]); }
    for (const PlacerElement& e : elements) {
        put_u32(out, e.n_tri);
        put_u32(out, e.mat_id);
    }
    for (size_t f = 0; f < geo.faces.size(); ++f) {
        put_u16(out, uint16_t(geo.faces[f][0]));
        put_u16(out, uint16_t(geo.faces[f][1]));
        put_u16(out, uint16_t(geo.faces[f][2]));
        put_u16(out, uint16_t(face_uvs[f][0]));
        put_u16(out, uint16_t(face_uvs[f][1]));
        put_u16(out, uint16_t(face_uvs[f][2]));
        put_u32(out, flags[f]);
    }

    // Trailing: element table + cooked stride-20 VB + u16 IB (byte-size field).
    std::vector<std::pair<uint32_t, uint32_t>> exp_keys;   // (vi, ui) in order
    std::vector<uint32_t> ib;
    {
        std::unordered_map<uint64_t, uint32_t> vert_map;
        for (size_t f = 0; f < geo.faces.size(); ++f) {
            for (int c = 0; c < 3; ++c) {
                uint32_t vi = geo.faces[f][size_t(c)];
                uint32_t ui = face_uvs[f][size_t(c)];
                uint64_t key = (uint64_t(vi) << 32) | ui;
                auto it = vert_map.find(key);
                uint32_t idx;
                if (it == vert_map.end()) {
                    idx = static_cast<uint32_t>(exp_keys.size());
                    vert_map.emplace(key, idx);
                    exp_keys.push_back({vi, ui});
                } else {
                    idx = it->second;
                }
                ib.push_back(idx);
            }
        }
    }
    uint32_t nb_expanded = static_cast<uint32_t>(exp_keys.size());
    uint32_t sec2_size = 8 + nb_expanded * 20;
    uint32_t nb_elems = static_cast<uint32_t>(elements.size());
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, nb_elems);
    put_u32(out, elements.empty() ? 0 : elements[0].mat_id);
    for (uint32_t i = 0; i < nb_elems; ++i) {
        put_u32(out, elements[i].n_tri);
        put_u32(out, i < nb_elems - 1 ? elements[i + 1].mat_id : sec2_size);
    }
    put_u32(out, 0);                 // VB_MAGIC
    put_u32(out, nb_expanded);
    put_u32(out, 20);                // VB_STRIDE
    for (const auto& key : exp_keys) {
        std::array<double, 3> p = key.first < geo.vertices.size()
                                      ? geo.vertices[key.first]
                                      : std::array<double, 3>{0.0, 0.0, 0.0};
        std::array<double, 2> uv = key.second < uvs.size()
                                       ? uvs[key.second]
                                       : std::array<double, 2>{0.0, 0.0};
        put_f32(out, p[0]); put_f32(out, p[1]); put_f32(out, p[2]);
        put_f32(out, uv[0]); put_f32(out, uv[1]);
    }
    put_u32(out, static_cast<uint32_t>(geo.faces.size()) * 6);
    for (uint32_t idx : ib) put_u16(out, uint16_t(idx & 0xFFFF));
    return out;
}

PlacerGeo build_primitive_geometry(const std::string& kind, const Vec3& size,
                                   uint32_t material_key,
                                   const std::array<int, 4>* color) {
    std::string k = kind.empty() ? "cube" : kind;
    for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
    Vec3 s = vec3_min(size, 0.001);
    PlacerGeo g;
    if (k == "sphere") g = build_sphere_geometry(s[0], s[1], s[2], material_key);
    else if (k == "cylinder") g = build_cylinder_geometry(s[0], s[1], s[2], material_key);
    else g = build_cube_geometry(s[0], s[1], s[2], material_key);
    if (color != nullptr) {
        g.has_colors = true;
        g.colors.assign(g.vertices.size(), *color);
    }
    return g;
}

std::vector<uint8_t> build_gao_payload(
    const std::string& name, uint32_t geometry_key, uint32_t material_key,
    const Vec3& position, const Vec3& lb_min, const Vec3& lb_max,
    bool collision_box, uint32_t colmap_key,
    const Vec3& rotation_euler_deg, const Vec3& scale,
    int nb_vertices, const VisualColors* colors) {
    // FLAG_BASE | FLAG_VISUAL | FLAG_INITIAL_POS | FLAG_OBBOX (+EXT+COLMAP).
    uint32_t identity = 0x00001000 | 0x00004000 | 0x00010000 | FLAG_OBBOX;
    if (collision_box) identity |= 0x00002000 | 0x00000100;

    static const uint8_t DEFAULT_STATUS[10] = {0x80, 0x00, 0x04, 0x00, 0x00,
                                               0x10, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t DEFAULT_STATUS_VISIBLE[10] = {0x00, 0x00, 0x04, 0x00, 0x00,
                                                       0x10, 0x00, 0x00, 0x00, 0x00};
    std::string nm = clean_gao_name(name);
    std::vector<uint8_t> out;
    put_u32(out, 10);                 // version
    put_u32(out, 8);                  // GAO_EDITOR_FLAGS
    put_u32(out, identity);
    put_u32(out, static_cast<uint32_t>(nm.size() + 1));
    out.insert(out.end(), nm.begin(), nm.end());
    out.push_back(0);
    const uint8_t* status = collision_box ? DEFAULT_STATUS : DEFAULT_STATUS_VISIBLE;
    out.insert(out.end(), status, status + 10);
    std::vector<uint8_t> m = matrix68(position, rotation_euler_deg, scale);
    out.insert(out.end(), m.begin(), m.end());
    std::vector<uint8_t> bv = obbox_bytes(lb_min, lb_max, rotation_euler_deg, scale);
    out.insert(out.end(), bv.begin(), bv.end());
    std::vector<uint8_t> vis = visual_bytes(geometry_key, material_key, nb_vertices, colors);
    out.insert(out.end(), vis.begin(), vis.end());
    if (collision_box) {
        std::vector<uint8_t> stub = extended_stub_bytes();
        out.insert(out.end(), stub.begin(), stub.end());
        put_u32(out, colmap_key);
    }
    std::vector<uint8_t> tail = runtime_editor_tail(nm);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

std::vector<uint8_t> clone_gao_payload(const uint8_t* src, size_t n,
                                       const std::string& new_name, const Vec3& position,
                                       const Vec3& rotation_euler_deg, const Vec3& scale) {
    std::vector<uint8_t> data = replace_gao_name(src, n, new_name);
    if (data.empty()) return {};
    bool rot_ident = std::fabs(rotation_euler_deg[0]) < 1e-6 &&
                     std::fabs(rotation_euler_deg[1]) < 1e-6 &&
                     std::fabs(rotation_euler_deg[2]) < 1e-6;
    bool scale_ident = std::fabs(scale[0] - 1.0) < 1e-6 &&
                       std::fabs(scale[1] - 1.0) < 1e-6 &&
                       std::fabs(scale[2] - 1.0) < 1e-6;
    if (rot_ident && scale_ident) {
        patch_gao_position(data, position);
        return data;
    }
    patch_gao_trs(data, position, rotation_euler_deg, scale);
    return data;
}

std::vector<uint8_t> make_sub_entry(uint32_t key, uint32_t gro_type,
                                    const std::vector<uint8_t>& payload) {
    return jade::make_sub_entry(key, gro_type, payload);
}

std::vector<uint8_t> make_sub_entry_ext(uint32_t key, const char ext[4],
                                        const std::vector<uint8_t>& payload) {
    return jade::make_sub_entry_ext(
        key, {uint8_t(ext[0]), uint8_t(ext[1]), uint8_t(ext[2]),
              uint8_t(ext[3])},
        payload);
}

uint32_t detect_geo_version_from_dec(const std::vector<SubEntry>& subs) {
    for (const SubEntry& s : subs) {
        if (s.gro_null || s.gro_type != 1) continue;
        if (s.data.size() >= 4) {
            uint32_t v = get_u32(s.data.data(), 0);
            if (v == 7 || v == 8) return v;
        }
    }
    return 7;
}

uint32_t detect_geo_platform_flags(const std::vector<SubEntry>& subs,
                                   uint32_t default_flags) {
    // Counter.most_common: highest count, insertion order on ties.
    std::vector<std::pair<uint32_t, uint32_t>> counts;   // (platform, count)
    for (const SubEntry& s : subs) {
        if (s.gro_null || s.gro_type != 1) continue;
        if (s.data.size() < 12) continue;
        uint32_t version = get_u32(s.data.data(), 0);
        uint32_t platform = get_u32(s.data.data(), 8);
        if ((version == 7 || version == 8) && platform != 0) {
            bool found = false;
            for (auto& kv : counts)
                if (kv.first == platform) { ++kv.second; found = true; break; }
            if (!found) counts.push_back({platform, 1});
        }
    }
    if (counts.empty()) return default_flags;
    std::pair<uint32_t, uint32_t> best = counts[0];
    for (const auto& kv : counts)
        if (kv.second > best.second) best = kv;
    return best.first;
}

// ── ColMap re-keying for intra-bin clones ──────────────────────────────────

namespace {

// collision._sub_full_content: [gro_type][payload] truncated to declared size.
std::vector<uint8_t> sub_full_content(const SubEntry& s) {
    std::vector<uint8_t> full;
    uint32_t gt = s.gro_null ? 0 : s.gro_type;
    for (int i = 0; i < 4; ++i) full.push_back(uint8_t(gt >> (8 * i)));
    full.insert(full.end(), s.data.begin(), s.data.end());
    uint32_t size = s.size ? s.size : static_cast<uint32_t>(full.size());
    if (size > 0 && size <= full.size()) full.resize(size);
    return full;
}

// collision.gao_colmap_key_offsets: every dword offset pointing at a ColMap.
// (Declared in ObjectPlacer.hpp — remove_object severs these refs.)
std::vector<std::pair<size_t, uint32_t>> gao_colmap_key_offsets_impl(
    const std::vector<uint8_t>& gao,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key) {
    return jade::gao_colmap_key_offsets(gao, by_key);
}

std::vector<uint8_t> clone_sub_entry_with_key(const SubEntry& src, uint32_t new_key) {
    uint32_t gro = src.gro_null ? 0 : src.gro_type;
    std::vector<uint8_t> payload = src.data;
    uint32_t size = src.size ? src.size : (4 + static_cast<uint32_t>(payload.size()));
    size_t keep = size >= 4 ? size - 4 : 0;
    if (payload.size() > keep) payload.resize(keep);
    return make_sub_entry(new_key, gro, payload);
}

// _clone_collision_refs: re-key any ColMap the source GAO references.
std::pair<std::vector<std::vector<uint8_t>>, std::vector<uint8_t>> clone_collision_refs(
    const std::vector<uint8_t>& gao_payload,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key,
    KeyAllocator& allocator) {
    if (gao_payload.size() < 16) return {{}, gao_payload};
    uint32_t identity = get_u32(gao_payload.data(), 8);
    if (!(identity & 0x00000100)) return {{}, gao_payload};   // FLAG_COLMAP
    std::vector<uint8_t> data = gao_payload;
    std::vector<std::vector<uint8_t>> additions;
    std::unordered_map<uint32_t, uint32_t> cloned;
    for (const auto& hit : gao_colmap_key_offsets_impl(data, by_key)) {
        uint32_t new_key;
        auto cit = cloned.find(hit.second);
        if (cit != cloned.end()) {
            new_key = cit->second;
        } else {
            auto sit = by_key.find(hit.second);
            if (sit == by_key.end()) continue;
            new_key = allocator.next();
            cloned[hit.second] = new_key;
            additions.push_back(clone_sub_entry_with_key(*sit->second, new_key));
        }
        set_u32(data, hit.first, new_key);
    }
    return {std::move(additions), std::move(data)};
}

// _bounds_from_points (numpy float32 min/max).
void bounds_from_points(const std::vector<std::array<double, 3>>& pts,
                        Vec3& mn, Vec3& mx) {
    if (pts.empty()) { mn = {0, 0, 0}; mx = {0, 0, 0}; return; }
    float fmn[3], fmx[3];
    for (int i = 0; i < 3; ++i) { fmn[i] = float(pts[0][size_t(i)]); fmx[i] = fmn[i]; }
    for (const auto& p : pts)
        for (int i = 0; i < 3; ++i) {
            float f = float(p[size_t(i)]);
            if (f < fmn[i]) fmn[i] = f;
            if (f > fmx[i]) fmx[i] = f;
        }
    for (int i = 0; i < 3; ++i) { mn[size_t(i)] = double(fmn[i]); mx[size_t(i)] = double(fmx[i]); }
}

uint32_t added_sub_entry_key(const std::vector<uint8_t>& rec, bool& ok) {
    static const uint8_t COOKIE[4] = {0x99, 0xC0, 0xFF, 0xEE};
    if (rec.size() < 16 || std::memcmp(rec.data() + 4, COOKIE, 4) != 0) {
        ok = false;
        return 0;
    }
    ok = true;
    return get_u32(rec.data(), 8);
}

struct PlacementError { std::string msg; };

// ── stage C: clone/primitive collision via host-COB extension ──────────────

// looks_like_cob_sub over a SubEntry (full-content view).
bool looks_like_cob_sub_pl(const SubEntry* s) {
    return jade::looks_like_cob_sub(s);
}

// _sample_host_bulk_element: (material, design, flag) of the largest element.
void sample_host_bulk_element(const SubEntry& cob, int32_t& mat, uint8_t& design,
                              uint8_t& flag) {
    mat = DEFAULT_COB_MATERIAL_ID;
    design = DEFAULT_COB_ELEMENT_DESIGN;
    flag = 0;
    const std::vector<uint8_t>& p = cob.data;
    if (p.size() < 6 || p[0] != COL_ZONE_TRIANGLES) return;
    if (p.size() < 2 + 4) return;
    uint32_t n_v = get_u32(p.data(), 2);
    size_t n_f_off = 2 + 4 + size_t(n_v) * 12;
    if (n_f_off + 4 > p.size()) return;
    uint32_t n_f = get_u32(p.data(), n_f_off);
    size_t n_e_off = n_f_off + 4 + size_t(n_f) * 12;
    if (n_e_off + 4 > p.size()) return;
    uint32_t n_e = get_u32(p.data(), n_e_off);
    if (n_e == 0) return;
    bool have = false;
    uint16_t best_tri = 0;
    size_t ofs = n_e_off + 4;
    for (uint32_t i = 0; i < n_e; ++i) {
        if (ofs + 8 > p.size()) break;
        uint16_t n_tri = uint16_t(p[ofs] | (p[ofs + 1] << 8));
        if (!have || n_tri > best_tri) {
            have = true;
            best_tri = n_tri;
            mat = int32_t(get_u32(p.data(), ofs + 4));
            design = p[ofs + 2];
            flag = p[ofs + 3];
        }
        ofs += 8 + size_t(n_tri) * 6;
    }
    if (!have) { mat = DEFAULT_COB_MATERIAL_ID; design = DEFAULT_COB_ELEMENT_DESIGN; flag = 0; }
}

// _find_host_gao_for_cob: the GAO whose ColMap chain reaches the COB.
const SubEntry* find_host_gao_for_cob(
    uint32_t target_cob_key, const std::vector<SubEntry>& subs,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key) {
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo info = parse_gao_full(s.data.data(), s.data.size());
        if (!info.ok || !(info.identity & 0x100)) continue;
        for (const auto& hit : gao_colmap_key_offsets_impl(s.data, by_key)) {
            auto cit = by_key.find(hit.second);
            if (cit == by_key.end()) continue;
            for (uint32_t ck : jade::colmap_cob_keys(cit->second, by_key))
                if (ck == target_cob_key) return &s;
        }
    }
    return nullptr;
}

// _pick_clone_host_cobs: every triangle COB the new object should extend.
std::vector<uint32_t> pick_clone_host_cobs(
    const Vec3& pos, const std::vector<SubEntry>& subs,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key) {
    struct Cand { uint32_t key; uint32_t size; Vec3 wmin, wmax; bool fake; };
    std::vector<Cand> candidates;
    bool have_fb = false;
    uint32_t largest_fb = 0;
    long long largest_size = -1;
    for (const SubEntry& s : subs) {
        if (!looks_like_cob_sub_pl(&s)) continue;
        if (s.data.empty() || s.data[0] != COL_ZONE_TRIANGLES) continue;
        // parse_cob is the strict discriminator (looks_like_cob_sub over-
        // matches gro-5 materials whose kind byte is also 5) — fixed in the
        // Python together with this mirror.
        if (!parse_cob(s.data.data(), s.data.size()).ok) continue;
        if (static_cast<long long>(s.size) > largest_size) {
            largest_size = static_cast<long long>(s.size);
            largest_fb = s.key;
            have_fb = true;
        }
        const SubEntry* host = find_host_gao_for_cob(s.key, subs, by_key);
        if (host == nullptr) continue;
        const std::vector<uint8_t>& hp = host->data;
        if (hp.size() < 16) continue;
        uint32_t name_size = get_u32(hp.data(), 12);
        uint32_t ident = get_u32(hp.data(), 8);
        if (16 + size_t(name_size) > hp.size()) continue;
        std::string host_name(reinterpret_cast<const char*>(hp.data() + 16),
                              name_size);
        while (!host_name.empty() && host_name.back() == '\0') host_name.pop_back();
        if (!(ident & 0x80000)) continue;                 // needs OBBox BV
        size_t bv_off = 16 + size_t(name_size) + 10 + 68;
        if (bv_off + 48 > hp.size()) continue;
        double lmin[3], lmax[3];
        for (int i = 0; i < 3; ++i) {
            lmin[i] = double(get_f32(hp.data(), bv_off + 24 + size_t(i) * 4));
            lmax[i] = double(get_f32(hp.data(), bv_off + 36 + size_t(i) * 4));
        }
        if (lmin[0] > lmax[0] || lmin[1] > lmax[1] || lmin[2] > lmax[2]) continue;
        GaoInfo hi = parse_gao_full(hp.data(), hp.size());
        if (!hi.ok || !hi.gmat_present || hi.gmat_raw.size() < 68) continue;
        double t[3];
        for (int i = 0; i < 3; ++i)
            t[i] = double(get_f32(hi.gmat_raw.data(), (12 + size_t(i)) * 4));
        Cand c;
        c.key = s.key;
        c.size = s.size;
        for (int i = 0; i < 3; ++i) {
            c.wmin[size_t(i)] = lmin[i] + t[i];
            c.wmax[size_t(i)] = lmax[i] + t[i];
        }
        c.fake = host_name.rfind("Fake_", 0) == 0;
        candidates.push_back(c);
    }
    auto contains = [&](const Vec3& mn, const Vec3& mx) {
        return mn[0] <= pos[0] && pos[0] <= mx[0] && mn[1] <= pos[1] &&
               pos[1] <= mx[1] && mn[2] <= pos[2] && pos[2] <= mx[2];
    };
    auto overlap = [](const Vec3& amn, const Vec3& amx, const Vec3& bmn, const Vec3& bmx) {
        return amn[0] <= bmx[0] && amx[0] >= bmn[0] && amn[1] <= bmx[1] &&
               amx[1] >= bmn[1] && amn[2] <= bmx[2] && amx[2] >= bmn[2];
    };
    std::vector<Cand> seeds;
    std::vector<std::pair<uint32_t, uint32_t>> selected;   // (key, size) insertion order
    auto sel_has = [&](uint32_t k) {
        for (const auto& kv : selected)
            if (kv.first == k) return true;
        return false;
    };
    for (const Cand& c : candidates)
        if (contains(c.wmin, c.wmax)) {
            seeds.push_back(c);
            if (!sel_has(c.key)) selected.push_back({c.key, c.size});
        }
    if (seeds.size() == 1) {
        const Cand& seed = seeds[0];
        const Cand* co = nullptr;
        long long co_size = -1;
        for (const Cand& c : candidates) {
            if (sel_has(c.key)) continue;
            if (c.fake) continue;
            if (!overlap(c.wmin, c.wmax, seed.wmin, seed.wmax)) continue;
            if (static_cast<long long>(c.size) > co_size) { co_size = static_cast<long long>(c.size); co = &c; }
        }
        if (co != nullptr) selected.push_back({co->key, co->size});
    }
    if (!selected.empty()) {
        std::stable_sort(selected.begin(), selected.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        std::vector<uint32_t> out;
        for (const auto& kv : selected) out.push_back(kv.first);
        return out;
    }
    if (have_fb) return {largest_fb};
    return {};
}

// numpy-style 4x4 inverse: partial-pivot Gauss-Jordan on the math matrix
// (column-major flat). ok=false on singular (Python falls back separately).
bool inv4x4(const Mat16& m, Mat16& out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[size_t(c * 4 + r)];
            a[r][c + 4] = (r == c) ? 1.0 : 0.0;
        }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (a[piv][col] == 0.0) return false;
        if (piv != col)
            for (int c = 0; c < 8; ++c) std::swap(a[piv][c], a[col][c]);
        double d = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            double f = a[r][col];
            if (f == 0.0) continue;
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[size_t(c * 4 + r)] = a[r][c + 4];
    return true;
}

// _apply_clone_collision_cob_extensions. Throws PlacementError like the Python.
std::vector<uint8_t> apply_clone_collision_cob_extensions(
    const std::vector<uint8_t>& dec_in, const std::vector<PlaceOp>& ops) {
    std::vector<PlaceOp> collision_ops;
    for (const PlaceOp& op : ops) {
        std::string k = op.kind;
        for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
        if ((k == "clone" || k == "cube" || k == "sphere" || k == "cylinder" ||
             k == "model") && op.collision)
            collision_ops.push_back(op);
    }
    if (collision_ops.empty()) return dec_in;

    std::vector<uint8_t> dec = dec_in;
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;

    // by_target: key -> ops (both in first-seen order, like the Python dict).
    std::vector<std::pair<uint32_t, std::vector<const PlaceOp*>>> by_target;
    auto target_ops = [&](uint32_t key) -> std::vector<const PlaceOp*>& {
        for (auto& kv : by_target)
            if (kv.first == key) return kv.second;
        by_target.push_back({key, {}});
        return by_target.back().second;
    };
    for (const PlaceOp& op : collision_ops) {
        std::vector<uint32_t> target_keys;
        if (op.has_room_cob_key) {
            target_keys = {op.room_cob_key};
        } else {
            target_keys = pick_clone_host_cobs(vec3(op.position), subs, by_key);
            if (target_keys.empty())
                throw PlacementError{
                    "clone-with-collision: no triangle-shape COB found in the "
                    "target bin to extend. Either disable collision or specify "
                    "a ``room_cob_key`` explicitly."};
        }
        for (uint32_t tk : target_keys) target_ops(tk).push_back(&op);
    }

    struct Target { size_t offset; uint32_t key; std::vector<const PlaceOp*> ops; };
    std::vector<Target> targets;
    for (const auto& kv : by_target) {
        auto it = by_key.find(kv.first);
        char kb[16];
        std::snprintf(kb, sizeof kb, "%08X", kv.first);
        if (it == by_key.end() || !looks_like_cob_sub_pl(it->second) ||
            !parse_cob(it->second->data.data(), it->second->data.size()).ok)
            throw PlacementError{std::string("clone-with-collision: target COB key 0x") +
                                 kb + " not found in bin or is not a COB sub-entry"};
        targets.push_back({it->second->offset, kv.first, kv.second});
    }
    std::sort(targets.begin(), targets.end(),
              [](const Target& a, const Target& b) { return a.offset > b.offset; });

    for (const Target& tgt : targets) {
        auto it = by_key.find(tgt.key);
        char kb[16];
        std::snprintf(kb, sizeof kb, "%08X", tgt.key);
        if (it == by_key.end() || !looks_like_cob_sub_pl(it->second))
            throw PlacementError{std::string("clone-with-collision: target COB key 0x") +
                                 kb + " not found in bin or is not a COB sub-entry"};
        const SubEntry* cob_sub = it->second;

        const SubEntry* host_gao = find_host_gao_for_cob(tgt.key, subs, by_key);
        bool have_host_m = false;
        Mat16 host_m_jade{};
        double htx = 0, hty = 0, htz = 0;
        if (host_gao != nullptr) {
            GaoInfo hi = parse_gao_full(host_gao->data.data(), host_gao->data.size());
            if (hi.ok && hi.gmat_present && hi.gmat_raw.size() >= 68) {
                have_host_m = true;
                for (int i = 0; i < 16; ++i)
                    host_m_jade[size_t(i)] =
                        double(get_f32(hi.gmat_raw.data(), size_t(i) * 4));
                htx = host_m_jade[12];
                hty = host_m_jade[13];
                htz = host_m_jade[14];
            }
        }

        int32_t bulk_mat;
        uint8_t bulk_design, bulk_flag;
        sample_host_bulk_element(*cob_sub, bulk_mat, bulk_design, bulk_flag);

        std::vector<uint8_t> new_payload = cob_sub->data;
        bool have_appended = false;
        Vec3 app_min{}, app_max{};
        auto track_box = [&](const std::array<std::array<double, 3>, 8>& corners) {
            Vec3 bmin = corners[0], bmax = corners[0];
            for (const auto& c : corners)
                for (int i = 0; i < 3; ++i) {
                    bmin[size_t(i)] = std::min(bmin[size_t(i)], c[size_t(i)]);
                    bmax[size_t(i)] = std::max(bmax[size_t(i)], c[size_t(i)]);
                }
            if (!have_appended) { app_min = bmin; app_max = bmax; have_appended = true; }
            else
                for (int i = 0; i < 3; ++i) {
                    app_min[size_t(i)] = std::min(app_min[size_t(i)], bmin[size_t(i)]);
                    app_max[size_t(i)] = std::max(app_max[size_t(i)], bmax[size_t(i)]);
                }
        };

        for (const PlaceOp* opp : tgt.ops) {
            const PlaceOp& op = *opp;
            std::string k = op.kind;
            for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
            Vec3 bmin, bmax;
            if (k == "clone") {
                auto sit = by_key.find(op.source_key);
                char sb[16];
                std::snprintf(sb, sizeof sb, "%08X", op.source_key);
                if (sit == by_key.end())
                    throw PlacementError{std::string("clone-with-collision: source GAO 0x") +
                                         sb + " not in bin (needed for OBBox bounds)"};
                ObboxBounds ob = obbox_local_bounds(sit->second->data.data(),
                                                    sit->second->data.size());
                if (ob.ok) { bmin = ob.mn; bmax = ob.mx; }
                else { bmin = {-0.5, -0.5, -0.5}; bmax = {0.5, 0.5, 0.5}; }
            } else {
                Vec3 s = vec3_min(op.size, 0.001);
                bmin = {-s[0] * 0.5, -s[1] * 0.5, -s[2] * 0.5};
                bmax = {s[0] * 0.5, s[1] * 0.5, s[2] * 0.5};
            }

            bool have_xform = false;
            Mat16 corner_xform{};
            if (op.has_world_matrix) {
                Mat16 clone_math = jade_matrix_to_math_4x4(op.world_matrix);
                Mat16 host_inv;
                bool inv_ok = false;
                if (have_host_m) {
                    Mat16 host_math = jade_matrix_to_math_4x4(host_m_jade);
                    inv_ok = inv4x4(host_math, host_inv);
                }
                if (!inv_ok) {
                    host_inv = matrix_values({-htx, -hty, -htz});
                }
                corner_xform = col_major_mul(host_inv, clone_math);
                have_xform = true;
            }

            bool is_ledge = op.collision_profile == "ledge_openbox";
            int32_t append_mat = is_ledge ? LEDGE_TOP_MATERIAL_ID : bulk_mat;
            uint8_t append_design = is_ledge ? CLIMBABLE_COB_ELEMENT_DESIGN : bulk_design;

            std::array<std::array<double, 3>, 8> local_corners = {{
                {bmin[0], bmin[1], bmin[2]}, {bmax[0], bmin[1], bmin[2]},
                {bmin[0], bmax[1], bmin[2]}, {bmax[0], bmax[1], bmin[2]},
                {bmin[0], bmin[1], bmax[2]}, {bmax[0], bmin[1], bmax[2]},
                {bmin[0], bmax[1], bmax[2]}, {bmax[0], bmax[1], bmax[2]}}};

            if (have_xform) {
                std::vector<uint8_t> np = extend_cob_triangle_box(
                    new_payload.data(), new_payload.size(), bmin, bmax,
                    {0, 0, 0}, &corner_xform, append_mat, append_design,
                    bulk_flag, is_ledge, is_ledge);
                if (np.empty())
                    throw PlacementError{"COB payload too small to extend"};
                new_payload = std::move(np);
                std::array<std::array<double, 3>, 8> hc;
                auto M = [&](int r, int c) { return corner_xform[size_t(c * 4 + r)]; };
                for (int i = 0; i < 8; ++i) {
                    double x = local_corners[size_t(i)][0], y = local_corners[size_t(i)][1],
                           z = local_corners[size_t(i)][2];
                    hc[size_t(i)] = {M(0, 0) * x + M(0, 1) * y + M(0, 2) * z + M(0, 3),
                                     M(1, 0) * x + M(1, 1) * y + M(1, 2) * z + M(1, 3),
                                     M(2, 0) * x + M(2, 1) * y + M(2, 2) * z + M(2, 3)};
                }
                track_box(hc);
            } else {
                Vec3 lp = {op.position[0] - htx, op.position[1] - hty,
                           op.position[2] - htz};
                std::vector<uint8_t> np = extend_cob_triangle_box(
                    new_payload.data(), new_payload.size(), bmin, bmax, lp,
                    nullptr, append_mat, append_design, bulk_flag, is_ledge,
                    is_ledge);
                if (np.empty())
                    throw PlacementError{"COB payload too small to extend"};
                new_payload = std::move(np);
                std::array<std::array<double, 3>, 8> hc;
                for (int i = 0; i < 8; ++i)
                    hc[size_t(i)] = {local_corners[size_t(i)][0] + lp[0],
                                     local_corners[size_t(i)][1] + lp[1],
                                     local_corners[size_t(i)][2] + lp[2]};
                track_box(hc);
            }
        }

        uint32_t gro = cob_sub->gro_null ? 0 : cob_sub->gro_type;
        std::vector<uint8_t> new_record = make_sub_entry(tgt.key, gro, new_payload);
        size_t record_start = cob_sub->offset - 4;
        size_t old_record_end = cob_sub->offset + 8 + cob_sub->size;
        uint32_t host_key = host_gao != nullptr ? host_gao->key : 0;
        {
            std::vector<uint8_t> nd;
            nd.reserve(dec.size() - (old_record_end - record_start) + new_record.size());
            nd.insert(nd.end(), dec.begin(), dec.begin() + long(record_start));
            nd.insert(nd.end(), new_record.begin(), new_record.end());
            nd.insert(nd.end(), dec.begin() + long(old_record_end), dec.end());
            dec = std::move(nd);
        }

        // Extend the host GAO's OBBox to cover the appended geometry.
        if (host_gao != nullptr && have_appended) {
            std::vector<SubEntry> refreshed = walk_sub_entries(dec);
            const SubEntry* hsub = nullptr;
            for (const SubEntry& s : refreshed)
                if (s.key == host_key) hsub = &s;   // last wins, like dict
            if (hsub != nullptr) {
                std::vector<uint8_t> ng = extend_obbox_to_include(
                    hsub->data.data(), hsub->data.size(), app_min, app_max);
                if (!ng.empty() && ng != hsub->data) {
                    std::vector<uint8_t> rec;
                    if (!hsub->ext.empty()) {
                        char ext4[4] = {0, 0, 0, 0};
                        for (size_t i = 0; i < 4 && i < hsub->ext.size(); ++i)
                            ext4[i] = hsub->ext[i];
                        rec = make_sub_entry_ext(hsub->key, ext4, ng);
                    } else {
                        rec = make_sub_entry(hsub->key, hsub->gro_type, ng);
                    }
                    size_t gs = hsub->offset - 4;
                    size_t ge = hsub->offset + 8 + hsub->size;
                    std::vector<uint8_t> nd;
                    nd.reserve(dec.size() - (ge - gs) + rec.size());
                    nd.insert(nd.end(), dec.begin(), dec.begin() + long(gs));
                    nd.insert(nd.end(), rec.begin(), rec.end());
                    nd.insert(nd.end(), dec.begin() + long(ge), dec.end());
                    dec = std::move(nd);
                }
            }
        }

        // NOTE: the Python keeps using the ORIGINAL subs/by_key for later
        // targets (descending offset order keeps their record positions valid;
        // the same-size GAO BV splice uses a FRESH parse above). Mirror that:
        // no re-walk here.
    }
    return dec;
}

// ── material auto-pick (_collect_materials / _first_material_key) ──────────
namespace {
// _NON_STATIC_HOST_PREFIXES gate: GFX/particle/bone/prefab hosts don't make a
// material "static-renderable" (their stub materials crash the placer's
// visual block in-game).
bool is_static_host_name(const std::string& name) {
    if (name.empty()) return false;
    static const char* kPrefixes[] = {"Fake_", "GFX_", "B_", "M_"};
    for (const char* p : kPrefixes)
        if (name.rfind(p, 0) == 0) return false;
    return true;
}

// _visual_material_ref_counts / _static_visual_material_ref_counts; the
// static variant skips toolkit-modded GAOs (0x7Axxxxxx) and non-static hosts.
std::unordered_map<uint32_t, int> visual_material_ref_counts(
    const std::vector<SubEntry>& subs, bool static_only) {
    std::unordered_map<uint32_t, int> counts;
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        if (static_only && (s.key & 0xFF000000u) == 0x7A000000u) continue;
        GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
        if (!gi.ok) continue;
        if (static_only && !is_static_host_name(gi.name)) continue;
        if (!gi.vis_read) continue;
        uint32_t material_key = gi.grm_key;
        if (material_key == INVALID_KEY) continue;
        ++counts[material_key];
    }
    return counts;
}
}  // namespace

std::vector<uint32_t> collect_material_keys(const std::vector<SubEntry>& subs) {
    std::unordered_map<uint32_t, int> visual_refs =
        visual_material_ref_counts(subs, /*static_only=*/false);
    std::unordered_map<uint32_t, int> static_refs =
        visual_material_ref_counts(subs, /*static_only=*/true);
    struct Cand { int refs; bool not_gro4; size_t order; uint32_t key; };
    std::vector<Cand> mats;
    for (size_t order = 0; order < subs.size(); ++order) {
        const SubEntry& s = subs[order];
        if (s.gro_type != 3 && s.gro_type != 4 && s.gro_type != 5) continue;
        if (s.gro_type == 5) {
            auto it = static_refs.find(s.key);
            if (it == static_refs.end() || it->second == 0) continue;
        }
        auto vit = visual_refs.find(s.key);
        mats.push_back({vit == visual_refs.end() ? 0 : vit->second,
                        s.gro_type != 4, order, s.key});
    }
    std::sort(mats.begin(), mats.end(), [](const Cand& a, const Cand& b) {
        if (a.refs != b.refs) return a.refs > b.refs;
        if (a.not_gro4 != b.not_gro4) return !a.not_gro4;
        return a.order < b.order;
    });
    std::vector<uint32_t> out;
    out.reserve(mats.size());
    for (const Cand& c : mats) out.push_back(c.key);
    return out;
}

uint32_t first_material_key(const std::vector<SubEntry>& subs) {
    std::vector<uint32_t> keys = collect_material_keys(subs);
    return keys.empty() ? 0 : keys.front();
}

namespace {
// _xbin_deep_copy_resources: deep-copy a cross-bin clone's transitive source
// resource chain under fresh keys and rewrite every old-key u32 (UNALIGNED
// scan — ColMap/extra-table refs sit at odd offsets) in both the cloned GAO
// body and every copied payload. The gro_type slot is overloaded (type code
// OR a key, e.g. a COB's game-material key) — rewrite it when it's a key.
std::pair<std::vector<std::vector<uint8_t>>, std::vector<uint8_t>>
xbin_deep_copy_resources(const std::vector<uint8_t>& gao_payload,
                         const std::vector<ChainEntry>& source_chain,
                         KeyAllocator& allocator) {
    if (source_chain.empty()) return {{}, gao_payload};

    std::unordered_map<uint32_t, uint32_t> rekey;
    std::vector<std::pair<const ChainEntry*, uint32_t>> raw_payloads;
    for (const ChainEntry& entry : source_chain) {
        if (rekey.count(entry.key)) continue;
        uint32_t new_key = allocator.next();
        rekey[entry.key] = new_key;
        raw_payloads.push_back({&entry, new_key});
    }

    auto rewrite = [&](const std::vector<uint8_t>& buf) {
        std::vector<uint8_t> data = buf;
        size_t n = data.size();
        size_t i = 0;
        while (i + 4 <= n) {
            uint32_t v = get_u32(data.data(), i);
            auto it = rekey.find(v);
            if (it != rekey.end()) {
                set_u32(data, i, it->second);
                i += 4;
            } else {
                ++i;
            }
        }
        return data;
    };

    std::vector<std::vector<uint8_t>> additions;
    for (const auto& pr : raw_payloads) {
        const ChainEntry& entry = *pr.first;
        std::vector<uint8_t> new_payload = rewrite(entry.data);
        if (!entry.has_gro_type) {
            char ext4[4] = {0, 0, 0, 0};
            for (size_t i = 0; i < 4 && i < entry.ext.size(); ++i) ext4[i] = entry.ext[i];
            additions.push_back(make_sub_entry_ext(pr.second, ext4, new_payload));
        } else {
            uint32_t gt = entry.gro_type;
            auto it = rekey.find(gt);
            if (it != rekey.end()) gt = it->second;  // it's actually a key
            additions.push_back(make_sub_entry(pr.second, gt, new_payload));
        }
    }
    return {std::move(additions), rewrite(gao_payload)};
}
}  // namespace

// _build_operation_sub_entries (clone incl. cross-bin, primitives, and
// imported static models).
BuiltOperation build_operation_sub_entries(
    const PlaceOp& op, const std::vector<SubEntry>& subs,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key,
    KeyAllocator& allocator, uint32_t geo_version, uint32_t geo_platform_flags) {
    std::string kind = op.kind.empty() ? "cube" : op.kind;
    for (char& c : kind) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string name = op.name.empty() ? ("JadePlaced_" + kind) : op.name;

    if (kind == "clone") {
        // Cross-bin clones inject source_gao_data (pre-read from another BF
        // entry via collect_xbin_source_chain); intra-bin clones resolve
        // source_key inside the target's sub_by_key.
        const std::vector<uint8_t>* source_data;
        bool cross_bin;
        if (op.has_source_gao_data) {
            source_data = &op.source_gao_data;
            cross_bin = true;
        } else {
            auto sit = sub_by_key.find(op.source_key);
            char kb[16];
            std::snprintf(kb, sizeof kb, "%08X", op.source_key);
            if (sit == sub_by_key.end() || sit->second->ext != ".gao")
                throw PlacementError{std::string("Clone source GAO 0x") + kb +
                                     " was not found"};
            source_data = &sit->second->data;
            cross_bin = false;
        }
        uint32_t new_key = allocator.next();
        std::vector<uint8_t> payload = clone_gao_payload(
            source_data->data(), source_data->size(), name, op.position,
            op.rotation_euler_deg, op.scale_xform);
        if (payload.empty())
            throw PlacementError{"Source GAO has an invalid name block"};
        if (op.has_world_matrix)
            patch_gao_world_matrix(payload, op.world_matrix);
        std::vector<std::vector<uint8_t>> collision_additions;
        if (cross_bin) {
            auto dc = xbin_deep_copy_resources(payload, op.source_resource_chain,
                                               allocator);
            collision_additions = std::move(dc.first);
            payload = std::move(dc.second);
        } else {
            auto cc = clone_collision_refs(payload, sub_by_key, allocator);
            collision_additions = std::move(cc.first);
            payload = std::move(cc.second);
        }

        BuiltOperation built;
        for (const std::vector<uint8_t>& addition : collision_additions) {
            bool okk = false;
            uint32_t k = added_sub_entry_key(addition, okk);
            if (!okk) throw PlacementError{"Generated sub-entry header was invalid"};
            built.additions.push_back({k, addition});
        }
        built.additions.push_back({new_key, make_sub_entry_ext(new_key, ".gao", payload)});
        built.object_keys = {new_key};
        for (const auto& kv : built.additions)
            if (kv.first != new_key) built.dependency_keys.push_back(kv.first);
        // Cross-bin: GAO through the regular append path, each dep placed
        // after a type-matched shipped donor (per_item_donor emit).
        built.per_item_donor = cross_bin;
        return built;
    }

    // Primitives/models. Python: int(op.get("material_key") or _first_material_key(subs)
    // or INVALID_KEY) — a missing OR zero/falsy key falls back to the auto-pick.
    uint32_t material_key;
    if (op.has_material_key && op.material_key != 0) {
        material_key = op.material_key;
    } else {
        uint32_t picked = first_material_key(subs);
        material_key = picked != 0 ? picked : INVALID_KEY;
    }
    Vec3 size = vec3_min(op.size, 0.001);
    PlacerGeo geo;
    try {
        if (kind == "model")
            geo = load_model_geometry(op.model_path, size, 0,
                                      op.import_vertex_colors);
        else
            geo = build_primitive_geometry(
                kind, size, 0,
                op.has_vertex_color ? &op.vertex_color : nullptr);
    } catch (const std::exception& error) {
        throw PlacementError{error.what()};
    }
    std::vector<uint8_t> geo_payload =
        geometry_to_payload_with_vb(geo, geo_version, geo_platform_flags);
    uint32_t geo_key = allocator.next();
    uint32_t gao_key = allocator.next();
    Vec3 lb_min, lb_max;
    bounds_from_points(geo.vertices, lb_min, lb_max);

    VisualColors vis_colors;
    const VisualColors* vis_ptr = nullptr;
    if (geo.has_colors && !geo.colors.empty()) {
        vis_colors.per_vertex = true;
        for (const auto& c : geo.colors)
            vis_colors.list.push_back({double(c[0]), double(c[1]), double(c[2])});
        vis_ptr = &vis_colors;
    }
    std::vector<uint8_t> gao_payload = build_gao_payload(
        name, geo_key, material_key, vec3(op.position), lb_min, lb_max,
        /*collision_box=*/false, INVALID_KEY, op.rotation_euler_deg, op.scale_xform,
        int(geo.vertices.size()), vis_ptr);
    if (op.has_world_matrix)
        patch_gao_world_matrix(gao_payload, op.world_matrix, &lb_min, &lb_max);

    BuiltOperation built;
    built.additions.push_back({geo_key, make_sub_entry(geo_key, 1, geo_payload)});
    built.additions.push_back({gao_key, make_sub_entry_ext(gao_key, ".gao", gao_payload)});
    built.object_keys = {gao_key};
    built.generated = true;
    built.dependency_keys = {geo_key};
    built.has_material_key = true;
    built.material_key = material_key;
    return built;
}

// ── the runtime-stream patcher ─────────────────────────────────────────────

size_t sub_entry_end(const SubEntry& s) { return s.offset + 8 + s.size; }

// _classify_sub_entry_record: ".xxx" ext (all-ASCII-alpha after the dot) or
// ("gt", gro_type). kind=="unknown" for records under 16 bytes.
struct RecordClass { std::string kind; uint32_t gt = 0; };
RecordClass classify_sub_entry_record(const std::vector<uint8_t>& record) {
    if (record.size() < 16) return {"unknown", 0};
    const uint8_t* t = record.data() + 12;
    bool ascii = t[0] < 128 && t[1] < 128 && t[2] < 128 && t[3] < 128;
    if (ascii && t[0] == '.' &&
        std::isalpha(t[1]) && std::isalpha(t[2]) && std::isalpha(t[3]))
        return {std::string(reinterpret_cast<const char*>(t), 4), 0};
    return {"gt", get_u32(record.data(), 12)};
}

// _select_per_item_donors: for each chain record (BFS order) pick a shipped
// sub-entry to insert it after — type-matched, increasing offsets, each donor
// used once; nullptr when nothing remains past the previous donor.
std::vector<const SubEntry*> select_per_item_donors(
    const std::vector<SubEntry>& subs,
    const std::vector<std::vector<uint8_t>>& chain_records, size_t min_offset) {
    std::vector<std::pair<std::string, std::vector<const SubEntry*>>> by_ext;
    std::vector<std::pair<uint32_t, std::vector<const SubEntry*>>> by_gt;
    std::vector<const SubEntry*> all_donors;
    for (const SubEntry& s : subs) {
        if (s.key == 0 || s.key == 0x0FF7C0DE || s.key == INVALID_KEY) continue;
        if (s.offset < min_offset) continue;
        all_donors.push_back(&s);
        if (!s.ext.empty()) {
            bool found = false;
            for (auto& kv : by_ext)
                if (kv.first == s.ext) { kv.second.push_back(&s); found = true; break; }
            if (!found) by_ext.push_back({s.ext, {&s}});
        } else {
            uint32_t gt = s.gro_null ? 0 : s.gro_type;
            bool found = false;
            for (auto& kv : by_gt)
                if (kv.first == gt) { kv.second.push_back(&s); found = true; break; }
            if (!found) by_gt.push_back({gt, {&s}});
        }
    }
    auto by_off = [](const SubEntry* a, const SubEntry* b) { return a->offset < b->offset; };
    for (auto& kv : by_ext) std::sort(kv.second.begin(), kv.second.end(), by_off);
    for (auto& kv : by_gt) std::sort(kv.second.begin(), kv.second.end(), by_off);
    std::sort(all_donors.begin(), all_donors.end(), by_off);

    std::unordered_set<size_t> used;
    std::vector<const SubEntry*> donors;
    long long prev_off = static_cast<long long>(min_offset) - 1;
    for (const std::vector<uint8_t>& record : chain_records) {
        RecordClass rc = classify_sub_entry_record(record);
        const SubEntry* donor = nullptr;
        const std::vector<const SubEntry*>* pool = nullptr;
        if (rc.kind != "unknown" && rc.kind != "gt") {
            for (const auto& kv : by_ext)
                if (kv.first == rc.kind) { pool = &kv.second; break; }
        } else if (rc.kind == "gt") {
            for (const auto& kv : by_gt)
                if (kv.first == rc.gt) { pool = &kv.second; break; }
        }
        if (pool != nullptr)
            for (const SubEntry* s : *pool)
                if (static_cast<long long>(s->offset) > prev_off && !used.count(s->offset)) {
                    donor = s;
                    break;
                }
        if (donor == nullptr)
            for (const SubEntry* s : all_donors)
                if (static_cast<long long>(s->offset) > prev_off && !used.count(s->offset)) {
                    donor = s;
                    break;
                }
        donors.push_back(donor);
        if (donor != nullptr) {
            used.insert(donor->offset);
            prev_off = static_cast<long long>(donor->offset);
        }
    }
    return donors;
}

std::vector<uint32_t> u32_values(const std::vector<uint8_t>& d) {
    std::vector<uint32_t> out;
    size_t usable = d.size() - (d.size() % 4);
    for (size_t o = 0; o < usable; o += 4) out.push_back(get_u32(d.data(), o));
    return out;
}

// (matches, ratio, format) for a world-object-list candidate.
struct ListScore { uint32_t matches = 0; double ratio = 0.0; bool pairs8 = false; };
ListScore score_world_object_list(const SubEntry& sub,
                                  const std::unordered_set<uint32_t>& gao_keys) {
    ListScore sc;
    std::vector<uint8_t> full = sub_full_content(sub);
    if (full.size() < 8) return sc;
    uint32_t key_matches = 0, key_nonzero = 0;
    for (uint32_t v : u32_values(full)) {
        if (v == 0 || v == INVALID_KEY) continue;
        ++key_nonzero;
        if (gao_keys.count(v)) ++key_matches;
    }
    double key_ratio = double(key_matches) / double(std::max<uint32_t>(key_nonzero, 1));
    uint32_t pair_matches = 0, pair_total = 0;
    if (full.size() % 8 == 0) {
        for (size_t o = 0; o < full.size(); o += 8) {
            uint32_t key = get_u32(full.data(), o);
            if (key == 0 || key == INVALID_KEY) continue;
            ++pair_total;
            if (gao_keys.count(key)) ++pair_matches;
        }
    }
    double pair_ratio = double(pair_matches) / double(std::max<uint32_t>(pair_total, 1));
    if (pair_matches > key_matches && pair_ratio >= 0.75) {
        sc.matches = pair_matches; sc.ratio = pair_ratio; sc.pairs8 = true;
        return sc;
    }
    sc.matches = key_matches; sc.ratio = key_ratio; sc.pairs8 = false;
    return sc;
}

// _find_world_object_list_sub. Returns (list, world, pairs8) or list==nullptr.
struct WorldList { const SubEntry* list = nullptr; const SubEntry* world = nullptr; bool pairs8 = false; };
WorldList find_world_object_list_sub(const std::vector<SubEntry>& subs) {
    std::unordered_set<uint32_t> gao_keys;
    for (const SubEntry& s : subs)
        if (s.ext == ".gao") gao_keys.insert(s.key);
    if (gao_keys.empty()) return {};
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;

    uint32_t best_m = 0; double best_r = 0.0;
    const SubEntry* best_cand = nullptr; const SubEntry* best_world = nullptr;
    bool best_pairs = false;

    for (const SubEntry& world_sub : subs) {
        if (world_sub.ext != ".wow" && world_sub.ext != ".wol") continue;
        const std::vector<uint8_t>& data = world_sub.data;
        if (data.size() < 4) continue;
        for (size_t off = 0; off + 4 <= data.size(); off += 4) {
            uint32_t ref = get_u32(data.data(), off);
            auto it = by_key.find(ref);
            if (it == by_key.end() || !it->second->ext.empty()) continue;
            ListScore sc = score_world_object_list(*it->second, gao_keys);
            if (sc.matches > best_m || (sc.matches == best_m && sc.ratio > best_r)) {
                best_m = sc.matches; best_r = sc.ratio;
                best_cand = it->second; best_world = &world_sub; best_pairs = sc.pairs8;
            }
        }
    }
    if (best_cand != nullptr && best_m >= 2 && best_r >= 0.6)
        return {best_cand, best_world, best_pairs};

    for (const SubEntry& cand : subs) {
        if (!cand.ext.empty()) continue;
        ListScore sc = score_world_object_list(cand, gao_keys);
        if (sc.matches > best_m || (sc.matches == best_m && sc.ratio > best_r)) {
            best_m = sc.matches; best_r = sc.ratio;
            best_cand = &cand; best_world = nullptr; best_pairs = sc.pairs8;
        }
    }
    if (best_cand != nullptr && best_m >= 2 && best_r >= 0.75)
        return {best_cand, best_world, best_pairs};
    return {};
}

std::vector<uint32_t> world_list_keys_ordered(const std::vector<uint8_t>& data, bool pairs8) {
    std::vector<uint32_t> keys;
    if (pairs8) {
        size_t usable = data.size() - (data.size() % 8);
        for (size_t o = 0; o < usable; o += 8) {
            uint32_t key = get_u32(data.data(), o);
            if (key != 0 && key != INVALID_KEY) keys.push_back(key);
        }
        return keys;
    }
    for (uint32_t v : u32_values(data))
        if (v != 0 && v != INVALID_KEY) keys.push_back(v);
    return keys;
}

std::vector<uint8_t> world_list_record_bytes(uint32_t key, bool pairs8) {
    std::vector<uint8_t> out;
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(key >> (8 * i)));
    if (pairs8) { out.push_back('.'); out.push_back('g'); out.push_back('a'); out.push_back('o'); }
    return out;
}

size_t world_list_insert_after_key(const SubEntry& object_list, bool pairs8,
                                   uint32_t donor_key) {
    std::vector<uint8_t> full = sub_full_content(object_list);
    size_t content_start = object_list.offset + 8;
    size_t step = pairs8 ? 8 : 4;
    size_t usable = full.size() - (full.size() % step);
    for (size_t o = 0; o < usable; o += step)
        if (get_u32(full.data(), o) == donor_key) return content_start + o + step;
    char kb[16];
    std::snprintf(kb, sizeof kb, "%08X", donor_key);
    throw PlacementError{std::string("Could not find donor GAO 0x") + kb +
                         " in the world object list"};
}

struct GeneratedDonor {
    uint32_t object_key = 0;
    uint32_t visual_geo_key = 0;
    bool has_material = false;
    uint32_t material_key = 0;
    size_t object_insert_at = 0;
    size_t dependency_insert_at = 0;
};

GeneratedDonor select_generated_donor(const std::vector<SubEntry>& subs,
                                      const std::vector<uint32_t>& ordered_object_keys,
                                      bool wanted_has, uint32_t wanted_material) {
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;
    bool want = wanted_has && wanted_material != INVALID_KEY;

    struct Cand { int rank; uint32_t geo_size; size_t order; GeneratedDonor d; };
    std::vector<Cand> cands;
    for (size_t order = 0; order < ordered_object_keys.size(); ++order) {
        uint32_t key = ordered_object_keys[order];
        auto git = by_key.find(key);
        if (git == by_key.end() || git->second->ext != ".gao") continue;
        GaoInfo gao = parse_gao_full(git->second->data.data(), git->second->data.size());
        if (!gao.ok || !gao.vis_read) continue;
        uint32_t geo_key = gao.gro_key;
        if (geo_key == 0 || geo_key == INVALID_KEY) continue;
        auto geo_it = by_key.find(geo_key);
        if (geo_it == by_key.end() || geo_it->second->gro_null ||
            geo_it->second->gro_type != 1)
            continue;
        size_t object_insert_at = sub_entry_end(*git->second);
        size_t dependency_insert_at = sub_entry_end(*geo_it->second);
        if (geo_it->second->offset <= object_insert_at) continue;
        uint32_t donor_material = gao.grm_key;
        int rank = (!want || donor_material == wanted_material) ? 0 : 1;
        GeneratedDonor d;
        d.object_key = key;
        d.visual_geo_key = geo_key;
        d.has_material = true;
        d.material_key = donor_material;
        d.object_insert_at = object_insert_at;
        d.dependency_insert_at = dependency_insert_at;
        cands.push_back({rank, geo_it->second->size, order, d});
    }
    if (cands.empty())
        throw PlacementError{"Could not find a direct-geometry world GAO to use as a "
                             "donor-local insertion point."};
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.geo_size != b.geo_size) return a.geo_size < b.geo_size;
        return a.order < b.order;
    });
    return cands[0].d;
}

}  // namespace

PlacementResult apply_placements_to_dec(const std::vector<uint8_t>& dec,
                                        const std::vector<PlaceOp>& ops,
                                        uint32_t geo_version,
                                        const std::vector<uint32_t>& extra_used_keys,
                                        uint32_t allocator_seed) {
    PlacementResult res;
    if (ops.empty()) {
        res.ok = true;
        res.patched = dec;
        return res;
    }
    try {
        for (const PlaceOp& op : ops) {
            std::string k = op.kind;
            for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (k == "replace")
                throw PlacementError{"apply_placements_to_dec does not handle replace ops"};
        }

        std::vector<SubEntry> pre_subs = walk_sub_entries(dec);
        if (geo_version == 0) geo_version = detect_geo_version_from_dec(pre_subs);
        uint32_t geo_platform_flags = detect_geo_platform_flags(pre_subs);

        // Room-COB extensions run FIRST (they reshape the dec), like the Python.
        std::vector<uint8_t> work_dec = apply_clone_collision_cob_extensions(dec, ops);
        std::vector<SubEntry> subs = walk_sub_entries(work_dec);
        std::unordered_map<uint32_t, const SubEntry*> sub_by_key;
        for (const SubEntry& s : subs) sub_by_key[s.key] = &s;

        std::unordered_set<uint32_t> used;
        for (const SubEntry& s : subs) used.insert(s.key);
        for (uint32_t k : extra_used_keys)
            if (k != INVALID_KEY) used.insert(k);
        KeyAllocator allocator(std::move(used), allocator_seed);

        std::vector<BuiltOperation> built_operations;
        for (const PlaceOp& op : ops) {
            BuiltOperation b = build_operation_sub_entries(
                op, subs, sub_by_key, allocator, geo_version, geo_platform_flags);
            built_operations.push_back(std::move(b));
        }
        for (const BuiltOperation& b : built_operations)
            for (const auto& kv : b.additions) res.additions.push_back(kv);

        // ── _patch_runtime_stream ──
        std::vector<uint32_t> all_object_keys;
        for (const BuiltOperation& b : built_operations)
            for (uint32_t k : b.object_keys) all_object_keys.push_back(k);
        if (all_object_keys.empty()) {
            res.patched = work_dec;
            for (const BuiltOperation& b : built_operations)
                for (const auto& kv : b.additions)
                    res.patched.insert(res.patched.end(), kv.second.begin(), kv.second.end());
            res.ok = true;
            return res;
        }

        WorldList wl = find_world_object_list_sub(subs);
        if (wl.list == nullptr)
            throw PlacementError{"Could not find the world's object list (.gol-style "
                                 "GAO key table). The new resources would exist in the "
                                 "BF but would not be instantiated by the game."};
        const SubEntry& object_list = *wl.list;
        std::vector<uint8_t> full = sub_full_content(object_list);
        std::vector<uint32_t> existing_ordered = world_list_keys_ordered(full, wl.pairs8);
        std::unordered_set<uint32_t> existing(existing_ordered.begin(), existing_ordered.end());

        std::map<size_t, std::vector<uint8_t>> insertions;
        uint32_t registered_count = 0;
        size_t list_bytes_added = 0;
        auto add_insert = [&](size_t pos, const std::vector<uint8_t>& data) {
            if (data.empty()) return;
            if (pos > work_dec.size())
                throw PlacementError{"Runtime stream insertion point was invalid"};
            auto& buf = insertions[pos];
            buf.insert(buf.end(), data.begin(), data.end());
        };
        auto add_world_key = [&](size_t pos, uint32_t key) {
            if (existing.count(key)) return;
            std::vector<uint8_t> data = world_list_record_bytes(key, wl.pairs8);
            add_insert(pos, data);
            existing.insert(key);
            ++registered_count;
            list_bytes_added += data.size();
        };

        size_t dependency_tail = work_dec.size();
        if (!subs.empty()) {
            const SubEntry& last = subs.back();
            if (last.key == 0x0FF7C0DE && sub_entry_end(last) == work_dec.size())
                dependency_tail = last.offset - 4;
        }
        size_t append_object_insert_at = 0;
        {
            bool any = false;
            for (const SubEntry& s : subs) {
                if (s.ext == ".gao" && existing.count(s.key)) {
                    any = true;
                    append_object_insert_at = std::max(append_object_insert_at, sub_entry_end(s));
                }
            }
            if (!any)
                throw PlacementError{"Could not find the existing world GAO stream block"};
        }
        size_t append_list_insert_at = object_list.offset + 8 +
                                       (object_list.size ? object_list.size
                                                         : static_cast<uint32_t>(full.size()));

        // Donor cache + groups (insertion order preserved like the Python dicts).
        std::vector<std::pair<uint32_t, GeneratedDonor>> donor_cache;   // by material key
        std::vector<std::pair<uint32_t, std::pair<GeneratedDonor, std::vector<const BuiltOperation*>>>> groups;
        std::vector<const BuiltOperation*> append_operations;
        std::vector<const BuiltOperation*> per_item_donor_operations;
        for (const BuiltOperation& b : built_operations) {
            if (b.per_item_donor) { per_item_donor_operations.push_back(&b); continue; }
            if (b.front_of_stream)   // dead branch in the Python too (never set)
                throw PlacementError{"front_of_stream placement layout is not ported"};
            if (!b.generated) { append_operations.push_back(&b); continue; }
            uint32_t donor_key = b.has_material_key ? b.material_key : INVALID_KEY;
            const GeneratedDonor* donor = nullptr;
            for (const auto& kv : donor_cache)
                if (kv.first == donor_key) { donor = &kv.second; break; }
            if (donor == nullptr) {
                GeneratedDonor d = select_generated_donor(
                    subs, existing_ordered, b.has_material_key, b.material_key);
                donor_cache.push_back({donor_key, d});
                donor = &donor_cache.back().second;
            }
            bool found = false;
            for (auto& g : groups)
                if (g.first == donor->object_key) {
                    g.second.second.push_back(&b);
                    found = true;
                    break;
                }
            if (!found)
                groups.push_back({donor->object_key, {*donor, {&b}}});
        }

        // Per-item donor emit (cross-bin clones): the cloned GAO record goes
        // through the regular append path; each dep gets its own type-matched
        // shipped donor in the sub-resource region, placed right after it.
        if (!per_item_donor_operations.empty()) {
            // min donor offset = just past the last shipped GOL GAO (the same
            // computation as append_object_insert_at — existing keys are
            // untouched at this point).
            size_t min_donor_off = append_object_insert_at;
            for (const BuiltOperation* b : per_item_donor_operations) {
                std::unordered_set<uint32_t> obj(b->object_keys.begin(),
                                                 b->object_keys.end());
                std::vector<uint8_t> gao_payload;
                std::vector<std::vector<uint8_t>> dep_records;
                for (const auto& kv : b->additions) {
                    if (obj.count(kv.first))
                        gao_payload.insert(gao_payload.end(), kv.second.begin(),
                                           kv.second.end());
                    else
                        dep_records.push_back(kv.second);
                }
                add_insert(append_object_insert_at, gao_payload);
                std::vector<const SubEntry*> donors =
                    select_per_item_donors(subs, dep_records, min_donor_off);
                for (size_t i = 0; i < dep_records.size(); ++i) {
                    if (donors[i] != nullptr)
                        add_insert(sub_entry_end(*donors[i]), dep_records[i]);
                    else
                        add_insert(dependency_tail, dep_records[i]);
                }
                for (uint32_t key : b->object_keys)
                    add_world_key(append_list_insert_at, key);
            }
        }

        for (const auto& g : groups) {
            const GeneratedDonor& donor = g.second.first;
            std::vector<uint8_t> object_payload, first_level, nested;
            for (const BuiltOperation* b : g.second.second) {
                std::unordered_set<uint32_t> obj(b->object_keys.begin(), b->object_keys.end());
                std::unordered_set<uint32_t> nst(b->nested_dependency_keys.begin(),
                                                 b->nested_dependency_keys.end());
                for (const auto& kv : b->additions) {
                    if (obj.count(kv.first))
                        object_payload.insert(object_payload.end(), kv.second.begin(), kv.second.end());
                    else if (nst.count(kv.first))
                        nested.insert(nested.end(), kv.second.begin(), kv.second.end());
                    else
                        first_level.insert(first_level.end(), kv.second.begin(), kv.second.end());
                }
            }
            add_insert(donor.object_insert_at, object_payload);
            std::vector<uint8_t> dep_payload = first_level;
            dep_payload.insert(dep_payload.end(), nested.begin(), nested.end());
            add_insert(donor.dependency_insert_at, dep_payload);
            size_t split_at = world_list_insert_after_key(object_list, wl.pairs8, donor.object_key);
            for (const BuiltOperation* b : g.second.second)
                for (uint32_t key : b->object_keys) add_world_key(split_at, key);
        }

        for (const BuiltOperation* b : append_operations) {
            std::unordered_set<uint32_t> obj(b->object_keys.begin(), b->object_keys.end());
            std::vector<uint8_t> object_payload, dependency_payload;
            for (const auto& kv : b->additions) {
                if (obj.count(kv.first))
                    object_payload.insert(object_payload.end(), kv.second.begin(), kv.second.end());
                else
                    dependency_payload.insert(dependency_payload.end(), kv.second.begin(), kv.second.end());
            }
            add_insert(append_object_insert_at, object_payload);
            add_insert(dependency_tail, dependency_payload);
            for (uint32_t key : b->object_keys) add_world_key(append_list_insert_at, key);
        }

        std::vector<uint8_t> patched = work_dec;
        for (auto it = insertions.rbegin(); it != insertions.rend(); ++it)
            patched.insert(patched.begin() + static_cast<long>(it->first),
                           it->second.begin(), it->second.end());
        if (list_bytes_added) {
            uint32_t size = object_list.size ? object_list.size
                                             : static_cast<uint32_t>(full.size());
            set_u32(patched, object_list.offset - 4,
                    size + static_cast<uint32_t>(list_bytes_added));
            if (wl.world != nullptr && registered_count > 0) {
                size_t count_off = wl.world->offset + 12 + 4;
                if (count_off + 4 <= patched.size()) {
                    uint32_t current = get_u32(patched.data(), count_off);
                    if (current)
                        set_u32(patched, count_off, current + registered_count);
                }
            }
        }
        res.patched = std::move(patched);
        res.has_world_list_key = true;
        res.world_list_key = object_list.key;
        res.objects_registered = registered_count;
        res.ok = true;
        return res;
    } catch (const PlacementError& e) {
        res.error = e.msg;
        return res;
    }
}

bool patch_gao_trs(std::vector<uint8_t>& data, const Vec3& position,
                   const Vec3& rotation_euler_deg, const Vec3& scale) {
    GaoOffsets o = gao_offsets(data.data(), data.size());
    if (!o.ok) return false;
    Mat16 src{};
    for (int i = 0; i < 16; ++i)
        src[size_t(i)] = double(get_f32(data.data(), o.global_matrix + size_t(i) * 4));
    src[12] = 0.0;
    src[13] = 0.0;
    src[14] = 0.0;
    Mat16 user = trs_to_matrix(position, rotation_euler_deg, scale);
    Mat16 composed = col_major_mul(user, src);
    for (int i = 0; i < 16; ++i)
        set_f32(data, o.global_matrix + size_t(i) * 4, composed[size_t(i)]);
    if ((o.identity & FLAG_OBBOX) && o.bv + 48 <= data.size()) {
        for (int i = 0; i < 24; ++i) data[o.bv + size_t(i)] = data[o.bv + 24 + size_t(i)];
    }
    return true;
}

// ── apply_placement_plan (BF-level writer) ──────────────────────────────────
namespace {
namespace fs = std::filesystem;

// Public path strings are UTF-8. Build absolute paths through u8path so MinGW
// does not reinterpret them through the active narrow-codepage locale.
std::string abs_path_narrow(const std::string& p) {
    std::error_code error;
    fs::path absolute = fs::absolute(fs::u8path(p), error);
    return error ? p : absolute.lexically_normal().u8string();
}

std::string normcase(std::string s) {
#ifdef _WIN32
    for (char& c : s) {
        if (c == '/') c = '\\';
        else c = char(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return s;
}

bool copy_file_narrow(const std::string& src, const std::string& dst) {
    std::ifstream in(fs::u8path(src), std::ios::binary);
    if (!in) return false;
    std::ofstream out(fs::u8path(dst), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return bool(out);
}

// Python f-string "{:,}" thousands separator (used in the strict-fit error).
std::string commas(unsigned long long v) {
    std::string digits = std::to_string(v);
    std::string out;
    int c = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (c && c % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++c;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// _write_compressed_entry: in-place (zero-padded slack) or, with
// strict_fit=false, appended sector-aligned; FAT pos + FileExt length
// repointed either way. NOT BigFile::write_entry — no size.grs queueing.
bool write_compressed_entry(std::fstream& f, const BigFile& bf, const BFFile& fi,
                            const std::vector<uint8_t>& compressed, bool strict_fit) {
    const FatDesc& fat0 = bf.fat_list[0];
    uint64_t fext_base = uint64_t(fat0.pos_fat) + uint64_t(bf.size_of_fat) * FILE_ENTRY_SZ;

    uint32_t new_pos;
    bool wrote_in_place;
    if (compressed.size() <= fi.length) {
        f.seekp(fi.pos);
        f.write(reinterpret_cast<const char*>(compressed.data()),
                std::streamsize(compressed.size()));
        size_t remaining = fi.length - compressed.size();
        if (remaining > 0) {
            std::vector<char> zeros(remaining, 0);
            f.write(zeros.data(), std::streamsize(remaining));
        }
        new_pos = fi.pos;
        wrote_in_place = true;
    } else {
        if (strict_fit)
            throw PlacementError{"Compressed entry grew and strict fit is enabled"};
        f.seekp(0, std::ios::end);
        uint64_t cur_end = uint64_t(f.tellp());
        uint64_t aligned = (cur_end + SECTOR_ALIGN - 1) & ~uint64_t(SECTOR_ALIGN - 1);
        if (aligned > cur_end) {
            std::vector<char> pad(size_t(aligned - cur_end), 0);
            f.write(pad.data(), std::streamsize(pad.size()));
        }
        f.write(reinterpret_cast<const char*>(compressed.data()),
                std::streamsize(compressed.size()));
        new_pos = uint32_t(aligned);
        wrote_in_place = false;
    }

    uint8_t le[4];
    auto put32 = [&](uint64_t off, uint32_t v) {
        le[0] = uint8_t(v); le[1] = uint8_t(v >> 8);
        le[2] = uint8_t(v >> 16); le[3] = uint8_t(v >> 24);
        f.seekp(std::streamoff(off));
        f.write(reinterpret_cast<const char*>(le), 4);
    };
    put32(uint64_t(fat0.pos_fat) + uint64_t(fi.index - fat0.first_index) * FILE_ENTRY_SZ,
          new_pos);
    put32(fext_base + uint64_t(fi.index - fat0.first_index) * bf.ext_size,
          uint32_t(compressed.size()));
    return wrote_in_place;
}

// _update_size_grs: name-keyed lookup ("size.grs"), walk (key, declared_len)
// pairs, patch the matching row in place.
bool update_size_grs(std::fstream& f, const BigFile& bf, uint32_t target_key,
                     uint32_t size_value) {
    const BFFile* size_fi = nullptr;
    for (const auto& kv : bf.files)
        if (kv.second.name == "size.grs") { size_fi = &kv.second; break; }
    if (size_fi == nullptr || size_fi->length < 12) return false;
    std::vector<uint8_t> raw(size_fi->length);
    f.seekg(size_fi->pos);
    f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
    for (size_t off = 4; off + 8 <= raw.size(); off += 8) {
        if (get_u32(raw.data(), off) == target_key) {
            uint8_t le[4] = {uint8_t(size_value), uint8_t(size_value >> 8),
                             uint8_t(size_value >> 16), uint8_t(size_value >> 24)};
            f.seekp(std::streamoff(uint64_t(size_fi->pos) + off + 4));
            f.write(reinterpret_cast<const char*>(le), 4);
            return true;
        }
    }
    return false;
}
}  // namespace

// ── ReplaceMesh flow (_apply_geo_replacements, GAO-to-GAO branch) ───────────

std::vector<uint8_t> scale_raw_geo(const std::vector<uint8_t>& raw, const Vec3& scale) {
    double sx = scale[0], sy = scale[1], sz = scale[2];
    std::vector<uint8_t> data = raw;
    if (data.size() < 40) return data;
    uint32_t nb_pts = get_u32(data.data(), 12);
    uint32_t abs_count = get_u32(data.data(), 16);
    uint32_t has_abs = get_u32(data.data(), 20);
    uint32_t nb_uvs = get_u32(data.data(), 24);
    uint32_t nb_elems = get_u32(data.data(), 28);
    uint32_t mrm_marker = get_u32(data.data(), 32);
    uint32_t skin_ok3 = get_u32(data.data(), 36);

    // Effective header size (skip skin data if present).
    size_t eff_hdr = 40;
    if (mrm_marker == 0xC0DE2002u) {
        size_t off = 36;
        if (off + 4 <= data.size()) {
            uint32_t skin_flags_numlist = get_u32(data.data(), off);
            uint32_t num_lists = (skin_flags_numlist >> 16) & 0xFFFF;
            off += 4;
            for (uint32_t i = 0; i < num_lists; ++i) {
                if (off + 4 > data.size()) break;
                uint32_t num_verts = uint16_t(get_u32(data.data(), off) >> 16);
                off += 4 + 68 + size_t(num_verts) * 4;
            }
            eff_hdr = off + 4;   // +4 for Skin_OK3
        }
    }

    // Scale parser vertices (Python multiplies the unpacked double then packs f32).
    size_t vert_start = eff_hdr;
    for (uint32_t i = 0; i < nb_pts; ++i) {
        size_t off = vert_start + size_t(i) * 12;
        if (off + 12 > data.size()) break;
        set_f32(data, off, double(get_f32(data.data(), off)) * sx);
        set_f32(data, off + 4, double(get_f32(data.data(), off + 4)) * sy);
        set_f32(data, off + 8, double(get_f32(data.data(), off + 8)) * sz);
    }

    // Locate the trailing block after the parser triangles.
    size_t norm_end = vert_start + size_t(nb_pts) * 12;
    if (skin_ok3) norm_end += size_t(nb_pts) * 12;
    size_t uv_off = norm_end;
    if (has_abs) uv_off += size_t(std::min(abs_count, nb_pts)) * 4;
    size_t elem_off = uv_off + size_t(nb_uvs) * 8;
    uint64_t total_tris = 0;
    for (uint32_t ei = 0; ei < nb_elems; ++ei) {
        size_t eoff = elem_off + size_t(ei) * 8;
        if (eoff + 4 <= data.size()) total_tris += get_u32(data.data(), eoff);
    }
    size_t trailing_start = elem_off + size_t(nb_elems) * 8 + size_t(total_tris) * 16;

    // Scan for a VB header (count u32, stride u32) and scale VB positions.
    if (trailing_start < data.size() && data.size() - trailing_start > 16) {
        size_t remaining = data.size() - trailing_start;
        static const uint32_t kStrides[] = {52, 32, 36, 40, 44, 48};
        bool found = false;
        for (uint32_t stride_val : kStrides) {
            if (found) break;
            size_t scan_max = std::min<size_t>(remaining, 200);
            if (scan_max < 4) break;
            for (size_t i = 0; i + 4 <= scan_max - 4 + 4; i += 4) {
                size_t scan_off = trailing_start + i;
                if (scan_off + 4 > data.size()) break;
                if (get_u32(data.data(), scan_off) != stride_val) continue;
                if (scan_off < trailing_start + 4) continue;
                uint32_t count = get_u32(data.data(), scan_off - 4);
                if (count < 1 || count > 100000) continue;
                size_t vb_start = scan_off + 4;
                for (uint32_t vi = 0; vi < count; ++vi) {
                    size_t voff = vb_start + size_t(vi) * stride_val;
                    if (voff + 12 > data.size()) break;
                    set_f32(data, voff, double(get_f32(data.data(), voff)) * sx);
                    set_f32(data, voff + 4, double(get_f32(data.data(), voff + 4)) * sy);
                    set_f32(data, voff + 8, double(get_f32(data.data(), voff + 8)) * sz);
                }
                found = true;
                break;
            }
        }
    }
    return data;
}

namespace {

std::string hex8u(uint32_t k) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", k);
    return std::string(b);
}

// Decompress a source entry and index its subs (owned buffer + views).
struct SourceEntry {
    bool ok = false;
    std::vector<uint8_t> dec;
    std::vector<SubEntry> subs;
    std::unordered_map<uint32_t, const SubEntry*> by_key;
};
SourceEntry load_source_entry(BigFile& bf, uint32_t entry_index) {
    SourceEntry se;
    if (bf.files.find(entry_index) == bf.files.end()) return se;
    LzoResult dr = decompress_lzo(bf.read_data(entry_index));
    if (!dr.ok) return se;
    se.dec = std::move(dr.data);
    se.subs = walk_sub_entries(se.dec);
    for (const SubEntry& s : se.subs) se.by_key[s.key] = &s;   // last wins
    se.ok = true;
    return se;
}

// AddObject's cross-bin project paths use parse_sub_entries(), not the robust
// declared-size walker. Preserve that product behavior here; the ordinary
// placement/model helpers above continue to use load_source_entry().
SourceEntry load_xbin_source_entry(BigFile& bf, uint32_t entry_index) {
    SourceEntry se;
    if (bf.files.find(entry_index) == bf.files.end()) return se;
    LzoResult dr = decompress_lzo(bf.read_data(entry_index));
    if (!dr.ok) return se;
    se.dec = std::move(dr.data);
    se.subs = parse_sub_entries(se.dec);
    for (const SubEntry& s : se.subs) se.by_key[s.key] = &s;  // last wins
    se.ok = true;
    return se;
}

// load_source_geo_raw: the raw GEO payload a source GAO's visual references.
std::vector<uint8_t> load_source_geo_raw(BigFile& bf, uint32_t entry_index,
                                         uint32_t gao_key) {
    if (bf.files.find(entry_index) == bf.files.end())
        throw PlacementError{"Source entry " + std::to_string(entry_index) +
                             " not found"};
    SourceEntry se = load_source_entry(bf, entry_index);
    if (!se.ok)
        throw PlacementError{"Source entry " + std::to_string(entry_index) +
                             " did not decompress"};
    auto git = se.by_key.find(gao_key);
    if (git == se.by_key.end() || git->second->ext != ".gao")
        throw PlacementError{"Source GAO 0x" + hex8u(gao_key) + " not found in entry " +
                             std::to_string(entry_index)};
    GaoInfo gi = parse_gao_full(git->second->data.data(), git->second->data.size());
    if (!gi.ok || !gi.vis_read)
        throw PlacementError{"Source GAO 0x" + hex8u(gao_key) +
                             " has no visual section"};
    auto geo_it = se.by_key.find(gi.gro_key);
    if (geo_it == se.by_key.end() || geo_it->second->gro_null ||
        geo_it->second->gro_type != 1)
        throw PlacementError{"Source GAO 0x" + hex8u(gao_key) + " references GEO 0x" +
                             hex8u(gi.gro_key) +
                             " which was not found in the source entry"};
    return geo_it->second->data;
}

// _resolve_source_grm_key: 0xFFFFFFFF+false when unresolvable (Python None).
bool resolve_source_grm_key(BigFile& bf, uint32_t entry_index, uint32_t gao_key,
                            uint32_t& out) {
    SourceEntry se = load_source_entry(bf, entry_index);
    if (!se.ok) return false;
    auto git = se.by_key.find(gao_key);
    if (git == se.by_key.end()) return false;
    GaoInfo gi = parse_gao_full(git->second->data.data(), git->second->data.size());
    if (!gi.ok || !gi.vis_read) return false;
    out = gi.grm_key;
    return true;
}

// _get_multimat_children (gro-4 payload: [key][count][child keys...]).
std::vector<uint32_t> get_multimat_children(const SubEntry& grm) {
    std::vector<uint32_t> children;
    if (grm.gro_null || grm.gro_type != 4) return children;
    const std::vector<uint8_t>& d = grm.data;
    if (d.size() < 8) return children;
    uint32_t n = get_u32(d.data(), 4);
    if (n > 100) return children;
    for (uint32_t i = 0; i < n; ++i) {
        size_t off = 8 + size_t(i) * 4;
        if (off + 4 <= d.size()) children.push_back(get_u32(d.data(), off));
    }
    return children;
}

struct RepSplice { size_t start; size_t end; std::vector<uint8_t> bytes; };

std::vector<uint8_t> replace_sub_entry_bytes(uint32_t key, uint32_t gro_type,
                                             const std::vector<uint8_t>& payload) {
    return make_sub_entry(key, gro_type, payload);
}

// _patch_material_children_inplace: swap material PAYLOADS at the target's
// existing keys (never the GAO's GRM pointer).
void patch_material_children_inplace(
    BigFile& bf, uint32_t source_entry_index, uint32_t source_grm_key,
    uint32_t target_grm_key,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key,
    std::vector<RepSplice>& splices) {
    if (source_grm_key == target_grm_key) return;
    auto tit = sub_by_key.find(target_grm_key);
    if (tit == sub_by_key.end()) return;
    const SubEntry* target_grm = tit->second;

    SourceEntry se = load_source_entry(bf, source_entry_index);
    if (!se.ok) return;
    auto sit = se.by_key.find(source_grm_key);
    if (sit == se.by_key.end()) return;
    const SubEntry* src_grm = sit->second;

    bool target5 = !target_grm->gro_null && target_grm->gro_type == 5;
    bool source5 = !src_grm->gro_null && src_grm->gro_type == 5;
    if (target5 && source5) {
        const std::vector<uint8_t>& sd = src_grm->data;
        const std::vector<uint8_t>& td = target_grm->data;
        if (sd == td) return;
        if (sd.size() != td.size()) {
            size_t start = target_grm->offset - 4;
            size_t end = target_grm->offset + 8 + target_grm->size;
            splices.push_back({start, end,
                               replace_sub_entry_bytes(target_grm_key, 5, sd)});
        } else {
            size_t data_start = target_grm->offset + 12;
            splices.push_back({data_start, data_start + td.size(), sd});
        }
        return;
    }

    std::vector<uint32_t> target_children = get_multimat_children(*target_grm);
    std::vector<uint32_t> source_children = get_multimat_children(*src_grm);
    if (source_children.empty() || target_children.empty()) return;

    for (size_t i = 0; i < source_children.size(); ++i) {
        if (i >= target_children.size()) break;
        auto scit = se.by_key.find(source_children[i]);
        auto tcit = sub_by_key.find(target_children[i]);
        if (scit == se.by_key.end() || tcit == sub_by_key.end()) continue;
        const SubEntry* sc = scit->second;
        const SubEntry* tc = tcit->second;
        if (sc->data.size() != tc->data.size()) {
            size_t start = tc->offset - 4;
            size_t end = tc->offset + 8 + tc->size;
            uint32_t gro = tc->gro_null ? 5 : tc->gro_type;
            splices.push_back({start, end,
                               replace_sub_entry_bytes(target_children[i], gro,
                                                       sc->data)});
        } else if (sc->data != tc->data) {
            size_t data_start = tc->offset + 12;
            splices.push_back({data_start, data_start + tc->data.size(), sc->data});
        }
    }
}

// _copy_source_materials: append missing source material subs at stream end.
void copy_source_materials(
    BigFile& bf, uint32_t source_entry_index, uint32_t source_grm_key,
    const std::unordered_map<uint32_t, const SubEntry*>& target_sub_by_key,
    std::vector<RepSplice>& splices, size_t dec_len) {
    if (target_sub_by_key.count(source_grm_key)) return;
    SourceEntry se = load_source_entry(bf, source_entry_index);
    if (!se.ok) return;
    auto git = se.by_key.find(source_grm_key);
    if (git == se.by_key.end()) return;

    std::vector<uint32_t> keys_needed{source_grm_key};
    std::vector<uint32_t> children = get_multimat_children(*git->second);
    keys_needed.insert(keys_needed.end(), children.begin(), children.end());

    std::vector<uint8_t> append_data;
    for (uint32_t mk : keys_needed) {
        if (target_sub_by_key.count(mk)) continue;
        auto sit = se.by_key.find(mk);
        if (sit == se.by_key.end()) continue;
        const SubEntry* src = sit->second;
        std::vector<uint8_t> rec;
        if (!src->gro_null) {
            rec = make_sub_entry(mk, src->gro_type, src->data);
        } else {
            char ext4[4] = {0, 0, 0, 0};
            for (size_t i = 0; i < 4 && i < src->ext.size(); ++i) ext4[i] = src->ext[i];
            rec = make_sub_entry_ext(mk, ext4, src->data);
        }
        append_data.insert(append_data.end(), rec.begin(), rec.end());
    }
    if (!append_data.empty())
        splices.push_back({dec_len, dec_len, std::move(append_data)});
}

// _apply_geo_replacements (GAO-to-GAO + imported-JGAO branches).
std::vector<uint8_t> apply_geo_replacements(
    const std::vector<uint8_t>& dec, const std::vector<SubEntry>& subs,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key,
    const std::vector<PlaceOp>& replace_ops, BigFile& bf) {
    (void)subs;
    std::vector<RepSplice> splices;
    std::unordered_set<uint32_t> seen_geo_keys;

    for (const PlaceOp& op : replace_ops) {
        uint32_t target_key = op.target_key;
        auto tit = sub_by_key.find(target_key);
        if (tit == sub_by_key.end() || tit->second->ext != ".gao")
            throw PlacementError{"Replace target 0x" + hex8u(target_key) +
                                 " is not a GAO in this zone"};
        const SubEntry* target_sub = tit->second;
        GaoInfo gi = parse_gao_full(target_sub->data.data(), target_sub->data.size());
        if (!gi.ok || !gi.vis_read)
            throw PlacementError{"Replace target GAO 0x" + hex8u(target_key) +
                                 " has no visual section"};

        uint32_t geo_key = gi.gro_key;
        auto geo_it = sub_by_key.find(geo_key);
        if (geo_it == sub_by_key.end() || geo_it->second->gro_null ||
            geo_it->second->gro_type != 1)
            throw PlacementError{"Replace target GAO 0x" + hex8u(target_key) +
                                 " references GEO 0x" + hex8u(geo_key) +
                                 " which was not found in this zone"};
        const SubEntry* geo_sub = geo_it->second;

        if (seen_geo_keys.count(geo_key)) continue;
        seen_geo_keys.insert(geo_key);

        Vec3 scale = vec3_min(op.size, 0.001);
        uint32_t mat_key = gi.grm_key;

        std::vector<uint8_t> new_payload;
        if (op.has_imported_jgao) {
            // Python intentionally copies imported GEO bytes without applying
            // the replacement size scale.
            new_payload = op.imported_geo_data;
            if (op.imported_mat_key != INVALID_KEY &&
                op.imported_mat_key != mat_key) {
                if (!op.imported_mat_data.empty()) {
                    auto target_mat = sub_by_key.find(mat_key);
                    if (target_mat != sub_by_key.end()) {
                        const SubEntry* sub = target_mat->second;
                        uint32_t gro = sub->gro_null ? 4u : sub->gro_type;
                        size_t start = sub->offset - 4;
                        size_t end = sub->offset + 8 + sub->size;
                        splices.push_back({start, end,
                            replace_sub_entry_bytes(mat_key, gro,
                                                    op.imported_mat_data)});
                    }
                }
                std::unordered_set<uint32_t> existing;
                for (const SubEntry& sub : subs) existing.insert(sub.key);
                std::vector<uint8_t> appended;
                for (const ChainEntry& child : op.imported_mat_children) {
                    if (existing.count(child.key)) continue;
                    existing.insert(child.key);
                    std::vector<uint8_t> record = make_sub_entry(
                        child.key, child.has_gro_type ? child.gro_type : 5u,
                        child.data);
                    appended.insert(appended.end(), record.begin(), record.end());
                }
                if (!appended.empty())
                    splices.push_back({dec.size(), dec.size(), std::move(appended)});
            }
        } else if (!op.model_path.empty()) {
            // Python replacement-model path deliberately emits the simple
            // parser-only GEO profile (platform word 0), unlike newly placed
            // objects which need a cooked VB/IB.
            const PlacerGeo imported = load_model_geometry(
                op.model_path, scale, mat_key,
                /*import_vertex_colors=*/false);
            uint32_t target_version = 7;
            if (geo_sub->data.size() >= 4) {
                const uint32_t candidate = get_u32(geo_sub->data.data(), 0);
                if (candidate == 7 || candidate == 8)
                    target_version = candidate;
            }
            new_payload = geometry_to_payload(imported, target_version);
        } else {
            if (!op.has_source_gao_key || !op.has_source_entry_index)
                throw PlacementError{"Replace operation has neither imported "
                                     "JGAO data nor source_gao_key + "
                                     "source_entry_index nor model_path"};

            std::vector<uint8_t> raw =
                load_source_geo_raw(bf, op.source_entry_index, op.source_gao_key);
            bool identity = std::abs(scale[0] - 1.0) < 1e-6 &&
                            std::abs(scale[1] - 1.0) < 1e-6 &&
                            std::abs(scale[2] - 1.0) < 1e-6;
            new_payload = identity ? raw : scale_raw_geo(raw, scale);

            uint32_t source_mat_key = 0;
            if (resolve_source_grm_key(bf, op.source_entry_index, op.source_gao_key,
                                       source_mat_key) &&
                source_mat_key != mat_key) {
                patch_material_children_inplace(bf, op.source_entry_index,
                                                source_mat_key, mat_key, sub_by_key,
                                                splices);
                copy_source_materials(bf, op.source_entry_index, source_mat_key,
                                      sub_by_key, splices, dec.size());
            }
        }

        size_t entry_start = geo_sub->offset - 4;
        size_t entry_end = geo_sub->offset + 8 + geo_sub->size;
        splices.push_back({entry_start, entry_end,
                           replace_sub_entry_bytes(geo_key, 1, new_payload)});
    }

    if (splices.empty()) return dec;

    std::stable_sort(splices.begin(), splices.end(),
                     [](const RepSplice& a, const RepSplice& b) {
                         return a.start > b.start;   // back-to-front
                     });
    std::vector<uint8_t> result = dec;
    for (const RepSplice& sp : splices) {
        std::vector<uint8_t> nr;
        nr.reserve(result.size() - (sp.end - sp.start) + sp.bytes.size());
        nr.insert(nr.end(), result.begin(), result.begin() + long(sp.start));
        nr.insert(nr.end(), sp.bytes.begin(), sp.bytes.end());
        nr.insert(nr.end(), result.begin() + long(sp.end), result.end());
        result = std::move(nr);
    }
    return result;
}

}  // namespace

uint32_t detect_geo_version(BigFile& bf, const std::vector<uint8_t>& dec) {
    if (!dec.empty()) {
        std::vector<SubEntry> subs = walk_sub_entries(dec);
        for (const SubEntry& s : subs) {
            if (s.gro_null || s.gro_type != 1 || s.data.size() < 4) continue;
            uint32_t v = get_u32(s.data.data(), 0);
            if (v == 7 || v == 8) return v;
        }
    }
    if (bf.version < 38) return 7;
    int sampled = 0;
    for (const auto& kv : bf.files) {
        const BFFile& fi = kv.second;
        if (fi.length < 100) continue;
        LzoResult dr = decompress_lzo(bf.read_data(fi.index));
        if (!dr.ok) continue;
        std::vector<SubEntry> subs = walk_sub_entries(dr.data);
        for (const SubEntry& s : subs) {
            if (s.gro_null || s.gro_type != 1 || s.data.size() < 4) continue;
            uint32_t v = get_u32(s.data.data(), 0);
            if (v == 7 || v == 8) return v;
        }
        if (++sampled >= 5) break;
    }
    return 7;
}

// ── apply_bin_replacement (_transplant_visuals + helpers) ───────────────────
namespace {

// _normalize_bone_name: strip ALL ".gao" occurrences + char-specific prefixes.
std::string normalize_bone_name(std::string n) {
    size_t p;
    while ((p = n.find(".gao")) != std::string::npos) n.erase(p, 4);
    for (const char* prefix : {"B_Pr_", "B_SP_"}) {
        size_t len = std::strlen(prefix);
        if (n.rfind(prefix, 0) == 0) return n.substr(len);
    }
    return n;
}

// Insertion-ordered {normalized name -> key} map (Python dict semantics:
// re-assignment updates in place, iteration keeps first-insert order).
struct BoneNameMap {
    std::vector<std::pair<std::string, uint32_t>> items;
    void assign(const std::string& k, uint32_t v) {
        for (auto& kv : items)
            if (kv.first == k) { kv.second = v; return; }
        items.push_back({k, v});
    }
    const uint32_t* find(const std::string& k) const {
        for (const auto& kv : items)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

BoneNameMap build_bone_name_map(const std::vector<SubEntry>& subs) {
    BoneNameMap m;
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
        if (!gi.ok) continue;
        std::string name = gi.name;
        size_t p;
        while ((p = name.find(".gao")) != std::string::npos) name.erase(p, 4);
        if (!name.empty()) m.assign(normalize_bone_name(name), s.key);
    }
    return m;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// _classify_visual_role.
std::string classify_visual_role(const std::string& name) {
    std::string n = lower_ascii(name);
    size_t p;
    while ((p = n.find(".gao")) != std::string::npos) n.erase(p, 4);
    auto has = [&](const char* sub) { return n.find(sub) != std::string::npos; };
    if (has("body")) return "body";
    if (has("face") && (has("hires") || has("highres"))) return "face_hires";
    if (has("face")) return "face";
    if (has("larm") && has("corrupt")) return "larm_corrupted";
    if (has("larm")) return "larm";
    if (has("rarm") && has("corrupt")) return "rarm_corrupted";
    if (has("rarm")) return "rarm";
    if (has("eye") && (has(" l") || has("_l"))) return "eye_l";
    if (has("eye") && (has(" r") || has("_r"))) return "eye_r";
    if (has("thigh") && (has(" l") || has("_l"))) return "thigh_l";
    if (has("thigh") && (has(" r") || has("_r"))) return "thigh_r";
    return "_misc_" + n;
}

struct VisualGao {
    uint32_t key = 0;
    std::string name;
    uint32_t geo_key = 0, mat_key = 0;
    std::string role;
};

std::vector<VisualGao> collect_visual_gaos(
    const std::vector<SubEntry>& subs,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key) {
    std::vector<VisualGao> results;
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
        if (!gi.ok || !gi.vis_read) continue;
        if (gi.gro_key != INVALID_KEY) {
            auto it = by_key.find(gi.gro_key);
            if (it == by_key.end() || it->second->gro_null ||
                it->second->gro_type != 1)
                continue;
        }
        results.push_back({s.key, gi.name, gi.gro_key, gi.grm_key,
                           classify_visual_role(gi.name)});
    }
    return results;
}

// _match_visual_gaos: exact role match, then a largest-effort body fallback.
std::vector<std::pair<const VisualGao*, const VisualGao*>> match_visual_gaos(
    const std::vector<VisualGao>& target_visuals,
    const std::vector<VisualGao>& source_visuals) {
    std::vector<std::pair<const VisualGao*, const VisualGao*>> pairs;
    std::unordered_set<uint32_t> used_sources;
    for (const VisualGao& tv : target_visuals) {
        if (tv.role.rfind("_misc_", 0) == 0) continue;
        for (const VisualGao& sv : source_visuals) {
            if (used_sources.count(sv.key)) continue;
            if (tv.role == sv.role) {
                pairs.push_back({&tv, &sv});
                used_sources.insert(sv.key);
                break;
            }
        }
    }
    bool has_body = false;
    for (const auto& pr : pairs)
        if (pr.first->role == "body") { has_body = true; break; }
    if (!has_body) {
        const VisualGao* tgt_body = nullptr;
        for (const VisualGao& t : target_visuals)
            if (t.role == "body") { tgt_body = &t; break; }
        const VisualGao* src_body = nullptr;
        for (const VisualGao& s : source_visuals)
            if (s.role == "body" && !used_sources.count(s.key)) { src_body = &s; break; }
        if (src_body == nullptr)
            for (const VisualGao& s : source_visuals)
                if (!used_sources.count(s.key) && s.role.rfind("_misc_", 0) != 0) {
                    src_body = &s;
                    break;
                }
        if (tgt_body != nullptr && src_body != nullptr) {
            pairs.push_back({tgt_body, src_body});
            used_sources.insert(src_body->key);
        }
    }
    return pairs;
}

// _rekeyed_gao_payload: source GAO body with target name/GRO/GRM/father and
// GizmoPtr bone keys remapped by normalized name. Empty vector = None.
std::vector<uint8_t> rekeyed_gao_payload(
    const std::vector<uint8_t>& src_gao_data, uint32_t tgt_geo_key,
    uint32_t tgt_mat_key, const BoneNameMap& target_bone_map,
    const std::unordered_map<uint32_t, const SubEntry*>& source_by_key,
    const SubEntry* tgt_gao_sub) {
    if (src_gao_data.size() < 16) return {};
    std::vector<uint8_t> gao = src_gao_data;
    uint32_t ident = get_u32(gao.data(), 8);
    uint32_t name_size = get_u32(gao.data(), 12);

    if (tgt_gao_sub != nullptr) {
        const std::vector<uint8_t>& tgt_data = tgt_gao_sub->data;
        if (tgt_data.size() >= 16) {
            uint32_t tgt_name_size = get_u32(tgt_data.data(), 12);
            size_t old_name_end = 16 + name_size;
            std::vector<uint8_t> rebuilt(gao.begin(), gao.begin() + 12);
            for (int i = 0; i < 4; ++i)
                rebuilt.push_back(uint8_t(tgt_name_size >> (8 * i)));
            size_t copy_n = std::min<size_t>(size_t(16) + tgt_name_size, tgt_data.size());
            rebuilt.insert(rebuilt.end(), tgt_data.begin() + 16,
                           tgt_data.begin() + long(copy_n));
            if (old_name_end <= gao.size())
                rebuilt.insert(rebuilt.end(), gao.begin() + long(old_name_end), gao.end());
            gao = std::move(rebuilt);
            name_size = tgt_name_size;
        }
    }

    size_t off = 16 + name_size + 10 + 68;
    off += (ident & GAO_ID_OBBOX) ? 48 : 24;

    if (ident & GAO_ID_VISUAL) {
        if (off + 8 <= gao.size()) {
            set_u32(gao, off, tgt_geo_key);
            set_u32(gao, off + 4, tgt_mat_key);
        }
        size_t vis_size = 42;
        if (off + 42 <= gao.size()) {
            uint32_t extra_count = uint32_t(gao[off + 38]) |
                                   (uint32_t(gao[off + 39]) << 8);
            if (extra_count > 0 && off + 46 <= gao.size()) {
                uint32_t remaining = get_u32(gao.data(), off + 42);
                if (remaining < gao.size()) vis_size = size_t(46) + remaining;
            }
        }
        off += vis_size;
    }

    if (ident & GAO_ID_HIERARCHY) {
        if (tgt_gao_sub != nullptr && off + 4 <= gao.size()) {
            GaoInfo ti = parse_gao_full(tgt_gao_sub->data.data(),
                                        tgt_gao_sub->data.size());
            if (ti.ok && ti.hier_read) set_u32(gao, off, ti.father_key);
        }
        off += 72;
    }

    if ((ident & GAO_ID_ADDMATRIX) && (ident & GAO_ID_BIT24)) {
        if (off + 4 <= gao.size()) {
            uint32_t count = get_u32(gao.data(), off);
            if (count > 0 && count < 1000 && off + 4 + size_t(count) * 8 <= gao.size()) {
                const uint32_t* fallback = nullptr;
                for (const char* fb : {"Bip Pelvis", "Bip Spine"}) {
                    fallback = target_bone_map.find(fb);
                    if (fallback != nullptr) break;
                }
                if (fallback == nullptr && !target_bone_map.items.empty())
                    fallback = &target_bone_map.items.front().second;

                for (uint32_t gi = 0; gi < count; ++gi) {
                    size_t gk_off = off + 4 + size_t(gi) * 8;
                    uint32_t src_bone_key = get_u32(gao.data(), gk_off);
                    std::string bname;
                    auto bit = source_by_key.find(src_bone_key);
                    if (bit != source_by_key.end() && bit->second->ext == ".gao") {
                        GaoInfo bi = parse_gao_full(bit->second->data.data(),
                                                    bit->second->data.size());
                        if (bi.ok) {
                            bname = bi.name;
                            size_t p;
                            while ((p = bname.find(".gao")) != std::string::npos)
                                bname.erase(p, 4);
                        }
                    }
                    const uint32_t* tk = target_bone_map.find(normalize_bone_name(bname));
                    uint32_t tgt_bone_key =
                        tk != nullptr ? *tk
                                      : (fallback != nullptr ? *fallback : src_bone_key);
                    set_u32(gao, gk_off, tgt_bone_key);
                }
            }
        }
    }
    return gao;
}

// _add_visual_section_to_gao: insert a default 42-byte visual + set 0x4000.
std::vector<uint8_t> add_visual_section_to_gao(const std::vector<uint8_t>& gao_data,
                                               uint32_t gro_key, uint32_t grm_key) {
    if (gao_data.size() < 16) return {};
    std::vector<uint8_t> gao = gao_data;
    uint32_t ident = get_u32(gao.data(), 8);
    if (ident & GAO_ID_VISUAL) return {};
    uint32_t name_size = get_u32(gao.data(), 12);
    size_t insert_off = 16 + name_size + 10 + 68;
    insert_off += (ident & GAO_ID_OBBOX) ? 48 : 24;

    std::vector<uint8_t> vis(42, 0);
    set_u32(vis, 0, gro_key);
    set_u32(vis, 4, grm_key);
    set_u32(vis, 8, 0x05A059BFu);   // DrawMask (standard)
    set_u32(vis, 12, 0xFF100000u);
    vis[16] = 0xFF; vis[17] = 0xFF;
    vis[22] = 0x01;
    vis[26] = 0xFF; vis[27] = 0xFF;
    for (size_t i = 28; i < 38; ++i) vis[i] = 0xFF;

    set_u32(gao, 8, ident | GAO_ID_VISUAL);
    if (insert_off > gao.size()) insert_off = gao.size();   // Python slice clamps
    gao.insert(gao.begin() + long(insert_off), vis.begin(), vis.end());
    return gao;
}

// _hide_gao_visual: GRO = INVALID in place (no size change).
void hide_gao_visual(std::vector<uint8_t>& stream, uint32_t gao_key) {
    std::vector<SubEntry> subs_now = walk_sub_entries(stream);
    for (const SubEntry& s : subs_now) {
        if (s.key != gao_key || s.ext != ".gao") continue;
        const std::vector<uint8_t>& gao_data = s.data;
        if (gao_data.size() < 16) continue;
        uint32_t ident = get_u32(gao_data.data(), 8);
        if (!(ident & GAO_ID_VISUAL)) continue;
        uint32_t name_size = get_u32(gao_data.data(), 12);
        size_t vis_off = 16 + name_size + 10 + 68;
        vis_off += (ident & GAO_ID_OBBOX) ? 48 : 24;
        if (vis_off + 4 <= gao_data.size()) {
            size_t abs = s.offset + 12 + vis_off;
            if (abs + 4 <= stream.size()) set_u32(stream, abs, INVALID_KEY);
        }
        break;
    }
}

// Full-span splice: [sub.offset-4 .. next sub.offset-4 or stream end) — the
// transplant flow replaces whole spans INCLUDING inter-entry padding.
bool full_span_splice(std::vector<uint8_t>& stream, uint32_t key,
                      const char* want_ext, int want_gro,
                      const std::vector<uint8_t>& replacement) {
    std::vector<SubEntry> subs_now = walk_sub_entries(stream);
    for (size_t i = 0; i < subs_now.size(); ++i) {
        const SubEntry& s = subs_now[i];
        if (s.key != key) continue;
        if (want_ext != nullptr && s.ext != want_ext) continue;
        if (want_gro >= 0 && (s.gro_null || int(s.gro_type) != want_gro)) continue;
        if (want_gro == -2 && (s.gro_null ||
                               (s.gro_type != 4 && s.gro_type != 5)))
            continue;                                  // material (gro 4 or 5)
        size_t entry_start = s.offset - 4;
        size_t entry_end = (i + 1 < subs_now.size()) ? subs_now[i + 1].offset - 4
                                                     : stream.size();
        std::vector<uint8_t> ns;
        ns.reserve(stream.size() - (entry_end - entry_start) + replacement.size());
        ns.insert(ns.end(), stream.begin(), stream.begin() + long(entry_start));
        ns.insert(ns.end(), replacement.begin(), replacement.end());
        ns.insert(ns.end(), stream.begin() + long(entry_end), stream.end());
        stream = std::move(ns);
        return true;
    }
    return false;
}

// _transplant_visuals: the full role-matched transplant. Returns swaps.
int transplant_visuals(const std::vector<uint8_t>& target_dec,
                       const std::vector<uint8_t>& source_dec,
                       std::vector<uint8_t>& out) {
    std::vector<SubEntry> target_subs = walk_sub_entries(target_dec);
    std::vector<SubEntry> source_subs = walk_sub_entries(source_dec);
    std::unordered_map<uint32_t, const SubEntry*> target_by_key, source_by_key;
    for (const SubEntry& s : target_subs) target_by_key[s.key] = &s;
    for (const SubEntry& s : source_subs) source_by_key[s.key] = &s;

    std::vector<VisualGao> target_visuals = collect_visual_gaos(target_subs, target_by_key);
    std::vector<VisualGao> source_visuals = collect_visual_gaos(source_subs, source_by_key);
    BoneNameMap target_bone_map = build_bone_name_map(target_subs);

    auto pairs = match_visual_gaos(target_visuals, source_visuals);
    if (pairs.empty())
        throw PlacementError{"No matching visual GAOs found between target and "
                             "source entries. The models may be too different "
                             "to transplant."};

    std::unordered_set<uint32_t> matched_target_keys;
    for (const auto& pr : pairs) matched_target_keys.insert(pr.first->key);

    std::vector<uint8_t> stream = target_dec;
    int swaps = 0;

    // mat_patches: insertion-ordered, re-assignment overwrites in place.
    std::vector<std::pair<uint32_t, const SubEntry*>> mat_patches;
    auto mat_assign = [&](uint32_t k, const SubEntry* v) {
        for (auto& kv : mat_patches)
            if (kv.first == k) { kv.second = v; return; }
        mat_patches.push_back({k, v});
    };
    std::vector<uint32_t> needed_mat_children;

    for (const auto& pr : pairs) {
        const VisualGao& tgt = *pr.first;
        const VisualGao& src = *pr.second;
        uint32_t tgt_geo_key = tgt.geo_key, src_geo_key = src.geo_key;
        if (tgt_geo_key == INVALID_KEY || src_geo_key == INVALID_KEY) continue;
        auto sgit = source_by_key.find(src_geo_key);
        if (sgit == source_by_key.end() || sgit->second->gro_null ||
            sgit->second->gro_type != 1)
            continue;
        const SubEntry* src_geo_sub = sgit->second;

        if (full_span_splice(stream, tgt_geo_key, nullptr, 1,
                             make_sub_entry(tgt_geo_key, 1, src_geo_sub->data)))
            ++swaps;

        uint32_t tgt_mat_key = tgt.mat_key, src_mat_key = src.mat_key;
        if (tgt_mat_key != INVALID_KEY && src_mat_key != INVALID_KEY &&
            tgt_mat_key != src_mat_key) {
            auto smit = source_by_key.find(src_mat_key);
            if (smit != source_by_key.end()) {
                mat_assign(tgt_mat_key, smit->second);
                if (!smit->second->gro_null && smit->second->gro_type == 4) {
                    std::vector<uint32_t> children = get_multimat_children(*smit->second);
                    needed_mat_children.insert(needed_mat_children.end(),
                                               children.begin(), children.end());
                }
            }
        }

        auto sait = source_by_key.find(src.key);
        if (sait != source_by_key.end() && sait->second->ext == ".gao") {
            auto tit = target_by_key.find(tgt.key);
            std::vector<uint8_t> patched_gao = rekeyed_gao_payload(
                sait->second->data, tgt_geo_key, tgt_mat_key, target_bone_map,
                source_by_key, tit != target_by_key.end() ? tit->second : nullptr);
            if (!patched_gao.empty())
                full_span_splice(stream, tgt.key, ".gao", -1,
                                 make_sub_entry_ext(tgt.key, ".gao", patched_gao));
        }
    }

    for (const auto& kv : mat_patches) {
        uint32_t gro = kv.second->gro_null ? 4 : kv.second->gro_type;
        full_span_splice(stream, kv.first, nullptr, -2,
                         make_sub_entry(kv.first, gro, kv.second->data));
    }

    // Append missing material children from the source.
    {
        std::vector<SubEntry> subs_now = walk_sub_entries(stream);
        std::unordered_set<uint32_t> existing_keys;
        for (const SubEntry& s : subs_now) existing_keys.insert(s.key);
        std::vector<uint8_t> append_data;
        std::unordered_set<uint32_t> seen;
        for (uint32_t ck : needed_mat_children) {
            if (existing_keys.count(ck) || seen.count(ck)) continue;
            seen.insert(ck);
            auto sit = source_by_key.find(ck);
            if (sit == source_by_key.end()) continue;
            uint32_t gro = sit->second->gro_null ? 5 : sit->second->gro_type;
            std::vector<uint8_t> rec = make_sub_entry(ck, gro, sit->second->data);
            append_data.insert(append_data.end(), rec.begin(), rec.end());
        }
        stream.insert(stream.end(), append_data.begin(), append_data.end());
    }

    // Inject source bone-visuals into target bones lacking visual sections.
    std::unordered_set<uint32_t> matched_source_keys;
    for (const auto& pr : pairs) matched_source_keys.insert(pr.second->key);
    uint32_t body_mat_key = INVALID_KEY;
    for (const auto& pr : pairs)
        if (pr.first->role == "body") { body_mat_key = pr.first->mat_key; break; }

    for (const VisualGao& sv : source_visuals) {
        if (matched_source_keys.count(sv.key)) continue;
        auto sit = source_by_key.find(sv.key);
        if (sit == source_by_key.end()) continue;
        GaoInfo si = parse_gao_full(sit->second->data.data(), sit->second->data.size());
        if (!si.ok) continue;
        std::string src_name = si.name;
        size_t p;
        while ((p = src_name.find(".gao")) != std::string::npos) src_name.erase(p, 4);
        const uint32_t* tk = target_bone_map.find(normalize_bone_name(src_name));
        if (tk == nullptr || *tk == 0) continue;      // Python: if not tgt_bone_key
        uint32_t tgt_bone_key = *tk;

        std::vector<SubEntry> subs_now = walk_sub_entries(stream);
        const SubEntry* tgt_bone_sub = nullptr;
        for (const SubEntry& s : subs_now)
            if (s.key == tgt_bone_key && s.ext == ".gao") { tgt_bone_sub = &s; break; }
        if (tgt_bone_sub == nullptr) continue;
        GaoInfo bi = parse_gao_full(tgt_bone_sub->data.data(), tgt_bone_sub->data.size());
        if (!bi.ok || bi.vis_read) continue;          // already has a visual

        uint32_t src_geo_key = sv.geo_key;
        uint32_t mat_key = body_mat_key != INVALID_KEY ? body_mat_key : sv.mat_key;
        std::vector<uint8_t> new_gao_data =
            add_visual_section_to_gao(tgt_bone_sub->data, src_geo_key, mat_key);
        if (new_gao_data.empty()) continue;

        full_span_splice(stream, tgt_bone_key, ".gao", -1,
                         make_sub_entry_ext(tgt_bone_key, ".gao", new_gao_data));

        auto git = source_by_key.find(src_geo_key);
        if (git != source_by_key.end()) {
            std::vector<SubEntry> now2 = walk_sub_entries(stream);
            bool present = false;
            for (const SubEntry& s : now2)
                if (s.key == src_geo_key) { present = true; break; }
            if (!present) {
                uint32_t gro = git->second->gro_null ? 1 : git->second->gro_type;
                std::vector<uint8_t> rec = make_sub_entry(src_geo_key, gro,
                                                          git->second->data);
                stream.insert(stream.end(), rec.begin(), rec.end());
            }
        }
    }

    // Hide unmatched same-role-category target visuals.
    std::unordered_set<std::string> source_roles;
    for (const VisualGao& sv : source_visuals)
        if (!sv.role.empty()) source_roles.insert(sv.role);
    static const std::unordered_set<std::string> kHideRoles{
        "body", "face", "face_hires", "larm", "rarm", "eye_l", "eye_r"};
    for (const VisualGao& tv : target_visuals) {
        if (matched_target_keys.count(tv.key)) continue;
        if (kHideRoles.count(tv.role) && source_roles.count(tv.role))
            hide_gao_visual(stream, tv.key);
    }

    out = std::move(stream);
    return swaps;
}

}  // namespace

PlacementSummary apply_bin_replacement(const std::string& bf_path,
                                       const std::string& out_path,
                                       uint32_t target_entry_index,
                                       uint32_t source_entry_index,
                                       bool strict_fit) {
    PlacementSummary sum;
    sum.entry_index = target_entry_index;
    sum.output_path = out_path;
    auto fail = [&](const std::string& m) { sum.error = m; return sum; };
    try {
        std::string src = abs_path_narrow(bf_path);
        std::string dst = abs_path_narrow(out_path);
        if (normcase(src) == normcase(dst))
            throw PlacementError{"Output path must be different from the source BF"};

        BigFile bf;
        try { bf.open(bf_path); } catch (const std::exception& e) {
            throw PlacementError{e.what()};
        }
        auto tit = bf.files.find(target_entry_index);
        if (tit == bf.files.end())
            throw PlacementError{"Target entry " + std::to_string(target_entry_index) +
                                 " not found"};
        BFFile& target_fi = tit->second;
        if (bf.files.find(source_entry_index) == bf.files.end())
            throw PlacementError{"Source entry " + std::to_string(source_entry_index) +
                                 " not found"};

        LzoResult td = decompress_lzo(bf.read_data(target_entry_index));
        if (!td.ok)
            throw PlacementError{"Target entry " + std::to_string(target_entry_index) +
                                 " did not decompress"};
        LzoResult sd = decompress_lzo(bf.read_data(source_entry_index));
        if (!sd.ok)
            throw PlacementError{"Source entry " + std::to_string(source_entry_index) +
                                 " did not decompress"};

        std::vector<uint8_t> patched;
        int swaps = transplant_visuals(td.data, sd.data, patched);

        std::vector<uint8_t> compressed = compress_lzo(patched, 9);
        if (strict_fit && compressed.size() > target_fi.length) {
            size_t over = compressed.size() - target_fi.length;
            throw PlacementError{
                "Transplanted entry does not fit the target's compressed slot: " +
                commas(compressed.size()) + "B > " + commas(target_fi.length) +
                "B (+" + commas(over) +
                "B). Disable strict fit to allow appending (may crash the engine)."};
        }

        if (!copy_file_narrow(bf_path, out_path))
            throw PlacementError{"could not copy the source BF to the output path"};
        std::fstream f(fs::u8path(out_path),
                       std::ios::in | std::ios::out | std::ios::binary);
        if (!f) throw PlacementError{"cannot open output archive for writing"};
        sum.wrote_in_place = write_compressed_entry(f, bf, target_fi, compressed,
                                                    strict_fit);
        update_size_grs(f, bf, target_fi.key, uint32_t(compressed.size() - 4));

        sum.original_compressed_size = target_fi.length;
        sum.new_compressed_size = uint32_t(compressed.size());
        sum.meshes_replaced = uint32_t(swaps);
        sum.ok = true;
        return sum;
    } catch (const PlacementError& e) {
        return fail(e.msg);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

XbinSource collect_xbin_source_chain(BigFile& bf, uint32_t source_entry_index,
                                     uint32_t source_gao_key) {
    XbinSource xs;
    SourceEntry se = load_xbin_source_entry(bf, source_entry_index);
    if (!se.ok) {
        xs.error = "cross-bin clone: source entry did not decompress";
        return xs;
    }
    auto git = se.by_key.find(source_gao_key);
    if (git == se.by_key.end() || git->second->ext != ".gao") {
        xs.error = "cross-bin clone: GAO 0x" + hex8u(source_gao_key) +
                   " not found in source entry";
        return xs;
    }
    // Trim to declared size-4 (drop padding to the next cookie).
    auto trimmed = [](const SubEntry& s) {
        std::vector<uint8_t> payload = s.data;
        uint32_t size = s.size ? s.size : (4 + uint32_t(payload.size()));
        size_t keep = size >= 4 ? size - 4 : 0;
        if (payload.size() > keep) payload.resize(keep);
        return payload;
    };
    xs.gao_data = trimmed(*git->second);

    // BFS-walk the transitive u32 reference closure in body-parse order (the
    // engine's FIFO fetch-queue order — see project/ops_object.py). Unaligned
    // scan, i += 1 even on a match; children queue behind their siblings.
    std::unordered_set<uint32_t> seen_keys;
    std::vector<std::vector<uint8_t>> level_queue{xs.gao_data};
    size_t qhead = 0;
    size_t walked = 0;
    size_t walk_cap = std::max<size_t>(se.by_key.size() * 2, 4096);
    while (qhead < level_queue.size() && walked < walk_cap) {
        std::vector<uint8_t> buf = level_queue[qhead++];
        ++walked;
        size_t n = buf.size();
        for (size_t i = 0; i + 4 <= n; ++i) {
            uint32_t v = get_u32(buf.data(), i);
            auto sit = se.by_key.find(v);
            if (sit == se.by_key.end() || seen_keys.count(v)) continue;
            seen_keys.insert(v);
            const SubEntry* sub = sit->second;
            ChainEntry ce;
            ce.key = v;
            ce.ext = sub->ext;
            ce.has_gro_type = !sub->gro_null && sub->ext.empty();
            ce.gro_type = ce.has_gro_type ? sub->gro_type : 0;
            ce.data = trimmed(*sub);
            level_queue.push_back(ce.data);
            if (ce.has_gro_type && se.by_key.count(ce.gro_type)) {
                std::vector<uint8_t> gtbuf(4);
                set_u32(gtbuf, 0, ce.gro_type);
                level_queue.push_back(std::move(gtbuf));
            }
            xs.chain.push_back(std::move(ce));
        }
    }
    xs.ok = true;
    return xs;
}

XbinSource collect_xbin_validation_source_chain(
    BigFile& bf, uint32_t source_entry_index, uint32_t source_gao_key) {
    XbinSource xs;
    SourceEntry se = load_xbin_source_entry(bf, source_entry_index);
    if (!se.ok) return xs;
    const auto git = se.by_key.find(source_gao_key);
    if (git == se.by_key.end() || git->second->ext != ".gao") return xs;

    auto trimmed = [](const SubEntry& sub) {
        std::vector<uint8_t> payload = sub.data;
        const uint32_t size = sub.size
                                  ? sub.size
                                  : 4u + uint32_t(payload.size());
        const size_t keep = size >= 4 ? size - 4 : 0;
        if (payload.size() > keep) payload.resize(keep);
        return payload;
    };

    // Python validation keeps the GAO's full cookie-delimited payload; only
    // child records are trimmed to their declared size.
    xs.gao_data = git->second->data;
    struct OrderedChain {
        size_t source_offset = 0;
        ChainEntry entry;
    };
    std::vector<OrderedChain> found;
    std::unordered_set<uint32_t> seen;
    std::vector<std::vector<uint8_t>> stack{xs.gao_data};
    size_t walked = 0;
    const size_t walk_cap = std::max<size_t>(se.by_key.size() * 2, 4096);
    while (!stack.empty() && walked < walk_cap) {
        std::vector<uint8_t> buf = std::move(stack.back());
        stack.pop_back();
        ++walked;
        for (size_t off = 0; off + 4 <= buf.size(); ++off) {
            const uint32_t value = get_u32(buf.data(), off);
            const auto sit = se.by_key.find(value);
            if (sit == se.by_key.end() || seen.count(value)) continue;
            seen.insert(value);
            const SubEntry* sub = sit->second;
            ChainEntry entry;
            entry.key = value;
            entry.ext = sub->ext;
            entry.has_gro_type = !sub->gro_null && sub->ext.empty();
            entry.gro_type = entry.has_gro_type ? sub->gro_type : 0;
            entry.data = trimmed(*sub);
            stack.push_back(entry.data);
            if (entry.has_gro_type && se.by_key.count(entry.gro_type)) {
                std::vector<uint8_t> type_key(4);
                set_u32(type_key, 0, entry.gro_type);
                stack.push_back(std::move(type_key));
            }
            found.push_back({sub->offset, std::move(entry)});
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const OrderedChain& left,
                        const OrderedChain& right) {
                         return left.source_offset < right.source_offset;
                     });
    xs.chain.reserve(found.size());
    for (OrderedChain& item : found)
        xs.chain.push_back(std::move(item.entry));
    xs.ok = true;
    return xs;
}

PlacementSummary apply_placement_plan(const std::string& bf_path,
                                      const std::string& out_path,
                                      uint32_t entry_index,
                                      const std::vector<PlaceOp>& ops,
                                      bool strict_fit) {
    PlacementSummary sum;
    sum.entry_index = entry_index;
    sum.output_path = out_path;
    auto fail = [&](const std::string& m) { sum.error = m; return sum; };
    try {
        if (ops.empty()) throw PlacementError{"No operations were provided"};

        // Python: os.path.normcase(abspath(a)) == normcase(abspath(b)).
        std::string src = abs_path_narrow(bf_path);
        std::string dst = abs_path_narrow(out_path);
        if (normcase(src) == normcase(dst))
            throw PlacementError{"Output path must be different from the source BF"};

        std::vector<PlaceOp> replace_ops, place_ops;
        for (const PlaceOp& op : ops)
            (op.kind == "replace" ? replace_ops : place_ops).push_back(op);

        BigFile bf;
        try { bf.open(bf_path); } catch (const std::exception& e) {
            throw PlacementError{e.what()};
        }
        auto fit = bf.files.find(entry_index);
        if (fit == bf.files.end())
            throw PlacementError{"Entry " + std::to_string(entry_index) + " not found"};
        BFFile& fi = fit->second;

        LzoResult dr = decompress_lzo(bf.read_data(entry_index));
        if (!dr.ok)
            throw PlacementError{"Entry " + std::to_string(entry_index) +
                                 " did not decompress"};
        std::vector<uint8_t> dec = std::move(dr.data);

        uint32_t geo_version = detect_geo_version(bf, dec);

        // Mesh replacements first (splice GEO sub-entries in-place).
        if (!replace_ops.empty()) {
            std::vector<SubEntry> subs = walk_sub_entries(dec);
            std::unordered_map<uint32_t, const SubEntry*> sub_by_key;
            for (const SubEntry& s : subs) sub_by_key[s.key] = &s;   // last wins
            dec = apply_geo_replacements(dec, subs, sub_by_key, replace_ops, bf);
            sum.meshes_replaced = uint32_t(replace_ops.size());
        }

        std::vector<uint8_t> patched = dec;
        PlacementResult r;
        if (!place_ops.empty()) {
            std::vector<uint32_t> extra_keys;
            extra_keys.reserve(bf.files.size());
            for (const auto& kv : bf.files)
                if (kv.second.key != INVALID_KEY) extra_keys.push_back(kv.second.key);
            r = apply_placements_to_dec(
                dec, place_ops, geo_version, extra_keys,
                (fi.key ^ entry_index) & 0xFFFFFFFFu);
            if (!r.ok) throw PlacementError{r.error};
            patched = std::move(r.patched);
        }

        std::vector<uint8_t> compressed = compress_lzo(patched, 9);
        if (strict_fit && compressed.size() > fi.length) {
            size_t over = compressed.size() - fi.length;
            throw PlacementError{
                "Patched entry does not fit its original compressed slot: " +
                commas(compressed.size()) + "B > " + commas(fi.length) + "B (+" +
                commas(over) + "B). Disable strict fit only for experiments; "
                "appended entries have crashed in prior tests."};
        }

        if (!copy_file_narrow(bf_path, out_path))
            throw PlacementError{"could not copy the source BF to the output path"};
        std::fstream f(fs::u8path(out_path),
                       std::ios::in | std::ios::out | std::ios::binary);
        if (!f) throw PlacementError{"cannot open output archive for writing"};
        sum.wrote_in_place = write_compressed_entry(f, bf, fi, compressed, strict_fit);
        update_size_grs(f, bf, fi.key, uint32_t(compressed.size() - 4));

        sum.objects_added = uint32_t(place_ops.size());
        sum.sub_entries_added = uint32_t(r.additions.size());
        sum.objects_registered = r.objects_registered;
        sum.original_compressed_size = fi.length;
        sum.new_compressed_size = uint32_t(compressed.size());
        sum.has_world_list_key = r.has_world_list_key;
        sum.world_list_key = r.world_list_key;
        sum.ok = true;
        return sum;
    } catch (const PlacementError& e) {
        return fail(e.msg);
    } catch (const std::exception& e) {
        return fail(e.what());   // never let a stray exception terminate the CLI
    }
}

std::vector<std::pair<size_t, uint32_t>> gao_colmap_key_offsets(
    const std::vector<uint8_t>& gao,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key) {
    return gao_colmap_key_offsets_impl(gao, by_key);
}

WorldListKeysResult world_object_list_keys(const std::vector<SubEntry>& subs) {
    WorldList wl = find_world_object_list_sub(subs);
    if (wl.list == nullptr) return {};
    std::vector<uint8_t> full = sub_full_content(*wl.list);
    return {true, world_list_keys_ordered(full, wl.pairs8)};
}

namespace {

// The 16 col-major floats of a GAO's stored global matrix (f32 -> double).
bool gmat_col_major(const GaoInfo& info, Mat16& out) {
    if (!info.gmat_present || info.gmat_raw.size() < 64) return false;
    for (size_t i = 0; i < 16; ++i) {
        float f;
        std::memcpy(&f, info.gmat_raw.data() + i * 4, 4);
        out[i] = double(f);
    }
    return true;
}

// _object_world_center: centre of the object's bounds (or mesh) under its
// Jade matrix — the host-COB query point reflects where the geometry IS,
// not where the matrix origin sits.
Vec3 object_world_center(const Mat16& m16, const ObboxBounds* local_bounds,
                         const std::vector<float>* verts) {
    Mat16 M = jade_matrix_to_math_4x4(m16);
    // NOTE the reversed accumulation (w-term first): numpy's 4x4 @ vec4 sums
    // the dot product back-to-front, and the doubles feed COB normals where
    // the last ulp is visible. Empirically exact against the installed numpy.
    auto xform = [&](double x, double y, double z) {
        std::array<double, 3> o;
        for (int r = 0; r < 3; ++r)
            o[size_t(r)] = ((M[size_t(3 * 4 + r)] + M[size_t(2 * 4 + r)] * z) +
                            M[size_t(1 * 4 + r)] * y) +
                           M[size_t(0 * 4 + r)] * x;
        return o;
    };
    bool any = false;
    std::array<double, 3> mn{}, mx{};
    auto feed = [&](const std::array<double, 3>& p) {
        if (!any) { mn = mx = p; any = true; return; }
        for (int i = 0; i < 3; ++i) {
            if (p[size_t(i)] < mn[size_t(i)]) mn[size_t(i)] = p[size_t(i)];
            if (p[size_t(i)] > mx[size_t(i)]) mx[size_t(i)] = p[size_t(i)];
        }
    };
    if (verts != nullptr) {
        for (size_t i = 0; i + 2 < verts->size(); i += 3)
            feed(xform(double((*verts)[i]), double((*verts)[i + 1]),
                       double((*verts)[i + 2])));
    } else if (local_bounds != nullptr && local_bounds->ok) {
        for (int xi = 0; xi < 2; ++xi)
            for (int yi = 0; yi < 2; ++yi)
                for (int zi = 0; zi < 2; ++zi)
                    feed(xform(xi ? local_bounds->mx[0] : local_bounds->mn[0],
                               yi ? local_bounds->mx[1] : local_bounds->mn[1],
                               zi ? local_bounds->mx[2] : local_bounds->mn[2]));
    }
    if (!any) return {m16[12], m16[13], m16[14]};
    return {(mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5,
            (mn[2] + mx[2]) * 0.5};
}

// _add_object_mesh_collision: append the object's visual triangles into the
// host COB(s) in each host's local frame, and widen the host GAO's BV.
std::vector<uint8_t> add_object_mesh_collision(
    const std::vector<uint8_t>& dec, uint32_t gao_key, const SubEntry& target,
    const GaoInfo& info, const Mat16& m16,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key,
    const std::vector<SubEntry>& subs, bool has_room_cob_key,
    uint32_t room_cob_key) {
    char b[160];
    uint32_t geo_key = info.vis_read ? info.gro_key : INVALID_KEY;
    auto git = by_key.find(geo_key);
    const SubEntry* geo_sub =
        git == by_key.end() ? nullptr : git->second;
    if (geo_sub == nullptr || geo_sub->gro_null || geo_sub->gro_type != 1) {
        std::snprintf(b, sizeof b,
                      "add_collision(mesh): GAO 0x%08X has no visual GEO to "
                      "use as collision — try the box shape instead", gao_key);
        throw PlacementError{b};
    }
    GeoInfo geo = parse_geometry(geo_sub->data.data(), geo_sub->data.size());
    std::vector<std::array<uint32_t, 3>> ofaces;
    for (size_t i = 0; i + 6 < geo.faces.size(); i += 7)
        ofaces.push_back({geo.faces[i], geo.faces[i + 1], geo.faces[i + 2]});
    if (!geo.ok || geo.vertices.empty() || ofaces.empty()) {
        std::snprintf(b, sizeof b,
                      "add_collision(mesh): GEO 0x%08X has no triangles",
                      geo_key);
        throw PlacementError{b};
    }

    Vec3 center = object_world_center(m16, nullptr, &geo.vertices);
    std::vector<uint32_t> host_keys;
    if (has_room_cob_key)
        host_keys = {room_cob_key};
    else {
        host_keys = pick_clone_host_cobs(center, subs, by_key);
        if (host_keys.empty())
            throw PlacementError{
                "add_collision: no triangle-shape COB found in the bin to "
                "host the collision; specify a room_cob_key explicitly."};
    }

    Mat16 M_obj = jade_matrix_to_math_4x4(m16);
    std::vector<uint8_t> new_dec = dec;
    for (uint32_t host_key : host_keys) {
        std::vector<SubEntry> rsubs = walk_sub_entries(new_dec);
        std::unordered_map<uint32_t, const SubEntry*> rbk;
        for (const SubEntry& s : rsubs) rbk[s.key] = &s;
        auto hit = rbk.find(host_key);
        const SubEntry* host_sub = hit == rbk.end() ? nullptr : hit->second;
        if (host_sub == nullptr || !looks_like_cob_sub_pl(host_sub)) continue;
        if (host_sub->data.empty() ||
            host_sub->data[0] != COL_ZONE_TRIANGLES)
            continue;
        const SubEntry* host_gao = find_host_gao_for_cob(host_key, rsubs, rbk);
        Mat16 M_host{};
        for (int i = 0; i < 4; ++i) M_host[size_t(i * 4 + i)] = 1.0;
        if (host_gao != nullptr) {
            GaoInfo hinfo =
                parse_gao_full(host_gao->data.data(), host_gao->data.size());
            Mat16 hm{};
            if (hinfo.ok && gmat_col_major(hinfo, hm))
                M_host = jade_matrix_to_math_4x4(hm);
        }
        Mat16 M_host_inv{};
        if (!inv4x4(M_host, M_host_inv)) {
            M_host_inv = Mat16{};
            for (int i = 0; i < 4; ++i) M_host_inv[size_t(i * 4 + i)] = 1.0;
        }
        Mat16 T = col_major_mul(M_host_inv, M_obj);   // object-local -> host-local
        std::vector<std::array<double, 3>> verts_local;
        verts_local.reserve(geo.vertices.size() / 3);
        // Reversed accumulation matches numpy's @ (see object_world_center).
        for (size_t i = 0; i + 2 < geo.vertices.size(); i += 3) {
            double x = double(geo.vertices[i]);
            double y = double(geo.vertices[i + 1]);
            double z = double(geo.vertices[i + 2]);
            std::array<double, 3> o;
            for (int r = 0; r < 3; ++r)
                o[size_t(r)] = ((T[size_t(3 * 4 + r)] +
                                 T[size_t(2 * 4 + r)] * z) +
                                T[size_t(1 * 4 + r)] * y) +
                               T[size_t(0 * 4 + r)] * x;
            verts_local.push_back(o);
        }
        int32_t mat; uint8_t design, flag_;
        sample_host_bulk_element(*host_sub, mat, design, flag_);
        std::vector<uint8_t> new_payload = extend_cob_triangle_mesh(
            host_sub->data.data(), host_sub->data.size(), verts_local, ofaces,
            mat, design, flag_);
        if (new_payload.empty()) {
            std::snprintf(b, sizeof b,
                          "add_collision(mesh): host COB 0x%08X could not be "
                          "extended", host_key);
            throw PlacementError{b};
        }
        std::vector<uint8_t> new_record =
            make_sub_entry(host_key, host_sub->gro_type, new_payload);
        size_t rec_start = host_sub->offset - 4;
        size_t rec_end = host_sub->offset + 8 + host_sub->size;
        std::vector<uint8_t> next;
        next.reserve(new_dec.size() + new_record.size());
        next.insert(next.end(), new_dec.begin(),
                    new_dec.begin() + long(rec_start));
        next.insert(next.end(), new_record.begin(), new_record.end());
        next.insert(next.end(), new_dec.begin() + long(rec_end),
                    new_dec.end());
        new_dec = std::move(next);

        // Widen the host GAO's BV to include the new (host-local) verts.
        if (host_gao != nullptr && !verts_local.empty()) {
            std::array<double, 3> lmin = verts_local[0], lmax = verts_local[0];
            for (const auto& v : verts_local)
                for (int i = 0; i < 3; ++i) {
                    if (v[size_t(i)] < lmin[size_t(i)])
                        lmin[size_t(i)] = v[size_t(i)];
                    if (v[size_t(i)] > lmax[size_t(i)])
                        lmax[size_t(i)] = v[size_t(i)];
                }
            uint32_t hg_key = host_gao->key;
            std::vector<SubEntry> r2 = walk_sub_entries(new_dec);
            const SubEntry* hg2 = nullptr;
            for (const SubEntry& s : r2)
                if (s.key == hg_key) hg2 = &s;
            if (hg2 != nullptr) {
                std::vector<uint8_t> newp = extend_obbox_to_include(
                    hg2->data.data(), hg2->data.size(), lmin, lmax);
                if (!newp.empty() && newp != hg2->data) {
                    std::vector<uint8_t> grec;
                    if (!hg2->ext.empty() && hg2->ext.size() == 4) {
                        char ext4[4];
                        std::memcpy(ext4, hg2->ext.data(), 4);
                        grec = make_sub_entry_ext(hg2->key, ext4, newp);
                    } else {
                        grec = make_sub_entry(hg2->key, hg2->gro_type, newp);
                    }
                    size_t gs = hg2->offset - 4;
                    size_t ge = hg2->offset + 8 + hg2->size;
                    std::vector<uint8_t> nx;
                    nx.reserve(new_dec.size() + grec.size());
                    nx.insert(nx.end(), new_dec.begin(),
                              new_dec.begin() + long(gs));
                    nx.insert(nx.end(), grec.begin(), grec.end());
                    nx.insert(nx.end(), new_dec.begin() + long(ge),
                              new_dec.end());
                    new_dec = std::move(nx);
                }
            }
        }
    }
    (void)target;
    return new_dec;
}

}  // namespace

AddCollisionResult add_object_collision_box(
    const std::vector<uint8_t>& dec, uint32_t gao_key,
    const std::string& collision_profile, bool has_room_cob_key,
    uint32_t room_cob_key, const std::string& shape) {
    try {
        std::vector<SubEntry> subs = walk_sub_entries(dec);
        std::unordered_map<uint32_t, const SubEntry*> by_key;
        for (const SubEntry& s : subs) by_key[s.key] = &s;
        auto it = by_key.find(gao_key);
        const SubEntry* target = it == by_key.end() ? nullptr : it->second;
        char b[112];
        if (target == nullptr || target->ext != ".gao") {
            std::snprintf(b, sizeof b,
                          "add_collision: GAO 0x%08X not found in entry",
                          gao_key);
            throw PlacementError{b};
        }
        GaoInfo info = parse_gao_full(target->data.data(),
                                      target->data.size());
        Mat16 m16{};
        if (!info.ok || !gmat_col_major(info, m16)) {
            std::snprintf(b, sizeof b,
                          "add_collision: GAO 0x%08X has no parseable matrix",
                          gao_key);
            throw PlacementError{b};
        }

        std::string sh = shape.empty() ? "mesh" : shape;
        for (char& c : sh) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (sh != "box") {
            AddCollisionResult r;
            r.dec = add_object_mesh_collision(dec, gao_key, *target, info,
                                              m16, by_key, subs,
                                              has_room_cob_key, room_cob_key);
            r.ok = true;
            return r;
        }

        // Box: a synthetic clone-shaped op so the extension path uses the
        // object's own OBBox bounds (source_key) and matrix for a tight,
        // correctly-oriented box; host picked from the WORLD geometry centre.
        ObboxBounds obj_bounds =
            obbox_local_bounds(target->data.data(), target->data.size());
        Vec3 center = object_world_center(m16, &obj_bounds, nullptr);
        PlaceOp synth;
        synth.kind = "clone";
        synth.collision = true;
        synth.source_key = gao_key;
        synth.has_world_matrix = true;
        synth.world_matrix = m16;
        synth.position = center;
        synth.collision_profile =
            collision_profile.empty() ? "simple_box" : collision_profile;
        synth.has_room_cob_key = has_room_cob_key;
        synth.room_cob_key = room_cob_key;
        AddCollisionResult r;
        r.dec = apply_clone_collision_cob_extensions(dec, {synth});
        r.ok = true;
        return r;
    } catch (const PlacementError& e) {
        return {false, e.msg, {}};
    } catch (const std::exception& e) {
        return {false, e.what(), {}};
    }
}

}  // namespace placer
}  // namespace jade
