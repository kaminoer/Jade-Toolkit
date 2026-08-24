// WolInfo.hpp — WOLInfo.bin (BF key 0xFC000001) parser/editor (read + splice).
//
// Port of jade_explorer/core/wolinfo.py. The engine's precomputed WOL->dependency
// table that shipped zones stream from. The stored BF entry is UNCOMPRESSED:
//   [total_len:u32][payload]   payload = [count:u32] then
//   count x ( wol_key:u32, N:u32, deps:u32[N] )   [+ trailing slack]
// All keys are internal (0xXX0YYYYY). Editing is a byte-splice so untouched
// regions stay bit-exact.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jade {

constexpr uint32_t WOLINFO_KEY = 0xFC000001u;

struct WolEntry {
    uint32_t wol_key = 0;
    std::vector<uint32_t> deps;   // internal keys, in load order
    size_t   start = 0;           // offset of wol_key within the payload
    size_t   n_off = 0;           // offset of the N field within the payload
    size_t   deps_off = 0;        // offset of deps[0] within the payload
    size_t   end = 0;             // one past the last dep within the payload
};

struct WolInfo {
    bool     ok = false;          // false where the Python ctor would raise
    std::vector<uint8_t> raw;     // [total_len][payload] (the stored entry)
    uint32_t total_len = 0;
    uint32_t count = 0;
    std::vector<WolEntry> entries;
    size_t   entries_end = 0;
    size_t   trailing_len = 0;

    const WolEntry* find(uint32_t wol_key) const;

    // Insert dep_key into wol_key's list (before before_key if given+present,
    // else before the last/self-ref dep). No-op (false) if already present or
    // wol_key absent. Rebuilds raw and re-parses so offsets stay valid.
    bool add_dep(uint32_t wol_key, uint32_t dep_key,
                 bool has_before = false, uint32_t before_key = 0);

    // Replace wol_key's ENTIRE dep list with new_deps (load order). Unlike
    // add_dep this DROPS deps not in new_deps — used to give a re-authored zone
    // its exact dep set. false if wol_key is absent. Rebuilds raw + re-parses.
    bool set_deps(uint32_t wol_key, const std::vector<uint32_t>& new_deps);

    std::vector<uint8_t> to_bytes() const { return raw; }
};

// Parse the raw stored WOLInfo.bin entry bytes. ok == false on bad/short data.
WolInfo parse_wolinfo(const uint8_t* d, size_t n);

inline WolInfo parse_wolinfo(const std::vector<uint8_t>& v) {
    return parse_wolinfo(v.data(), v.size());
}

}  // namespace jade
