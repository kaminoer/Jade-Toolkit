// ZoneCreate.hpp -- reusable zone cloning/rekeying primitives.
//
// Port of core/zone_create.py.  The project builder and Create Zone GUI use
// this surface rather than keeping private copies of its allocation/rewrite
// logic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jade/BigFile.hpp"

namespace jade {
namespace zonecreate {

constexpr size_t WOW_NAME_FIELD_OFFSET = 12;
constexpr size_t WOW_NAME_FIELD_LEN = 32;
inline constexpr uint8_t WOW_RECORD0_TAG[4] = {'.', 'w', 'o', 'w'};
constexpr uint32_t MODDED_KEY_PREFIX = 0x27;

using KeyMap = std::unordered_map<uint32_t, uint32_t>;

// Exception carrying the Python exception class used by validation reports.
class Error : public std::runtime_error {
public:
    Error(std::string python_type, const std::string& message)
        : std::runtime_error(message), python_type(std::move(python_type)) {}
    std::string python_type;
};

struct ClonedZone {
    std::vector<uint8_t> wow_bytes;
    std::vector<uint8_t> wol_bytes;
    uint32_t new_wow_key = 0;
    uint32_t new_wol_key = 0;
    int rekeyed_count = 0;
    std::unordered_set<uint32_t> local_keys_seen;
    uint32_t new_map_prefix = 0;
};

struct AllocatedZoneKeys {
    uint32_t wow_key = 0;
    uint32_t wol_key = 0;
    uint32_t low16 = 0;
};

std::unordered_set<uint32_t> collect_local_keys(
    const std::vector<uint8_t>& wow_dec, uint32_t map_prefix);

KeyMap build_key_map(const std::unordered_set<uint32_t>& local_keys,
                     uint32_t old_prefix, uint32_t new_prefix);

std::pair<std::vector<uint8_t>, int> rekey_stream(
    const std::vector<uint8_t>& data, const KeyMap& key_map);

std::vector<uint8_t> patch_wow_name(const std::vector<uint8_t>& wow_bytes,
                                    const std::string& new_name);

ClonedZone clone_zone(
    const std::vector<uint8_t>& wow_dec,
    const std::vector<uint8_t>& wol_dec,
    const std::string& new_name,
    uint32_t new_low16,
    const std::unordered_set<uint32_t>& used_keys,
    uint32_t new_map_prefix = MODDED_KEY_PREFIX);

std::pair<std::vector<uint8_t>, int> add_wol_deps(
    const std::vector<uint8_t>& wol_bytes,
    const std::vector<uint32_t>& new_dep_keys);

AllocatedZoneKeys alloc_new_zone_bf_keys(
    const BigFile& bigfile,
    uint32_t new_map_prefix = MODDED_KEY_PREFIX,
    const std::vector<uint32_t>& extra_used_keys = {});

}  // namespace zonecreate
}  // namespace jade
