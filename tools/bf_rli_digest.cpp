// bf_rli_digest — find GAOs with baked RLI vertex lighting, parse, digest.
//
//   bf_rli_digest <file.bf> [out] [limit]
//
// For each active entry (sorted, up to `limit`; 0 = all): decompress, walk the
// sub-entry stream, and for every ".gao" record that has a visual block, resolve
// its gro_key to one or more gro_type-1 GEO sub-entries (geo_group_members). For
// each (GAO, GEO) pair whose GAO carries an RLI primary table (has_rli, sized by
// the GEO's base vertex count nb_points), emit one canonical line: the primary
// colours, the expanded extra block, the expanded->base mapping and the cooked
// mesh (positions/uvs/faces), all as CRC-32 over their raw little-endian bytes so
// the diff vs core.rli is exact without float formatting.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/Rli.hpp"
#include "jade/SubEntry.hpp"

// CRC of an (r,g,b,a) colour list as raw r,g,b,a bytes (matches Python
// bytes(c for t in seq for c in t)). "-" when empty.
static std::string crc_colors(const std::vector<jade::RgbaColor>& cols) {
    if (cols.empty()) return "-";
    std::vector<uint8_t> buf;
    buf.reserve(cols.size() * 4);
    for (const jade::RgbaColor& c : cols) {
        buf.push_back(c.r); buf.push_back(c.g); buf.push_back(c.b); buf.push_back(c.a);
    }
    return jade::hex8(jade::crc32(buf.data(), buf.size()));
}
// CRC of a float array as raw LE bytes (matches struct.pack '<Nf'). "-" empty.
static std::string crc_floats(const std::vector<float>& v) {
    if (v.empty()) return "-";
    return jade::hex8(jade::crc32(reinterpret_cast<const uint8_t*>(v.data()), v.size() * 4));
}
static std::string crc_u32(const std::vector<uint32_t>& v) {
    if (v.empty()) return "-";
    return jade::hex8(jade::crc32(reinterpret_cast<const uint8_t*>(v.data()), v.size() * 4));
}
static std::string crc_u16(const std::vector<uint16_t>& v) {
    if (v.empty()) return "-";
    return jade::hex8(jade::crc32(reinterpret_cast<const uint8_t*>(v.data()), v.size() * 2));
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

        std::vector<jade::SubEntry> subs = jade::walk_sub_entries(r.data);
        std::unordered_map<uint32_t, const jade::SubEntry*> by_key;
        std::unordered_set<uint32_t> geo_keys;
        for (const jade::SubEntry& s : subs) {
            by_key[s.key] = &s;
            if (s.gro_null == false && s.gro_type == 1) geo_keys.insert(s.key);
        }

        for (const jade::SubEntry& s : subs) {
            if (s.ext != ".gao") continue;
            jade::GaoInfo g = jade::parse_gao_full(s.data.data(), s.data.size());
            if (!g.ok || !g.vis_read) continue;   // visual block required

            std::vector<uint32_t> members =
                jade::geo_group_members(g.gro_key, by_key, &geo_keys);
            for (uint32_t gk : members) {
                auto it = by_key.find(gk);
                if (it == by_key.end()) continue;
                const jade::SubEntry* t = it->second;
                if (t->gro_null || t->gro_type != 1) continue;
                jade::GeoInfo geo = jade::parse_geometry(t->data.data(), t->data.size());
                if (!geo.ok || geo.ps2) continue;
                uint32_t nb = geo.nb_points;

                const uint8_t* gd = s.data.data();
                size_t gn = s.data.size();
                long long cs = jade::find_primary_table(gd, gn, nb);
                if (cs < 0) continue;

                jade::PrimaryColors pc = jade::read_primary_colors(gd, gn, nb);
                jade::ExtraColors    ec = jade::read_extra_colors(gd, gn, nb);
                jade::ExpandedToBase e2b = jade::expanded_to_base(
                    t->data.data(), t->data.size(), nb);
                jade::CookedMesh cm = jade::cooked_mesh(
                    t->data.data(), t->data.size());

                std::ostringstream ln;
                ln << "R " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
                   << ' ' << jade::hex8(gk) << " nb=" << nb << " cs=" << cs
                   << " primn=" << pc.colors.size()
                   << " primcrc=" << crc_colors(pc.colors)
                   << " extn=" << (ec.ok ? std::to_string(ec.count_exp) : std::string("-"))
                   << " extcrc=" << (ec.ok ? crc_colors(ec.colors) : std::string("-"))
                   << " e2bn=" << (e2b.ok ? std::to_string(e2b.map.size()) : std::string("-"))
                   << " e2bcrc=" << (e2b.ok ? crc_u32(e2b.map) : std::string("-"))
                   << " cmn=" << (cm.ok ? std::to_string(cm.n) : std::string("-"))
                   << " cmf=" << (cm.ok ? std::to_string(cm.faces.size() / 3) : std::string("-"))
                   << " poscrc=" << (cm.ok ? crc_floats(cm.positions) : std::string("-"))
                   << " uvcrc=" << (cm.ok ? crc_floats(cm.uvs) : std::string("-"))
                   << " facecrc=" << (cm.ok ? crc_u16(cm.faces) : std::string("-"));
                lines.push_back(ln.str());
            }
        }
    }

    os << "RLI v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "rlis " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_rli_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_rli_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_rli_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
