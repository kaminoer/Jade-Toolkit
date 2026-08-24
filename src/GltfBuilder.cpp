// GltfBuilder.cpp — implementation. Faithful port of build_glb's mesh path.
#include "jade/GltfBuilder.hpp"
#include "jade/Gao.hpp"
#include "jade/LevelBlender.hpp"
#include "jade/Material.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/Skeleton.hpp"
#include "jade/Texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>

namespace jade {
namespace gltfbuild {

namespace {

// Python repr shortest-roundtrip float text (json.dumps of a float).
std::string pyfloat(double v) {
    for (int prec = 1; prec <= 17; ++prec) {
        char b[64];
        std::snprintf(b, sizeof b, "%.*g", prec, v);
        if (std::strtod(b, nullptr) == v) {
            std::string s(b);
            if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
                s += ".0";
            return s;
        }
    }
    return "0.0";
}

struct Bin {
    std::vector<uint8_t> data;
    std::vector<std::string> buffer_views, accessors;
    int add_bv(const uint8_t* d, size_t n, int target) {
        while (data.size() % 4) data.push_back(0);
        size_t off = data.size();
        data.insert(data.end(), d, d + n);
        std::string j = "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
                        ",\"byteLength\":" + std::to_string(n);
        if (target) j += ",\"target\":" + std::to_string(target);
        j += "}";
        buffer_views.push_back(std::move(j));
        return int(buffer_views.size()) - 1;
    }
    int add_acc(int bv, int comp, size_t count, const char* type,
                const std::string& mn = "", const std::string& mx = "") {
        std::string j = "{\"bufferView\":" + std::to_string(bv) +
                        ",\"componentType\":" + std::to_string(comp) +
                        ",\"count\":" + std::to_string(count) + ",\"type\":\"" +
                        type + "\"";
        if (!mn.empty()) j += ",\"min\":" + mn + ",\"max\":" + mx;
        j += "}";
        accessors.push_back(std::move(j));
        return int(accessors.size()) - 1;
    }
};

}  // namespace

std::vector<uint8_t> build_geo_model_glb(const GeoInfo& geo,
                                         const std::string& mesh_name,
                                         const std::string& jade_key_hex,
                                         const std::string& scene_name) {
    size_t nverts = geo.vertices.size() / 3;
    size_t nfaces = geo.faces.size() / 7;   // FILTERED tris (may be < nb_tris)
    if (nverts == 0 || nfaces == 0) return {};
    bool have_norms = geo.normals.size() == geo.vertices.size();
    bool have_uvs = !geo.uvs.empty();

    // UV seam splitting by (vert, uv) combo — dict-ordered like the Python.
    struct V3 { float x, y, z; };
    std::vector<V3> verts, norms;
    std::vector<std::array<float, 2>> uv_per_vert;
    std::vector<uint32_t> orig_vert_indices;
    std::vector<std::array<uint32_t, 3>> faces;
    auto vert_of = [&](size_t vi) {
        if (vi < nverts)
            return V3{geo.vertices[vi * 3], geo.vertices[vi * 3 + 1],
                      geo.vertices[vi * 3 + 2]};
        return V3{0, 0, 0};
    };
    bool split = have_uvs && nfaces > 0;
    if (split) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> combo;
        size_t nuvs = geo.uvs.size() / 2;
        for (size_t fi = 0; fi < nfaces; ++fi) {
            std::array<uint32_t, 3> nf{};
            for (int c = 0; c < 3; ++c) {
                uint32_t vi = geo.faces[fi * 7 + size_t(c)];
                uint32_t ui = geo.faces[fi * 7 + 3 + size_t(c)];
                auto key = std::make_pair(vi, ui);
                auto it = combo.find(key);
                uint32_t ni;
                if (it == combo.end()) {
                    ni = uint32_t(verts.size());
                    combo[key] = ni;
                    verts.push_back(vert_of(vi));
                    if (have_norms) {
                        if (vi < nverts)
                            norms.push_back({geo.normals[vi * 3],
                                             geo.normals[vi * 3 + 1],
                                             geo.normals[vi * 3 + 2]});
                        else
                            norms.push_back({0, 0, 1});
                    }
                    if (ui < nuvs)
                        uv_per_vert.push_back({geo.uvs[ui * 2], geo.uvs[ui * 2 + 1]});
                    else
                        uv_per_vert.push_back({0.0f, 0.0f});
                    orig_vert_indices.push_back(vi);
                } else {
                    ni = it->second;
                }
                nf[size_t(c)] = ni;
            }
            faces.push_back(nf);
        }
    } else {
        for (size_t vi = 0; vi < nverts; ++vi) {
            verts.push_back(vert_of(vi));
            if (have_norms)
                norms.push_back({geo.normals[vi * 3], geo.normals[vi * 3 + 1],
                                 geo.normals[vi * 3 + 2]});
            orig_vert_indices.push_back(uint32_t(vi));
        }
        for (size_t fi = 0; fi < nfaces; ++fi)
            faces.push_back({geo.faces[fi * 7], geo.faces[fi * 7 + 1],
                             geo.faces[fi * 7 + 2]});
    }

    // Z-up -> Y-up.
    for (V3& v : verts) v = {v.x, v.z, -v.y};
    for (V3& n : norms) n = {n.x, n.z, -n.y};

    Bin bin;
    // POSITION with f32 min/max (np.min over the float32 array).
    std::vector<float> pf;
    pf.reserve(verts.size() * 3);
    float mn[3], mx[3];
    for (size_t i = 0; i < verts.size(); ++i) {
        float c[3] = {verts[i].x, verts[i].y, verts[i].z};
        for (int k = 0; k < 3; ++k) {
            pf.push_back(c[k]);
            if (i == 0) { mn[k] = mx[k] = c[k]; }
            else {
                mn[k] = std::min(mn[k], c[k]);
                mx[k] = std::max(mx[k], c[k]);
            }
        }
    }
    auto list3 = [&](const float* v) {
        return "[" + pyfloat(double(v[0])) + "," + pyfloat(double(v[1])) + "," +
               pyfloat(double(v[2])) + "]";
    };
    int pos_acc = bin.add_acc(
        bin.add_bv(reinterpret_cast<const uint8_t*>(pf.data()), pf.size() * 4, 34962),
        5126, verts.size(), "VEC3", list3(mn), list3(mx));

