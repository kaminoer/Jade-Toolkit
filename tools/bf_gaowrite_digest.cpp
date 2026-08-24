// bf_gaowrite_digest — GAO write helpers (object_placer primitives), digest.
//
//   bf_gaowrite_digest <file.bf> [out] [limit]
//
// For every .gao sub-entry: emit the matrix/BV offsets + bounds, prove the
// matrix WRITE round-trips against the real bytes (write back the floats read
// from the payload -> identical), and exercise extend_obbox_to_include with
// both the no-op (current bounds) and a deterministic grow. Diffed against
// core.gao by tests/run_golden.py.
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Gao.hpp"
#include "jade/SubEntry.hpp"

static std::string crc_or_none(const std::vector<uint8_t>& v) {
    if (v.empty()) return "none";
    return jade::hex8(jade::crc32(v.data(), v.size()));
}
static float rdf(const uint8_t* d, size_t o) {
    uint32_t b = d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (uint32_t(d[o + 3]) << 24);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
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
            const uint8_t* d = s.data.data();
            size_t n = s.data.size();

            long long gmo = jade::global_matrix_offset(d, n);
            long long bvo = jade::obbox_offset(d, n);
            jade::ObboxBounds bl = jade::obbox_local_bounds(d, n);

            // Matrix write round-trip: write back the floats read at the slot.
            int rt = -1;
            if (gmo >= 0) {
                std::array<double, 16> m{};
                for (int k = 0; k < 16; ++k)
                    m[size_t(k)] = double(rdf(d, size_t(gmo) + size_t(k) * 4));
                std::vector<uint8_t> w = jade::write_global_matrix(d, n, m);
                rt = (w == s.data) ? 1 : 0;
            }

            // extend: no-op with current bounds; grow by a fixed delta.
            std::string ext_same = "none", ext_grow = "none";
            if (bl.ok) {
                std::vector<uint8_t> e1 = jade::extend_obbox_to_include(d, n, bl.mn, bl.mx);
                ext_same = e1.empty() ? "none" : (e1 == s.data ? "same" : crc_or_none(e1));
                std::array<double, 3> gmn{bl.mn[0] - 1.5, bl.mn[1] - 1.5, bl.mn[2] - 1.5};
                std::array<double, 3> gmx{bl.mx[0] + 2.25, bl.mx[1] + 2.25, bl.mx[2] + 2.25};
                ext_grow = crc_or_none(jade::extend_obbox_to_include(d, n, gmn, gmx));
            }

            std::ostringstream ln;
            ln << "W " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
               << " gmo=" << gmo << " bvo=" << bvo << " bl=" << (bl.ok ? 1 : 0)
               << " rt=" << rt << " es=" << ext_same << " eg=" << ext_grow;
            lines.push_back(ln.str());
        }
    }

    os << "GAOWRITE v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "gaos " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_gaowrite_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_gaowrite_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_gaowrite_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
