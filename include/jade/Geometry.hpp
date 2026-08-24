// Geometry.hpp — Jade GEO v7/v8 geometry parser (read path).
//
// Port of jade_explorer/core/geometry.py. Parses the PC GEO header, optional
// skin block, vertices, normals (gated by skin_ok3), per-vertex colours (gated
// by has_abs), UVs, element groups, and the filtered triangle list. PS2 GEOs
// share the gro_type=1 marker but use a VIF format; they are detected and
// reported as undecoded (matching the toolkit).
//
// Parsed arrays are kept as flat little-endian-friendly vectors so the golden
// digest can CRC their raw memory directly (x86/x64 LE == struct.pack '<...').
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace jade {

// One skin bone list, retained verbatim from the GEO's MRM/skin block so a
// mesh swap can fall back to the ORIGINAL skin when the incoming GLB carries
// none (weights stay as raw u16 ponds; weight_float = pond / 65535.0).
struct GeoBone {
    uint16_t bone_idx = 0;
    std::array<float, 16> bind_matrix{};
    int32_t  matrix_type = 0;
    std::vector<std::pair<uint16_t, uint16_t>> weights;   // (vert_idx, pond)
};

struct GeoInfo {
    bool ok  = false;   // parsed (PC or PS2)
    bool ps2 = false;

    // PC header (10 u32). skin_ok3 is the *effective* value (overwritten from
    // after the skin block when an MRM/skin header is present).
    uint32_t version = 0, flags1 = 0, flags2 = 0, nb_pts = 0, abs_count = 0,
             has_abs = 0, hdr_nb_uvs = 0, nb_elems = 0, mrm_marker = 0, skin_ok3 = 0;

    uint32_t nb_points = 0, nb_uvs = 0, nb_elements = 0, nb_tris = 0;
    bool has_colors = false;

    size_t   elements_offset = 0;     // byte offset of the element table (PC)

    std::vector<float>    vertices;  // 3*nb_points
    std::vector<float>    normals;   // 3*nb_points, empty if skin_ok3==0
    std::vector<float>    uvs;        // 2*nb_uvs, empty if none read
    std::vector<uint8_t>  colors;     // 4*ncol RGBA, empty if has_abs==0/none
    std::vector<uint32_t> elements;   // 2*nb_elements (nTri,matId)
    std::vector<uint16_t> faces;      // 7*nb_tris (i0,i1,i2,uv0,uv1,uv2,elem)

    // Skin: mrm_present => an MRM/skin block was attempted at offset 36.
    bool     mrm_present = false;
    bool     skin_present = false;   // skin block parsed (vs malformed/None)
    size_t   skin_size = 0;          // bytes consumed at offset 36
    uint16_t skin_flags = 0;
    size_t   skin_nbones = 0;
    std::vector<GeoBone> skin_bones;   // full lists when skin_present

    // PS2 (undecoded VIF format) — only the header is surfaced.
    // nb_points is a deliberately rough payload_size/32 estimate, matching
    // geometry.py; it is not exact until the VIF packets are decoded.
    uint32_t ps2_stream_count = 0, ps2_mesh_count = 0, ps2_item_count = 0,
             ps2_payload_offset = 0, ps2_payload_size = 0;
    bool ps2_nb_points_estimated = false;
    std::vector<uint32_t> ps2_items;
};

bool is_geometry_entry(const uint8_t* d, size_t n, const uint32_t* gro_type = nullptr);
bool is_geometry_entry_ps2(const uint8_t* d, size_t n, const uint32_t* gro_type = nullptr);

// Parse a geometry sub-entry payload. `ok == false` mirrors the Python None.
GeoInfo parse_geometry(const uint8_t* d, size_t n);

// --- matId edit helpers (positional into the owning GAO's multi-material) ---

// Byte offset of element `elem_idx`'s matId in the GEO body, or -1
// (unparseable / PS2 / out of range).
long long element_matid_offset(const uint8_t* d, size_t n, uint32_t elem_idx);

// Copy of the payload with one element's body matId rewritten; empty on failure.
// NOTE: a renderable GEO also repeats (matId,nTri) in its cooked trailing block
// and the engine draws from THAT — see cooked_element_matid_offsets.
std::vector<uint8_t> set_element_matid(const uint8_t* d, size_t n,
                                       uint32_t elem_idx, uint32_t new_matid);

// Byte offsets of each element's matId inside the cooked trailing block
// (located by its [0,0][nb_elems] header + matching nTri pairs). Empty when the
// table can't be found (e.g. magic-3 meshes) — mirrors the Python None.
std::vector<size_t> cooked_element_matid_offsets(const uint8_t* d, size_t n);

}  // namespace jade
