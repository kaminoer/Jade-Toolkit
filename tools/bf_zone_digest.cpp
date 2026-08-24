// bf_zone_digest — discover + analyze playable zones, digest per-zone stats.
//
//   bf_zone_digest <file.bf> [out] [limit]
//
// Builds the _wow_ resource index once, discovers _wow_/_wol_ zone pairs, and
// analyzes the first `limit` (0 = all): record/GAO counts, map prefix, the
// player CheckPoint GAO, and _wol_ dependency health. Diffed against core.zone
// (an integration check over wow_stream + gao + wol + keys).
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8, canon_name
#include "jade/Keys.hpp"
#include "jade/Zone.hpp"

static void emit(std::ostream& os, const jade::BigFile& bf, size_t limit) {
    const char NL = '\n';
    std::unordered_set<uint32_t> index = jade::build_wow_resource_index(bf);
    std::vector<jade::ZoneInfo> zones = jade::discover_zones(bf);
    size_t emit_n = (limit == 0) ? zones.size() : std::min(limit, zones.size());

    os << "ZONE v1" << NL;
    os << "limit " << limit << NL;
    os << "zone_total " << zones.size() << NL;
    os << "emitted " << emit_n << NL;

    for (size_t i = 0; i < emit_n; ++i) {
        jade::ZoneInfo& z = zones[i];
        jade::analyze_zone(bf, z, index);
        std::ostringstream ln;
        ln << "Z ";
        jade::canon_name(ln, z.name);
        ln << " rc=" << z.record_count << " gc=" << z.gao_count
           << " mp=" << (z.map_prefix < 0 ? std::string("-") : std::to_string(z.map_prefix))
           << " cpk=" << (z.checkpoint_key < 0 ? std::string("-")
                          : jade::hex8(static_cast<uint32_t>(z.checkpoint_key)))
           << " cpn=";
        if (z.has_checkpoint) jade::canon_name(ln, z.checkpoint_name);
        else ln << "-";
        ln << " dt=" << z.dep_total << " dm=" << z.dep_missing
           << " load=" << (z.has_checkpoint ? 1 : 0)
           << " wik=" << (z.wol_internal_key < 0 ? std::string("-")
                          : jade::hex8(static_cast<uint32_t>(z.wol_internal_key)));
        os << ln.str() << NL;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_zone_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_zone_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_zone_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
