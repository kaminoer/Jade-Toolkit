// CharacterSwap.hpp — bundled character-pack application
// (io_ops/character_swap.py).
//
// A character pack = manifest + assets, all targeting ONE BF entry: mesh GLB
// swaps, texture replacements, and 48-byte null-stub disables, applied in
// sequence on a single decompressed copy and written once. Order matters:
// stubs first (shrinks), then meshes, then textures. As with texture_upscale,
// the native texture op takes decoded RGBA + dims (the Python op's only PIL
// use is the image decode — that stays host-side).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jade {
namespace charswap {

struct MeshOp {
    uint32_t key = 0;
    std::string glb_path;       // absolute (the manifest loader resolves)
    std::string label;          // basename, for logs
};

struct TexOp {
    uint32_t key = 0;
    // The regular manifest surface uses image_path. rgba/w/h remain available
    // to embedders that already decoded the image.
    std::string image_path;
    std::vector<uint8_t> rgba;  // w*h*4 decoded pixels (optional)
    uint32_t w = 0, h = 0;
    uint32_t target_format = 0xFFFFFFFFu;   // auto (the Python None)
    std::string label;
};

struct SwapSpec {
    uint32_t bf_entry = 0;
    std::vector<MeshOp> meshes;
    std::vector<TexOp> textures;
    std::vector<uint32_t> stubs;
};

struct SwapResult {
    bool ok = false;
    std::string error;
    uint32_t bf_entry = 0;
    uint32_t meshes_applied = 0;    // spec counts, like the Python (skips
    uint32_t textures_applied = 0;  //   inside an op category still count)
    uint32_t stubs_applied = 0;
    size_t decompressed_size = 0, compressed_size = 0;
    uint64_t bf_entry_pos = 0;
};

struct ManifestResult {
    bool ok = false;
    SwapSpec spec;
    std::string error;
};

using SwapLogFn = std::function<void(const std::string&)>;

// Load character_swap.py's unmodified mod.json schema. Asset paths are
// resolved relative to the manifest directory and checked for existence.
ManifestResult load_manifest(const std::string& manifest_path);

// apply_character_swap: every op in the spec against bf_path's entry.
// Mirrors the Python exactly, including the missing size.grs flush (the
// same upstream quirk texture_upscale carries).
SwapResult apply_character_swap(const std::string& bf_path, const SwapSpec& spec,
                                bool backup = true, SwapLogFn log = {});

// Manifest-path convenience matching Python's public apply_character_swap.
SwapResult apply_character_swap(const std::string& bf_path,
                                const std::string& manifest_path,
                                bool backup = true, SwapLogFn log = {});

}  // namespace charswap
}  // namespace jade
