// PatcherModel.hpp — the patcher's MODEL half (io_ops/patcher.py
// _patch_model_glb and its dependency closure): replace one GEO sub-entry
// from a GLB.
//
// Two rebuild strategies, chosen by _glb_is_foreign:
//   * toolkit round-trip (rebuild_geo_binary): the GLB carries _JADE_VERT_ID /
//     jade_orig_vert_map markers proving it was exported from THIS geo — snap
//     its vertices back onto the original vertex array, keep the skin block
//     verbatim, re-derive UVs / faces / per-element partition, regenerate or
//     patch the cooked trailing block.
//   * foreign (rebuild_geo_foreign): write the GLB's own geometry verbatim and
//     build a fresh skin from JOINTS_0/WEIGHTS_0 through the bone-remap
//     machinery (auto name/geometric map + explicit overrides + drops +
//     rigid-bind fallbacks + opt-in auto-rig for skinless props).
//
// Byte-parity contract: every code path is validated against the REAL Python
// _patch_model_glb on real archives (tests/patchmodel_check.py).
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "jade/Geometry.hpp"
#include "jade/SubEntry.hpp"

namespace jade {
namespace patcher {

// ── _parse_glb_meshes ──────────────────────────────────────────────────────

// One parsed GLB mesh (patcher dict shape). Vertices are already baked
// through the node world transform (non-skinned meshes only) and axis-swapped
// into Jade's Z-up frame.
struct GlbPatchMesh {
    std::string name;
    std::string jade_key_str;          // extras jade_key when a string
    bool has_jade_key_int = false;     // extras jade_key when a number
    uint32_t jade_key_int = 0;
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<double, 3>> normals;
    std::vector<std::array<double, 2>> uvs;
    std::vector<std::array<uint32_t, 3>> faces;
    std::vector<int> face_elems;
    bool has_joints = false;           // len(joints) == len(vertices)
    std::vector<std::array<int, 4>> joints;
    bool has_weights = false;
    std::vector<std::array<double, 4>> weights;
    bool has_colors = false;
    std::vector<std::array<int, 4>> colors;
    std::vector<std::string> joint_names;
    std::vector<std::array<double, 16>> joint_ibms;   // col-major; empty = None
    bool has_orig_vert_map = false;
    std::vector<int> orig_vert_map;
    bool has_vert_id_map = false;
    std::vector<int> vert_id_map;
    double vert_id_max_frac = 0.0;
};

// Parse every mesh of a GLB file's bytes. Empty result mirrors the Python
// None (bad magic / missing chunks / no meshes).
std::vector<GlbPatchMesh> parse_glb_meshes(const std::vector<uint8_t>& glb);

// ── _glb_is_foreign ────────────────────────────────────────────────────────

struct ForeignCheck {
    bool foreign = false;
    std::string reason;                // set when foreign
};
ForeignCheck glb_is_foreign(const GlbPatchMesh& m, const GeoInfo& orig_geo,
                            bool has_target_key, uint32_t target_geo_key);

// ── _patch_model_glb ───────────────────────────────────────────────────────

struct PatchModelOptions {
    // GLB joint index -> original bone-list index (bone-remap UI output).
    std::map<int, int> bone_map;
    std::map<int, std::string> bone_map_source;   // joint -> user|name|geometric
    std::set<int> bone_drops;
    bool has_rigid_bind_bone = false;             // Python None vs int
    int rigid_bind_bone = 0;
    std::map<int, int> drop_targets;              // dropped joint -> orig bone
    bool auto_rig = false;
    bool diagnose_rest_pose = false;
    bool import_vertex_colors = false;
    bool keep_original_skin = false;
};

struct PatchModelResult {
    bool changed = false;              // false mirrors the Python None return
    std::vector<uint8_t> patched;      // full new entry bytes when changed
};

// Patch GEO `sub_key` inside decompressed entry `dec` from `glb_bytes`.
// Mirrors _patch_model_glb exactly, including the host-GAO RLI resize.
// Log lines match the Python log_fn text.
PatchModelResult patch_model_glb(const std::vector<uint8_t>& dec,
                                 uint32_t sub_key,
                                 const std::vector<uint8_t>& glb_bytes,
                                 const PatchModelOptions& opts,
                                 std::vector<std::string>& log);

// patcher._patch_scene_glb: apply every GLB mesh carrying a string
// extras.jade_key="0x..." to its matching GEO in one decompressed entry.
struct PatchSceneResult {
    bool changed = false;
    uint32_t patch_count = 0;
    std::vector<uint8_t> patched;
};
PatchSceneResult patch_scene_glb(const std::vector<uint8_t>& dec,
                                 const std::vector<uint8_t>& glb_bytes,
                                 const std::string& source_name,
                                 std::vector<std::string>& log);

// ── exposed internals (validation + the ReplaceMesh validator) ─────────────

// _resolve_skeleton_gizmos: gizmo bone names of the skeleton this GEO is
// skinned to (the GAO whose visual names it directly or through a geometry
// group; largest array fallback).
std::vector<std::string> resolve_skeleton_gizmos(
    const std::vector<SubEntry>& subs, bool has_geo_key, uint32_t geo_key);

// _norm_bone_name: canonical anatomical bone key (prefix/order-robust),
// after the user's ~/.jade_explorer/bone_aliases.json alias map.
std::string norm_bone_name(const std::string& s);

// _compute_bone_map: best-effort GLB-joint -> orig-bone map. Returns pairs
// (bone_map, source) keyed by joint; source is "name" or "geometric".
void compute_bone_map(const GlbPatchMesh& mesh, const GeoInfo& orig_geo,
                      const std::vector<std::string>& gizmo_names,
                      std::map<int, int>& bone_map_out,
                      std::map<int, std::string>& source_out);

// _gltf_to_jade_matrix / _bone_world_from_ibm (frame conversion + rest pose).
std::array<double, 16> gltf_to_jade_matrix_pub(const std::array<double, 16>& v);
std::array<double, 3> bone_world_from_ibm_pub(const std::array<double, 16>& m);

// ── mesh_swap.analyze_bone_mapping (the Bone Remap dialog's data) ──────────

struct JointStats {
    uint32_t vertex_count = 0;
    uint32_t dominant_count = 0;
    double weight_share = 0.0;
};

struct AnalyzeBoneResult {
    bool ok = false;
    std::string error;               // the Python exception text
    bool is_foreign = false;
    std::string foreign_reason;      // empty = None
    std::vector<std::string> glb_joint_names;
    std::vector<std::string> dp_bone_names;
    std::map<int, int> auto_map;
    std::map<int, std::string> auto_map_source;
    std::vector<JointStats> joint_stats;
    // (has, xyz) per joint/bone — None entries mirror the Python.
    std::vector<std::pair<bool, std::array<double, 3>>> joint_centroids;
    std::vector<std::pair<bool, std::array<double, 3>>> bone_centroids;
    double mesh_diagonal = 0.0;
    std::vector<std::pair<bool, std::array<double, 3>>> joint_rest_positions;
    std::vector<std::pair<bool, std::array<double, 3>>> bone_rest_positions;
    std::vector<double> orig_bone_weight_shares;
};

// Gather everything the bone-remap UI needs for one (entry, geo, GLB) triple.
AnalyzeBoneResult analyze_bone_mapping(const std::vector<uint8_t>& dec,
                                       uint32_t geo_key,
                                       const std::vector<uint8_t>& glb_bytes);

}  // namespace patcher
}  // namespace jade