    int norm_acc = -1;
    if (!norms.empty() && norms.size() == verts.size()) {
        std::vector<float> nf2;
        nf2.reserve(norms.size() * 3);
        for (const V3& n : norms) { nf2.push_back(n.x); nf2.push_back(n.y); nf2.push_back(n.z); }
        norm_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(nf2.data()), nf2.size() * 4, 34962),
            5126, norms.size(), "VEC3");
    }
    int uv_acc = -1;
    if (split && uv_per_vert.size() == verts.size()) {
        std::vector<float> uf;
        uf.reserve(uv_per_vert.size() * 2);
        for (const auto& u : uv_per_vert) { uf.push_back(u[0]); uf.push_back(u[1]); }
        uv_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(uf.data()), uf.size() * 4, 34962),
            5126, verts.size(), "VEC2");
    }
    // TEXCOORD_1 = the Jade vertex id.
    int jade_uv1_acc = -1;
    {
        std::vector<float> jf(verts.size() * 2, 0.0f);
        for (size_t i = 0; i < verts.size(); ++i) jf[i * 2] = float(orig_vert_indices[i]);
        jade_uv1_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(jf.data()), jf.size() * 4, 34962),
            5126, verts.size(), "VEC2");
    }
    // Skin: JOINTS_0 u8x4 + WEIGHTS_0 f32x4, top-4 by weight, normalized.
    int joints_acc = -1, weights_acc = -1;
    bool has_skin = geo.skin_present && !geo.skin_bones.empty();
    bool any_weights = false;
    if (has_skin) {
        std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, double>>> vw;
        for (size_t bi = 0; bi < geo.skin_bones.size(); ++bi)
            for (const auto& w : geo.skin_bones[bi].weights) {
                vw[w.first].push_back({uint32_t(bi), double(w.second) / 65535.0});
                any_weights = true;
            }
        if (any_weights) {
            std::vector<uint8_t> joints(verts.size() * 4, 0);
            std::vector<float> weights(verts.size() * 4, 0.0f);
            for (size_t nv = 0; nv < verts.size(); ++nv) {
                auto it = vw.find(orig_vert_indices[nv]);
                if (it == vw.end()) continue;
                auto infl = it->second;
                std::stable_sort(infl.begin(), infl.end(),
                                 [](const auto& a, const auto& b) {
                                     return a.second > b.second;
                                 });
                if (infl.size() > 4) infl.resize(4);
                double total = 0;
                for (const auto& iw : infl) total += iw.second;
                for (size_t ii = 0; ii < infl.size(); ++ii) {
                    joints[nv * 4 + ii] = uint8_t(std::min<uint32_t>(infl[ii].first, 255));
                    weights[nv * 4 + ii] =
                        total > 0 ? float(infl[ii].second / total) : 0.0f;
                }
            }
            joints_acc = bin.add_acc(
                bin.add_bv(joints.data(), joints.size(), 34962), 5121,
                verts.size(), "VEC4");
            weights_acc = bin.add_acc(
                bin.add_bv(reinterpret_cast<const uint8_t*>(weights.data()),
                           weights.size() * 4, 34962),
                5126, verts.size(), "VEC4");
        }
    }

    // Single face group (no material_indices) -> one primitive, sorted key {0}.
    std::vector<uint32_t> indices;
    indices.reserve(faces.size() * 3);
    uint32_t imin = 0, imax = 0;
    for (size_t fi = 0; fi < faces.size(); ++fi)
        for (int c = 0; c < 3; ++c) {
            uint32_t ix = faces[fi][size_t(c)];
            if (fi == 0 && c == 0) imin = imax = ix;
            imin = std::min(imin, ix);
            imax = std::max(imax, ix);
            indices.push_back(ix);
        }
    std::vector<uint8_t> ib;
    int ct = imax <= 0xFFFF ? 5123 : 5125;
    for (uint32_t ix : indices) {
        ib.push_back(uint8_t(ix));
        ib.push_back(uint8_t(ix >> 8));
        if (ct == 5125) { ib.push_back(uint8_t(ix >> 16)); ib.push_back(uint8_t(ix >> 24)); }
    }
    int idx_acc = bin.add_acc(bin.add_bv(ib.data(), ib.size(), 34963), ct,
                              indices.size(), "SCALAR",
                              "[" + std::to_string(imin) + "]",
                              "[" + std::to_string(imax) + "]");

    std::string attrs = "\"POSITION\":" + std::to_string(pos_acc);
    if (norm_acc >= 0) attrs += ",\"NORMAL\":" + std::to_string(norm_acc);
    if (uv_acc >= 0) attrs += ",\"TEXCOORD_0\":" + std::to_string(uv_acc);
    attrs += ",\"TEXCOORD_1\":" + std::to_string(jade_uv1_acc);
    if (joints_acc >= 0) attrs += ",\"JOINTS_0\":" + std::to_string(joints_acc);
    if (weights_acc >= 0) attrs += ",\"WEIGHTS_0\":" + std::to_string(weights_acc);
    std::string prim = "{\"attributes\":{" + attrs + "},\"indices\":" +
                       std::to_string(idx_acc) + ",\"mode\":4}";

    std::string ovm;
    for (uint32_t v : orig_vert_indices) {
        if (!ovm.empty()) ovm += ",";
        ovm += std::to_string(v);
    }
    auto esc = [](const std::string& s) {
        std::string o = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else o += c;
        }
        return o + "\"";
    };
    std::string mesh_extras = "{\"jade_key\":" + esc(jade_key_hex) +
                              ",\"jade_orig_vert_map\":[" + ovm + "]}";
    std::string mesh_j = "{\"name\":" + esc(mesh_name) + ",\"primitives\":[" +
                         prim + "],\"extras\":" + mesh_extras + "}";
    std::string node_j = "{\"name\":" + esc(mesh_name) +
                         ",\"mesh\":0,\"extras\":{\"jade_key\":" +
                         esc(jade_key_hex) + "}}";

    // Skins: no bone nodes exist on the model path, so joints default to 0.
    std::string skins_j;
    if (has_skin && any_weights) {
        std::vector<float> ibm;
        ibm.reserve(geo.skin_bones.size() * 16);
        // jade_to_gltf_matrix on a 16-float bind matrix; clamped to f32 max.
        for (const GeoBone& b : geo.skin_bones) {
            double m[16];
            bool finite = true;
            for (int i = 0; i < 16; ++i) {
                m[i] = double(b.bind_matrix[size_t(i)]);
                if (!std::isfinite(m[i])) finite = false;
            }
            // jade_to_gltf_matrix = flatten_F(S @ reshape_F(m) @ S_inv). Done as
            // the REAL two matmuls (k-ordered sums) — a direct sign-flip table
            // yields -0.0 where numpy's 0-term accumulation yields +0.0.
            static const double S[4][4] = {
                {1, 0, 0, 0}, {0, 0, 1, 0}, {0, -1, 0, 0}, {0, 0, 0, 1}};
            static const double Sinv[4][4] = {
                {1, 0, 0, 0}, {0, 0, -1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}};
            double R[4][4], A[4][4], M[4][4];
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) R[i][j] = m[j * 4 + i];   // order='F'
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) {
                    double acc = 0.0;
                    for (int k = 0; k < 4; ++k) acc += S[i][k] * R[k][j];
                    A[i][j] = acc;
                }
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) {
                    double acc = 0.0;
                    for (int k = 0; k < 4; ++k) acc += A[i][k] * Sinv[k][j];
                    M[i][j] = acc;
                }
            double g[16];
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) g[j * 4 + i] = M[i][j];   // order='F'
            if (!finite) {
                static const double kId[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                               0, 0, 1, 0, 0, 0, 0, 1};
                std::memcpy(g, kId, sizeof g);
            }
            const double fmax = 3.4028235e+38;
            for (int i = 0; i < 16; ++i)
                ibm.push_back(float(std::max(-fmax, std::min(fmax, g[i]))));
        }
        int ibm_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(ibm.data()), ibm.size() * 4, 0),
            5126, geo.skin_bones.size(), "MAT4");
        std::string joints;
        for (size_t i = 0; i < geo.skin_bones.size(); ++i) {
            if (!joints.empty()) joints += ",";
            joints += "0";
        }
        skins_j = "{\"joints\":[" + joints + "],\"inverseBindMatrices\":" +
                  std::to_string(ibm_acc) + ",\"skeleton\":0}";
        node_j = "{\"name\":" + esc(mesh_name) +
                 ",\"mesh\":0,\"extras\":{\"jade_key\":" + esc(jade_key_hex) +
                 "},\"skin\":0}";
    }

    auto join = [](const std::vector<std::string>& v) {
        std::string o;
        for (const std::string& s : v) {
            if (!o.empty()) o += ",";
            o += s;
        }
        return o;
    };
    while (bin.data.size() % 4) bin.data.push_back(0);
    std::string gltf =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"JadeExplorer\"},"
        "\"scene\":0,\"scenes\":[{\"name\":" + esc(scene_name) +
        ",\"nodes\":[0]}],\"nodes\":[" + node_j + "],\"meshes\":[" + mesh_j +
        "],\"accessors\":[" + join(bin.accessors) + "],\"bufferViews\":[" +
        join(bin.buffer_views) + "]";
    if (!skins_j.empty()) gltf += ",\"skins\":[" + skins_j + "]";
    gltf += ",\"buffers\":[{\"byteLength\":" + std::to_string(bin.data.size()) +
            "}]}";

    std::vector<uint8_t> json_bytes(gltf.begin(), gltf.end());
    while (json_bytes.size() % 4) json_bytes.push_back(' ');
    std::vector<uint8_t> out;
    uint32_t total = uint32_t(12 + 8 + json_bytes.size() + 8 + bin.data.size());
    auto w32 = [&](uint32_t v) {
        out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 24));
    };
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    w32(2);
    w32(total);
    w32(uint32_t(json_bytes.size()));
    out.insert(out.end(), {'J', 'S', 'O', 'N'});
    out.insert(out.end(), json_bytes.begin(), json_bytes.end());
    w32(uint32_t(bin.data.size()));
    out.insert(out.end(), {'B', 'I', 'N', 0});
    out.insert(out.end(), bin.data.begin(), bin.data.end());
    return out;
}

// ── export_geo_to_glb_hierarchical (mesh_swap) ─────────────────────────────

