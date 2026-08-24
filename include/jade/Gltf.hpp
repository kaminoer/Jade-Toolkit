// Gltf.hpp — GLB (binary glTF 2.0) reader for mesh import.
//
// Port of the hand-rolled GLB parser in io_ops/jgao_converter.py. This header
// covers the byte-exact foundation: split the GLB container into its JSON + BIN
// chunks, and decode an accessor's data (honouring bufferView/accessor byte
// offsets and an explicit byteStride) into packed bytes. The mesh transform and
// GEO-payload layers build on top of this.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "jade/Json.hpp"

namespace jade {
namespace gltf {

constexpr uint32_t GLB_MAGIC     = 0x46546C67;  // "glTF"
constexpr uint32_t CHUNK_JSON    = 0x4E4F534A;  // "JSON"
constexpr uint32_t CHUNK_BIN     = 0x004E4942;  // "BIN\0"

// glTF componentType codes.
constexpr uint32_t COMP_I8  = 5120, COMP_U8 = 5121, COMP_I16 = 5122,
                   COMP_U16 = 5123, COMP_U32 = 5125, COMP_F32 = 5126;

struct GlbDoc {
    json::Value gltf;             // parsed JSON chunk
    std::vector<uint8_t> bin;     // BIN chunk bytes
};

// Split a GLB into its JSON + BIN chunks and parse the JSON. Throws on a bad
// magic or a missing JSON/BIN chunk (mirrors the Python ValueErrors).
GlbDoc parse_glb(const uint8_t* d, size_t n);

// One accessor's data, destrided into packed little-endian bytes
// (count * n_comps * component_size).
struct AccessorData {
    uint32_t comp_type = 0;   // 5120..5126
    uint32_t n_comps   = 0;   // 1 (SCALAR) .. 16 (MAT4)
    uint32_t count     = 0;
    std::vector<uint8_t> raw; // packed elements
};

// Byte size of one component of `comp_type` (1/2/4), or 0 if unknown.
size_t component_size(uint32_t comp_type);

// Read + destride accessor `acc_idx`. Throws if the accessor/bufferView is
// malformed or runs past the BIN chunk.
AccessorData read_accessor(const GlbDoc& doc, int acc_idx);

// World 4x4 matrix (row-major 16 doubles) per mesh index, from the glTF scene
// graph (node local transforms composed up the parent chain). Mirrors
// gltf_builder.gltf_mesh_world_matrices. Missing meshes default to identity.
std::map<int, std::array<double, 16>> mesh_world_matrices(const json::Value& gltf);

// Ordered mesh instances from the active glTF scene. Unlike the map above,
// this retains repeated nodes that reference the same mesh, as required by
// general static-model scene concatenation.
std::vector<std::pair<int, std::array<double, 16>>>
mesh_world_instances(const json::Value& gltf);

// Convert a COLOR_0 accessor to (r,g,b,a) 0-255 (float/normalized u8/u16).
std::vector<std::array<int, 4>> colors_to_rgba255(const AccessorData& acc);

struct GltfBone {
    int32_t bone_idx = 0;
    std::array<double, 16> bind_matrix{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    int32_t matrix_type = 0;
    std::vector<std::pair<uint32_t, double>> weights;   // (vertex index, weight)
};

struct GltfSkin {
    uint32_t flags = 0;
    std::vector<GltfBone> bones;
    // vertex -> list of (bone-list index, weight)
    std::map<uint32_t, std::vector<std::pair<uint32_t, double>>> vertex_weights;
};

// The mesh + skin extracted from a GLB, in Jade's Z-up frame — the C++ analogue
// of _parse_glb_mesh's returned dict. Positions/normals are float64 (post
// transform); uvs are float32 as read; colors are 0-255 (empty if none).
struct MeshData {
    bool ok = false;
    std::vector<std::array<double, 3>>   vertices;
    std::vector<std::array<double, 3>>   normals;
    std::vector<std::array<float, 2>>    uvs;
    bool                                 has_colors = false;
    std::vector<std::array<int, 4>>      colors;
    std::vector<std::array<uint32_t, 3>> faces;
    std::vector<std::array<uint32_t, 3>> face_uvs;
    std::vector<std::pair<uint32_t, int32_t>> elements;   // (nTri, matId)
    std::vector<uint32_t>                face_elems;
    bool                                 has_skin = false;
    GltfSkin                             skin;
};

// Parse a GLB's first mesh + skin for Jade GEO reconstruction (port of
// _parse_glb_mesh). Applies the mesh node's world transform, the normal matrix
// (inverse-transpose), normalization, and the glTF Y-up -> Jade Z-up axis swap.
MeshData parse_glb_mesh(const uint8_t* d, size_t n);

// Stride (52 or 64) of the skinned cooked-VB section-2 block in an ORIGINAL GEO
// payload, or 0 if not found. Stride 64 means the original carried a per-vertex
// TANGENT in the TEXCOORD1 slot @ offset 52 — i.e. the actor is normal-mapped.
// WW builds the tangent basis in-shader FROM that slot; rebuilding such an actor
// at stride 52 makes the engine read the next vertex's position as the tangent
// (normal map shades as surface bumps). Port of _orig_skinned_vb_stride.
uint32_t orig_skinned_vb_stride(const uint8_t* raw, size_t n);

// Options for build_geo_payload, taken from the ORIGINAL GEO being replaced.
struct GeoBuildOpts {
    uint32_t version = 8;
    uint32_t flags1  = 0x1009;
    uint32_t flags2  = 0;
    uint32_t vb_magic = 2;                 // preserved cooked-VB tag: 2/5/6/8
    bool     shipped_skinned = false;      // false = 'legacy' trailing
    // Original skinned VB stride (from orig_skinned_vb_stride). 64 => emit a
    // stride-64 cooked VB with computed per-vertex tangents.
    uint32_t orig_skinned_stride = 0;
    const std::vector<std::array<int, 4>>* colors = nullptr;  // ARGB source; null = no colour buffer
};

// Rebuild a complete Jade GEO binary payload from parsed mesh data (port of
// _build_geo_payload). Vertices/normals are written as float32 — the point where
// the float64 transform noise is quantized.
std::vector<uint8_t> build_geo_payload(const MeshData& md, const GeoBuildOpts& opts);

}  // namespace gltf
}  // namespace jade
