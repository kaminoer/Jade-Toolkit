// LoadSim.hpp — BigFile load simulator "through the engine's eyes"
// (io_ops/load_sim.py).
//
// Models how the WW/T2T engine streams a zone to detect resource-resolution
// SPINS (black-screen hangs) statically: the load is sound iff every
// sub-entry key the zone's resources reference lands inside the zone's
// WOLInfo dep closure. Only gro_type-5 FX/decal stubs carry a structured
// cross-zone reference (the MODEL key = trailing dword); other types
// resolve in-closure by construction.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/SubEntry.hpp"

namespace jade {
namespace loadsim {

using LoadSimLogFn = std::function<void(const std::string&)>;

// _keyshaped: non-zero area byte + non-zero low half, not -1.
inline bool keyshaped(uint32_t v) {
    return (v >> 24) != 0 && (v & 0xFFFF) != 0 && v != 0xFFFFFFFFu;
}

// Structured outgoing references queued by the engine. The Python public
// helper currently recognizes the trailing model key on type-5 stubs only.
std::vector<uint32_t> extract_refs(const SubEntry& sub);

// Provider index: sub-entry key -> (bin bf_key, bin name) list, first-seen
// order. The path-taking overload supports Python-compatible pickle caching;
// the one-argument overload remains an uncached internal-validator helper.
struct KeyIndex {
    // dict-ordered: first-seen key order, provider list in scan order.
    std::vector<std::pair<uint32_t, std::vector<std::pair<uint32_t, std::string>>>> items;
    std::unordered_map<uint32_t, size_t> pos;
    const std::vector<std::pair<uint32_t, std::string>>* find(uint32_t key) const {
        auto it = pos.find(key);
        return it == pos.end() ? nullptr : &items[it->second].second;
    }
};
KeyIndex build_key_index(const BigFile& bf);

// Python-compatible public workflow. With cache enabled this reads/writes the
// interoperable <bf>.subkeyidx.pkl sidecar, validated by archive size and
// integer mtime, and emits the same progress/cache log messages.
KeyIndex build_key_index(const BigFile& bf, const std::string& bf_path,
                         bool cache = true, LoadSimLogFn log = {});

// zone_dep_bins: ordered [(internal_key, bf_key, name-or-"")] for a WOL.
struct DepBin {
    uint32_t internal_key = 0;
    uint32_t bf_key = 0;
    std::string name;    // "" when the bf key is not in the FAT
    bool has_name = false;
};
std::vector<DepBin> zone_dep_bins(const BigFile& bf, uint32_t wol_key);

struct Unresolved {
    uint32_t ref = 0;
    std::vector<std::pair<uint32_t, std::string>> providers;
    // (zone base name, referrer sub key, gro_type) — set semantics, unordered;
    // consumers should sort for display/compare.
    std::vector<std::tuple<std::string, uint32_t, uint32_t>> referrers;
};

struct SimReport {
    uint32_t wol_key = 0;
    uint32_t n_deps = 0;
    uint32_t deps_loaded = 0;
    // (internal, bf_key, decompress_failed)
    std::vector<std::tuple<uint32_t, uint32_t, bool>> missing_dep_bins;
    size_t closure_size = 0;
    std::vector<Unresolved> unresolved;   // insertion order (first hit)
};

// simulate_zone: closure + spin-candidate report for one WOL.
SimReport simulate_zone(const BigFile& bf, uint32_t wol_key,
                        const KeyIndex& key_index);

// Python's optional-key-index surface. A null key_index builds one using the
// ordinary cached workflow before simulating the zone.
SimReport simulate_zone(const BigFile& bf, const std::string& bf_path,
                        uint32_t wol_key, const KeyIndex* key_index = nullptr,
                        bool cache = true, LoadSimLogFn log = {});

}  // namespace loadsim
}  // namespace jade
