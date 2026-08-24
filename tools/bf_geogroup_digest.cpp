// bf_geogroup_digest — validate gao.geo_group_members on real bins.
//
//   bf_geogroup_digest <file.bf> [out] [limit]
//
// For each active entry: walk its sub-entries, build {key -> sub} + the
// gro_type-1 key set, and run geo_group_members on every sub-entry's own key.
// Emits only the NON-trivial expansions (a group sub-entry resolving to member
// GEO keys, != [self]); diffed against core.gao.geo_group_members.
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
        for (const jade::SubEntry& s : subs) by_key[s.key] = &s;  // last wins (dict)
        std::unordered_set<uint32_t> geo_keys;
        for (const auto& kv : by_key)
            if (kv.second && !kv.second->gro_null && kv.second->gro_type == 1)
                geo_keys.insert(kv.first);

        for (const jade::SubEntry& s : subs) {
            std::vector<uint32_t> mem = jade::geo_group_members(s.key, by_key, &geo_keys);
            if (mem.size() == 1 && mem[0] == s.key) continue;  // trivial [self]
            std::ostringstream ln;
            ln << "GG " << idx << ' ' << jade::hex8(s.key) << " n=" << mem.size()
               << " crc=" << jade::hex8(jade::crc32(
                      reinterpret_cast<const uint8_t*>(mem.data()), mem.size() * 4));
            lines.push_back(ln.str());
        }
    }

    os << "GEOGROUP v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "groups " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_geogroup_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_geogroup_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_geogroup_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
