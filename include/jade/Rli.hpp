// Rli.hpp — Jade baked per-vertex radiosity lighting (RLI) reader (read path).
//
// Port of jade_explorer/core/rli.py read functions. The engine lights *static*
// geometry with per-vertex colours that live in the GAO's visual block in two
// copies:
//   * the PRIMARY table   [0xFFFF][count=nb][BGRA x nb]  (base-vertex order), and
//   * an EXTRA block       [marker][size][count_exp][12] + (BGRA,-1f,-1f) x count_exp
//     (expanded to the cooked vertex-buffer's vertex count — UV-seam splits).
//
// The extra block is what actually renders. nb (the GEO's base vertex count) and
// the cooked vertex buffer come from the GAO's GEO. expanded->base mapping matches
// cooked-VB positions to the GEO's base-point positions exactly.
//
// All offsets/bounds/edge-cases mirror the Python byte-for-byte. ok==false /
// empty mirror the Python None.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace jade {

// (r,g,b,a) 0–255 for one vertex (reordered from a stored BGRA quad).
struct RgbaColor {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};

// ── primary-table locator ────────────────────────────────────────────────

// Byte offset of the GAO's visual block (port of gao.visual_block_offset):
// header(16) + name_size + 10 + 68 matrix + BV(24/48). Returns -1 when there is
// no visual block (identity & 0x4000 == 0) or it runs past the payload.
long long visual_block_offset(const uint8_t* gao, size_t n);

// Validate a candidate [0xFFFF][count=nb][BGRA x nb] table whose first BGRA entry
// is at byte offset `cs` (port of rli._is_real_color_table).
bool is_real_color_table(const uint8_t* gao, size_t n, size_t cs, uint32_t nb);

// Byte offset of the first BGRA entry of the primary dul_VertexColors table
// (count == nb), or -1. Validated so a stray 0xFFFF never matches.
long long find_primary_table(const uint8_t* gao, size_t n, uint32_t nb);

// Locate the expanded extra block after the primary table. On success sets
// `entries_start` (byte offset of the first 12-byte entry) and `count_exp`, and
// returns true; returns false (and leaves outputs untouched) when not found.
bool find_extra_block(const uint8_t* gao, size_t n, size_t primary_end,
                      size_t& entries_start, uint32_t& count_exp);

bool has_rli(const uint8_t* gao, size_t n, uint32_t nb);

// ── colour read ──────────────────────────────────────────────────────────

// Per BASE vertex (r,g,b,a) from the primary table. Empty + ok=false if no table.
struct PrimaryColors {
    bool ok = false;
    std::vector<RgbaColor> colors;   // length nb when ok
};
PrimaryColors read_primary_colors(const uint8_t* gao, size_t n, uint32_t nb);

// Per EXPANDED vertex (r,g,b,a) from the extra block, plus its location.
struct ExtraColors {
    bool ok = false;
    std::vector<RgbaColor> colors;   // length count_exp when ok
    size_t entries_start = 0;
    uint32_t count_exp = 0;
};
ExtraColors read_extra_colors(const uint8_t* gao, size_t n, uint32_t nb);

// ── expanded -> base mapping (via cooked vertex buffer positions) ──────────

// The cooked GPU vertex buffer section [magic][n][stride] (largest-n match).
struct CookedVbSection {
    bool ok = false;
    size_t data_start = 0;   // byte offset of vertex 0
    uint32_t n = 0;          // vertex count
    uint32_t stride = 0;     // 20/24/28/32/44
};
CookedVbSection cooked_vb_section(const uint8_t* geo, size_t n);

// The cooked GPU mesh, 1:1 with the extra-block colours.
struct CookedMesh {
    bool ok = false;
    std::vector<float> positions;   // 3*n (x,y,z)
    std::vector<float> uvs;          // 2*n (u,v)
    std::vector<uint16_t> faces;     // 3*ntri (i0,i1,i2)
    uint32_t n = 0;
};
CookedMesh cooked_mesh(const uint8_t* geo, size_t n);

// For each cooked vertex, the base vertex index it came from (length == cooked
// VB n). ok=false when the cooked VB / base points can't be read. Unmatched
// verts fall back to index 0.
struct ExpandedToBase {
    bool ok = false;
    std::vector<uint32_t> map;   // length == cooked VB n
};
ExpandedToBase expanded_to_base(const uint8_t* geo, size_t gn, uint32_t nb);

// ── colour write (in place, same byte size) ────────────────────────────────
//
// All return the new GAO bytes, or an EMPTY vector for the Python None (no RLI
// table / no net change). RGB inputs are clamped to 0-255 and rounded
// half-to-even (Python int(round(c))); each entry's existing alpha and the extra
// block's two -1.0 float sentinels are preserved.

// One vertex colour for the write helpers; alpha is taken from the existing entry.
using RgbF = std::array<double, 3>;

// Apply color_fn(r,g,b)->(r,g,b) to every existing RLI entry (primary + extra).
// Rescales the colours already present (atmosphere grading).
using RliColorFn = std::function<std::array<int, 3>(int, int, int)>;
std::vector<uint8_t> transform_rli(const uint8_t* gao, size_t n, uint32_t nb,
                                   const RliColorFn& color_fn);

// Write per-BASE-vertex colours (length nb). When `geo` is given the extra block
// is refreshed via the expanded->base map; else only the primary is written
// (update_extra=false skips the extra block entirely).
std::vector<uint8_t> write_rli(const uint8_t* gao, size_t n, uint32_t nb,
                               const std::vector<RgbF>& base_colors,
                               const uint8_t* geo, size_t geo_n,
                               bool update_extra = true);

// Write per-COOKED-vertex colours 1:1 into the extra block. When `geo` is given
// the primary table is refreshed too (base vertex <- a cooked vertex at its pos).
std::vector<uint8_t> write_extra_colors(const uint8_t* gao, size_t n, uint32_t nb,
                                        const std::vector<RgbF>& cooked_colors,
                                        const uint8_t* geo, size_t geo_n);

// Write a lightmap uv1 (u,v) per cooked vertex into each extra-block entry's two
// trailing floats; leaves the BGRA colour untouched.
std::vector<uint8_t> write_extra_uv1(const uint8_t* gao, size_t n, uint32_t nb,
                                     const std::vector<std::array<float, 2>>& cooked_uv1);

}  // namespace jade
