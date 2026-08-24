// bf_grade_digest — validate texture.grade_texture on real textures with a
// fixed deterministic transform (RGB invert), exercising the DXT colour-block
// mode-preservation + LUT-remap logic.
//
//   bf_grade_digest <file.bf> [out] [limit]
//
// For each embedded texture (magic scan), grade the base-level extent with the
// invert transform and emit a CRC of the result (or NULL for an unsupported
// format / no change). Diffed against core.texture.grade_texture with the same
// transform.
#include <array>
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
#include "jade/Texture.hpp"

static void emit(std::ostream& os, const jade::BigFile& bf, size_t limit) {
    const char NL = '\n';
    // Fixed transform: invert each channel (must match dump_grade exactly).
    jade::ColorFn invert = [](std::array<int, 3> c) {
        return std::array<int, 3>{{255 - c[0], 255 - c[1], 255 - c[2]}};
    };

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
        const uint8_t* dec = r.data.data();
        size_t declen = r.data.size();
        for (size_t off : jade::find_texture_offsets(dec, declen)) {
            jade::TexInfo ti = jade::parse_texture(dec + off, declen - off);
            std::ostringstream ln;
            ln << "GR " << idx << ' ' << off << ' ';
            if (!ti.valid) {
                ln << "INVALID";
                lines.push_back(ln.str());
                continue;
            }
            size_t base = jade::base_level_size(ti.format, ti.width, ti.height);
            size_t extent = std::min(declen - off, ti.pix_start + base);
            std::vector<uint8_t> graded = jade::grade_texture(dec + off, extent, invert);
            ln << "fmt=" << ti.format << ' ';
            if (graded.empty())
                ln << "NULL";
            else
                ln << "crc=" << jade::hex8(jade::crc32(graded));
            lines.push_back(ln.str());
        }
    }

    os << "GRADE v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "textures " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_grade_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_grade_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_grade_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
