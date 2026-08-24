// RliRescale.cpp — implementation. Faithful port of io_ops/rli_rescale.py:
// the operational path (make_transform / grade_light / _classify_zone_textures
// / rescale_zone_in_bf) plus the GUI preview trio (sample_zone /
// _palette_stats / preview_after), which the native Level-Blender tab uses.
#include "jade/RliRescale.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/Light.hpp"
#include "jade/Material.hpp"
#include "jade/MeshSwap.hpp"   // splice_sub_entry
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

namespace jade {

RliColorFn make_transform(double brightness, std::array<int, 3> tint, double contrast) {
    int mx = std::max(tint[0], std::max(tint[1], tint[2]));
    if (mx == 0) mx = 1;                                   // Python `max(tint) or 1`
    std::array<double, 3> gain{brightness * (tint[0] / double(mx)),
                               brightness * (tint[1] / double(mx)),
                               brightness * (tint[2] / double(mx))};
    return [gain, contrast](int r, int g, int b) -> std::array<int, 3> {
        int in[3] = {r, g, b};
        std::array<int, 3> out{};
        for (int i = 0; i < 3; ++i) {
            double x = in[i] / 255.0;
            if (contrast != 1.0) x = std::pow(x, contrast);
            x *= gain[static_cast<size_t>(i)];
            int v = static_cast<int>(x * 255.0 + 0.5);      // int() truncation
            out[static_cast<size_t>(i)] = v < 0 ? 0 : (v > 255 ? 255 : v);
        }
        return out;
    };
}

std::vector<uint8_t> grade_light(const uint8_t* d, size_t n, const RliColorFn& fn) {
    LightInfo li = parse_light(d, n);
    if (!li.ok) return {};
    LightEdit e;
    bool any = false;
    // Python gates on tuple truthiness; every present RGB tuple, including
    // (0,0,0), is non-empty and therefore truthy.
    if (li.diffuse.present) {
        int r = int(li.diffuse.bits & 0xFF), g = int((li.diffuse.bits >> 8) & 0xFF),
            b = int((li.diffuse.bits >> 16) & 0xFF);
        e.diffuse = fn(r, g, b);
        any = true;
    }
    if (li.specular.present) {
        int r = int(li.specular.bits & 0xFF), g = int((li.specular.bits >> 8) & 0xFF),
            b = int((li.specular.bits >> 16) & 0xFF);
        e.specular = fn(r, g, b);
        any = true;
    }
    if (!any) return {};
    std::vector<uint8_t> nw = write_light_fields(d, n, e);
    if (nw.size() == n && std::equal(nw.begin(), nw.end(), d)) return {};
    return nw;
}

namespace {

constexpr uint32_t GAO_ID_VISUAL = 0x00004000;
// Bone | Anim | AddMatrix | Hierarchy | Events | FlashMatrix | ODE
constexpr uint32_t DYNAMIC_GAO_MASK = 0x00000001 | 0x00000002 | 0x00200000 |
                                      0x00400000 | 0x02000000 | 0x04000000 |
                                      0x10000000;

// _geo_nb_map: gro_type-1 subs that pass is_geometry_entry and parse.
std::unordered_map<uint32_t, uint32_t> geo_nb_map(const std::vector<SubEntry>& subs) {
    std::unordered_map<uint32_t, uint32_t> out;
    const uint32_t one = 1;
    for (const SubEntry& s : subs) {
        if (s.gro_null || s.gro_type != 1) continue;
        if (!is_geometry_entry(s.data.data(), s.data.size(), &one)) continue;
        GeoInfo g = parse_geometry(s.data.data(), s.data.size());
        if (g.ok) out[s.key] = g.nb_points;
    }
    return out;
}

// level_blender._element_texture_key: matId -> texture via the material chain.
// {found, key}; found=false is the Python None.
std::pair<bool, uint32_t> element_texture_key(
    const GaoInfo& info, uint32_t mat_id,
    const std::unordered_map<uint32_t, const SubEntry*>& by) {
    if (!info.vis_read) return {false, 0};
    auto mit = by.find(info.grm_key);
    if (mit == by.end()) return {false, 0};
    const SubEntry* m = mit->second;
    if (!m->gro_null && m->gro_type == 4) {                // multi-material
        MatInfo mm = parse_material(m->data.data(), m->data.size(), GRO_TYPE_MAT_MULTI);
        const std::vector<uint32_t>& keys = mm.sub_material_keys;
        if (mat_id < keys.size()) {
            auto sit = by.find(keys[mat_id]);
            if (sit != by.end()) {
                uint32_t k = resolve_texture_key(sit->second->data.data(),
                                                 sit->second->data.size());
                return {k != 0, k};
            }
        }
        auto dit = by.find(mat_id);                        // matId as direct key
        if (dit != by.end() && !dit->second->gro_null &&
            (dit->second->gro_type == 5 || dit->second->gro_type == 3 ||
             dit->second->gro_type == 4)) {
            uint32_t k = resolve_texture_key(dit->second->data.data(),
                                             dit->second->data.size());
            return {k != 0, k};
        }
        return {false, 0};
    }
    uint32_t k = resolve_texture_key(m->data.data(), m->data.size());
    return {k != 0, k};
}

// _gao_texture_keys: every texture the GAO's GEO elements reference.
std::set<uint32_t> gao_texture_keys(
    const GaoInfo& info, const GeoInfo& geo,
    const std::unordered_map<uint32_t, const SubEntry*>& by) {
    std::set<uint32_t> mat_ids;
    size_t nb_elems = geo.elements.size() / 2;
    for (size_t e = 0; e < nb_elems; ++e) mat_ids.insert(geo.elements[e * 2 + 1]);
    if (mat_ids.empty()) mat_ids.insert(0);                // Python `or {0}`
    std::set<uint32_t> keys;
    for (uint32_t mid : mat_ids) {
        auto [found, k] = element_texture_key(info, mid, by);
        if (found) keys.insert(k);
    }
    return keys;
}

// _is_unlit_static_decor: static architecture (grade its texture) vs a GFX /
// particle emitter (leave the additive effect alone).
bool is_unlit_static_decor(const GaoInfo& info, const GeoInfo& geo,
                           const std::vector<uint8_t>& geo_raw) {
    if (geo.flags1 & 0x1000) return false;                 // GFX/sprite profile
    if (info.identity & DYNAMIC_GAO_MASK) return false;    // dynamic object
    if (!cooked_vb_section(geo_raw.data(), geo_raw.size()).ok) return false;
    return true;
}

// _classify_zone_textures: (used-by-lit, used-by-unlit-static) texture keys.
void classify_zone_textures(const std::vector<SubEntry>& subs,
                            std::set<uint32_t>& lit, std::set<uint32_t>& unlit) {
    std::unordered_map<uint32_t, const SubEntry*> by;
    for (const SubEntry& s : subs) by[s.key] = &s;
    std::unordered_map<uint32_t, uint32_t> geo_nb = geo_nb_map(subs);

    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo info = parse_gao_full(s.data.data(), s.data.size());
        if (!info.ok || !info.vis_read) continue;
        uint32_t gk = info.gro_key;
        auto git = by.find(gk);
        if (git == by.end() || git->second->data.empty()) continue;
        const std::vector<uint8_t>& ged = git->second->data;
        GeoInfo geo = parse_geometry(ged.data(), ged.size());
        if (!geo.ok || geo.skin_present) continue;         // skinned = dynamic-lit
        auto nit = geo_nb.find(gk);
        bool prim_ok = false;
        PrimaryColors pc;
        if (nit != geo_nb.end() && nit->second != 0) {
            pc = read_primary_colors(s.data.data(), s.data.size(), nit->second);
            prim_ok = pc.ok;
        }
        std::set<uint32_t> keys = gao_texture_keys(info, geo, by);
        if (!prim_ok) {                                    // no RLI table
            if (is_unlit_static_decor(info, geo, ged))
                unlit.insert(keys.begin(), keys.end());
            continue;                                      // else GFX/FX: leave
        }
        bool any_lit = false;
        for (const RgbaColor& c : pc.colors)
            if (c.r || c.g || c.b) { any_lit = true; break; }
        if (any_lit) lit.insert(keys.begin(), keys.end());
        else unlit.insert(keys.begin(), keys.end());       // all-zero = unlit bg
    }
}

}  // namespace

