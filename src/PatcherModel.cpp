// PatcherModel.cpp — port of io_ops/patcher.py's MODEL half (see the header).
//
// FP-determinism notes (hard-won — see the project memory):
//  * CPython round(x, n) is decimal rounding — reproduced with
//    snprintf("%.*f") + strtod (the MeshSwap _qpos trick).
//  * CPython round(x) / int(round(x)) is half-to-even — std::nearbyint under
//    the default rounding mode.
//  * numpy matrix inverses are AVOIDED in the oracle for byte-visible paths:
//    patcher.py uses _inv3x3_pp / _inv4x4_pp (deterministic partial-pivot
//    Gauss-Jordan), mirrored here operation-for-operation.
//  * The vertex world-bake in _parse_glb_meshes is explicit scalar
//    arithmetic in Python (no numpy matmul), so plain left-to-right sums
//    match bit-for-bit.
#include "jade/PatcherModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>

#include "jade/Gao.hpp"
#include "jade/Gltf.hpp"
#include "jade/Json.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/Rli.hpp"

namespace jade {
namespace patcher {

namespace {

// ── shared little helpers ──────────────────────────────────────────────────

double pyround_dec(double x, int nd) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", nd, x);
    return std::strtod(b, nullptr);
}

// CPython round-half-even.
double pyround(double x) { return std::nearbyint(x); }

// Fold -0.0 into +0.0 so map keys unify like Python dict keys.
double zfold(double x) { return x == 0.0 ? 0.0 : x; }

std::string key3(double a, double b, double c) {
    double v[3] = {zfold(a), zfold(b), zfold(c)};
    return std::string(reinterpret_cast<const char*>(v), 24);
}
std::string key2(double a, double b) {
    double v[2] = {zfold(a), zfold(b)};
    return std::string(reinterpret_cast<const char*>(v), 16);
}

void put_u16v(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void put_u32v(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put_i32v(std::vector<uint8_t>& v, int32_t x) {
    put_u32v(v, uint32_t(x));
}
void put_f32v(std::vector<uint8_t>& v, double d) {
    float f = float(d);
    uint32_t b;
    std::memcpy(&b, &f, 4);
    put_u32v(v, b);
}
uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
void matput32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + size_t(i)] = uint8_t(x >> (8 * i));
}
uint16_t rd_u16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}
float rd_f32(const uint8_t* p) {
    float f;
    uint32_t b = rd_u32(p);
    std::memcpy(&f, &b, 4);
    return f;
}

char logbuf[512];
#define LOGF(...)                                            \
    do {                                                     \
        std::snprintf(logbuf, sizeof logbuf, __VA_ARGS__);   \
        log.push_back(logbuf);                               \
    } while (0)

// ── typed accessor reads (the Python local read_accessor's semantics) ──────

// One element as doubles (converted per component type; u8/u16 weights etc.
// are scaled by the CALLER exactly like the Python).
std::vector<std::vector<double>> acc_doubles(const gltf::GlbDoc& doc, int idx) {
    gltf::AccessorData a = gltf::read_accessor(doc, idx);
    size_t cs = gltf::component_size(a.comp_type);
    std::vector<std::vector<double>> out(a.count);
    const uint8_t* p = a.raw.data();
    for (uint32_t i = 0; i < a.count; ++i) {
        std::vector<double>& e = out[i];
        e.resize(a.n_comps);
        for (uint32_t c = 0; c < a.n_comps; ++c) {
            const uint8_t* q = p + (size_t(i) * a.n_comps + c) * cs;
            switch (a.comp_type) {
                case gltf::COMP_I8:  e[c] = double(int8_t(q[0])); break;
                case gltf::COMP_U8:  e[c] = double(q[0]); break;
                case gltf::COMP_I16: e[c] = double(int16_t(rd_u16(q))); break;
                case gltf::COMP_U16: e[c] = double(rd_u16(q)); break;
                case gltf::COMP_U32: e[c] = double(rd_u32(q)); break;
                default:             e[c] = double(rd_f32(q)); break;
            }
        }
    }
    return out;
}

// ── deterministic 3x3 inverse (mirrors patcher._inv3x3_pp) ─────────────────

// Partial-pivot Gauss-Jordan on the row-major 3x3. false = singular.
bool inv3x3_pp(const double M[3][3], double out[3][3]) {
    double a[3][6];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            a[r][c] = M[r][c];
            a[r][c + 3] = (r == c) ? 1.0 : 0.0;
        }
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (a[piv][col] == 0.0) return false;
        if (piv != col)
            for (int c = 0; c < 6; ++c) std::swap(a[piv][c], a[col][c]);
        double d = a[col][col];
        for (int c = 0; c < 6; ++c) a[col][c] /= d;
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            double f = a[r][col];
            if (f == 0.0) continue;
            for (int c = 0; c < 6; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r][c] = a[r][c + 3];
    return true;
}

// ── bone-name normalisation (+ user alias file) ────────────────────────────

bool is_part_token(const std::string& t) {
    static const std::set<std::string> kParts = {
        "hand", "forearm", "upperarm", "arm", "clavicle", "shoulder",
        "elbow", "wrist", "finger", "thumb", "digit",
        "thigh", "calf", "leg", "foot", "toe", "knee", "ankle", "hip",
        "heel",
        "spine", "pelvis", "neck", "head", "skull", "jaw", "chest", "rib",
        "tail", "butt", "root",
        "eye", "eyelid", "cheek", "mouth", "tongue", "langue", "brow",
        "eyebrow", "nose", "ear", "lip", "tooth", "teeth", "chin",
        "forehead",
        "ponytail", "hair", "dress", "cape", "breast", "belt", "scarf",
        "cloth", "horn", "wing", "skirt"};
    return kParts.count(t) != 0;
}

// _QUALIFIER_MAP: returns the canonical single letter, or "" when not one.
std::string qualifier_of(const std::string& t) {
    if (t == "l" || t == "left") return "l";
    if (t == "r" || t == "right") return "r";
    if (t == "b" || t == "back") return "b";
    if (t == "f" || t == "front") return "f";
    return "";
}

const std::map<std::string, std::string>& load_bone_aliases() {
    static std::map<std::string, std::string> aliases;
    static bool loaded = false;
    if (loaded) return aliases;
    loaded = true;
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr) home = std::getenv("HOME");
    if (home == nullptr) return aliases;
    std::string path = std::string(home) + "/.jade_explorer/bone_aliases.json";
    std::ifstream f(path, std::ios::binary);
    if (!f) return aliases;
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    try {
        json::Value v = json::parse(text);
        const json::Value* raw = v.find("norm_aliases");
        if (raw != nullptr && raw->type == json::Value::Type::Object)
            for (const auto& kv : raw->obj)
                if (kv.second.is_str()) {
                    std::string k = kv.first, val = kv.second.str;
                    for (char& c : k) c = char(std::tolower(uint8_t(c)));
                    for (char& c : val) c = char(std::tolower(uint8_t(c)));
                    aliases[k] = val;
                }
    } catch (...) {
        aliases.clear();
    }
    return aliases;
}

bool fullmatch_bip(const std::string& t) {
    // re.fullmatch(r'bip[0-9]*|biped')
    if (t == "biped") return true;
    if (t.size() < 3 || t.compare(0, 3, "bip") != 0) return false;
    for (size_t i = 3; i < t.size(); ++i)
        if (!std::isdigit(uint8_t(t[i]))) return false;
    return true;
}

}  // namespace

std::string norm_bone_name(const std::string& s_in) {
    std::string s = s_in;
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".gao") == 0)
        s.resize(s.size() - 4);
    // re.findall(r'[a-z]+|[0-9]+')
    std::vector<std::string> toks;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') {
            size_t j = i;
            while (j < s.size() && s[j] >= 'a' && s[j] <= 'z') ++j;
            toks.push_back(s.substr(i, j - i));
            i = j;
        } else if (c >= '0' && c <= '9') {
            size_t j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
            toks.push_back(s.substr(i, j - i));
            i = j;
        } else {
            ++i;
        }
    }
    if (!toks.empty() && toks[0] == "b") toks.erase(toks.begin());
    long long anchor = -1;
    for (size_t k = 0; k < toks.size(); ++k)
        if (is_part_token(toks[k]) || !qualifier_of(toks[k]).empty()) {
            anchor = (long long)k;
            break;
        }
    if (anchor >= 0) {
        toks.erase(toks.begin(), toks.begin() + anchor);
    } else {
        while (!toks.empty() && fullmatch_bip(toks[0]))
            toks.erase(toks.begin());
        if (!toks.empty()) toks.erase(toks.begin());
    }
    std::set<std::string> quals;
    std::vector<std::string> digits, parts;
    for (const std::string& t : toks) {
        std::string q = qualifier_of(t);
        if (!q.empty()) {
            quals.insert(q);
            continue;
        }
        bool isdig = !t.empty() && std::isdigit(uint8_t(t[0]));
        if (isdig)
            digits.push_back(t);
        else
            parts.push_back(t);
    }
    std::sort(parts.begin(), parts.end());
    std::string norm;
    for (const std::string& q : quals) norm += q;       // std::set = sorted
    for (const std::string& p : parts) norm += p;
    for (const std::string& d : digits) norm += d;
    const auto& aliases = load_bone_aliases();
    auto it = aliases.find(norm);
    return it == aliases.end() ? norm : it->second;
}

// ── _parse_glb_meshes ──────────────────────────────────────────────────────

