// bf_light_digest — find GRO_Light (gro_type 2) sub-entries, parse, digest.
//
//   bf_light_digest <file.bf> [out] [limit]
//
// For each active entry (sorted, up to `limit`; 0 = all): decompress, walk the
// sub-entry stream, and for every gro_type==2 sub-entry that is_light_payload
// accepts, parse_light and emit a line. Float/colour fields are emitted as their
// raw u32 bits (hex) so the diff vs core.light is exact without float formatting.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Compression.hpp"
#include "jade/Light.hpp"
#include "jade/SubEntry.hpp"

static std::string hf(const jade::LightField& f) {
    return f.present ? jade::hex8(f.bits) : std::string("-");
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
            if (s.gro_null || s.gro_type != jade::GRO_LIGHT) continue;
            if (!jade::is_light_payload(s.data.data(), s.data.size())) continue;
            jade::LightInfo li = jade::parse_light(s.data.data(), s.data.size());
            std::ostringstream ln;
            ln << "L " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
               << " ver=" << li.version << " flags=" << li.flags
               << " type=" << li.type << " tn=" << li.type_name
               << " diff=" << hf(li.diffuse) << " spec=" << hf(li.specular)
               << " near=" << hf(li.near_) << " far=" << hf(li.far_)
               << " inner=" << hf(li.inner) << " outer=" << hf(li.outer)
               << " intens=" << hf(li.intensity)
               << " hasI=" << (li.has_intensity ? 1 : 0)
               << " hasS=" << (li.has_specular ? 1 : 0)
               << " size=" << li.size << " base=" << li.base_size;
            lines.push_back(ln.str());
        }
    }

    os << "LIGHT v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "lights " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_light_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_light_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_light_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