namespace {

// Deterministic partial-pivot GJ inverse on a col-major flat 4x4 (mirrors
// object_placer._inv4x4_pp, which the oracle uses at this site).
bool hier_inv4(const std::array<double, 16>& m, std::array<double, 16>& out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[size_t(c * 4 + r)];
            a[r][c + 4] = (r == c) ? 1.0 : 0.0;
        }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (a[piv][col] == 0.0) return false;
        if (piv != col)
            for (int c = 0; c < 8; ++c) std::swap(a[piv][c], a[col][c]);
        double d = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            double f = a[r][col];
            if (f == 0.0) continue;
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[size_t(c * 4 + r)] = a[r][c + 4];
    return true;
}

// numpy A @ B on col-major flats (k-ordered left-to-right sums).
std::array<double, 16> hier_matmul(const std::array<double, 16>& A,
                                   const std::array<double, 16>& B) {
    std::array<double, 16> out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double acc = 0.0;
            for (int k = 0; k < 4; ++k)
                acc += A[size_t(k * 4 + i)] * B[size_t(j * 4 + k)];
            out[size_t(j * 4 + i)] = acc;
        }
    return out;
}

// jade_to_gltf_matrix on a col-major flat (the two real matmuls, like the
// model path above).
std::array<double, 16> hier_jade_to_gltf(const std::array<double, 16>& m) {
    static const double S[4][4] = {
        {1, 0, 0, 0}, {0, 0, 1, 0}, {0, -1, 0, 0}, {0, 0, 0, 1}};
    static const double Sinv[4][4] = {
        {1, 0, 0, 0}, {0, 0, -1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}};
    double R[4][4], A[4][4], M[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) R[i][j] = m[size_t(j * 4 + i)];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double acc = 0.0;
            for (int k = 0; k < 4; ++k) acc += S[i][k] * R[k][j];
            A[i][j] = acc;
        }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double acc = 0.0;
            for (int k = 0; k < 4; ++k) acc += A[i][k] * Sinv[k][j];
            M[i][j] = acc;
        }
    std::array<double, 16> g{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) g[size_t(j * 4 + i)] = M[i][j];
    return g;
}

// scene_export._sanitize_name.
std::string hier_sanitize(const std::string& in) {
    std::string name = in;
    size_t a = name.find_first_not_of(" \t\r\n\f\v");
    size_t b = name.find_last_not_of(" \t\r\n\f\v");
    name = a == std::string::npos ? "" : name.substr(a, b - a + 1);
    name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
    std::string safe;
    for (char c : name)
        if (std::isalnum(uint8_t(c)) || c == '-' || c == '_' || c == '.' ||
            c == ' ')
            safe += c;
    a = safe.find_first_not_of(' ');
    b = safe.find_last_not_of(' ');
    safe = a == std::string::npos ? "" : safe.substr(a, b - a + 1);
    std::string collapsed;
    for (char c : safe)
        if (!(c == ' ' && !collapsed.empty() && collapsed.back() == ' '))
            collapsed += c;
    for (char& c : collapsed)
        if (c == ' ') c = '_';
    if (collapsed.size() > 80) collapsed.resize(80);
    return collapsed;
}

std::string hier_esc(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else o += c;
    }
    return o + "\"";
}

std::string hier_mat16_json(const std::array<double, 16>& m) {
    std::string o = "[";
    for (int i = 0; i < 16; ++i) {
        if (i) o += ",";
        o += pyfloat(m[size_t(i)]);
    }
    return o + "]";
}

std::string scene_join(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) out += ',';
        out += value;
    }
    return out;
}

std::string scene_object_add(const std::string& object,
                             const std::string& key,
                             const std::string& value) {
    if (object.empty() || object == "{}")
        return "{\"" + key + "\":" + value + "}";
    if (object.front() != '{' || object.back() != '}')
        return "{\"" + key + "\":" + value + "}";
    std::string out = object.substr(0, object.size() - 1);
    if (out.size() > 1) out += ',';
    out += "\"" + key + "\":" + value + "}";
    return out;
}

std::vector<uint8_t> scene_finish_glb(const std::string& gltf, Bin& bin) {
    while (bin.data.size() % 4) bin.data.push_back(0);
    std::vector<uint8_t> json_bytes(gltf.begin(), gltf.end());
    while (json_bytes.size() % 4) json_bytes.push_back(' ');
    std::vector<uint8_t> out;
    uint32_t total = uint32_t(12 + 8 + json_bytes.size() + 8 + bin.data.size());
    auto w32 = [&](uint32_t value) {
        out.push_back(uint8_t(value));
        out.push_back(uint8_t(value >> 8));
        out.push_back(uint8_t(value >> 16));
        out.push_back(uint8_t(value >> 24));
    };
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    w32(2);
    w32(total);
    w32(uint32_t(json_bytes.size()));
    out.insert(out.end(), {'J', 'S', 'O', 'N'});
    out.insert(out.end(), json_bytes.begin(), json_bytes.end());
    w32(uint32_t(bin.data.size()));
    out.insert(out.end(), {'B', 'I', 'N', 0});
    out.insert(out.end(), bin.data.begin(), bin.data.end());
    return out;
}

}  // namespace

