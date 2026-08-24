// SubEntry.cpp — implementation. Faithful port of core/sub_entry.py size-walk.
#include "jade/SubEntry.hpp"

#include <algorithm>

namespace jade {

namespace {

inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}

inline bool is_alpha(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Python: dec[p:p+4].decode('ascii') startswith '.' and rest .isalpha().
// '.' + three ASCII letters; anything else (incl. non-ASCII bytes) -> no tag.
bool ascii_ext(const uint8_t* b, std::string& out) {
    if (b[0] == '.' && is_alpha(b[1]) && is_alpha(b[2]) && is_alpha(b[3])) {
        out.assign(reinterpret_cast<const char*>(b), 4);
        return true;
    }
    return false;
}

// First index i in [start, end_limit] (inclusive) where the 4 cookie bytes
// match, else -1. Mirrors Python str.find(cookie, start, end_limit+4).
long long find_cookie(const uint8_t* d, uint32_t cookie, size_t start, size_t end_limit) {
    uint8_t c0 = cookie & 0xFF, c1 = (cookie >> 8) & 0xFF,
            c2 = (cookie >> 16) & 0xFF, c3 = (cookie >> 24) & 0xFF;
    for (size_t i = start; i <= end_limit; ++i) {
        if (d[i] == c0 && d[i + 1] == c1 && d[i + 2] == c2 && d[i + 3] == c3)
            return static_cast<long long>(i);
    }
    return -1;
}

}  // namespace

long long find_first_subentry(const uint8_t* d, size_t n, size_t max_scan) {
    if (!d || n < 12) return -1;
    size_t limit = std::min(max_scan, n - 12);
    for (uint32_t cookie : {JADE_COOKIE, JADE_COOKIE_ALT}) {  // EE first, then FE
        size_t pos = 0;
        while (pos <= limit) {
            long long i = find_cookie(d, cookie, pos, limit);
            if (i < 0) break;
            if (i >= 4) {
                size_t rec_off = static_cast<size_t>(i) - 4;
                uint32_t size = le32(d, rec_off);
                if (rec_off + 12 + size <= n) return static_cast<long long>(rec_off);
            }
            pos = static_cast<size_t>(i) + 4;
        }
    }
    return -1;
}

std::vector<SubEntry> parse_sub_entries(const uint8_t* d, size_t n) {
    std::vector<SubEntry> out;
    if (!d || n < 4) return out;
    std::vector<size_t> positions;
    for (uint32_t cookie : {JADE_COOKIE, JADE_COOKIE_ALT}) {
        if (n < 4) continue;
        size_t pos = 0;
        const size_t limit = n - 4;
        while (pos <= limit) {
            long long found = find_cookie(d, cookie, pos, limit);
            if (found < 0) break;
            positions.push_back(size_t(found));
            pos = size_t(found) + 4;
        }
    }
    std::sort(positions.begin(), positions.end());
    for (size_t i = 0; i < positions.size(); ++i) {
        const size_t cpos = positions[i];
        if (cpos + 12 > n) continue;
        SubEntry e;
        e.offset = cpos;
        e.size = cpos >= 4 ? le32(d, cpos - 4) : 0;
        e.key = le32(d, cpos + 4);
        std::string ext;
        if (ascii_ext(d + cpos + 8, ext)) {
            e.ext = std::move(ext);
            e.gro_null = true;
        } else {
            e.gro_type = le32(d, cpos + 8);
        }
        const size_t payload_start = cpos + 12;
        const size_t payload_end = i + 1 < positions.size()
                                       ? positions[i + 1] - 4
                                       : n;
        if (payload_end >= payload_start)
            e.data.assign(d + payload_start, d + payload_end);
        out.push_back(std::move(e));
    }
    return out;
}

SubEntryBounds find_sub_entry_bounds(const uint8_t* d, size_t n,
                                     uint32_t target_key) {
    SubEntryBounds result;
    if (!d) return result;
    const std::vector<SubEntry> entries = parse_sub_entries(d, n);
    for (const SubEntry& entry : entries) {
        if (entry.key != target_key) continue;
        result.found = true;
        result.cookie_pos = entry.offset;
        result.payload_start = entry.offset + 12;
        result.payload_end = result.payload_start + entry.data.size();
        return result;
    }
    return result;
}

std::vector<SubEntry> walk_sub_entries(const uint8_t* d, size_t n) {
    std::vector<SubEntry> out;
    if (!d || n < 12) return out;
    long long first = find_first_subentry(d, n);
    if (first < 0) return out;

    size_t off = static_cast<size_t>(first);
    while (off + 12 <= n) {
        uint32_t size = le32(d, off);
        uint32_t cookie = le32(d, off + 4);
        if (cookie != JADE_COOKIE && cookie != JADE_COOKIE_ALT) break;
        if (off + 12 + size > n) break;

        SubEntry e;
        e.offset = off + 4;
        e.size = size;
        e.key = le32(d, off + 8);
        if (size >= 4) {
            std::string ext;
            if (ascii_ext(d + off + 12, ext)) {
                e.ext = std::move(ext);
                e.gro_null = true;
            } else {
                e.gro_null = false;
                e.gro_type = le32(d, off + 12);
            }
            e.data.assign(d + off + 16, d + off + 12 + size);
        } else {
            e.gro_null = false;
            e.gro_type = 0;
        }
        out.push_back(std::move(e));
        off += 12 + size;
    }
    return out;
}

}  // namespace jade