std::vector<GlbPatchMesh> parse_glb_meshes(const std::vector<uint8_t>& glb) {
    gltf::GlbDoc doc;
    try {
        doc = gltf::parse_glb(glb.data(), glb.size());
    } catch (...) {
        return {};
    }
    const json::Value& g = doc.gltf;

    const json::Value* nodes = g.find("nodes");
    const json::Value* skins = g.find("skins");
    std::vector<std::string> skin_joint_names;
    std::vector<std::array<double, 16>> skin_joint_ibms;
    if (skins != nullptr && skins->is_arr() && !skins->arr.empty()) {
        const json::Value& skin0 = skins->arr[0];
        const json::Value* joints = skin0.find("joints");
        if (joints != nullptr && joints->is_arr())
            for (const json::Value& jv : joints->arr) {
                long long nidx = jv.is_num() ? (long long)jv.num : -1;
                std::string nm;
                if (nodes != nullptr && nodes->is_arr() && nidx >= 0 &&
                    size_t(nidx) < nodes->arr.size()) {
                    const json::Value* n = nodes->arr[size_t(nidx)].find("name");
                    if (n != nullptr && n->is_str()) nm = n->str;
                }
                skin_joint_names.push_back(nm);
            }
        const json::Value* ibm = skin0.find("inverseBindMatrices");
        if (ibm != nullptr && ibm->is_num()) {
            try {
                auto rows = acc_doubles(doc, int(ibm->num));
                for (const auto& row : rows) {
                    std::array<double, 16> m{};
                    for (size_t k = 0; k < 16 && k < row.size(); ++k)
                        m[k] = row[k];
                    skin_joint_ibms.push_back(m);
                }
            } catch (...) {
                skin_joint_ibms.clear();
            }
        }
    }

    std::map<int, std::array<double, 16>> mesh_world =
        gltf::mesh_world_matrices(g);

    // Skinned meshes: node transform must NOT be baked (glTF Skins spec).
    std::set<int> skinned_mesh_indices;
    if (nodes != nullptr && nodes->is_arr())
        for (const json::Value& n : nodes->arr) {
            const json::Value* mi = n.find("mesh");
            const json::Value* si = n.find("skin");
            if (mi != nullptr && mi->is_num() && si != nullptr &&
                si->type != json::Value::Type::Null)
                skinned_mesh_indices.insert(int(mi->num));
        }

    std::vector<GlbPatchMesh> meshes;
    const json::Value* gmeshes = g.find("meshes");
    if (gmeshes == nullptr || !gmeshes->is_arr()) return meshes;

    static const std::array<double, 16> kI16 = {1, 0, 0, 0, 0, 1, 0, 0,
                                               0, 0, 1, 0, 0, 0, 0, 1};
    for (size_t mesh_idx = 0; mesh_idx < gmeshes->arr.size(); ++mesh_idx) {
        const json::Value& gm = gmeshes->arr[mesh_idx];
        GlbPatchMesh out;
        const json::Value* nm = gm.find("name");
        if (nm != nullptr && nm->is_str()) out.name = nm->str;
        const json::Value* extras = gm.find("extras");
        if (extras != nullptr) {
            const json::Value* jk = extras->find("jade_key");
            if (jk != nullptr && jk->is_str())
                out.jade_key_str = jk->str;
            else if (jk != nullptr && jk->is_num()) {
                out.has_jade_key_int = true;
                out.jade_key_int = uint32_t((long long)jk->num);
            }
            const json::Value* ovm = extras->find("jade_orig_vert_map");
            if (ovm != nullptr && ovm->is_arr()) {
                out.has_orig_vert_map = true;
                for (const json::Value& v : ovm->arr)
                    out.orig_vert_map.push_back(int(v.num));
            }
        }

        // Row-major world matrix M (identity for skinned / missing).
        std::array<double, 16> M = kI16;
        if (skinned_mesh_indices.count(int(mesh_idx)) == 0) {
            auto it = mesh_world.find(int(mesh_idx));
            if (it != mesh_world.end()) M = it->second;
        }
        double M3[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M3[r][c] = M[size_t(r * 4 + c)];
        // N3 = inv(M3).T, falling back to M3 when singular (patcher's
        // _inv3x3_pp — deterministic, unlike np.linalg.inv).
        double N3[3][3];
        {
            double inv[3][3];
            if (inv3x3_pp(M3, inv)) {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) N3[r][c] = inv[c][r];
            } else {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) N3[r][c] = M3[r][c];
            }
        }

        std::vector<int> vert_ids_uv1;
        double vert_id_max_frac = 0.0;
        std::unordered_map<int, size_t> pos_acc_offsets;

        const json::Value* prims = gm.find("primitives");
        size_t n_prims = (prims != nullptr && prims->is_arr())
                             ? prims->arr.size() : 0;
        for (size_t prim_idx = 0; prim_idx < n_prims; ++prim_idx) {
            const json::Value& prim = prims->arr[prim_idx];
            const json::Value* attrs = prim.find("attributes");
            auto attr = [&](const char* k) -> const json::Value* {
                return attrs != nullptr ? attrs->find(k) : nullptr;
            };
            const json::Value* pos_acc = attr("POSITION");
            size_t vert_offset;
            bool shared_verts;
            if (pos_acc != nullptr &&
                pos_acc_offsets.count(int(pos_acc->num))) {
                vert_offset = pos_acc_offsets[int(pos_acc->num)];
                shared_verts = true;
            } else {
                vert_offset = out.vertices.size();
                if (pos_acc != nullptr)
                    pos_acc_offsets[int(pos_acc->num)] = vert_offset;
                shared_verts = false;
            }
            const json::Value* pex = prim.find("extras");
            long long elem_idx;
            const json::Value* ei =
                pex != nullptr ? pex->find("element_index") : nullptr;
            if (ei != nullptr && ei->is_num()) {
                elem_idx = (long long)ei->num;
            } else {
                const json::Value* mat = prim.find("material");
                elem_idx = (mat != nullptr && mat->is_num())
                               ? (long long)mat->num : 0;
            }

            if (pos_acc != nullptr && !shared_verts) {
                for (const auto& p : acc_doubles(doc, int(pos_acc->num))) {
                    double x = p[0], y = p[1], z = p[2];
                    double wx = M[0] * x + M[1] * y + M[2] * z + M[3];
                    double wy = M[4] * x + M[5] * y + M[6] * z + M[7];
                    double wz = M[8] * x + M[9] * y + M[10] * z + M[11];
                    out.vertices.push_back({wx, -wz, wy});
                }
            }
            const json::Value* nrm_acc = attr("NORMAL");
            if (nrm_acc != nullptr && !shared_verts) {
                for (const auto& p : acc_doubles(doc, int(nrm_acc->num))) {
                    double nx = p[0], ny = p[1], nz = p[2];
                    double wx = N3[0][0] * nx + N3[0][1] * ny + N3[0][2] * nz;
                    double wy = N3[1][0] * nx + N3[1][1] * ny + N3[1][2] * nz;
                    double wz = N3[2][0] * nx + N3[2][1] * ny + N3[2][2] * nz;
                    double L = std::sqrt(wx * wx + wy * wy + wz * wz);
                    if (L > 1e-9) {
                        wx /= L;
                        wy /= L;
                        wz /= L;
                    }
                    out.normals.push_back({wx, -wz, wy});
                }
            }
            const json::Value* uv_acc = attr("TEXCOORD_0");
            if (uv_acc != nullptr && !shared_verts)
                for (const auto& p : acc_doubles(doc, int(uv_acc->num)))
                    out.uvs.push_back({p[0], p[1]});

            const json::Value* col_acc = attr("COLOR_0");
            if (col_acc != nullptr && !shared_verts) {
                gltf::AccessorData meta =
                    gltf::read_accessor(doc, int(col_acc->num));
                double cscale = 255.0;
                if (meta.comp_type == gltf::COMP_U16)
                    cscale = 255.0 / 65535.0;
                else if (meta.comp_type == gltf::COMP_U8)
                    cscale = 1.0;
                for (const auto& c : acc_doubles(doc, int(col_acc->num))) {
                    std::array<int, 4> chan{255, 255, 255, 255};
                    int nch = int(std::min<size_t>(c.size(), 4));
                    std::vector<int> vals;
                    for (int k = 0; k < nch; ++k) {
                        int v = int(pyround(c[size_t(k)] * cscale));
                        vals.push_back(std::max(0, std::min(255, v)));
                    }
                    int r = vals.size() > 0 ? vals[0] : 255;
                    int gch = vals.size() > 1 ? vals[1] : r;
                    int b = vals.size() > 2 ? vals[2] : gch;
                    int a = vals.size() > 3 ? vals[3] : 255;
                    chan = {r, gch, b, a};
                    out.colors.push_back(chan);
                }
            }
            const json::Value* uv1_acc = attr("TEXCOORD_1");
            if (uv1_acc != nullptr && !shared_verts) {
                for (const auto& p : acc_doubles(doc, int(uv1_acc->num))) {
                    double u = p[0];
                    double r = pyround(u);
                    double frac = std::fabs(u - r);
                    if (frac > vert_id_max_frac) vert_id_max_frac = frac;
                    vert_ids_uv1.push_back(int(r));
                }
            }
            const json::Value* j_acc = attr("JOINTS_0");
            if (j_acc != nullptr && !shared_verts)
                for (const auto& j : acc_doubles(doc, int(j_acc->num))) {
                    std::array<int, 4> jj{0, 0, 0, 0};
                    for (size_t k = 0; k < 4 && k < j.size(); ++k)
                        jj[k] = int(j[k]);
                    out.joints.push_back(jj);
                }
            const json::Value* w_acc = attr("WEIGHTS_0");
            if (w_acc != nullptr && !shared_verts) {
                gltf::AccessorData meta =
                    gltf::read_accessor(doc, int(w_acc->num));
                double w_scale = 1.0;
                if (meta.comp_type == gltf::COMP_U8)
                    w_scale = 255.0;
                else if (meta.comp_type == gltf::COMP_U16)
                    w_scale = 65535.0;
                for (const auto& w : acc_doubles(doc, int(w_acc->num))) {
                    std::array<double, 4> ww{0, 0, 0, 0};
                    for (size_t k = 0; k < 4 && k < w.size(); ++k)
                        ww[k] = w[k] / w_scale;
                    out.weights.push_back(ww);
                }
            }
            const json::Value* idx = prim.find("indices");
            if (idx != nullptr && idx->is_num()) {
                auto indices = acc_doubles(doc, int(idx->num));
                for (size_t k = 0; k + 2 < indices.size(); k += 3) {
                    out.faces.push_back(
                        {uint32_t(indices[k][0] + double(vert_offset)),
                         uint32_t(indices[k + 1][0] + double(vert_offset)),
                         uint32_t(indices[k + 2][0] + double(vert_offset))});
                    out.face_elems.push_back(int(elem_idx));
                }
            }
        }

        size_t nv = out.vertices.size();
        out.has_joints = !out.joints.empty() && out.joints.size() == nv;
        if (!out.has_joints) out.joints.clear();
        out.has_weights = !out.weights.empty() && out.weights.size() == nv;
        if (!out.has_weights) out.weights.clear();
        out.has_colors = !out.colors.empty() && out.colors.size() == nv;
        if (!out.has_colors) out.colors.clear();
        out.joint_names = skin_joint_names;
        out.joint_ibms = skin_joint_ibms;
        if (vert_ids_uv1.size() == nv && nv > 0) {
            out.has_vert_id_map = true;
            out.vert_id_map = std::move(vert_ids_uv1);
            out.vert_id_max_frac = vert_id_max_frac;
        }
        meshes.push_back(std::move(out));
    }
    return meshes;
}

// ── _glb_is_foreign ────────────────────────────────────────────────────────

ForeignCheck glb_is_foreign(const GlbPatchMesh& m, const GeoInfo& orig_geo,
                            bool has_target_key, uint32_t target_geo_key) {
    char b[256];
    if (!m.has_vert_id_map && !m.has_orig_vert_map)
        return {true,
                "no round-trip markers (_JADE_VERT_ID / jade_orig_vert_map)"};

    if (m.has_vert_id_map && m.vert_id_max_frac > 0.02)
        return {true,
                "vert ids are interpolated (mesh edited in Blender \xe2\x80\x94 "
                "subdivided / added geometry), so it is not a 1:1 "
                "round-trip; rebuilding from GLB geometry"};

    if (has_target_key) {
        bool have_src = false;
        uint32_t src_key = 0;
        if (!m.jade_key_str.empty()) {
            // Python: int(jk.strip(), 16) with ValueError -> None.
            std::string t = m.jade_key_str;
            size_t s0 = t.find_first_not_of(" \t\r\n");
            size_t s1 = t.find_last_not_of(" \t\r\n");
            if (s0 != std::string::npos) {
                t = t.substr(s0, s1 - s0 + 1);
                char* endp = nullptr;
                unsigned long v = std::strtoul(t.c_str(), &endp, 16);
                if (endp != nullptr && *endp == '\0' && !t.empty()) {
                    have_src = true;
                    src_key = uint32_t(v);
                }
            }
        } else if (m.has_jade_key_int) {
            have_src = true;
            src_key = m.jade_key_int;
        }
        if (have_src && src_key != target_geo_key) {
            std::snprintf(b, sizeof b,
                          "toolkit export of a DIFFERENT mesh (0x%08x, "
                          "target is 0x%08x)", src_key, target_geo_key);
            return {true, b};
        }
    }

    uint32_t orig_nb_pts = orig_geo.nb_points;
    const std::vector<int>* vmap =
        m.has_vert_id_map ? &m.vert_id_map
                          : (m.has_orig_vert_map ? &m.orig_vert_map : nullptr);
    if (vmap != nullptr && !vmap->empty() && orig_nb_pts != 0) {
        int vmax = *std::max_element(vmap->begin(), vmap->end());
        if (vmax >= 0 && uint32_t(vmax) >= orig_nb_pts) {
            std::snprintf(b, sizeof b,
                          "vert ids reach %d but the target has only %u base "
                          "vertices \xe2\x80\x94 exported from a different mesh",
                          vmax, orig_nb_pts);
            return {true, b};
        }
    }

    size_t orig_bones = orig_geo.skin_present ? orig_geo.skin_bones.size() : 0;
    if (!m.joint_names.empty() && orig_bones != 0 &&
        m.joint_names.size() != orig_bones) {
        std::snprintf(b, sizeof b,
                      "GLB skin binds %zu joints but the target skin has %zu "
                      "bones \xe2\x80\x94 different skeleton",
                      m.joint_names.size(), orig_bones);
        return {true, b};
    }
    return {false, ""};
}

// ── small internals shared by both rebuild paths ───────────────────────────

