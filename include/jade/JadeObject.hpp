// JadeObject.hpp — Jade binarized object/property pair-stream (read path).
//
// Port of jade_explorer/core/jade_object.py. A pair-stream record payload is:
//   [header : 3 x u32][pairs : N x (tag:u32, value:u32)][blob header : 16B?][blob?]
// header[1] is the byte size of (pairs + blob header), which locates the blob
// boundary. This is the serialization behind Menu/SpecialLoad records; it has no
// gro_type/ext discriminator, so there is no BigFile golden target — coverage is
// the synthetic round-trip unit test.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace jade {

constexpr size_t JOS_HEADER_SIZE      = 12;
constexpr size_t JOS_PAIR_SIZE        = 8;
constexpr size_t JOS_BLOB_HEADER_SIZE = 16;

// The 14-tag signature of one SpecialLoad map unit.
constexpr std::array<uint32_t, 14> SPECIAL_LOAD_UNIT_TAGS = {
    0x02000898, 0x05000021, 0x0200089b, 0x05000021, 0x15000026, 0x01000026,
    0x02000898, 0x05000021, 0x15000021, 0x01000021,
    0x02000898, 0x05000021, 0x15000021, 0x01000021,
};

struct JadeObjectStream {
    bool     ok = false;
    uint32_t header[3] = {0, 0, 0};
    std::vector<std::pair<uint32_t, uint32_t>> pairs;  // (tag, value)
    bool                 has_blob_header = false;
    std::vector<uint8_t> blob_header;                  // 16 bytes when present
    bool                 has_blob = false;
    std::vector<uint8_t> blob;
};

// Decompose a record payload. ok == false mirrors the Python None.
JadeObjectStream parse_jade_object(const uint8_t* d, size_t n);

// Re-emit the payload bytes (inverse of parse; round-trips byte-for-byte).
std::vector<uint8_t> serialize_jade_object(const JadeObjectStream& s);

// Start pair-indices of every SpecialLoad map unit (run of the 14 tags).
std::vector<size_t> find_special_load_units(const JadeObjectStream& s);

}  // namespace jade
