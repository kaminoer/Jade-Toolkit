// Rli.cpp — implementation. Faithful port of core/rli.py (read + write paths).
#include "jade/Rli.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace jade {

namespace {

inline uint16_t le16(const uint8_t* p, size_t o) {
    return static_cast<uint16_t>(p[o] | (p[o + 1] << 8));
}
inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}
inline float lef(const uint8_t* p, size_t o) {
    uint32_t v = le32(p, o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

constexpr uint32_t GAO_ID_VISUAL = 0x00004000;
constexpr uint32_t GAO_ID_OBBOX  = 0x00080000;

inline RgbaColor bgra_at(const uint8_t* d, size_t o) {
    // _bgra_at: stored as B,G,R,A -> return (r,g,b,a)
    RgbaColor c;
    c.b = d[o]; c.g = d[o + 1]; c.r = d[o + 2]; c.a = d[o + 3];
    return c;
}

}  // namespace

long long visual_block_offset(const uint8_t* gao, size_t n) {
    if (n < 16) return -1;                       // parse_gao_header -> None
    uint32_t identity = le32(gao, 8);
    if (!(identity & GAO_ID_VISUAL)) return -1;
    uint32_t name_size = le32(gao, 12);
    size_t mat_off = 16 + static_cast<size_t>(name_size) + 10;
    bool has_obbox = (identity & GAO_ID_OBBOX) != 0;
    size_t bv_size = has_obbox ? 48 : 24;
    size_t vis_off = mat_off + 68 + bv_size;
    if (vis_off + 8 > n) return -1;
    return static_cast<long long>(vis_off);
}

bool is_real_color_table(const uint8_t* d, size_t len, size_t cs, uint32_t nb) {
    size_t end = cs + static_cast<size_t>(nb) * 4;
    if (end + 4 > len) return false;
    if (le32(d, end) != 1) return false;
    uint32_t cap = nb < 64 ? nb : 64;
    if (cap == 0) return false;                  // n==0 -> Python ZeroDivision avoided
    uint32_t good = 0;
    for (uint32_t i = 0; i < cap; ++i) {
        uint8_t a = d[cs + static_cast<size_t>(i) * 4 + 3];
        if (a == 0xFD || a == 0xFE || a == 0xFF) ++good;
    }
    return static_cast<double>(good) / cap >= 0.9;
}

long long find_primary_table(const uint8_t* gao, size_t n, uint32_t nb) {
    long long vo = visual_block_offset(gao, n);
    if (vo < 0) return -1;
    size_t lo = static_cast<size_t>(vo);
    // range(vo, min(vo+64, len-6)) — empty if len < 6.
    if (n < 6) return -1;
    size_t hi = lo + 64;
    if (hi > n - 6) hi = n - 6;
    for (size_t off = lo; off < hi; ++off) {
        if (gao[off] == 0xFF && gao[off + 1] == 0xFF &&
            le32(gao, off + 2) == nb) {
            size_t cs = off + 6;
            if (is_real_color_table(gao, n, cs, nb))
                return static_cast<long long>(cs);
        }
    }
    return -1;
}

bool find_extra_block(const uint8_t* gao, size_t n, size_t primary_end,
                      size_t& entries_start, uint32_t& count_exp) {
    for (size_t w = 0; w < 80; w += 4) {
        size_t o = primary_end + w;
        if (o + 16 > n) break;
        uint32_t size = le32(gao, o + 4);
        uint32_t cnt = le32(gao, o + 8);
        uint32_t stride = le32(gao, o + 12);
        if (stride == 12 && cnt > 0 && cnt <= 300000 &&
            size == 8 + cnt * 12 &&
            o + 16 + static_cast<size_t>(cnt) * 12 <= n) {
            entries_start = o + 16;
            count_exp = cnt;
            return true;
        }
    }
    return false;
}

bool has_rli(const uint8_t* gao, size_t n, uint32_t nb) {
    return find_primary_table(gao, n, nb) >= 0;
}

PrimaryColors read_primary_colors(const uint8_t* gao, size_t n, uint32_t nb) {
    PrimaryColors out;
    long long cs = find_primary_table(gao, n, nb);
    if (cs < 0) return out;
    out.ok = true;
    out.colors.reserve(nb);
    for (uint32_t i = 0; i < nb; ++i)
        out.colors.push_back(bgra_at(gao, static_cast<size_t>(cs) + static_cast<size_t>(i) * 4));
    return out;
}

ExtraColors read_extra_colors(const uint8_t* gao, size_t n, uint32_t nb) {
    ExtraColors out;
    long long cs = find_primary_table(gao, n, nb);
    if (cs < 0) return out;
    size_t es;
    uint32_t cnt;
    if (!find_extra_block(gao, n, static_cast<size_t>(cs) + static_cast<size_t>(nb) * 4, es, cnt))
        return out;
    out.ok = true;
    out.entries_start = es;
    out.count_exp = cnt;
    out.colors.reserve(cnt);
    for (uint32_t j = 0; j < cnt; ++j)
        out.colors.push_back(bgra_at(gao, es + static_cast<size_t>(j) * 12));
    return out;
}

CookedVbSection cooked_vb_section(const uint8_t* geo, size_t n) {
    CookedVbSection best;
    if (n < 12) return best;
    // range(40, len-12, 4)
    for (size_t off = 40; off + 12 < n; off += 4) {
        uint32_t m = le32(geo, off);
        uint32_t cnt = le32(geo, off + 4);
        uint32_t st = le32(geo, off + 8);
        bool st_ok = (st == 20 || st == 24 || st == 28 || st == 32 || st == 44);
        if (m <= 8 && st_ok && cnt >= 1 && cnt <= 400000 &&
            off + 12 + static_cast<size_t>(cnt) * st <= n + 8) {
            if (!best.ok || cnt > best.n) {
                best.ok = true;
                best.data_start = off + 12;
                best.n = cnt;
                best.stride = st;
            }
        }
    }
    return best;
}

CookedMesh cooked_mesh(const uint8_t* geo, size_t n) {
    CookedMesh out;
    CookedVbSection sec = cooked_vb_section(geo, n);
    if (!sec.ok) return out;
    out.n = sec.n;
    out.positions.reserve(static_cast<size_t>(sec.n) * 3);
    out.uvs.reserve(static_cast<size_t>(sec.n) * 2);
    // _cooked_vb_section accepts a VB whose last vertex can spill up to 8 bytes
    // past the payload (`<= n+8`). The Python reads each vertex with
    // struct.unpack_from('<3f') (pos) + '<2f' (uv), which RAISES struct.error
    // when those bytes run off the buffer — i.e. the whole cooked_mesh call
    // fails for that GEO. dump_rli catches the raise as None, so we mirror it by
    // returning a not-ok (null) mesh on the first out-of-bounds read.
    for (uint32_t j = 0; j < sec.n; ++j) {
        size_t o = sec.data_start + static_cast<size_t>(j) * sec.stride;
        if (o + 12 > n) return CookedMesh{};            // pos '<3f' OOB -> Python raises
        out.positions.push_back(lef(geo, o));
        out.positions.push_back(lef(geo, o + 4));
        out.positions.push_back(lef(geo, o + 8));
        if (sec.stride >= 20) {
            if (o + 20 > n) return CookedMesh{};         // uv '<2f' OOB -> Python raises
            out.uvs.push_back(lef(geo, o + 12));
            out.uvs.push_back(lef(geo, o + 16));
        } else {
            out.uvs.push_back(0.0f);
            out.uvs.push_back(0.0f);
        }
    }
    out.ok = true;
    size_t vb_end = sec.data_start + static_cast<size_t>(sec.n) * sec.stride;
    if (vb_end + 4 <= n) {
        uint32_t ib_bytes = le32(geo, vb_end);
        if (ib_bytes % 2 == 0 && vb_end + 4 + ib_bytes <= n) {
            uint32_t ni = ib_bytes / 2;
            uint32_t ntri = ni / 3;
            out.faces.reserve(static_cast<size_t>(ntri) * 3);
            for (uint32_t t = 0; t < ntri; ++t) {
                size_t base = vb_end + 4 + static_cast<size_t>(t * 3) * 2;
                out.faces.push_back(le16(geo, base));
                out.faces.push_back(le16(geo, base + 2));
                out.faces.push_back(le16(geo, base + 4));
            }
        }
    }
    return out;
}

ExpandedToBase expanded_to_base(const uint8_t* geo, size_t gn, uint32_t nb) {
    ExpandedToBase out;
    CookedVbSection sec = cooked_vb_section(geo, gn);
    if (!sec.ok) return out;

    // _base_point_keys: position bytes -> base index, first wins (setdefault).
    struct Key12 {
        std::array<uint8_t, 12> b;
        bool operator==(const Key12& o) const { return b == o.b; }
    };
    struct Key12Hash {
        size_t operator()(const Key12& k) const {
            size_t h = 1469598103934665603ull;
            for (uint8_t c : k.b) { h ^= c; h *= 1099511628211ull; }
            return h;
        }
    };
    std::unordered_map<Key12, uint32_t, Key12Hash> base_keys;
    for (uint32_t i = 0; i < nb; ++i) {
        size_t o = 40 + static_cast<size_t>(i) * 12;
        if (o + 12 > gn) break;
        Key12 k;
        std::memcpy(k.b.data(), geo + o, 12);
        base_keys.emplace(k, i);   // emplace = setdefault (first occurrence wins)
    }
    if (base_keys.empty()) return out;

    out.ok = true;
    out.map.reserve(sec.n);
    for (uint32_t j = 0; j < sec.n; ++j) {
        size_t o = sec.data_start + static_cast<size_t>(j) * sec.stride;
        Key12 k;
        std::memcpy(k.b.data(), geo + o, 12);
        auto it = base_keys.find(k);
        out.map.push_back(it == base_keys.end() ? 0u : it->second);
    }
    return out;
}

// ── write helpers ──────────────────────────────────────────────────────────

namespace {

// _pack_bgra's per-channel clamp: int(round(c)) then max(0,min(255,·)). round()
// is Python's half-to-even, which std::nearbyint reproduces under the default
// FE_TONEAREST rounding mode.
inline uint8_t clamp_round(double c) {
    int v = static_cast<int>(std::nearbyint(c));
    return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
}
// Write (b,g,r) at o/o+1/o+2 from an (r,g,b) colour; alpha at o+3 is preserved.
inline void pack_bgra(std::vector<uint8_t>& out, size_t o, double r, double g, double b) {
    out[o]     = clamp_round(b);
    out[o + 1] = clamp_round(g);
    out[o + 2] = clamp_round(r);
}
inline void put_le32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = x & 0xFF; v[o + 1] = (x >> 8) & 0xFF;
    v[o + 2] = (x >> 16) & 0xFF; v[o + 3] = (x >> 24) & 0xFF;
}

}  // namespace

