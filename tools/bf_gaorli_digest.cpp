// bf_gaorli_digest — host-GAO RLI rewrite (mesh_swap slice), digest.
//
//   bf_gaorli_digest <file.bf> [out] [limit]
//
// Same (GAO, GEO) locator as bf_rli_digest. For every pair with an RLI primary
// table, exercise the swap's lighting-consistency path:
//   orig = original_rli_colors(subs, geo_key, geo, geo's own vertices) — the
//          position-keyed remap (exercises the 3-decimal _qpos grouping), and
//   rwO  = rewrite_gao_instance_colors(gao, geo_key, nb, orig, geo)  (composed)
//   rwS  = rewrite_gao_instance_colors(gao, geo_key, nb, synthetic, geo)
// CRC each ('-'/'none' for the Python None). Diffed against io_ops/mesh_swap.py.
#include <array>
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
#include "jade/MeshSwap.hpp"
#include "jade/Rli.hpp"
#include "jade/SubEntry.hpp"

static std::string crc_bytes(const std::vector<uint8_t>& v) {
    if (v.empty()) return "none";
    return jade::hex8(jade::crc32(v.data(), v.size()));
}
static std::string crc_colors(const jade::OrigRliColors& oc) {
    if (!oc.ok) return "-";
    std::vector<uint8_t> buf;
    buf.reserve(oc.colors.size() * 4);
    for (const jade::Rgba4& c : oc.colors)
        for (int k = 0; k < 4; ++k) buf.push_back(static_cast<uint8_t>(c[static_cast<size_t>(k)]));
    return jade::hex8(jade::crc32(buf.data(), buf.size()));
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
            if (!s.gro_null && s.gro_type == 1) geo_keys.insert(s.key);
        }
        for (const jade::SubEntry& s : subs) {
            if (s.ext != ".gao") continue;
            jade::GaoInfo g = jade::parse_gao_full(s.data.data(), s.data.size());
            if (!g.ok || !g.vis_read) continue;
            for (uint32_t gk : jade::geo_group_members(g.gro_key, by_key, &geo_keys)) {
                auto it = by_key.find(gk);
                if (it == by_key.end()) continue;
                const jade::SubEntry* t = it->second;
                if (t->gro_null || t->gro_type != 1) continue;
                jade::GeoInfo geo = jade::parse_geometry(t->data.data(), t->data.size());
                if (!geo.ok || geo.ps2) continue;
                uint32_t nb = geo.nb_points;
                if (jade::find_primary_table(s.data.data(), s.data.size(), nb) < 0) continue;

                // original_rli_colors remapped onto the geo's OWN vertices.
                std::vector<std::array<double, 3>> verts;
                verts.reserve(geo.vertices.size() / 3);
                for (size_t v = 0; v + 2 < geo.vertices.size(); v += 3)
                    verts.push_back({double(geo.vertices[v]), double(geo.vertices[v + 1]),
                                     double(geo.vertices[v + 2])});
                jade::OrigRliColors orig =
                    jade::original_rli_colors(subs, gk, geo, verts);

                std::string rwo = "-";
                if (orig.ok)
                    rwo = crc_bytes(jade::rewrite_gao_instance_colors(
                        s.data.data(), s.data.size(), gk, nb, orig.colors,
                        t->data.data(), t->data.size()));

                std::vector<jade::Rgba4> synth(nb);
                for (uint32_t k = 0; k < nb; ++k)
                    synth[k] = {double((k * 13) & 255), double((k * 29) & 255),
                                double((k * 47) & 255), 255.0};
                std::string rws = crc_bytes(jade::rewrite_gao_instance_colors(
                    s.data.data(), s.data.size(), gk, nb, synth,
                    t->data.data(), t->data.size()));

                std::ostringstream ln;
                ln << "G " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
                   << ' ' << jade::hex8(gk) << " nb=" << nb
                   << " orig=" << crc_colors(orig) << " rwO=" << rwo << " rwS=" << rws;
                lines.push_back(ln.str());
            }
        }
    }

    os << "GAORLI v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "pairs " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_gaorli_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_gaorli_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_gaorli_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
