// Material.cpp — implementation. Faithful port of core/material.py read path.
#include "jade/Material.hpp"

#include <algorithm>
#include <string>

namespace jade {

std::array<double, 4> argb_to_rgba(uint32_t argb) {
    return {{double((argb >> 16) & 0xff) / 255.0,
             double((argb >> 8) & 0xff) / 255.0,
             double(argb & 0xff) / 255.0,
             double((argb >> 24) & 0xff) / 255.0}};
}

namespace {

inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}

inline void put_le32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = static_cast<uint8_t>(x);       v[o + 1] = static_cast<uint8_t>(x >> 8);
    v[o + 2] = static_cast<uint8_t>(x >> 16); v[o + 3] = static_cast<uint8_t>(x >> 24);
}

// _MAT_KIND_TEX_OFFSET: payload offset of a (sub-)material's base texture key by
// kind; -1 for an unrecognised kind.
long long kind_tex_offset(uint32_t kind) {
    switch (kind) {
        case 4: case 5: case 6: return 40;
        case 7: return 42;
        case 8: return 50;
        case 9: return 63;
        default: return -1;
    }
}
// _MAT_KIND_LAYER_STRIDE: per-layer byte stride for multi-layer kinds; 0 = single.
long long kind_layer_stride(uint32_t kind) {
    switch (kind) { case 8: return 26; case 9: return 39; default: return 0; }
}

MatInfo parse_single(const uint8_t* d, size_t n) {
    MatInfo m;
    if (n < 32) return m;
    m.type = 0;
    m.ambient   = le32(d, 0);
    m.diffuse   = le32(d, 4);
    m.specular  = le32(d, 8);
    m.sexp_bits = le32(d, 12);
    m.opac_bits = le32(d, 16);
    m.flags     = le32(d, 20);
    m.texture_key   = le32(d, 24);
    m.validate_mask = le32(d, 28);
    if (m.texture_key && m.texture_key != 0xFFFFFFFFu)
        m.texture_keys.push_back(m.texture_key);
    m.ok = true;
    return m;
}

MatInfo parse_multitex(const uint8_t* d, size_t n) {
    MatInfo m;
    if (n < 32) return m;
    m.type = 1;
    m.ambient  = le32(d, 0);
    m.diffuse  = le32(d, 4);
    m.specular = le32(d, 8);
    m.sexpraw  = le32(d, 12);
    m.opac_bits = le32(d, 16);
    m.flags     = le32(d, 20);
    m.texture_key   = le32(d, 24);   // first_level
    m.validate_mask = le32(d, 28);
    m.has_version = (m.sexpraw & 0x80000000u) != 0;
    m.hdr_size = m.has_version ? 36 : 32;
    size_t tex_key_off = static_cast<size_t>(m.hdr_size) + 4;
    if (tex_key_off + 4 <= n) {
        uint32_t tk = le32(d, tex_key_off);
        uint32_t hi = tk >> 24;
        if (tk && tk != 0xFFFFFFFFu && (hi == 0x13 || hi == 0x33))
            m.texture_keys.push_back(tk);
    }
    m.ok = true;
    return m;
}

MatInfo parse_multi(const uint8_t* d, size_t n) {
    MatInfo m;
    if (n < 8) return m;
    m.type = 2;
    m.n_sub = le32(d, 4);
    size_t avail = (n > 8) ? (n - 8) / 4 : 0;
    size_t n_read = (m.n_sub <= 1000000u)
                        ? std::min(static_cast<size_t>(m.n_sub), avail)
                        : avail;
    m.sub_material_keys.reserve(n_read);
    for (size_t i = 0; i < n_read; ++i)
        m.sub_material_keys.push_back(le32(d, 8 + i * 4));
    m.ok = true;
    return m;
}

}  // namespace

MatInfo parse_material(const uint8_t* d, size_t n, int gro_type) {
    if (!d || n < 8) return MatInfo{};
    if (gro_type == GRO_TYPE_MAT_SINGLE)       return parse_single(d, n);
    if (gro_type == GRO_TYPE_MAT_MULTITEXTURE) return parse_multitex(d, n);
    if (gro_type == GRO_TYPE_MAT_MULTI)        return parse_multi(d, n);
    if (gro_type < 0 && n >= 32)               return parse_single(d, n);
    return MatInfo{};
}

