// bf_subentry_digest — walk the sub-entry stream of each BigFile entry, digest.
//
//   bf_subentry_digest <file.bf> [out] [limit]
//
// For each active entry (sorted by index, up to `limit`; 0 = all): decompress
// it, walk_sub_entries the decompressed stream, and emit one line per record
// (offset, size, key, ext/gro_type, payload length + CRC-32). tests/run_golden.py
// diffs this against the Python oracle (core.sub_entry.walk_sub_entries),
// proving the stream walk on real v37/v38 data.
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
        for (const jade::SubEntry& s : jade::walk_sub_entries(r.data)) {
            std::ostringstream ln;
            ln << "S " << idx << ' ' << s.offset << ' ' << s.size << ' '
               << jade::hex8(s.key) << ' '
               << (s.ext.empty() ? "-" : s.ext) << ' '
               << (s.gro_null ? std::string("-") : std::to_string(s.gro_type)) << ' '
               << s.data.size() << ' ' << jade::hex8(jade::crc32(s.data));
            lines.push_back(ln.str());
        }
    }

    os << "SUBENT v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "records " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_subentry_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_subentry_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_subentry_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
