// SubEntry.hpp — walk the decompressed Jade record stream (sub-entries).
//
// Port of jade_explorer/core/sub_entry.py (the size-walking path). A decompressed
// .bin / _wow_ stream is a flat forward sequence of records:
//
//   [size:u32][cookie:u32][key:u32][type:u32 (if size>=4)][payload:(size-4) bytes]
//
// cookie is 0xEEFFC099 (bytes 99 C0 FF EE) or the modded variant 0xFEFFC099
// (..FE). The walker locates the first record then steps 12+size per record,
// re-checking the cookie at each header — never re-scanning (the memory rule:
// "walk by size field, not by cookie scan"). The first 4 payload bytes are an
// ASCII '.ext' tag (e.g. ".gao") or, when not ASCII, a numeric gro_type.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jade {

// 0xEEFFC099 and the modded 0xFEFFC099 (bit 0x10 of the last byte flipped).
constexpr uint32_t JADE_COOKIE     = 0xEEFFC099u;
constexpr uint32_t JADE_COOKIE_ALT = 0xFEFFC099u;

struct SubEntry {
    size_t   offset   = 0;      // cookie position (record_start + 4), matching
                                // the Python dict's 'offset' field
    uint32_t size     = 0;      // declared size field
    uint32_t key      = 0;      // resource key
    std::string ext;            // ".gao" etc., or "" when the tag isn't ASCII
    bool     gro_null = false;  // true when ext is set (Python gro_type = None)
    uint32_t gro_type = 0;      // u32 tag when ext is empty
    std::vector<uint8_t> data;  // payload ((size-4) bytes, or empty)
};

// Legacy cookie-scan parser. Unlike the size walker, this intentionally finds
// every cookie-looking byte sequence and delimits payloads by the next match,
// exactly like Python parse_sub_entries(). Callers that need robust archive
// traversal should continue to prefer walk_sub_entries().
std::vector<SubEntry> parse_sub_entries(const uint8_t* d, size_t n);
inline std::vector<SubEntry> parse_sub_entries(
    const std::vector<uint8_t>& v) {
    return parse_sub_entries(v.data(), v.size());
}

struct SubEntryBounds {
    bool found = false;
    size_t payload_start = 0;
    size_t payload_end = 0;
    size_t cookie_pos = 0;
};

// Cookie-scan bounds lookup used by the Python mutation helpers.
SubEntryBounds find_sub_entry_bounds(const uint8_t* d, size_t n,
                                     uint32_t target_key);
inline SubEntryBounds find_sub_entry_bounds(const std::vector<uint8_t>& v,
                                            uint32_t target_key) {
    return find_sub_entry_bounds(v.data(), v.size(), target_key);
}

// Offset of the first record's size field (almost always 0). -1 if none in the
// leading `max_scan` window.
long long find_first_subentry(const uint8_t* d, size_t n, size_t max_scan = 0x400);

// Walk the whole stream by declared size from the first valid header.
std::vector<SubEntry> walk_sub_entries(const uint8_t* d, size_t n);

inline std::vector<SubEntry> walk_sub_entries(const std::vector<uint8_t>& v) {
    return walk_sub_entries(v.data(), v.size());
}

}  // namespace jade
