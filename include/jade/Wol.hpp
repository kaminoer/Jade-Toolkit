// Wol.hpp — Jade _wol_ (world object list) parser (read path).
//
// Port of jade_explorer/core/wol.py. The decompressed _wol_ is one record
// [size:u32][magic:0xEEFFC099][key:u32][payload]; the payload's dependency list
// is a contiguous run of 8-byte entries (".wow" + u32 internal key).
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace jade {

struct Wol {
    bool     ok = false;
    uint32_t size = 0;
    uint32_t key = 0;
    std::vector<uint32_t> deps;  // internal resource keys

    // Dep keys absent from the index (broken references).
    std::vector<uint32_t> missing_deps(const std::unordered_set<uint32_t>& index) const;
};

// Parse a decompressed _wol_ stream. ok == false mirrors the Python None.
Wol parse_wol(const uint8_t* d, size_t n);

inline Wol parse_wol(const std::vector<uint8_t>& v) { return parse_wol(v.data(), v.size()); }

}  // namespace jade
