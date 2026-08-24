// Light.cpp — implementation. Faithful port of core/light.py (read + write).
#include "jade/Light.hpp"

#include <cstring>

namespace jade {

namespace {

constexpr uint32_t TYPE_MASK = 0x7;

inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}

// Read a present-checked raw u32 at a field offset (has_off=false => absent,
// mirroring the Python offset dict carrying None for specular on v9).
LightField field(const uint8_t* d, size_t n, bool has_off, size_t off) {
    LightField f;
    if (has_off && off + 4 <= n) { f.present = true; f.bits = le32(d, off); }
    return f;
}

}  // namespace

bool is_light_payload(const uint8_t* d, size_t n) {
    if (!d || n < 12) return false;
    uint32_t v = le32(d, 0);
    return v == 9 || v == 10;
}

std::string light_type_name(uint32_t flags) {
    switch (flags & TYPE_MASK) {
        case 0: return "Omni";
        case 1: return "Directional";
        case 2: return "Spot";
        case 3: return "Fog";
        case 5: return "Ambient";
        default: return "Type" + std::to_string(flags & TYPE_MASK);
    }
}

LightInfo parse_light(const uint8_t* d, size_t n) {
    LightInfo li;
    if (!is_light_payload(d, n)) return li;
    uint32_t version = le32(d, 0);

    // Field offsets per version (v10 inserts a specular u32 at +12).
    bool spec_off = (version >= 10);
    size_t o_flags = 4, o_diffuse = 8, o_specular = 12;
    size_t o_near, o_far, o_inner, o_outer, o_intensity;
    if (version >= 10) {
        o_near = 16; o_far = 20; o_inner = 24; o_outer = 28; o_intensity = 32;
    } else {
        o_near = 12; o_far = 16; o_inner = 20; o_outer = 24; o_intensity = 28;
    }
    uint32_t base = (version == 9) ? 32 : 36;

    LightField flags_f = field(d, n, true, o_flags);
    uint32_t flags = flags_f.present ? flags_f.bits : 0;  // Python: `or 0`

    li.ok = true;
    li.version = version;
    li.flags = flags;
    li.type = flags & TYPE_MASK;
    li.type_name = light_type_name(flags);
    li.diffuse  = field(d, n, true, o_diffuse);
    li.specular = field(d, n, spec_off, o_specular);
    li.near_    = field(d, n, true, o_near);
    li.far_     = field(d, n, true, o_far);
    li.inner    = field(d, n, true, o_inner);
    li.outer    = field(d, n, true, o_outer);
    li.has_specular = spec_off;
    li.has_intensity = (n == base);
    li.intensity = li.has_intensity ? field(d, n, true, o_intensity) : LightField{};
    li.size = static_cast<uint32_t>(n);
    li.base_size = base;
    return li;
}

namespace {
inline void put_le32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = x & 0xFF; v[o + 1] = (x >> 8) & 0xFF;
    v[o + 2] = (x >> 16) & 0xFF; v[o + 3] = (x >> 24) & 0xFF;
}
inline void put_f32(std::vector<uint8_t>& v, size_t o, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    put_le32(v, o, b);
}
// _pack_color: (r,g,b) -> 0x00BBGGRR, preserving orig_raw's top (alpha) byte.
inline uint32_t pack_color(const std::array<int, 3>& rgb, uint32_t orig_raw) {
    uint32_t r = static_cast<uint32_t>(rgb[0]) & 0xFF;
    uint32_t g = static_cast<uint32_t>(rgb[1]) & 0xFF;
    uint32_t b = static_cast<uint32_t>(rgb[2]) & 0xFF;
    return (orig_raw & 0xFF000000u) | (b << 16) | (g << 8) | r;
}
}  // namespace

std::vector<uint8_t> write_light_fields(const uint8_t* d, size_t n, const LightEdit& e) {
    LightInfo info = parse_light(d, n);
    if (!info.ok) return {};                     // Python raises ValueError

    bool spec_off = (info.version >= 10);
    size_t o_flags = 4, o_diffuse = 8, o_specular = 12;
    size_t o_near, o_far, o_inner, o_outer, o_intensity;
    if (info.version >= 10) { o_near = 16; o_far = 20; o_inner = 24; o_outer = 28; o_intensity = 32; }
    else                    { o_near = 12; o_far = 16; o_inner = 20; o_outer = 24; o_intensity = 28; }

    std::vector<uint8_t> buf(d, d + n);

    if (e.light_type && o_flags + 4 <= n) {
        uint32_t flags = (info.flags & ~TYPE_MASK) | (*e.light_type & TYPE_MASK);
        put_le32(buf, o_flags, flags);
    }
    if (e.diffuse && o_diffuse + 4 <= n)
        put_le32(buf, o_diffuse, pack_color(*e.diffuse, info.diffuse.bits));
    if (e.specular && spec_off && o_specular + 4 <= n)
        put_le32(buf, o_specular, pack_color(*e.specular, info.specular.bits));

    const std::pair<const std::optional<float>&, size_t> fs[] = {
        {e.near_, o_near}, {e.far_, o_far}, {e.inner, o_inner}, {e.outer, o_outer}};
    for (const auto& f : fs)
        if (f.first && f.second + 4 <= n) put_f32(buf, f.second, *f.first);

    if (e.intensity && info.has_intensity && o_intensity + 4 <= n)
        put_f32(buf, o_intensity, *e.intensity);

    return buf;
}

}  // namespace jade
