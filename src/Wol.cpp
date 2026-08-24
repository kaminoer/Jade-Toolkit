// Wol.cpp — implementation. Faithful port of core/wol.py.
#include "jade/Wol.hpp"

#include "jade/WowStream.hpp"   // WOW_MAGIC

namespace jade {

namespace {
inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}
// ".wow" = 2e 77 6f 77
inline bool is_wow_tag(const uint8_t* p) {
    return p[0] == 0x2e && p[1] == 0x77 && p[2] == 0x6f && p[3] == 0x77;
}
}  // namespace

std::vector<uint32_t> Wol::missing_deps(const std::unordered_set<uint32_t>& index) const {
    std::vector<uint32_t> out;
    for (uint32_t d : deps)
        if (index.find(d) == index.end()) out.push_back(d);
    return out;
}

Wol parse_wol(const uint8_t* d, size_t n) {
    Wol w;
    if (!d || n < 12) return w;
    w.size = le32(d, 0);
    uint32_t magic = le32(d, 4);
    w.key = le32(d, 8);
    if (magic != WOW_MAGIC) return w;  // ok == false

    // Find the first ".wow" at >= 12, then walk contiguous 8-byte dep entries.
    size_t i = 12;
    while (i + 4 <= n && !is_wow_tag(d + i)) ++i;
    while (i + 8 <= n && is_wow_tag(d + i)) {
        w.deps.push_back(le32(d, i + 4));
        i += 8;
    }
    w.ok = true;
    return w;
}

}  // namespace jade
