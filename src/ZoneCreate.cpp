// ZoneCreate.cpp -- faithful port of core/zone_create.py.
#include "jade/ZoneCreate.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "jade/Keys.hpp"
#include "jade/WowStream.hpp"

namespace jade {
namespace zonecreate {
namespace {

constexpr const uint8_t* WOW_TAG = WOW_RECORD0_TAG;

uint32_t read_u32(const std::vector<uint8_t>& data, size_t offset) {
    return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
           (uint32_t(data[offset + 2]) << 16) |
           (uint32_t(data[offset + 3]) << 24);
}

void write_u32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset] = uint8_t(value);
    data[offset + 1] = uint8_t(value >> 8);
    data[offset + 2] = uint8_t(value >> 16);
    data[offset + 3] = uint8_t(value >> 24);
}

std::vector<uint32_t> utf8_codepoints(const std::string& value) {
    std::vector<uint32_t> result;
    for (size_t index = 0; index < value.size();) {
        const uint8_t lead = uint8_t(value[index]);
        uint32_t codepoint = lead;
        size_t width = 1;
        if ((lead & 0xe0u) == 0xc0u && index + 1 < value.size()) {
            codepoint = (uint32_t(lead & 0x1fu) << 6) |
                        uint32_t(uint8_t(value[index + 1]) & 0x3fu);
            width = 2;
        } else if ((lead & 0xf0u) == 0xe0u && index + 2 < value.size()) {
            codepoint = (uint32_t(lead & 0x0fu) << 12) |
                        (uint32_t(uint8_t(value[index + 1]) & 0x3fu) << 6) |
                        uint32_t(uint8_t(value[index + 2]) & 0x3fu);
            width = 3;
        } else if ((lead & 0xf8u) == 0xf0u && index + 3 < value.size()) {
            codepoint = (uint32_t(lead & 0x07u) << 18) |
                        (uint32_t(uint8_t(value[index + 1]) & 0x3fu) << 12) |
                        (uint32_t(uint8_t(value[index + 2]) & 0x3fu) << 6) |
                        uint32_t(uint8_t(value[index + 3]) & 0x3fu);
            width = 4;
        }
        result.push_back(codepoint);
        index += width;
    }
    return result;
}

[[noreturn]] void throw_ascii_error(const std::string& value) {
    const std::vector<uint32_t> codepoints = utf8_codepoints(value);
    size_t first = 0;
    while (first < codepoints.size() && codepoints[first] < 0x80) ++first;
    size_t last = first;
    while (last + 1 < codepoints.size() && codepoints[last + 1] >= 0x80) ++last;

    char escaped[16];
    if (codepoints[first] <= 0xff)
        std::snprintf(escaped, sizeof escaped, "\\x%02x",
                      unsigned(codepoints[first]));
    else if (codepoints[first] <= 0xffff)
        std::snprintf(escaped, sizeof escaped, "\\u%04x",
                      unsigned(codepoints[first]));
    else
        std::snprintf(escaped, sizeof escaped, "\\U%08x",
                      unsigned(codepoints[first]));

    std::string message = "'ascii' codec can't encode ";
    if (first == last)
        message += "character '" + std::string(escaped) + "' in position " +
                   std::to_string(first);
    else
        message += "characters in position " + std::to_string(first) + "-" +
                   std::to_string(last);
    message += ": ordinal not in range(128)";
    throw Error("UnicodeEncodeError", message);
}

void require_ascii(const std::string& value) {
    for (unsigned char byte : value)
        if (byte >= 0x80) throw_ascii_error(value);
}

}  // namespace

std::unordered_set<uint32_t> collect_local_keys(
    const std::vector<uint8_t>& wow_dec, uint32_t map_prefix) {
    std::unordered_set<uint32_t> keys;
    for (const WowRecord& record : walk_wow(wow_dec))
        if (((record.key >> 24) & 0xffu) == (map_prefix & 0xffu))
            keys.insert(record.key);
    return keys;
}

