// LevelBlender.cpp — standard (non-lightmap) level_blender.py port.
#include "jade/LevelBlender.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/Json.hpp"
#include "jade/Light.hpp"
#include "jade/Material.hpp"
#include "jade/MeshSwap.hpp"     // splice_sub_entry
#include "jade/Rli.hpp"
#include "jade/Texture.hpp"

namespace jade {
namespace levelblend {

namespace fs = std::filesystem;

namespace {

// _xform_world_yup: GAO matrix (col-major floats) applied to a Jade point,
// then Z-up -> Y-up.
struct V3 { double x, y, z; };
V3 xform_world_yup(const double m[16], double x, double y, double z) {
    double wx = x * m[0] + y * m[4] + z * m[8] + m[12];
    double wy = x * m[1] + y * m[5] + z * m[9] + m[13];
    double wz = x * m[2] + y * m[6] + z * m[10] + m[14];
    return {wx, wz, -wy};
}

// CPython round(x, 2): decimal-correct nearest via %.2f + strtod (the same
// trick mesh_swap's _qpos uses for round(x, 3)).
double round2(double x) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", x);
    return std::strtod(buf, nullptr);
}

struct QPos {
    double a, b, c;
    bool operator==(const QPos& o) const { return a == o.a && b == o.b && c == o.c; }
};
struct QPosHash {
    static double norm(double v) { return v == 0.0 ? 0.0 : v; }   // unify ±0.0
    size_t operator()(const QPos& q) const {
        auto h = [](double v) {
            uint64_t bits;
            std::memcpy(&bits, &v, 8);
            return std::hash<uint64_t>{}(bits);
        };
        size_t s = h(norm(q.a));
        s = s * 1000003u ^ h(norm(q.b));
        s = s * 1000003u ^ h(norm(q.c));
        return s;
    }
};
QPos qpos(const V3& p) { return {round2(p.x), round2(p.y), round2(p.z)}; }

struct Rgb { int r, g, b; };

double linear_to_srgb(double c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

// _to255: clamp, optional sRGB-encode, int(round(x*255)) (half-even).
Rgb to255(const std::vector<double>& c, bool srgb) {
    int out[3];
    for (int i = 0; i < 3; ++i) {
        double x = i < int(c.size()) ? c[size_t(i)] : 0.0;
        x = std::max(0.0, std::min(1.0, x));
        if (srgb) x = linear_to_srgb(x);
        int v = int(std::nearbyint(x * 255.0));
        out[i] = std::max(0, std::min(255, v));
    }
    return {out[0], out[1], out[2]};
}

std::string strip_blender_suffix(const std::string& name) {
    if (name.size() > 4 && name[name.size() - 4] == '.' &&
        std::isdigit(static_cast<unsigned char>(name[name.size() - 3])) &&
        std::isdigit(static_cast<unsigned char>(name[name.size() - 2])) &&
        std::isdigit(static_cast<unsigned char>(name[name.size() - 1])))
        return name.substr(0, name.size() - 4);
    return name;
}

using PosMap = std::unordered_map<QPos, Rgb, QPosHash>;

// _nearest_color: the 26 neighbouring 0.01-buckets.
const Rgb* nearest_color(const PosMap& pos_map, const V3& wp) {
    QPos q = qpos(wp);
    static const double kD[3] = {0.0, 0.01, -0.01};
    for (double dx : kD)
        for (double dy : kD)
            for (double dz : kD) {
                QPos k{round2(q.a + dx), round2(q.b + dy), round2(q.c + dz)};
                auto it = pos_map.find(k);
                if (it != pos_map.end()) return &it->second;
            }
    return nullptr;
}

// _read_glb.
bool read_glb(const std::string& path, json::Value& gltf, std::vector<uint8_t>& blob,
              std::string& err) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    if (all.size() < 12 || std::memcmp(all.data(), "glTF", 4) != 0) {
        err = "not a GLB";
        return false;
    }
    auto u32 = [&](size_t o) {
        return uint32_t(all[o]) | (uint32_t(all[o + 1]) << 8) |
               (uint32_t(all[o + 2]) << 16) | (uint32_t(all[o + 3]) << 24);
    };
    uint32_t total = u32(8);
    size_t off = 12;
    bool have_json = false;
    while (off + 8 <= std::min<size_t>(total, all.size())) {
        uint32_t clen = u32(off);
        const uint8_t* ctype = all.data() + off + 4;
        size_t cdata = off + 8;
        if (cdata + clen > all.size()) break;
        if (std::memcmp(ctype, "JSON", 4) == 0) {
            try {
                gltf = json::parse(std::string(all.begin() + long(cdata),
                                               all.begin() + long(cdata + clen)));
                have_json = true;
            } catch (const std::exception& e) {
                err = e.what();
                return false;
            }
        } else if (std::memcmp(ctype, "BIN\0", 4) == 0) {
            blob.assign(all.begin() + long(cdata), all.begin() + long(cdata + clen));
        }
        off = cdata + clen;
    }
    if (!have_json) { err = "no JSON chunk"; return false; }
    return true;
}

int compn_of(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    return 0;
}

// _read_accessor: floats, or normalized u8/u16 (default normalized=True).
std::vector<std::vector<double>> read_accessor(const json::Value& gltf,
                                               const std::vector<uint8_t>& blob,
                                               int acc_i) {
    std::vector<std::vector<double>> out;
    const json::Value* accs = gltf.find("accessors");
    const json::Value* bvs = gltf.find("bufferViews");
    if (!accs || !bvs || acc_i < 0 || size_t(acc_i) >= accs->arr.size()) return out;
    const json::Value& acc = accs->arr[size_t(acc_i)];
    const json::Value* bvi = acc.find("bufferView");
    if (!bvi || size_t(bvi->num) >= bvs->arr.size()) return out;
    const json::Value& bv = bvs->arr[size_t(bvi->num)];
    auto num_of = [](const json::Value* v, double def) {
        return v && v->is_num() ? v->num : def;
    };
    size_t base = size_t(num_of(bv.find("byteOffset"), 0)) +
                  size_t(num_of(acc.find("byteOffset"), 0));
    size_t cnt = size_t(num_of(acc.find("count"), 0));
    const json::Value* tv = acc.find("type");
    int n = compn_of(tv && tv->is_str() ? tv->str : "");
    if (n == 0) return out;
    int ct = int(num_of(acc.find("componentType"), 0));
    size_t sz;
    double div;
    bool norm;
    if (ct == 5126) { sz = 4; div = 1.0; norm = false; }
    else if (ct == 5121) { sz = 1; div = 255.0; norm = true; }
    else if (ct == 5123) { sz = 2; div = 65535.0; norm = true; }
    else return out;
    if (ct != 5126) {
        const json::Value* nz = acc.find("normalized");
        if (nz && !nz->b) norm = false;   // explicit normalized:false
    }
    size_t stride = size_t(num_of(bv.find("byteStride"), 0));
    if (stride == 0) stride = size_t(n) * sz;
    out.reserve(cnt);
    for (size_t i = 0; i < cnt; ++i) {
        std::vector<double> v;
        v.reserve(size_t(n));
        size_t o = base + i * stride;
        if (o + size_t(n) * sz > blob.size()) break;
        for (int k = 0; k < n; ++k) {
            size_t ko = o + size_t(k) * sz;
            double x;
            if (ct == 5126) {
                float fv;
                std::memcpy(&fv, blob.data() + ko, 4);
                x = double(fv);
            } else if (ct == 5121) {
                x = double(blob[ko]);
            } else {
                x = double(uint32_t(blob[ko]) | (uint32_t(blob[ko + 1]) << 8));
            }
            v.push_back(norm ? x / div : x);
        }
        out.push_back(std::move(v));
    }
    return out;
}

// _pick_baked_color_attr.
std::string pick_baked_color_attr(const json::Value& prims, const json::Value& gltf,
                                  const std::vector<uint8_t>& blob,
                                  const LevelBlendLogFn& log) {
    std::vector<std::pair<int, std::string>> names;   // (set index, name)
    for (const json::Value& p : prims.arr) {
        const json::Value* attrs = p.find("attributes");
        if (!attrs) continue;
        for (const auto& kv : attrs->obj) {
            if (kv.first.rfind("COLOR_", 0) != 0) continue;
            std::string tail = kv.first.substr(6);
            bool digit = !tail.empty() &&
                         std::all_of(tail.begin(), tail.end(), [](char c) {
                             return std::isdigit(static_cast<unsigned char>(c));
                         });
            int idx = digit ? std::atoi(tail.c_str()) : 0;
            bool present = false;
            for (const auto& e : names)
                if (e.second == kv.first) { present = true; break; }
            if (!present) names.push_back({idx, kv.first});
        }
    }
    if (names.empty()) return "";
    std::stable_sort(names.begin(), names.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    auto sample = [&](const std::string& nm) {
        for (const json::Value& p : prims.arr) {
            const json::Value* attrs = p.find("attributes");
            const json::Value* a = attrs ? attrs->find(nm) : nullptr;
            if (a && a->is_num()) return read_accessor(gltf, blob, int(a->num));
        }
        return std::vector<std::vector<double>>{};
    };
    auto uniform_white = [](const std::vector<std::vector<double>>& vals) {
        if (vals.empty()) return false;
        size_t lim = std::min<size_t>(vals.size(), 256);
        for (size_t i = 0; i < lim; ++i) {
            const auto& v = vals[i];
            if (v.size() < 3 || !(v[0] > 0.996 && v[1] > 0.996 && v[2] > 0.996))
                return false;
        }
        return true;
    };

    std::string chosen = names.front().second;
    if (uniform_white(sample(chosen))) {
        for (size_t i = 1; i < names.size(); ++i)
            if (!uniform_white(sample(names[i].second))) {
                if (log)
                    log("  bake reimport: " + chosen +
                        " is uniform white; using " + names[i].second +
                        " instead");
                chosen = names[i].second;
                break;
            }
    }
    if (names.size() > 1 && log) {
        std::string repr = "[";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) repr += ", ";
            repr += "'" + names[i].second + "'";
        }
        repr += "]";
        log("  bake reimport: NOTE this mesh has " +
            std::to_string(names.size()) + " colour sets " + repr +
            "; using " + chosen +
            ". If your bake landed in a different set, make it the active "
            "render colour in Blender so it exports as COLOR_0.");
    }
    return chosen;
}

// _baked_color_map: (pos_map, exact-or-empty, has_exact).
void baked_color_map(const json::Value& gltf, const std::vector<uint8_t>& blob,
                     int mesh_idx, bool srgb, PosMap& pos_map,
                     std::vector<Rgb>& exact, bool& has_exact,
                     const LevelBlendLogFn& log) {
    has_exact = false;
    const json::Value* meshes = gltf.find("meshes");
    if (!meshes || mesh_idx < 0 || size_t(mesh_idx) >= meshes->arr.size()) return;
    const json::Value* prims = meshes->arr[size_t(mesh_idx)].find("primitives");
    if (!prims || !prims->is_arr()) return;
    std::string cname = pick_baked_color_attr(*prims, gltf, blob, log);
    if (cname.empty()) return;

    std::vector<int> color_accs;
    for (const json::Value& p : prims->arr) {
        const json::Value* attrs = p.find("attributes");
        const json::Value* a = attrs ? attrs->find(cname) : nullptr;
        if (a && a->is_num()) {
            int ai = int(a->num);
            if (std::find(color_accs.begin(), color_accs.end(), ai) == color_accs.end())
                color_accs.push_back(ai);
        }
    }
    for (const json::Value& p : prims->arr) {
        const json::Value* attrs = p.find("attributes");
        const json::Value* pa = attrs ? attrs->find("POSITION") : nullptr;
        const json::Value* ca = attrs ? attrs->find(cname) : nullptr;
        if (!pa || !ca || !pa->is_num() || !ca->is_num()) continue;
        auto positions = read_accessor(gltf, blob, int(pa->num));
        auto colors = read_accessor(gltf, blob, int(ca->num));
        size_t lim = std::min(positions.size(), colors.size());
        for (size_t i = 0; i < lim; ++i) {
            const auto& p3 = positions[i];
            if (p3.size() < 3) continue;
            pos_map[QPos{round2(p3[0]), round2(p3[1]), round2(p3[2])}] =
                to255(colors[i], srgb);
        }
    }
    if (color_accs.size() == 1) {
        auto vals = read_accessor(gltf, blob, color_accs.front());
        exact.reserve(vals.size());
        for (const auto& c : vals) exact.push_back(to255(c, srgb));
        has_exact = true;
    }
}

// _reimport_context lookups.
struct Ctx {
    std::unordered_map<uint32_t, const SubEntry*> by;
    std::unordered_map<uint32_t, uint32_t> geo_nb;
    std::vector<std::pair<std::string, uint32_t>> name_to_key;   // setdefault
    const uint32_t* name_lookup(const std::string& n) const {
        for (const auto& kv : name_to_key)
            if (kv.first == n) return &kv.second;
        return nullptr;
    }
};
Ctx reimport_context(const std::vector<SubEntry>& subs) {
    Ctx ctx;
    for (const SubEntry& s : subs) ctx.by[s.key] = &s;   // last wins
    for (const SubEntry& s : subs) {
        if (s.gro_null || s.gro_type != 1) continue;
        uint32_t gt = 1;
        if (!is_geometry_entry(s.data.data(), s.data.size(), &gt)) continue;
        GeoInfo gi = parse_geometry(s.data.data(), s.data.size());
        ctx.geo_nb[s.key] = gi.nb_points;
    }
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
        if (!gi.ok) continue;
        std::string nm = gi.name;
        nm.erase(std::remove(nm.begin(), nm.end(), '\0'), nm.end());
        if (nm.empty()) continue;
        if (ctx.name_lookup(nm) == nullptr) ctx.name_to_key.push_back({nm, s.key});
    }
    return ctx;
}

