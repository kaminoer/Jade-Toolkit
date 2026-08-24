// ZoneTransplant.hpp — cross-game zone transplant
// (io_ops/zone_transplant.py).
//
// Clone a zone (_wow_ + _wol_ pair) AND its wol dependency closure from one
// trilogy archive into another. Deps already present in the target at the
// same internal key are skipped; everything else copies VERBATIM (compressed
// bytes + original keys — cross-references stay valid with no rekeying).
// exclude_keys omits chosen deps and rebuilds the wol without them.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jade/BigFile.hpp"

namespace jade {
namespace zonetx {

enum class DepStatus { Present, Copy, Unresolved, Excluded };
inline const char* dep_status_name(DepStatus s) {
    switch (s) {
        case DepStatus::Present: return "present";
        case DepStatus::Copy: return "copy";
        case DepStatus::Unresolved: return "unresolved";
        default: return "excluded";
    }
}

// One resolved _wol_ dependency and how it maps into the target.
struct DepEntry {
    uint32_t dep_key = 0;             // donor internal resource key
    DepStatus status = DepStatus::Unresolved;
    std::string name;                 // resolved BF entry name ("" if not)
    bool has_file = false;
    uint32_t file_index = 0;          // in the TARGET for Present, else donor
    uint32_t length = 0;              // that file's compressed length
    bool has_target_bf_key = false;
    uint32_t target_bf_key = 0;
    bool is_self_ref = false;
    std::string note;                 // same-named counterpart info
};

struct CopyItem {
    uint32_t bf_key = 0;
    std::string name;
    uint32_t donor_index = 0;
    uint32_t length = 0;
};

struct TransplantPlan {
    bool ok = false;
    std::string error;                // the Python ValueError message
    std::string zone_name;
    uint32_t zone_internal_key = 0;
    uint32_t wow_index = 0, wol_index = 0;
    uint32_t wol_bf_key = 0;
    std::vector<DepEntry> deps;
    std::vector<CopyItem> copy_items;
    std::vector<std::pair<uint32_t, std::string>> collisions;
    bool has_wol_dec = false;         // deps excluded -> rebuilt wol below
    std::vector<uint8_t> wol_dec;
    std::string wol_name;
    uint32_t excluded_count = 0;

    std::vector<const DepEntry*> present() const;
    std::vector<const DepEntry*> to_copy() const;
    std::vector<const DepEntry*> unresolved() const;
    uint64_t total_copy_bytes() const;
};

using TrueWowIndex = std::vector<std::pair<uint32_t, uint32_t>>;

// filter_wol_deps: drop dep entries whose key is excluded; the leading size
// field shrinks by 8 per removal. Returns (new_wol_bytes, removed_count).
std::pair<std::vector<uint8_t>, int> filter_wol_deps(
    const std::vector<uint8_t>& wol_bytes,
    const std::unordered_set<uint32_t>& exclude_keys);

// base_resource_name: strip the _wow_<key>.bin / _wol_<key>.bin suffix.
std::string base_resource_name(const std::string& name);

// plan_transplant: pure analysis (no writes).
TransplantPlan plan_transplant(const BigFile& donor_bf, const BigFile& target_bf,
                               const std::string& zone_name,
                               const std::unordered_set<uint32_t>& exclude_keys = {},
                               const TrueWowIndex* donor_index = nullptr,
                               const TrueWowIndex* target_index = nullptr);

struct TransplantStats {
    bool ok = false;
    std::string error;
    uint32_t added = 0;
    uint64_t added_bytes = 0;
    uint32_t skipped = 0;
    int size_grs_rows = 0;
    std::vector<std::pair<uint32_t, std::string>> collisions;
    std::vector<uint32_t> added_keys;
    uint32_t excluded = 0;
};

// execute_transplant: write the plan into output_path (a COPY of the target):
// add every copy item verbatim (donor compressed bytes, original keys), add
// the rebuilt wol when deps were excluded (recompressed at level 9), then
// flush size.grs. parent_dir_idx < 0 = auto (the dir holding _wow_ entries).
TransplantStats execute_transplant(const BigFile& donor_bf,
                                   const std::string& output_path,
                                   const TransplantPlan& plan,
                                   long long parent_dir_idx = -1,
                                   const BigFileLogFn& log = {});

}  // namespace zonetx
}  // namespace jade
