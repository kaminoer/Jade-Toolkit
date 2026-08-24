// bf_mat_digest — find material sub-entries (gro_type 3/4/5), parse, digest.
//
//   bf_mat_digest <file.bf> [out] [limit]
//
// Diffed against core.material by tests/run_golden.py. u32 fields emitted as
// hex; key arrays as a CRC-32 of their raw LE bytes.
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
#include "jade/Material.hpp"
#include "jade/SubEntry.hpp"

static std::string keys_crc(const std::vector<uint32_t>& v) {
    if (v.empty()) return "-";
    return jade::hex8(jade::crc32(reinterpret_cast<const uint8_t*>(v.data()),
                                  v.size() * 4));
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
            if (s.gro_null) continue;
            int gt = static_cast<int>(s.gro_type);
            if (gt != jade::GRO_TYPE_MAT_SINGLE && gt != jade::GRO_TYPE_MAT_MULTI &&
                gt != jade::GRO_TYPE_MAT_MULTITEXTURE)
                continue;
            jade::MatInfo m = jade::parse_material(s.data.data(), s.data.size(), gt);
            std::ostringstream ln;
            ln << "M " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
               << ' ' << gt << ' ';
            if (!m.ok) {
                ln << "NULL";
            } else if (m.type == 0) {
                ln << "single amb=" << jade::hex8(m.ambient) << " dif=" << jade::hex8(m.diffuse)
                   << " spec=" << jade::hex8(m.specular) << " sexp=" << jade::hex8(m.sexp_bits)
                   << " opac=" << jade::hex8(m.opac_bits) << " flags=" << jade::hex8(m.flags)
                   << " tkey=" << jade::hex8(m.texture_key) << " vmask=" << jade::hex8(m.validate_mask)
                   << " tk=" << keys_crc(m.texture_keys);
            } else if (m.type == 1) {
                ln << "mtex amb=" << jade::hex8(m.ambient) << " dif=" << jade::hex8(m.diffuse)
                   << " spec=" << jade::hex8(m.specular) << " sexpraw=" << jade::hex8(m.sexpraw)
                   << " hasver=" << (m.has_version ? 1 : 0) << " opac=" << jade::hex8(m.opac_bits)
                   << " flags=" << jade::hex8(m.flags) << " first=" << jade::hex8(m.texture_key)
                   << " vmask=" << jade::hex8(m.validate_mask) << " hdr=" << m.hdr_size
                   << " tk=" << keys_crc(m.texture_keys);
            } else {
                ln << "multi nsub=" << m.n_sub << " nread=" << m.sub_material_keys.size()
                   << " skcrc=" << keys_crc(m.sub_material_keys);
            }
            lines.push_back(ln.str());
        }
    }

    os << "MAT v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "mats " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_mat_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_mat_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_mat_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
