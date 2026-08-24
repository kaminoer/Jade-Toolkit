// bf_matresolve_digest — element -> material/texture resolution, digest.
//
//   bf_matresolve_digest <file.bf> [out] [limit]
//
// For each active entry (sorted, up to `limit`; 0 = all): decompress, walk the
// sub-entry stream, and for every gro_type-1 GEO that parses with a non-empty
// element list, run resolve_element_texture_keys (the GAO -> grm -> sub-material
// -> texture chain with the engine's matId clamp) and emit one line per GEO:
//   M <idx> <offset> <geokey> grm=<hex8|-> slots=<n> tex=<t0,t1,...>
// where ti is hex8 or '-' (unresolvable). Diffed against io_ops/mesh_swap.py's
// resolve_element_texture_keys by tests/run_golden.py.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Compression.hpp"
#include "jade/Geometry.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/SubEntry.hpp"

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
        for (const jade::SubEntry& s : subs) by_key[s.key] = &s;

        for (const jade::SubEntry& s : subs) {
            if (s.gro_null || s.gro_type != 1) continue;
            jade::GeoInfo geo = jade::parse_geometry(s.data.data(), s.data.size());
            if (!geo.ok || geo.elements.empty()) continue;

            std::vector<jade::ResolvedTex> tex =
                jade::resolve_element_texture_keys(subs, geo, s.key);

            uint32_t grm = 0;
            bool have_grm = jade::owning_grm_key(subs, s.key, grm);
            uint32_t slots = 0;
            if (have_grm) {
                auto it = by_key.find(grm);
                slots = static_cast<uint32_t>(jade::grm_sub_material_keys(
                    it == by_key.end() ? nullptr : it->second).size());
            }

            std::ostringstream ln;
            ln << "M " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
               << " grm=" << (have_grm ? jade::hex8(grm) : std::string("-"))
               << " slots=" << slots << " tex=";
            for (size_t t = 0; t < tex.size(); ++t) {
                if (t) ln << ',';
                ln << (tex[t].ok ? jade::hex8(tex[t].key) : std::string("-"));
            }
            lines.push_back(ln.str());
        }
    }

    os << "MATRESOLVE v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "geos " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_matresolve_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_matresolve_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_matresolve_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
