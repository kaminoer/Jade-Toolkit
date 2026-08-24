// Gao.cpp — implementation. Faithful port of core/gao.py parse_gao_full.
#include "jade/Gao.hpp"

#include <algorithm>
#include <cstring>

namespace jade {

namespace {

inline uint16_t le16(const uint8_t* p, size_t o) {
    return static_cast<uint16_t>(p[o] | (p[o + 1] << 8));
}
inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}

// ascii+replace decode (bytes >= 0x80 -> U+FFFD), matching the Python name field.
std::string decode_ascii_replace(const uint8_t* d, size_t a, size_t b) {
    std::string out;
    for (size_t i = a; i < b; ++i) {
        uint8_t c = d[i];
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else { out.push_back('\xEF'); out.push_back('\xBF'); out.push_back('\xBD'); }
    }
    return out;
}

}  // namespace

GaoInfo parse_gao_full(const uint8_t* d, size_t n) {
    GaoInfo g;
    if (n < 16) return g;

    g.version      = le32(d, 0);
    g.editor_flags = le32(d, 4);
    g.identity     = le32(d, 8);

    // name: first NUL at index >= 16, accepted only if 16 < end < 136.
    size_t end = 16;
    while (end < n && d[end] != 0) ++end;
    bool found = (end < n);  // a NUL was hit before EOF
    if (found && end > 16 && end < 16 + 120)
        g.name = decode_ascii_replace(d, 16, end);

    uint32_t ident = g.identity;
    g.name_size = le32(d, 12);
    size_t post_name = 16 + static_cast<size_t>(g.name_size);
    size_t mat_off = post_name + 10;

    if (mat_off + 68 <= n) {
        g.gmat_present = true;
        g.gmat_raw.assign(d + mat_off, d + mat_off + 68);
        g.gmat_type = le32(d, mat_off + 64);
    }

    bool has_obbox = (ident & GAO_ID_OBBOX) != 0;
    size_t bv_size = has_obbox ? 48 : 24;
    size_t off = mat_off + 68 + bv_size;

    if (ident & GAO_ID_VISUAL) {
        g.vis_flag = true;
        if (off + 16 <= n) {
            g.vis_read = true;
            g.gro_key = le32(d, off);
            g.grm_key = le32(d, off + 4);
        }
        uint32_t vis_size = 42;
        if (off + 42 <= n) {
            uint16_t extra_count = le16(d, off + 38);
            if (extra_count > 0 && off + 46 <= n) {
                uint32_t remaining = le32(d, off + 42);
                if (remaining < n) vis_size = 46 + remaining;
            }
        }
        g.vis_size = vis_size;
        off += vis_size;
    }

    if (ident & GAO_ID_HIERARCHY) {
        g.hier_flag = true;
        if (off + 72 <= n) {
            g.hier_read = true;
            g.father_key = le32(d, off);
            g.lmat_present = true;
            g.lmat_raw.assign(d + off + 4, d + off + 4 + 68);
        } else {
            g.father_key = 0xFFFFFFFFu;
        }
        off += 72;
    }

    if (ident & GAO_ID_ADDMATRIX) {
        if (ident & GAO_ID_BIT24) {
            if (off + 4 <= n) {
                uint32_t gizmo_count = le32(d, off);
                if (gizmo_count > 0 && gizmo_count < 1000 &&
                    off + 4 + static_cast<size_t>(gizmo_count) * 8 <= n) {
                    g.gizmo_flat.reserve(static_cast<size_t>(gizmo_count) * 2);
                    for (uint32_t gi = 0; gi < gizmo_count; ++gi) {
                        g.gizmo_flat.push_back(le32(d, off + 4 + gi * 8));
                        g.gizmo_flat.push_back(le32(d, off + 4 + gi * 8 + 4));
                    }
                }
            }
        }
    }

    g.ok = true;
    return g;
}

GaoInfo parse_gao_record(const uint8_t* d, size_t n) {
    if (n < 4 || d[0] != '.' || d[1] != 'g' || d[2] != 'a' || d[3] != 'o')
        return GaoInfo{};
    return parse_gao_full(d + 4, n - 4);
}

namespace {
// A sub-entry "is gro_type 1" only when it has no ASCII ext (gro_type present).
inline bool is_geo_sub(const SubEntry* s) {
    return s && !s->gro_null && s->gro_type == 1;
}
}  // namespace

std::vector<uint32_t> geo_group_members(
    uint32_t gro_key,
    const std::unordered_map<uint32_t, const SubEntry*>& subs_by_key,
    const std::unordered_set<uint32_t>* geo_keys) {
    auto it = subs_by_key.find(gro_key);
    const SubEntry* sub = (it == subs_by_key.end()) ? nullptr : it->second;
    if (sub == nullptr || is_geo_sub(sub)) return {gro_key};

    const std::vector<uint8_t>& data = sub->data;
    if (data.size() < 4) return {gro_key};

    std::unordered_set<uint32_t> local;
    if (geo_keys == nullptr) {
        for (const auto& kv : subs_by_key)
            if (is_geo_sub(kv.second)) local.insert(kv.first);
        geo_keys = &local;
    }
    if (geo_keys->empty()) return {gro_key};

    std::vector<uint32_t> members;
    std::unordered_set<uint32_t> seen;
    for (size_t o = 0; o + 4 <= data.size(); ++o) {
        uint32_t v = le32(data.data(), o);
        if (geo_keys->count(v) && !seen.count(v)) {
            seen.insert(v);
            members.push_back(v);
        }
    }
    return members.empty() ? std::vector<uint32_t>{gro_key} : members;
}