// _bake_rli_for_gao.
std::vector<uint8_t> bake_rli_for_gao(uint32_t gao_key, const Ctx& ctx,
                                      const PosMap& pos_map,
                                      const std::vector<Rgb>& exact, bool has_exact,
                                      bool refresh_primary, uint32_t& misses) {
    auto sit = ctx.by.find(gao_key);
    if (sit == ctx.by.end()) return {};
    const SubEntry& s = *sit->second;
    GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
    if (!gi.ok || !gi.vis_read) return {};
    auto nbit = ctx.geo_nb.find(gi.gro_key);
    if (nbit == ctx.geo_nb.end()) return {};
    uint32_t nb = nbit->second;
    const SubEntry& geo_sub = *ctx.by.at(gi.gro_key);

    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (gi.gmat_present && gi.gmat_raw.size() >= 64)
        for (int i = 0; i < 16; ++i) {
            float fv;
            std::memcpy(&fv, gi.gmat_raw.data() + size_t(i) * 4, 4);
            m[i] = double(fv);
        }

    CookedMesh cm = cooked_mesh(geo_sub.data.data(), geo_sub.data.size());
    ExtraColors ec = read_extra_colors(s.data.data(), s.data.size(), nb);
    bool cooked = cm.ok && ec.ok;

    std::vector<V3> canon;
    uint32_t target_n;
    GeoInfo geo;
    if (cooked) {
        canon.reserve(cm.n);
        for (uint32_t i = 0; i < cm.n; ++i)
            canon.push_back(xform_world_yup(m, double(cm.positions[i * 3]),
                                            double(cm.positions[i * 3 + 1]),
                                            double(cm.positions[i * 3 + 2])));
        target_n = ec.count_exp;
    } else {
        geo = parse_geometry(geo_sub.data.data(), geo_sub.data.size());
        if (geo.skin_present) return {};       // dynamic-lit; never cook RLI in
        if (geo.vertices.empty()) return {};
        canon.reserve(geo.vertices.size() / 3);
        for (size_t i = 0; i + 2 < geo.vertices.size(); i += 3)
            canon.push_back(xform_world_yup(m, double(geo.vertices[i]),
                                            double(geo.vertices[i + 1]),
                                            double(geo.vertices[i + 2])));
        target_n = nb;
    }

    std::vector<Rgb> colors;
    if (has_exact && exact.size() == target_n) {
        colors = exact;
    } else {
        Rgb last{128, 128, 128};
        size_t lim = std::min<size_t>(canon.size(), target_n);
        colors.reserve(lim);
        for (size_t i = 0; i < lim; ++i) {
            const Rgb* c = nullptr;
            auto it = pos_map.find(qpos(canon[i]));
            if (it != pos_map.end()) c = &it->second;
            else c = nearest_color(pos_map, canon[i]);
            if (c == nullptr) {
                ++misses;
                colors.push_back(last);
            } else {
                last = *c;
                colors.push_back(*c);
            }
        }
    }
    if (colors.size() < target_n) return {};

    std::vector<RgbF> cf;
    cf.reserve(colors.size());
    for (const Rgb& c : colors) cf.push_back({double(c.r), double(c.g), double(c.b)});
    if (cooked)
        return write_extra_colors(s.data.data(), s.data.size(), nb, cf,
                                  refresh_primary ? geo_sub.data.data() : nullptr,
                                  refresh_primary ? geo_sub.data.size() : 0);
    return write_rli(s.data.data(), s.data.size(), nb, cf,
                     geo_sub.data.data(), geo_sub.data.size());
}