void classify_zone_textures_pub(const std::vector<SubEntry>& subs,
                                std::set<uint32_t>& lit,
                                std::set<uint32_t>& unlit_static) {
    classify_zone_textures(subs, lit, unlit_static);
}

RescaleDecResult rescale_dec(const std::vector<uint8_t>& dec,
                             const RliColorFn& fn, bool grade_lights_flag,
                             const std::set<uint32_t>& target_tex) {
    RescaleDecResult out;
    ColorFn tex_fn = [&fn](std::array<int, 3> c) { return fn(c[0], c[1], c[2]); };
    std::vector<SubEntry> subs = parse_sub_entries(dec);

    struct Splice { size_t off; uint32_t size; std::vector<uint8_t> payload; };
    std::vector<Splice> spliced;

    std::unordered_map<uint32_t, uint32_t> geo_nb = geo_nb_map(subs);
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo info = parse_gao_full(s.data.data(), s.data.size());
        if (!info.ok || !info.vis_read) continue;
        auto nit = geo_nb.find(info.gro_key);
        if (nit == geo_nb.end() || nit->second == 0) continue;
        uint32_t nb = nit->second;
        if (!has_rli(s.data.data(), s.data.size(), nb)) continue;
        std::vector<uint8_t> nw = transform_rli(s.data.data(), s.data.size(), nb, fn);
        if (!nw.empty() && nw.size() == s.data.size()) {
            spliced.push_back({s.offset, s.size, std::move(nw)});
            ++out.gaos;
        }
    }
    if (grade_lights_flag) {
        for (const SubEntry& s : subs) {
            if (s.gro_null || s.gro_type != 2) continue;
            if (!is_light_payload(s.data.data(), s.data.size())) continue;
            std::vector<uint8_t> nw = grade_light(s.data.data(), s.data.size(), fn);
            if (!nw.empty() && nw.size() == s.data.size()) {
                spliced.push_back({s.offset, s.size, std::move(nw)});
                ++out.lights;
            }
        }
    }
    if (!target_tex.empty()) {
        for (const SubEntry& s : subs) {
            if (target_tex.find(s.key) == target_tex.end()) continue;
            if (!is_texture_entry(s.data.data(), s.data.size())) continue;
            std::vector<uint8_t> nw = grade_texture(s.data.data(), s.data.size(), tex_fn);
            if (!nw.empty() && nw.size() == s.data.size()) {
                spliced.push_back({s.offset, s.size, std::move(nw)});
                ++out.textures;
            }
        }
    }
    if (spliced.empty()) return out;
    std::vector<uint8_t> new_dec = dec;
    for (const Splice& sp : spliced)                    // same-size: offsets stable
        new_dec = splice_sub_entry(new_dec, sp.off, sp.size, sp.payload);
    if (new_dec.size() != dec.size()) return out;
    out.changed = true;
    out.dec = std::move(new_dec);
    return out;
}

