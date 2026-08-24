// Collision.cpp — implementation. Faithful port of core/collision.py read path.
#include "jade/Collision.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <set>

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

}  // namespace

bool looks_like_cob(bool ext_empty, const uint8_t* payload, size_t payload_len) {
    // full = [gro_type:u32][payload]; need len(full) >= 10 (=> payload >= 6) and
    // full[4] (= payload[0]) is a known shape code.
    if (!ext_empty) return false;
    if (payload_len < 6) return false;
    uint8_t shape = payload[0];
    return shape == 1 || shape == 2 || shape == 3 || shape == 5;
}

CobInfo parse_cob(const uint8_t* p, size_t n) {
    CobInfo c;
    if (n < 10 || p[0] != COL_ZONE_TRIANGLES) return c;

    size_t o = 0;
    c.type = p[o++];
    c.flag = p[o++];

    uint32_t n_verts = le32(p, o); o += 4;
    if (n_verts > 0x100000u || o + static_cast<size_t>(n_verts) * 12 + 4 > n) return c;
    c.verts.reserve(static_cast<size_t>(n_verts) * 3);
    for (uint32_t i = 0; i < n_verts; ++i) {
        size_t b = o + static_cast<size_t>(i) * 12;
        c.verts.push_back(lef(p, b));
        c.verts.push_back(lef(p, b + 4));
        c.verts.push_back(lef(p, b + 8));
    }
    o += static_cast<size_t>(n_verts) * 12;

    uint32_t n_faces = le32(p, o); o += 4;
    if (n_faces > 0x100000u || o + static_cast<size_t>(n_faces) * 12 + 4 > n) return c;
    c.normals.reserve(static_cast<size_t>(n_faces) * 3);
    for (uint32_t i = 0; i < n_faces; ++i) {
        size_t b = o + static_cast<size_t>(i) * 12;
        c.normals.push_back(lef(p, b));
        c.normals.push_back(lef(p, b + 4));
        c.normals.push_back(lef(p, b + 8));
    }
    o += static_cast<size_t>(n_faces) * 12;

    uint32_t n_elems = le32(p, o); o += 4;
    if (n_elems > 0x10000u) return c;

    uint64_t total_tris = 0;
    c.elements.reserve(n_elems);
    for (uint32_t e = 0; e < n_elems; ++e) {
        if (o + 8 > n) return c;
        CobElement el;
        el.n_tri    = le16(p, o);
        el.design   = p[o + 2];
        el.flag     = p[o + 3];
        el.material = static_cast<int32_t>(le32(p, o + 4));
        o += 8;
        if (o + static_cast<size_t>(el.n_tri) * 6 > n) return c;
        el.faces.reserve(static_cast<size_t>(el.n_tri) * 3);
        for (uint32_t i = 0; i < el.n_tri; ++i) {
            size_t b = o + static_cast<size_t>(i) * 6;
            el.faces.push_back(le16(p, b));
            el.faces.push_back(le16(p, b + 2));
            el.faces.push_back(le16(p, b + 4));
        }
        o += static_cast<size_t>(el.n_tri) * 6;
        total_tris += el.n_tri;
        c.elements.push_back(std::move(el));
    }

    if (total_tris != n_faces) return c;
    if (o + static_cast<size_t>(n_faces) * 6 > n) return c;
    c.proximity.reserve(static_cast<size_t>(n_faces) * 3);
    for (uint32_t i = 0; i < n_faces; ++i) {
        size_t b = o + static_cast<size_t>(i) * 6;
        c.proximity.push_back(le16(p, b));
        c.proximity.push_back(le16(p, b + 2));
        c.proximity.push_back(le16(p, b + 4));
    }
    o += static_cast<size_t>(n_faces) * 6;

    c.trailing.assign(p + o, p + n);
    c.n_verts = n_verts;
    c.n_faces = n_faces;
    c.n_elems = n_elems;
    c.total_tris = static_cast<uint32_t>(total_tris);
    c.ok = true;
    return c;
}