uint32_t resolve_texture_key(const uint8_t* d, size_t n) {
    if (!d || n < 4) return 0;
    long long off = kind_tex_offset(le32(d, 0));
    if (off < 0 || static_cast<size_t>(off) + 4 > n) return 0;
    uint32_t key = le32(d, static_cast<size_t>(off));
    if (key == 0 || key == 0xFFFFFFFFu) return 0;
    return key;
}

MatHeader parse_material_header(const uint8_t* d, size_t n) {
    MatHeader h;
    if (!d || n < 24) return h;                 // _MAT_ULFLAGS_OFFSET
    uint32_t kind = le32(d, 0);
    if (kind_tex_offset(kind) < 0) return h;
    h.ok = true;
    h.kind = kind;
    h.ambient = le32(d, 4);
    h.diffuse = le32(d, 8);
    h.specular = le32(d, 12);
    h.spec_exp_raw = le32(d, 16);
    h.opacity_raw = le32(d, 20);
    return h;
}

long long render_ulflags_offset(const uint8_t* d, size_t n, uint32_t layer) {
    if (!d || n < 4) return -1;
    uint32_t kind = le32(d, 0);
    if (kind_tex_offset(kind) < 0) return -1;     // kind not in 4-9
    long long stride = kind_layer_stride(kind);   // 0 for single kinds 4-7
    // Single material: ignored-but-present header word at 24. Multitexture
    // (8/9): the per-LEVEL ul_Flags the engine actually reads, at 34+layer*stride.
    long long off = (stride == 0) ? 24
                                  : 34 + static_cast<long long>(layer) * stride;
    return (n >= static_cast<size_t>(off) + 4) ? off : -1;
}

long long material_ulflags_offset(const uint8_t* d, size_t n, uint32_t layer) {
    return render_ulflags_offset(d, n, layer);
}

MatRenderFlags decode_render_flags(uint32_t v) {
    MatRenderFlags r;
    r.ok = true;
    r.ul_flags = v;
    r.blend = (v >> 16) & 0xF;
    r.colorop = (v >> 12) & 0xF;
    r.alpha_test = (v & (1u << 4)) != 0;
    r.alpha_thresh = static_cast<int>(((v >> 24) & 0x3F) << 2);

    static const char* FB[12] = {"TileU", "TileV", "Bilinear", "Trilinear",
        "AlphaTest", "HideAlpha", "HideColor", "InvertAlpha", "ZEqual",
        "NoZWrite", "UseLocalAlpha", "Inactive"};
    for (int b = 0; b < 12; ++b)
        if (v & (1u << b)) r.flags.push_back(FB[b]);

    static const char* BLEND[10] = {"Copy", "Alpha", "AlphaPremult", "AlphaDest",
        "AlphaDestPremult", "Add", "Sub", "Glow", "PSX2Shadow", "SpecialContrast"};
    static const char* COLOROP[12] = {"Diffuse", "Specular", "Disable", "RLI",
        "FullLight", "InvertDiffuse", "Diffuse2X", "SpecularColor", "DiffuseColor",
        "ConstantColor", "XeAlphaAdd", "XeModulateColor"};
    r.blend_name = (r.blend < 10) ? BLEND[r.blend] : "blend" + std::to_string(r.blend);
    r.colorop_name = (r.colorop < 12) ? COLOROP[r.colorop] : "op" + std::to_string(r.colorop);

    if (v & (1u << 11)) {
        r.mode = "Inactive";
    } else if (r.blend != 0) {
        r.mode = r.blend_name + (r.alpha_test ? " (a-test)" : "");
    } else if (r.alpha_test) {
        bool overlay = (v & (1u << 7)) || (v & (1u << 6));
        r.mode = overlay ? "Alpha-test overlay" : "Alpha-test";
    } else {
        r.mode = "Opaque";
    }
    return r;
}