// ── before/after preview sampling (rli_rescale.py's GUI trio) ──────────────

namespace {

double luma(const std::array<int, 3>& c) {
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
}

// Python round(): nearest integer with ties to even. std::lround uses ties
// away from zero, which differs for preview averages ending in x.5.
int python_round(double v) {
    const double lo = std::floor(v);
    const double frac = v - lo;
    if (frac < 0.5) return static_cast<int>(lo);
    if (frac > 0.5) return static_cast<int>(lo + 1.0);
    const long long i = static_cast<long long>(lo);
    return static_cast<int>((i & 1LL) ? i + 1LL : i);
}

// _palette_stats.
PaletteStats palette_stats(std::vector<std::array<int, 3>>& colors) {
    PaletteStats st;
    if (colors.empty()) return st;
    const size_t n = colors.size();
    long long sr = 0, sg = 0, sb = 0;
    for (const auto& c : colors) { sr += c[0]; sg += c[1]; sb += c[2]; }
    st.average = {double(sr) / double(n), double(sg) / double(n),
                  double(sb) / double(n)};
    std::stable_sort(colors.begin(), colors.end(),
                     [](const std::array<int, 3>& a,
                        const std::array<int, 3>& b) {
                         return luma(a) < luma(b);
                     });
    const double picks[5] = {0.0, 0.25, 0.5, 0.75, 0.99};
    for (double p : picks)
        st.percentiles.push_back(
            colors[std::min(n - 1, size_t(p * double(n)))]);
    st.ok = true;
    st.count = n;
    st.min = colors.front();
    st.max = colors.back();
    return st;
}

}  // namespace

