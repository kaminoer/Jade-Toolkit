// WolInfo.cpp — implementation. Faithful port of core/wolinfo.py.
#include "jade/WolInfo.hpp"

namespace jade {

namespace {
inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}
inline void put_le32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = x & 0xFF; v[o + 1] = (x >> 8) & 0xFF;
    v[o + 2] = (x >> 16) & 0xFF; v[o + 3] = (x >> 24) & 0xFF;
}
}  // namespace

WolInfo parse_wolinfo(const uint8_t* d, size_t n) {
    WolInfo w;
    if (!d || n < 8) return w;
    w.raw.assign(d, d + n);
    w.total_len = le32(d, 0);
    // payload begins after the 4-byte self-length prefix.
    const uint8_t* p = d + 4;
    size_t pn = n - 4;
    w.count = le32(p, 0);
    size_t off = 4;
    for (uint32_t i = 0; i < w.count; ++i) {
        if (off + 8 > pn) return WolInfo{};        // truncated -> ok=false
        WolEntry e;
        e.wol_key = le32(p, off);
        uint32_t cnt = le32(p, off + 4);
        e.deps_off = off + 8;
        e.end = e.deps_off + 4u * cnt;
        if (cnt > 1000000u || e.end > pn) return WolInfo{};
        e.deps.reserve(cnt);
        for (uint32_t j = 0; j < cnt; ++j) e.deps.push_back(le32(p, e.deps_off + 4u * j));
        e.start = off;
        e.n_off = off + 4;
        w.entries.push_back(std::move(e));
        off = w.entries.back().end;
    }
    w.entries_end = off;
    w.trailing_len = pn - off;
    w.ok = true;
    return w;
}

const WolEntry* WolInfo::find(uint32_t wol_key) const {
    for (const WolEntry& e : entries)
        if (e.wol_key == wol_key) return &e;
    return nullptr;
}

bool WolInfo::add_dep(uint32_t wol_key, uint32_t dep_key, bool has_before, uint32_t before_key) {
    const WolEntry* e = find(wol_key);
    if (e == nullptr) return false;
    for (uint32_t d : e->deps) if (d == dep_key) return false;

    // Capture the fields we need before the splice invalidates `e`.
    size_t ins = e->deps.size();
    if (ins > 0) --ins;                            // default: before the last dep
    if (has_before) {
        for (size_t i = 0; i < e->deps.size(); ++i)
            if (e->deps[i] == before_key) { ins = i; break; }
    }
    size_t deps_off = e->deps_off;
    size_t n_off = e->n_off;
    uint32_t new_n = static_cast<uint32_t>(e->deps.size()) + 1;
    size_t abs_ins = deps_off + 4 * ins;           // payload offset

    std::vector<uint8_t> p(raw.begin() + 4, raw.end());   // payload copy
    uint8_t db[4] = {static_cast<uint8_t>(dep_key), static_cast<uint8_t>(dep_key >> 8),
                     static_cast<uint8_t>(dep_key >> 16), static_cast<uint8_t>(dep_key >> 24)};
    p.insert(p.begin() + abs_ins, db, db + 4);
    put_le32(p, n_off, new_n);

    std::vector<uint8_t> nr;
    nr.reserve(p.size() + 4);
    uint32_t tl = static_cast<uint32_t>(p.size()) + 4;
    nr.push_back(tl & 0xFF); nr.push_back((tl >> 8) & 0xFF);
    nr.push_back((tl >> 16) & 0xFF); nr.push_back((tl >> 24) & 0xFF);
    nr.insert(nr.end(), p.begin(), p.end());

    *this = parse_wolinfo(nr.data(), nr.size());   // refresh offsets/entries
    return true;
}

bool WolInfo::set_deps(uint32_t wol_key, const std::vector<uint32_t>& new_deps) {
    const WolEntry* e = find(wol_key);
    if (e == nullptr) return false;
    size_t deps_off = e->deps_off, end = e->end, n_off = e->n_off;

    std::vector<uint8_t> p(raw.begin() + 4, raw.end());          // payload copy
    std::vector<uint8_t> blob;
    blob.reserve(new_deps.size() * 4);
    for (uint32_t d : new_deps) {
        blob.push_back(d & 0xFF); blob.push_back((d >> 8) & 0xFF);
        blob.push_back((d >> 16) & 0xFF); blob.push_back((d >> 24) & 0xFF);
    }
    // Replace the dep region [deps_off, end) with the new blob, then update N.
    p.erase(p.begin() + static_cast<long>(deps_off), p.begin() + static_cast<long>(end));
    p.insert(p.begin() + static_cast<long>(deps_off), blob.begin(), blob.end());
    put_le32(p, n_off, static_cast<uint32_t>(new_deps.size()));

    std::vector<uint8_t> nr;
    nr.reserve(p.size() + 4);
    uint32_t tl = static_cast<uint32_t>(p.size()) + 4;
    nr.push_back(tl & 0xFF); nr.push_back((tl >> 8) & 0xFF);
    nr.push_back((tl >> 16) & 0xFF); nr.push_back((tl >> 24) & 0xFF);
    nr.insert(nr.end(), p.begin(), p.end());

    *this = parse_wolinfo(nr.data(), nr.size());
    return true;
}

}  // namespace jade
