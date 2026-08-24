// Keys.hpp — Jade BigFile key-space conversions.
//
// Port of jade_explorer/core/keys.py. The internal (resource) key form is
// XX 00 YYYY where XX is the owning map's resource-prefix byte. build the
// resource index by reconstructing (map_prefix << 24) | (bf_key & 0xFFFF) for
// every _wow_ file, which resolves _wol_ dependency keys exactly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jade/BigFile.hpp"

namespace jade {

// Historical BF -> internal transform. It assumes BF bits 16..23 are the map
// prefix and is wrong for many maps; retained for Python API parity.
inline uint32_t bf_to_internal_naive(uint32_t bf_key) {
    return (((bf_key >> 16) & 0xFFu) << 24) | (bf_key & 0xFFFFu);
}

// Correct BF -> internal transform when the owning map prefix is known.
inline uint32_t bf_to_internal(uint32_t bf_key, uint32_t map_prefix) {
    return ((map_prefix & 0xFFu) << 24) | (bf_key & 0xFFFFu);
}

// A BF key and internal key share their low-16 resource id. The map prefix is
// not recoverable from a BF key alone, so callers may need map scoping.
inline bool bf_matches_internal(uint32_t bf_key, uint32_t internal_key) {
    return (bf_key & 0xFFFFu) == (internal_key & 0xFFFFu);
}

// BF files whose keyed FAT record shares internal_key's low-16 resource id,
// in BF file-index order. Unkeyed entries are omitted.
std::vector<const BFFile*> find_bf_files_for_internal(
    const BigFile& bf, uint32_t internal_key);

// Resource-prefix byte of an internal key (its top byte).
inline uint32_t map_prefix_of(uint32_t internal_key) { return (internal_key >> 24) & 0xFF; }

// Map prefix from a decompressed _wow_ stream (top byte of its first record
// key). Returns -1 if the stream has no walkable records.
long long derive_map_prefix(const uint8_t* dec, size_t n);

// {internal_key} for every _wow_ in the archive: derive_map_prefix << 24 |
// (bf_key & 0xFFFF). Membership resolves _wol_ deps. Only the first record of
// each _wow_ is needed, so decompression is capped at 64 bytes.
// `progress(done, total)` mirrors the Python's optional callback (GUI scans).
std::unordered_set<uint32_t> build_wow_resource_index(
    const BigFile& bf,
    const std::function<void(int, int)>& progress = {});

// Same walk, but keeps the mapping the Python dict carries:
// {internal_key -> BF file index} (the GUI bin picker lists names/keys).
// Later duplicates overwrite like a Python dict.
std::map<uint32_t, uint32_t> build_wow_resource_index_map(
    const BigFile& bf,
    const std::function<void(int, int)>& progress = {});

// Engine-exact internal -> BF FAT key transform for streamable resources:
// bf = 0xFF000000 | ((internal_top & 0x0F) << 16) | (internal & 0xFFFF).
// Verified trilogy-wide (SandOfTime 0x280000cd -> 0xff0800cd, hourglass
// 0x3c011723 -> 0xff0c1723). Port of keys.internal_to_bf_key.
inline uint32_t internal_to_bf_key(uint32_t internal_key) {
    return 0xFF000000u | (((internal_key >> 24) & 0x0Fu) << 16) |
           (internal_key & 0xFFFFu);
}

// {true_internal_key -> file index} for every _wow_: each file's ACTUAL
// first-record key (not the lossy prefix reconstruction — the middle byte is
// load-bearing for dep resolution, e.g. the hourglass self-ref 0x3c011723).
// Returned in file-index order (last duplicate wins, like the Python dict).
// Port of keys.build_true_wow_index.
std::vector<std::pair<uint32_t, uint32_t>> build_true_wow_index(
    const BigFile& bf,
    const std::function<void(int, int)>& progress = {},
    size_t max_output = 256);

}  // namespace jade
