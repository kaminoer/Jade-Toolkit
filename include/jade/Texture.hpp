// Texture.hpp — Jade texture parser + decoders (read path).
//
// Port of jade_explorer/core/texture.py. Formats:
//   0 = BGRA, 1 = PAL8, 5 = DXT1, 6 = DXT3, 7 = DXT5, 11 = 4-bit grey.
//
// Decode-output parity with the live toolkit (which uses texture2ddecoder for
// DXT1/DXT5 but a hand-rolled path for DXT3) is reproduced exactly:
//   * DXT1/DXT5 use standard 565 bit-replication + floor interpolation
//     ((v<<3)|(v>>2), (2a+b)/3, ...), with idx3 = transparent black in the
//     c0<=c1 blocks (DXT1 only; DXT5 takes alpha from its alpha block).
//   * DXT3 uses the legacy *255//31 rounding + 4-bit explicit alpha.
// Verified byte-for-byte against the oracle by tests/run_golden.py.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace jade {

constexpr size_t   TEX_HEADER_SIZE = 44;
constexpr size_t   TEX_PIXDATA_OFF = 52;
constexpr uint32_t TEX_MAGIC_1     = 0xCAD01234u;
constexpr uint32_t TEX_MAGIC_2     = 0xC0DEC0DEu;

struct TexInfo {
    bool     valid          = false;
    uint32_t width          = 0;   // actual dims (govern pixel layout)
    uint32_t height         = 0;
    uint32_t logical_width  = 0;
    uint32_t logical_height = 0;
    uint32_t format         = 0;
    uint32_t version        = 0;
    uint32_t mip_count      = 0;
    size_t   pix_start      = 0;   // offset of pixel data within the source
    std::vector<uint8_t> prefix;   // 4-byte (fmt 1/5) or 8-byte (fmt 11 v>=4)
};

// True if the buffer begins with a Jade embedded-texture header.
bool is_texture_entry(const uint8_t* d, size_t n);

// Parse a texture header at the start of [d, d+n). `valid == false` on failure.
TexInfo parse_texture(const uint8_t* d, size_t n);

// Byte size of the base (largest) mip for a format/dims (0 if unknown).
size_t base_level_size(uint32_t fmt, uint32_t w, uint32_t h);

// True if `pixdata_len` can't even hold the base mip (a header-only stub).
bool is_placeholder(const TexInfo& ti, size_t pixdata_len);

// Decode a parsed texture to RGBA (h*w*4, row-major). Empty on failure.
// `palette` (1024 bytes) is used for PAL8; without it PAL8 decodes greyscale.
std::vector<uint8_t> decode_texture(const uint8_t* d, size_t n, const TexInfo& ti,
                                    const uint8_t* palette = nullptr,
                                    size_t pal_len = 0);

// Offsets of every embedded texture header inside a buffer (magic scan). Used
// by the golden harness to locate real textures without the sub-entry layer.
std::vector<size_t> find_texture_offsets(const uint8_t* d, size_t n);

// ── DDS I/O (the exporter's texture path; closes the DDS known-gap) ────────

// write_dds: RGBA -> DXT1-compressed DDS bytes (port of texture.write_dds).
std::vector<uint8_t> write_dds(const uint8_t* rgba, uint32_t w, uint32_t h);

// write_dds_raw: lossless for DXT sources (base-level blocks copied through),
// else decode -> uncompressed A8R8G8B8. Empty vector = the Python ValueError
// (undecodable). `d,n` = the texture sub-entry payload `ti` was parsed from.
std::vector<uint8_t> write_dds_raw(const uint8_t* d, size_t n, const TexInfo& ti,
                                   const uint8_t* palette = nullptr,
                                   size_t pal_len = 0);

// read_dds: DXT1/3/5 or 32-bpp uncompressed -> RGBA. ok=false = Python None.
struct DdsImage {
    bool ok = false;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
DdsImage read_dds(const uint8_t* d, size_t n);

// The 1024-byte palette a PAL8 texture references BY KEY (its 4-byte prefix);
// falls back to the entry's first palette; nullptr for non-PAL8 / none found.
// Port of texture.palette_for_texture (closes part of that known gap).
struct SubEntry;   // fwd (SubEntry.hpp)
std::vector<std::vector<uint8_t>> find_palettes(
    const std::vector<struct SubEntry>& subs);
const std::vector<uint8_t>* palette_for_texture(
    const TexInfo& ti, const std::vector<struct SubEntry>& subs);

// (r,g,b) 0-255 -> (r,g,b) colour transform.
using ColorFn = std::function<std::array<int, 3>(std::array<int, 3>)>;

// Apply `color_fn` to a texture's colours in place (same byte size), preserving
// each DXT colour block's c0>c1 vs c0<=c1 mode. DXT1/DXT3/DXT5 grade the colour
// endpoints; BGRA grades pixels; PAL8 / 4-bit are skipped. Returns empty (the
// Python None) for an unsupported format or when nothing changed.
std::vector<uint8_t> grade_texture(const uint8_t* d, size_t n, const ColorFn& color_fn);

// Encode an RGBA buffer (w*h*4 bytes, row-major) to BGRA bytes (swap R<->B),
// the exact inverse of the BGRA decode path. `rgba` must hold >= w*h*4 bytes.
std::vector<uint8_t> encode_bgra(const uint8_t* rgba, uint32_t w, uint32_t h);

// Encode an RGBA buffer (w*h*4) to DXT1 (BC1) bytes. Matches texture.encode_dxt1:
// the vendored quicktex BC1 encoder (level 5, FourColor, Ideal) for fully-opaque
// input, falling back to a hand-rolled punch-through encoder when any pixel has
// alpha < 128 (quicktex ignores source alpha). w,h should be multiples of 4.
std::vector<uint8_t> encode_dxt1(const uint8_t* rgba, uint32_t w, uint32_t h);

// Encode an RGBA buffer (w*h*4) to DXT5 (BC3) bytes via the vendored quicktex BC3
// encoder (level 5, Ideal) — matches texture.encode_dxt5.
std::vector<uint8_t> encode_dxt5(const uint8_t* rgba, uint32_t w, uint32_t h);

// Generate base + top-down mip levels from RGBA, matching
// core/texture.py::generate_mipmaps. `pixdata_len < 0` leaves the natural
// result size; otherwise the result is padded/truncated to that exact size.
// PAL8 deliberately preserves the Python grayscale-as-index behavior.
std::vector<uint8_t> generate_mipmaps(const uint8_t* rgba, uint32_t w,
                                      uint32_t h, uint32_t fmt,
                                      uint32_t mip_count,
                                      int64_t pixdata_len = -1);

}  // namespace jade
