// bf_dxtenc_digest — validate the DXT encoders against the toolkit's quicktex.
//
//   bf_dxtenc_digest <file.bf> [out] [limit]
//
// Same locator as bf_tex_digest. For every decodable texture with multiple-of-4
// dims, decode to RGBA (the already-golden decode path, so the input is identical
// on both sides) and re-encode with encode_dxt1 + encode_dxt5, emitting a CRC of
// each. Diffed against core.texture (dump_dxtenc) — a matching line means the
// vendored quicktex BC1/BC3 (and the hand DXT1 path) reproduce the toolkit's
// output byte-for-byte for that image.
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

static std::string crc(const std::vector<uint8_t>& v) {
    return jade::hex8(jade::crc32(v.data(), v.size()));
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
        const uint8_t* d = r.data.data();
        size_t dn = r.data.size();
        for (size_t off : jade::find_texture_offsets(d, dn)) {
            const uint8_t* sub = d + off;
            size_t subn = dn - off;
            jade::TexInfo ti = jade::parse_texture(sub, subn);
            if (!ti.valid) continue;
            if (jade::is_placeholder(ti, subn - ti.pix_start)) continue;
            uint32_t w = ti.width, h = ti.height;
            if (w == 0 || h == 0 || (w % 4) != 0 || (h % 4) != 0) continue;  // DXT needs mult-of-4
            std::vector<uint8_t> rgba = jade::decode_texture(sub, subn, ti);
            if (rgba.size() != static_cast<size_t>(w) * h * 4) continue;

            bool has_alpha = false;
            for (size_t k = 0; k < static_cast<size_t>(w) * h; ++k)
                if (rgba[k * 4 + 3] < 128) { has_alpha = true; break; }

            std::vector<uint8_t> e1 = jade::encode_dxt1(rgba.data(), w, h);
            std::vector<uint8_t> e5 = jade::encode_dxt5(rgba.data(), w, h);
            std::ostringstream ln;
            ln << "E " << idx << ' ' << off << ' ' << w << ' ' << h
               << " a=" << (has_alpha ? 1 : 0)
               << " dxt1=" << crc(e1) << " dxt5=" << crc(e5);
            lines.push_back(ln.str());
        }
    }

    os << "DXTENC v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "textures " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_dxtenc_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_dxtenc_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_dxtenc_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
