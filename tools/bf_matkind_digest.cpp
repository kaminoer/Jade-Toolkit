// bf_matkind_digest — validate the leaf-material readers (parse_material_header,
// parse_material_render_flags, resolve_texture_keys) on real sub-materials.
//
//   bf_matkind_digest <file.bf> [out] [limit]
//
// For each multi-material (gro_type 4) in a bin, resolve each sub_material_key
// against the bin's {key -> sub} map and run the kind-based readers on the leaf
// payload (its first u32 is a kind 4-9). This mirrors how the toolkit uses them
// and avoids false positives from GEO/light payloads. Diffed against core.material.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Material.hpp"
#include "jade/SubEntry.hpp"

static std::string join(const std::vector<std::string>& v) {
    if (v.empty()) return "-";
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += v[i]; }
    return s;
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
        for (const jade::SubEntry& s : subs) by_key[s.key] = &s;

        for (const jade::SubEntry& s : subs) {
            if (s.gro_null || s.gro_type != jade::GRO_TYPE_MAT_MULTI) continue;
            jade::MatInfo m = jade::parse_material(s.data.data(), s.data.size(),
                                                   jade::GRO_TYPE_MAT_MULTI);
            if (!m.ok) continue;
            for (size_t slot = 0; slot < m.sub_material_keys.size(); ++slot) {
                uint32_t sk = m.sub_material_keys[slot];
                if (sk == 0 || sk == 0xFFFFFFFFu) continue;
                auto it = by_key.find(sk);
                if (it == by_key.end()) continue;       // leaf not in this bin
                const std::vector<uint8_t>& ld = it->second->data;
                jade::MatHeader h = jade::parse_material_header(ld.data(), ld.size());
                if (!h.ok) continue;
                jade::MatRenderFlags rf =
                    jade::parse_material_render_flags(ld.data(), ld.size());
                std::vector<jade::TexLayer> layers =
                    jade::resolve_texture_keys(ld.data(), ld.size());
                std::vector<uint32_t> lflat;
                for (const jade::TexLayer& L : layers) {
                    lflat.push_back(L.layer_index);
                    lflat.push_back(static_cast<uint32_t>(L.offset));
                    lflat.push_back(L.key);
                }
                std::ostringstream ln;
                ln << "MK " << idx << ' ' << jade::hex8(s.key) << " slot=" << slot
                   << " leaf=" << jade::hex8(sk) << " kind=" << h.kind
                   << " amb=" << jade::hex8(h.ambient) << " dif=" << jade::hex8(h.diffuse)
                   << " spec=" << jade::hex8(h.specular)
                   << " sexpraw=" << jade::hex8(h.spec_exp_raw)
                   << " opacraw=" << jade::hex8(h.opacity_raw)
                   << " ulf=" << jade::hex8(rf.ul_flags) << " blend=" << rf.blend
                   << " bn=" << rf.blend_name << " cop=" << rf.colorop
                   << " cn=" << rf.colorop_name << " at=" << (rf.alpha_test ? 1 : 0)
                   << " athr=" << rf.alpha_thresh << " mode=" << rf.mode
                   << " flags=" << join(rf.flags) << " nlayers=" << layers.size()
                   << " lcrc=" << (lflat.empty() ? "-" : jade::hex8(jade::crc32(
                          reinterpret_cast<const uint8_t*>(lflat.data()), lflat.size() * 4)));
                lines.push_back(ln.str());
            }
        }
    }

    os << "MATKIND v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "leaves " << lines.size() << NL;
    for (const std::string& s : lines) os << s << NL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_matkind_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_matkind_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_matkind_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