std::vector<uint8_t> build_scene_glb(const SceneInput& scene) {
    Bin bin;
    std::vector<std::string> buffer_images;
    std::vector<std::string> textures;
    std::vector<std::string> materials;
    std::vector<std::string> meshes;

    // Python appends embedded images before all mesh buffers.
    for (size_t i = 0; i < scene.images_png.size(); ++i) {
        const auto& png = scene.images_png[i];
        int bv = bin.add_bv(png.data(), png.size(), 0);
        buffer_images.push_back("{\"bufferView\":" + std::to_string(bv) +
                                ",\"mimeType\":\"image/png\",\"name\":\"texture_" +
                                std::to_string(i) + "\"}");
        textures.push_back("{\"sampler\":0,\"source\":" +
                           std::to_string(i) + "}");
    }

    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const SceneMaterial& material = scene.materials[i];
        std::string name = material.name.empty()
            ? "material_" + std::to_string(i) : material.name;
        std::string pbr = "{\"metallicFactor\":0.0,\"roughnessFactor\":0.8,";
        pbr += "\"baseColorFactor\":[";
        for (size_t c = 0; c < 4; ++c) {
            if (c) pbr += ',';
            pbr += pyfloat(material.base_color[c]);
        }
        pbr += ']';
        if (material.texture_idx >= 0 &&
            size_t(material.texture_idx) < textures.size()) {
            pbr += ",\"baseColorTexture\":{\"index\":" +
                   std::to_string(material.texture_idx) + "}";
        }
        pbr += '}';
        std::string out = "{\"name\":" + hier_esc(name) +
                          ",\"pbrMetallicRoughness\":" + pbr;
        if (!material.extras_json.empty())
            out += ",\"extras\":" + material.extras_json;
        if (material.base_color[3] < 1.0)
            out += ",\"alphaMode\":\"BLEND\"";
        out += '}';
        materials.push_back(std::move(out));
    }

    struct BuiltSkin {
        size_t mesh_index = 0;
        const SceneMesh* mesh = nullptr;
    };
    std::vector<BuiltSkin> built_skins;

    for (const SceneMesh& source : scene.meshes) {
        const GeoInfo& geo = source.geo;
        size_t nverts = geo.vertices.size() / 3;
        size_t nfaces = geo.faces.size() / 7;
        if (nverts == 0 || nfaces == 0) continue;
        bool have_norms = geo.normals.size() == geo.vertices.size();
        bool split = !geo.uvs.empty() && nfaces > 0;
        size_t nuvs = geo.uvs.size() / 2;

        struct V3 { float x, y, z; };
        std::vector<V3> verts, norms;
        std::vector<std::array<float, 2>> uv_per_vert;
        std::vector<uint32_t> orig;
        std::vector<std::array<uint32_t, 3>> faces;
        auto vert_of = [&](size_t vi) {
            if (vi < nverts)
                return V3{geo.vertices[vi * 3], geo.vertices[vi * 3 + 1],
                          geo.vertices[vi * 3 + 2]};
            return V3{0, 0, 0};
        };
        if (split) {
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> combo;
            for (size_t fi = 0; fi < nfaces; ++fi) {
                std::array<uint32_t, 3> face{};
                for (int c = 0; c < 3; ++c) {
                    uint32_t vi = geo.faces[fi * 7 + size_t(c)];
                    uint32_t ui = geo.faces[fi * 7 + 3 + size_t(c)];
                    auto key = std::make_pair(vi, ui);
                    auto found = combo.find(key);
                    uint32_t new_index = 0;
                    if (found == combo.end()) {
                        new_index = uint32_t(verts.size());
                        combo.emplace(key, new_index);
                        verts.push_back(vert_of(vi));
                        if (have_norms) {
                            if (vi < nverts)
                                norms.push_back({geo.normals[vi * 3],
                                                 geo.normals[vi * 3 + 1],
                                                 geo.normals[vi * 3 + 2]});
                            else
                                norms.push_back({0, 0, 1});
                        }
                        if (ui < nuvs)
                            uv_per_vert.push_back({geo.uvs[ui * 2],
                                                   geo.uvs[ui * 2 + 1]});
                        else
                            uv_per_vert.push_back({0, 0});
                        orig.push_back(vi);
                    } else {
                        new_index = found->second;
                    }
                    face[size_t(c)] = new_index;
                }
                faces.push_back(face);
            }
        } else {
            for (size_t vi = 0; vi < nverts; ++vi) {
                verts.push_back(vert_of(vi));
                if (have_norms)
                    norms.push_back({geo.normals[vi * 3], geo.normals[vi * 3 + 1],
                                     geo.normals[vi * 3 + 2]});
                orig.push_back(uint32_t(vi));
            }
            for (size_t fi = 0; fi < nfaces; ++fi)
                faces.push_back({geo.faces[fi * 7], geo.faces[fi * 7 + 1],
                                 geo.faces[fi * 7 + 2]});
        }
        for (V3& value : verts) value = {value.x, value.z, -value.y};
        for (V3& value : norms) value = {value.x, value.z, -value.y};

        std::vector<float> positions;
        positions.reserve(verts.size() * 3);
        float mn[3]{}, mx[3]{};
        for (size_t vi = 0; vi < verts.size(); ++vi) {
            float values[3] = {verts[vi].x, verts[vi].y, verts[vi].z};
            for (int c = 0; c < 3; ++c) {
                positions.push_back(values[c]);
                if (vi == 0) mn[c] = mx[c] = values[c];
                else {
                    mn[c] = std::min(mn[c], values[c]);
                    mx[c] = std::max(mx[c], values[c]);
                }
            }
        }
        auto vec3_json = [&](const float* value) {
            return "[" + pyfloat(value[0]) + "," + pyfloat(value[1]) + "," +
                   pyfloat(value[2]) + "]";
        };
        int pos_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(positions.data()),
                       positions.size() * sizeof(float), 34962),
            5126, verts.size(), "VEC3", vec3_json(mn), vec3_json(mx));
        int norm_acc = -1;
        if (!norms.empty() && norms.size() == verts.size()) {
            std::vector<float> values;
            values.reserve(norms.size() * 3);
            for (const V3& normal : norms) {
                values.push_back(normal.x);
                values.push_back(normal.y);
                values.push_back(normal.z);
            }
            norm_acc = bin.add_acc(
                bin.add_bv(reinterpret_cast<const uint8_t*>(values.data()),
                           values.size() * sizeof(float), 34962),
                5126, norms.size(), "VEC3");
        }
        int uv_acc = -1;
        if (split && uv_per_vert.size() == verts.size()) {
            std::vector<float> values;
            values.reserve(uv_per_vert.size() * 2);
            for (const auto& uv : uv_per_vert) {
                values.push_back(uv[0]);
                values.push_back(uv[1]);
            }
            uv_acc = bin.add_acc(
                bin.add_bv(reinterpret_cast<const uint8_t*>(values.data()),
                           values.size() * sizeof(float), 34962),
                5126, verts.size(), "VEC2");
        }
        std::vector<float> jade_uv(orig.size() * 2, 0.0f);
        for (size_t i = 0; i < orig.size(); ++i) jade_uv[i * 2] = float(orig[i]);
        int jade_uv_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(jade_uv.data()),
                       jade_uv.size() * sizeof(float), 34962),
            5126, orig.size(), "VEC2");

        int joints_acc = -1, weights_acc = -1;
        bool any_weights = false;
        if (geo.skin_present && !geo.skin_bones.empty()) {
            std::unordered_map<uint32_t,
                std::vector<std::pair<uint32_t, double>>> vertex_weights;
            for (size_t bi = 0; bi < geo.skin_bones.size(); ++bi) {
                for (const auto& weight : geo.skin_bones[bi].weights) {
                    vertex_weights[weight.first].push_back(
                        {uint32_t(bi), double(weight.second) / 65535.0});
                    any_weights = true;
                }
            }
            if (any_weights) {
                std::vector<uint8_t> joints(verts.size() * 4, 0);
                std::vector<float> weights(verts.size() * 4, 0.0f);
                for (size_t vi = 0; vi < verts.size(); ++vi) {
                    auto found = vertex_weights.find(orig[vi]);
                    if (found == vertex_weights.end()) continue;
                    auto influences = found->second;
                    std::stable_sort(influences.begin(), influences.end(),
                        [](const auto& a, const auto& b) { return a.second > b.second; });
                    if (influences.size() > 4) influences.resize(4);
                    double total = 0;
                    for (const auto& influence : influences) total += influence.second;
                    for (size_t ii = 0; ii < influences.size(); ++ii) {
                        joints[vi * 4 + ii] = uint8_t(
                            std::min<uint32_t>(influences[ii].first, 255));
                        weights[vi * 4 + ii] = total > 0
                            ? float(influences[ii].second / total) : 0.0f;
                    }
                }
                joints_acc = bin.add_acc(bin.add_bv(joints.data(), joints.size(), 34962),
                                         5121, verts.size(), "VEC4");
                weights_acc = bin.add_acc(
                    bin.add_bv(reinterpret_cast<const uint8_t*>(weights.data()),
                               weights.size() * sizeof(float), 34962),
                    5126, verts.size(), "VEC4");
            }
        }

        std::map<uint32_t, std::vector<size_t>> face_groups;
        for (size_t fi = 0; fi < faces.size(); ++fi) {
            uint32_t material = fi < source.material_indices.size()
                ? source.material_indices[fi] : 0;
            face_groups[material].push_back(fi);
        }
        std::vector<std::string> primitives;
        for (const auto& group : face_groups) {
            std::vector<uint32_t> indices;
            uint32_t imin = 0, imax = 0;
            bool first = true;
            for (size_t fi : group.second) {
                for (uint32_t index : faces[fi]) {
                    if (first) { imin = imax = index; first = false; }
                    imin = std::min(imin, index);
                    imax = std::max(imax, index);
                    indices.push_back(index);
                }
            }
            int component = imax <= 0xFFFF ? 5123 : 5125;
            std::vector<uint8_t> packed;
            packed.reserve(indices.size() * (component == 5123 ? 2 : 4));
            for (uint32_t index : indices) {
                packed.push_back(uint8_t(index));
                packed.push_back(uint8_t(index >> 8));
                if (component == 5125) {
                    packed.push_back(uint8_t(index >> 16));
                    packed.push_back(uint8_t(index >> 24));
                }
            }
            int index_acc = bin.add_acc(
                bin.add_bv(packed.data(), packed.size(), 34963), component,
                indices.size(), "SCALAR", "[" + std::to_string(imin) + "]",
                "[" + std::to_string(imax) + "]");
            std::string attributes = "{\"POSITION\":" + std::to_string(pos_acc);
            if (norm_acc >= 0) attributes += ",\"NORMAL\":" + std::to_string(norm_acc);
            if (uv_acc >= 0) attributes += ",\"TEXCOORD_0\":" + std::to_string(uv_acc);
            attributes += ",\"TEXCOORD_1\":" + std::to_string(jade_uv_acc);
            if (joints_acc >= 0)
                attributes += ",\"JOINTS_0\":" + std::to_string(joints_acc);
            if (weights_acc >= 0)
                attributes += ",\"WEIGHTS_0\":" + std::to_string(weights_acc);
            attributes += '}';
            std::string primitive = "{\"attributes\":" + attributes +
                ",\"indices\":" + std::to_string(index_acc) + ",\"mode\":4";
            if (group.first < materials.size())
                primitive += ",\"material\":" + std::to_string(group.first);
            primitive += '}';
            primitives.push_back(std::move(primitive));
        }
        std::string orig_json = "[";
        for (size_t i = 0; i < orig.size(); ++i) {
            if (i) orig_json += ',';
            orig_json += std::to_string(orig[i]);
        }
        orig_json += ']';
        std::string extras = scene_object_add(source.extras_json,
                                              "jade_orig_vert_map", orig_json);
        std::string mesh_name = source.name.empty()
            ? "mesh_" + std::to_string(meshes.size()) : source.name;
        meshes.push_back("{\"name\":" + hier_esc(mesh_name) +
                         ",\"primitives\":[" + scene_join(primitives) +
                         "],\"extras\":" + extras + "}");
        if (any_weights)
            built_skins.push_back({meshes.size() - 1, &source});
    }

    std::vector<SceneNode> source_nodes = scene.nodes;
    if (source_nodes.empty()) {
        for (size_t i = 0; i < scene.meshes.size(); ++i) {
            SceneNode node;
            node.name = scene.meshes[i].name.empty()
                ? "mesh_" + std::to_string(i) : scene.meshes[i].name;
            node.mesh_idx = int(i);
            node.extras_json = scene.meshes[i].extras_json;
            source_nodes.push_back(std::move(node));
        }
    }
    std::vector<std::string> nodes;
    std::unordered_map<uint32_t, size_t> bone_key_to_node;
    std::set<uint32_t> all_children;
    for (size_t i = 0; i < source_nodes.size(); ++i) {
        const SceneNode& node = source_nodes[i];
        std::string name = node.name.empty() ? "node_" + std::to_string(i) : node.name;
        std::string out = "{\"name\":" + hier_esc(name);
        if (node.mesh_idx >= 0 && size_t(node.mesh_idx) < meshes.size())
            out += ",\"mesh\":" + std::to_string(node.mesh_idx);
        if (!node.children.empty()) {
            out += ",\"children\":[";
            for (size_t ci = 0; ci < node.children.size(); ++ci) {
                if (ci) out += ',';
                out += std::to_string(node.children[ci]);
                all_children.insert(node.children[ci]);
            }
            out += ']';
        }
        if (node.has_matrix) {
            bool finite = true;
            for (double value : node.matrix)
                if (!std::isfinite(value)) finite = false;
            static const std::array<double, 16> identity{{
                1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
            out += ",\"matrix\":" + hier_mat16_json(
                finite ? hier_jade_to_gltf(node.matrix) : identity);
        }
        if (!node.extras_json.empty()) out += ",\"extras\":" + node.extras_json;
        out += '}';
        nodes.push_back(std::move(out));
        if (node.is_bone) bone_key_to_node[node.jade_key] = i;
    }
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < nodes.size(); ++i)
        if (!all_children.count(i)) roots.push_back(i);

    std::vector<std::string> skins;
    for (const BuiltSkin& built : built_skins) {
        const SceneMesh& source = *built.mesh;
        std::vector<uint32_t> joints;
        std::vector<float> matrices;
        for (const GeoBone& bone : source.geo.skin_bones) {
            size_t node_index = 0;
            bool have_node = false;
            if (bone.bone_idx < source.gizmo_gao_keys.size()) {
                auto found = bone_key_to_node.find(source.gizmo_gao_keys[bone.bone_idx]);
                if (found != bone_key_to_node.end()) {
                    node_index = found->second;
                    have_node = true;
                }
            }
            if (!have_node) {
                auto found = bone_key_to_node.find(bone.bone_idx);
                if (found != bone_key_to_node.end()) node_index = found->second;
            }
            joints.push_back(uint32_t(node_index));
            std::array<double, 16> jade_matrix{};
            bool finite = true;
            for (size_t i = 0; i < 16; ++i) {
                jade_matrix[i] = bone.bind_matrix[i];
                if (!std::isfinite(jade_matrix[i])) finite = false;
            }
            if (!finite)
                jade_matrix = {{1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1}};
            auto gltf_matrix = hier_jade_to_gltf(jade_matrix);
            const double fmax = 3.4028235e+38;
            for (double value : gltf_matrix)
                matrices.push_back(float(std::max(-fmax, std::min(fmax, value))));
        }
        int matrix_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(matrices.data()),
                       matrices.size() * sizeof(float), 0),
            5126, source.geo.skin_bones.size(), "MAT4");
        std::string skin = "{\"joints\":[";
        for (size_t i = 0; i < joints.size(); ++i) {
            if (i) skin += ',';
            skin += std::to_string(joints[i]);
        }
        skin += "],\"inverseBindMatrices\":" + std::to_string(matrix_acc);
        if (!roots.empty()) skin += ",\"skeleton\":" + std::to_string(roots[0]);
        skin += '}';
        size_t skin_index = skins.size();
        skins.push_back(std::move(skin));
        for (size_t ni = 0; ni < source_nodes.size(); ++ni) {
            if (source_nodes[ni].mesh_idx == int(built.mesh_index)) {
                nodes[ni] = scene_object_add(nodes[ni], "skin",
                                             std::to_string(skin_index));
                break;
            }
        }
    }

    std::string roots_json;
    for (uint32_t root : roots) {
        if (!roots_json.empty()) roots_json += ',';
        roots_json += std::to_string(root);
    }
    std::string gltf =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"JadeExplorer\"},"
        "\"scene\":0,\"scenes\":[{\"name\":" + hier_esc(scene.scene_name) +
        ",\"nodes\":[" + roots_json + "]";
    if (!scene.extras_json.empty()) gltf += ",\"extras\":" + scene.extras_json;
    gltf += "}]";
    if (!nodes.empty()) gltf += ",\"nodes\":[" + scene_join(nodes) + ']';
    if (!meshes.empty()) gltf += ",\"meshes\":[" + scene_join(meshes) + ']';
    if (!bin.accessors.empty())
        gltf += ",\"accessors\":[" + scene_join(bin.accessors) + ']';
    if (!bin.buffer_views.empty())
        gltf += ",\"bufferViews\":[" + scene_join(bin.buffer_views) + ']';
    if (!materials.empty()) gltf += ",\"materials\":[" + scene_join(materials) + ']';
    if (!textures.empty()) gltf += ",\"textures\":[" + scene_join(textures) + ']';
    if (!buffer_images.empty())
        gltf += ",\"images\":[" + scene_join(buffer_images) + ']';
    if (!buffer_images.empty())
        gltf += ",\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,"
                "\"wrapS\":10497,\"wrapT\":10497}]";
    if (!skins.empty()) gltf += ",\"skins\":[" + scene_join(skins) + ']';
    // Python pads bin_data before recording byteLength.
    while (bin.data.size() % 4) bin.data.push_back(0);
    gltf += ",\"buffers\":[{\"byteLength\":" +
            std::to_string(bin.data.size()) + "}]}";
    return scene_finish_glb(gltf, bin);
}

