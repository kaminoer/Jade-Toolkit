// bf_skeleton_digest — build the bone-node forest per bin, digest it.
//
//   bf_skeleton_digest <file.bf> [out] [limit]
//
// Diffed against core.skeleton (build_bone_nodes, bone-forest half) by
// tests/run_golden.py. For each active entry (up to `limit`; 0 = all):
// decompress, walk the sub-entry stream, build_bone_nodes. When a forest is
// produced (ok), emit a per-bin header line then one line per bone node.
//
// Per node we emit: node index, source gao key (hex), child-index array CRC,
// matrix presence + matrix CRC (16 col-major floats packed '<16f', so the diff
// is exact without float formatting), and the canonicalised name.
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8, canon_name
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Skeleton.hpp"
#include "jade/SubEntry.hpp"

static std::string crc_u32(const std::vector<uint32_t>& v) {
    if (v.empty()) return "-";
    return jade::hex8(jade::crc32(reinterpret_cast<const uint8_t*>(v.data()),
                                  v.size() * 4));
}

static std::string crc_mat(const std::vector<float>& m) {
    if (m.size() != 16) return "-";
    uint8_t buf[64];
    for (int i = 0; i < 16; ++i) std::memcpy(buf + i * 4, &m[i], 4);
    return jade::hex8(jade::crc32(buf, 64));
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
    size_t bins_with = 0;
    for (size_t i = 0; i < emit_n; ++i) {
        uint32_t idx = active[i];
        jade::LzoResult r = jade::decompress_lzo(bf.read_data(idx));
        if (!r.ok) continue;
        jade::BoneForest f = jade::build_bone_nodes(jade::walk_sub_entries(r.data));
        if (!f.ok) continue;
        ++bins_with;
        std::ostringstream bn;
        bn << "B " << idx << " nodes=" << f.nodes.size();
        lines.push_back(bn.str());
        for (size_t ni = 0; ni < f.nodes.size(); ++ni) {
            const jade::BoneNode& n = f.nodes[ni];
            std::ostringstream ln;
            ln << "N " << idx << ' ' << ni << ' ' << jade::hex8(n.key)
               << " ch=" << crc_u32(n.children)
               << " nch=" << n.children.size()
               << " hasm=" << (n.has_matrix ? 1 : 0)
               << " mc=" << (n.has_matrix ? crc_mat(n.matrix) : std::string("-"))
               << " name=";
            jade::canon_name(ln, n.name);
            lines.push_back(ln.str());
        }
    }

    os << "SKELETON v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "skeletons " << bins_with << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_skeleton_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_skeleton_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_skeleton_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
