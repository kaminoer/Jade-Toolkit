// Zone.hpp — Jade playable-zone model (_wow_ + _wol_ pair) analysis.
//
// Port of jade_explorer/core/zone.py. discover_zones pairs every _wow_ with its
// matching _wol_ by shared name; analyze() decompresses and walks the _wow_
// (record/GAO counts, map prefix, a player CheckPoint GAO) and resolves the
// _wol_ dependency list against the archive's resource index.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "jade/BigFile.hpp"

namespace jade {

struct ZoneInfo {
    std::string name;
    uint32_t    wow_index = 0, wol_index = 0;
    uint32_t    wol_key = 0;

    // Analysis (defaults hold until analyze()).
    uint32_t    record_count = 0, gao_count = 0;
    long long   map_prefix = -1;       // -1 == None
    long long   checkpoint_key = -1;   // -1 == None
    std::string checkpoint_name;
    bool        has_checkpoint = false;
    uint32_t    dep_total = 0, dep_missing = 0;
    long long   wol_internal_key = -1; // -1 == None
    bool        error = false;
};

// Pair every _wow_ with its matching _wol_; name-sorted, unanalyzed.
std::vector<ZoneInfo> discover_zones(const BigFile& bf);

// Decompress + inspect a zone (idempotent in the Python; here a plain call).
void analyze_zone(const BigFile& bf, ZoneInfo& z,
                  const std::unordered_set<uint32_t>& resource_index);

}  // namespace jade