HierExportResult build_hierarchical_geo_glb(const std::vector<SubEntry>& subs,
                                            uint32_t geo_key) {
    HierExportResult res;
    char eb[160];
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;

    const SubEntry* geo_sub = pick_geo_sub(subs, geo_key);
    GeoInfo geo;
    if (geo_sub != nullptr)
        geo = parse_geometry(geo_sub->data.data(), geo_sub->data.size());
    if (geo_sub == nullptr || !geo.ok || geo.vertices.empty() ||
        geo.faces.empty()) {
        std::snprintf(eb, sizeof eb,
                      "geo 0x%08x produced no drawable mesh in entry (empty "
                      "or unparsed)", geo_key);
        res.error = eb;
        return res;
    }
    if (!geo.skin_present || geo.skin_bones.empty()) {
        std::snprintf(eb, sizeof eb,
                      "geo 0x%08x is not skinned \xe2\x80\x94 use the "
                      "standard 'Export to GLB' for static meshes (there is "
                      "no bone hierarchy to emit).", geo_key);
        res.error = eb;
        return res;
    }
    const auto& bones = geo.skin_bones;
    size_t nbones = bones.size();

    // Owning GAO -> gizmo pairs + mesh name (scene_export geo_owner_name).
    const SubEntry* host = host_gao_sub(subs, geo_key);
    std::vector<uint32_t> gp;              // gao_key per gizmo slot
    std::string mesh_name;
    if (host != nullptr) {
        GaoInfo hi = parse_gao_full(host->data.data(), host->data.size());
        if (hi.ok) {
            for (size_t i = 0; i + 1 < hi.gizmo_flat.size(); i += 2)
                gp.push_back(hi.gizmo_flat[i]);
            std::string clean = hier_sanitize(hi.name);
            if (!clean.empty() && hi.vis_read && hi.gro_key != 0 &&
                hi.gro_key != 0xFFFFFFFFu) {
                std::unordered_set<uint32_t> geo_keys_all;
                for (const SubEntry& s : subs)
                    if (!s.gro_null && s.gro_type == 1)
                        geo_keys_all.insert(s.key);
                std::vector<uint32_t> members = geo_group_members(
                    hi.gro_key, by_key, &geo_keys_all);
                if (members.size() > 1) {
                    for (size_t mi = 0; mi < members.size(); ++mi)
                        if (members[mi] == geo_key) {
                            mesh_name = clean + "_" + std::to_string(mi);
                            break;
                        }
                } else {
                    mesh_name = clean;
                }
            }
        }
    }
    if (mesh_name.empty()) {
        std::snprintf(eb, sizeof eb, "mesh_0x%08X", geo_key);
        mesh_name = eb;
    }

    // Bone forest (father_key tree) + key -> node map + parent map.
    BoneForest forest = build_bone_nodes(subs);
    std::unordered_map<uint32_t, size_t> key_to_node;
    std::unordered_map<size_t, size_t> parent_of;
    if (forest.ok) {
        for (size_t ni = 0; ni < forest.nodes.size(); ++ni)
            key_to_node.emplace(forest.nodes[ni].key, ni);
        for (size_t ni = 0; ni < forest.nodes.size(); ++ni)
            for (uint32_t c : forest.nodes[ni].children)
                parent_of[size_t(c)] = ni;
    }

    // Per skin bone: gao key + full-tree node.
    std::vector<bool> has_gk(nbones, false), has_ni(nbones, false);
    std::vector<uint32_t> bone_gk(nbones, 0);
    std::vector<size_t> bone_ni(nbones, 0);
    std::unordered_map<size_t, size_t> full_ni_to_bone;
    for (size_t i = 0; i < nbones; ++i) {
        int bi = bones[i].bone_idx;
        if (bi >= 0 && size_t(bi) < gp.size()) {
            has_gk[i] = true;
            bone_gk[i] = gp[size_t(bi)];
            auto it = key_to_node.find(bone_gk[i]);
            if (it != key_to_node.end()) {
                has_ni[i] = true;
                bone_ni[i] = it->second;
            }
        }
        if (has_ni[i]) full_ni_to_bone[bone_ni[i]] = i;
    }
    auto nearest_skin_parent = [&](size_t bone_i, size_t& out) {
        if (!has_ni[bone_i]) return false;
        auto cur = parent_of.find(bone_ni[bone_i]);
        while (cur != parent_of.end()) {
            auto hit = full_ni_to_bone.find(cur->second);
            if (hit != full_ni_to_bone.end()) {
                out = hit->second;
                return true;
            }
            cur = parent_of.find(cur->second);
        }
        return false;
    };

    auto ibm_jade = [&](size_t i) {
        std::array<double, 16> m{};
        bool finite = true;
        for (int k = 0; k < 16; ++k) {
            m[size_t(k)] = double(bones[i].bind_matrix[size_t(k)]);
            if (!std::isfinite(m[size_t(k)])) finite = false;
        }
        if (!finite)
            m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        return m;
    };
    std::vector<std::array<double, 16>> inv_ibm(nbones);
    for (size_t i = 0; i < nbones; ++i)
        if (!hier_inv4(ibm_jade(i), inv_ibm[i]))
            inv_ibm[i] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    std::vector<bool> has_parent(nbones, false);
    std::vector<size_t> skin_parent(nbones, 0);
    for (size_t i = 0; i < nbones; ++i)
        has_parent[i] = nearest_skin_parent(i, skin_parent[i]);

    // Node JSON list: one per skin bone, then the mesh node.
    struct NodeJ {
        std::string name;
        bool has_matrix = false;
        std::array<double, 16> matrix{};
        std::vector<size_t> children;
        std::string extras;
    };
    std::vector<NodeJ> nodes(nbones + 1);
    for (size_t i = 0; i < nbones; ++i) {
        NodeJ& n = nodes[i];
        n.has_matrix = true;
        n.matrix = has_parent[i]
                       ? hier_matmul(ibm_jade(skin_parent[i]), inv_ibm[i])
                       : inv_ibm[i];
        if (has_ni[i] && !forest.nodes[bone_ni[i]].name.empty()) {
            n.name = forest.nodes[bone_ni[i]].name;
        } else {
            std::snprintf(eb, sizeof eb, "bone_%d",
                          has_ni[i] ? int(bones[i].bone_idx)
                                    : int(bones[i].bone_idx));
            n.name = eb;
        }
        n.extras = "{\"jade_type\":\"bone\",\"bone_idx\":" +
                   std::to_string(int(bones[i].bone_idx));
        if (has_gk[i]) {
            std::snprintf(eb, sizeof eb, "0x%08X", bone_gk[i]);
            n.extras += ",\"jade_key\":\"" + std::string(eb) + "\"";
        }
        n.extras += "}";
    }
    for (size_t i = 0; i < nbones; ++i)
        if (has_parent[i]) nodes[skin_parent[i]].children.push_back(i);
    {
        NodeJ& mn = nodes[nbones];
        mn.name = mesh_name;
        std::snprintf(eb, sizeof eb, "0x%08X", geo_key);
        mn.extras = "{\"jade_key\":\"" + std::string(eb) + "\"}";
    }

    // In-bin material rows (resolve_mesh_materials' back-compat path).
    uint32_t grm = 0;
    bool have_grm = owning_grm_key(subs, geo_key, grm);
    std::vector<uint32_t> sub_keys;
    if (have_grm) {
        auto it = by_key.find(grm);
        sub_keys = grm_sub_material_keys(it == by_key.end() ? nullptr
                                                            : it->second);
    }
    size_t n_elems = geo.elements.size() / 2;
    std::vector<std::string> materials_j, images_png;
    std::map<uint32_t, int> img_by_tex;      // texture key -> image idx | -1
    std::vector<int> elem_to_mat(n_elems, 0);
    for (size_t ei = 0; ei < n_elems; ++ei) {
        uint32_t mid = geo.elements[ei * 2 + 1];
        int64_t slot = clamp_matid(int64_t(mid), int64_t(sub_keys.size()));
        uint32_t matk = 0;
        if (slot >= 0 && size_t(slot) < sub_keys.size())
            matk = sub_keys[size_t(slot)];
        if (matk == 0xFFFFFFFFu) matk = 0;
        uint32_t texk = 0;
        if (matk != 0) {
            auto mit = by_key.find(matk);
            if (mit != by_key.end())
                texk = resolve_texture_key(mit->second->data.data(),
                                           mit->second->data.size());
        }
        std::string nm = "elem" + std::to_string(ei) + "_matId" +
                         std::to_string(mid);
        if (matk != 0) {
            std::snprintf(eb, sizeof eb, "_0x%08X", matk);
            nm += eb;
        }
        std::string gm = "{\"name\":" + hier_esc(nm) +
                         ",\"pbrMetallicRoughness\":{\"metallicFactor\":0.0,"
                         "\"roughnessFactor\":0.8,\"baseColorFactor\":[0.8,"
                         "0.8,0.8,1.0]";
        int tex_idx = -1;
        if (texk != 0) {
            auto tit = img_by_tex.find(texk);
            if (tit == img_by_tex.end()) {
                img_by_tex[texk] = -1;
                // decode_texture_preview: pick the LARGEST-pixdata real
                // occurrence, resolve a palette for PAL8, decode to RGBA.
                const SubEntry* best = nullptr;
                TexInfo best_ti;
                size_t best_pix = 0;
                for (const SubEntry& s : subs) {
                    if (s.key != texk || s.data.empty() ||
                        !is_texture_entry(s.data.data(), s.data.size()))
                        continue;
                    TexInfo ti = parse_texture(s.data.data(), s.data.size());
                    if (!ti.valid) continue;
                    size_t pix = s.data.size() > ti.pix_start
                                     ? s.data.size() - ti.pix_start : 0;
                    if (best == nullptr || pix > best_pix) {
                        best = &s;
                        best_ti = ti;
                        best_pix = pix;
                    }
                }
                if (best != nullptr) {
                    const std::vector<uint8_t>* pal =
                        palette_for_texture(best_ti, subs);
                    std::vector<uint8_t> rgba = decode_texture(
                        best->data.data(), best->data.size(), best_ti,
                        pal != nullptr ? pal->data() : nullptr,
                        pal != nullptr ? pal->size() : 0);
                    if (!rgba.empty() &&
                        rgba.size() ==
                            size_t(best_ti.width) * best_ti.height * 4) {
                        img_by_tex[texk] = int(images_png.size());
                        auto png = levelblend::png_encode_rgba_pub(
                            rgba.data(), best_ti.width, best_ti.height);
                        images_png.push_back(
                            std::string(png.begin(), png.end()));
                    }
                }
            }
            tex_idx = img_by_tex[texk];
        }
        if (tex_idx >= 0)
            gm += ",\"baseColorTexture\":{\"index\":" +
                  std::to_string(tex_idx) + "}";
        gm += "},\"extras\":{\"jade_matId\":" + std::to_string(mid) +
              ",\"jade_element_index\":" + std::to_string(ei);
        if (matk != 0) {
            std::snprintf(eb, sizeof eb, "0x%08X", matk);
            gm += ",\"jade_key\":\"" + std::string(eb) + "\"";
        }
        gm += "}}";
        elem_to_mat[ei] = int(materials_j.size());
        materials_j.push_back(std::move(gm));
    }

    // ── mesh emission (build_glb, with per-element material grouping) ──
    Bin bin;
    // Image bufferViews first (Python adds images before mesh data).
    std::vector<int> img_bvs;
    for (const std::string& png : images_png)
        img_bvs.push_back(bin.add_bv(
            reinterpret_cast<const uint8_t*>(png.data()), png.size(), 0));

    size_t nverts0 = geo.vertices.size() / 3;
    size_t nfaces = geo.faces.size() / 7;
    size_t nuvs = geo.uvs.size() / 2;
    bool have_norms = geo.normals.size() == geo.vertices.size();
    bool have_uvs = !geo.uvs.empty();
    struct V3f { float x, y, z; };
    std::vector<V3f> verts, norms;
    std::vector<std::array<float, 2>> uv_per_vert;
    std::vector<uint32_t> orig_vert_indices;
    std::vector<std::array<uint32_t, 3>> faces;
    auto vert_of = [&](size_t vi) {
        if (vi < nverts0)
            return V3f{geo.vertices[vi * 3], geo.vertices[vi * 3 + 1],
                       geo.vertices[vi * 3 + 2]};
        return V3f{0, 0, 0};
    };
    bool split = have_uvs && nfaces > 0;
    if (split) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> combo;
        for (size_t fi = 0; fi < nfaces; ++fi) {
            std::array<uint32_t, 3> nf{};
            for (int c = 0; c < 3; ++c) {
                uint32_t vi = geo.faces[fi * 7 + size_t(c)];
                uint32_t ui = geo.faces[fi * 7 + 3 + size_t(c)];
                auto key = std::make_pair(vi, ui);
                auto it = combo.find(key);
                uint32_t ni;
                if (it == combo.end()) {
                    ni = uint32_t(verts.size());
                    combo[key] = ni;
                    verts.push_back(vert_of(vi));
                    if (have_norms) {
                        if (vi < nverts0)
                            norms.push_back({geo.normals[vi * 3],
                                             geo.normals[vi * 3 + 1],
                                             geo.normals[vi * 3 + 2]});
                        else
                            norms.push_back({0, 0, 1});
                    }
                    if (ui < nuvs)
                        uv_per_vert.push_back(
                            {geo.uvs[ui * 2], geo.uvs[ui * 2 + 1]});
                    else
                        uv_per_vert.push_back({0.0f, 0.0f});
                    orig_vert_indices.push_back(vi);
                } else {
                    ni = it->second;
                }
                nf[size_t(c)] = ni;
            }
            faces.push_back(nf);
        }
    } else {
        for (size_t vi = 0; vi < nverts0; ++vi) {
            verts.push_back(vert_of(vi));
            if (have_norms)
                norms.push_back({geo.normals[vi * 3],
                                 geo.normals[vi * 3 + 1],
                                 geo.normals[vi * 3 + 2]});
            orig_vert_indices.push_back(uint32_t(vi));
        }
        for (size_t fi = 0; fi < nfaces; ++fi)
            faces.push_back({geo.faces[fi * 7], geo.faces[fi * 7 + 1],
                             geo.faces[fi * 7 + 2]});
    }
    for (V3f& v : verts) v = {v.x, v.z, -v.y};
    for (V3f& n : norms) n = {n.x, n.z, -n.y};

    std::vector<float> pf;
    pf.reserve(verts.size() * 3);
    float mn3[3] = {0, 0, 0}, mx3[3] = {0, 0, 0};
    for (size_t i = 0; i < verts.size(); ++i) {
        float c[3] = {verts[i].x, verts[i].y, verts[i].z};
        for (int k = 0; k < 3; ++k) {
            pf.push_back(c[k]);
            if (i == 0) { mn3[k] = mx3[k] = c[k]; }
            else {
                mn3[k] = std::min(mn3[k], c[k]);
                mx3[k] = std::max(mx3[k], c[k]);
            }
        }
    }
    auto list3f = [&](const float* v) {
        return "[" + pyfloat(double(v[0])) + "," + pyfloat(double(v[1])) +
               "," + pyfloat(double(v[2])) + "]";
    };
    int pos_acc = bin.add_acc(
        bin.add_bv(reinterpret_cast<const uint8_t*>(pf.data()),
                   pf.size() * 4, 34962),
        5126, verts.size(), "VEC3", list3f(mn3), list3f(mx3));
    int norm_acc = -1;
    if (!norms.empty() && norms.size() == verts.size()) {
        std::vector<float> nf2;
        for (const V3f& n : norms) {
            nf2.push_back(n.x);
            nf2.push_back(n.y);
            nf2.push_back(n.z);
        }
        norm_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(nf2.data()),
                       nf2.size() * 4, 34962),
            5126, norms.size(), "VEC3");
    }
    int uv_acc = -1;
    if (split && uv_per_vert.size() == verts.size()) {
        std::vector<float> uf;
        for (const auto& u : uv_per_vert) {
            uf.push_back(u[0]);
            uf.push_back(u[1]);
        }
        uv_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(uf.data()),
                       uf.size() * 4, 34962),
            5126, verts.size(), "VEC2");
    }
    int jade_uv1_acc = -1;
    {
        std::vector<float> jf(verts.size() * 2, 0.0f);
        for (size_t i = 0; i < verts.size(); ++i)
            jf[i * 2] = float(orig_vert_indices[i]);
        jade_uv1_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(jf.data()),
                       jf.size() * 4, 34962),
            5126, verts.size(), "VEC2");
    }
    int joints_acc = -1, weights_acc = -1;
    bool any_weights = false;
    {
        std::unordered_map<uint32_t,
                           std::vector<std::pair<uint32_t, double>>> vw;
        for (size_t bi = 0; bi < nbones; ++bi)
            for (const auto& w : bones[bi].weights) {
                vw[w.first].push_back(
                    {uint32_t(bi), double(w.second) / 65535.0});
                any_weights = true;
            }
        if (any_weights) {
            std::vector<uint8_t> joints(verts.size() * 4, 0);
            std::vector<float> weights(verts.size() * 4, 0.0f);
            for (size_t nv = 0; nv < verts.size(); ++nv) {
                auto it = vw.find(orig_vert_indices[nv]);
                if (it == vw.end()) continue;
                auto infl = it->second;
                std::stable_sort(infl.begin(), infl.end(),
                                 [](const auto& a, const auto& b) {
                                     return a.second > b.second;
                                 });
                if (infl.size() > 4) infl.resize(4);
                double total = 0;
                for (const auto& iw : infl) total += iw.second;
                for (size_t ii = 0; ii < infl.size(); ++ii) {
                    joints[nv * 4 + ii] =
                        uint8_t(std::min<uint32_t>(infl[ii].first, 255));
                    weights[nv * 4 + ii] =
                        total > 0 ? float(infl[ii].second / total) : 0.0f;
                }
            }
            joints_acc = bin.add_acc(
                bin.add_bv(joints.data(), joints.size(), 34962), 5121,
                verts.size(), "VEC4");
            weights_acc = bin.add_acc(
                bin.add_bv(reinterpret_cast<const uint8_t*>(weights.data()),
                           weights.size() * 4, 34962),
                5126, verts.size(), "VEC4");
        }
    }

    // Face groups by material key (sorted, like Python's sorted(face_groups)).
    std::map<int, std::vector<size_t>> face_groups;
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        uint32_t elem = fi < nfaces ? geo.faces[fi * 7 + 6] : 0;
        int mk = elem < elem_to_mat.size() ? elem_to_mat[elem] : 0;
        face_groups[mk].push_back(fi);
    }
    std::string prims_j;
    for (const auto& fg : face_groups) {
        std::vector<uint32_t> indices;
        uint32_t imin = 0, imax = 0;
        bool first = true;
        for (size_t fi : fg.second)
            for (int c = 0; c < 3; ++c) {
                uint32_t ix = faces[fi][size_t(c)];
                if (first) { imin = imax = ix; first = false; }
                imin = std::min(imin, ix);
                imax = std::max(imax, ix);
                indices.push_back(ix);
            }
        std::vector<uint8_t> ib;
        int ct = imax <= 0xFFFF ? 5123 : 5125;
        for (uint32_t ix : indices) {
            ib.push_back(uint8_t(ix));
            ib.push_back(uint8_t(ix >> 8));
            if (ct == 5125) {
                ib.push_back(uint8_t(ix >> 16));
                ib.push_back(uint8_t(ix >> 24));
            }
        }
        int idx_acc = bin.add_acc(bin.add_bv(ib.data(), ib.size(), 34963),
                                  ct, indices.size(), "SCALAR",
                                  "[" + std::to_string(imin) + "]",
                                  "[" + std::to_string(imax) + "]");
        std::string attrs = "\"POSITION\":" + std::to_string(pos_acc);
        if (norm_acc >= 0) attrs += ",\"NORMAL\":" + std::to_string(norm_acc);
        if (uv_acc >= 0)
            attrs += ",\"TEXCOORD_0\":" + std::to_string(uv_acc);
        attrs += ",\"TEXCOORD_1\":" + std::to_string(jade_uv1_acc);
        if (joints_acc >= 0)
            attrs += ",\"JOINTS_0\":" + std::to_string(joints_acc);
        if (weights_acc >= 0)
            attrs += ",\"WEIGHTS_0\":" + std::to_string(weights_acc);
        std::string prim = "{\"attributes\":{" + attrs + "},\"indices\":" +
                           std::to_string(idx_acc) + ",\"mode\":4";
        if (fg.first >= 0 && size_t(fg.first) < materials_j.size())
            prim += ",\"material\":" + std::to_string(fg.first);
        prim += "}";
        if (!prims_j.empty()) prims_j += ",";
        prims_j += prim;
    }
    std::string ovm;
    for (uint32_t v : orig_vert_indices) {
        if (!ovm.empty()) ovm += ",";
        ovm += std::to_string(v);
    }
    std::snprintf(eb, sizeof eb, "0x%08X", geo_key);
    std::string jade_key_hex = eb;
    std::string mesh_j = "{\"name\":" + hier_esc(mesh_name) +
                         ",\"primitives\":[" + prims_j +
                         "],\"extras\":{\"jade_key\":" +
                         hier_esc(jade_key_hex) +
                         ",\"jade_orig_vert_map\":[" + ovm + "]}}";

    // Roots = nodes that are nobody's child (bone roots + the mesh node).
    std::vector<size_t> roots;
    {
        std::set<size_t> all_children;
        for (const NodeJ& n : nodes)
            for (size_t c : n.children) all_children.insert(c);
        for (size_t i = 0; i < nodes.size(); ++i)
            if (!all_children.count(i)) roots.push_back(i);
    }

    // Skin: joints via bone jade_key -> node (bones emit in order => i).
    std::string skins_j;
    if (any_weights) {
        std::vector<float> ibm;
        for (size_t i = 0; i < nbones; ++i) {
            std::array<double, 16> g = hier_jade_to_gltf(ibm_jade(i));
            const double fmax = 3.4028235e+38;
            for (int k = 0; k < 16; ++k)
                ibm.push_back(
                    float(std::max(-fmax, std::min(fmax, g[size_t(k)]))));
        }
        int ibm_acc = bin.add_acc(
            bin.add_bv(reinterpret_cast<const uint8_t*>(ibm.data()),
                       ibm.size() * 4, 0),
            5126, nbones, "MAT4");
        std::string joints;
        for (size_t i = 0; i < nbones; ++i) {
            if (!joints.empty()) joints += ",";
            // Python: bone_key_to_node via jade_key extras; bones with no
            // gao key fall back to bone_key_to_node.get(bone_idx, 0) -> 0.
            joints += std::to_string(has_gk[i] ? i : 0);
        }
        skins_j = "{\"joints\":[" + joints + "],\"inverseBindMatrices\":" +
                  std::to_string(ibm_acc);
        if (!roots.empty())
            skins_j += ",\"skeleton\":" + std::to_string(roots[0]);
        skins_j += "}";
    }

    // Node JSON (Python order: name, mesh, children, matrix, extras[, skin]).
    std::vector<std::string> nodes_j;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const NodeJ& n = nodes[i];
        std::string j = "{\"name\":" + hier_esc(n.name);
        if (i == nbones) j += ",\"mesh\":0";
        if (!n.children.empty()) {
            j += ",\"children\":[";
            for (size_t c = 0; c < n.children.size(); ++c) {
                if (c) j += ",";
                j += std::to_string(n.children[c]);
            }
            j += "]";
        }
        if (n.has_matrix)
            j += ",\"matrix\":" +
                 hier_mat16_json(hier_jade_to_gltf(n.matrix));
        j += ",\"extras\":" + n.extras;
        if (i == nbones && !skins_j.empty()) j += ",\"skin\":0";
        j += "}";
        nodes_j.push_back(std::move(j));
    }

    auto join = [](const std::vector<std::string>& v) {
        std::string o;
        for (const std::string& s : v) {
            if (!o.empty()) o += ",";
            o += s;
        }
        return o;
    };
    std::string roots_j;
    for (size_t r : roots) {
        if (!roots_j.empty()) roots_j += ",";
        roots_j += std::to_string(r);
    }
    while (bin.data.size() % 4) bin.data.push_back(0);
    std::string scene_name = "geo_" + jade_key_hex;
    std::string gltf =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"JadeExplorer\"},"
        "\"scene\":0,\"scenes\":[{\"name\":" + hier_esc(scene_name) +
        ",\"nodes\":[" + roots_j + "],\"extras\":{\"jade_source\":" +
        hier_esc(jade_key_hex) + ",\"export\":\"hierarchical\"}}],"
        "\"nodes\":[" + join(nodes_j) + "],\"meshes\":[" + mesh_j +
        "],\"accessors\":[" + join(bin.accessors) + "],\"bufferViews\":[" +
        join(bin.buffer_views) + "]";
    if (!materials_j.empty())
        gltf += ",\"materials\":[" + join(materials_j) + "]";
    if (!images_png.empty()) {
        std::string texs, imgs;
        for (size_t i = 0; i < images_png.size(); ++i) {
            if (i) { texs += ","; imgs += ","; }
            texs += "{\"sampler\":0,\"source\":" + std::to_string(i) + "}";
            imgs += "{\"bufferView\":" + std::to_string(img_bvs[i]) +
                    ",\"mimeType\":\"image/png\",\"name\":\"texture_" +
                    std::to_string(i) + "\"}";
        }
        gltf += ",\"textures\":[" + texs + "],\"images\":[" + imgs +
                "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,"
                "\"wrapS\":10497,\"wrapT\":10497}]";
    }
    if (!skins_j.empty()) gltf += ",\"skins\":[" + skins_j + "]";
    gltf += ",\"buffers\":[{\"byteLength\":" +
            std::to_string(bin.data.size()) + "}]}";

    std::vector<uint8_t> json_bytes(gltf.begin(), gltf.end());
    while (json_bytes.size() % 4) json_bytes.push_back(' ');
    std::vector<uint8_t> out;
    uint32_t total = uint32_t(12 + 8 + json_bytes.size() + 8 +
                              bin.data.size());
    auto w32 = [&](uint32_t v) {
        out.push_back(uint8_t(v));
        out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >> 24));
    };
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    w32(2);
    w32(total);
    w32(uint32_t(json_bytes.size()));
    out.insert(out.end(), {'J', 'S', 'O', 'N'});
    out.insert(out.end(), json_bytes.begin(), json_bytes.end());
    w32(uint32_t(bin.data.size()));
    out.insert(out.end(), {'B', 'I', 'N', 0});
    out.insert(out.end(), bin.data.begin(), bin.data.end());

    res.ok = true;
    res.glb = std::move(out);
    res.points = uint32_t(nverts0);
    res.tris = uint32_t(nfaces);
    res.bones = uint32_t(nbones);
    uint32_t nroots = 0;
    for (size_t i = 0; i < nbones; ++i)
        if (!has_parent[i]) ++nroots;
    res.roots = nroots;
    return res;
}

}  // namespace gltfbuild
}  // namespace jade