namespace {

// _qpos_p: 3-decimal rounded position key.
std::string qpos_key(const std::array<double, 3>& v) {
    return key3(pyround_dec(v[0], 3), pyround_dec(v[1], 3),
                pyround_dec(v[2], 3));
}

// _colors_by_position: src colours onto dst verts by rounded position.
std::vector<std::array<int, 4>> colors_by_position(
    const std::vector<std::array<double, 3>>& src_verts,
    const std::vector<std::array<int, 4>>& src_colors,
    const std::vector<std::array<double, 3>>& dst_verts) {
    if (src_verts.empty() || src_colors.empty() || dst_verts.empty())
        return {};
    std::unordered_map<std::string, std::array<int, 4>> posmap;
    for (size_t i = 0; i < src_verts.size() && i < src_colors.size(); ++i)
        posmap.emplace(qpos_key(src_verts[i]), src_colors[i]);
    std::vector<std::array<int, 4>> out;
    out.reserve(dst_verts.size());
    for (const auto& v : dst_verts) {
        auto it = posmap.find(qpos_key(v));
        out.push_back(it != posmap.end() ? it->second
                                         : std::array<int, 4>{255, 255, 255,
                                                              255});
    }
    return out;
}

// _orig_cooked_vb_magic: the RAW cooked-VB magic dword, has=false when the
// section is absent (vb_magic_of clamps to {2,5,6,8} — not usable here).
bool orig_cooked_vb_magic(const std::vector<uint8_t>& raw, uint32_t& magic) {
    CookedVbSection sec = cooked_vb_section(raw.data(), raw.size());
    if (!sec.ok || sec.data_start < 12) return false;
    magic = rd_u32(raw.data() + (sec.data_start - 12));
    return true;
}

// jgao_converter._compute_vertex_tangents: Lengyel +U tangents followed by
// Gram-Schmidt orthogonalisation against each vertex normal.  WW consumes
// these from TEXCOORD1 in its stride-64 skinned vertex declaration.
std::vector<std::array<double, 3>> compute_vertex_tangents(
    const std::vector<std::array<double, 3>>& positions,
    const std::vector<std::array<double, 3>>& normals,
    const std::vector<std::array<double, 2>>& uvs,
    const std::vector<std::array<uint32_t, 3>>& faces,
    const std::vector<std::array<uint32_t, 3>>& face_uvs) {
    std::vector<std::array<double, 3>> accumulated(
        positions.size(), std::array<double, 3>{0.0, 0.0, 0.0});
    size_t face_count = std::min(faces.size(), face_uvs.size());
    for (size_t fi = 0; fi < face_count; ++fi) {
        const auto& face = faces[fi];
        const auto& fuv = face_uvs[fi];
        if (face[0] >= positions.size() || face[1] >= positions.size() ||
            face[2] >= positions.size() || fuv[0] >= uvs.size() ||
            fuv[1] >= uvs.size() || fuv[2] >= uvs.size())
            continue;
        const auto& p0 = positions[face[0]];
        const auto& p1 = positions[face[1]];
        const auto& p2 = positions[face[2]];
        const auto& w0 = uvs[fuv[0]];
        const auto& w1 = uvs[fuv[1]];
        const auto& w2 = uvs[fuv[2]];
        double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        double du1 = w1[0] - w0[0], dv1 = w1[1] - w0[1];
        double du2 = w2[0] - w0[0], dv2 = w2[1] - w0[1];
        double denom = du1 * dv2 - du2 * dv1;
        if (std::fabs(denom) < 1e-12) continue;
        std::array<double, 3> tangent{};
        for (size_t axis = 0; axis < 3; ++axis)
            tangent[axis] = (e1[axis] * dv2 - e2[axis] * dv1) / denom;
        for (uint32_t vertex : face)
            for (size_t axis = 0; axis < 3; ++axis)
                accumulated[vertex][axis] += tangent[axis];
    }

    std::vector<std::array<double, 3>> result;
    result.reserve(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        std::array<double, 3> normal =
            i < normals.size() ? normals[i]
                               : std::array<double, 3>{0.0, 0.0, 1.0};
        double dot = normal[0] * accumulated[i][0] +
                     normal[1] * accumulated[i][1] +
                     normal[2] * accumulated[i][2];
        std::array<double, 3> tangent = {
            accumulated[i][0] - normal[0] * dot,
            accumulated[i][1] - normal[1] * dot,
            accumulated[i][2] - normal[2] * dot};
        double length = std::sqrt(tangent[0] * tangent[0] +
                                  tangent[1] * tangent[1] +
                                  tangent[2] * tangent[2]);
        if (length < 1e-9) {
            std::array<double, 3> axis =
                std::fabs(normal[0]) < 0.9
                    ? std::array<double, 3>{1.0, 0.0, 0.0}
                    : std::array<double, 3>{0.0, 1.0, 0.0};
            double axis_dot = normal[0] * axis[0] +
                              normal[1] * axis[1] +
                              normal[2] * axis[2];
            tangent = {axis[0] - normal[0] * axis_dot,
                       axis[1] - normal[1] * axis_dot,
                       axis[2] - normal[2] * axis_dot};
            length = std::sqrt(tangent[0] * tangent[0] +
                               tangent[1] * tangent[1] +
                               tangent[2] * tangent[2]);
            if (length == 0.0) length = 1.0;
        }
        result.push_back({tangent[0] / length, tangent[1] / length,
                          tangent[2] / length});
    }
    return result;
}

// _parse_skin_influences: vert -> [(gizmo_bone_idx, pond_u16), ...].
std::unordered_map<uint16_t, std::vector<std::pair<uint16_t, uint16_t>>>
parse_skin_influences(const std::vector<uint8_t>& geo_data) {
    std::unordered_map<uint16_t, std::vector<std::pair<uint16_t, uint16_t>>>
        influences;
    size_t off = 36;
    if (off + 4 > geo_data.size()) return influences;
    uint16_t num_lists = rd_u16(geo_data.data() + off + 2);
    off += 4;
    for (uint16_t li = 0; li < num_lists; ++li) {
        if (off + 72 > geo_data.size()) break;
        uint16_t bone_idx = rd_u16(geo_data.data() + off);
        uint16_t num_verts = rd_u16(geo_data.data() + off + 2);
        off += 4 + 64 + 4;
        if (off + size_t(num_verts) * 4 > geo_data.size()) break;
        for (uint16_t wi = 0; wi < num_verts; ++wi) {
            uint16_t vidx = rd_u16(geo_data.data() + off + size_t(wi) * 4);
            uint16_t pond = rd_u16(geo_data.data() + off + size_t(wi) * 4 + 2);
            influences[vidx].push_back({bone_idx, pond});
        }
        off += size_t(num_verts) * 4;
    }
    return influences;
}

// _regenerate_trailing_block. Empty (with regen_failed=true) = Python None.
std::vector<uint8_t> regenerate_trailing_block(
    const std::vector<std::array<double, 3>>& positions,
    const std::vector<std::array<double, 3>>& normals,
    const std::vector<std::array<double, 2>>& uvs,
    const std::vector<std::array<uint32_t, 3>>& faces,
    const std::vector<std::array<uint32_t, 3>>& face_uvs,
    const std::vector<std::vector<size_t>>& faces_by_elem,
    const std::vector<std::pair<uint32_t, uint32_t>>& elements,  // (nTri,matId)
    const std::unordered_map<uint16_t,
                             std::vector<std::pair<uint16_t, uint16_t>>>&
        influences,
    uint32_t magic,
    const std::vector<std::array<double, 3>>* tangents,
    std::vector<std::string>& log) {
    size_t nb_elems = elements.size();
    std::unordered_map<uint64_t, size_t> corner_map;
    std::vector<std::pair<uint32_t, uint32_t>> vbuf;   // (pos_idx, uv_idx)
    std::vector<uint16_t> index_buf;
    for (size_t ei = 0; ei < nb_elems; ++ei)
        for (size_t fi : faces_by_elem[ei])
            for (int c = 0; c < 3; ++c) {
                uint64_t key = (uint64_t(faces[fi][size_t(c)]) << 32) |
                               face_uvs[fi][size_t(c)];
                auto it = corner_map.find(key);
                size_t si;
                if (it == corner_map.end()) {
                    si = vbuf.size();
                    corner_map[key] = si;
                    vbuf.push_back({faces[fi][size_t(c)],
                                    face_uvs[fi][size_t(c)]});
                } else {
                    si = it->second;
                }
                index_buf.push_back(uint16_t(si));
            }
    size_t n2 = vbuf.size();
    if (n2 > 0xFFFF) {
        LOGF("    trailing regen skipped: %zu cooked vertices exceed "
             "the u16 index limit", n2);
        return {};
    }
    uint32_t stride;
    switch (magic) {
        case 0: stride = 20; break;
        case 2: case 5: stride = 32; break;
        case 6: case 8: stride = 44; break;
        default: stride = (magic == 3 && tangents) ? 64 : 52; break;
    }

    std::vector<uint8_t> out;
    put_u32v(out, 0); put_u32v(out, 0);
    put_u32v(out, uint32_t(nb_elems)); put_u32v(out, 0);
    uint32_t sec2_size = uint32_t(8 + n2 * stride);
    for (size_t i = 0; i < nb_elems; ++i) {
        put_u32v(out, uint32_t(faces_by_elem[i].size()));
        // Faithful to the Python: x = NEXT element's matId, last = sec2 size.
        uint32_t x = (i < nb_elems - 1) ? elements[i + 1].second : sec2_size;
        put_u32v(out, x);
    }

    put_u32v(out, magic);
    put_u32v(out, uint32_t(n2));
    put_u32v(out, stride);
    for (const auto& pv : vbuf) {
        uint32_t pos_idx = pv.first, uv_idx = pv.second;
        double px = positions[pos_idx][0], py = positions[pos_idx][1],
               pz = positions[pos_idx][2];
        double u = 0.0, v = 0.0;
        if (uv_idx < uvs.size()) {
            u = uvs[uv_idx][0];
            v = uvs[uv_idx][1];
        }
        if (magic == 0) {
            put_f32v(out, px); put_f32v(out, py); put_f32v(out, pz);
            put_f32v(out, u); put_f32v(out, v);
            continue;
        }
        double nx = 0.0, ny = 0.0, nz = 1.0;
        if (pos_idx < normals.size()) {
            nx = normals[pos_idx][0];
            ny = normals[pos_idx][1];
            nz = normals[pos_idx][2];
        }
        if (magic == 2 || magic == 5) {
            put_f32v(out, px); put_f32v(out, py); put_f32v(out, pz);
            put_f32v(out, nx); put_f32v(out, ny); put_f32v(out, nz);
            put_f32v(out, u); put_f32v(out, v);
            continue;
        }
        if (magic == 6 || magic == 8) {
            if (magic == 8) { nx = ny = nz = 0.0; }
            put_f32v(out, px); put_f32v(out, py); put_f32v(out, pz);
            put_f32v(out, nx); put_f32v(out, ny); put_f32v(out, nz);
            put_f32v(out, u); put_f32v(out, v);
            for (int z = 0; z < 12; ++z) out.push_back(0);
            continue;
        }
        // magic == 3 — skinned vertex; top 3 influences by (-pond, bone).
        std::vector<std::pair<uint16_t, uint16_t>> infl;
        auto iit = influences.find(uint16_t(pos_idx));
        if (iit != influences.end()) infl = iit->second;
        std::stable_sort(infl.begin(), infl.end(),
                         [](const std::pair<uint16_t, uint16_t>& a,
                            const std::pair<uint16_t, uint16_t>& b) {
                             if (a.second != b.second)
                                 return a.second > b.second;
                             return a.first < b.first;
                         });
        // Magic-3 consumes three explicit weights and has no implicit fourth.
        // Match patcher.py: keep the top three and renormalize their sum.
        if (infl.size() > 3) infl.resize(3);
        uint32_t bones[4] = {0, 0, 0, 0};
        double wts[3] = {0.0, 0.0, 0.0};
        for (size_t k = 0; k < infl.size(); ++k) {
            bones[k] = infl[k].first;
            uint32_t bits = uint32_t(infl[k].second) << 16;
            float f;
            std::memcpy(&f, &bits, 4);
            wts[k] = double(f);
        }
        double wsum = wts[0] + wts[1] + wts[2];
        if (wsum > 0.0) {
            wts[0] /= wsum;
            wts[1] /= wsum;
            wts[2] /= wsum;
        }
        uint32_t pack_a = (bones[0] * 3) | ((bones[1] * 3) << 16);
        uint32_t pack_b = (bones[2] * 3) | ((bones[3] * 3) << 16);
        put_f32v(out, px); put_f32v(out, py); put_f32v(out, pz);
        put_f32v(out, nx); put_f32v(out, ny); put_f32v(out, nz);
        put_u32v(out, pack_a); put_u32v(out, pack_b);
        put_f32v(out, wts[0]); put_f32v(out, wts[1]); put_f32v(out, wts[2]);
        put_f32v(out, u); put_f32v(out, v);
        if (stride == 64) {
            std::array<double, 3> tangent = {1.0, 0.0, 0.0};
            if (tangents && pos_idx < tangents->size())
                tangent = (*tangents)[pos_idx];
            put_f32v(out, tangent[0]);
            put_f32v(out, tangent[1]);
            put_f32v(out, tangent[2]);
        }
    }

    put_u32v(out, uint32_t(faces.size() * 6));
    for (uint16_t iv : index_buf) put_u16v(out, iv);

    LOGF("    regenerated trailing block: %zu cooked verts "
         "(magic=%u stride=%u), %zu tris, %zu elements",
         n2, magic, stride, faces.size(), nb_elems);
    return out;
}

// _rebuild_geo_binary: toolkit round-trip rebuild — snap the GLB back onto
// the original vertex array. Empty = the Python None (no change / skip).
std::vector<uint8_t> rebuild_geo_binary(const GeoInfo& orig_geo,
                                        const std::vector<uint8_t>& raw,
                                        const GlbPatchMesh& glb_mesh,
                                        bool import_vertex_colors, bool force,
                                        std::vector<std::string>& log) {
    const auto& glb_verts = glb_mesh.vertices;
    const auto& glb_norms = glb_mesh.normals;
    const auto& glb_uvs = glb_mesh.uvs;
    const auto& glb_faces = glb_mesh.faces;

    bool has_normals = glb_norms.size() == glb_verts.size() &&
                       !glb_verts.empty();
    uint32_t orig_nb_pts = orig_geo.nb_points;
    uint32_t orig_nb_uvs = orig_geo.nb_uvs;
    size_t n_orig_verts = orig_geo.vertices.size() / 3;

    if (n_orig_verts < orig_nb_pts) {
        LOGF("    skip rebuild: orig_verts=%zu < nb_points=%u (mesh variant "
             "not yet supported)", n_orig_verts, orig_nb_pts);
        return {};
    }
    size_t n_orig_normals = orig_geo.normals.size() / 3;
    if (has_normals && n_orig_normals < orig_nb_pts) {
        LOGF("    skip rebuild: normals=%zu < nb_points=%u (GLB has normals "
             "but original GEO stores none of this size \xe2\x80\x94 mesh "
             "variant not yet supported)", n_orig_normals, orig_nb_pts);
        return {};
    }

    // Map glTF vertices -> Jade position indices.
    std::vector<int> glb_to_jade_pos;
    bool have_map = false;
    if (glb_mesh.has_vert_id_map &&
        glb_mesh.vert_id_map.size() == glb_verts.size()) {
        glb_to_jade_pos = glb_mesh.vert_id_map;
        have_map = true;
    } else if (glb_mesh.has_orig_vert_map &&
               glb_mesh.orig_vert_map.size() == glb_verts.size()) {
        glb_to_jade_pos = glb_mesh.orig_vert_map;
        have_map = true;
    } else if (n_orig_verts > 0) {
        glb_to_jade_pos.reserve(glb_verts.size());
        for (const auto& v : glb_verts) {
            size_t best_idx = 0;
            double best_d2 = std::numeric_limits<double>::infinity();
            for (size_t oi = 0; oi < n_orig_verts; ++oi) {
                double dx = v[0] - double(orig_geo.vertices[oi * 3]);
                double dy = v[1] - double(orig_geo.vertices[oi * 3 + 1]);
                double dz = v[2] - double(orig_geo.vertices[oi * 3 + 2]);
                double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_idx = oi;
                }
            }
            glb_to_jade_pos.push_back(int(best_idx));
        }
        have_map = true;
    }

    // Build Jade position + normal arrays.
    std::vector<std::array<double, 3>> jade_positions, jade_normals;
    uint32_t nb_pts;
    if (have_map) {
        jade_positions.reserve(n_orig_verts);
        for (size_t i = 0; i < n_orig_verts; ++i)
            jade_positions.push_back({double(orig_geo.vertices[i * 3]),
                                      double(orig_geo.vertices[i * 3 + 1]),
                                      double(orig_geo.vertices[i * 3 + 2])});
        if (n_orig_normals > 0) {
            jade_normals.reserve(n_orig_normals);
            for (size_t i = 0; i < n_orig_normals; ++i)
                jade_normals.push_back({double(orig_geo.normals[i * 3]),
                                        double(orig_geo.normals[i * 3 + 1]),
                                        double(orig_geo.normals[i * 3 + 2])});
        } else {
            jade_normals.assign(orig_nb_pts, {0.0, 0.0, 1.0});
        }
        for (size_t gi = 0; gi < glb_to_jade_pos.size(); ++gi) {
            int oi = glb_to_jade_pos[gi];
            if (oi >= 0 && uint32_t(oi) < orig_nb_pts) {
                jade_positions[size_t(oi)] = glb_verts[gi];
                if (has_normals && gi < glb_norms.size())
                    jade_normals[size_t(oi)] = glb_norms[gi];
            }
        }
        nb_pts = orig_nb_pts;
    } else {
        // pos-dedup path (no original vertices at all).
        std::unordered_map<std::string, size_t> pos_map;
        for (size_t gi = 0; gi < glb_verts.size(); ++gi) {
            const auto& v = glb_verts[gi];
            std::string pk = key3(pyround_dec(v[0], 5), pyround_dec(v[1], 5),
                                  pyround_dec(v[2], 5));
            auto it = pos_map.find(pk);
            if (it == pos_map.end()) {
                pos_map[pk] = jade_positions.size();
                jade_positions.push_back(v);
                jade_normals.push_back(has_normals && gi < glb_norms.size()
                                           ? glb_norms[gi]
                                           : std::array<double, 3>{0, 0, 1});
            }
            glb_to_jade_pos.push_back(int(pos_map[pk]));
        }
        nb_pts = uint32_t(jade_positions.size());
    }

    // UV array (deduped).
    std::unordered_map<std::string, size_t> uv_map;
    std::vector<std::array<double, 2>> jade_uvs;
    std::vector<uint32_t> glb_to_uv;
    for (const auto& uv : glb_uvs) {
        std::string uk = key2(pyround_dec(uv[0], 6), pyround_dec(uv[1], 6));
        auto it = uv_map.find(uk);
        if (it == uv_map.end()) {
            uv_map[uk] = jade_uvs.size();
            jade_uvs.push_back(uv);
        }
        glb_to_uv.push_back(uint32_t(uv_map[uk]));
    }
    while (glb_to_uv.size() < glb_verts.size()) glb_to_uv.push_back(0);
    uint32_t nb_uvs = uint32_t(jade_uvs.size());

    // Face arrays.
    std::vector<std::array<uint32_t, 3>> jade_faces, jade_face_uvs;
    for (const auto& f : glb_faces) {
        jade_faces.push_back({uint32_t(glb_to_jade_pos[f[0]]),
                              uint32_t(glb_to_jade_pos[f[1]]),
                              uint32_t(glb_to_jade_pos[f[2]])});
        jade_face_uvs.push_back(
            {glb_to_uv[f[0]], glb_to_uv[f[1]], glb_to_uv[f[2]]});
    }

    // No-change short-circuit (first 100 verts, 1e-5).
    if (!force && nb_pts == orig_nb_pts && n_orig_verts == nb_pts) {
        double max_diff = 0.0;
        uint32_t lim = std::min<uint32_t>(nb_pts, 100);
        for (uint32_t i = 0; i < lim; ++i)
            for (int j = 0; j < 3; ++j) {
                double d = std::fabs(double(orig_geo.vertices[i * 3 +
                                                              uint32_t(j)]) -
                                     jade_positions[i][size_t(j)]);
                if (d > max_diff) max_diff = d;
            }
        if (max_diff < 1e-5) return {};
    }

    if (orig_geo.skin_present && nb_pts != orig_nb_pts) {
        LOGF("    Position count changed (%u \xe2\x86\x92 %u) on skinned "
             "mesh, skipping", orig_nb_pts, nb_pts);
        return {};
    }