std::vector<std::array<uint16_t, 3>> triangle_proximity(
    const std::vector<std::array<uint16_t, 3>>& faces) {
    // edge (sorted pair) -> face indices, insertion-ordered per edge.
    std::map<std::pair<uint16_t, uint16_t>, std::vector<uint16_t>> edge_to_faces;
    auto skey = [](uint16_t a, uint16_t b) {
        return a <= b ? std::make_pair(a, b) : std::make_pair(b, a);
    };
    for (size_t idx = 0; idx < faces.size(); ++idx) {
        uint16_t a = faces[idx][0], b = faces[idx][1], c = faces[idx][2];
        edge_to_faces[skey(a, b)].push_back(uint16_t(idx));
        edge_to_faces[skey(a, c)].push_back(uint16_t(idx));
        edge_to_faces[skey(b, c)].push_back(uint16_t(idx));
    }
    std::vector<std::array<uint16_t, 3>> out;
    out.reserve(faces.size());
    for (size_t idx = 0; idx < faces.size(); ++idx) {
        uint16_t a = faces[idx][0], b = faces[idx][1], c = faces[idx][2];
        std::pair<uint16_t, uint16_t> edges[3] = {skey(a, b), skey(a, c), skey(b, c)};
        std::array<uint16_t, 3> nb{0xFFFF, 0xFFFF, 0xFFFF};
        for (int e = 0; e < 3; ++e) {
            for (uint16_t other : edge_to_faces[edges[e]])
                if (other != idx) { nb[size_t(e)] = other; break; }
        }
        out.push_back(nb);
    }
    return out;
}

namespace {

// _unit_box_geometry: retail simple-box vertex/face/normal order.
struct BoxGeo {
    std::array<std::array<double, 3>, 8> verts;
    std::vector<std::array<uint16_t, 3>> faces;
    std::vector<std::array<double, 3>> normals;
};
BoxGeo unit_box_geometry(const std::array<double, 3>& mn,
                         const std::array<double, 3>& mx) {
    BoxGeo g;
    g.verts = {{{mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]},
                {mn[0], mx[1], mn[2]}, {mx[0], mx[1], mn[2]},
                {mn[0], mn[1], mx[2]}, {mx[0], mn[1], mx[2]},
                {mn[0], mx[1], mx[2]}, {mx[0], mx[1], mx[2]}}};
    g.faces = {{0, 2, 3}, {3, 1, 0}, {4, 5, 7}, {7, 6, 4},
               {0, 1, 5}, {5, 4, 0}, {1, 3, 7}, {7, 5, 1},
               {3, 2, 6}, {6, 7, 3}, {2, 0, 4}, {4, 6, 2}};
    g.normals = {{0, 0, -1}, {0, 0, -1}, {0, 0, 1}, {0, 0, 1},
                 {0, -1, 0}, {0, -1, 0}, {1, 0, 0}, {1, 0, 0},
                 {0, 1, 0}, {0, 1, 0}, {-1, 0, 0}, {-1, 0, 0}};
    return g;
}

inline void put_u16v(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}
inline void put_u32v(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
inline void put_f32v(std::vector<uint8_t>& v, double d) {
    float f = static_cast<float>(d);
    uint32_t b;
    std::memcpy(&b, &f, 4);
    put_u32v(v, b);
}

}  // namespace

CobProfileLookup get_cob_profile(const std::string& name) {
    CobProfileLookup out;
    if (name.empty() || name == "simple_box") {
        out.ok = true;                    // RETAIL_SIMPLE_BOX (all defaults)
        return out;
    }
    if (name == "ledge_openbox") {
        out.ok = true;
        out.profile.name = "ledge_openbox";
        out.profile.material_id = LEDGE_TOP_MATERIAL_ID;
        out.profile.element_design = CLIMBABLE_COB_ELEMENT_DESIGN;
        out.profile.geometry = "open_front_box";
        return out;
    }
    if (name == "ledge_box") {            // RETAIL_LEDGE_BOX (SoT pair 67/72? —
        // profile carries wall mat 67 top-split; kept for back-compat.)
        out.ok = true;
        out.profile.name = "ledge_box";
        out.profile.material_id = LEDGE_TOP_MATERIAL_ID;
        out.profile.element_overrides = {{{8, 9}, LEDGE_FLOOR_MATERIAL_ID,
                                          DEFAULT_COB_ELEMENT_DESIGN, 0}};
        return out;
    }
    return out;                            // unknown -> Python ValueError
}

