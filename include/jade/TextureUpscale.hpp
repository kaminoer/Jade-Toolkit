// TextureUpscale.hpp — texture replacement (io_ops/texture_upscale.py).
//
// Replace a texture inside a BF entry with a higher-resolution image at its
// native resolution (no resampling; mip_count is set to 0 — the mip pipeline
// with PIL-Lanczos belongs to lightmap_bake/patcher and is not ported here).
// The native API accepts either decoded RGBA + dimensions or an image path;
// the path overload mirrors Python's archive-validation/decode ordering.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "jade/SubEntry.hpp"

namespace jade {
namespace texup {

using UpscaleLogFn = std::function<void(const std::string&)>;

constexpr size_t HDR_FMT_OFF = 36;
constexpr size_t HDR_W_OFF = 40;
constexpr size_t HDR_H_OFF = 44;
constexpr size_t HDR_MIPCOUNT_OFF = 48;

// _pick_texture_sub: the largest texture-format sub-entry for the key (stubs lose).
const SubEntry* pick_texture_sub(const std::vector<SubEntry>& subs, uint32_t tex_key);

// _sync_texture_stub: patch a stub header's fmt/dims (+ zero a stale fmt-1 prefix).
std::vector<uint8_t> sync_texture_stub(const std::vector<uint8_t>& data,
                                       uint32_t new_w, uint32_t new_h, uint32_t new_fmt);

// build_upscaled_payload: header preserved except fmt/w/h/mip(=0); pixels
// re-encoded as BGRA(0)/DXT1(5, +4-byte prefix)/DXT5(7). target_format 0xFFFFFFFF
// = auto (the Python None: 7 for PAL8/4-bit sources, else keep). Empty result +
// error = the Python ValueError.
struct UpscalePayload {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> payload;
};
UpscalePayload build_upscaled_payload(const std::vector<uint8_t>& orig_payload,
                                      const uint8_t* rgba, uint32_t w, uint32_t h,
                                      uint32_t target_format = 0xFFFFFFFFu);

struct UpscaleStats {
    bool ok = false;
    std::string error;
    uint32_t tex_key = 0;
    uint32_t orig_logical_w = 0, orig_logical_h = 0;
    uint32_t orig_actual_w = 0, orig_actual_h = 0;
    uint32_t new_w = 0, new_h = 0;
    uint32_t orig_format = 0, new_format = 0;
    size_t   decompressed_size = 0, compressed_size = 0;   // compressed differs
    uint64_t bf_entry_pos = 0;                             //   from Python (LZO level)
    uint32_t stubs_synced = 0;
};

// upscale_texture_in_bf: splice the new payload over the data-bearing
// occurrence, sync every stub occurrence's header (highest offset first),
// recompress + write back. `rgba` is w*h*4 (the PIL-decoded image).
UpscaleStats upscale_texture_in_bf(const std::string& bf_path, uint32_t entry_idx,
                                   uint32_t tex_key, const uint8_t* rgba,
                                   uint32_t w, uint32_t h,
                                   uint32_t target_format = 0xFFFFFFFFu,
                                   bool backup = true,
                                   UpscaleLogFn log = {});

// Python's public image-path surface. Decoding happens after archive/entry
// validation and before decompression, at the same stage as Image.open().
UpscaleStats upscale_texture_in_bf(const std::string& bf_path,
                                   uint32_t entry_idx, uint32_t tex_key,
                                   const std::string& image_path,
                                   uint32_t target_format = 0xFFFFFFFFu,
                                   bool backup = true,
                                   UpscaleLogFn log = {});

// upscale_textures_in_bf: batch variant — one decompress/recompress pass for
// several textures in the same entry. A key with no texture sub is SKIPPED
// (not an error); a payload-build failure aborts the whole op unwritten.
struct BatchSpec {
    uint32_t tex_key = 0;
    const uint8_t* rgba = nullptr;
    uint32_t w = 0, h = 0;
    uint32_t target_format = 0xFFFFFFFFu;
    // When set, decode this image instead of consuming rgba/w/h.
    std::string image_path;
    std::string label;
};
struct BatchItem {
    uint32_t tex_key = 0;
    uint32_t new_w = 0, new_h = 0, new_format = 0;
};
struct BatchStats {
    bool ok = false;
    std::string error;
    std::vector<BatchItem> entries;   // applied specs, in order (skips absent)
    uint32_t skipped = 0;
    size_t   decompressed_size = 0, compressed_size = 0;
    uint64_t bf_entry_pos = 0;
};
BatchStats upscale_textures_in_bf(const std::string& bf_path, uint32_t entry_idx,
                                  const std::vector<BatchSpec>& specs,
                                  bool backup = true,
                                  UpscaleLogFn log = {});

}  // namespace texup
}  // namespace jade
