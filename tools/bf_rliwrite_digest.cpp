// bf_rliwrite_digest — validate the RLI write path against core.rli.
//
//   bf_rliwrite_digest <file.bf> [out] [limit]
//
// Same locator as bf_rli_digest. For each (GAO, GEO) RLI pair, exercise every
// writer with a fixed/deterministic input and CRC the result:
//   inv = transform_rli(invert),  wr = write_rli(synthetic base colours),
//   wec = write_extra_colors(synthetic cooked colours),
//   weu = write_extra_uv1(synthetic uv1).
// A writer that returns None (no table / no net change) emits "none". Diffed
// against core.rli (dump_rliwrite) — the synthetic inputs are generated
// identically on both sides, so a matching line means the writer is byte-exact.
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/Rli.hpp"
#include "jade/SubEntry.hpp"

static std::string crc_or_none(const std::vector<uint8_t>& v) {
    if (v.empty()) return "none";
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
        std::vector<jade::SubEntry> subs = jade::walk_sub_entries(r.data);
        std::unordered_map<uint32_t, const jade::SubEntry*> by_key;
        std::unordered_set<uint32_t> geo_keys;
        for (const jade::SubEntry& s : subs) {
            by_key[s.key] = &s;
            if (!s.gro_null && s.gro_type == 1) geo_keys.insert(s.key);
        }
        for (const jade::SubEntry& s : subs) {
            if (s.ext != ".gao") continue;
            jade::GaoInfo g = jade::parse_gao_full(s.data.data(), s.data.size());
            if (!g.ok || !g.vis_read) continue;
            for (uint32_t gk : jade::geo_group_members(g.gro_key, by_key, &geo_keys)) {
                auto it = by_key.find(gk);
                if (it == by_key.end()) continue;
                const jade::SubEntry* t = it->second;
                if (t->gro_null || t->gro_type != 1) continue;
                jade::GeoInfo geo = jade::parse_geometry(t->data.data(), t->data.size());
                if (!geo.ok || geo.ps2) continue;
                uint32_t nb = geo.nb_points;

                const uint8_t* gd = s.data.data();
                size_t gn = s.data.size();
                if (jade::find_primary_table(gd, gn, nb) < 0) continue;
                const uint8_t* geod = t->data.data();
                size_t geon = t->data.size();

                // (1) transform_rli: fixed invert.
                std::vector<uint8_t> inv = jade::transform_rli(
                    gd, gn, nb, [](int cr, int cg, int cb) {
                        return std::array<int, 3>{255 - cr, 255 - cg, 255 - cb};
                    });

                // (2) write_rli: synthetic per-base colours.
                std::vector<jade::RgbF> base_colors(nb);
                for (uint32_t k = 0; k < nb; ++k)
                    base_colors[k] = {double((k * 13) & 255), double((k * 29) & 255), double((k * 47) & 255)};
                std::vector<uint8_t> wr = jade::write_rli(gd, gn, nb, base_colors, geod, geon, true);

                // extra-block count for the per-cooked-vertex writers.
                jade::ExtraColors ec = jade::read_extra_colors(gd, gn, nb);
                uint32_t cnt = ec.ok ? ec.count_exp : 0;

                // (3) write_extra_colors: synthetic per-cooked colours.
                std::vector<jade::RgbF> cooked(cnt);
                for (uint32_t k = 0; k < cnt; ++k)
                    cooked[k] = {double((k * 7) & 255), double((k * 11) & 255), double((k * 17) & 255)};
                std::vector<uint8_t> wec = jade::write_extra_colors(gd, gn, nb, cooked, geod, geon);

                // (4) write_extra_uv1: synthetic per-cooked uv.
                std::vector<std::array<float, 2>> uv1(cnt);
                for (uint32_t k = 0; k < cnt; ++k)
                    uv1[k] = {float(k) * 0.5f, float(k) * 0.25f};
                std::vector<uint8_t> weu = jade::write_extra_uv1(gd, gn, nb, uv1);

                std::ostringstream ln;
                ln << "R " << idx << ' ' << s.offset << ' ' << jade::hex8(s.key)
                   << ' ' << jade::hex8(gk) << " nb=" << nb << " cnt=" << cnt
                   << " inv=" << crc_or_none(inv) << " wr=" << crc_or_none(wr)
                   << " wec=" << crc_or_none(wec) << " weu=" << crc_or_none(weu);
                lines.push_back(ln.str());
            }
        }
    }

    os << "RLIWRITE v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "rlis " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_rliwrite_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_rliwrite_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_rliwrite_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