bool has_json_suffix(const std::string& path) {
    if (path.size() < 5) return false;
    std::string tail = path.substr(path.size() - 5);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    return tail == ".json";
}

void set_update(BakedImport& res, uint32_t key, std::vector<uint8_t> payload) {
    for (auto& kv : res.updates) {
        if (kv.first == key) {
            kv.second = std::move(payload);
            return;
        }
    }
    res.updates.push_back({key, std::move(payload)});
}

BakedImport import_baked_rli_dump(const std::string& json_path,
                                  const std::vector<SubEntry>& subs,
                                  bool refresh_primary, bool srgb_lighting,
                                  const LevelBlendLogFn& log) {
    BakedImport res;
    std::ifstream f(fs::u8path(json_path));
    if (!f) {
        res.error = "cannot open " + json_path;
        return res;
    }
    std::string raw((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    json::Value data;
    try {
        data = json::parse_strict(raw);
    } catch (const std::exception& e) {
        res.error = e.what();
        return res;
    }
    Ctx ctx = reimport_context(subs);
    const json::Value* objects = data.find("objects");
    if (objects && objects->is_arr()) {
        for (const json::Value& obj : objects->arr) {
            uint32_t gao_key = 0;
            bool have_key = false;
            const json::Value* key = obj.find("key");
            if (key && key->is_str() && !key->str.empty()) {
                char* end = nullptr;
                unsigned long value = std::strtoul(key->str.c_str(), &end, 16);
                if (end != key->str.c_str() && end && *end == '\0') {
                    gao_key = uint32_t(value);
                    have_key = true;
                }
            }
            if (!have_key) {
                const json::Value* name = obj.find("name");
                const uint32_t* found = ctx.name_lookup(strip_blender_suffix(
                    name && name->is_str() ? name->str : ""));
                if (found) {
                    gao_key = *found;
                    have_key = true;
                }
            }
            if (!have_key) continue;

            PosMap pos_map;
            const json::Value* positions = obj.find("p");
            const json::Value* colors = obj.find("c");
            if (positions && positions->is_arr() && colors && colors->is_arr()) {
                const size_t n = std::min(positions->arr.size(), colors->arr.size());
                for (size_t i = 0; i < n; ++i) {
                    const json::Value& p = positions->arr[i];
                    const json::Value& c = colors->arr[i];
                    if (!p.is_arr() || p.arr.size() < 3 ||
                        !p.arr[0].is_num() || !p.arr[1].is_num() ||
                        !p.arr[2].is_num() || !c.is_arr())
                        continue;
                    std::vector<double> cv;
                    for (size_t j = 0; j < std::min<size_t>(3, c.arr.size()); ++j)
                        if (c.arr[j].is_num()) cv.push_back(c.arr[j].num);
                    pos_map[QPos{round2(p.arr[0].num), round2(p.arr[1].num),
                                round2(p.arr[2].num)}] =
                        to255(cv, srgb_lighting);
                }
            }
            std::vector<uint8_t> updated = bake_rli_for_gao(
                gao_key, ctx, pos_map, {}, false, refresh_primary, res.misses);
            if (!updated.empty()) set_update(res, gao_key, std::move(updated));
        }
    }
    res.ok = true;
    if (log) {
        std::string line = "baked RLI updated " +
            std::to_string(res.updates.size()) + " GAOs from " + json_path +
            " (dump)";
        if (res.misses)
            line += " (" + std::to_string(res.misses) +
                    " unmatched verts filled)";
        log(line);
    }
    return res;
}

}  // namespace

BakedImport import_baked_rli(const std::string& glb_path,
                             const std::vector<SubEntry>& subs,
                             bool refresh_primary, bool srgb_lighting,
                             LevelBlendLogFn log_fn) {
    if (has_json_suffix(glb_path))
        return import_baked_rli_dump(glb_path, subs, refresh_primary,
                                     srgb_lighting, log_fn);
    BakedImport res;
    json::Value gltf;
    std::vector<uint8_t> blob;
    std::string err;
    if (!read_glb(glb_path, gltf, blob, err)) {
        res.error = err;
        return res;
    }
    Ctx ctx = reimport_context(subs);

    const json::Value* nodes = gltf.find("nodes");
    if (nodes && nodes->is_arr()) {
        for (const json::Value& node : nodes->arr) {
            const json::Value* mesh = node.find("mesh");
            if (!mesh || !mesh->is_num()) continue;
            const json::Value* ex = node.find("extras");
            if (ex) {
                const json::Value* sk = ex->find("jade_skinned");
                if (sk && sk->b) continue;
            }
            uint32_t gao_key = 0;
            bool have_key = false;
            const json::Value* jg = ex ? ex->find("jade_gao") : nullptr;
            if (jg && jg->is_str()) {
                char* end = nullptr;
                unsigned long v = std::strtoul(jg->str.c_str(), &end, 16);
                if (end && *end == '\0' && end != jg->str.c_str()) {
                    gao_key = uint32_t(v);
                    have_key = true;
                }
            }
            if (!have_key) {
                const json::Value* nm = node.find("name");
                const uint32_t* k = ctx.name_lookup(
                    strip_blender_suffix(nm && nm->is_str() ? nm->str : ""));
                if (k != nullptr) { gao_key = *k; have_key = true; }
            }
            if (!have_key) continue;

            PosMap pos_map;
            std::vector<Rgb> exact;
            bool has_exact = false;
            baked_color_map(gltf, blob, int(mesh->num), srgb_lighting, pos_map,
                            exact, has_exact, log_fn);
            std::vector<uint8_t> nw = bake_rli_for_gao(
                gao_key, ctx, pos_map, exact, has_exact, refresh_primary,
                res.misses);
            if (!nw.empty()) {
                set_update(res, gao_key, std::move(nw));
            }
        }
    }
    res.ok = true;
    if (log_fn) {
        std::string line = "baked RLI updated " +
            std::to_string(res.updates.size()) + " GAOs from " + glb_path;
        if (res.misses)
            line += " (" + std::to_string(res.misses) +
                    " unmatched verts filled)";
        log_fn(line);
    }
    return res;
}

ApplyStats apply_baked_rli_to_bf(
    const std::string& bf_path,
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& updates,
    const std::string& district_filter, LevelBlendLogFn log_fn) {
    ApplyStats st;
    if (updates.empty()) { st.ok = true; return st; }
    std::unordered_map<uint32_t, const std::vector<uint8_t>*> up;
    for (const auto& kv : updates) up[kv.first] = &kv.second;

    BigFile bf;
    try { bf.open(bf_path); } catch (const std::exception& e) {
        st.error = e.what();
        return st;
    }
    std::fstream f(fs::u8path(bf_path),
                   std::ios::in | std::ios::out | std::ios::binary);
    if (!f) { st.error = "cannot open archive for writing"; return st; }

    for (auto& kv : bf.files) {
        BFFile& fi = kv.second;
        if (!district_filter.empty() &&
            fi.name.find(district_filter) == std::string::npos)
            continue;
        LzoResult dr = decompress_lzo(bf.read_data(fi.index));
        if (!dr.ok) continue;
        const std::vector<uint8_t>& dec = dr.data;
        std::vector<SubEntry> subs = parse_sub_entries(dec);
        struct Sp { size_t off; uint32_t size; const std::vector<uint8_t>* p; };
        std::vector<Sp> spliced;
        for (const SubEntry& s : subs) {
            auto it = up.find(s.key);
            if (it != up.end() && s.ext == ".gao" &&
                it->second->size() == s.data.size())
                spliced.push_back({s.offset, s.size, it->second});
        }
        if (spliced.empty()) continue;
        std::vector<uint8_t> new_dec = dec;
        for (const Sp& sp : spliced)
            new_dec = splice_sub_entry(new_dec, sp.off, sp.size, *sp.p);
        if (new_dec.size() != dec.size()) continue;
        std::vector<uint8_t> compressed = compress_lzo(new_dec, 9);
        bf.write_entry(f, compressed, fi);
        st.bins += 1;
        st.gaos += uint32_t(spliced.size());
    }
    bf.flush_size_grs(f);
    st.ok = true;
    if (log_fn)
        log_fn("applied baked RLI to " + std::to_string(st.gaos) +
               " GAO copies across " + std::to_string(st.bins) + " bins");
    return st;
}

// ── export half (zone -> Blender GLB) ───────────────────────────────────────
namespace {

inline uint32_t get_u32(const uint8_t* d, size_t o) {
    return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
           (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}

// zlib-stream PNG writer using STORED deflate blocks: pixel-identical to any
// other PNG of the same RGBA, container bytes differ from PIL's (the harness
// compares decoded pixels). Scanlines carry filter byte 0.
uint32_t adler32_of(const uint8_t* d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void png_chunk(std::vector<uint8_t>& out, const char type[4],
               const std::vector<uint8_t>& data) {
    put_be32(out, uint32_t(data.size()));
    size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t c = crc32(out.data() + crc_start, out.size() - crc_start);
    put_be32(out, c);
}

std::vector<uint8_t> png_encode_rgba(const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);   // filter: None
        raw.insert(raw.end(), rgba + size_t(y) * w * 4,
                   rgba + size_t(y + 1) * w * 4);
    }
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);      // zlib header, no dict
    size_t off = 0;
    while (off < raw.size() || raw.empty()) {
        size_t n = std::min<size_t>(raw.size() - off, 65535);
        bool final = off + n >= raw.size();
        z.push_back(final ? 1 : 0);                       // BFINAL + BTYPE=00
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(off), raw.begin() + long(off + n));
        off += n;
        if (raw.empty()) break;
    }
    put_be32(z, adler32_of(raw.data(), raw.size()));

    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, w);
    put_be32(ihdr, h);
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // colour type RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", z);
    png_chunk(out, "IEND", {});
    return out;
}

