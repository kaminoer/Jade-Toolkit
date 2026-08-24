// bf_tex_digest — find embedded textures in BigFile entries, decode, digest.
//
//   bf_tex_digest <file.bf> [out] [limit]
//
// For each active entry (sorted by index, up to `limit`; 0 = all): decompress
// it, magic-scan the decompressed bytes for embedded texture headers, parse and
// decode each (no palette -> PAL8 greyscale), and emit one line per texture with
// format/dims and a CRC-32 of the decoded RGBA. tests/run_golden.py diffs this
// against the Python oracle (core.texture, which uses texture2ddecoder for
// DXT1/DXT5), proving decode parity on real v36/v37/v38 textures. The magic scan
// stands in for the not-yet-ported sub-entry layer; both sides scan identically.
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

    std::vector<uint32_t> active;
    for (const auto& kv : bf.files) {
        const jade::BFFile& e = kv.second;
        if (!e.name.empty() && e.key != jade::INVALID_KEY) active.push_back(e.index);
    }
    size_t emit_n = (limit == 0) ? active.size() : std::min(limit, active.size());

    std::vector<std::string> lines;
    for (size_t i = 0; i < emit_n; ++i) {
        uint32_t idx = active[i];
        std::vector<uint8_t> raw = bf.read_data(idx);
        jade::LzoResult r = jade::decompress_lzo(raw);
        if (!r.ok) continue;

        const uint8_t* dec = r.data.data();
        size_t declen = r.data.size();
        for (size_t off : jade::find_texture_offsets(dec, declen)) {
            jade::TexInfo ti = jade::parse_texture(dec + off, declen - off);
            std::ostringstream ln;
            ln << "T " << idx << ' ' << off << ' ';
            if (!ti.valid) {
                ln << "INVALID";
            } else {
                ln << ti.format << ' ' << ti.width << ' ' << ti.height
                   << " ver=" << ti.version << " mip=" << ti.mip_count << ' ';
                std::vector<uint8_t> rgba =
                    jade::decode_texture(dec + off, declen - off, ti);
                if (rgba.empty())
                    ln << "NULL";
                else
                    ln << "len=" << rgba.size() << " crc=" << jade::hex8(jade::crc32(rgba));
            }
            lines.push_back(ln.str());
        }
    }

    os << "TEX v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "textures " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_tex_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_tex_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_tex_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