    uint32_t mrm_marker = orig_geo.mrm_marker;
    bool has_orig_normals = n_orig_normals > 0;
    uint32_t has_abs = orig_geo.has_abs;
    uint32_t abs_count = orig_geo.abs_count;

    size_t eff_hdr = 40;
    if (mrm_marker == 0xC0DE2002 && orig_geo.skin_size > 0)
        eff_hdr = 36 + orig_geo.skin_size + 4;

    size_t orig_vert_start = eff_hdr;
    size_t orig_vert_end = orig_vert_start + size_t(orig_nb_pts) * 12;
    size_t off = orig_vert_end;
    if (has_orig_normals) off += size_t(orig_nb_pts) * 12;
    size_t orig_norm_end = off;
    if (has_abs != 0) off += size_t(std::min(abs_count, orig_nb_pts)) * 4;
    size_t orig_abs_end = off;
    size_t orig_uv_end = orig_abs_end + size_t(orig_nb_uvs) * 8;
    size_t nb_elems_orig = orig_geo.elements.size() / 2;
    size_t orig_elem_end = orig_uv_end + nb_elems_orig * 8;
    uint32_t orig_total_tris = 0;
    for (size_t i = 0; i < nb_elems_orig; ++i)
        orig_total_tris += orig_geo.elements[i * 2];
    size_t orig_tri_end = orig_elem_end + size_t(orig_total_tris) * 16;

    std::vector<uint8_t> result;
    result.insert(result.end(), raw.begin(), raw.begin() + long(orig_vert_start));
    for (const auto& v : jade_positions) {
        put_f32v(result, v[0]);
        put_f32v(result, v[1]);
        put_f32v(result, v[2]);
    }
    if (has_orig_normals && has_normals) {
        for (const auto& n : jade_normals) {
            put_f32v(result, n[0]);
            put_f32v(result, n[1]);
            put_f32v(result, n[2]);
        }
    } else if (has_orig_normals) {
        result.insert(result.end(), raw.begin() + long(orig_vert_end),
                      raw.begin() + long(orig_norm_end));
    }

    // Vertex colours (dul_PointColors).
    bool color_override_applied = false;
    if (import_vertex_colors && glb_mesh.has_colors && !glb_to_jade_pos.empty()) {
        std::vector<bool> set_(nb_pts, false);
        std::vector<std::array<int, 4>> jade_colors(
            nb_pts, std::array<int, 4>{255, 255, 255, 255});
        for (size_t gi = 0; gi < glb_to_jade_pos.size(); ++gi) {
            int oi = glb_to_jade_pos[gi];
            if (oi >= 0 && uint32_t(oi) < nb_pts &&
                gi < glb_mesh.colors.size() && !set_[size_t(oi)]) {
                jade_colors[size_t(oi)] = glb_mesh.colors[gi];
                set_[size_t(oi)] = true;
            }
        }
        // _pack_vertex_colors: nb D3DCOLOR AARRGGBB dwords.
        for (uint32_t i = 0; i < nb_pts; ++i) {
            const auto& c = jade_colors[i];
            put_u32v(result, (uint32_t(c[3] & 0xFF) << 24) |
                                 (uint32_t(c[0] & 0xFF) << 16) |
                                 (uint32_t(c[1] & 0xFF) << 8) |
                                 uint32_t(c[2] & 0xFF));
        }
        color_override_applied = true;
        LOGF("    vertex colors: rebuilt %u from GLB COLOR_0", nb_pts);
    } else {
        result.insert(result.end(), raw.begin() + long(orig_norm_end),
                      raw.begin() + long(orig_abs_end));
    }

    for (const auto& uv : jade_uvs) {
        put_f32v(result, uv[0]);
        put_f32v(result, uv[1]);
    }

    // Elements: original matIds, recomputed per-element nTri from the GLB's
    // primitive partition.
    std::vector<int> glb_face_elems = glb_mesh.face_elems;
    if (glb_face_elems.size() != jade_faces.size())
        glb_face_elems.assign(jade_faces.size(), 0);
    std::vector<std::vector<size_t>> faces_by_elem(
        std::max<size_t>(nb_elems_orig, 1));
    uint32_t overflow = 0;
    for (size_t fi = 0; fi < glb_face_elems.size(); ++fi) {
        int eidx = glb_face_elems[fi];
        if (eidx >= 0 && size_t(eidx) < nb_elems_orig) {
            faces_by_elem[size_t(eidx)].push_back(fi);
        } else {
            faces_by_elem[0].push_back(fi);
            ++overflow;
        }
    }
    if (overflow)
        LOGF("    %u face(s) had element_index outside [0,%zu); merged into "
             "element 0", overflow, nb_elems_orig);
    for (size_t i = 0; i < nb_elems_orig; ++i) {
        put_u32v(result, uint32_t(faces_by_elem[i].size()));
        put_u32v(result, orig_geo.elements[i * 2 + 1]);
    }

    // Triangles in element order.
    for (size_t ei = 0; ei < nb_elems_orig; ++ei)
        for (size_t fi : faces_by_elem[ei]) {
            const auto& pi = jade_faces[fi];
            const auto& ui = jade_face_uvs[fi];
            put_u16v(result, uint16_t(pi[0]));
            put_u16v(result, uint16_t(pi[1]));
            put_u16v(result, uint16_t(pi[2]));
            put_u16v(result, uint16_t(ui[0]));
            put_u16v(result, uint16_t(ui[1]));
            put_u16v(result, uint16_t(ui[2]));
            put_u32v(result, 0);
        }

    // Trailing block.
    std::vector<uint8_t> orig_trailing(raw.begin() + long(std::min(
                                           orig_tri_end, raw.size())),
                                       raw.end());
    std::vector<uint8_t> regenerated;
    bool regen_attempted = false;
    size_t sec1_len = 16 + nb_elems_orig * 8;
    if (mrm_marker == 0xC0DE2002 && nb_elems_orig > 0 &&
        orig_trailing.size() >= sec1_len + 12 &&
        std::all_of(orig_trailing.begin(), orig_trailing.begin() + 8,
                    [](uint8_t x) { return x == 0; }) &&
        rd_u32(orig_trailing.data() + 8) == uint32_t(nb_elems_orig)) {
        uint32_t magic = rd_u32(orig_trailing.data() + sec1_len);
        uint32_t stride = rd_u32(orig_trailing.data() + sec1_len + 8);
        if ((stride == 52 || stride == 64) && magic > 0 && magic < 256) {
            auto influences = parse_skin_influences(raw);
            std::vector<std::pair<uint32_t, uint32_t>> elems_pairs;
            for (size_t i = 0; i < nb_elems_orig; ++i)
                elems_pairs.push_back({orig_geo.elements[i * 2],
                                       orig_geo.elements[i * 2 + 1]});
            std::vector<std::array<double, 3>> tangents;
            const std::vector<std::array<double, 3>>* tangent_ptr = nullptr;
            if (magic == 3 && stride == 64 && !jade_normals.empty()) {
                tangents = compute_vertex_tangents(
                    jade_positions, jade_normals, jade_uvs, jade_faces,
                    jade_face_uvs);
                tangent_ptr = &tangents;
                LOGF("  original is stride-64 (normal-mapped): emitting "
                     "per-vertex tangents in TEXCOORD1 to prevent bump "
                     "artifacts");
            }
            regenerated = regenerate_trailing_block(
                jade_positions, jade_normals, jade_uvs, jade_faces,
                jade_face_uvs, faces_by_elem, elems_pairs, influences, magic,
                tangent_ptr, log);
            regen_attempted = true;
        } else {
            LOGF("    trailing block has unknown vertex format (magic=%u, "
                 "stride=%u); preserving verbatim", magic, stride);
        }
    }

    if (regen_attempted && !regenerated.empty()) {
        result.insert(result.end(), regenerated.begin(), regenerated.end());
    } else {
        if (nb_pts != orig_nb_pts || nb_uvs != orig_nb_uvs ||
            jade_faces.size() != orig_total_tris)
            LOGF("    WARNING: mesh topology changed but the cooked trailing "
                 "block could not be regenerated \xe2\x80\x94 it will be "
                 "stale, so the model may render incorrectly or crash. "
                 "Verify in-game.");
        std::vector<uint8_t> trailing_data = orig_trailing;
        uint32_t trailing_patches = 0;
        uint32_t lim = std::min<uint32_t>(nb_pts, uint32_t(n_orig_verts));
        for (uint32_t vi = 0; vi < lim; ++vi) {
            double old_pos[3] = {double(orig_geo.vertices[vi * 3]),
                                 double(orig_geo.vertices[vi * 3 + 1]),
                                 double(orig_geo.vertices[vi * 3 + 2])};
            const auto& new_pos = jade_positions[vi];
            bool same = true;
            for (int j = 0; j < 3; ++j)
                if (std::fabs(old_pos[j] - new_pos[size_t(j)]) > 1e-7)
                    same = false;
            if (same) continue;
            std::vector<uint8_t> old_b, new_b;
            for (int j = 0; j < 3; ++j) put_f32v(old_b, old_pos[j]);
            for (int j = 0; j < 3; ++j) put_f32v(new_b, new_pos[size_t(j)]);
            size_t search_start = 0;
            while (trailing_data.size() >= 12 &&
                   search_start < trailing_data.size() - 11) {
                auto it = std::search(trailing_data.begin() + long(search_start),
                                      trailing_data.end(), old_b.begin(),
                                      old_b.end());
                if (it == trailing_data.end()) break;
                size_t idx = size_t(it - trailing_data.begin());
                std::copy(new_b.begin(), new_b.end(),
                          trailing_data.begin() + long(idx));
                ++trailing_patches;
                if (vi < n_orig_normals && vi < jade_normals.size() &&
                    idx + 24 <= trailing_data.size()) {
                    std::vector<uint8_t> old_n, new_n;
                    for (int j = 0; j < 3; ++j) {
                        put_f32v(old_n,
                                 double(orig_geo.normals[vi * 3 + uint32_t(j)]));
                        put_f32v(new_n, jade_normals[vi][size_t(j)]);
                    }
                    if (std::equal(old_n.begin(), old_n.end(),
                                   trailing_data.begin() + long(idx + 12)))
                        std::copy(new_n.begin(), new_n.end(),
                                  trailing_data.begin() + long(idx + 12));
                }
                search_start = idx + 12;
            }
        }
        result.insert(result.end(), trailing_data.begin(),
                      trailing_data.end());
        if (trailing_patches > 0)
            LOGF("    Patched %u trailing VB positions", trailing_patches);
    }

    // Header nb_points / nb_uvs (+ colour-override abs fields).
    if (result.size() >= 28) {
        matput32(result, 12, nb_pts);
        matput32(result, 24, nb_uvs);
        if (color_override_applied) {
            matput32(result, 16, nb_pts);
            matput32(result, 20, 1);
        }
    }
    return result;
}

// ── frame conversion + rest-pose helpers (T4.2 diagnostic) ─────────────────

// Deterministic partial-pivot GJ 4x4 inverse (diagnostic use only here).
bool inv4x4_pp(const std::array<double, 16>& m, std::array<double, 16>& out) {
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

// _gltf_to_jade_matrix: conjugate a col-major glTF IBM by the axis swap.
// The S matrices are signed permutations, so the products are EXACT.
std::array<double, 16> gltf_to_jade_matrix(const std::array<double, 16>& v) {
    for (double x : v)
        if (!std::isfinite(x))
            return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Row-major forms: S_G2J = [[1,0,0,0],[0,0,-1,0],[0,1,0,0],[0,0,0,1]],
    //                  S_J2G = [[1,0,0,0],[0,0,1,0],[0,-1,0,0],[0,0,0,1]].
    auto M = [&](int r, int c) { return v[size_t(c * 4 + r)]; };
    double A[4][4];   // S_G2J @ M (row swap: row1' = -row2, row2' = row1)
    for (int c = 0; c < 4; ++c) {
        A[0][c] = M(0, c);
        A[1][c] = -M(2, c);
        A[2][c] = M(1, c);
        A[3][c] = M(3, c);
    }
    // (A) @ S_J2G (col swap: col1' = -col2, col2' = col1).
    std::array<double, 16> out{};
    for (int r = 0; r < 4; ++r) {
        out[size_t(0 * 4 + r)] = A[r][0];
        out[size_t(1 * 4 + r)] = -A[r][2];
        out[size_t(2 * 4 + r)] = A[r][1];
        out[size_t(3 * 4 + r)] = A[r][3];
    }
    return out;
}

// _bone_world_from_ibm: rest-pose world position = inv(IBM) translation.
std::array<double, 3> bone_world_from_ibm(const std::array<double, 16>& ibm) {
    for (double x : ibm)
        if (!std::isfinite(x)) return {0.0, 0.0, 0.0};
    std::array<double, 16> inv{};
    if (!inv4x4_pp(ibm, inv)) return {0.0, 0.0, 0.0};
    return {inv[12], inv[13], inv[14]};
}

// Python vertex_weights: vert -> [(bone_list_idx, weight)], bone-list order.
struct VertexWeights {
    std::unordered_map<uint32_t, std::vector<std::pair<int, double>>> map;
};
VertexWeights derive_vertex_weights(const GeoInfo& geo) {
    VertexWeights vw;
    for (size_t bli = 0; bli < geo.skin_bones.size(); ++bli)
        for (const auto& wp : geo.skin_bones[bli].weights)
            vw.map[wp.first].push_back({int(bli), double(wp.second) / 65535.0});
    return vw;
}

// _orig_bone_centroids.
std::vector<std::pair<bool, std::array<double, 3>>> orig_bone_centroids(
    const GeoInfo& orig_geo) {
    size_t nbones = orig_geo.skin_bones.size();
    std::vector<std::pair<bool, std::array<double, 3>>> centroids(
        nbones, {false, {0, 0, 0}});
    size_t nverts = orig_geo.vertices.size() / 3;
    VertexWeights vw = derive_vertex_weights(orig_geo);
    std::unordered_map<int, std::vector<uint32_t>> clusters;
    for (const auto& kv : vw.map) {
        if (kv.first >= nverts) continue;
        int best = -1;
        double best_w = -1.0;
        for (const auto& bw : kv.second)
            if (bw.second > best_w) {
                best_w = bw.second;
                best = bw.first;
            }
        if (best >= 0) clusters[best].push_back(kv.first);
    }
    for (const auto& kv : clusters) {
        int b = kv.first;
        if (kv.second.empty() || b < 0 || size_t(b) >= nbones) continue;
        double sx = 0, sy = 0, sz = 0;
        for (uint32_t vi : kv.second) {
            sx += double(orig_geo.vertices[vi * 3]);
            sy += double(orig_geo.vertices[vi * 3 + 1]);
            sz += double(orig_geo.vertices[vi * 3 + 2]);
        }
        double n = double(kv.second.size());
        centroids[size_t(b)] = {true, {sx / n, sy / n, sz / n}};
    }
    return centroids;
}

}  // namespace

