// Jgao.hpp - portable GAO bundle container and GLB -> JGAO conversion.
//
// Ports object_placer.import_gao/export_gao's file format and both directions
// of jgao_converter, including its flat skeleton, material, texture, bone-name,
// vertex-colour, and template-rebuild behaviour.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jade {
struct SubEntry;
namespace jgao {

constexpr uint32_t VERSION = 1;

struct MaterialChild {
    uint32_t key = 0;
    uint32_t gro_type = 0;
    std::vector<uint8_t> data;
};

struct File {
    bool ok = false;
    std::string error;
    uint32_t version = VERSION;
    uint32_t gao_key = 0;
    uint32_t geo_key = 0;
    uint32_t mat_key = 0xFFFFFFFFu;
    std::vector<uint8_t> gao_data;
    std::vector<uint8_t> geo_data;
    std::vector<uint8_t> mat_data;
    std::vector<MaterialChild> mat_children;
};

// Parse/serialize the version-1 .jgao wire format. serialize writes the
// supplied version; glb_to_jgao always writes VERSION, matching Python.
File parse(const uint8_t* data, size_t size);
std::vector<uint8_t> serialize(const File& file);

struct ExportResult {
    bool ok = false;
    std::string error;
    std::string name;
    std::vector<uint8_t> jgao;
};

// Bundle a visual GAO and its referenced GEO/material children from one
// decompressed entry's sub-entry list (object_placer.export_gao).
ExportResult export_gao(const std::vector<SubEntry>& subs, uint32_t gao_key);

// Exact port of _adjust_material_for_elements for valid multi-material data.
void adjust_material_for_elements(std::vector<uint8_t>& mat_data,
                                  std::vector<MaterialChild>& mat_children,
                                  uint32_t n_new, uint32_t n_old);

struct ConvertResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> jgao;
    uint32_t vertices = 0;
    uint32_t faces = 0;
    uint32_t elements = 0;
    uint32_t geo_size = 0;
    uint32_t bones = 0;
};

// Rebuild the GEO inside a template JGAO from an edited GLB. The GAO, keys,
// and material payloads are retained; multi-material children are adjusted if
// the GLB primitive/element count changed.
ConvertResult glb_to_jgao(const uint8_t* glb_data, size_t glb_size,
                          const uint8_t* template_data, size_t template_size);

struct GlbResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> glb;
    uint32_t vertices = 0;
    uint32_t faces = 0;
    uint32_t elements = 0;
    uint32_t bones = 0;
    uint32_t materials = 0;
    uint32_t textures = 0;
};

// Convert a JGAO to the editable flat-armature GLB emitted by Python's
// jgao_to_glb. Optional sub-entry context resolves real bone names and embeds
// textures referenced by the JGAO's material children.
GlbResult jgao_to_glb(const uint8_t* jgao_data, size_t jgao_size,
                      const std::vector<SubEntry>* context = nullptr);

}  // namespace jgao
}  // namespace jade