CobProfile make_ledge_profile_for_game(const std::string& game_code) {
    CobProfile profile;
    profile.name = "ledge_box_";
    profile.name.reserve(profile.name.size() + game_code.size());
    for (unsigned char c : game_code)
        profile.name.push_back(
            c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : char(c));
    profile.material_id = LEDGE_TOP_MATERIAL_ID;
    profile.element_overrides = {{{8, 9}, LEDGE_FLOOR_MATERIAL_ID,
                                  DEFAULT_COB_ELEMENT_DESIGN, 0}};
    return profile;
}

uint32_t pick_cob_gamemat_key(
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key) {
    return sub_by_key.count(DEFAULT_COB_GAMEMAT_KEY)
               ? DEFAULT_COB_GAMEMAT_KEY
               : 0xFFFFFFFFu;
}

std::vector<uint8_t> build_cob_triangle_box(
    const std::array<double, 3>& lb_min, const std::array<double, 3>& lb_max,
    uint32_t gmat_key, const CobProfile& profile) {
    if (profile.shape_type != COL_ZONE_TRIANGLES) return {};
    BoxGeo g = unit_box_geometry(lb_min, lb_max);
    std::vector<std::array<uint16_t, 3>> faces = g.faces;
    std::vector<std::array<double, 3>> normals = g.normals;
    if (profile.geometry == "open_front_box") {
        faces.erase(faces.begin(), faces.begin() + 2);
        normals.erase(normals.begin(), normals.begin() + 2);
    } else if (profile.geometry != "closed_box") {
        return {};
    }
    bool has_gmat = gmat_key != 0xFFFFFFFFu;

    // Element table: overrides first collected, primary = unclaimed faces,
    // then primary-first + overrides, faces reordered per element.
    struct Elem { int32_t mat; uint8_t design; uint8_t flag; std::vector<int> fl; };
    std::vector<Elem> overridden;
    std::set<int> claimed;
    for (const CobElementOverride& ov : profile.element_overrides) {
        for (int idx : ov.face_indices) {
            if (claimed.count(idx)) return {};
            if (idx < 0 || size_t(idx) >= faces.size()) return {};
            claimed.insert(idx);
        }
        overridden.push_back({ov.material_id, ov.design, ov.flag, ov.face_indices});
    }
    std::vector<int> primary_faces;
    for (size_t i = 0; i < faces.size(); ++i)
        if (!claimed.count(int(i))) primary_faces.push_back(int(i));
    std::vector<Elem> elements;
    if (!primary_faces.empty())
        elements.push_back({profile.material_id, profile.element_design,
                            profile.element_flag, primary_faces});
    for (Elem& e : overridden) elements.push_back(std::move(e));

    std::vector<int> face_order;
    for (const Elem& e : elements)
        for (int i : e.fl) face_order.push_back(i);
    std::vector<std::array<uint16_t, 3>> reordered_faces;
    std::vector<std::array<double, 3>> reordered_normals;
    for (int i : face_order) {
        reordered_faces.push_back(faces[size_t(i)]);
        reordered_normals.push_back(normals[size_t(i)]);
    }

    std::vector<uint8_t> data;
    data.push_back(profile.shape_type);
    data.push_back(has_gmat ? COL_COB_GAMEMAT_FLAG : 0);
    put_u32v(data, 8);
    for (const auto& v : g.verts) {
        put_f32v(data, v[0]); put_f32v(data, v[1]); put_f32v(data, v[2]);
    }
    put_u32v(data, static_cast<uint32_t>(reordered_faces.size()));
    for (const auto& nrm : reordered_normals) {
        put_f32v(data, nrm[0]); put_f32v(data, nrm[1]); put_f32v(data, nrm[2]);
    }
    put_u32v(data, static_cast<uint32_t>(elements.size()));
    std::map<int, size_t> new_idx_of_face;
    for (size_t nw = 0; nw < face_order.size(); ++nw) new_idx_of_face[face_order[nw]] = nw;
    for (const Elem& e : elements) {
        int32_t eff_mat = has_gmat ? e.mat : -1;
        put_u16v(data, static_cast<uint16_t>(e.fl.size()));
        data.push_back(e.design);
        data.push_back(e.flag);
        put_u32v(data, static_cast<uint32_t>(eff_mat));
        for (int old_idx : e.fl) {
            const auto& f = reordered_faces[new_idx_of_face[old_idx]];
            put_u16v(data, f[0]); put_u16v(data, f[1]); put_u16v(data, f[2]);
        }
    }
    for (const auto& prox : triangle_proximity(reordered_faces)) {
        put_u16v(data, prox[0]); put_u16v(data, prox[1]); put_u16v(data, prox[2]);
    }
    put_u32v(data, 0);
    return data;
}