KeyMap build_key_map(const std::unordered_set<uint32_t>& local_keys,
                     uint32_t old_prefix, uint32_t new_prefix) {
    KeyMap result;
    for (uint32_t key : local_keys)
        if (((key >> 24) & 0xffu) == (old_prefix & 0xffu))
            result[key] = ((new_prefix & 0xffu) << 24) |
                          (key & 0x00ffffffu);
    return result;
}

std::pair<std::vector<uint8_t>, int> rekey_stream(
    const std::vector<uint8_t>& data, const KeyMap& key_map) {
    if (key_map.empty()) return {data, 0};
    std::vector<uint8_t> output = data;
    int count = 0;
    size_t offset = 0;
    while (offset + 4 <= output.size()) {
        const auto found = key_map.find(read_u32(output, offset));
        if (found == key_map.end()) {
            ++offset;
            continue;
        }
        write_u32(output, offset, found->second);
        ++count;
        offset += 4;
    }
    return {std::move(output), count};
}

std::vector<uint8_t> patch_wow_name(const std::vector<uint8_t>& wow_bytes,
                                    const std::string& new_name) {
    if (wow_bytes.empty() || wow_bytes.size() < 24 ||
        std::memcmp(wow_bytes.data() + 12, WOW_TAG, 4) != 0)
        return wow_bytes;
    require_ascii(new_name);
    const size_t name_size = std::min<size_t>(new_name.size(),
                                              WOW_NAME_FIELD_LEN - 1);
    std::vector<uint8_t> output = wow_bytes;
    const size_t start = 12 + WOW_NAME_FIELD_OFFSET;
    if (output.size() < start + WOW_NAME_FIELD_LEN)
        output.resize(start + WOW_NAME_FIELD_LEN, 0);
    std::fill(output.begin() + static_cast<long long>(start),
              output.begin() + static_cast<long long>(start + WOW_NAME_FIELD_LEN),
              0);
    std::copy_n(reinterpret_cast<const uint8_t*>(new_name.data()), name_size,
                output.begin() + static_cast<long long>(start));
    return output;
}

ClonedZone clone_zone(
    const std::vector<uint8_t>& wow_dec,
    const std::vector<uint8_t>& wol_dec,
    const std::string& new_name,
    uint32_t new_low16,
    const std::unordered_set<uint32_t>& used_keys,
    uint32_t new_map_prefix) {
    (void)used_keys;  // Python accepts this collision context informationally.
    const std::vector<WowRecord> records = walk_wow(wow_dec);
    if (records.empty())
        throw Error("ValueError",
                    "template wow has no walkable records (no record header found)");

    const uint32_t template_wow_key = records.front().key;
    const uint32_t old_prefix = map_prefix_of(template_wow_key);
    std::unordered_set<uint32_t> local_keys =
        collect_local_keys(wow_dec, old_prefix);
    const uint32_t new_wow_key = ((new_map_prefix & 0xffu) << 24) |
                                 (new_low16 & 0xffffu);
    const uint32_t new_wol_key = ((new_map_prefix & 0xffu) << 24) |
                                 ((new_low16 + 1u) & 0xffffu);
    KeyMap key_map = build_key_map(local_keys, old_prefix, new_map_prefix);
    key_map[template_wow_key] = new_wow_key;

    auto wow = rekey_stream(wow_dec, key_map);
    wow.first = patch_wow_name(wow.first, new_name);
    auto wol = rekey_stream(wol_dec, key_map);

    ClonedZone result;
    result.wow_bytes = std::move(wow.first);
    result.wol_bytes = std::move(wol.first);
    result.new_wow_key = new_wow_key;
    result.new_wol_key = new_wol_key;
    result.rekeyed_count = wow.second + wol.second;
    result.local_keys_seen = std::move(local_keys);
    result.new_map_prefix = new_map_prefix & 0xffu;
    return result;
}