// Minimal insertion-ordered JSON text builder (json.dumps-compatible enough
// for the semantic harness: floats via shortest round-trip repr).
std::string jflt(double v) {
    for (int prec = 1; prec <= 17; ++prec) {
        char b[64];
        std::snprintf(b, sizeof b, "%.*g", prec, v);
        if (std::strtod(b, nullptr) == v) {
            std::string s(b);
            if (s.find('.') == std::string::npos &&
                s.find('e') == std::string::npos &&
                s.find("inf") == std::string::npos &&
                s.find("nan") == std::string::npos)
                s += ".0";
            return s;
        }
    }
    return "0.0";
}
std::string jstr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (uint8_t(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else out += c;
    }
    out += '"';
    return out;
}

// _element_texture_key: element matId -> texture key via the material chain.
// Returns (has_key, key).
std::pair<bool, uint32_t> element_texture_key(
    uint32_t grm_key, uint32_t mat_id,
    const std::unordered_map<uint32_t, const SubEntry*>& by) {
    auto mit = by.find(grm_key);
    if (mit == by.end()) return {false, 0};
    const SubEntry* m = mit->second;
    auto tex_of = [](const SubEntry* s) -> std::pair<bool, uint32_t> {
        uint32_t k = resolve_texture_key(s->data.data(), s->data.size());
        if (k == 0 || k == INVALID_KEY) return {false, 0};
        return {true, k};
    };
    if (!m->gro_null && m->gro_type == 4) {
        MatInfo mm = parse_material(m->data.data(), m->data.size(), 4);
        const std::vector<uint32_t>& keys = mm.sub_material_keys;
        if (mat_id < keys.size()) {
            auto sit = by.find(keys[mat_id]);
            if (sit != by.end()) return tex_of(sit->second);
        }
        auto dit = by.find(mat_id);        // matId as a direct sub-mat key
        if (dit != by.end() && !dit->second->gro_null &&
            (dit->second->gro_type == 5 || dit->second->gro_type == 3 ||
             dit->second->gro_type == 4))
            return tex_of(dit->second);
        return {false, 0};
    }
    return tex_of(m);
}

