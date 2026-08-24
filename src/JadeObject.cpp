// JadeObject.cpp — implementation. Faithful port of core/jade_object.py.
#include "jade/JadeObject.hpp"

namespace jade {

namespace {
inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}
inline void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
}  // namespace

JadeObjectStream parse_jade_object(const uint8_t* d, size_t n) {
    JadeObjectStream s;
    if (!d || n < JOS_HEADER_SIZE) return s;
    s.header[0] = le32(d, 0);
    s.header[1] = le32(d, 4);
    s.header[2] = le32(d, 8);

    const uint8_t* body = d + JOS_HEADER_SIZE;
    size_t body_len = n - JOS_HEADER_SIZE;
    uint32_t region = s.header[1];

    size_t pair_bytes_len;
    if (region >= JOS_BLOB_HEADER_SIZE && region <= body_len &&
        (region - JOS_BLOB_HEADER_SIZE) % JOS_PAIR_SIZE == 0) {
        pair_bytes_len = region - JOS_BLOB_HEADER_SIZE;
        s.has_blob_header = true;
        s.blob_header.assign(body + pair_bytes_len, body + region);
        s.has_blob = true;
        s.blob.assign(body + region, body + body_len);
    } else if (body_len % JOS_PAIR_SIZE == 0) {
        pair_bytes_len = body_len;  // no trailing blob
    } else {
        return s;  // ok == false
    }

    for (size_t i = 0; i + JOS_PAIR_SIZE <= pair_bytes_len; i += JOS_PAIR_SIZE)
        s.pairs.emplace_back(le32(body, i), le32(body, i + 4));
    s.ok = true;
    return s;
}

std::vector<uint8_t> serialize_jade_object(const JadeObjectStream& s) {
    std::vector<uint8_t> out;
    put32(out, s.header[0]);
    put32(out, s.header[1]);
    put32(out, s.header[2]);
    for (const auto& p : s.pairs) { put32(out, p.first); put32(out, p.second); }
    if (s.has_blob_header) out.insert(out.end(), s.blob_header.begin(), s.blob_header.end());
    if (s.has_blob) out.insert(out.end(), s.blob.begin(), s.blob.end());
    return out;
}

std::vector<size_t> find_special_load_units(const JadeObjectStream& s) {
    std::vector<size_t> starts;
    const size_t U = SPECIAL_LOAD_UNIT_TAGS.size();
    if (s.pairs.size() < U) return starts;
    for (size_t i = 0; i + U <= s.pairs.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < U; ++k) {
            if (s.pairs[i + k].first != SPECIAL_LOAD_UNIT_TAGS[k]) { match = false; break; }
        }
        if (match) starts.push_back(i);
    }
    return starts;
}

}  // namespace jade