std::vector<uint8_t> extend_cob_triangle_box(
    const uint8_t* p, size_t n,
    const std::array<double, 3>& lb_min, const std::array<double, 3>& lb_max,
    const std::array<double, 3>& world_translation,
    const std::array<double, 16>* corner_transform,
    int32_t material_id, uint8_t element_design, uint8_t element_flag,
    bool open_minus_z, bool include_climb_edges) {
    if (!p || n < 6) return {};
    size_t cursor = 0;
    uint8_t type_b = p[cursor++];
    uint8_t flag_b = p[cursor++];
    if (type_b != COL_ZONE_TRIANGLES) return {};
    flag_b = uint8_t(flag_b & ~COL_C_Cob_OK3);     // force linear-scan fallback

    uint32_t n_verts_old = le32(p, cursor); cursor += 4;
    size_t verts_start = cursor;
    size_t verts_end = cursor + size_t(n_verts_old) * 12;
    if (verts_end > n) return {};
    cursor = verts_end;

    uint32_t n_faces_old = le32(p, cursor); cursor += 4;
    size_t normals_start = cursor;
    size_t normals_end = cursor + size_t(n_faces_old) * 12;
    if (normals_end > n) return {};
    cursor = normals_end;

    uint32_t n_elems_old = le32(p, cursor); cursor += 4;
    size_t elements_start = cursor;
    for (uint32_t i = 0; i < n_elems_old; ++i) {
        if (cursor + 8 > n) return {};
        uint16_t n_tri = le16(p, cursor);
        cursor += 8 + size_t(n_tri) * 6;
        if (cursor > n) return {};
    }
    size_t elements_end = cursor;

    size_t proximity_start = cursor;
    size_t proximity_end = cursor + size_t(n_faces_old) * 6;
    if (proximity_end > n) return {};
    cursor = proximity_end;

    // Preserve prior toolkit climb-edge records (count*52 exact signature).
    std::vector<uint8_t> preserved_records;
    uint32_t preserved_count = 0;
    if (cursor + 4 <= n) {
        uint32_t existing_count = le32(p, cursor);
        size_t bytes_after = n - (cursor + 4);
        if (existing_count > 0 && size_t(existing_count) * 52 == bytes_after) {
            preserved_records.assign(p + cursor + 4, p + n);
            preserved_count = existing_count;
        }
    }

    BoxGeo box = unit_box_geometry(lb_min, lb_max);
    std::vector<std::array<uint16_t, 3>> box_faces = box.faces;
    std::vector<std::array<double, 3>> box_normals = box.normals;
    if (open_minus_z) {
        box_faces.erase(box_faces.begin(), box_faces.begin() + 2);
        box_normals.erase(box_normals.begin(), box_normals.begin() + 2);
    }
    uint32_t n_new_faces = static_cast<uint32_t>(box_faces.size());

    std::array<std::array<double, 3>, 8> new_vertices;
    std::vector<std::array<double, 3>> new_normals;
    if (corner_transform != nullptr) {
        const std::array<double, 16>& m = *corner_transform;   // col-major flat
        auto M = [&](int r, int c) { return m[size_t(c * 4 + r)]; };
        for (int i = 0; i < 8; ++i) {
            double vx = box.verts[size_t(i)][0], vy = box.verts[size_t(i)][1],
                   vz = box.verts[size_t(i)][2];
            new_vertices[size_t(i)] = {
                M(0, 0) * vx + M(0, 1) * vy + M(0, 2) * vz + M(0, 3),
                M(1, 0) * vx + M(1, 1) * vy + M(1, 2) * vz + M(1, 3),
                M(2, 0) * vx + M(2, 1) * vy + M(2, 2) * vz + M(2, 3)};
        }
        for (const auto& nrm : box_normals) {
            double nx = M(0, 0) * nrm[0] + M(0, 1) * nrm[1] + M(0, 2) * nrm[2];
            double ny = M(1, 0) * nrm[0] + M(1, 1) * nrm[1] + M(1, 2) * nrm[2];
            double nz = M(2, 0) * nrm[0] + M(2, 1) * nrm[1] + M(2, 2) * nrm[2];
            double mag = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (mag < 1e-9) new_normals.push_back(nrm);
            else new_normals.push_back({nx / mag, ny / mag, nz / mag});
        }
    } else {
        for (int i = 0; i < 8; ++i)
            new_vertices[size_t(i)] = {box.verts[size_t(i)][0] + world_translation[0],
                                       box.verts[size_t(i)][1] + world_translation[1],
                                       box.verts[size_t(i)][2] + world_translation[2]};
        new_normals = box_normals;
    }

    std::vector<std::array<uint16_t, 3>> prox_local = triangle_proximity(box_faces);

    std::vector<uint8_t> climb_records;
    if (include_climb_edges) {
        const std::array<double, 3>& v4 = new_vertices[4];
        const std::array<double, 3>& v5 = new_vertices[5];
        const std::array<double, 3>& v6 = new_vertices[6];
        const std::array<double, 3>& v7 = new_vertices[7];
        struct Edge { const std::array<double, 3>*a, *b; std::array<double, 3> fn; };
        const Edge edges[4] = {{&v4, &v5, {0, -1, 0}}, {&v4, &v6, {-1, 0, 0}},
                               {&v5, &v7, {1, 0, 0}}, {&v6, &v7, {0, 1, 0}}};
        for (const Edge& e : edges) {
            put_u32v(climb_records, 1);
            put_u32v(climb_records, 0x80000001u);
            for (int i = 0; i < 3; ++i) put_f32v(climb_records, (*e.a)[size_t(i)]);
            for (int i = 0; i < 3; ++i) put_f32v(climb_records, (*e.b)[size_t(i)]);
            for (int i = 0; i < 3; ++i) put_f32v(climb_records, e.fn[size_t(i)]);
            put_u32v(climb_records, 0);
            put_u32v(climb_records, 6);
        }
    }
    uint32_t new_record_count = static_cast<uint32_t>(climb_records.size() / 52);

    std::vector<uint8_t> out;
    out.push_back(type_b);
    out.push_back(flag_b);
    put_u32v(out, n_verts_old + 8);
    out.insert(out.end(), p + verts_start, p + verts_end);
    for (const auto& v : new_vertices) {
        put_f32v(out, v[0]); put_f32v(out, v[1]); put_f32v(out, v[2]);
    }
    put_u32v(out, n_faces_old + n_new_faces);
    out.insert(out.end(), p + normals_start, p + normals_end);
    for (const auto& nrm : new_normals) {
        put_f32v(out, nrm[0]); put_f32v(out, nrm[1]); put_f32v(out, nrm[2]);
    }
    put_u32v(out, n_elems_old + 1);
    out.insert(out.end(), p + elements_start, p + elements_end);
    put_u16v(out, static_cast<uint16_t>(n_new_faces));
    out.push_back(element_design);
    out.push_back(element_flag);
    put_u32v(out, static_cast<uint32_t>(material_id));
    for (const auto& f : box_faces) {
        put_u16v(out, uint16_t(f[0] + n_verts_old));
        put_u16v(out, uint16_t(f[1] + n_verts_old));
        put_u16v(out, uint16_t(f[2] + n_verts_old));
    }
    out.insert(out.end(), p + proximity_start, p + proximity_end);
    for (const auto& pr : prox_local)
        for (int e = 0; e < 3; ++e)
            put_u16v(out, pr[size_t(e)] == 0xFFFF
                              ? uint16_t(0xFFFF)
                              : uint16_t(pr[size_t(e)] + n_faces_old));
    put_u32v(out, preserved_count + new_record_count);
    out.insert(out.end(), preserved_records.begin(), preserved_records.end());
    out.insert(out.end(), climb_records.begin(), climb_records.end());
    return out;
}