constexpr double OMNI_CD_PER_RANGE2 = 22.0;
constexpr double SUN_LUX = 2000.0;

double omni_intensity(bool has_far, double far) {
    double f = (has_far && far > 0.5) ? far : 5.0;
    return OMNI_CD_PER_RANGE2 * f * f;
}

double round3d(double x) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", x);
    return std::strtod(buf, nullptr);
}

double light_radius(bool has_near, double near_v, bool has_far, double far_v) {
    double f = (has_far && far_v != 0.0) ? far_v : 10.0;
    double n = (has_near && near_v > 0) ? near_v : f * 0.15;
    return round3d(std::max(0.1, std::min(n, f * 0.5)));
}

std::array<double, 4> dir_to_quat_yup(double dx, double dy, double dz) {
    double d0 = dx, d1 = dz, d2 = -dy;                    // Z-up -> Y-up
    double n = std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
    if (n == 0.0) n = 1.0;
    d0 /= n; d1 /= n; d2 /= n;
    double dot = -d2;                                     // f=(0,0,-1)·d
    if (dot > 0.99999) return {0.0, 0.0, 0.0, 1.0};
    if (dot < -0.99999) return {0.0, 1.0, 0.0, 0.0};
    double ax = 0.0 * d2 - (-1.0) * d1;                   // f × d
    double ay = (-1.0) * d0 - 0.0 * d2;
    double az = 0.0 * d1 - 0.0 * d0;
    double s = std::sqrt((1 + dot) * 2);
    return {ax / s, ay / s, az / s, s / 2};
}

// The _Glb writer (accessors/bufferViews/meshes/materials/nodes/lights).
struct GlbWriter {
    std::vector<uint8_t> bin;
    std::vector<std::string> bufferViews, accessors, meshes, materials, images,
        textures, nodes, lights;
    std::vector<int> scene_nodes;
    bool has_ambient = false;
    double ambient[3] = {0, 0, 0};
    bool color_to_linear = false;

    int bv(const std::vector<uint8_t>& data, int target) {
        while (bin.size() % 4) bin.push_back(0);
        size_t off = bin.size();
        bin.insert(bin.end(), data.begin(), data.end());
        std::string j = "{\"buffer\": 0, \"byteOffset\": " + std::to_string(off) +
                        ", \"byteLength\": " + std::to_string(data.size());
        if (target) j += ", \"target\": " + std::to_string(target);
        j += "}";
        bufferViews.push_back(std::move(j));
        return int(bufferViews.size()) - 1;
    }
    int acc(int bvi, int comp, size_t count, const char* atype,
            const double* mn = nullptr, const double* mx = nullptr) {
        std::string j = "{\"bufferView\": " + std::to_string(bvi) +
                        ", \"componentType\": " + std::to_string(comp) +
                        ", \"count\": " + std::to_string(count) +
                        ", \"type\": " + jstr(atype);
        if (mn != nullptr) {
            j += ", \"min\": [" + jflt(mn[0]) + ", " + jflt(mn[1]) + ", " +
                 jflt(mn[2]) + "], \"max\": [" + jflt(mx[0]) + ", " + jflt(mx[1]) +
                 ", " + jflt(mx[2]) + "]";
        }
        j += "}";
        accessors.push_back(std::move(j));
        return int(accessors.size()) - 1;
    }
    int add_image_png(const std::vector<uint8_t>& png) {
        int b = bv(png, 0);
        images.push_back("{\"bufferView\": " + std::to_string(b) +
                         ", \"mimeType\": \"image/png\"}");
        textures.push_back("{\"sampler\": 0, \"source\": " +
                           std::to_string(int(images.size()) - 1) + "}");
        return int(textures.size()) - 1;
    }
    int add_material(const std::string& name, int tex_idx) {
        std::string pbr = "{\"baseColorFactor\": [0.8, 0.8, 0.8, 1.0], "
                          "\"metallicFactor\": 0.0, \"roughnessFactor\": 1.0";
        if (tex_idx >= 0)
            pbr += ", \"baseColorTexture\": {\"index\": " + std::to_string(tex_idx) + "}";
        pbr += "}";
        materials.push_back("{\"name\": " + jstr(name) +
                            ", \"pbrMetallicRoughness\": " + pbr + "}");
        return int(materials.size()) - 1;
    }
    std::string node_json(const std::string& body) { return "{" + body + "}"; }
};

std::vector<uint8_t> f32bytes(const std::vector<float>& v) {
    std::vector<uint8_t> out(v.size() * 4);
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}

}  // namespace

