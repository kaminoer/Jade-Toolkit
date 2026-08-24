// Gao.hpp — Jade Game Object (GAO) parser (read path).
//
// Port of jade_explorer/core/gao.py. A GAO record payload in the _wow_ stream
// begins with a ".gao" tag (surfaced as the sub-entry `ext`), then the struct
// parsed here: header (version/editor_flags/identity/name), a 68-byte global
// matrix, a bounding volume (24 or 48 B by the OBBox identity bit), then
// identity-gated blocks — visual (gro/grm keys), hierarchy (father + local
// matrix), and gizmo pointers.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jade/SubEntry.hpp"

namespace jade {

constexpr uint32_t GAO_ID_VISUAL    = 0x00004000;
constexpr uint32_t GAO_ID_OBBOX     = 0x00080000;
constexpr uint32_t GAO_ID_ADDMATRIX = 0x00200000;
constexpr uint32_t GAO_ID_HIERARCHY = 0x00400000;
constexpr uint32_t GAO_ID_BIT24     = 0x01000000;

struct GaoInfo {
    bool        ok = false;
    uint32_t    version = 0, editor_flags = 0, identity = 0, name_size = 0;
    std::string name;                 // ascii+replace decoded (UTF-8)

    // 68-byte global matrix (16 floats + type u32) at header+name+10.
    bool                  gmat_present = false;
    uint32_t              gmat_type = 0;
    std::vector<uint8_t>  gmat_raw;    // 68 bytes for CRC

    // Visual block (identity & 0x4000).
    bool     vis_flag = false, vis_read = false;
    uint32_t gro_key = 0, grm_key = 0, vis_size = 0;

    // Hierarchy block (identity & 0x400000).
    bool                  hier_flag = false, hier_read = false, lmat_present = false;
    uint32_t              father_key = 0;
    std::vector<uint8_t>  lmat_raw;    // 68 bytes for CRC

    // Gizmo pointers (identity & 0x200000 && & 0x1000000).
    std::vector<uint32_t> gizmo_flat;  // (gao_key, mat_id) pairs
};

GaoInfo parse_gao_full(const uint8_t* d, size_t n);

// Parse a GAO *record payload* (begins with the ".gao" tag); strips it and
// delegates to parse_gao_full. ok == false if the tag is absent.
GaoInfo parse_gao_record(const uint8_t* d, size_t n);

// The actual GEO key(s) a GAO's gro_key draws. A gro_key may name a geometry
// GROUP / LOD list (a non-gro_type-1 sub-entry that lists member GEO keys in its
// payload) rather than a GEO directly. Returns {gro_key} for a direct GEO
// (gro_type 1) or unknown key; otherwise the member GEO keys found in the
// group's payload that are themselves gro_type-1 in `subs_by_key`. Pass a
// precomputed `geo_keys` (the gro_type-1 key set) to avoid rebuilding it.
// ── write helpers (object_placer's move/extend primitives) ────────────────
// All offsets mirror gao.py: header 16 + name_size + 10, then the 68-byte
// global matrix, then the BV block (48 with the OBBox identity bit, else 24).

// Byte offset of the 16-float global_matrix slot, or -1 (payload too short).
long long global_matrix_offset(const uint8_t* d, size_t n);

// Byte offset of the bounding-volume block, or -1.
long long obbox_offset(const uint8_t* d, size_t n);

// Local-space AABB from the BV block. ok == false when the payload doesn't
// parse or the BV is sphere-encoded (min > max on any axis).
struct ObboxBounds {
    bool ok = false;
    std::array<double, 3> mn{}, mx{};
};
ObboxBounds obbox_local_bounds(const uint8_t* d, size_t n);

// Enlarge the GAO's 48-byte OBBox so it contains the given world AABB (the
// component-wise union, written to all four 12-byte slots). Empty = the Python
// None (no OBBox-extended block / sphere BV). A covered AABB is a no-op that
// still returns the (unchanged) payload, like the Python.
std::vector<uint8_t> extend_obbox_to_include(const uint8_t* d, size_t n,
                                             const std::array<double, 3>& world_min,
                                             const std::array<double, 3>& world_max);

// Replace the 16-float column-major global_matrix. Empty = the Python None.
std::vector<uint8_t> write_global_matrix(const uint8_t* d, size_t n,
                                         const std::array<double, 16>& col_major);

std::vector<uint32_t> geo_group_members(
    uint32_t gro_key,
    const std::unordered_map<uint32_t, const SubEntry*>& subs_by_key,
    const std::unordered_set<uint32_t>* geo_keys = nullptr);

}  // namespace jade
