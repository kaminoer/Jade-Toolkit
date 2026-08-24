// bf_cobwrite_digest — validate the COB WRITE path by round-trip.
//
//   bf_cobwrite_digest <file.bf> [out] [limit]
//
// Same locator as bf_cob_digest: for every active entry (sorted, up to `limit`;
// 0 = all), walk the sub-entry stream and for each looks_like_cob sub that
// parse_cob accepts, re-serialize it and check serialize_cob(parse_cob(p)) == p
// against the ORIGINAL payload bytes (the real game data — not just an oracle).
// Emits one line per parseable COB with rt=1/0. Diffed against core.collision
// (dump_cobwrite) by tests/run_golden.py; both should report rt=1 for every COB.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Collision.hpp"
#include "jade/Compression.hpp"
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
    size_t ok_count = 0;
    for (size_t i = 0; i < emit_n; ++i) {
        uint32_t idx = active[i];
        jade::LzoResult r = jade::decompress_lzo(bf.read_data(idx));
        if (!r.ok) continue;
        for (const jade::SubEntry& s : jade::walk_sub_entries(r.data)) {
            if (!jade::looks_like_cob(s.ext.empty(), s.data.data(), s.data.size())) continue;
            jade::CobInfo c = jade::parse_cob(s.data.data(), s.data.size());
            if (!c.ok) continue;
            std::vector<uint8_t> ser = jade::serialize_cob(c);
            int rt = (ser == s.data) ? 1 : 0;
            ok_count += rt;
            std::ostringstream ln;
            ln << "C " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
               << " rt=" << rt << " n=" << s.data.size();
            lines.push_back(ln.str());
        }
    }

    os << "COBWRITE v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "cobs " << lines.size() << NL;
    os << "roundtrip_ok " << ok_count << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_cobwrite_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_cobwrite_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_cobwrite_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