std::vector<uint8_t> transform_rli(const uint8_t* gao, size_t n, uint32_t nb,
                                   const RliColorFn& color_fn) {
    long long cs = find_primary_table(gao, n, nb);
    if (cs < 0) return {};
    std::vector<uint8_t> out(gao, gao + n);
    auto apply = [&](size_t o) {
        int r = out[o + 2], g = out[o + 1], b = out[o];    // stored BGRA -> (r,g,b)
        std::array<int, 3> c = color_fn(r, g, b);
        pack_bgra(out, o, c[0], c[1], c[2]);
    };
    for (uint32_t i = 0; i < nb; ++i) apply(static_cast<size_t>(cs) + static_cast<size_t>(i) * 4);
    size_t es;
    uint32_t cnt;
    if (find_extra_block(out.data(), out.size(),
                         static_cast<size_t>(cs) + static_cast<size_t>(nb) * 4, es, cnt)) {
        for (uint32_t j = 0; j < cnt; ++j) apply(es + static_cast<size_t>(j) * 12);
    }
    if (std::equal(out.begin(), out.end(), gao)) return {};   // no net change -> None
    return out;
}

std::vector<uint8_t> write_rli(const uint8_t* gao, size_t n, uint32_t nb,
                               const std::vector<RgbF>& base_colors,
                               const uint8_t* geo, size_t geo_n, bool update_extra) {
    long long cs = find_primary_table(gao, n, nb);
    if (cs < 0 || base_colors.size() < nb) return {};
    std::vector<uint8_t> out(gao, gao + n);
    for (uint32_t i = 0; i < nb; ++i) {
        size_t o = static_cast<size_t>(cs) + static_cast<size_t>(i) * 4;
        pack_bgra(out, o, base_colors[i][0], base_colors[i][1], base_colors[i][2]);
    }
    if (update_extra) {
        size_t es;
        uint32_t cnt;
        if (find_extra_block(out.data(), out.size(),
                             static_cast<size_t>(cs) + static_cast<size_t>(nb) * 4, es, cnt)) {
            ExpandedToBase e2b = (geo != nullptr) ? expanded_to_base(geo, geo_n, nb) : ExpandedToBase{};
            for (uint32_t j = 0; j < cnt; ++j) {
                uint32_t bi;
                if (e2b.ok && j < e2b.map.size()) bi = e2b.map[j];
                else bi = (j < nb) ? j : 0;
                if (bi >= nb) bi = 0;
                size_t o = es + static_cast<size_t>(j) * 12;
                pack_bgra(out, o, base_colors[bi][0], base_colors[bi][1], base_colors[bi][2]);
            }
        }
    }
    if (std::equal(out.begin(), out.end(), gao)) return {};
    return out;
}