PaletteStats sample_zone(const std::vector<SubEntry>& subs,
                         size_t max_colors) {
    std::vector<std::array<int, 3>> colors;
    std::unordered_map<uint32_t, uint32_t> geo_nb = geo_nb_map(subs);
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo info = parse_gao_full(s.data.data(), s.data.size());
        if (!info.ok || !info.vis_read) continue;
        auto nit = geo_nb.find(info.gro_key);
        if (nit == geo_nb.end() || nit->second == 0) continue;
        const uint32_t nb = nit->second;
        if (!has_rli(s.data.data(), s.data.size(), nb)) continue;
        PrimaryColors prim =
            read_primary_colors(s.data.data(), s.data.size(), nb);
        if (!prim.ok || prim.colors.empty()) continue;
        bool any = false;
        for (const RgbaColor& c : prim.colors)
            if (c.r || c.g || c.b) { any = true; break; }
        if (!any) continue;                    // entirely unlit → skip
        for (const RgbaColor& c : prim.colors)
            colors.push_back({int(c.r), int(c.g), int(c.b)});
        if (colors.size() >= max_colors) break;
    }
    return palette_stats(colors);
}

PaletteStats preview_after(const PaletteStats& stats, const RliColorFn& fn) {
    PaletteStats out;
    if (!stats.ok) return out;
    out.ok = true;
    out.count = stats.count;
    const std::array<int, 3> avg_in = {
        python_round(stats.average[0]), python_round(stats.average[1]),
        python_round(stats.average[2])};
    const auto avg = fn(avg_in[0], avg_in[1], avg_in[2]);
    out.average = {double(avg[0]), double(avg[1]), double(avg[2])};
    for (const auto& c : stats.percentiles) {
        const auto g = fn(c[0], c[1], c[2]);
        out.percentiles.push_back({g[0], g[1], g[2]});
    }
    const auto mn = fn(stats.min[0], stats.min[1], stats.min[2]);
    const auto mx = fn(stats.max[0], stats.max[1], stats.max[2]);
    out.min = {mn[0], mn[1], mn[2]};
    out.max = {mx[0], mx[1], mx[2]};
    return out;
}

RescaleStats rescale_zone_in_bf(const std::string& bf_path, double brightness,
                                std::array<int, 3> tint, double contrast,
                                const std::string& district_filter,
                                bool grade_lights_flag, bool grade_textures_flag,
                                RescaleLogFn log) {
    RescaleStats res;
    RliColorFn fn = make_transform(brightness, tint, contrast);

    BigFile bf;
    try { bf.open(bf_path); } catch (const std::exception& e) {
        res.error = e.what();
        return res;
    }

    std::vector<uint32_t> district;
    for (const auto& kv : bf.files) {
        if (district_filter.empty() ||
            kv.second.name.find(district_filter) != std::string::npos)
            district.push_back(kv.first);
    }

    // Pass 1 (textures only): which textures belong to UNLIT surfaces zone-wide.
    std::set<uint32_t> target_tex;
    if (grade_textures_flag) {
        std::set<uint32_t> lit_t, unlit_t;
        for (uint32_t idx : district) {
            LzoResult dr = decompress_lzo(bf.read_data(idx));
            if (!dr.ok) continue;
            classify_zone_textures(parse_sub_entries(dr.data), lit_t, unlit_t);
        }
        for (uint32_t k : unlit_t)
            if (lit_t.find(k) == lit_t.end()) target_tex.insert(k);
        if (log)
            log("unlit textures to shade: " + std::to_string(target_tex.size()));
    }

    std::fstream f(bf_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) { res.error = "cannot open archive for writing"; return res; }

    for (uint32_t idx : district) {
        BFFile& fi = bf.files.at(idx);
        LzoResult dr = decompress_lzo(bf.read_data(idx));
        if (!dr.ok) continue;
        RescaleDecResult rd = rescale_dec(dr.data, fn, grade_lights_flag,
                                          grade_textures_flag ? target_tex
                                                              : std::set<uint32_t>{});
        if (!rd.changed) continue;
        std::vector<uint8_t> compressed = compress_lzo(rd.dec, 9);
        bf.write_entry(f, compressed, fi);
        res.bins += 1;
        res.gaos += rd.gaos;
        res.lights += rd.lights;
        res.textures += rd.textures;
        if (log) {
            std::string line = "  " + fi.name + ": " +
                               std::to_string(rd.gaos) + " RLI";
            if (grade_lights_flag)
                line += ", " + std::to_string(rd.lights) + " lights";
            if (grade_textures_flag)
                line += ", " + std::to_string(rd.textures) + " textures";
            log(line);
        }
    }
    bf.flush_size_grs(f);
    if (log)
        log("graded " + std::to_string(res.gaos) + " RLI, " +
            std::to_string(res.lights) + " lights, " +
            std::to_string(res.textures) + " textures across " +
            std::to_string(res.bins) + " bins");
    res.ok = true;
    return res;
}

}  // namespace jade
