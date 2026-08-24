// WowStream.cpp — implementation. Faithful port of core/wow_stream.py.
#include "jade/WowStream.hpp"

#include <algorithm>

namespace jade {

namespace {

inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}
inline bool is_word(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}
inline bool is_alpha(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Magic 0xEEFFC099 little-endian = bytes 99 C0 FF EE.
const uint8_t MAGIC_B[4] = {0x99, 0xC0, 0xFF, 0xEE};

// First index i in [start, end_incl] where the 4 magic bytes match, else -1.
long long find_magic(const uint8_t* d, size_t start, size_t end_incl) {
    for (size_t i = start; i <= end_incl; ++i) {
        if (d[i] == MAGIC_B[0] && d[i + 1] == MAGIC_B[1] &&
            d[i + 2] == MAGIC_B[2] && d[i + 3] == MAGIC_B[3])
            return static_cast<long long>(i);
    }
    return -1;
}

}  // namespace

std::string wow_tag_of(const uint8_t* head, size_t head_len) {
    // Match \.[a-zA-Z]{2,4}\b: a '.', then 2..4 letters, then a word boundary
    // (the run must be exactly 2..4 letters and terminated by a non-word char
    // or the end of the window).
    for (size_t i = 0; i < head_len; ++i) {
        if (head[i] != '.') continue;
        size_t r = 0;
        while (i + 1 + r < head_len && is_alpha(head[i + 1 + r]) && r < 5) ++r;
        if (r < 2 || r > 4) continue;
        size_t after = i + 1 + r;
        if (after >= head_len || !is_word(head[after]))
            return std::string(reinterpret_cast<const char*>(head + i), r + 1);
    }
    return "";
}

long long find_first_record(const uint8_t* d, size_t n, size_t max_scan) {
    if (!d || n < 12) return -1;
    size_t limit = std::min(max_scan, n - 12);
    size_t off = 0;
    // Python: dec.find(MAGIC, off, limit+8) => match start i <= limit+4.
    size_t end_incl = (limit + 4 < n - 3) ? limit + 4 : n - 4;
    while (true) {
        long long i = find_magic(d, off, end_incl);
        if (i < 0) return -1;
        if (i >= 4) {
            size_t rec_off = static_cast<size_t>(i) - 4;
            uint32_t size = le32(d, rec_off);
            if (rec_off + 12 + size <= n) return static_cast<long long>(rec_off);
        }
        off = static_cast<size_t>(i) + 4;
        if (off > end_incl) return -1;
    }
}

std::vector<WowRecord> walk_wow(const uint8_t* d, size_t n) {
    std::vector<WowRecord> out;
    if (!d) return out;
    long long first = find_first_record(d, n);
    if (first < 0) return out;
    size_t off = static_cast<size_t>(first);
    while (off + 12 <= n) {
        uint32_t size = le32(d, off);
        uint32_t magic = le32(d, off + 4);
        uint32_t key = le32(d, off + 8);
        if (magic != WOW_MAGIC) break;
        if (off + 12 + static_cast<size_t>(size) > n) break;
        size_t body = off + 12;
        size_t head_len = std::min<size_t>(size, 80);
        WowRecord r;
        r.offset = off;
        r.key = key;
        r.size = size;
        r.tag = wow_tag_of(d + body, head_len);
        r.body_off = body;
        out.push_back(std::move(r));
        off += 12 + static_cast<size_t>(size);
    }
    return out;
}

}  // namespace jade
