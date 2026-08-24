// Jtmod.cpp — implementation. Port of jade_explorer/build/jtmod.py (read side).
#include "jade/Jtmod.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "jade/Sha256.hpp"
#include "jade/SubEntry.hpp"

namespace jade {
namespace jtmod {

namespace {

// Integrity trailer: MAGIC(8) + sha256(SALT + content). Mirrors jtmod.py.
const uint8_t kIntegMagic[8] = {'J', 'T', 'M', 'H', 'A', 'S', 'H', '1'};
const char kIntegSalt[] = "jtmod-integrity-v1";   // 18 chars; domain separator

// Sequential little-endian reader with bounds checking (like the C++ patcher's).
struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    Reader(const uint8_t* d, size_t len) : p(d), n(len) {}

    void need(size_t k) {
        if (i + k > n) throw std::runtime_error(".jtmod: truncated");
    }
    uint8_t u8() { need(1); return p[i++]; }
    uint32_t u32() {
        need(4);
        uint32_t v = uint32_t(p[i]) | (uint32_t(p[i + 1]) << 8) |
                     (uint32_t(p[i + 2]) << 16) | (uint32_t(p[i + 3]) << 24);
        i += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t lo = u32(), hi = u32();
        return lo | (hi << 32);
    }
    std::string str() {
        uint32_t len = u32();
        need(len);
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
    std::vector<uint8_t> blob() {
        uint32_t len = u32();
        need(len);
        std::vector<uint8_t> v(p + i, p + i + len);
        i += len;
        return v;
    }
    void raw(uint8_t* out, size_t k) { need(k); std::copy(p + i, p + i + k, out); i += k; }
};

const char* kHex = "0123456789abcdef";

std::string to_hex(const uint8_t* b, size_t n) {
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(kHex[b[i] >> 4]);
        s.push_back(kHex[b[i] & 0xf]);
    }
    return s;
}

}  // namespace

Jtmod parse(const uint8_t* data, size_t len) {
    Reader r(data, len);
    char magic[8];
    r.raw(reinterpret_cast<uint8_t*>(magic), 8);
    static const char kMagic[8] = {'J', 'A', 'D', 'E', 'J', 'T', 'M', '1'};
    for (int k = 0; k < 8; ++k)
        if (magic[k] != kMagic[k])
            throw std::runtime_error("not a .jtmod (bad magic)");

    Jtmod m;
    m.format_version = r.u32();
    r.u32();  // flags (reserved)
    if (m.format_version != 1 && m.format_version != 2 && m.format_version != 3)
        throw std::runtime_error("unsupported .jtmod version");

    m.game = r.str();
    m.archive_name = r.str();
    m.base_size = r.u64();
    uint8_t sha[32];
    r.raw(sha, 32);
    m.base_sha256 = to_hex(sha, 32);
    m.name = r.str();
    m.author = r.str();
    m.version_text = r.str();
    m.description = r.str();
    m.created = r.str();
    m.tool = r.str();
    if (m.format_version >= 2) m.image = r.blob();

    uint32_t mk = r.u32();
    m.minted_keys.reserve(mk);
    for (uint32_t k = 0; k < mk; ++k) m.minted_keys.push_back(r.u32());

    uint32_t bin_count = r.u32();
    m.bins.reserve(bin_count);
    for (uint32_t b = 0; b < bin_count; ++b) {
        BinDelta d;
        d.bin_key = r.u32();
        d.mode = r.u8();
        if (d.mode == MODE_SUBENTRY) {
            d.base_record_count = r.u32();
            if (m.format_version >= 3) { d.base_hash.resize(32); r.raw(d.base_hash.data(), 32); }
            uint32_t hc = r.u32();
            d.hunks.reserve(hc);
            for (uint32_t h = 0; h < hc; ++h) {
                Hunk hk;
                hk.base_start = r.u32();
                hk.base_end = r.u32();
                uint32_t nr = r.u32();
                hk.records.reserve(nr);
                for (uint32_t k = 0; k < nr; ++k) hk.records.push_back(r.blob());
                d.hunks.push_back(std::move(hk));
            }
        } else if (d.mode == MODE_WHOLEBIN) {
            d.payload = r.blob();
        } else if (d.mode == MODE_NEWBIN) {
            d.name = r.str();
            d.parent_dir_idx = r.u32();
            d.payload = r.blob();
        } else {
            throw std::runtime_error(".jtmod: unknown bin mode");
        }
        m.bins.push_back(std::move(d));
    }
    return m;
}

Integrity check_integrity(const uint8_t* data, size_t len) {
    const size_t kTrailer = 8 + 32;
    if (len < kTrailer) return Integrity::Missing;
    const uint8_t* tr = data + (len - kTrailer);
    if (std::memcmp(tr, kIntegMagic, 8) != 0) return Integrity::Missing;
    size_t content = len - kTrailer;
    sha_detail::Ctx c;
    sha_detail::init(c);
    sha_detail::update(c, reinterpret_cast<const uint8_t*>(kIntegSalt), sizeof(kIntegSalt) - 1);
    sha_detail::update(c, data, content);
    uint8_t out[32];
    sha_detail::finish(c, out);
    return std::memcmp(out, tr + 8, 32) == 0 ? Integrity::Ok : Integrity::Mismatch;
}

SplitResult split_records(const uint8_t* dec, size_t n) {
    SplitResult out;
    std::vector<SubEntry> recs = walk_sub_entries(dec, n);
    if (recs.empty()) return out;  // prefix empty, no records, clean=false

    size_t first = recs[0].offset - 4;  // record start of the first record
    out.prefix.assign(dec, dec + first);

    size_t total = first;
    for (const SubEntry& e : recs) {
        size_t start = e.offset - 4;
        size_t reclen = 12 + e.size;
        out.records.emplace_back(dec + start, dec + start + reclen);
        total += reclen;
    }
    // Records are contiguous from `first` (walk steps 12+size), so this equality
    // is exactly the Python's prefix + b"".join(records) == dec.
    out.clean = (total == n);
    return out;
}

std::vector<Record> apply_hunks(const std::vector<Record>& base_records,
                                const std::vector<Hunk>& hunks) {
    std::vector<Record> out = base_records;
    std::vector<const Hunk*> sorted;
    sorted.reserve(hunks.size());
    for (const Hunk& h : hunks) sorted.push_back(&h);
    std::sort(sorted.begin(), sorted.end(),
              [](const Hunk* a, const Hunk* b) { return a->base_start > b->base_start; });
    for (const Hunk* h : sorted) {
        // Clamp indices to the actual record count. Normally they're exact (base
        // matches), but when force-applying onto a *differing* bin this prevents
        // an out-of-range splice (UB) — worst case the hunk lands at the end.
        size_t n = out.size();
        uint32_t hi = h->base_end > h->base_start ? h->base_end : h->base_start;
        size_t s = size_t(h->base_start) < n ? size_t(h->base_start) : n;
        size_t e = size_t(hi) < n ? size_t(hi) : n;
        if (e < s) e = s;
        out.erase(out.begin() + s, out.begin() + e);
        out.insert(out.begin() + s, h->records.begin(), h->records.end());
    }
    return out;
}

bool hunks_overlap(const Hunk& a, const Hunk& b) {
    uint32_t as = a.base_start, ae = a.base_end, bs = b.base_start, be = b.base_end;
    if (as == ae && bs == be) return as == bs;  // two inserts
    if (as == ae) return bs < as && as < be;     // a is an insert inside b
    if (bs == be) return as < bs && bs < ae;     // b is an insert inside a
    return as < be && bs < ae;
}

}  // namespace jtmod
}  // namespace jade
