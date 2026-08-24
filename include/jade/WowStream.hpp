// WowStream.hpp — walk the decompressed _wow_/_wol_ record stream.
//
// Port of jade_explorer/core/wow_stream.py. Records are
// [size:u32][magic:u32=0xEEFFC099][key:u32][payload:size]; the walker locates
// the first record then steps 12+size, stopping on a bad magic or overrun. The
// payload's leading ASCII ".xxx" tag is found by the same regex the Python uses
// (\.[a-zA-Z]{2,4}\b over the first 80 payload bytes).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jade {

constexpr uint32_t WOW_MAGIC = 0xEEFFC099u;

struct WowRecord {
    size_t   offset = 0;    // record start (size field)
    uint32_t key = 0;
    uint32_t size = 0;      // payload length
    std::string tag;        // leading ".xxx" tag, or ""
    size_t   body_off = 0;  // payload offset within the stream
};

// Offset of the first record (size field), or -1 if none in the leading window.
long long find_first_record(const uint8_t* d, size_t n, size_t max_scan = 0x400);

// Tag of a payload head: first match of \.[a-zA-Z]{2,4}\b, or "".
std::string wow_tag_of(const uint8_t* head, size_t head_len);

// Walk the whole stream from the first valid record.
std::vector<WowRecord> walk_wow(const uint8_t* d, size_t n);

inline std::vector<WowRecord> walk_wow(const std::vector<uint8_t>& v) {
    return walk_wow(v.data(), v.size());
}

}  // namespace jade