std::vector<uint8_t> serialize_cob(const CobInfo& c) {
    std::vector<uint8_t> out;
    auto put_u16 = [&](uint16_t v) {
        out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8));
    };
    auto put_u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    auto put_f32 = [&](float f) {
        uint32_t b; std::memcpy(&b, &f, 4); put_u32(b);
    };

    out.push_back(c.type);
    out.push_back(c.flag);
    put_u32(static_cast<uint32_t>(c.verts.size() / 3));
    for (float f : c.verts) put_f32(f);
    put_u32(static_cast<uint32_t>(c.normals.size() / 3));
    for (float f : c.normals) put_f32(f);
    put_u32(static_cast<uint32_t>(c.elements.size()));
    for (const CobElement& e : c.elements) {
        put_u16(static_cast<uint16_t>(e.faces.size() / 3));
        out.push_back(e.design);
        out.push_back(e.flag);
        put_u32(static_cast<uint32_t>(e.material));     // i32 two's-complement
        for (uint16_t idx : e.faces) put_u16(idx);
    }
    for (uint16_t pr : c.proximity) put_u16(pr);
    out.insert(out.end(), c.trailing.begin(), c.trailing.end());
    return out;
}

std::vector<std::array<double, 3>> compute_face_normals(
    const std::vector<std::array<double, 3>>& verts,
    const std::vector<std::array<uint32_t, 3>>& faces) {
    std::vector<std::array<double, 3>> out;
    out.reserve(faces.size());
    for (const auto& f : faces) {
        const auto& v0 = verts[f[0]];
        const auto& v1 = verts[f[1]];
        const auto& v2 = verts[f[2]];
        double e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
        double e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];
        double nx = e1y * e2z - e1z * e2y;
        double ny = e1z * e2x - e1x * e2z;
        double nz = e1x * e2y - e1y * e2x;
        double m = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (m > 1e-12)
            out.push_back({nx / m, ny / m, nz / m});
        else
            out.push_back({0.0, 1.0, 0.0});
    }
    return out;
}

