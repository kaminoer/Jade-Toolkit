// LevelBlender.hpp — standard (non-lightmap) level_blender.py port.
//
// Exports zones to GLB, reads baked vertex colours from GLB or a direct JSON
// dump, writes each matching GAO's RLI, and updates every district copy. The
// lightmap_atlas modes remain deliberately excluded.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <array>

#include "jade/BigFile.hpp"
#include "jade/SubEntry.hpp"

namespace jade {
namespace levelblend {

using LevelBlendLogFn = std::function<void(const std::string&)>;

// import_baked_rli (.glb or direct Blender .json dump path):
// {gao_key -> new same-size GAO payload} for
// the GAOs that changed. Does NOT touch the BF.
struct BakedImport {
    bool ok = false;
    std::string error;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> updates;  // dict order
    uint32_t misses = 0;      // unmatched verts filled with the last colour
};
BakedImport import_baked_rli(const std::string& glb_path,
                             const std::vector<SubEntry>& subs,
                             bool refresh_primary = true,
                             bool srgb_lighting = false,
                             LevelBlendLogFn log_fn = {});

// apply_baked_rli_to_bf: splice the updates into every bin whose name matches
// district_filter (substring; "" = all bins), same-size only, write at LZO
// level 9, flush size.grs. Returns (bins, gaos) touched.
struct ApplyStats {
    bool ok = false;
    std::string error;
    uint32_t bins = 0, gaos = 0;
};
ApplyStats apply_baked_rli_to_bf(
    const std::string& bf_path,
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& updates,
    const std::string& district_filter = "500",
    LevelBlendLogFn log_fn = {});

// ── export half (zone -> Blender GLB) ───────────────────────────────────────
// export_level_to_glb minus the lightmap_atlas modes (lightmaps are excluded
// from the roadmap). Textures embed as PNG (a stored-deflate writer — pixel-
// identical to the Python's PIL PNGs, different container bytes, so the
// harness compares GLBs SEMANTICALLY). texture_bf, when given, resolves
// textures shared from sibling bins (build_texture_resolver).
struct ExportManifest {
    bool ok = false;
    std::string error;
    uint32_t objects = 0;
    uint32_t lights = 0;
    std::vector<std::pair<std::string, uint32_t>> light_types;   // dict order
    std::vector<std::array<uint32_t, 3>> exported;   // (gao, geo, nb)
};
// Stored-deflate PNG writer (shared with the hierarchical GEO export).
std::vector<uint8_t> png_encode_rgba_pub(const uint8_t* rgba,
                                         uint32_t w, uint32_t h);

ExportManifest export_level_to_glb(const std::vector<SubEntry>& subs,
                                   const std::string& out_path,
                                   bool include_lights = true,
                                   const BigFile* texture_bf = nullptr,
                                   bool srgb_lighting = false,
                                   LevelBlendLogFn log_fn = {});

}  // namespace levelblend
}  // namespace jade
