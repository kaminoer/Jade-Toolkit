// GltfBuilder.hpp — the GLB WRITER (core/gltf_builder.py build_glb), mesh
// It covers both the exporter's single-GEO models path and its generic
// non-animated scene surface: multiple meshes, materials, images, nodes,
// extras, hierarchy, and skins.
//
// Faithful behaviours: UV-seam vertex splitting by (vert, uv) combo, Z-up ->
// Y-up, TEXCOORD_1 = the ORIGINAL Jade vertex id, JOINTS_0/WEIGHTS_0 from the
// GEO skin (top-4 influences, weight-normalized), jade_orig_vert_map mesh
// extras, compact json.dumps(separators=(',',':')) JSON.
#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

#include "jade/Geometry.hpp"
#include "jade/SubEntry.hpp"

namespace jade {
namespace gltfbuild {

// Build the exporter's single-GEO model GLB. `jade_key_hex` becomes the
// node/mesh extras {"jade_key": "0x..."}. Empty result = no verts/faces.
std::vector<uint8_t> build_geo_model_glb(const GeoInfo& geo,
                                         const std::string& mesh_name,
                                         const std::string& jade_key_hex,
                                         const std::string& scene_name);

// Generic, non-animated build_glb surface used by scene_export. These mirror
// the Python dictionaries closely while keeping the already-parsed GeoInfo as
// the mesh source. PNG images are embedded verbatim; texture/image ordering is
// therefore caller-controlled just like Python's images list.
struct SceneMaterial {
    std::string name;
    std::array<double, 4> base_color{{0.8, 0.8, 0.8, 1.0}};
    int texture_idx = -1;
    std::string extras_json;  // empty or a JSON object
};

struct SceneMesh {
    std::string name;
    GeoInfo geo;
    std::vector<uint32_t> material_indices;  // one value per filtered face
    std::string extras_json;                 // empty or a JSON object
    // Owning GAO gizmo_ptrs projected to gao_key by positional slot. This is
    // attached to Python's skin dict by skeleton.build_bone_nodes.
    std::vector<uint32_t> gizmo_gao_keys;
};

struct SceneNode {
    std::string name;
    int mesh_idx = -1;
    std::vector<uint32_t> children;
    bool has_matrix = false;
    std::array<double, 16> matrix{};  // Jade column-major
    std::string extras_json;          // empty or a JSON object
    bool is_bone = false;
    uint32_t jade_key = 0;
};

struct SceneInput {
    std::vector<SceneMesh> meshes;
    std::vector<SceneMaterial> materials;
    std::vector<std::vector<uint8_t>> images_png;
    std::vector<SceneNode> nodes;  // empty => one default node per mesh
    std::string scene_name = "JadeExport";
    std::string extras_json;       // empty or a JSON object
};

// Port of core.gltf_builder.build_glb excluding only animation channels.
std::vector<uint8_t> build_scene_glb(const SceneInput& scene);

// mesh_swap.export_geo_to_glb_hierarchical: one skinned GEO as a GLB with
// the REAL parent-child bone tree (recovered from the GAO father_key links),
// rest-posed from the inverse-bind matrices (local = IBM(parent) ·
// inverse(IBM(self)); roots use inverse(IBM(self)) so world · IBM = I and
// the mesh sits at its own vertices). Bone nodes carry bone_idx + jade_key
// extras; materials are one glTF material PER GEO ELEMENT resolved through
// the in-bin GAO -> grm chain, with decoded textures embedded as PNG.
// Static GEOs are rejected (use the flat exporter).
struct HierExportResult {
    bool ok = false;
    std::string error;               // the Python exception text
    std::vector<uint8_t> glb;
    uint32_t points = 0, tris = 0, bones = 0, roots = 0;
};
HierExportResult build_hierarchical_geo_glb(const std::vector<SubEntry>& subs,
                                            uint32_t geo_key);

}  // namespace gltfbuild
}  // namespace jade
