// bf_objkind_digest — classify every GAO in a bin (object_kinds) and digest.
//
//   bf_objkind_digest <file.bf> [out] [limit]
//
// For each active entry: walk sub-entries, build {key->sub} + the .gao key set,
// and for each GAO record classify_object + is_bone with its visual-target
// gro_type and father-in-bin context. Diffed against core.object_kinds.
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
#include "jade/Gao.hpp"
#include "jade/ObjectKinds.hpp"
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
        std::unordered_set<uint32_t> gao_keys;
        for (const jade::SubEntry& s : subs) {
            by_key[s.key] = &s;
            if (s.ext == ".gao") gao_keys.insert(s.key);
        }
        for (const jade::SubEntry& s : subs) {
            if (s.ext != ".gao") continue;
            // walk_sub_entries already split off the ".gao" tag (it is the ext),
            // so the payload is the bare struct -> parse_gao_full, not _record.
            jade::GaoInfo g = jade::parse_gao_full(s.data.data(), s.data.size());
            if (!g.ok) continue;
            int gro_type = -1;
            if (g.vis_read) {
                auto it = by_key.find(g.gro_key);
                if (it != by_key.end() && !it->second->gro_null)
                    gro_type = static_cast<int>(it->second->gro_type);
            }
            bool father_in_bin = g.hier_flag && gao_keys.count(g.father_key);
            std::string cat = jade::classify_object(g.name, g.identity, gro_type, father_in_bin);
            bool bone = jade::is_bone(g.name, g.identity, father_in_bin);
            std::ostringstream ln;
            ln << "OK " << idx << ' ' << jade::hex8(s.key) << " cat=" << cat
               << " bone=" << (bone ? 1 : 0);
            lines.push_back(ln.str());
        }
    }

    os << "OBJKIND v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "gaos " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_objkind_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_objkind_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_objkind_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
