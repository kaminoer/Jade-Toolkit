// placer_leaf — emit the object_placer leaf-builder outputs for a fixed,
// deterministic case matrix (hex / f64-bit dumps). Compared byte-for-byte
// against the REAL Python helpers by tests/placer_leaf_check.py.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "jade/Crc32.hpp"
#include "jade/ObjectPlacer.hpp"

using namespace jade::placer;

static void hexline(const char* tag, const std::vector<uint8_t>& v) {
    std::printf("%s ", tag);
    for (uint8_t b : v) std::printf("%02x", b);
    std::printf("\n");
}
static void f64line(const char* tag, const double* v, int n) {
    std::printf("%s", tag);
    for (int i = 0; i < n; ++i) {
        uint64_t b;
        std::memcpy(&b, &v[i], 8);
        std::printf(" %016llx", static_cast<unsigned long long>(b));
    }
    std::printf("\n");
}

int main() {
    const Vec3 positions[] = {{0, 0, 0}, {12.5, -3.75, 220.0625}, {-0.001, 1e6, 3.14159}};
    const Vec3 rotations[] = {{0, 0, 0}, {90, 0, 0}, {12.3, -45.6, 78.9}, {0, 0, 33.333}};
    const Vec3 scales[]    = {{1, 1, 1}, {2, 2, 2}, {0.25, 1.5, 3.75}};

    // trs_to_matrix / ltype / math4x4 — exact f64 bits (the FP core).
    for (const Vec3& p : positions)
        for (const Vec3& r : rotations)
            for (const Vec3& s : scales) {
                Mat16 m = trs_to_matrix(p, r, s);
                f64line("TRS", m.data(), 16);
            }
    {
        Mat16 a = trs_to_matrix({1, 2, 3}, {30, 40, 50}, {1, 1, 1});
        Mat16 b = trs_to_matrix({-4, 0, 9}, {0, 15, 0}, {2, 1, 0.5});
        Mat16 c = col_major_mul(a, b);
        f64line("MUL", c.data(), 16);
        // Jade-layout sample: I/J/K + S slots + T.
        Mat16 jm = {0.8, 0.6, 0.0, 2.5, -0.6, 0.8, 0.0, 0.0,
                    0.0, 0.0, 1.0, 1.25, 10.0, -20.0, 30.0, 1.0};
        Mat16 mm = jade_matrix_to_math_4x4(jm);
        f64line("J2M", mm.data(), 16);
        std::printf("LTYPE %u %u %u\n",
                    ltype_from_jade_matrix(jm),
                    ltype_from_jade_matrix(matrix_values({0, 0, 0})),
                    ltype_from_jade_matrix(matrix_values({5, 0, 0})));
    }

    // matrix68 + obbox_bytes (f32-packed).
    for (const Vec3& p : positions)
        for (const Vec3& r : rotations)
            for (const Vec3& s : scales)
                hexline("M68", matrix68(p, r, s));
    const Vec3 bmin[] = {{-1, -2, -3}, {0, 0, 0}, {-0.5, -12.25, 3.5}};
    const Vec3 bmax[] = {{4, 5, 6}, {1, 1, 1}, {0.5, 42.0, 9.75}};
    for (int i = 0; i < 3; ++i)
        for (const Vec3& r : rotations)
            for (const Vec3& s : scales)
                hexline("OBB", obbox_bytes(bmin[i], bmax[i], r, s));

    // visual_bytes variants.
    hexline("VIS0", visual_bytes(0x7A001111, 0x7A002222, 0));
    hexline("VISW", visual_bytes(0x7A001111, 0x7A002222, 5));
    {
        VisualColors flat;
        flat.flat = true;
        flat.tint = {13.6, 250.0, 3.0};
        hexline("VISF", visual_bytes(0x7A001111, 0x7A002222, 5, &flat));
        VisualColors pv;
        pv.per_vertex = true;
        pv.list = {{0, 0, 0}, {255, 128, 64.5}, {300.5, -2, 12}};
        hexline("VISP", visual_bytes(0x7A001111, 0x7A002222, 5, &pv));
    }

    // ode / stub / tail / names.
    hexline("ODE", ode_box_bytes({1.5, 0.0, 3.25}));
    hexline("ODE", ode_box_bytes({0.0002, 2.0, -4.0}));
    hexline("STUB", extended_stub_bytes());
    const char* names[] = {"My Object", "weird/name:*here", "", "   ", "already.GAO",
                           "x", "a_very_long_name_padded_out_to_exceed_the_editor_limit_"
                                "0123456789_0123456789_0123456789_0123456789_0123456789_end"};
    for (const char* nm : names) {
        std::printf("NAME %s\n", clean_gao_name(nm).c_str());
        hexline("TAIL", runtime_editor_tail(nm));
    }

    // Synthetic GAO: header16 (identity OBBOX|extra), name 8, +10, matrix 68,
    // BV 48, tail 4 -> patch position / world matrix (with+without bounds) / trs.
    std::vector<uint8_t> gao(16 + 8 + 10 + 68 + 48 + 4, 0);
    auto w32 = [&](size_t o, uint32_t v) {
        gao[o] = v & 0xFF; gao[o + 1] = (v >> 8) & 0xFF;
        gao[o + 2] = (v >> 16) & 0xFF; gao[o + 3] = (v >> 24) & 0xFF;
    };
    auto wf = [&](size_t o, float f) { uint32_t b; std::memcpy(&b, &f, 4); w32(o, b); };
    w32(0, 8);
    w32(8, FLAG_OBBOX | 0x4000);
    w32(12, 8);
    std::memcpy(&gao[16], "test.gao", 8);
    size_t gm = 16 + 8 + 10;
    wf(gm + 0, 1); wf(gm + 20, 1); wf(gm + 40, 1);           // identity I/J/K
    wf(gm + 48, 7.5f); wf(gm + 52, -2.5f); wf(gm + 56, 90.0f);  // T
    size_t bv = gm + 68;
    wf(bv + 24, -1); wf(bv + 28, -2); wf(bv + 32, -3);        // LMin
    wf(bv + 36, 4); wf(bv + 40, 5); wf(bv + 44, 6);           // LMax

    {
        std::vector<uint8_t> g1 = gao;
        patch_gao_position(g1, {100.25, -50.5, 0.125});
        hexline("PPOS", g1);
        std::vector<uint8_t> g2 = gao;
        Mat16 wm = trs_to_matrix({9, 8, 7}, {15, 25, 35}, {1, 1, 1});
        patch_gao_world_matrix(g2, wm);
        hexline("PWM0", g2);
        std::vector<uint8_t> g3 = gao;
        Vec3 lo{-1, -2, -3}, hi{4, 5, 6};
        patch_gao_world_matrix(g3, wm, &lo, &hi);
        hexline("PWMB", g3);
        std::vector<uint8_t> g4 = gao;
        patch_gao_trs(g4, {3, 2, 1}, {0, 90, 0}, {2, 2, 2});
        hexline("PTRS", g4);
        hexline("RNAME", replace_gao_name(gao.data(), gao.size(), "renamed thing"));
    }
    // --- B3: primitive geometry -> GEO payloads (CRC'd; big buffers). ---
    auto crcline = [](const char* tag, const std::vector<uint8_t>& v) {
        std::printf("%s len=%zu crc=%08x\n", tag, v.size(),
                    jade::crc32(v.data(), v.size()));
    };
    {
        PlacerGeo cube = build_cube_geometry(1.5, 2.0, 0.75, 0x7A004242);
        // Small enough to compare fully.
        hexline("CUBE7", geometry_to_payload(cube, 7));
        hexline("CUBE8", geometry_to_payload(cube, 8));
        hexline("CUBEVB4", geometry_to_payload_with_vb(cube, 7, 0x4));
        crcline("CUBEVB8", geometry_to_payload_with_vb(cube, 8, 0x8));
        PlacerGeo cyl = build_cylinder_geometry(2.0, 3.0, 5.5, 0x7A00AAAA);
        crcline("CYL", geometry_to_payload_with_vb(cyl, 7, 0x4));
        crcline("CYLNP", geometry_to_payload(cyl, 7));
        PlacerGeo sph = build_sphere_geometry(4.0, 4.0, 2.5, 0x7A00BBBB);
        crcline("SPH", geometry_to_payload_with_vb(sph, 8, 0x8));
        PlacerGeo cyl6 = build_cylinder_geometry(1.0, 1.0, 1.0, 0x7A00CCCC, 6);
        hexline("CYL6", geometry_to_payload_with_vb(cyl6, 7, 0x4));
    }
    return 0;
}