ExportManifest export_level_to_glb(const std::vector<SubEntry>& subs,
                                   const std::string& out_path,
                                   bool include_lights, const BigFile* texture_bf,
                                   bool srgb_lighting, LevelBlendLogFn log_fn) {
    ExportManifest man;
    Ctx ctx = reimport_context(subs);   // by-key + geo_nb (same lookups)
    GlbWriter glb;
    glb.color_to_linear = srgb_lighting;

    // Lazy shared-texture index over the whole archive (build_texture_resolver).
    std::unordered_map<uint32_t, std::vector<uint8_t>> tex_cache;
    bool tex_built = false;
    auto resolve_tex = [&](uint32_t tk) -> const std::vector<uint8_t>* {
        auto it = ctx.by.find(tk);
        if (it != ctx.by.end() &&
            is_texture_entry(it->second->data.data(), it->second->data.size()))
            return &it->second->data;
        if (texture_bf == nullptr) return nullptr;
        if (!tex_built) {
            tex_built = true;
            for (const auto& kv : texture_bf->files) {
                LzoResult r = decompress_lzo(texture_bf->read_data(kv.second.index));
                if (!r.ok) continue;
                for (const SubEntry& s : parse_sub_entries(r.data))
                    if (!tex_cache.count(s.key) &&
                        is_texture_entry(s.data.data(), s.data.size()))
                        tex_cache[s.key] = s.data;
            }
        }
        auto cit = tex_cache.find(tk);
        return cit == tex_cache.end() ? nullptr : &cit->second;
    };

    // texture key -> material idx (or -1), dedup.
    std::vector<std::pair<uint32_t, int>> tex_to_mat;
    auto material_for_tex = [&](bool has_tk, uint32_t tk) -> int {
        if (!has_tk) return -1;
        for (const auto& kv : tex_to_mat)
            if (kv.first == tk) return kv.second;
        const std::vector<uint8_t>* td = resolve_tex(tk);
        int mi = -1;
        if (td != nullptr) {
            TexInfo ti = parse_texture(td->data(), td->size());
            std::vector<uint8_t> rgba;
            if (ti.valid) {
                const std::vector<uint8_t>* pal = palette_for_texture(ti, subs);
                rgba = decode_texture(td->data(), td->size(), ti,
                                      pal ? pal->data() : nullptr,
                                      pal ? pal->size() : 0);
            }
            if (!rgba.empty()) {
                char nm[32];
                std::snprintf(nm, sizeof nm, "mat_tex_0x%08X", tk);
                int timg = glb.add_image_png(
                    png_encode_rgba(rgba.data(), ti.width, ti.height));
                mi = glb.add_material(nm, timg);
            }
        }
        tex_to_mat.push_back({tk, mi});
        return mi;
    };

    auto srgb_dec = [](double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    auto color_floats = [&](const std::vector<RgbaColor>& cs) {
        std::vector<float> out;
        out.reserve(cs.size() * 4);
        for (const RgbaColor& c : cs) {
            double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
            if (glb.color_to_linear) { r = srgb_dec(r); g = srgb_dec(g); b = srgb_dec(b); }
            out.push_back(float(r));
            out.push_back(float(g));
            out.push_back(float(b));
            out.push_back(float(c.a / 255.0));
        }
        return out;
    };

    // add_mesh: shared POSITION/TEXCOORD_0/COLOR_0, one primitive per element
    // group. Returns the mesh index.
    auto add_mesh = [&](const std::vector<V3>& world, const std::vector<float>* uvs,
                        const std::vector<float>& colors4,
                        const std::vector<std::pair<std::vector<uint32_t>, int>>& prims) {
        std::vector<float> pos;
        pos.reserve(world.size() * 3);
        // min/max are computed over the SOURCE doubles (like the Python), not
        // the f32-quantized values.
        double mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
        for (size_t i = 0; i < world.size(); ++i) {
            pos.push_back(float(world[i].x));
            pos.push_back(float(world[i].y));
            pos.push_back(float(world[i].z));
            double dx = world[i].x, dy = world[i].y, dz = world[i].z;
            if (i == 0) { mn[0] = mx[0] = dx; mn[1] = mx[1] = dy; mn[2] = mx[2] = dz; }
            else {
                mn[0] = std::min(mn[0], dx); mx[0] = std::max(mx[0], dx);
                mn[1] = std::min(mn[1], dy); mx[1] = std::max(mx[1], dy);
                mn[2] = std::min(mn[2], dz); mx[2] = std::max(mx[2], dz);
            }
        }
        int pos_acc = glb.acc(glb.bv(f32bytes(pos), 34962), 5126, world.size(),
                              "VEC3", mn, mx);
        std::string attrs = "\"POSITION\": " + std::to_string(pos_acc);
        if (uvs != nullptr && !uvs->empty())
            attrs += ", \"TEXCOORD_0\": " +
                     std::to_string(glb.acc(glb.bv(f32bytes(*uvs), 34962), 5126,
                                            uvs->size() / 2, "VEC2"));
        if (!colors4.empty())
            attrs += ", \"COLOR_0\": " +
                     std::to_string(glb.acc(glb.bv(f32bytes(colors4), 34962), 5126,
                                            colors4.size() / 4, "VEC4"));
        bool wide = world.size() > 65535;
        std::string pj;
        for (const auto& pr : prims) {
            if (pr.first.empty()) continue;
            std::vector<uint8_t> ib;
            for (uint32_t ix : pr.first) {
                if (wide) {
                    ib.push_back(uint8_t(ix)); ib.push_back(uint8_t(ix >> 8));
                    ib.push_back(uint8_t(ix >> 16)); ib.push_back(uint8_t(ix >> 24));
                } else {
                    ib.push_back(uint8_t(ix)); ib.push_back(uint8_t(ix >> 8));
                }
            }
            int idx_acc = glb.acc(glb.bv(ib, 34963), wide ? 5125 : 5123,
                                  pr.first.size(), "SCALAR");
            if (!pj.empty()) pj += ", ";
            pj += "{\"attributes\": {" + attrs + "}, \"indices\": " +
                  std::to_string(idx_acc) + ", \"mode\": 4";
            if (pr.second >= 0) pj += ", \"material\": " + std::to_string(pr.second);
            pj += "}";
        }
        glb.meshes.push_back("{\"primitives\": [" + pj + "]}");
        return int(glb.meshes.size()) - 1;
    };

    // ── mesh pass ──
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
        if (!gi.ok || !gi.vis_read) continue;
        auto nbit = ctx.geo_nb.find(gi.gro_key);
        if (nbit == ctx.geo_nb.end()) continue;
        uint32_t nb = nbit->second;
        if (!has_rli(s.data.data(), s.data.size(), nb)) continue;
        const SubEntry& geo_sub = *ctx.by.at(gi.gro_key);

        double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (gi.gmat_present && gi.gmat_raw.size() >= 64)
            for (int i = 0; i < 16; ++i) {
                float fv;
                std::memcpy(&fv, gi.gmat_raw.data() + size_t(i) * 4, 4);
                m[i] = double(fv);
            }
        std::string name = gi.name;
        name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
        if (name.empty()) {
            char b[24];
            std::snprintf(b, sizeof b, "gao_0x%08X", s.key);
            name = b;
        }

        CookedMesh cm = cooked_mesh(geo_sub.data.data(), geo_sub.data.size());
        ExtraColors ec = read_extra_colors(s.data.data(), s.data.size(), nb);
        char gao_hex[16], geo_hex[16];
        std::snprintf(gao_hex, sizeof gao_hex, "0x%08X", s.key);
        std::snprintf(geo_hex, sizeof geo_hex, "0x%08X", gi.gro_key);

        if (!cm.ok || !ec.ok || cm.n != ec.colors.size() || cm.faces.empty()) {
            // Primary-only fallback (skinned or no cooked VB).
            GeoInfo geo = parse_geometry(geo_sub.data.data(), geo_sub.data.size());
            PrimaryColors prim = read_primary_colors(s.data.data(), s.data.size(), nb);
            if (!prim.ok || geo.vertices.empty() || geo.faces.empty()) continue;
            std::vector<V3> world;
            world.reserve(geo.vertices.size() / 3);
            for (size_t i = 0; i + 2 < geo.vertices.size(); i += 3)
                world.push_back(xform_world_yup(m, double(geo.vertices[i]),
                                                double(geo.vertices[i + 1]),
                                                double(geo.vertices[i + 2])));
            std::vector<uint32_t> faces;
            faces.reserve(size_t(geo.nb_tris) * 3);
            for (uint32_t fi = 0; fi < geo.nb_tris; ++fi) {
                faces.push_back(geo.faces[size_t(fi) * 7]);
                faces.push_back(geo.faces[size_t(fi) * 7 + 1]);
                faces.push_back(geo.faces[size_t(fi) * 7 + 2]);
            }
            int mesh_idx = add_mesh(world, nullptr, color_floats(prim.colors),
                                    {{faces, -1}});
            bool unlit = true;
            for (const RgbaColor& c : prim.colors)
                if (c.r || c.g || c.b) { unlit = false; break; }
            std::string ex = std::string("\"jade_gao\": ") + jstr(gao_hex) +
                             ", \"jade_geo\": " + jstr(geo_hex) +
                             ", \"nb\": " + std::to_string(nb) + ", " +
                             (geo.skin_present ? "\"jade_skinned\"" : "\"jade_base\"") +
                             ": true";
            if (unlit) ex += ", \"jade_unlit\": true";
            glb.nodes.push_back("{\"name\": " + jstr(name) +
                                ", \"mesh\": " + std::to_string(mesh_idx) +
                                ", \"extras\": {" + ex + "}}");
            glb.scene_nodes.push_back(int(glb.nodes.size()) - 1);
            man.exported.push_back({s.key, gi.gro_key, nb});
            ++man.objects;
            continue;
        }

        std::vector<V3> world;
        world.reserve(cm.n);
        for (uint32_t i = 0; i < cm.n; ++i)
            world.push_back(xform_world_yup(m, double(cm.positions[i * 3]),
                                            double(cm.positions[i * 3 + 1]),
                                            double(cm.positions[i * 3 + 2])));
        // Split cooked faces by element -> matId -> texture key (dict order).
        GeoInfo geo = parse_geometry(geo_sub.data.data(), geo_sub.data.size());
        struct TexGroup { bool has; uint32_t tk; std::vector<uint32_t> faces; };
        std::vector<TexGroup> groups;
        size_t ntri = cm.faces.size() / 3;
        for (size_t fi = 0; fi < ntri; ++fi) {
            uint32_t el = fi < geo.nb_tris ? geo.faces[fi * 7 + 6] : 0;
            uint32_t mat_id = (size_t(el) * 2 + 1) < geo.elements.size()
                                  ? geo.elements[size_t(el) * 2 + 1]
                                  : 0;
            auto tk = element_texture_key(gi.grm_key, mat_id, ctx.by);
            TexGroup* g = nullptr;
            for (auto& e : groups)
                if (e.has == tk.first && (!e.has || e.tk == tk.second)) { g = &e; break; }
            if (g == nullptr) {
                groups.push_back({tk.first, tk.second, {}});
                g = &groups.back();
            }
            g->faces.push_back(cm.faces[fi * 3]);
            g->faces.push_back(cm.faces[fi * 3 + 1]);
            g->faces.push_back(cm.faces[fi * 3 + 2]);
        }
        std::vector<std::pair<std::vector<uint32_t>, int>> prims;
        for (const TexGroup& g : groups)
            prims.push_back({g.faces, material_for_tex(g.has, g.tk)});

        int mesh_idx = add_mesh(world, &cm.uvs, color_floats(ec.colors), prims);
        bool unlit = true;
        for (const RgbaColor& c : ec.colors)
            if (c.r || c.g || c.b) { unlit = false; break; }
        std::string ex = std::string("\"jade_gao\": ") + jstr(gao_hex) +
                         ", \"jade_geo\": " + jstr(geo_hex) +
                         ", \"nb\": " + std::to_string(nb);
        if (unlit) ex += ", \"jade_unlit\": true";
        glb.nodes.push_back("{\"name\": " + jstr(name) +
                            ", \"mesh\": " + std::to_string(mesh_idx) +
                            ", \"extras\": {" + ex + "}}");
        glb.scene_nodes.push_back(int(glb.nodes.size()) - 1);
        man.exported.push_back({s.key, gi.gro_key, nb});
        ++man.objects;
    }

    // ── lights ──
    auto ltype_count = [&](const std::string& t) {
        for (auto& kv : man.light_types)
            if (kv.first == t) { ++kv.second; return; }
        man.light_types.push_back({t, 1});
    };
    if (include_lights) {
        // light key -> (is_visual, matrix) marker map; prefer non-visual.
        struct Marker { bool is_vis; bool has_m; double m[16]; };
        std::vector<std::pair<uint32_t, Marker>> marker_mat;
        auto marker_find = [&](uint32_t k) -> Marker* {
            for (auto& kv : marker_mat)
                if (kv.first == k) return &kv.second;
            return nullptr;
        };
        for (const SubEntry& gs : subs) {
            if (gs.ext != ".gao" || gs.data.size() < 4) continue;
            uint32_t lk = get_u32(gs.data.data(), gs.data.size() - 4);
            GaoInfo gi = parse_gao_full(gs.data.data(), gs.data.size());
            bool is_vis = gi.ok && gi.vis_read;
            Marker mk;
            mk.is_vis = is_vis;
            mk.has_m = gi.ok && gi.gmat_present && gi.gmat_raw.size() >= 64;
            if (mk.has_m)
                for (int i = 0; i < 16; ++i) {
                    float fv;
                    std::memcpy(&fv, gi.gmat_raw.data() + size_t(i) * 4, 4);
                    mk.m[i] = double(fv);
                }
            Marker* cur = marker_find(lk);
            if (cur == nullptr) marker_mat.push_back({lk, mk});
            else if (cur->is_vis && !is_vis) *cur = mk;
        }

        bool have_ambient = false;
        double amb[3] = {0, 0, 0};
        for (const SubEntry& s : subs) {
            if (s.gro_null || s.gro_type != 2 ||
                !is_light_payload(s.data.data(), s.data.size()))
                continue;
            LightInfo li = parse_light(s.data.data(), s.data.size());
            if (!li.ok) continue;
            uint32_t ltype = li.type;
            std::string tname = li.type_name.empty() ? std::to_string(ltype)
                                                     : li.type_name;
            double col[3] = {1.0, 1.0, 1.0};
            if (li.diffuse.present) {
                col[0] = double(li.diffuse.bits & 0xFF) / 255.0;
                col[1] = double((li.diffuse.bits >> 8) & 0xFF) / 255.0;
                col[2] = double((li.diffuse.bits >> 16) & 0xFF) / 255.0;
            }
            auto f32of = [](const LightField& f) {
                float v;
                uint32_t b = f.bits;
                std::memcpy(&v, &b, 4);
                return double(v);
            };
            bool has_near = li.near_.present, has_far = li.far_.present;
            double near_v = has_near ? f32of(li.near_) : 0.0;
            double far_v = has_far ? f32of(li.far_) : 0.0;
            Marker* mk = marker_find(s.key);
            bool has_m = mk != nullptr && mk->has_m;
            double fwd[3] = {0.0, 0.0, -1.0};
            if (has_m) { fwd[0] = -mk->m[4]; fwd[1] = -mk->m[5]; fwd[2] = -mk->m[6]; }
            if (ltype == 3) continue;                      // Fog
            if (ltype == 5) {                              // Ambient -> World
                if (!have_ambient) {
                    have_ambient = true;
                    amb[0] = col[0]; amb[1] = col[1]; amb[2] = col[2];
                } else {
                    amb[0] = std::max(amb[0], col[0]);
                    amb[1] = std::max(amb[1], col[1]);
                    amb[2] = std::max(amb[2], col[2]);
                }
                ltype_count(tname);
                continue;
            }
            char lname[32];
            std::snprintf(lname, sizeof lname, "GRO_Light_0x%08X", s.key);
            std::string ex = std::string("\"jade_light\": ") +
                             jstr(std::string("0x") + (lname + 12)) +
                             ", \"jade_light_type\": " + jstr(tname) +
                             ", \"jade_radius\": " +
                             jflt(light_radius(has_near, near_v, has_far, far_v));
            std::string cols = "[" + jflt(col[0]) + ", " + jflt(col[1]) + ", " +
                               jflt(col[2]) + "]";
            auto trans_of = [&]() {
                double x = has_m ? mk->m[12] : 0.0;
                double y = has_m ? mk->m[14] : 0.0;
                double z = has_m ? -mk->m[13] : 0.0;
                return "[" + jflt(x) + ", " + jflt(y) + ", " + jflt(z) + "]";
            };
            auto rot_of = [&]() {
                std::array<double, 4> q = dir_to_quat_yup(fwd[0], fwd[1], fwd[2]);
                return "[" + jflt(q[0]) + ", " + jflt(q[1]) + ", " + jflt(q[2]) +
                       ", " + jflt(q[3]) + "]";
            };
            if (ltype == 1) {                              // Directional
                glb.lights.push_back("{\"type\": \"directional\", \"name\": " +
                                     jstr(lname) + ", \"color\": " + cols +
                                     ", \"intensity\": " + jflt(SUN_LUX) + "}");
                glb.nodes.push_back("{\"name\": " + jstr(lname) +
                                    ", \"rotation\": " + rot_of() +
                                    ", \"extras\": {" + ex + "}, \"extensions\": "
                                    "{\"KHR_lights_punctual\": {\"light\": " +
                                    std::to_string(int(glb.lights.size()) - 1) + "}}}");
            } else if (ltype == 2) {                       // Spot
                double outer = 0.7853, inner = 0.0;
                if (li.outer.present && f32of(li.outer) != 0.0) outer = f32of(li.outer);
                outer = std::min(1.5, outer);
                if (li.inner.present && f32of(li.inner) != 0.0) inner = f32of(li.inner);
                inner = std::min(outer, inner);
                std::string lj = "{\"type\": \"spot\", \"name\": " + jstr(lname) +
                                 ", \"color\": " + cols + ", \"intensity\": " +
                                 jflt(omni_intensity(has_far, far_v));
                if (has_far && far_v != 0.0) lj += ", \"range\": " + jflt(far_v);
                lj += ", \"spot\": {\"innerConeAngle\": " + jflt(inner) +
                      ", \"outerConeAngle\": " + jflt(outer) + "}}";
                glb.lights.push_back(lj);
                glb.nodes.push_back("{\"name\": " + jstr(lname) +
                                    ", \"translation\": " + trans_of() +
                                    ", \"rotation\": " + rot_of() +
                                    ", \"extras\": {" + ex + "}, \"extensions\": "
                                    "{\"KHR_lights_punctual\": {\"light\": " +
                                    std::to_string(int(glb.lights.size()) - 1) + "}}}");
            } else {                                       // Omni -> point
                std::string lj = "{\"type\": \"point\", \"name\": " + jstr(lname) +
                                 ", \"color\": " + cols + ", \"intensity\": " +
                                 jflt(omni_intensity(has_far, far_v));
                if (has_far && far_v != 0.0) lj += ", \"range\": " + jflt(far_v);
                lj += "}";
                glb.lights.push_back(lj);
                glb.nodes.push_back("{\"name\": " + jstr(lname) +
                                    ", \"translation\": " + trans_of() +
                                    ", \"extras\": {" + ex + "}, \"extensions\": "
                                    "{\"KHR_lights_punctual\": {\"light\": " +
                                    std::to_string(int(glb.lights.size()) - 1) + "}}}");
            }
            glb.scene_nodes.push_back(int(glb.nodes.size()) - 1);
            ltype_count(tname);
            ++man.lights;
        }
        if (have_ambient) {
            glb.has_ambient = true;
            glb.ambient[0] = amb[0]; glb.ambient[1] = amb[1]; glb.ambient[2] = amb[2];
        }
    }

    // ── container ──
    auto join = [](const std::vector<std::string>& v) {
        std::string out;
        for (const std::string& s : v) {
            if (!out.empty()) out += ", ";
            out += s;
        }
        return out;
    };
    std::string sn;
    for (int i : glb.scene_nodes) {
        if (!sn.empty()) sn += ", ";
        sn += std::to_string(i);
    }
    std::string gltf =
        "{\"asset\": {\"version\": \"2.0\", \"generator\": "
        "\"jade_explorer level_blender\"}, \"scene\": 0, \"scenes\": "
        "[{\"nodes\": [" + sn + "]}], \"nodes\": [" + join(glb.nodes) +
        "], \"meshes\": [" + join(glb.meshes) + "], \"accessors\": [" +
        join(glb.accessors) + "], \"bufferViews\": [" + join(glb.bufferViews) +
        "], \"buffers\": [{\"byteLength\": " + std::to_string(glb.bin.size()) + "}]";
    if (!glb.materials.empty()) gltf += ", \"materials\": [" + join(glb.materials) + "]";
    if (!glb.images.empty()) {
        gltf += ", \"images\": [" + join(glb.images) + "], \"textures\": [" +
                join(glb.textures) +
                "], \"samplers\": [{\"magFilter\": 9729, \"minFilter\": 9729}]";
    }
    if (!glb.lights.empty())
        gltf += ", \"extensions\": {\"KHR_lights_punctual\": {\"lights\": [" +
                join(glb.lights) + "]}}, \"extensionsUsed\": [\"KHR_lights_punctual\"]";
    std::string extras = "\"jade_level\": true, \"objects\": " +
                         std::to_string(man.objects) + ", \"light_types\": {";
    {
        std::string lt;
        for (const auto& kv : man.light_types) {
            if (!lt.empty()) lt += ", ";
            lt += jstr(kv.first) + ": " + std::to_string(kv.second);
        }
        extras += lt + "}";
    }
    if (glb.has_ambient)
        extras += ", \"jade_ambient\": [" + jflt(glb.ambient[0]) + ", " +
                  jflt(glb.ambient[1]) + ", " + jflt(glb.ambient[2]) + "]";
    gltf += ", \"extras\": {" + extras + "}}";

    std::vector<uint8_t> json_bytes(gltf.begin(), gltf.end());
    while (json_bytes.size() % 4) json_bytes.push_back(' ');
    std::vector<uint8_t> bin_blob = glb.bin;
    while (bin_blob.size() % 4) bin_blob.push_back(0);
    uint32_t total = uint32_t(12 + 8 + json_bytes.size() + 8 + bin_blob.size());
    std::ofstream f(fs::u8path(out_path), std::ios::binary);
    if (!f) { man.error = "cannot write " + out_path; return man; }
    auto w32 = [&](uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        f.write(reinterpret_cast<const char*>(b), 4);
    };
    f.write("glTF", 4);
    w32(2);
    w32(total);
    w32(uint32_t(json_bytes.size()));
    f.write("JSON", 4);
    f.write(reinterpret_cast<const char*>(json_bytes.data()),
            std::streamsize(json_bytes.size()));
    w32(uint32_t(bin_blob.size()));
    f.write("BIN\0", 4);
    f.write(reinterpret_cast<const char*>(bin_blob.data()),
            std::streamsize(bin_blob.size()));
    man.ok = true;
    if (log_fn) {
        std::string types = "{";
        for (size_t i = 0; i < man.light_types.size(); ++i) {
            if (i) types += ", ";
            types += "'" + man.light_types[i].first + "': " +
                     std::to_string(man.light_types[i].second);
        }
        types += "}";
        log_fn("exported " + std::to_string(man.objects) +
               " RLI meshes + " + std::to_string(man.lights) + " lights " +
               types + " (+ambient) -> " + out_path);
    }
    return man;
}

std::vector<uint8_t> png_encode_rgba_pub(const uint8_t* rgba,
                                         uint32_t w, uint32_t h) {
    return png_encode_rgba(rgba, w, h);
}

}  // namespace levelblend
}  // namespace jade
