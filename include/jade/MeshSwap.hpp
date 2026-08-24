// MeshSwap.hpp — mesh_swap's element -> material/texture resolution (read side).
//
// Port of io_ops/mesh_swap.py's in-bin resolution slice: which texture each GEO
// element (sub-mesh) draws, via the GAO -> grm (multi-material container) ->
// sub-material -> texture chain. This is the single source of truth shared by
// the Mesh Swap material table and the Asset Browser 3D preview; critically
// there is NO "guess the multi-material" fallback — the owning GAO names the
// exact container (a wow entry can hold several multi-materials, and using any
// other one paints elements with a sibling mesh's textures).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "jade/Geometry.hpp"
#include "jade/Gltf.hpp"
#include "jade/SubEntry.hpp"

namespace jade {

// Map a GEO element's matId to a valid container slot the way the ENGINE does
// (POP3 FUN_004473b0): out-of-range clamps to the LAST slot (n_sub-1), not 0 and
// not "no material" (shipped geos rely on this). 0 for an empty container.
// matId is widened so a huge unsigned matId clamps high, like Python.
int64_t clamp_matid(int64_t matid, int64_t n_sub);

// Positional sub-material keys for a GAO's grm sub-entry. A real multi-material
// CONTAINER (gro_type 4) yields its key array; a directly-named SINGLE material
// (gro_type 3/5 — common in WW) is a one-slot list of its own key so every
// element clamps onto that one real material. Empty for nullptr / key 0.
std::vector<uint32_t> grm_sub_material_keys(const SubEntry* grm_sub);

// The grm_key (multi-material) of the GAO whose visual names `geo_key` —
// directly, or via a geometry GROUP / LOD list containing it. Returns true and
// sets `out` when found (the found grm_key may itself be 0/0xFFFFFFFF; callers
// look it up in by_key like the Python does).
bool owning_grm_key(const std::vector<SubEntry>& subs, uint32_t geo_key,
                    uint32_t& out);

// Per-element resolved texture key for `geo` (aligned with its element list);
// an entry is {false, 0} when unresolvable (the Python None).
struct ResolvedTex {
    bool     ok = false;
    uint32_t key = 0;
};
std::vector<ResolvedTex> resolve_element_texture_keys(
    const std::vector<SubEntry>& subs, const GeoInfo& geo, uint32_t geo_key);

// ── host-GAO RLI rewrite (the swap's lighting-consistency piece) ───────────

// The first .gao sub-entry that DRAWS geo_key (visual gro_key == geo_key, or a
// geometry group listing it). This GAO owns the mesh's RLI lighting and the
// gizmo_ptrs naming its joints. nullptr when no host GAO is present.
const SubEntry* host_gao_sub(const std::vector<SubEntry>& subs, uint32_t geo_key);

// One (r,g,b,a) colour; only r,g,b are written by the rewrite (alpha 0xFE).
using Rgba4 = std::array<double, 4>;

// For a static mesh whose colours live in the host GAO's RLI: the ORIGINAL
// per-vertex lighting remapped onto new_vertices BY POSITION (3-decimal
// rounding, matching Python round()), so "preserve original" survives a GLB
// UV-split growing the vertex count. ok=false when no host-GAO RLI exists.
struct OrigRliColors {
    bool ok = false;
    std::vector<Rgba4> colors;   // length == new_vertices.size() when ok
};
OrigRliColors original_rli_colors(const std::vector<SubEntry>& subs, uint32_t geo_key,
                                  const GeoInfo& orig_geo,
                                  const std::vector<std::array<double, 3>>& new_vertices);

// Rewrite a host GAO's per-instance vertex-colour table (dul_VertexColors) after
// a mesh swap: splice a fresh primary table of new_colors.size() BGRA entries
// (alpha 0xFE) over the old orig_nb_pts-entry one, then refresh — and, when the
// cooked-VB count changed, REBUILD/resize — the expanded extra block via the
// expanded->base map of `geo` (the NEW GEO body; pass nullptr for the legacy 1:1
// fast path). Returns the new GAO bytes, or EMPTY (Python None) when this GAO
// has no instance table for geo_key.
std::vector<uint8_t> rewrite_gao_instance_colors(
    const uint8_t* gao, size_t n, uint32_t geo_key, uint32_t orig_nb_pts,
    const std::vector<Rgba4>& new_colors,
    const uint8_t* geo = nullptr, size_t geo_n = 0);

// Advisory skin compatibility report (validate_glb_skin). This does not block
// a swap; the GUI lets the user continue after reviewing warnings.
struct SkinNameMatch {
    std::string glb_name;
    std::string original_name;
};
struct SkinValidationResult {
    bool ok = false;
    std::string error;
    bool glb_has_skin = false;
    uint32_t glb_joint_count = 0;
    std::vector<std::string> glb_joint_names;
    uint32_t orig_bone_count = 0;
    std::vector<std::string> orig_bone_names;
    std::vector<SkinNameMatch> name_matches;
    std::vector<std::string> warnings;
};

SkinValidationResult validate_glb_skin(
    const std::vector<uint8_t>& decompressed_entry, uint32_t geo_key,
    const std::vector<uint8_t>& glb_bytes);
SkinValidationResult validate_glb_skin(
    const std::string& glb_path, const std::string& bf_path,
    uint32_t entry_idx, uint32_t geo_key);

// ── swap_mesh_in_bf (the end-to-end op) ────────────────────────────────────

// The geometry-payload sub-entry for geo_key: a key may appear as a 52-byte
// stub plus the real entry — pick the largest parseable gro_type-1 candidate.
const SubEntry* pick_geo_sub(const std::vector<SubEntry>& subs, uint32_t geo_key);

// Replace one sub-entry's payload inside a decompressed entry, updating its
// size field (at cookie-4; new size = 4 + payload length). `sub_offset` is the
// cookie position as reported by walk_sub_entries.
std::vector<uint8_t> splice_sub_entry(const std::vector<uint8_t>& dec,
                                      size_t sub_offset, uint32_t sub_size,
                                      const std::vector<uint8_t>& new_payload);

// _trailing_strategy_for: true = 'shipped_skinned' (skinned mesh on a PC
// SoT/WW/T2T archive per gameprofiles detection), false = 'legacy'.
bool shipped_skinned_strategy(const std::string& bf_path, bool has_skin);

// _build_geo_payload's vb_magic derivation: preserve the original cooked-VB
// tag (2/5/6/8; anything else -> 2) so the engine keeps its draw path.
uint32_t vb_magic_of(const uint8_t* raw, size_t n);

// A GEO-retained skin -> the glTF builder's skin shape (weight = pond/65535.0;
// _serialize_skin's repack restores the pond exactly).
gltf::GltfSkin geo_skin_to_gltf(const GeoInfo& geo);

// Result of swap_mesh_in_bf — the Python stats dict. `compressed_size` and
// `bf_entry_pos` are NOT comparable with the Python op (it compresses with
// LZO1X-999; we use miniLZO 1X-1) — everything else is.
struct SwapStats {
    bool        ok = false;
    std::string error;
    uint32_t geo_key = 0;
    uint32_t orig_points = 0, orig_uvs = 0, orig_tris = 0;
    size_t   orig_payload = 0;
    uint32_t new_points = 0, new_uvs = 0, new_tris = 0;
    size_t   new_payload = 0;
    size_t   decompressed_size = 0, compressed_size = 0;
    uint64_t bf_entry_pos = 0;
    bool     had_skin = false;            // the GLB carried a skin
    uint32_t wrote_colors = 0;
    std::string color_source;             // glb/original/dropped/none/original_rli
    uint32_t gao_color_updates = 0;
    bool     orig_had_colors = false;
};

// Replace .geo `geo_key` inside BF entry `entry_idx` with the first mesh from
// `glb_path` (port of swap_mesh_in_bf): rebuild the GEO payload with the
// original's header flags / cooked-VB magic / trailing strategy, apply the
// vertex-colour policy (+ the original-GAO-RLI fallback), splice it in, rewrite
// each host GAO's instance colour table, recompress and write back (in place or
// appended), and flush size.grs. On error `ok == false` with `error` set
// (mirrors the Python exceptions).
SwapStats swap_mesh_in_bf(const std::string& bf_path, uint32_t entry_idx,
                          uint32_t geo_key, const std::string& glb_path,
                          bool import_vertex_colors = false, bool backup = true);

}  // namespace jade
