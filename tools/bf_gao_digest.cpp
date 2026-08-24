// bf_gao_digest — find GAO records (sub-entry ext == ".gao"), parse, digest.
//
//   bf_gao_digest <file.bf> [out] [limit]
//
// Diffed against core.gao by tests/run_golden.py. Matrices are CRC-32'd as raw
// 68-byte regions; keys emitted as hex; gizmo pointer array as a CRC.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8, canon_name
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Gao.hpp"
#include "jade/SubEntry.hpp"

static std::string crcb(const std::vector<uint8_t>& v) {
    if (v.empty()) return "-";
    return jade::hex8(jade::crc32(v.data(), v.size()));
}
static std::string crcu(const std::vector<uint32_t>& v) {
    if (v.empty()) return "-";
    return jade::hex8(jade::crc32(reinterpret_cast<const uint8_t*>(v.data()), v.size() * 4));
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
            if (s.ext != ".gao") continue;
            jade::GaoInfo g = jade::parse_gao_full(s.data.data(), s.data.size());
            std::ostringstream ln;
            ln << "A " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key);
            if (!g.ok) {
                ln << " NULL";
                lines.push_back(ln.str());
                continue;
            }
            // Offset-chain fields (name_size, vis_size, bv) are validated
            // transitively: a wrong offset shifts the matrix CRC / keys below.
            ln << " ver=" << g.version << " ef=" << jade::hex8(g.editor_flags)
               << " id=" << jade::hex8(g.identity) << " name=";
            jade::canon_name(ln, g.name);
            ln << " gmt=" << (g.gmat_present ? std::to_string(g.gmat_type) : "-")
               << " gmc=" << crcb(g.gmat_raw)
               << " gro=" << (g.vis_read ? jade::hex8(g.gro_key) : "-")
               << " grm=" << (g.vis_read ? jade::hex8(g.grm_key) : "-")
               << " fkey=" << (g.hier_flag ? jade::hex8(g.father_key) : "-")
               << " lmc=" << crcb(g.lmat_raw)
               << " gizn=" << (g.gizmo_flat.size() / 2)
               << " gizc=" << crcu(g.gizmo_flat);
            lines.push_back(ln.str());
        }
    }

    os << "GAO v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "gaos " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_gao_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_gao_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_gao_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
