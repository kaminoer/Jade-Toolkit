// Collision.hpp — Jade triangle COB (COL_ZONE_TRIANGLES) parser (read path).
//
// Port of jade_explorer/core/collision.py parse_cob + looks_like_cob_sub.
// Layout (little-endian):
//   u8 type(==5) u8 flag
//   u32 n_verts; n_verts * vec3f                 (vertices, owning GAO local frame)
//   u32 n_faces; n_faces * vec3f                 (per-face normals)
//   u32 n_elems; repeat: u16 n_tri,u8 design,u8 flag,i32 material, n_tri*(3xu16)
//   n_faces * (3xu16)                            (edge-proximity table)
//   <trailing bytes>                             (count + optional climb/OK3)
// The per-face arrays are indexed by the FLAT face order (concatenated element
// triangle lists); the parse requires sum(n_tri) == n_faces, which also acts as
// a discriminator so non-COB gro_type-5 resources are rejected.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jade/SubEntry.hpp"

namespace jade {

constexpr uint8_t COL_ZONE_TRIANGLES = 5;

struct CobElement {
    uint16_t n_tri    = 0;
    uint8_t  design   = 0;
    uint8_t  flag     = 0;
    int32_t  material = 0;
    std::vector<uint16_t> faces;  // 3 * n_tri vertex indices
};

struct CobInfo {
    bool    ok    = false;
    uint8_t type  = 0;
    uint8_t flag  = 0;
    uint32_t n_verts = 0, n_faces = 0, n_elems = 0, total_tris = 0;
    std::vector<float>       verts;      // 3 * n_verts
    std::vector<float>       normals;    // 3 * n_faces
    std::vector<CobElement>  elements;
    std::vector<uint16_t>    proximity;  // 3 * n_faces
    std::vector<uint8_t>     trailing;
};

// Parse a triangle COB payload. ok == false mirrors the Python None (not a
// shape-5 triangle COB, bounds overrun, or sum(n_tri) != n_faces).
CobInfo parse_cob(const uint8_t* d, size_t n);

// Inverse of parse_cob: re-emit the COB payload bytes. Faithful round-trip —
// serialize_cob(parse_cob(p)) == p for every real triangle COB. Counts come from
// the vectors (verts.size()/3, normals.size()/3, elements.size(), per-element
// faces.size()/3), mirroring the Python which writes len(...) not the parsed n_*.
std::vector<uint8_t> serialize_cob(const CobInfo& c);

// Locator matching looks_like_cob_sub: no ASCII ext, >=10 bytes of full content
// ([gro_type:u32][payload]), shape byte (payload[0]) in {1,2,3,5}.
bool looks_like_cob(bool ext_empty, const uint8_t* payload, size_t payload_len);

// ── authoring (the COB builder suite) ──────────────────────────────────────

constexpr uint8_t  COL_C_Cob_OK3 = 0x08;
constexpr uint8_t  COL_C_Cob_GameMat = 0x01;
constexpr uint8_t  COL_C_Cob_Updated = 0x80;
constexpr uint8_t  COL_COB_GAMEMAT_FLAG = COL_C_Cob_GameMat;
constexpr int32_t  DEFAULT_COB_MATERIAL_ID = 65;
constexpr uint8_t  DEFAULT_COB_ELEMENT_DESIGN = 1;
constexpr int32_t  LEDGE_TOP_MATERIAL_ID = 67;
constexpr int32_t  LEDGE_FLOOR_MATERIAL_ID = 168;
constexpr int32_t  LEDGE_WALL_MATERIAL_ID = 167;  // legacy Python alias
constexpr uint8_t  CLIMBABLE_COB_ELEMENT_DESIGN = 0;
constexpr uint32_t DEFAULT_COB_GAMEMAT_KEY = 0xCC000228u;
constexpr uint32_t COLMAP_COMPACT_HEADER = 0x0000FF01u;

// Per-triangle edge-neighbour table: (n_edge12, n_edge13, n_edge23) per face,
// 0xFFFF for boundary edges. Slot order is load-bearing (Edge12/Edge13/Edge23).
std::vector<std::array<uint16_t, 3>> triangle_proximity(
    const std::vector<std::array<uint16_t, 3>>& faces);

// A face-group override (secondary element) + the profile recipe.
struct CobElementOverride {
    std::vector<int> face_indices;
    int32_t material_id = 0;
    uint8_t design = 0;
    uint8_t flag = 0;
};
struct CobProfile {
    std::string name = "simple_box";
    uint8_t shape_type = COL_ZONE_TRIANGLES;
    int32_t material_id = DEFAULT_COB_MATERIAL_ID;
    uint8_t element_design = DEFAULT_COB_ELEMENT_DESIGN;
    uint8_t element_flag = 0;
    std::string geometry = "closed_box";     // closed_box | open_front_box
    std::vector<CobElementOverride> element_overrides;
};
// The registered profiles: "simple_box" (default; ""/unset too),
// "ledge_openbox" (verified-grabbable open-front box, mat 67 / design 0).
// Unknown names set ok=false (the Python ValueError).
struct CobProfileLookup { bool ok = false; CobProfile profile; };
CobProfileLookup get_cob_profile(const std::string& name);

// Per-game legacy ledge recipe. Python currently uses the same calibrated
// (67,168) pair for SoT/WW/WW_PS2/T2T and for unknown game codes.
CobProfile make_ledge_profile_for_game(const std::string& game_code);

// Return the default GameMaterial key only when it exists in this bin.
uint32_t pick_cob_gamemat_key(
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key);

// COB payload bytes for an axis-aligned box in the GAO's object space.
// Empty = the Python ValueError (bad profile geometry / overrides).
std::vector<uint8_t> build_cob_triangle_box(
    const std::array<double, 3>& lb_min, const std::array<double, 3>& lb_max,
    uint32_t gmat_key = DEFAULT_COB_GAMEMAT_KEY,
    const CobProfile& profile = CobProfile{});

// Append an oriented box (8 verts; 12 tris, or 10 with open_minus_z) as a new
// element of an existing triangle COB. Clears the OK3 flag, preserves prior
// toolkit climb-edge records (count*52 signature), optionally emits 4 new
// climb-edge records on the +Z perimeter. Positioning: `world_translation`
// (legacy axis-aligned) or `corner_transform` (math 4x4, column-major flat —
// supersedes the translation when non-null). Empty = the Python ValueError.
std::vector<uint8_t> extend_cob_triangle_box(
    const uint8_t* existing, size_t n,
    const std::array<double, 3>& lb_min, const std::array<double, 3>& lb_max,
    const std::array<double, 3>& world_translation,
    const std::array<double, 16>* corner_transform,
    int32_t material_id = DEFAULT_COB_MATERIAL_ID,
    uint8_t element_design = DEFAULT_COB_ELEMENT_DESIGN,
    uint8_t element_flag = 0,
    bool open_minus_z = false,
    bool include_climb_edges = false);

// compute_face_normals: per-face geometric normals following the triangles'
// winding (cross(v1-v0, v2-v0), normalised); degenerate faces fall back +Y.
std::vector<std::array<double, 3>> compute_face_normals(
    const std::vector<std::array<double, 3>>& verts,
    const std::vector<std::array<uint32_t, 3>>& faces);

// extend_cob_triangle_mesh: append an arbitrary triangle mesh as ONE new
// element of a triangle COB — shape-accurate collision (a corridor keeps its
// doorway) vs the box variant's sealed OBBox. verts are in the host COB's
// local frame; faces index into them (out-of-range / degenerate faces are
// dropped like the Python). New triangles inherit the given material/design/
// flag; their adjacency is computed among themselves and shifted into the
// global face space; OK3 is cleared so the engine linear-scans. Built on
// parse_cob/serialize_cob so existing elements round-trip exactly.
// Empty = the Python ValueError (unparseable host / no usable triangles).
std::vector<uint8_t> extend_cob_triangle_mesh(
    const uint8_t* existing, size_t n,
    const std::vector<std::array<double, 3>>& verts_local,
    const std::vector<std::array<uint32_t, 3>>& faces,
    int32_t material_id, uint8_t element_design, uint8_t element_flag = 0,
    bool clear_ok3 = true);

// Same operation with caller-supplied per-face normals. The Python accepts
// normals verbatim (including a mismatched count), so this overload does too.
std::vector<uint8_t> extend_cob_triangle_mesh(
    const uint8_t* existing, size_t n,
    const std::vector<std::array<double, 3>>& verts_local,
    const std::vector<std::array<uint32_t, 3>>& faces,
    const std::vector<std::array<double, 3>>& normals,
    int32_t material_id, uint8_t element_design, uint8_t element_flag = 0,
    bool clear_ok3 = true);

// Flat element face order (the index space used by normals/proximity).
std::vector<std::array<uint16_t, 3>> cob_flat_faces(const CobInfo& cob);

// Largest COB-like triangle resource in the bin, or nullptr. This deliberately
// mirrors Python's lightweight heuristic and does not require parse_cob().
const SubEntry* find_room_master_cob(const std::vector<SubEntry>& subs);

// Compact ColMap and generic sub-entry authoring helpers.
std::vector<uint8_t> build_colmap_compact_payload(uint32_t cob_key);
std::vector<uint8_t> make_sub_entry(
    uint32_t key, uint32_t gro_type, const std::vector<uint8_t>& payload);
std::vector<uint8_t> make_sub_entry_ext(
    uint32_t key, const std::array<uint8_t, 4>& ext,
    const std::vector<uint8_t>& payload);
std::vector<uint8_t> make_cob_sub_entry(
    uint32_t cob_key, uint32_t gmat_key,
    const std::vector<uint8_t>& payload_body);
std::vector<uint8_t> make_colmap_compact_sub_entry(
    uint32_t colmap_key, uint32_t cob_key);

// Parsed-sub-entry collision classification and GAO reference discovery.
bool looks_like_cob_sub(const SubEntry* sub);
bool is_colmap_sub(
    const SubEntry* sub,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key);
std::vector<uint32_t> colmap_cob_keys(
    const SubEntry* colmap_sub,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key);
std::vector<std::pair<size_t, uint32_t>> gao_colmap_key_offsets(
    const std::vector<uint8_t>& gao_payload,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key);

}  // namespace jade