std::vector<uint8_t> write_extra_colors(const uint8_t* gao, size_t n, uint32_t nb,
                                        const std::vector<RgbF>& cooked_colors,
                                        const uint8_t* geo, size_t geo_n) {
    long long cs = find_primary_table(gao, n, nb);
    if (cs < 0) return {};
    size_t es;
    uint32_t cnt;
    if (!find_extra_block(gao, n, static_cast<size_t>(cs) + static_cast<size_t>(nb) * 4, es, cnt))
        return {};
    if (cooked_colors.size() < cnt) return {};
    std::vector<uint8_t> out(gao, gao + n);
    for (uint32_t j = 0; j < cnt; ++j) {
        size_t o = es + static_cast<size_t>(j) * 12;
        pack_bgra(out, o, cooked_colors[j][0], cooked_colors[j][1], cooked_colors[j][2]);
    }
    if (geo != nullptr) {                                 // refresh primary (base order)
        ExpandedToBase e2b = expanded_to_base(geo, geo_n, nb);
        if (e2b.ok) {
            std::vector<char> seen(nb, 0);
            std::vector<RgbF> base_col(nb);
            uint32_t lim = std::min<uint32_t>(cnt, static_cast<uint32_t>(e2b.map.size()));
            for (uint32_t j = 0; j < lim; ++j) {          // setdefault: first cooked wins
                uint32_t bidx = e2b.map[j];
                if (bidx < nb && !seen[bidx]) { seen[bidx] = 1; base_col[bidx] = cooked_colors[j]; }
            }
            for (uint32_t i = 0; i < nb; ++i) {
                if (!seen[i]) continue;
                size_t o = static_cast<size_t>(cs) + static_cast<size_t>(i) * 4;
                pack_bgra(out, o, base_col[i][0], base_col[i][1], base_col[i][2]);
            }
        }
    }
    if (std::equal(out.begin(), out.end(), gao)) return {};
    return out;
}

std::vector<uint8_t> write_extra_uv1(const uint8_t* gao, size_t n, uint32_t nb,
                                     const std::vector<std::array<float, 2>>& cooked_uv1) {
    long long cs = find_primary_table(gao, n, nb);
    if (cs < 0) return {};
    size_t es;
    uint32_t cnt;
    if (!find_extra_block(gao, n, static_cast<size_t>(cs) + static_cast<size_t>(nb) * 4, es, cnt))
        return {};
    if (cooked_uv1.size() < cnt) return {};
    std::vector<uint8_t> out(gao, gao + n);
    for (uint32_t j = 0; j < cnt; ++j) {
        size_t o = es + static_cast<size_t>(j) * 12 + 4;   // the two trailing floats
        uint32_t bu, bv;
        float u = cooked_uv1[j][0], v = cooked_uv1[j][1];
        std::memcpy(&bu, &u, 4);
        std::memcpy(&bv, &v, 4);
        put_le32(out, o, bu);
        put_le32(out, o + 4, bv);
    }
    if (std::equal(out.begin(), out.end(), gao)) return {};
    return out;
}

}  // namespace jade
