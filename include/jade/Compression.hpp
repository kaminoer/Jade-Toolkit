// Compression.hpp — Jade LZO compression framing (read path).
//
// Port of jade_explorer/core/compression.py. The Jade BigFile entry payload is:
//   [bin_file_size:u32] then blocks [dec_size:u32][comp_size:u32][data]...
//   terminated by dec_size == 0.
// A block is stored raw when comp_size >= dec_size, else it is LZO1X-compressed.
// The LZO1X codec itself is vendored miniLZO (see third_party/minilzo); this
// file owns only the Jade-specific framing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jade {

// Jade LZO block size (matches BLOCK_SIZE in core/compression.py / prince.bf).
constexpr size_t LZO_BLOCK_SIZE = 0x20000;  // 128 KiB

// Result of decompress_lzo. `ok == false` corresponds to the Python returning
// None for the data (too-small input, empty result, or a genuine LZO failure);
// `total_dec` still carries the summed declared block sizes, like the Python's
// second return value.
struct LzoResult {
    bool ok = false;
    std::vector<uint8_t> data;
    uint32_t total_dec = 0;
};

// Decompress a Jade-LZO entry payload. `max_output > 0` stops accumulating
// output once that many bytes are produced (still walking the block table to
// keep `total_dec` exact), mirroring the Python's `stopped_early` behaviour.
LzoResult decompress_lzo(const uint8_t* raw, size_t raw_len, size_t max_output = 0);

inline LzoResult decompress_lzo(const std::vector<uint8_t>& raw, size_t max_output = 0) {
    return decompress_lzo(raw.data(), raw.size(), max_output);
}

// Compress `data` into a Jade-LZO entry payload (the inverse of
// decompress_lzo): [bin_file_size:u32] then [dec_size:u32][comp_size:u32][data]
// blocks of up to LZO_BLOCK_SIZE, terminated by dec_size == 0. A block whose
// LZO output is not smaller than the input is stored raw with comp_size ==
// dec_size, exactly as core/compression.py does.
//
// `level` is accepted for API parity with the Python (1 / 9) but miniLZO only
// implements lzo1x_1 — the output is a valid stream the engine decompresses;
// it is NOT byte-identical to the toolkit's level-9 (lzo1x_999) output, so
// golden-diff is checked at the decompressed level, never on raw bytes. (Swap
// in full LZO 999 here later for a smaller archive if desired.)
std::vector<uint8_t> compress_lzo(const uint8_t* data, size_t len, int level = 1);

inline std::vector<uint8_t> compress_lzo(const std::vector<uint8_t>& data, int level = 1) {
    return compress_lzo(data.data(), data.size(), level);
}

// Offset of the dec_size==0 terminator inside a compressed stream (the value
// the engine stores per-entry in size.grs to cap streaming reads). Returns -1
// when no terminator is found — matching the Python returning None.
long long lzo_terminator_offset(const uint8_t* compressed, size_t len);

inline long long lzo_terminator_offset(const std::vector<uint8_t>& c) {
    return lzo_terminator_offset(c.data(), c.size());
}

}  // namespace jade