std::vector<uint8_t> extend_cob_triangle_mesh(
    const uint8_t* existing, size_t n,
    const std::vector<std::array<double, 3>>& verts_local,
    const std::vector<std::array<uint32_t, 3>>& faces_in,
    int32_t material_id, uint8_t element_design, uint8_t element_flag,
    bool clear_ok3) {
    return extend_cob_triangle_mesh(
        existing, n, verts_local, faces_in,
        compute_face_normals(verts_local, [&]() {
            std::vector<std::array<uint32_t, 3>> usable;
            const uint32_t nv = uint32_t(verts_local.size());
            for (const auto& face : faces_in)
                if (face[0] < nv && face[1] < nv && face[2] < nv &&
                    face[0] != face[1] && face[0] != face[2] &&
                    face[1] != face[2])
                    usable.push_back(face);
            return usable;
        }()),
        material_id, element_design, element_flag, clear_ok3);
}

std::vector<uint8_t> extend_cob_triangle_mesh(
    const uint8_t* existing, size_t n,
    const std::vector<std::array<double, 3>>& verts_local,
    const std::vector<std::array<uint32_t, 3>>& faces_in,
    const std::vector<std::array<double, 3>>& normals_in,
    int32_t material_id, uint8_t element_design, uint8_t element_flag,
    bool clear_ok3) {
    CobInfo parsed = parse_cob(existing, n);
    if (!parsed.ok) return {};
    uint32_t nv = uint32_t(verts_local.size());
    std::vector<std::array<uint32_t, 3>> faces;
    for (const auto& f : faces_in)
        if (f[0] < nv && f[1] < nv && f[2] < nv &&
            f[0] != f[1] && f[0] != f[2] && f[1] != f[2])
            faces.push_back(f);
    if (faces.empty()) return {};

    uint32_t old_nf = uint32_t(parsed.proximity.size() / 3);
    uint32_t base = uint32_t(parsed.verts.size() / 3);

    for (const auto& v : verts_local) {
        parsed.verts.push_back(float(v[0]));
        parsed.verts.push_back(float(v[1]));
        parsed.verts.push_back(float(v[2]));
    }
    for (const auto& nrm : normals_in) {
        parsed.normals.push_back(float(nrm[0]));
        parsed.normals.push_back(float(nrm[1]));
        parsed.normals.push_back(float(nrm[2]));
    }
    CobElement el;
    el.n_tri = uint16_t(faces.size());
    el.design = element_design;
    el.flag = element_flag;
    el.material = material_id;
    for (const auto& f : faces) {
        el.faces.push_back(uint16_t(f[0] + base));
        el.faces.push_back(uint16_t(f[1] + base));
        el.faces.push_back(uint16_t(f[2] + base));
    }
    parsed.elements.push_back(std::move(el));

    std::vector<std::array<uint16_t, 3>> faces16;
    faces16.reserve(faces.size());
    for (const auto& f : faces)
        faces16.push_back({uint16_t(f[0]), uint16_t(f[1]), uint16_t(f[2])});
    constexpr uint16_t kInvalid16 = 0xFFFF;
    for (const auto& row : triangle_proximity(faces16))
        for (uint16_t v : row)
            parsed.proximity.push_back(
                v == kInvalid16 ? kInvalid16 : uint16_t(v + old_nf));

    if (clear_ok3) {
        parsed.flag = uint8_t(parsed.flag & ~COL_C_Cob_OK3);
        parsed.trailing = {0, 0, 0, 0};
    }
    return serialize_cob(parsed);
}

