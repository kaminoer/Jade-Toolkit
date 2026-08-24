// Material.hpp — Jade material parser (read path).
//
// Port of jade_explorer/core/material.py. Three forms, by gro_type:
//   3 = single (32B), 5 = multitexture, 4 = multi (sub-material key container).
// The multi key array is POSITIONAL and uncapped (GEO element matIds index it
// directly, often past 64), so every slot is preserved including 0/0xFFFFFFFF
// sentinels.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jade {

constexpr int GRO_TYPE_MAT_SINGLE       = 3;
constexpr int GRO_TYPE_MAT_MULTI        = 4;
constexpr int GRO_TYPE_MAT_MULTITEXTURE = 5;

struct MatInfo {
    bool ok   = false;
    int  type = -1;          // 0 single, 1 multitexture, 2 multi

    // single + multitexture
    uint32_t ambient = 0, diffuse = 0, specular = 0;
    uint32_t opac_bits = 0;          // raw bits of the opacity float (@16)
    uint32_t flags = 0, validate_mask = 0;
    uint32_t texture_key = 0;        // single: tex_id (@24); multitex: first_level
    std::vector<uint32_t> texture_keys;
    // single only
    uint32_t sexp_bits = 0;          // raw bits of spec_exp float (@12)
    // multitexture only
    uint32_t sexpraw = 0;            // original u32 @12 (top bit = version flag)
    bool     has_version = false;
    uint32_t hdr_size = 0;

    // multi
    uint32_t n_sub = 0;
    std::vector<uint32_t> sub_material_keys;
};

MatInfo parse_material(const uint8_t* d, size_t n, int gro_type);

// Texture key a (sub-)material points at via its kind-dependent offset, or 0.
uint32_t resolve_texture_key(const uint8_t* d, size_t n);

// ── leaf/sub-material read helpers (payload's first u32 is a kind 4-9) ──

// Uniform 28-byte colour/lighting header (the CORRECT read: colours at +4/+8/+12,
// ul_Flags at +24). ok == false for a non-leaf / unrecognised material.
struct MatHeader {
    bool     ok = false;
    uint32_t kind = 0, ambient = 0, diffuse = 0, specular = 0;
    uint32_t spec_exp_raw = 0, opacity_raw = 0;  // raw bits (often sentinels)
};
MatHeader parse_material_header(const uint8_t* d, size_t n);

// Offset of the ul_Flags word the ENGINE reads for texture `layer`: the
// per-level word (34 + layer*stride) for multitexture kinds 8/9, the header word
// at offset 24 for single kinds 4-7. -1 for a non-material / too-short payload.
long long render_ulflags_offset(const uint8_t* d, size_t n, uint32_t layer = 0);

// Alias of render_ulflags_offset (the header word at 24 is ignored by the engine
// for kinds 8/9, so this is the offset to read/write for an in-game render change).
long long material_ulflags_offset(const uint8_t* d, size_t n, uint32_t layer = 0);

// Decoded base render state. ok == false for a non-material.
struct MatRenderFlags {
    bool        ok = false;
    uint32_t    ul_flags = 0;
    int         blend = 0;     std::string blend_name;
    int         colorop = 0;   std::string colorop_name;
    bool        alpha_test = false;
    int         alpha_thresh = 0;
    std::vector<std::string> flags;   // misc flag-bit names set
    std::string mode;                 // short label (Opaque / Alpha-test / ...)
};

// Decode a raw 32-bit ul_Flags value into the render-flags fields.
MatRenderFlags decode_render_flags(uint32_t v);

// Read + decode the engine-consulted ul_Flags for texture `layer`.
MatRenderFlags parse_material_render_flags(const uint8_t* d, size_t n, uint32_t layer = 0);

// All texture-layer key offsets (base first), including empty slots. Kinds 8/9
// chain layers at base + i*stride; others are single-layer.
std::vector<size_t> texture_layer_offsets(const uint8_t* d, size_t n);

// Offset of layer `layer`'s key (0 = base), or -1.
long long texture_layer_offset(const uint8_t* d, size_t n, uint32_t layer);

// Every non-empty texture layer: (layer_index, payload_offset, key).
struct TexLayer { uint32_t layer_index; size_t offset; uint32_t key; };
std::vector<TexLayer> resolve_texture_keys(const uint8_t* d, size_t n);

// ── write helpers ──

// Copy with texture `layer`'s key rewritten; empty on failure.
std::vector<uint8_t> set_texture_key(const uint8_t* d, size_t n, uint32_t new_key,
                                     uint32_t layer = 0);

// Offset of sub_material_keys[slot] in a MAT_tdst_Multi container, or -1.
long long multi_slot_offset(const uint8_t* d, size_t n, uint32_t slot);

// Copy with a multi container's positional `slot` rewritten; empty on failure.
std::vector<uint8_t> set_multi_sub_key(const uint8_t* d, size_t n, uint32_t slot,
                                       uint32_t new_key);

// Copy with `new_key` appended as a new slot (n_sub += 1); empty on failure.
std::vector<uint8_t> append_multi_sub_key(const uint8_t* d, size_t n, uint32_t new_key);

// ARGB u32 -> normalized RGBA, matching material.argb_to_rgba().
std::array<double, 4> argb_to_rgba(uint32_t argb);

}  // namespace jade
