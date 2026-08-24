// bf_geo_digest — find GEO sub-entries, parse, and digest each section.
//
//   bf_geo_digest <file.bf> [out] [limit]
//
// For each active entry (sorted, up to `limit`; 0 = all): decompress, walk the
// sub-entry stream, and for every sub-entry that is_geometry_entry accepts,
// parse_geometry and emit a line with the header fields, counts, and a CRC-32
// per parsed section (vertices/normals/uvs/colours/elements/faces/skin).
// tests/run_golden.py diffs this vs core.geometry. Section CRCs are over the
// raw little-endian section bytes, so they match struct.pack on the oracle side.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Geometry.hpp"
#include "jade/SubEntry.hpp"

template <typename T>
static std::string crc_vec(const std::vector<T>& v) {
    if (v.empty()) return "-";
    return jade::hex8(jade::crc32(reinterpret_cast<const uint8_t*>(v.data()),
                                  v.size() * sizeof(T)));
}

static void emit(std::ostream& os, const jade::BigFile& bf, size_t limit) {
    const char NL = '\n';
    std::vector<uint32_t> active;
    for (const auto& kv : bf.files) {
        const jade::BFFile& e = kv.second;
        if (!e.name.empty() && e.key != jade::INVALID_KEY) active.push_back(e.index);
    }
    size_t emit_n = (limit == 0) ? active.size() : std::min(limit, active.size());

    std::vector<std::string> lines;
    for (size_t i = 0; i < emit_n; ++i) {
        uint32_t idx = active[i];
        jade::LzoResult r = jade::decompress_lzo(bf.read_data(idx));
        if (!r.ok) continue;
        for (const jade::SubEntry& s : jade::walk_sub_entries(r.data)) {
            const uint32_t* grop = s.gro_null ? nullptr : &s.gro_type;
            if (!jade::is_geometry_entry(s.data.data(), s.data.size(), grop)) continue;

            jade::GeoInfo g = jade::parse_geometry(s.data.data(), s.data.size());
            std::ostringstream ln;
            ln << "G " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key) << ' ';
            if (!g.ok) {
                ln << "NULL";
            } else if (g.ps2) {
                ln << "PS2 v=" << g.version << " sc=" << g.ps2_stream_count
                   << " mc=" << g.ps2_mesh_count << " ic=" << g.ps2_item_count
                   << " psz=" << g.ps2_payload_size;
            } else {
                std::string sk;
                if (!g.mrm_present) {
                    sk = "-";
                } else {
                    std::string scrc = jade::hex8(
                        jade::crc32(s.data.data() + 36, g.skin_size));
                    sk = g.skin_present
                             ? scrc + "/" + std::to_string(g.skin_flags) + "/" +
                                   std::to_string(g.skin_nbones)
                             : "partial/" + scrc;
                }
                ln << "PC v=" << g.version << " f1=" << g.flags1 << " f2=" << g.flags2
                   << " pts=" << g.nb_points << " uvs=" << g.nb_uvs
                   << " el=" << g.nb_elements << " tris=" << g.nb_tris
                   << " mrm=" << g.mrm_marker << " sk3=" << g.skin_ok3
                   << " hascol=" << (g.has_colors ? 1 : 0)
                   << " vcrc=" << crc_vec(g.vertices)
                   << " ncrc=" << crc_vec(g.normals)
                   << " ucrc=" << crc_vec(g.uvs)
                   << " ccrc=" << crc_vec(g.colors)
                   << " ecrc=" << crc_vec(g.elements)
                   << " fcrc=" << crc_vec(g.faces)
                   << " sk=" << sk
                   << " eoff=" << g.elements_offset;
                // Cooked trailing matId table offsets (matId edit target).
                std::vector<size_t> cooked =
                    jade::cooked_element_matid_offsets(s.data.data(), s.data.size());
                std::vector<uint32_t> ck32(cooked.begin(), cooked.end());
                ln << " ckc=" << crc_vec(ck32);
            }
            lines.push_back(ln.str());
        }
    }

    os << "GEO v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "geos " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_geo_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_geo_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_geo_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