std::vector<std::array<uint16_t, 3>> cob_flat_faces(const CobInfo& cob) {
    std::vector<std::array<uint16_t, 3>> out;
    out.reserve(cob.total_tris);
    for (const CobElement& element : cob.elements)
        for (size_t i = 0; i + 2 < element.faces.size(); i += 3)
            out.push_back({element.faces[i], element.faces[i + 1],
                           element.faces[i + 2]});
    return out;
}

namespace {

std::vector<uint8_t> sub_full_content(const SubEntry& sub) {
    std::vector<uint8_t> full;
    put_u32v(full, sub.gro_null ? 0u : sub.gro_type);
    full.insert(full.end(), sub.data.begin(), sub.data.end());
    const size_t declared = sub.size ? size_t(sub.size) : full.size();
    if (declared > 0 && declared <= full.size()) full.resize(declared);
    return full;
}

const SubEntry* map_find(
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key,
    uint32_t key) {
    const auto found = sub_by_key.find(key);
    return found == sub_by_key.end() ? nullptr : found->second;
}

}  // namespace

const SubEntry* find_room_master_cob(const std::vector<SubEntry>& subs) {
    const SubEntry* best = nullptr;
    for (const SubEntry& sub : subs) {
        if (!looks_like_cob_sub(&sub) || sub.data.empty() ||
            sub.data[0] != COL_ZONE_TRIANGLES)
            continue;
        if (best == nullptr || sub.size > best->size) best = &sub;
    }
    return best;
}

std::vector<uint8_t> build_colmap_compact_payload(uint32_t cob_key) {
    std::vector<uint8_t> out;
    put_u32v(out, cob_key);
    return out;
}

