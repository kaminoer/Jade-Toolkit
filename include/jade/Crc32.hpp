// Crc32.hpp — zlib-compatible CRC-32 (poly 0xEDB88320), used by the decompress
// golden harness to digest decompressed payloads. Matches Python's
// zlib.crc32(data) & 0xffffffff exactly, so the C++ and oracle digests compare.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace jade {

inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0) {
    uint32_t crc = ~seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
}

inline uint32_t crc32(const std::vector<uint8_t>& v) {
    return crc32(v.data(), v.size());
}

}  // namespace jade