// _resolve_skeleton_gizmos.
std::vector<std::string> resolve_skeleton_gizmos(
    const std::vector<SubEntry>& subs, bool has_geo_key, uint32_t geo_key) {
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;
    std::vector<uint32_t> chosen;   // gao_key per gizmo slot
    bool have = false;
    if (has_geo_key) {
        // Python resolves the GAO that hosts this GEO directly OR through a
        // geometry group. Using the first arbitrary skeleton for a group
        // member produces false name-mismatch warnings.
        const SubEntry* host = host_gao_sub(subs, geo_key);
        if (host) {
            GaoInfo info;
            try { info = parse_gao_full(host->data.data(), host->data.size()); }
            catch (...) {}
            if (info.ok && !info.gizmo_flat.empty()) {
                for (size_t i = 0; i + 1 < info.gizmo_flat.size(); i += 2)
                    chosen.push_back(info.gizmo_flat[i]);
                have = true;
            }
        }
    }
    if (!have) {
        for (const SubEntry& s : subs) {
            if (s.ext != ".gao") continue;
            GaoInfo info;
            try { info = parse_gao_full(s.data.data(), s.data.size()); }
            catch (...) { continue; }
            if (!info.ok || info.gizmo_flat.empty()) continue;
            size_t cnt = info.gizmo_flat.size() / 2;
            if (!have || cnt > chosen.size()) {
                chosen.clear();
                for (size_t i = 0; i + 1 < info.gizmo_flat.size(); i += 2)
                    chosen.push_back(info.gizmo_flat[i]);
                have = true;
            }
        }
    }
    std::vector<std::string> names;
    if (!have) return names;
    for (uint32_t gk : chosen) {
        std::string nm;
        auto it = by_key.find(gk);
        if (it != by_key.end() && it->second->ext == ".gao") {
            GaoInfo hdr;
            try {
                hdr = parse_gao_full(it->second->data.data(),
                                     it->second->data.size());
            } catch (...) {}
            if (hdr.ok && !hdr.name.empty()) {
                nm = hdr.name;
                // .strip() + remove '\x00'.
                size_t a = nm.find_first_not_of(" \t\r\n\f\v");
                size_t b = nm.find_last_not_of(" \t\r\n\f\v");
                nm = a == std::string::npos ? "" : nm.substr(a, b - a + 1);
                nm.erase(std::remove(nm.begin(), nm.end(), '\0'), nm.end());
            }
        }
        names.push_back(nm);
    }
    return names;
}

// _compute_bone_map.
void compute_bone_map(const GlbPatchMesh& mesh, const GeoInfo& orig_geo,
                      const std::vector<std::string>& gizmo_names,
                      std::map<int, int>& bone_map_out,
                      std::map<int, std::string>& source_out) {
    bone_map_out.clear();
    source_out.clear();
    const auto& glb_verts = mesh.vertices;
    size_t n_orig_verts = orig_geo.vertices.size() / 3;
    size_t nbones = orig_geo.skin_bones.size();
    if (!mesh.has_joints || !mesh.has_weights || nbones == 0 ||
        n_orig_verts == 0)
        return;

    // Name match.
    std::map<int, int> name_map;
    if (!gizmo_names.empty() && !mesh.joint_names.empty()) {
        std::unordered_map<std::string, int> dp_by_name;
        for (size_t bi = 0; bi < nbones; ++bi) {
            int gz = orig_geo.skin_bones[bi].bone_idx;
            if (gz >= 0 && size_t(gz) < gizmo_names.size() &&
                !gizmo_names[size_t(gz)].empty())
                dp_by_name.emplace(norm_bone_name(gizmo_names[size_t(gz)]),
                                   int(bi));
        }
        for (size_t j = 0; j < mesh.joint_names.size(); ++j) {
            auto it = dp_by_name.find(norm_bone_name(mesh.joint_names[j]));
            if (it != dp_by_name.end()) name_map[int(j)] = it->second;
        }
    }

    // Insertion-ordered clusters (iteration order is byte-visible through
    // strict-< tie-breaks below — mirror the Python dict order).
    struct OrderedClusters {
        std::vector<int> order;
        std::unordered_map<int, std::vector<uint32_t>> map;
        void add(int k, uint32_t v) {
            auto it = map.find(k);
            if (it == map.end()) {
                order.push_back(k);
                map[k].push_back(v);
            } else {
                it->second.push_back(v);
            }
        }
    };
    OrderedClusters glb_clusters;
    for (uint32_t vi = 0; vi < glb_verts.size(); ++vi) {
        const auto& ws = mesh.weights[vi];
        const auto& js = mesh.joints[vi];
        int best = -1;
        double best_w = -1.0;
        for (int k = 0; k < 4; ++k)
            if (ws[size_t(k)] > best_w) {
                best_w = ws[size_t(k)];
                best = js[size_t(k)];
            }
        if (best >= 0) glb_clusters.add(best, vi);
    }
    // Original clusters: Python iterates skin['vertex_weights'].items() —
    // that dict was built vert-by-vert in bone-list weight order; its key
    // order is FIRST-SEEN vertex order. Rebuild it the same way.
    OrderedClusters dp_clusters;
    {
        std::vector<uint32_t> vert_order;
        std::unordered_map<uint32_t,
                           std::vector<std::pair<int, double>>> vw;
        for (size_t bli = 0; bli < nbones; ++bli)
            for (const auto& wp : orig_geo.skin_bones[bli].weights) {
                auto it = vw.find(wp.first);
                if (it == vw.end()) vert_order.push_back(wp.first);
                vw[wp.first].push_back(
                    {int(bli), double(wp.second) / 65535.0});
            }
        for (uint32_t vert : vert_order) {
            if (vert >= n_orig_verts) continue;
            int best = -1;
            double best_w = -1.0;
            for (const auto& bw : vw[vert])
                if (bw.second > best_w) {
                    best_w = bw.second;
                    best = bw.first;
                }
            if (best >= 0) dp_clusters.add(best, vert);
        }
    }

    auto centroids = [](const OrderedClusters& cl,
                        const std::vector<std::array<double, 3>>& verts,
                        const std::vector<float>* fverts)
        -> std::vector<std::pair<int, std::array<double, 3>>> {
        std::vector<std::pair<int, std::array<double, 3>>> out;
        for (int b : cl.order) {
            const auto& vis = cl.map.at(b);
            if (vis.empty()) continue;
            double sx = 0, sy = 0, sz = 0;
            for (uint32_t vi : vis) {
                if (fverts != nullptr) {
                    sx += double((*fverts)[vi * 3]);
                    sy += double((*fverts)[vi * 3 + 1]);
                    sz += double((*fverts)[vi * 3 + 2]);
                } else {
                    sx += verts[vi][0];
                    sy += verts[vi][1];
                    sz += verts[vi][2];
                }
            }
            double n = double(vis.size());
            out.push_back({b, {sx / n, sy / n, sz / n}});
        }
        return out;
    };
    auto glb_c = centroids(glb_clusters, glb_verts, nullptr);
    auto dp_c = centroids(dp_clusters, {}, &orig_geo.vertices);
    if (glb_c.empty() || dp_c.empty()) {
        for (const auto& kv : name_map) {
            bone_map_out[kv.first] = kv.second;
            source_out[kv.first] = "name";
        }
        return;
    }

    auto normalise = [](std::vector<std::pair<int, std::array<double, 3>>>& c) {
        double mn[3], mx[3];
        for (int i = 0; i < 3; ++i) {
            mn[i] = c[0].second[size_t(i)];
            mx[i] = c[0].second[size_t(i)];
        }
        for (const auto& kv : c)
            for (int i = 0; i < 3; ++i) {
                if (kv.second[size_t(i)] < mn[i]) mn[i] = kv.second[size_t(i)];
                if (kv.second[size_t(i)] > mx[i]) mx[i] = kv.second[size_t(i)];
            }
        double rng[3];
        for (int i = 0; i < 3; ++i) {
            rng[i] = mx[i] - mn[i];
            if (rng[i] == 0.0) rng[i] = 1.0;
        }
        for (auto& kv : c)
            for (int i = 0; i < 3; ++i)
                kv.second[size_t(i)] = (kv.second[size_t(i)] - mn[i]) / rng[i];
    };
    normalise(glb_c);
    normalise(dp_c);
    for (const auto& g : glb_c) {
        int best = -1;
        double best_d = 1e30;
        for (const auto& d : dp_c) {
            double dd = 0;
            for (int i = 0; i < 3; ++i) {
                double x = g.second[size_t(i)] - d.second[size_t(i)];
                dd += x * x;
            }
            if (dd < best_d) {
                best_d = dd;
                best = d.first;
            }
        }
        if (best >= 0) {
            bone_map_out[g.first] = best;
            source_out[g.first] = "geometric";
        }
    }
    for (const auto& kv : name_map) {
        bone_map_out[kv.first] = kv.second;
        source_out[kv.first] = "name";
    }
}