MatRenderFlags parse_material_render_flags(const uint8_t* d, size_t n, uint32_t layer) {
    long long off = render_ulflags_offset(d, n, layer);
    if (off < 0) return MatRenderFlags{};
    return decode_render_flags(le32(d, static_cast<size_t>(off)));
}

std::vector<size_t> texture_layer_offsets(const uint8_t* d, size_t n) {
    std::vector<size_t> out;
    if (!d || n < 4) return out;
    long long base = kind_tex_offset(le32(d, 0));
    if (base < 0) return out;
    long long stride = kind_layer_stride(le32(d, 0));
    size_t single_len = static_cast<size_t>(base) + 4;  // base map's key is last
    std::vector<size_t> offs;
    if (stride > 0 && n >= single_len && (n - single_len) % static_cast<size_t>(stride) == 0) {
        size_t cnt = (n - single_len) / static_cast<size_t>(stride) + 1;
        for (size_t i = 0; i < cnt; ++i)
            offs.push_back(static_cast<size_t>(base) + i * static_cast<size_t>(stride));
    } else {
        offs.push_back(static_cast<size_t>(base));
    }
    for (size_t o : offs)
        if (o + 4 <= n) out.push_back(o);
    return out;
}

long long texture_layer_offset(const uint8_t* d, size_t n, uint32_t layer) {
    std::vector<size_t> offs = texture_layer_offsets(d, n);
    if (layer < offs.size()) return static_cast<long long>(offs[layer]);
    return -1;
}

std::vector<TexLayer> resolve_texture_keys(const uint8_t* d, size_t n) {
    std::vector<TexLayer> out;
    std::vector<size_t> offs = texture_layer_offsets(d, n);
    for (size_t i = 0; i < offs.size(); ++i) {
        uint32_t key = le32(d, offs[i]);
        if (key == 0 || key == 0xFFFFFFFFu) continue;
        if (i > 0 && (key >> 24) == 0) continue;  // extra layers must carry a type byte
        out.push_back({static_cast<uint32_t>(i), offs[i], key});
    }
    return out;
}

std::vector<uint8_t> set_texture_key(const uint8_t* d, size_t n, uint32_t new_key,
                                     uint32_t layer) {
    long long off = texture_layer_offset(d, n, layer);
    if (off < 0) return {};
    std::vector<uint8_t> buf(d, d + n);
    put_le32(buf, static_cast<size_t>(off), new_key);
    return buf;
}

long long multi_slot_offset(const uint8_t* d, size_t n, uint32_t slot) {
    if (!d || n < 8) return -1;
    uint32_t n_sub = le32(d, 4);
    size_t off = 8 + static_cast<size_t>(slot) * 4;
    if (slot >= n_sub || off + 4 > n) return -1;
    return static_cast<long long>(off);
}

std::vector<uint8_t> set_multi_sub_key(const uint8_t* d, size_t n, uint32_t slot,
                                       uint32_t new_key) {
    long long off = multi_slot_offset(d, n, slot);
    if (off < 0) return {};
    std::vector<uint8_t> buf(d, d + n);
    put_le32(buf, static_cast<size_t>(off), new_key);
    return buf;
}

std::vector<uint8_t> append_multi_sub_key(const uint8_t* d, size_t n, uint32_t new_key) {
    if (!d || n < 8) return {};
    uint32_t n_sub = le32(d, 4);
    size_t insert_at = 8 + static_cast<size_t>(n_sub) * 4;
    if (n_sub > 1000000u || insert_at > n) return {};
    std::vector<uint8_t> buf;
    buf.reserve(n + 4);
    buf.insert(buf.end(), d, d + insert_at);
    buf.push_back(static_cast<uint8_t>(new_key));
    buf.push_back(static_cast<uint8_t>(new_key >> 8));
    buf.push_back(static_cast<uint8_t>(new_key >> 16));
    buf.push_back(static_cast<uint8_t>(new_key >> 24));
    buf.insert(buf.end(), d + insert_at, d + n);
    put_le32(buf, 4, n_sub + 1);
    return buf;
}

}  // namespace jade
