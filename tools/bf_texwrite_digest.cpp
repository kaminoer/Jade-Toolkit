// bf_texwrite_digest — validate the BGRA texture encoder by round-trip.
//
//   bf_texwrite_digest <file.bf> [out] [limit]
//
// Same locator as bf_tex_digest. For every BGRA (format 0) texture that isn't a
// header-only stub: decode to RGBA, re-encode with encode_bgra, and check the
// result equals the ORIGINAL base-level pixel bytes. Emits rt=1/0 per texture;
// diffed against core.texture (dump_texwrite). Both should report rt=1 for all.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
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
    size_t ok_count = 0;
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
            if (!ti.valid || ti.format != 0) continue;           // BGRA only
            if (jade::is_placeholder(ti, subn - ti.pix_start)) continue;
            std::vector<uint8_t> rgba = jade::decode_texture(sub, subn, ti);
            if (rgba.empty()) continue;
            std::vector<uint8_t> enc = jade::encode_bgra(rgba.data(), ti.width, ti.height);
            size_t n = static_cast<size_t>(ti.width) * ti.height * 4;
            int rt = 0;
            if (enc.size() == n && ti.pix_start + n <= subn &&
                std::equal(enc.begin(), enc.end(), sub + ti.pix_start))
                rt = 1;
            ok_count += rt;
            std::ostringstream ln;
            ln << "T " << idx << ' ' << off << ' ' << ti.width << ' ' << ti.height
               << " rt=" << rt << " n=" << n;
            lines.push_back(ln.str());
        }
    }

    os << "TEXWRITE v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "bgra " << lines.size() << NL;
    os << "roundtrip_ok " << ok_count << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_texwrite_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_texwrite_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_texwrite_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