std::pair<std::vector<uint8_t>, int> add_wol_deps(
    const std::vector<uint8_t>& wol_bytes,
    const std::vector<uint32_t>& new_dep_keys) {
    if (wol_bytes.size() < 12) return {wol_bytes, 0};
    std::vector<uint32_t> normalized;
    for (uint32_t key : new_dep_keys)
        if (key != INVALID_KEY) normalized.push_back(key);
    if (normalized.empty()) return {wol_bytes, 0};

    long long cursor = -1;
    for (size_t offset = 12; offset + 4 <= wol_bytes.size(); ++offset)
        if (std::memcmp(wol_bytes.data() + offset, WOW_TAG, 4) == 0) {
            cursor = static_cast<long long>(offset);
            break;
        }
    std::vector<std::pair<size_t, uint32_t>> existing;
    while (cursor >= 0 && size_t(cursor) + 8 <= wol_bytes.size() &&
           std::memcmp(wol_bytes.data() + cursor, WOW_TAG, 4) == 0) {
        existing.push_back({size_t(cursor),
                            read_u32(wol_bytes, size_t(cursor) + 4)});
        cursor += 8;
    }
    std::unordered_set<uint32_t> existing_keys;
    for (const auto& item : existing) existing_keys.insert(item.second);

    // Python checks only against the original set. Repeated requested keys
    // that were not already present are intentionally inserted repeatedly.
    std::vector<uint32_t> to_add;
    for (uint32_t key : normalized)
        if (!existing_keys.count(key)) to_add.push_back(key);
    if (to_add.empty()) return {wol_bytes, 0};

    const size_t insert_at = !existing.empty()
                                 ? existing.back().first
                                 : (cursor >= 0 ? size_t(cursor)
                                                : wol_bytes.size());
    std::vector<uint8_t> appended;
    appended.reserve(to_add.size() * 8);
    for (uint32_t key : to_add) {
        appended.insert(appended.end(), WOW_TAG, WOW_TAG + 4);
        const size_t offset = appended.size();
        appended.resize(offset + 4);
        write_u32(appended, offset, key);
    }
    std::vector<uint8_t> output;
    output.reserve(wol_bytes.size() + appended.size());
    output.insert(output.end(), wol_bytes.begin(),
                  wol_bytes.begin() + static_cast<long long>(insert_at));
    output.insert(output.end(), appended.begin(), appended.end());
    output.insert(output.end(),
                  wol_bytes.begin() + static_cast<long long>(insert_at),
                  wol_bytes.end());
    write_u32(output, 0, read_u32(wol_bytes, 0) + uint32_t(appended.size()));
    return {std::move(output), static_cast<int>(to_add.size())};
}

AllocatedZoneKeys alloc_new_zone_bf_keys(
    const BigFile& bigfile, uint32_t new_map_prefix,
    const std::vector<uint32_t>& extra_used_keys) {
    std::unordered_set<uint32_t> used;
    for (const auto& item : bigfile.files)
        if (item.second.key != INVALID_KEY) used.insert(item.second.key);
    for (uint32_t key : extra_used_keys)
        if (key != INVALID_KEY) used.insert(key);

    const uint32_t bf_mid = new_map_prefix & 0x0fu;
    const uint32_t base = 0xff000000u | (bf_mid << 16);
    for (uint32_t low16 = 1; low16 < 0xfffeu; ++low16) {
        const uint32_t wow_key = base | low16;
        const uint32_t wol_key = base | ((low16 + 1u) & 0xffffu);
        if (used.count(wow_key) || used.count(wol_key)) continue;
        return {wow_key, wol_key, low16};
    }
    char message[128];
    std::snprintf(message, sizeof message,
                  "no free BF FAT key pair under prefix 0x%02x (bf_mid=0x%02x)",
                  new_map_prefix, bf_mid);
    throw Error("RuntimeError", message);
}

}  // namespace zonecreate
}  // namespace jade