namespace {

// _pond: a weight's float32 top 16 bits.
uint16_t pond_of(double weight) {
    float f = float(weight);
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return uint16_t(b >> 16);
}

// _rebuild_geo_foreign. Empty = the Python None.
std::vector<uint8_t> rebuild_geo_foreign(
    const GeoInfo& orig_geo, const std::vector<uint8_t>& raw,
    const GlbPatchMesh& glb_mesh, const PatchModelOptions& opts,
    const std::vector<std::string>& gizmo_names,
    std::vector<std::string>& log) {
    bool is_skinned =
        orig_geo.skin_present && !orig_geo.skin_bones.empty();

    const auto& glb_verts = glb_mesh.vertices;
    std::vector<std::array<double, 3>> glb_norms = glb_mesh.normals;
    const auto& glb_uvs = glb_mesh.uvs;
    const auto& glb_faces = glb_mesh.faces;
    std::vector<int> glb_face_elems = glb_mesh.face_elems;
    if (glb_face_elems.size() != glb_faces.size())
        glb_face_elems.assign(glb_faces.size(), 0);

    uint32_t nb_pts = uint32_t(glb_verts.size());
    if (nb_pts == 0 || glb_faces.empty()) {
        log.push_back("  foreign-mesh rebuild: GLB has no geometry");
        return {};
    }
    if (nb_pts > 0xFFFF) {
        LOGF("  foreign-mesh rebuild: %u vertices exceed the u16 limit",
             nb_pts);
        return {};
    }
    if (glb_norms.size() != nb_pts)
        glb_norms.assign(nb_pts, {0.0, 0.0, 1.0});

    uint32_t skin_ok3;
    if (is_skinned) {
        skin_ok3 = 0x00220000;
        if (!raw.empty() && orig_geo.skin_size > 0 &&
            36 + orig_geo.skin_size + 4 <= raw.size())
            skin_ok3 = rd_u32(raw.data() + 36 + orig_geo.skin_size);
    } else {
        skin_ok3 = orig_geo.skin_ok3;
    }
    bool has_normals = skin_ok3 != 0;

    // UVs (dedup).
    std::unordered_map<std::string, size_t> uv_map;
    std::vector<std::array<double, 2>> jade_uvs;
    std::vector<uint32_t> glb_to_uv;
    for (const auto& uv : glb_uvs) {
        std::string k = key2(pyround_dec(uv[0], 6), pyround_dec(uv[1], 6));
        auto it = uv_map.find(k);
        if (it == uv_map.end()) {
            uv_map[k] = jade_uvs.size();
            jade_uvs.push_back({uv[0], uv[1]});
        }
        glb_to_uv.push_back(uint32_t(uv_map[k]));
    }
    while (glb_to_uv.size() < nb_pts) glb_to_uv.push_back(0);
    uint32_t nb_uvs = uint32_t(jade_uvs.size());

    // Elements grouped positionally against the ORIGINAL matIds.
    size_t n_orig_elems = orig_geo.elements.size() / 2;
    std::vector<int> elem_ids;
    {
        std::set<int> uniq(glb_face_elems.begin(), glb_face_elems.end());
        elem_ids.assign(uniq.begin(), uniq.end());
    }
    std::unordered_map<int, size_t> elem_remap;
    for (size_t i = 0; i < elem_ids.size(); ++i) elem_remap[elem_ids[i]] = i;
    size_t nb_elems = elem_ids.size();
    std::vector<std::vector<size_t>> faces_by_elem(nb_elems);
    for (size_t fi = 0; fi < glb_face_elems.size(); ++fi)
        faces_by_elem[elem_remap[glb_face_elems[fi]]].push_back(fi);
    std::vector<std::pair<uint32_t, uint32_t>> elements;   // (nTri, matId)
    uint32_t clamped_count = 0;
    for (size_t i = 0; i < nb_elems; ++i) {
        uint32_t mid;
        if (i < n_orig_elems) {
            mid = orig_geo.elements[i * 2 + 1];
        } else if (n_orig_elems > 0) {
            mid = orig_geo.elements[(n_orig_elems - 1) * 2 + 1];
            ++clamped_count;
        } else {
            mid = uint32_t(elem_ids[i]);
        }
        elements.push_back({uint32_t(faces_by_elem[i].size()), mid});
    }
    if (clamped_count)
        LOGF("  WARNING: %u new element(s) clamped to matId=%u (the actor's "
             "multi-material has only %zu slot(s); new geometry will share "
             "the last shipped material)", clamped_count,
             orig_geo.elements[(n_orig_elems - 1) * 2 + 1], n_orig_elems);

    // Skin.
    std::vector<uint8_t> skin;
    size_t nbones = 0;
    std::unordered_map<uint16_t, std::vector<std::pair<uint16_t, uint16_t>>>
        influences;
    if (is_skinned) {
        const auto& orig_bones = orig_geo.skin_bones;
        nbones = orig_bones.size();
        std::vector<std::vector<std::pair<uint16_t, uint16_t>>> bone_wl(
            nbones);

        std::map<int, int> joint_map;
        std::map<int, std::string> joint_src;
        compute_bone_map(glb_mesh, orig_geo, gizmo_names, joint_map,
                         joint_src);
        bool had_auto = !joint_map.empty();
        if (!opts.bone_map.empty()) {
            for (const auto& kv : opts.bone_map) {
                joint_map[kv.first] = kv.second;
                auto sit = opts.bone_map_source.find(kv.first);
                joint_src[kv.first] =
                    sit != opts.bone_map_source.end() && !sit->second.empty()
                        ? sit->second : "user";
            }
        }
        int rb_bone = 0;
        if (opts.has_rigid_bind_bone)
            rb_bone = std::max(0, std::min(int(nbones) - 1,
                                           opts.rigid_bind_bone));
        if (rb_bone != 0) {
            std::string rb_name;
            int gz = orig_bones[size_t(rb_bone)].bone_idx;
            if (!gizmo_names.empty() && gz >= 0 &&
                size_t(gz) < gizmo_names.size())
                rb_name = gizmo_names[size_t(gz)];
            else {
                char nb[24];
                std::snprintf(nb, sizeof nb, "bone_%d", rb_bone);
                rb_name = nb;
            }
            LOGF("  rigid-bind fallback: orig bone[%d] %s", rb_bone,
                 rb_name.c_str());
        }
        if (!joint_map.empty())
            LOGF("  bone map: %zu GLB joints -> original bones (%s)",
                 joint_map.size(),
                 !opts.bone_map.empty() ? "explicit + auto" : "auto");
        else
            LOGF("  bone map: none available \xe2\x80\x94 unmapped joints "
                 "will be rigid-bound to bone %d (deformation will likely "
                 "be wrong)", rb_bone);
        (void)had_auto;

        std::set<int> unmapped_seen;
        auto map_joint = [&](int j) {
            auto it = joint_map.find(j);
            if (it == joint_map.end()) {
                unmapped_seen.insert(j);
                return rb_bone;
            }
            return std::min(std::max(it->second, 0), int(nbones) - 1);
        };
        const std::set<int>& joint_drops_set = opts.bone_drops;
        std::map<int, int> drop_target_map;
        for (const auto& kv : opts.drop_targets)
            drop_target_map[kv.first] =
                std::min(std::max(kv.second, 0), int(nbones) - 1);
        if (!drop_target_map.empty()) {
            std::map<int, int> by_bone;
            for (const auto& kv : drop_target_map) by_bone[kv.second] += 1;
            std::string summary;
            for (const auto& kv : by_bone) {
                if (!summary.empty()) summary += ", ";
                char sb[96];
                int gz = orig_bones[size_t(kv.first)].bone_idx;
                std::string bn;
                if (!gizmo_names.empty() && gz >= 0 &&
                    size_t(gz) < gizmo_names.size())
                    bn = gizmo_names[size_t(gz)];
                else {
                    std::snprintf(sb, sizeof sb, "%d", kv.first);
                    bn = sb;
                }
                std::snprintf(sb, sizeof sb, "%d drop(s)->bone[%d] %s",
                              kv.second, kv.first, bn.c_str());
                summary += sb;
            }
            LOGF("  per-drop rigid targets: %s", summary.c_str());
        }

        // Per-joint accumulators (insertion-ordered like the Python dicts).
        std::vector<int> joint_order;
        std::unordered_map<int, uint32_t> joint_pre_verts;
        std::unordered_map<int, double> joint_pre_w;
        double total_pre = 0.0;
        uint32_t verts_all_dropped = 0;
        if (glb_mesh.has_joints && glb_mesh.has_weights) {
            for (uint32_t vi = 0; vi < nb_pts; ++vi) {
                const auto& js = glb_mesh.joints[vi];
                const auto& ws = glb_mesh.weights[vi];
                std::vector<std::pair<int, double>> kept, dropped;
                for (int k = 0; k < 4; ++k) {
                    double w = ws[size_t(k)];
                    if (w <= 1e-6) continue;
                    int j_raw = js[size_t(k)];
                    if (joint_pre_w.find(j_raw) == joint_pre_w.end())
                        joint_order.push_back(j_raw);
                    joint_pre_verts[j_raw] += 1;
                    joint_pre_w[j_raw] += w;
                    total_pre += w;
                    if (joint_drops_set.count(j_raw)) {
                        dropped.push_back({j_raw, w});
                        continue;
                    }
                    kept.push_back({j_raw, w});
                }
                if (kept.empty()) {
                    if (!joint_drops_set.empty()) ++verts_all_dropped;
                    int tgt = rb_bone;
                    if (!dropped.empty()) {
                        int dom_j = dropped[0].first;
                        double dom_w = dropped[0].second;
                        for (const auto& dw : dropped)
                            if (dw.second > dom_w) {
                                dom_w = dw.second;
                                dom_j = dw.first;
                            }
                        auto tit = drop_target_map.find(dom_j);
                        if (tit != drop_target_map.end()) tgt = tit->second;
                    }
                    uint16_t pond_full = pond_of(1.0);
                    bone_wl[size_t(tgt)].push_back(
                        {uint16_t(vi), pond_full});
                    influences[uint16_t(vi)].push_back(
                        {orig_bones[size_t(tgt)].bone_idx, pond_full});
                    continue;
                }
                double kept_sum = 0.0;
                for (const auto& kw : kept) kept_sum += kw.second;
                if (kept_sum > 0 && kept_sum != 1.0)
                    for (auto& kw : kept) kw.second /= kept_sum;
                for (const auto& kw : kept) {
                    int bi = map_joint(kw.first);
                    uint16_t pond = pond_of(kw.second);
                    if (pond == 0) continue;
                    bone_wl[size_t(bi)].push_back({uint16_t(vi), pond});
                    influences[uint16_t(vi)].push_back(
                        {orig_bones[size_t(bi)].bone_idx, pond});
                }
            }
        } else {
            bool auto_rig_applied = false;
            if (opts.auto_rig) {
                auto bone_centroids = orig_bone_centroids(orig_geo);
                std::vector<std::pair<int, std::array<double, 3>>>
                    centroid_bones;
                for (size_t b = 0; b < bone_centroids.size(); ++b)
                    if (bone_centroids[b].first)
                        centroid_bones.push_back(
                            {int(b), bone_centroids[b].second});
                if (!centroid_bones.empty()) {
                    auto_rig_applied = true;
                    LOGF("  foreign-mesh rebuild: GLB skinless \xe2\x80\x94 "
                         "auto-rigging %u verts to nearest of %zu "
                         "centroid-bearing bones", nb_pts,
                         centroid_bones.size());
                    for (uint32_t vi = 0; vi < nb_pts; ++vi) {
                        const auto& v = glb_verts[vi];
                        std::vector<std::pair<double, int>> scored;
                        for (const auto& bc : centroid_bones) {
                            double dx = v[0] - bc.second[0];
                            double dy = v[1] - bc.second[1];
                            double dz = v[2] - bc.second[2];
                            scored.push_back(
                                {dx * dx + dy * dy + dz * dz, bc.first});
                        }
                        std::sort(scored.begin(), scored.end());
                        if (scored.size() > 4) scored.resize(4);
                        std::vector<double> invs;
                        double s = 0.0;
                        for (const auto& sc : scored) {
                            double inv = 1.0 / (std::sqrt(sc.first) + 1e-6);
                            invs.push_back(inv);
                            s += inv;
                        }
                        if (s <= 0) continue;
                        for (size_t k = 0; k < scored.size(); ++k) {
                            int bi = std::min(
                                std::max(scored[k].second, 0),
                                int(nbones) - 1);
                            uint16_t pond = pond_of(invs[k] / s);
                            if (pond == 0) continue;
                            bone_wl[size_t(bi)].push_back(
                                {uint16_t(vi), pond});
                            influences[uint16_t(vi)].push_back(
                                {orig_bones[size_t(bi)].bone_idx, pond});
                        }
                    }
                } else {
                    log.push_back(
                        "  auto_rig requested but no orig bone has a "
                        "computable centroid \xe2\x80\x94 falling back to "
                        "rigid bind");
                }
            }
            if (!auto_rig_applied) {
                LOGF("  foreign-mesh rebuild: GLB carries no skin \xe2\x80\x94 "
                     "binding the whole mesh rigidly to bone %d", rb_bone);
                uint16_t full = pond_of(1.0);
                bone_wl[size_t(rb_bone)].clear();
                for (uint32_t vi = 0; vi < nb_pts; ++vi)
                    bone_wl[size_t(rb_bone)].push_back({uint16_t(vi), full});
                influences.clear();
                uint16_t g_rb = orig_bones[size_t(rb_bone)].bone_idx;
                for (uint32_t vi = 0; vi < nb_pts; ++vi)
                    influences[uint16_t(vi)] = {{g_rb, full}};
            }
        }

        // Per-joint action table (log only).
        const auto& jn_list = glb_mesh.joint_names;
        if (!joint_pre_w.empty()) {
            std::vector<int> ranked = joint_order;
            std::stable_sort(ranked.begin(), ranked.end(),
                             [&](int a, int b) {
                                 return joint_pre_w[a] > joint_pre_w[b];
                             });
            log.push_back(
                "  per-joint actions (sorted by pre-drop weight share):");
            for (int j : ranked) {
                double share = total_pre > 0
                                   ? joint_pre_w[j] / total_pre * 100.0 : 0.0;
                std::string jn =
                    (j >= 0 && size_t(j) < jn_list.size())
                        ? jn_list[size_t(j)] : "";
                if (jn.empty() && !(j >= 0 && size_t(j) < jn_list.size())) {
                    char nb[24];
                    std::snprintf(nb, sizeof nb, "joint_%d", j);
                    jn = nb;
                }
                std::string action;
                if (joint_drops_set.count(j)) {
                    action = "-> DROPPED (weights renormalised onto rest)";
                } else if (joint_map.count(j)) {
                    int bi = std::min(std::max(joint_map[j], 0),
                                      int(nbones) - 1);
                    std::string src = joint_src.count(j) ? joint_src[j] : "?";
                    int gz = orig_bones[size_t(bi)].bone_idx;
                    std::string bn;
                    if (!gizmo_names.empty() && gz >= 0 &&
                        size_t(gz) < gizmo_names.size() &&
                        !gizmo_names[size_t(gz)].empty())
                        bn = gizmo_names[size_t(gz)];
                    else {
                        char nb[24];
                        std::snprintf(nb, sizeof nb, "bone_%d", bi);
                        bn = nb;
                    }
                    char ab[128];
                    std::snprintf(ab, sizeof ab, "-> bone[%d] %s (%s)", bi,
                                  bn.c_str(), src.c_str());
                    action = ab;
                } else {
                    char ab[64];
                    std::snprintf(ab, sizeof ab,
                                  "-> UNMAPPED (rigid-bound to bone %d)",
                                  rb_bone);
                    action = ab;
                }
                LOGF("    joint[%3d] %-32.32s %5uv  %5.1f%%   %s", j,
                     jn.c_str(), joint_pre_verts[j], share, action.c_str());
            }
        }
        if (!unmapped_seen.empty()) {
            std::string names;
            int shown = 0;
            for (int j : unmapped_seen) {
                if (shown == 6) break;
                if (shown) names += ", ";
                char nb[64];
                std::string jn =
                    (j >= 0 && size_t(j) < jn_list.size())
                        ? jn_list[size_t(j)] : "";
                if (!(j >= 0 && size_t(j) < jn_list.size())) {
                    std::snprintf(nb, sizeof nb, "joint_%d", j);
                    jn = nb;
                }
                std::snprintf(nb, sizeof nb, "%d:%s", j, jn.c_str());
                names += nb;
                ++shown;
            }
            LOGF("  WARNING: %zu GLB joint(s) had no mapping and were "
                 "rigid-bound to bone %d: %s%s", unmapped_seen.size(),
                 rb_bone, names.c_str(),
                 unmapped_seen.size() > 6 ? " \xe2\x80\xa6" : "");
        }
        if (!joint_drops_set.empty()) {
            std::string where;
            if (!drop_target_map.empty()) {
                std::set<int> distinct;
                for (const auto& kv : drop_target_map)
                    distinct.insert(kv.second);
                char wb[128];
                std::snprintf(wb, sizeof wb,
                              "their dominant drop's target (%zu distinct "
                              "bone(s)) or fallback bone %d",
                              distinct.size(), rb_bone);
                where = wb;
            } else {
                char wb[32];
                std::snprintf(wb, sizeof wb, "bone %d", rb_bone);
                where = wb;
            }
            LOGF("  drops applied: %zu joint(s); %u vertex/-ices lost every "
                 "influence and were rigid-bound to %s",
                 joint_drops_set.size(), verts_all_dropped, where.c_str());
        }

        // T4.2 rest-pose diagnostic (log only).
        if (opts.diagnose_rest_pose && !joint_map.empty()) {
            if (glb_mesh.joint_ibms.empty()) {
                log.push_back(
                    "  rest-pose diagnostic requested but GLB carries no "
                    "inverseBindMatrices accessor \xe2\x80\x94 skipped");
            } else {
                std::map<int, std::vector<std::pair<int, int>>> inv;
                for (const auto& kv : joint_map) {
                    int j = kv.first;
                    int bi = std::min(std::max(kv.second, 0),
                                      int(nbones) - 1);
                    if (joint_drops_set.count(j)) continue;
                    std::string src = joint_src.count(j) ? joint_src[j] : "";
                    int pri = src == "name" ? 0
                              : (src == "geometric" ? 1 : 2);
                    inv[bi].push_back({pri, j});
                }
                struct Delta {
                    double dist;
                    int bi, j;
                    std::string bn, jn;
                    std::array<double, 3> gp, op;
                };
                std::vector<Delta> deltas;
                for (auto& kv : inv) {
                    std::sort(kv.second.begin(), kv.second.end());
                    int j = kv.second[0].second;
                    if (!(j >= 0 &&
                          size_t(j) < glb_mesh.joint_ibms.size()))
                        continue;
                    auto ibm_jade =
                        gltf_to_jade_matrix(glb_mesh.joint_ibms[size_t(j)]);
                    auto glb_pos = bone_world_from_ibm(ibm_jade);
                    std::array<double, 16> ob{};
                    for (int k = 0; k < 16; ++k)
                        ob[size_t(k)] = double(
                            orig_geo.skin_bones[size_t(kv.first)]
                                .bind_matrix[size_t(k)]);
                    auto orig_pos = bone_world_from_ibm(ob);
                    double dx = glb_pos[0] - orig_pos[0];
                    double dy = glb_pos[1] - orig_pos[1];
                    double dz = glb_pos[2] - orig_pos[2];
                    Delta d;
                    d.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    d.bi = kv.first;
                    d.j = j;
                    int gz = orig_geo.skin_bones[size_t(kv.first)].bone_idx;
                    if (!gizmo_names.empty() && gz >= 0 &&
                        size_t(gz) < gizmo_names.size() &&
                        !gizmo_names[size_t(gz)].empty())
                        d.bn = gizmo_names[size_t(gz)];
                    else {
                        char nb[24];
                        std::snprintf(nb, sizeof nb, "bone_%d", kv.first);
                        d.bn = nb;
                    }
                    d.jn = size_t(j) < jn_list.size() ? jn_list[size_t(j)]
                                                      : "";
                    if (d.jn.empty() && size_t(j) >= jn_list.size()) {
                        char nb[24];
                        std::snprintf(nb, sizeof nb, "joint_%d", j);
                        d.jn = nb;
                    }
                    d.gp = glb_pos;
                    d.op = orig_pos;
                    deltas.push_back(d);
                }
                if (!deltas.empty()) {
                    std::stable_sort(deltas.begin(), deltas.end(),
                                     [](const Delta& a, const Delta& b) {
                                         return a.dist > b.dist;
                                     });
                    double max_dist = deltas[0].dist;
                    LOGF("  rest-pose diagnostic \xe2\x80\x94 comparing %zu "
                         "mapped bone(s):", deltas.size());
                    LOGF("    %7s  %-24s  %-24s  %-26s  %-26s", "dist",
                         "orig bone", "glb joint", "glb pos (jade)",
                         "orig pos");
                    size_t shown = std::min<size_t>(deltas.size(), 25);
                    for (size_t k = 0; k < shown; ++k) {
                        const Delta& d = deltas[k];
                        LOGF("    %7.3f  [%3d] %-18.18s  [%3d] %-18.18s  "
                             "(%+7.2f,%+7.2f,%+7.2f)  (%+7.2f,%+7.2f,%+7.2f)",
                             d.dist, d.bi, d.bn.c_str(), d.j, d.jn.c_str(),
                             d.gp[0], d.gp[1], d.gp[2], d.op[0], d.op[1],
                             d.op[2]);
                    }
                    if (deltas.size() > 25)
                        LOGF("    \xe2\x80\xa6" "and %zu more",
                             deltas.size() - 25);
                    if (max_dist > 0.5) {
                        LOGF("  WARNING: largest rest-pose delta is %.2f "
                             "\xe2\x80\x94 GLB and orig rest poses do NOT "
                             "match. Skinning will render at rest pose but "
                             "animation may produce distorted deformation.",
                             max_dist);
                        log.push_back(
                            "  To fix: retarget the GLB to the orig "
                            "skeleton's rest pose in Blender (apply armature "
                            "transforms, match bind positions) before "
                            "re-exporting.");
                    } else if (max_dist > 0.1) {
                        LOGF("  rest poses are close but not identical (max "
                             "delta %.3f) \xe2\x80\x94 minor animation drift "
                             "possible", max_dist);
                    } else {
                        LOGF("  rest poses are well aligned (max delta %.3f)",
                             max_dist);
                    }
                }
            }
        }

        put_u16v(skin, orig_geo.skin_flags);
        put_u16v(skin, uint16_t(nbones));
        for (size_t bi = 0; bi < nbones; ++bi) {
            const GeoBone& ob = orig_geo.skin_bones[bi];
            put_u16v(skin, ob.bone_idx);
            put_u16v(skin, uint16_t(bone_wl[bi].size()));
            for (int k = 0; k < 16; ++k)
                put_f32v(skin, double(ob.bind_matrix[size_t(k)]));
            put_i32v(skin, ob.matrix_type);
            for (const auto& vp : bone_wl[bi]) {
                put_u16v(skin, vp.first);
                put_u16v(skin, vp.second);
            }
        }
    }

    // Vertex colours (opt-in; foreign keeps glb order 1:1).
    std::vector<uint8_t> color_bytes;
    if (opts.import_vertex_colors && glb_mesh.has_colors) {
        uint32_t nn = std::min(uint32_t(glb_mesh.colors.size()), nb_pts);
        for (uint32_t i = 0; i < nn; ++i) {
            const auto& c = glb_mesh.colors[i];
            put_u32v(color_bytes, (uint32_t(c[3] & 0xFF) << 24) |
                                      (uint32_t(c[0] & 0xFF) << 16) |
                                      (uint32_t(c[1] & 0xFF) << 8) |
                                      uint32_t(c[2] & 0xFF));
        }
        for (uint32_t i = nn; i < nb_pts; ++i)
            put_u32v(color_bytes, 0xFFFFFFFFu);
        LOGF("  vertex colors: writing %u from GLB COLOR_0", nb_pts);
    }
    uint32_t has_abs = color_bytes.empty() ? 0 : 1;
    uint32_t abs_count = nb_pts;   // Sablier profile keeps nb_pts either way

    // Assemble.
    std::vector<uint8_t> result;
    if (is_skinned) {
        put_u32v(result, orig_geo.version);
        put_u32v(result, orig_geo.flags1);
        put_u32v(result, orig_geo.flags2);
        put_u32v(result, nb_pts);
        put_u32v(result, abs_count);
        put_u32v(result, has_abs);
        put_u32v(result, nb_uvs);
        put_u32v(result, uint32_t(nb_elems));
        put_u32v(result, 0xC0DE2002u);
        result.insert(result.end(), skin.begin(), skin.end());
        put_u32v(result, skin_ok3);
    } else {
        put_u32v(result, orig_geo.version);
        put_u32v(result, orig_geo.flags1);
        put_u32v(result, orig_geo.flags2);
        put_u32v(result, nb_pts);
        put_u32v(result, abs_count);
        put_u32v(result, has_abs);
        put_u32v(result, nb_uvs);
        put_u32v(result, uint32_t(nb_elems));
        put_u32v(result, 0);
        put_u32v(result, skin_ok3);
    }
    for (const auto& v : glb_verts) {
        put_f32v(result, v[0]);
        put_f32v(result, v[1]);
        put_f32v(result, v[2]);
    }
    if (has_normals)
        for (const auto& n : glb_norms) {
            put_f32v(result, n[0]);
            put_f32v(result, n[1]);
            put_f32v(result, n[2]);
        }
    result.insert(result.end(), color_bytes.begin(), color_bytes.end());
    for (const auto& uv : jade_uvs) {
        put_f32v(result, uv[0]);
        put_f32v(result, uv[1]);
    }
    for (const auto& e : elements) {
        put_u32v(result, e.first);
        put_u32v(result, e.second);
    }
    std::vector<std::array<uint32_t, 3>> face_uvs;
    for (const auto& f : glb_faces)
        face_uvs.push_back({glb_to_uv[f[0]], glb_to_uv[f[1]],
                            glb_to_uv[f[2]]});
    for (size_t ei = 0; ei < nb_elems; ++ei)
        for (size_t fi : faces_by_elem[ei]) {
            const auto& f = glb_faces[fi];
            const auto& u = face_uvs[fi];
            put_u16v(result, uint16_t(f[0]));
            put_u16v(result, uint16_t(f[1]));
            put_u16v(result, uint16_t(f[2]));
            put_u16v(result, uint16_t(u[0]));
            put_u16v(result, uint16_t(u[1]));
            put_u16v(result, uint16_t(u[2]));
            put_u32v(result, 0);
        }

    uint32_t orig_magic = 0;
    bool have_magic = orig_cooked_vb_magic(raw, orig_magic);
    uint32_t trail_magic;
    if (is_skinned)
        trail_magic = 3;
    else if (have_magic && (orig_magic == 6 || orig_magic == 8))
        trail_magic = orig_magic;
    else if (has_normals && have_magic &&
             (orig_magic == 2 || orig_magic == 5))
        trail_magic = orig_magic;
    else if (has_normals)
        trail_magic = 2;
    else
        trail_magic = 0;
    std::vector<std::array<double, 3>> tangents;
    const std::vector<std::array<double, 3>>* tangent_ptr = nullptr;
    uint32_t original_stride =
        gltf::orig_skinned_vb_stride(raw.data(), raw.size());
    if (is_skinned && has_normals && original_stride == 64) {
        tangents = compute_vertex_tangents(glb_verts, glb_norms, jade_uvs,
                                           glb_faces, face_uvs);
        tangent_ptr = &tangents;
        LOGF("  original is stride-64 (normal-mapped): emitting per-vertex "
             "tangents in TEXCOORD1 to prevent bump artifacts");
    }
    std::vector<uint8_t> trailing = regenerate_trailing_block(
        glb_verts, glb_norms, jade_uvs, glb_faces, face_uvs, faces_by_elem,
        elements, influences, trail_magic, tangent_ptr, log);
    if (trailing.empty()) return {};
    result.insert(result.end(), trailing.begin(), trailing.end());

    {
        char skinned_txt[32];
        if (is_skinned)
            std::snprintf(skinned_txt, sizeof skinned_txt, "%zu bones",
                          nbones);
        else
            std::snprintf(skinned_txt, sizeof skinned_txt, "unskinned");
        LOGF("  foreign-mesh rebuild: %uv / %uuv / %zutri / %s / %zu "
             "element(s)", nb_pts, nb_uvs, glb_faces.size(), skinned_txt,
             nb_elems);
    }
    return result;
}

// _update_host_gao_rli.
std::vector<uint8_t> update_host_gao_rli(
    std::vector<uint8_t> patched, uint32_t sub_key, const GeoInfo& orig_geo,
    const std::vector<uint8_t>& new_binary, const GlbPatchMesh& glb_mesh,
    const std::vector<SubEntry>& pre_subs, bool import_vertex_colors,
    std::vector<std::string>& log) {
    GeoInfo new_geo = parse_geometry(new_binary.data(), new_binary.size());
    if (!new_geo.ok || new_geo.vertices.empty()) return patched;
    size_t new_nb = new_geo.nb_points;
    std::vector<std::array<double, 3>> new_base_verts;
    for (size_t i = 0; i + 2 < new_geo.vertices.size(); i += 3)
        new_base_verts.push_back({double(new_geo.vertices[i]),
                                  double(new_geo.vertices[i + 1]),
                                  double(new_geo.vertices[i + 2])});

    std::vector<SubEntry> subs_after = walk_sub_entries(patched);
    std::vector<const SubEntry*> host_gaos;
    for (const SubEntry& s : subs_after) {
        if (s.ext != ".gao") continue;
        GaoInfo gi;
        try { gi = parse_gao_full(s.data.data(), s.data.size()); }
        catch (...) { continue; }
        if (gi.ok && gi.vis_read && gi.gro_key == sub_key)
            host_gaos.push_back(&s);
    }
    if (host_gaos.empty()) return patched;

    std::vector<Rgba4> new_colors;
    const char* csrc;
    bool have_colors = false;
    if (import_vertex_colors && glb_mesh.has_colors) {
        std::vector<std::array<int, 4>> ic;
        if (glb_mesh.colors.size() == new_nb)
            ic = glb_mesh.colors;
        else
            ic = colors_by_position(glb_mesh.vertices, glb_mesh.colors,
                                    new_base_verts);
        for (const auto& c : ic)
            new_colors.push_back({double(c[0]), double(c[1]), double(c[2]),
                                  double(c[3])});
        have_colors = !new_colors.empty();
        csrc = "GLB COLOR_0";
    } else {
        OrigRliColors oc = original_rli_colors(pre_subs, sub_key, orig_geo,
                                               new_base_verts);
        if (oc.ok) {
            new_colors = oc.colors;
            have_colors = true;
        }
        csrc = "original baked RLI (remapped)";
    }
    if (!have_colors) {
        log.push_back("  host GAO RLI: no colour source \xe2\x80\x94 "
                      "instance table left as-is");
        return patched;
    }

    struct Update {
        size_t offset;
        size_t old_len;
        std::vector<uint8_t> payload;
    };
    std::vector<Update> updates;
    for (const SubEntry* s2 : host_gaos) {
        std::vector<uint8_t> new_gao = rewrite_gao_instance_colors(
            s2->data.data(), s2->data.size(), sub_key, orig_geo.nb_points,
            new_colors, new_binary.data(), new_binary.size());
        if (!new_gao.empty() && new_gao != s2->data)
            updates.push_back({s2->offset, s2->data.size(),
                               std::move(new_gao)});
    }
    std::sort(updates.begin(), updates.end(),
              [](const Update& a, const Update& b) {
                  return a.offset > b.offset;
              });
    for (const Update& u : updates) {
        size_t pstart = u.offset + 12;
        std::vector<uint8_t> next;
        next.reserve(patched.size() + u.payload.size());
        next.insert(next.end(), patched.begin(),
                    patched.begin() + long(pstart));
        next.insert(next.end(), u.payload.begin(), u.payload.end());
        next.insert(next.end(),
                    patched.begin() + long(pstart + u.old_len),
                    patched.end());
        matput32(next, u.offset - 4, uint32_t(u.payload.size() + 4));
        patched = std::move(next);
    }
    if (!updates.empty())
        LOGF("  updated %zu host GAO instance colour table(s) to %zu verts "
             "(%s)", updates.size(), new_colors.size(), csrc);
    else
        log.push_back("  host GAO has no resizable instance table \xe2\x80\x94 "
                      "GEO body colour is used directly");
    return patched;
}

}  // namespace

