// Compression.cpp — implementation. Faithful port of core/compression.py.
#include "jade/Compression.hpp"

extern "C" {
#include "minilzo.h"

// Vendored lzo1x_999 (third_party/lzo999) behind a size_t wrapper — the real
// optimizer python-lzo runs for every level > 1 (compress_lzo_9's path).
size_t jade_lzo999_wrkmem_size(void);
int jade_lzo1x_999_compress(const unsigned char* in, size_t in_len,
                            unsigned char* out, size_t* out_len, void* wrkmem);
}

namespace jade {

namespace {

inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void put_le32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

// lzo_init() validates type sizes and must run once before any LZO call.
bool lzo_ready() {
    static const bool ok = (lzo_init() == LZO_E_OK);
    return ok;
}

}  // namespace

LzoResult decompress_lzo(const uint8_t* raw, size_t raw_len, size_t max_output) {
    LzoResult res;
    if (!raw || raw_len < 12) return res;  // (None, 0)
    if (!lzo_ready()) return res;

    size_t off = 4;  // skip bin_file_size header
    std::vector<uint8_t>& result = res.data;
    uint32_t total_dec = 0;
    bool stopped_early = false;
    bool failed = false;

    while (off + 4 <= raw_len) {
        uint32_t dec_size = le32(raw + off);
        if (dec_size == 0) break;
        off += 4;
        if (off + 4 > raw_len) break;
        uint32_t comp_size = le32(raw + off);
        off += 4;
        total_dec += dec_size;

        if (stopped_early) {
            off += (comp_size >= dec_size) ? dec_size : comp_size;
            continue;
        }

        if (off + comp_size > raw_len && comp_size < dec_size) break;

        if (comp_size >= dec_size) {
            // Stored raw (incompressible block). Python copies dec_size bytes
            // and advances by dec_size; the slice clamps near EOF.
            size_t avail = (off + dec_size <= raw_len) ? dec_size : (raw_len - off);
            result.insert(result.end(), raw + off, raw + off + avail);
            off += dec_size;
        } else {
            std::vector<uint8_t> block(dec_size);
            lzo_uint dlen = dec_size;
            int r = lzo1x_decompress_safe(
                reinterpret_cast<const unsigned char*>(raw + off), comp_size,
                block.data(), &dlen, nullptr);
            if (r != LZO_E_OK || dlen != dec_size) {
                // Python: _lzo.decompress raised -> return None, total_dec.
                failed = true;
                break;
            }
            result.insert(result.end(), block.begin(), block.end());
            off += comp_size;
        }

        if (max_output > 0 && result.size() >= max_output) stopped_early = true;
    }

    res.total_dec = total_dec;
    if (failed) {
        res.data.clear();
        res.ok = false;
        return res;
    }
    // Python returns bytes(result) if result else None.
    res.ok = !result.empty();
    if (!res.ok) res.data.clear();
    return res;
}

std::vector<uint8_t> compress_lzo(const uint8_t* data, size_t len, int level) {
    std::vector<uint8_t> out;
    out.resize(4, 0);  // placeholder for bin_file_size (patched at the end)

    // python-lzo maps level 1 -> lzo1x_1_compress and EVERY other level ->
    // lzo1x_999_compress (no per-level tuning), so compress_lzo_9 == 999.
    const bool use_999 = (level != 1);

    if (lzo_ready() && data && len > 0) {
        std::vector<uint8_t> wrkmem(use_999 ? jade_lzo999_wrkmem_size()
                                            : size_t(LZO1X_1_MEM_COMPRESS));
        size_t off = 0;
        while (off < len) {
            const size_t dec_size = (len - off < LZO_BLOCK_SIZE)
                                        ? (len - off) : LZO_BLOCK_SIZE;
            // LZO worst-case expansion bound for the destination buffer.
            std::vector<uint8_t> comp(dec_size + dec_size / 16 + 64 + 3);
            int r;
            lzo_uint comp_len;
            if (use_999) {
                size_t clen = comp.size();
                r = jade_lzo1x_999_compress(
                    reinterpret_cast<const unsigned char*>(data + off), dec_size,
                    comp.data(), &clen, wrkmem.data());
                comp_len = static_cast<lzo_uint>(clen);
            } else {
                comp_len = comp.size();
                r = lzo1x_1_compress(
                    reinterpret_cast<const unsigned char*>(data + off), dec_size,
                    comp.data(), &comp_len, wrkmem.data());
            }

            if (r != LZO_E_OK || comp_len >= dec_size) {
                // Stored raw: comp_size == dec_size signals "incompressible".
                put_le32(out, static_cast<uint32_t>(dec_size));
                put_le32(out, static_cast<uint32_t>(dec_size));
                out.insert(out.end(), data + off, data + off + dec_size);
            } else {
                put_le32(out, static_cast<uint32_t>(dec_size));
                put_le32(out, static_cast<uint32_t>(comp_len));
                out.insert(out.end(), comp.data(), comp.data() + comp_len);
            }
            off += LZO_BLOCK_SIZE;
        }
    }

    put_le32(out, 0);  // dec_size == 0 terminator
    const uint32_t total = static_cast<uint32_t>(out.size());
    out[0] = static_cast<uint8_t>(total);
    out[1] = static_cast<uint8_t>(total >> 8);
    out[2] = static_cast<uint8_t>(total >> 16);
    out[3] = static_cast<uint8_t>(total >> 24);
    return out;
}

long long lzo_terminator_offset(const uint8_t* compressed, size_t len) {
    if (!compressed || len < 8) return -1;
    size_t off = 4;  // skip bin_file_size header
    while (off + 4 <= len) {
        uint32_t dec_size = le32(compressed + off);
        if (dec_size == 0) return static_cast<long long>(off);
        if (off + 8 > len) return -1;
        uint32_t comp_size = le32(compressed + off + 4);
        uint32_t block_len = (comp_size < dec_size) ? comp_size : dec_size;
        off += 8 + block_len;
    }
    return -1;
}

}  // namespace jade
