// bf_cob_digest — find COB (collision) sub-entries, parse, digest.
//
//   bf_cob_digest <file.bf> [out] [limit]
//
// For each active entry (sorted, up to `limit`; 0 = all): decompress, walk the
// sub-entry stream, and for every sub-entry looks_like_cob accepts, parse_cob
// and emit a line with counts + a CRC-32 per section (verts/normals/elements/
// proximity/trailing). Diffed against core.collision by tests/run_golden.py.
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
#include "jade/Crc32.hpp"
#include "jade/SubEntry.hpp"

template <typename T>
static std::string crch(const std::vector<T>& v) {
    return jade::hex8(jade::crc32(reinterpret_cast<const uint8_t*>(v.data()),
                                  v.size() * sizeof(T)));
}

// Serialize COB elements to their on-disk layout (header + inline triangles).
static std::string elements_crc(const std::vector<jade::CobElement>& els) {
    std::vector<uint8_t> b;
    for (const jade::CobElement& e : els) {
        b.push_back(uint8_t(e.n_tri)); b.push_back(uint8_t(e.n_tri >> 8));
        b.push_back(e.design); b.push_back(e.flag);
        uint32_t m = static_cast<uint32_t>(e.material);
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(m >> (8 * i)));
        for (uint16_t f : e.faces) { b.push_back(uint8_t(f)); b.push_back(uint8_t(f >> 8)); }
    }
    return jade::hex8(jade::crc32(b.data(), b.size()));
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
            if (!jade::looks_like_cob(s.ext.empty(), s.data.data(), s.data.size())) continue;
            uint8_t shape = s.data.empty() ? 0 : s.data[0];
            jade::CobInfo c = jade::parse_cob(s.data.data(), s.data.size());
            std::ostringstream ln;
            ln << "C " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
               << " shape=" << int(shape) << ' ';
            if (!c.ok) {
                ln << "NULL";
            } else {
                ln << "type=" << int(c.type) << " flag=" << int(c.flag)
                   << " nv=" << c.n_verts << " nf=" << c.n_faces
                   << " ne=" << c.n_elems << " tris=" << c.total_tris
                   << " vcrc=" << crch(c.verts) << " ncrc=" << crch(c.normals)
                   << " ecrc=" << elements_crc(c.elements)
                   << " pcrc=" << crch(c.proximity)
                   << " tlen=" << c.trailing.size() << " tcrc=" << crch(c.trailing);
            }
            lines.push_back(ln.str());
        }
    }

    os << "COB v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "cobs " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_cob_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_cob_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_cob_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
