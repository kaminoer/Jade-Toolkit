// Geometry.cpp — implementation. Faithful port of core/geometry.py.
#include "jade/Geometry.hpp"

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
inline float lef(const uint8_t* p, size_t o) {
    uint32_t v = le32(p, o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// Skin parse mirroring core.geometry.parse_skin_data: returns bytes consumed
// (even on a malformed list), whether it fully succeeded, and a small summary.
struct SkinResult {
    bool     ok       = false;
    size_t   consumed = 0;
    uint16_t flags    = 0;
    size_t   nbones   = 0;
};

SkinResult parse_skin_data(const uint8_t* d, size_t n, size_t off,
                           std::vector<GeoBone>* bones_out = nullptr) {
    SkinResult r;
    size_t start = off;
    if (off + 4 > n) return r;  // (None, 0)
    r.flags = le16(d, off);
    uint16_t num_lists = le16(d, off + 2);
    off += 4;
    for (uint16_t li = 0; li < num_lists; ++li) {
        if (off + 72 > n) { r.consumed = off - start; return r; }  // (None, consumed)
        uint16_t bone_idx = le16(d, off);
        uint16_t num_verts = le16(d, off + 2);
        off += 4;          // bone_idx, num_verts
        GeoBone bone;
        if (bones_out) {
            bone.bone_idx = bone_idx;
            for (int k = 0; k < 16; ++k) {
                uint32_t b32 = le32(d, off + static_cast<size_t>(k) * 4);
                std::memcpy(&bone.bind_matrix[static_cast<size_t>(k)], &b32, 4);
            }
        }
        off += 64;         // bind matrix (16 floats)
        if (bones_out) bone.matrix_type = static_cast<int32_t>(le32(d, off));
        off += 4;          // matrix_type
        if (off + static_cast<size_t>(num_verts) * 4 > n) { r.consumed = off - start; return r; }
        if (bones_out) {
            bone.weights.reserve(num_verts);
            for (uint16_t wi = 0; wi < num_verts; ++wi) {
                size_t wo = off + static_cast<size_t>(wi) * 4;
                bone.weights.push_back({le16(d, wo), le16(d, wo + 2)});
            }
            bones_out->push_back(std::move(bone));
        }
        off += static_cast<size_t>(num_verts) * 4;
        ++r.nbones;
    }
    r.ok = true;
    r.consumed = off - start;
    return r;
}

}  // namespace

bool is_geometry_entry_ps2(const uint8_t* d, size_t n, const uint32_t* gro_type) {
    if (gro_type && *gro_type != 1) return false;
    if (n < 16) return false;
    uint32_t v = le32(d, 0), c1 = le32(d, 4), c2 = le32(d, 8), nn = le32(d, 12);
    if (v != 7 || c1 != 4 || c2 != 1) return false;
    return nn <= 4096 && 16 + static_cast<size_t>(nn) * 4 <= n;
}

bool is_geometry_entry(const uint8_t* d, size_t n, const uint32_t* gro_type) {
    if (n < 40) return false;
    if (gro_type && *gro_type != 1) return false;
    if (is_geometry_entry_ps2(d, n, gro_type)) return false;
    uint32_t version = le32(d, 0);
    if (version != 7 && version != 8) return false;
    uint32_t nb_pts = le32(d, 12);
    if (nb_pts < 1 || nb_pts > 100000) return false;
    uint32_t nb_elems = le32(d, 28);
    return nb_elems <= 10000;
}

GeoInfo parse_geometry(const uint8_t* d, size_t n) {
    GeoInfo g;
    if (n < 40) return g;

    if (is_geometry_entry_ps2(d, n, nullptr)) {
        g.ok = true;
        g.ps2 = true;
        g.version = le32(d, 0);
        g.ps2_stream_count = le32(d, 4);
        g.ps2_mesh_count = le32(d, 8);
        g.ps2_item_count = le32(d, 12);
        size_t payload_off = 16 + static_cast<size_t>(g.ps2_item_count) * 4;
        g.ps2_items.reserve(g.ps2_item_count);
        for (uint32_t i = 0; i < g.ps2_item_count; ++i)
            g.ps2_items.push_back(le32(d, 16 + static_cast<size_t>(i) * 4));
        g.ps2_payload_offset = static_cast<uint32_t>(payload_off);
        g.ps2_payload_size = (n > payload_off) ? static_cast<uint32_t>(n - payload_off) : 0;
        g.nb_points = g.ps2_payload_size / 32;
        g.nb_elements = g.ps2_mesh_count;
        g.ps2_nb_points_estimated = true;
        return g;
    }

    uint32_t version = le32(d, 0);
    if (version != 7 && version != 8) return g;

    g.version    = version;
    g.flags1     = le32(d, 4);
    g.flags2     = le32(d, 8);
    g.nb_pts     = le32(d, 12);
    g.abs_count  = le32(d, 16);
    g.has_abs    = le32(d, 20);
    g.hdr_nb_uvs = le32(d, 24);
    g.nb_elems   = le32(d, 28);
    g.mrm_marker = le32(d, 32);
    g.skin_ok3   = le32(d, 36);

    uint32_t nb_pts = g.nb_pts, nb_uvs = g.hdr_nb_uvs, nb_elems = g.nb_elems;
    uint32_t abs_count = g.abs_count, has_abs = g.has_abs;
    if (nb_pts < 1 || nb_pts > 100000 || nb_elems > 10000) return g;

    size_t eff_hdr = 40;
    uint32_t skin_ok3 = g.skin_ok3;
    if (g.mrm_marker == 0xC0DE2002u) {
        g.mrm_present = true;
        std::vector<GeoBone> bones;
        SkinResult sk = parse_skin_data(d, n, 36, &bones);
        if (sk.consumed == 0) return g;          // Python: skin_size <= 0 -> None
        g.skin_present = sk.ok;
        if (sk.ok) g.skin_bones = std::move(bones);
        g.skin_size = sk.consumed;
        g.skin_flags = sk.flags;
        g.skin_nbones = sk.nbones;
        size_t skin_off = 36 + sk.consumed;
        if (skin_off + 4 > n) return g;
        // This post-skin value gates normals, but the Python `header` dict keeps
        // the *original* offset-36 skin_ok3, so g.skin_ok3 must stay untouched.
        skin_ok3 = le32(d, skin_off);
        eff_hdr = skin_off + 4;
    }

    // Vertices
    size_t voff = eff_hdr;
    if (voff + static_cast<size_t>(nb_pts) * 12 > n) return g;
    g.vertices.reserve(static_cast<size_t>(nb_pts) * 3);
    for (uint32_t i = 0; i < nb_pts; ++i) {
        size_t o = voff + static_cast<size_t>(i) * 12;
        g.vertices.push_back(lef(d, o));
        g.vertices.push_back(lef(d, o + 4));
        g.vertices.push_back(lef(d, o + 8));
    }

    size_t off = voff + static_cast<size_t>(nb_pts) * 12;

    // Normals (only when skin_ok3 != 0)
    if (skin_ok3 != 0) {
        if (off + static_cast<size_t>(nb_pts) * 12 > n) return g;
        g.normals.reserve(static_cast<size_t>(nb_pts) * 3);
        for (uint32_t i = 0; i < nb_pts; ++i) {
            size_t o = off + static_cast<size_t>(i) * 12;
            g.normals.push_back(lef(d, o));
            g.normals.push_back(lef(d, o + 4));
            g.normals.push_back(lef(d, o + 8));
        }
        off += static_cast<size_t>(nb_pts) * 12;
    }

    // Vertex colours (D3DCOLOR ARGB -> RGBA), gated by has_abs. Note off
    // advances by n_col*4 even when the read doesn't fit, matching the Python.
    if (has_abs != 0) {
        uint32_t n_col = (abs_count < nb_pts) ? abs_count : nb_pts;
        if (off + static_cast<size_t>(n_col) * 4 <= n) {
            g.colors.reserve(static_cast<size_t>(n_col) * 4);
            for (uint32_t i = 0; i < n_col; ++i) {
                uint32_t argb = le32(d, off + static_cast<size_t>(i) * 4);
                g.colors.push_back((argb >> 16) & 0xFF);  // R
                g.colors.push_back((argb >> 8) & 0xFF);   // G
                g.colors.push_back(argb & 0xFF);          // B
                g.colors.push_back((argb >> 24) & 0xFF);  // A
            }
        }
        off += static_cast<size_t>(n_col) * 4;
    }
    g.has_colors = !g.colors.empty();

    // UVs (off advances by nb_uvs*8 regardless, like the Python)
    if (nb_uvs > 0 && off + static_cast<size_t>(nb_uvs) * 8 <= n) {
        g.uvs.reserve(static_cast<size_t>(nb_uvs) * 2);
        for (uint32_t i = 0; i < nb_uvs; ++i) {
            size_t o = off + static_cast<size_t>(i) * 8;
            g.uvs.push_back(lef(d, o));
            g.uvs.push_back(lef(d, o + 4));
        }
    }
    off += static_cast<size_t>(nb_uvs) * 8;

    // Element headers ([u32 nTri][u32 matId] x nb_elems).
    if (off + static_cast<size_t>(nb_elems) * 8 > n) return g;
    g.elements_offset = off;
    g.elements.reserve(static_cast<size_t>(nb_elems) * 2);
    uint64_t total_tris = 0;
    for (uint32_t i = 0; i < nb_elems; ++i) {
        uint32_t n_tri = le32(d, off + static_cast<size_t>(i) * 8);
        uint32_t mat_id = le32(d, off + static_cast<size_t>(i) * 8 + 4);
        if (n_tri > 100000) return g;
        g.elements.push_back(n_tri);
        g.elements.push_back(mat_id);
        total_tris += n_tri;
    }
    off += static_cast<size_t>(nb_elems) * 8;

    g.nb_points = nb_pts;
    g.nb_uvs = nb_uvs;
    g.nb_elements = nb_elems;

    if (total_tris == 0 || nb_elems == 0) {
        g.ok = true;     // header-only mesh, no faces
        g.nb_tris = 0;
        return g;
    }

    // Triangles (filtered: keep only those with all indices < nb_pts)
    if (off + static_cast<size_t>(total_tris) * 16 > n) return g;
    g.faces.reserve(static_cast<size_t>(total_tris) * 7);
    size_t tri_idx = 0;
    for (uint32_t ei = 0; ei < nb_elems; ++ei) {
        uint32_t elem_tris = g.elements[static_cast<size_t>(ei) * 2];
        for (uint32_t t = 0; t < elem_tris; ++t) {
            size_t toff = off + tri_idx * 16;
            uint16_t i0 = le16(d, toff), i1 = le16(d, toff + 2), i2 = le16(d, toff + 4);
            uint16_t uv0 = le16(d, toff + 6), uv1 = le16(d, toff + 8), uv2 = le16(d, toff + 10);
            if (i0 < nb_pts && i1 < nb_pts && i2 < nb_pts) {
                g.faces.push_back(i0); g.faces.push_back(i1); g.faces.push_back(i2);
                g.faces.push_back(uv0); g.faces.push_back(uv1); g.faces.push_back(uv2);
                g.faces.push_back(static_cast<uint16_t>(ei));
            }
            ++tri_idx;
        }
    }
    g.nb_tris = static_cast<uint32_t>(g.faces.size() / 7);
    g.ok = true;
    return g;
}

long long element_matid_offset(const uint8_t* d, size_t n, uint32_t elem_idx) {
    GeoInfo g = parse_geometry(d, n);
    if (!g.ok || g.ps2) return -1;           // PS2 dict carries no elements_offset
    if (elem_idx >= g.nb_elements) return -1;
    return static_cast<long long>(g.elements_offset + static_cast<size_t>(elem_idx) * 8 + 4);
}

std::vector<uint8_t> set_element_matid(const uint8_t* d, size_t n,
                                       uint32_t elem_idx, uint32_t new_matid) {
    long long off = element_matid_offset(d, n, elem_idx);
    if (off < 0) return {};
    std::vector<uint8_t> buf(d, d + n);
    size_t o = static_cast<size_t>(off);
    buf[o] = static_cast<uint8_t>(new_matid);
    buf[o + 1] = static_cast<uint8_t>(new_matid >> 8);
    buf[o + 2] = static_cast<uint8_t>(new_matid >> 16);
    buf[o + 3] = static_cast<uint8_t>(new_matid >> 24);
    return buf;
}

std::vector<size_t> cooked_element_matid_offsets(const uint8_t* d, size_t n) {
    GeoInfo g = parse_geometry(d, n);
    if (!g.ok) return {};
    uint32_t ne = g.nb_elements;
    if (ne == 0) return {};
    // Body element pairs are (nTri, matId); the cooked table is (matId, nTri),
    // located by a [0,0][nb_elems] header whose nTri column matches the body.
    if (n < 12 + static_cast<size_t>(ne) * 8) return {};
    size_t limit = n - 12 - static_cast<size_t>(ne) * 8;
    for (size_t off = 0; off <= limit; off += 4) {
        if (le32(d, off) == 0 && le32(d, off + 4) == 0 && le32(d, off + 8) == ne) {
            size_t base = off + 12;
            bool all_match = true;
            for (uint32_t j = 0; j < ne; ++j) {
                uint32_t body_ntri = g.elements[static_cast<size_t>(j) * 2];
                if (le32(d, base + static_cast<size_t>(j) * 8 + 4) != body_ntri) {
                    all_match = false;
                    break;
                }
            }
            if (all_match) {
                std::vector<size_t> out;
                out.reserve(ne);
                for (uint32_t j = 0; j < ne; ++j)
                    out.push_back(base + static_cast<size_t>(j) * 8);
                return out;
            }
        }
    }
    return {};
}

}  // namespace jade