std::array<double, 16> gltf_to_jade_matrix_pub(
    const std::array<double, 16>& v) {
    return gltf_to_jade_matrix(v);
}
std::array<double, 3> bone_world_from_ibm_pub(
    const std::array<double, 16>& m) {
    return bone_world_from_ibm(m);
}

// ── mesh_swap.analyze_bone_mapping ─────────────────────────────────────────

AnalyzeBoneResult analyze_bone_mapping(const std::vector<uint8_t>& dec,
                                       uint32_t geo_key,
                                       const std::vector<uint8_t>& glb_bytes) {
    AnalyzeBoneResult r;
    char b[96];
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = pick_geo_sub(subs, geo_key);
    if (target == nullptr) {
        std::snprintf(b, sizeof b,
                      "No .geo sub-entry 0x%08x in entry", geo_key);
        r.error = b;
        return r;
    }
    GeoInfo orig_geo = parse_geometry(target->data.data(),
                                      target->data.size());
    if (!orig_geo.ok) {
        r.error = "Could not parse original geometry";
        return r;
    }
    std::vector<GlbPatchMesh> meshes = parse_glb_meshes(glb_bytes);
    if (meshes.empty()) {
        r.error = "No meshes found in GLB";
        return r;
    }
    const GlbPatchMesh& glb = meshes[0];

    ForeignCheck fc = glb_is_foreign(glb, orig_geo, true, geo_key);
    r.is_foreign = fc.foreign;
    r.foreign_reason = fc.reason;

    std::vector<std::string> gizmo_names =
        resolve_skeleton_gizmos(subs, true, geo_key);
    compute_bone_map(glb, orig_geo, gizmo_names, r.auto_map,
                     r.auto_map_source);
    r.glb_joint_names = glb.joint_names;

    const auto& bones = orig_geo.skin_bones;
    for (const GeoBone& bo : bones) {
        int gz = bo.bone_idx;
        std::string nm;
        if (gz >= 0 && size_t(gz) < gizmo_names.size() &&
            !gizmo_names[size_t(gz)].empty())
            nm = gizmo_names[size_t(gz)];
        else {
            std::snprintf(b, sizeof b, "bone_%03d", gz);
            nm = b;
        }
        if (nm.size() >= 4 && nm.compare(nm.size() - 4, 4, ".gao") == 0)
            nm.resize(nm.size() - 4);
        r.dp_bone_names.push_back(nm);
    }

    // _glb_joint_stats.
    size_t n_joints = glb.joint_names.size();
    r.joint_stats.assign(n_joints, JointStats{});
    if (glb.has_joints && glb.has_weights && n_joints > 0) {
        std::vector<double> sums(n_joints, 0.0);
        double total = 0.0;
        for (size_t vi = 0; vi < glb.joints.size(); ++vi) {
            const auto& js = glb.joints[vi];
            const auto& ws = glb.weights[vi];
            int best = -1;
            double best_w = -1.0;
            for (int k = 0; k < 4; ++k) {
                int j = js[size_t(k)];
                double w = ws[size_t(k)];
                if (w > 1e-6 && j >= 0 && size_t(j) < n_joints) {
                    r.joint_stats[size_t(j)].vertex_count += 1;
                    sums[size_t(j)] += w;
                    total += w;
                }
                if (w > 1e-6 && w > best_w) {
                    best_w = w;
                    best = j;
                }
            }
            if (best >= 0 && size_t(best) < n_joints)
                r.joint_stats[size_t(best)].dominant_count += 1;
        }
        if (total > 0.0)
            for (size_t j = 0; j < n_joints; ++j)
                r.joint_stats[j].weight_share = sums[j] / total;
    }

    // _compute_centroids (recentered by each mesh's own vertex centre).
    size_t n_bones = bones.size();
    r.joint_centroids.assign(n_joints, {false, {0, 0, 0}});
    r.bone_centroids.assign(n_bones, {false, {0, 0, 0}});
    if (glb.has_joints && glb.has_weights && !glb.vertices.empty()) {
        std::unordered_map<int, std::array<double, 4>> acc;  // sum + count
        for (size_t vi = 0; vi < glb.vertices.size(); ++vi) {
            const auto& js = glb.joints[vi];
            const auto& ws = glb.weights[vi];
            int best = -1;
            double best_w = -1.0;
            for (int k = 0; k < 4; ++k)
                if (ws[size_t(k)] > best_w) {
                    best_w = ws[size_t(k)];
                    best = js[size_t(k)];
                }
            if (best < 0) continue;
            auto& a = acc[best];
            a[0] += glb.vertices[vi][0];
            a[1] += glb.vertices[vi][1];
            a[2] += glb.vertices[vi][2];
            a[3] += 1.0;
        }
        for (const auto& kv : acc)
            if (kv.first >= 0 && size_t(kv.first) < n_joints &&
                kv.second[3] > 0)
                r.joint_centroids[size_t(kv.first)] = {
                    true, {kv.second[0] / kv.second[3],
                           kv.second[1] / kv.second[3],
                           kv.second[2] / kv.second[3]}};
    }
    {
        // Ordered like the Python vertex_weights dict (first-seen vert order)
        // so per-cluster float sums accumulate identically.
        std::vector<uint32_t> vert_order;
        std::unordered_map<uint32_t,
                           std::vector<std::pair<int, double>>> vw;
        for (size_t bli = 0; bli < bones.size(); ++bli)
            for (const auto& wp : bones[bli].weights) {
                auto it = vw.find(wp.first);
                if (it == vw.end()) vert_order.push_back(wp.first);
                vw[wp.first].push_back(
                    {int(bli), double(wp.second) / 65535.0});
            }
        size_t nverts = orig_geo.vertices.size() / 3;
        std::unordered_map<int, std::array<double, 4>> acc;
        for (uint32_t vert : vert_order) {
            if (vert >= nverts) continue;
            int best = -1;
            double best_w = -1.0;
            for (const auto& bw : vw[vert])
                if (bw.second > best_w) {
                    best_w = bw.second;
                    best = bw.first;
                }
            if (best < 0) continue;
            auto& a = acc[best];
            a[0] += double(orig_geo.vertices[vert * 3]);
            a[1] += double(orig_geo.vertices[vert * 3 + 1]);
            a[2] += double(orig_geo.vertices[vert * 3 + 2]);
            a[3] += 1.0;
        }
        for (const auto& kv : acc)
            if (kv.first >= 0 && size_t(kv.first) < n_bones &&
                kv.second[3] > 0)
                r.bone_centroids[size_t(kv.first)] = {
                    true, {kv.second[0] / kv.second[3],
                           kv.second[1] / kv.second[3],
                           kv.second[2] / kv.second[3]}};
    }
    // Recenter + diagonal.
    auto centre_of = [](auto get, size_t n) {
        std::array<double, 3> c{0, 0, 0};
        if (n == 0) return c;
        for (size_t i = 0; i < n; ++i) {
            auto v = get(i);
            c[0] += v[0];
            c[1] += v[1];
            c[2] += v[2];
        }
        for (int k = 0; k < 3; ++k) c[size_t(k)] /= double(n);
        return c;
    };
    std::array<double, 3> glb_centre = centre_of(
        [&](size_t i) { return glb.vertices[i]; }, glb.vertices.size());
    size_t n_ov = orig_geo.vertices.size() / 3;
    std::array<double, 3> orig_centre = centre_of(
        [&](size_t i) {
            return std::array<double, 3>{
                double(orig_geo.vertices[i * 3]),
                double(orig_geo.vertices[i * 3 + 1]),
                double(orig_geo.vertices[i * 3 + 2])};
        },
        n_ov);
    for (auto& jc : r.joint_centroids)
        if (jc.first)
            for (int k = 0; k < 3; ++k)
                jc.second[size_t(k)] -= glb_centre[size_t(k)];
    for (auto& bc : r.bone_centroids)
        if (bc.first)
            for (int k = 0; k < 3; ++k)
                bc.second[size_t(k)] -= orig_centre[size_t(k)];
    {
        bool use_glb = !glb.vertices.empty();
        size_t n = use_glb ? glb.vertices.size() : n_ov;
        if (n > 0) {
            double mn[3], mx[3];
            for (int k = 0; k < 3; ++k) {
                double v0 = use_glb ? glb.vertices[0][size_t(k)]
                                    : double(orig_geo.vertices[size_t(k)]);
                mn[k] = mx[k] = v0;
            }
            for (size_t i = 0; i < n; ++i)
                for (int k = 0; k < 3; ++k) {
                    double v = use_glb
                                   ? glb.vertices[i][size_t(k)]
                                   : double(orig_geo.vertices[i * 3 +
                                                              size_t(k)]);
                    if (v < mn[k]) mn[k] = v;
                    if (v > mx[k]) mx[k] = v;
                }
            double dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
            r.mesh_diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    // _compute_rest_positions.
    r.joint_rest_positions.assign(n_joints, {false, {0, 0, 0}});
    for (size_t j = 0; j < glb.joint_ibms.size() && j < n_joints; ++j)
        r.joint_rest_positions[j] = {
            true, bone_world_from_ibm(gltf_to_jade_matrix(glb.joint_ibms[j]))};
    r.bone_rest_positions.assign(n_bones, {false, {0, 0, 0}});
    for (size_t bi = 0; bi < n_bones; ++bi) {
        std::array<double, 16> m{};
        for (int k = 0; k < 16; ++k)
            m[size_t(k)] = double(bones[bi].bind_matrix[size_t(k)]);
        r.bone_rest_positions[bi] = {true, bone_world_from_ibm(m)};
    }

    // T3.4 orig weight shares (first-seen vert order for exact float sums).
    r.orig_bone_weight_shares.assign(n_bones, 0.0);
    {
        std::vector<uint32_t> vert_order;
        std::unordered_map<uint32_t,
                           std::vector<std::pair<int, double>>> vw;
        for (size_t bli = 0; bli < bones.size(); ++bli)
            for (const auto& wp : bones[bli].weights) {
                auto it = vw.find(wp.first);
                if (it == vw.end()) vert_order.push_back(wp.first);
                vw[wp.first].push_back(
                    {int(bli), double(wp.second) / 65535.0});
            }
        double total = 0.0;
        for (uint32_t vert : vert_order)
            for (const auto& bw : vw[vert])
                if (bw.first >= 0 && size_t(bw.first) < n_bones) {
                    r.orig_bone_weight_shares[size_t(bw.first)] += bw.second;
                    total += bw.second;
                }
        if (total > 0.0)
            for (double& s : r.orig_bone_weight_shares) s /= total;
    }
    r.ok = true;
    return r;
}

// ── _patch_model_glb ───────────────────────────────────────────────────────

PatchModelResult patch_model_glb(const std::vector<uint8_t>& dec,
                                 uint32_t sub_key,
                                 const std::vector<uint8_t>& glb_bytes,
                                 const PatchModelOptions& opts,
                                 std::vector<std::string>& log) {
    PatchModelResult res;
    std::vector<GlbPatchMesh> glb_meshes = parse_glb_meshes(glb_bytes);
    if (glb_meshes.empty()) {
        log.push_back("  No meshes found in GLB");
        return res;
    }

    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* geo_sub = nullptr;
    bool is_ps2 = false;
    for (const SubEntry& s : subs) {
        if (s.key != sub_key || s.gro_null || s.gro_type != 1 ||
            s.data.empty())
            continue;
        uint32_t gro1 = 1;
        if (is_geometry_entry(s.data.data(), s.data.size(), &gro1)) {
            geo_sub = &s;
            break;
        }
        if (is_geometry_entry_ps2(s.data.data(), s.data.size(), &gro1)) {
            is_ps2 = true;
            geo_sub = &s;
            break;
        }
    }
    char b[192];
    if (is_ps2) {
        std::snprintf(b, sizeof b,
                      "  Geo 0x%08X is PS2 format (VIF packets) \xe2\x80\x94 "
                      "mesh swap is not supported on PS2 archives yet. See "
                      "docs/ps2-support.md for what's needed.", sub_key);
        log.push_back(b);
        return res;
    }
    if (geo_sub == nullptr) {
        std::snprintf(b, sizeof b, "  Geo 0x%08X not found in entry", sub_key);
        log.push_back(b);
        return res;
    }

    GeoInfo orig_geo = parse_geometry(geo_sub->data.data(),
                                      geo_sub->data.size());
    if (!orig_geo.ok) return res;

    const GlbPatchMesh& glb_mesh = glb_meshes[0];
    ForeignCheck fc = glb_is_foreign(glb_mesh, orig_geo, true, sub_key);
    std::vector<uint8_t> new_binary;
    if (fc.foreign && !opts.keep_original_skin) {
        std::snprintf(b, sizeof b,
                      "  0x%08X: foreign mesh (%s) \xe2\x80\x94 full rebuild "
                      "from GLB geometry + skin", sub_key, fc.reason.c_str());
        log.push_back(b);
        std::vector<std::string> gizmo_names =
            resolve_skeleton_gizmos(subs, true, sub_key);
        new_binary = rebuild_geo_foreign(orig_geo, geo_sub->data, glb_mesh,
                                         opts, gizmo_names, log);
    } else {
        if (opts.keep_original_skin && fc.foreign) {
            std::snprintf(b, sizeof b,
                          "  0x%08X: keep original skinning \xe2\x80\x94 "
                          "importing GLB geometry/UVs/faces/matIds onto the "
                          "original vertex array; the original skin block is "
                          "preserved verbatim (no skin re-derivation)",
                          sub_key);
            log.push_back(b);
        }
        new_binary = rebuild_geo_binary(orig_geo, geo_sub->data, glb_mesh,
                                        opts.import_vertex_colors,
                                        opts.keep_original_skin && fc.foreign,
                                        log);
    }
    if (new_binary.empty()) {
        std::snprintf(b, sizeof b, "  No changes detected in mesh 0x%08X",
                      sub_key);
        log.push_back(b);
        return res;
    }

    size_t old_size = geo_sub->data.size();
    size_t payload_start = geo_sub->offset + 12;
    std::vector<uint8_t> patched;
    patched.reserve(dec.size() + new_binary.size());
    patched.insert(patched.end(), dec.begin(),
                   dec.begin() + long(payload_start));
    patched.insert(patched.end(), new_binary.begin(), new_binary.end());
    patched.insert(patched.end(), dec.begin() + long(payload_start + old_size),
                   dec.end());
    matput32(patched, geo_sub->offset - 4, uint32_t(new_binary.size() + 4));

    LOGF("  Patched GEO 0x%08X: %zuB (sub-entry size field %zu -> %zu)",
         sub_key, new_binary.size(), old_size + 4, new_binary.size() + 4);

    try {
        patched = update_host_gao_rli(std::move(patched), sub_key, orig_geo,
                                      new_binary, glb_mesh, subs,
                                      opts.import_vertex_colors, log);
    } catch (const std::exception& e) {
        LOGF("  WARNING: host-GAO RLI colour update skipped (%s)", e.what());
    }
    res.changed = true;
    res.patched = std::move(patched);
    return res;
}

PatchSceneResult patch_scene_glb(const std::vector<uint8_t>& dec,
                                 const std::vector<uint8_t>& glb_bytes,
                                 const std::string& source_name,
                                 std::vector<std::string>& log) {
    PatchSceneResult res;
    res.patched = dec;
    std::vector<GlbPatchMesh> meshes = parse_glb_meshes(glb_bytes);
    if (meshes.empty()) {
        log.push_back("  No meshes found in scene GLB: " + source_name);
        return res;
    }

    std::vector<SubEntry> subs = walk_sub_entries(res.patched);
    for (const GlbPatchMesh& mesh : meshes) {
        // Python accepts only the string extras form beginning with "0x".
        if (mesh.jade_key_str.rfind("0x", 0) != 0) continue;
        char* end = nullptr;
        unsigned long parsed = std::strtoul(mesh.jade_key_str.c_str(), &end, 16);
        if (end == nullptr || *end != '\0') continue;
        uint32_t geo_key = static_cast<uint32_t>(parsed);

        const SubEntry* geo_sub = nullptr;
        for (const SubEntry& s : subs) {
            uint32_t gro_type = s.gro_type;
            if (s.key == geo_key && !s.gro_null && gro_type == 1 &&
                !s.data.empty() &&
                is_geometry_entry(s.data.data(), s.data.size(), &gro_type)) {
                geo_sub = &s;
                break;
            }
        }
        if (geo_sub == nullptr) {
            char b[96];
            std::snprintf(b, sizeof b,
                          "  Geo 0x%08X not found in entry, skipping", geo_key);
            log.push_back(b);
            continue;
        }
        GeoInfo orig_geo =
            parse_geometry(geo_sub->data.data(), geo_sub->data.size());
        if (!orig_geo.ok) continue;

        std::vector<uint8_t> new_binary = rebuild_geo_binary(
            orig_geo, geo_sub->data, mesh, false, false, log);
        if (new_binary.empty()) continue;

        size_t old_size = geo_sub->data.size();
        size_t payload_start = geo_sub->offset + 12;
        res.patched.erase(res.patched.begin() + long(payload_start),
                          res.patched.begin() + long(payload_start + old_size));
        res.patched.insert(res.patched.begin() + long(payload_start),
                           new_binary.begin(), new_binary.end());
        if (new_binary.size() != old_size) {
            matput32(res.patched, geo_sub->offset - 4,
                     uint32_t(new_binary.size() + 4));
            subs = walk_sub_entries(res.patched);
        }

        ++res.patch_count;
        char b[192];
        std::snprintf(b, sizeof b,
                      "  Patched GEO 0x%08X (%s): %uv \xe2\x86\x92 %zuB", geo_key,
                      mesh.name.empty() ? "?" : mesh.name.c_str(),
                      orig_geo.nb_points, new_binary.size());
        log.push_back(b);
    }

    if (res.patch_count == 0)
        log.push_back("  No geometry changes detected in GLB");
    res.changed = res.patch_count != 0;
    return res;
}

}  // namespace patcher
}  // namespace jade
