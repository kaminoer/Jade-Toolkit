// bf_lightwrite_digest — validate write_light_fields against core.light.
//
//   bf_lightwrite_digest <file.bf> [out] [limit]
//
// Same locator as bf_light_digest. For every GRO_Light payload:
//   rtok = write_light_fields(all current values) == the original bytes (proves
//          the writer reproduces real data byte-exactly, not just C++==Python), and
//   inv  = CRC of write_light_fields(type=2, diffuse/specular inverted, fixed
//          near/far/inner/outer/intensity) — exercises every field writer.
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
#include "jade/Light.hpp"
#include "jade/SubEntry.hpp"

static std::array<int, 3> unpack_rgb(uint32_t raw) {
    return {int(raw & 0xFF), int((raw >> 8) & 0xFF), int((raw >> 16) & 0xFF)};
}
static float bits_to_f(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
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
            if (s.gro_null || s.gro_type != jade::GRO_LIGHT) continue;
            if (!jade::is_light_payload(s.data.data(), s.data.size())) continue;
            jade::LightInfo li = jade::parse_light(s.data.data(), s.data.size());

            // round-trip: write every present field back at its current value.
            jade::LightEdit rt;
            rt.light_type = li.type;
            if (li.diffuse.present)  rt.diffuse  = unpack_rgb(li.diffuse.bits);
            if (li.specular.present) rt.specular = unpack_rgb(li.specular.bits);
            if (li.near_.present)    rt.near_    = bits_to_f(li.near_.bits);
            if (li.far_.present)     rt.far_     = bits_to_f(li.far_.bits);
            if (li.inner.present)    rt.inner    = bits_to_f(li.inner.bits);
            if (li.outer.present)    rt.outer    = bits_to_f(li.outer.bits);
            if (li.intensity.present) rt.intensity = bits_to_f(li.intensity.bits);
            std::vector<uint8_t> rtb = jade::write_light_fields(s.data.data(), s.data.size(), rt);
            int rtok = (rtb == s.data) ? 1 : 0;

            // fixed transform exercising every writer.
            jade::LightEdit inv;
            inv.light_type = 2;
            if (li.diffuse.present) {
                std::array<int, 3> d = unpack_rgb(li.diffuse.bits);
                inv.diffuse = {255 - d[0], 255 - d[1], 255 - d[2]};
            }
            if (li.specular.present) {
                std::array<int, 3> sp = unpack_rgb(li.specular.bits);
                inv.specular = {255 - sp[0], 255 - sp[1], 255 - sp[2]};
            }
            inv.near_ = 1.5f; inv.far_ = 100.0f; inv.inner = 0.5f; inv.outer = 1.0f;
            inv.intensity = 2.0f;
            std::vector<uint8_t> invb = jade::write_light_fields(s.data.data(), s.data.size(), inv);

            std::ostringstream ln;
            ln << "L " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
               << " ver=" << li.version << " rtok=" << rtok
               << " inv=" << jade::hex8(jade::crc32(invb.data(), invb.size()));
            lines.push_back(ln.str());
        }
    }

    os << "LIGHTWRITE v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "lights " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_lightwrite_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_lightwrite_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_lightwrite_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