std::vector<uint8_t> make_sub_entry(
    uint32_t key, uint32_t gro_type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    put_u32v(out, 4u + static_cast<uint32_t>(payload.size()));
    put_u32v(out, JADE_COOKIE);
    put_u32v(out, key);
    put_u32v(out, gro_type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> make_sub_entry_ext(
    uint32_t key, const std::array<uint8_t, 4>& ext,
    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    put_u32v(out, 4u + static_cast<uint32_t>(payload.size()));
    put_u32v(out, JADE_COOKIE);
    put_u32v(out, key);
    out.insert(out.end(), ext.begin(), ext.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> make_cob_sub_entry(
    uint32_t cob_key, uint32_t gmat_key,
    const std::vector<uint8_t>& payload_body) {
    return make_sub_entry(cob_key, gmat_key, payload_body);
}

std::vector<uint8_t> make_colmap_compact_sub_entry(
    uint32_t colmap_key, uint32_t cob_key) {
    return make_sub_entry(colmap_key, COLMAP_COMPACT_HEADER,
                          build_colmap_compact_payload(cob_key));
}

bool looks_like_cob_sub(const SubEntry* sub) {
    if (sub == nullptr || !sub->ext.empty()) return false;
    const std::vector<uint8_t> full = sub_full_content(*sub);
    if (full.size() < 10) return false;
    const uint8_t shape = full[4];
    return shape == 1 || shape == 2 || shape == 3 || shape == 5;
}

bool is_colmap_sub(
    const SubEntry* sub,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key) {
    if (sub == nullptr || !sub->ext.empty()) return false;
    const std::vector<uint8_t> full = sub_full_content(*sub);
    const size_t size = sub->size ? size_t(sub->size) : full.size();
    const uint32_t gro_type = sub->gro_null ? 0u : sub->gro_type;
    if (size == 4) return looks_like_cob_sub(map_find(sub_by_key, gro_type));
    if (full.size() < 8) return false;
    const uint8_t count = full[0];
    if (count < 1 || count > 8 || full.size() < 4 + size_t(count) * 4)
        return false;
    if (gro_type == COLMAP_COMPACT_HEADER) return true;
    for (uint8_t i = 0; i < count; ++i)
        if (!looks_like_cob_sub(
                map_find(sub_by_key, le32(full.data(), 4 + size_t(i) * 4))))
            return false;
    return true;
}

std::vector<uint32_t> colmap_cob_keys(
    const SubEntry* colmap_sub,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key) {
    if (!is_colmap_sub(colmap_sub, sub_by_key)) return {};
    const uint32_t gro_type = colmap_sub->gro_null ? 0u : colmap_sub->gro_type;
    if (colmap_sub->size == 4) return {gro_type};
    const std::vector<uint8_t> full = sub_full_content(*colmap_sub);
    if (gro_type == COLMAP_COMPACT_HEADER)
        return full.size() >= 8 ? std::vector<uint32_t>{le32(full.data(), 4)}
                                : std::vector<uint32_t>{};
    std::vector<uint32_t> keys;
    const uint8_t count = full.empty() ? 0 : full[0];
    for (uint8_t i = 0; i < count; ++i) {
        const size_t offset = 4 + size_t(i) * 4;
        if (offset + 4 <= full.size()) keys.push_back(le32(full.data(), offset));
    }
    return keys;
}

std::vector<std::pair<size_t, uint32_t>> gao_colmap_key_offsets(
    const std::vector<uint8_t>& gao_payload,
    const std::unordered_map<uint32_t, const SubEntry*>& sub_by_key) {
    std::vector<std::pair<size_t, uint32_t>> hits;
    for (size_t offset = 0; offset + 4 <= gao_payload.size(); ++offset) {
        const uint32_t key = le32(gao_payload.data(), offset);
        if (is_colmap_sub(map_find(sub_by_key, key), sub_by_key))
            hits.push_back({offset, key});
    }
    return hits;
}

}  // namespace jade