// ── write helpers ──────────────────────────────────────────────────────────

namespace {
inline float rd_f32(const uint8_t* d, size_t o) {
    uint32_t b = static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
                 (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline void wr_f32(std::vector<uint8_t>& v, size_t o, double x) {
    float f = static_cast<float>(x);
    uint32_t b;
    std::memcpy(&b, &f, 4);
    v[o] = b & 0xFF; v[o + 1] = (b >> 8) & 0xFF;
    v[o + 2] = (b >> 16) & 0xFF; v[o + 3] = (b >> 24) & 0xFF;
}
}  // namespace

long long global_matrix_offset(const uint8_t* d, size_t n) {
    if (!d || n < 16) return -1;
    uint32_t name_size = le32(d, 12);
    size_t off = 16 + static_cast<size_t>(name_size) + 10;
    if (off + 64 > n) return -1;
    return static_cast<long long>(off);
}

long long obbox_offset(const uint8_t* d, size_t n) {
    if (!d || n < 16) return -1;
    uint32_t name_size = le32(d, 12);
    size_t bv_off = 16 + static_cast<size_t>(name_size) + 10 + 68;
    if (bv_off + 24 > n) return -1;
    return static_cast<long long>(bv_off);
}

ObboxBounds obbox_local_bounds(const uint8_t* d, size_t n) {
    ObboxBounds out;
    long long bvo = obbox_offset(d, n);
    if (bvo < 0) return out;
    size_t o = static_cast<size_t>(bvo);
    for (int i = 0; i < 3; ++i) {
        out.mn[static_cast<size_t>(i)] = double(rd_f32(d, o + static_cast<size_t>(i) * 4));
        out.mx[static_cast<size_t>(i)] = double(rd_f32(d, o + 12 + static_cast<size_t>(i) * 4));
    }
    // Sphere-encoded BV (min > max on any axis) is not a usable AABB.
    for (int i = 0; i < 3; ++i)
        if (out.mn[static_cast<size_t>(i)] > out.mx[static_cast<size_t>(i)]) return out;
    out.ok = true;
    return out;
}

std::vector<uint8_t> extend_obbox_to_include(const uint8_t* d, size_t n,
                                             const std::array<double, 3>& world_min,
                                             const std::array<double, 3>& world_max) {
    long long bvo = obbox_offset(d, n);
    if (bvo < 0) return {};
    size_t o = static_cast<size_t>(bvo);
    if (n < o + 48) return {};                 // sphere-only BV: leave alone
    double cur_min[3], cur_max[3];
    for (int i = 0; i < 3; ++i) {
        cur_min[i] = double(rd_f32(d, o + static_cast<size_t>(i) * 4));
        cur_max[i] = double(rd_f32(d, o + 12 + static_cast<size_t>(i) * 4));
    }
    for (int i = 0; i < 3; ++i)
        if (cur_min[i] > cur_max[i]) return {};   // sphere-encoded: don't widen
    // Python min()/max() pick `world` only on a STRICT comparison, and the
    // tuple no-op check hits CPython's identity shortcut — so a NaN component
    // (real data: some shipped BVs carry NaN, e.g. f32 0xffffffff) keeps `cur`
    // and counts as UNCHANGED. Mirror by branching on the strict comparison.
    double new_min[3], new_max[3];
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        if (world_min[static_cast<size_t>(i)] < cur_min[i]) {
            new_min[i] = world_min[static_cast<size_t>(i)];
            changed = true;
        } else {
            new_min[i] = cur_min[i];
        }
        if (world_max[static_cast<size_t>(i)] > cur_max[i]) {
            new_max[i] = world_max[static_cast<size_t>(i)];
            changed = true;
        } else {
            new_max[i] = cur_max[i];
        }
    }
    std::vector<uint8_t> out(d, d + n);
    if (!changed) return out;                  // no-op still returns the payload
    for (int slot = 0; slot < 4; ++slot) {
        size_t so = o + static_cast<size_t>(slot) * 12;
        const double* v = (slot % 2 == 0) ? new_min : new_max;
        for (int i = 0; i < 3; ++i) wr_f32(out, so + static_cast<size_t>(i) * 4, v[i]);
    }
    return out;
}

std::vector<uint8_t> write_global_matrix(const uint8_t* d, size_t n,
                                         const std::array<double, 16>& col_major) {
    long long off = global_matrix_offset(d, n);
    if (off < 0) return {};
    std::vector<uint8_t> out(d, d + n);
    for (int i = 0; i < 16; ++i)
        wr_f32(out, static_cast<size_t>(off) + static_cast<size_t>(i) * 4,
               col_major[static_cast<size_t>(i)]);
    return out;
}

}  // namespace jade
