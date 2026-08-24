// Gltf.cpp — GLB container + accessor reader + mesh parse (see Gltf.hpp).
#include "jade/Gltf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace jade {
namespace gltf {

namespace {
inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}
inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline float rf32(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }
[[noreturn]] void fail(const char* m) { throw std::runtime_error(std::string("gltf: ") + m); }

uint32_t type_ncomps(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    if (t == "MAT4") return 16;
    fail("unknown accessor type");
}
}  // namespace

size_t component_size(uint32_t comp_type) {
    switch (comp_type) {
        case COMP_I8: case COMP_U8:  return 1;
        case COMP_I16: case COMP_U16: return 2;
        case COMP_U32: case COMP_F32: return 4;
        default: return 0;
    }
}

GlbDoc parse_glb(const uint8_t* d, size_t n) {
    if (!d || n < 12) fail("too short");
    if (le32(d, 0) != GLB_MAGIC) fail("bad magic");

    size_t off = 12;
    std::string json_str;
    std::vector<uint8_t> bin;
    bool have_json = false, have_bin = false;
    while (off + 8 <= n) {                          // Python: while offset < len-7
        uint32_t clen = le32(d, off);
        uint32_t ctype = le32(d, off + 4);
        off += 8;
        size_t take = clen;
        if (take > n - off) take = n - off;         // Python slice clamps to end
        if (ctype == CHUNK_JSON) {
            json_str.assign(reinterpret_cast<const char*>(d + off), take);
            have_json = true;
        } else if (ctype == CHUNK_BIN) {
            bin.assign(d + off, d + off + take);
            have_bin = true;
        }
        off += clen;
    }
    if (!have_json || !have_bin) fail("missing JSON or BIN chunk");

    GlbDoc doc;
    doc.gltf = json::parse(json_str);
    doc.bin = std::move(bin);
    return doc;
}

AccessorData read_accessor(const GlbDoc& doc, int acc_idx) {
    const json::Value* accs = doc.gltf.find("accessors");
    const json::Value* bvs = doc.gltf.find("bufferViews");
    if (!accs || !accs->is_arr() || acc_idx < 0 ||
        static_cast<size_t>(acc_idx) >= accs->arr.size())
        fail("bad accessor index");
    const json::Value& acc = accs->arr[static_cast<size_t>(acc_idx)];
    long long bv_idx = acc.get_int("bufferView", -1);
    if (!bvs || !bvs->is_arr() || bv_idx < 0 ||
        static_cast<size_t>(bv_idx) >= bvs->arr.size())
        fail("bad bufferView index");
    const json::Value& bv = bvs->arr[static_cast<size_t>(bv_idx)];

    size_t start = static_cast<size_t>(bv.get_int("byteOffset", 0)) +
                   static_cast<size_t>(acc.get_int("byteOffset", 0));

    AccessorData out;
    out.comp_type = static_cast<uint32_t>(acc.get_int("componentType", 0));
    out.count = static_cast<uint32_t>(acc.get_int("count", 0));
    const json::Value* tv = acc.find("type");
    out.n_comps = type_ncomps(tv && tv->is_str() ? tv->str : std::string());

    size_t comp_sz = component_size(out.comp_type);
    if (comp_sz == 0) fail("unknown componentType");
    size_t elem_size = comp_sz * out.n_comps;
    size_t stride = static_cast<size_t>(bv.get_int("byteStride", 0));
    if (stride == 0) stride = elem_size;             // Python: byteStride or elem_size

    out.raw.resize(static_cast<size_t>(out.count) * elem_size);
    for (uint32_t i = 0; i < out.count; ++i) {
        size_t src = start + static_cast<size_t>(i) * stride;
        if (src + elem_size > doc.bin.size()) fail("accessor runs past BIN chunk");
        std::memcpy(&out.raw[static_cast<size_t>(i) * elem_size], &doc.bin[src], elem_size);
    }
    return out;
}

// ── mesh parse (transforms + assembly) ─────────────────────────────────────

namespace {

using Mat4 = std::array<double, 16>;   // row-major: M[r*4+c]

Mat4 identity4() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }

Mat4 mat4_mul(const Mat4& A, const Mat4& B) {
    Mat4 C{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += A[i * 4 + k] * B[k * 4 + j];  // -ffp-contract=off
            C[i * 4 + j] = s;
        }
    return C;
}

// Inverse of the top-left 3x3 of `M` (cofactor/adjugate). Returns false if
// singular. `out` is row-major 3x3.
bool mat3_inv(const Mat4& M, double out[9]) {
    double a = M[0], b = M[1], c = M[2],
           d = M[4], e = M[5], f = M[6],
           g = M[8], h = M[9], i = M[10];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (det == 0.0) return false;
    double inv = 1.0 / det;
    out[0] = A * inv;             out[1] = (c * h - b * i) * inv; out[2] = (b * f - c * e) * inv;
    out[3] = B * inv;             out[4] = (a * i - c * g) * inv; out[5] = (c * d - a * f) * inv;
    out[6] = C * inv;             out[7] = (b * g - a * h) * inv; out[8] = (a * e - b * d) * inv;
    return true;
}

// Local transform of a glTF node: explicit column-major `matrix`, else T*R*S.
Mat4 local_matrix(const json::Value& n) {
    const json::Value* m = n.find("matrix");
    if (m && m->is_arr() && m->arr.size() == 16) {
        Mat4 out{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = m->arr[static_cast<size_t>(j * 4 + i)].num;  // column-major -> row-major
        return out;
    }
    double t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
    const json::Value* tv = n.find("translation");
    if (tv && tv->is_arr() && tv->arr.size() == 3)
        for (int k = 0; k < 3; ++k) t[k] = tv->arr[static_cast<size_t>(k)].num;
    const json::Value* rv = n.find("rotation");
    if (rv && rv->is_arr() && rv->arr.size() == 4)
        for (int k = 0; k < 4; ++k) r[k] = rv->arr[static_cast<size_t>(k)].num;
    const json::Value* sv = n.find("scale");
    if (sv && sv->is_arr() && sv->arr.size() == 3)
        for (int k = 0; k < 3; ++k) s[k] = sv->arr[static_cast<size_t>(k)].num;
    double x = r[0], y = r[1], z = r[2], w = r[3];
    // Rotation matrix R (rows), then M = R*S (S diagonal -> R[i][j]*s[j], +0.0 exact),
    // with the translation column set last (matches gltf_builder.local_matrix).
    double R[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w)},
        {2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)}};
    Mat4 out = identity4();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = R[i][j] * s[j];
    out[0 * 4 + 3] = t[0]; out[1 * 4 + 3] = t[1]; out[2 * 4 + 3] = t[2];
    return out;
}

}  // namespace

std::map<int, Mat4> mesh_world_matrices(const json::Value& gltf) {
    const json::Value* nodes_v = gltf.find("nodes");
    std::map<int, Mat4> result;
    if (!nodes_v || !nodes_v->is_arr()) return result;
    const std::vector<json::Value>& nodes = nodes_v->arr;
    int n = static_cast<int>(nodes.size());

    std::vector<int> parent(static_cast<size_t>(n), -1);
    for (int ni = 0; ni < n; ++ni) {
        const json::Value* ch = nodes[static_cast<size_t>(ni)].find("children");
        if (ch && ch->is_arr())
            for (const json::Value& c : ch->arr) {
                int ci = static_cast<int>(c.int_or(-1));
                if (ci >= 0 && ci < n) parent[static_cast<size_t>(ci)] = ni;
            }
    }

    std::map<int, Mat4> world;
    std::function<Mat4(int)> get_world = [&](int ni) -> Mat4 {
        auto it = world.find(ni);
        if (it != world.end()) return it->second;
        Mat4 loc = local_matrix(nodes[static_cast<size_t>(ni)]);
        int p = parent[static_cast<size_t>(ni)];
        Mat4 w = (p < 0) ? loc : mat4_mul(get_world(p), loc);
        world[ni] = w;
        return w;
    };

    for (int ni = 0; ni < n; ++ni) {
        const json::Value* mv = nodes[static_cast<size_t>(ni)].find("mesh");
        if (!mv || !mv->is_num()) continue;
        int mi = static_cast<int>(mv->num);
        if (result.find(mi) != result.end()) continue;
        result[mi] = get_world(ni);
    }
    return result;
}

std::vector<std::pair<int, Mat4>> mesh_world_instances(
    const json::Value& gltf) {
    const json::Value* nodes_value = gltf.find("nodes");
    std::vector<std::pair<int, Mat4>> result;
    if (!nodes_value || !nodes_value->is_arr()) return result;
    const std::vector<json::Value>& nodes = nodes_value->arr;
    const int count = int(nodes.size());
    std::vector<int> parent(size_t(count), -1);
    for (int node = 0; node < count; ++node) {
        const json::Value* children = nodes[size_t(node)].find("children");
        if (!children || !children->is_arr()) continue;
        for (const json::Value& child : children->arr) {
            const int index = int(child.int_or(-1));
            if (index >= 0 && index < count) parent[size_t(index)] = node;
        }
    }
    std::map<int, Mat4> world_cache;
    std::function<Mat4(int)> world = [&](int node) -> Mat4 {
        auto cached = world_cache.find(node);
        if (cached != world_cache.end()) return cached->second;
        const Mat4 local = local_matrix(nodes[size_t(node)]);
        const int p = parent[size_t(node)];
        const Mat4 value = p < 0 ? local : mat4_mul(world(p), local);
        world_cache[node] = value;
        return value;
    };

    std::vector<int> order;
    std::vector<uint8_t> visited(size_t(count), 0);
    std::function<void(int)> visit = [&](int node) {
        if (node < 0 || node >= count || visited[size_t(node)]) return;
        visited[size_t(node)] = 1;
        order.push_back(node);
        const json::Value* children = nodes[size_t(node)].find("children");
        if (children && children->is_arr())
            for (const json::Value& child : children->arr)
                visit(int(child.int_or(-1)));
    };
    const json::Value* scenes = gltf.find("scenes");
    const int scene_index = int(gltf.get_int("scene", 0));
    if (scenes && scenes->is_arr() && scene_index >= 0
        && size_t(scene_index) < scenes->arr.size()) {
        const json::Value* roots = scenes->arr[size_t(scene_index)].find("nodes");
        if (roots && roots->is_arr())
            for (const json::Value& root : roots->arr)
                visit(int(root.int_or(-1)));
    }
    // Malformed/minimal files without a usable scene keep the old permissive
    // behavior and visit every node in document order.
    if (order.empty())
        for (int node = 0; node < count; ++node) visit(node);
    for (int node : order) {
        const json::Value* mesh = nodes[size_t(node)].find("mesh");
        if (mesh && mesh->is_num())
            result.push_back({int(mesh->num), world(node)});
    }
    return result;
}

std::vector<std::array<int, 4>> colors_to_rgba255(const AccessorData& acc) {
    double scale = 255.0;
    if (acc.comp_type == COMP_U16) scale = 255.0 / 65535.0;
    else if (acc.comp_type == COMP_U8) scale = 1.0;   // 255/255
    // F32 (and any other) -> 255.0
    uint32_t nc = acc.n_comps;
    size_t csz = component_size(acc.comp_type);
    std::vector<std::array<int, 4>> out;
    out.reserve(acc.count);
    for (uint32_t i = 0; i < acc.count; ++i) {
        int vals[4] = {0, 0, 0, 0};
        uint32_t lim = std::min(nc, 4u);
        for (uint32_t c = 0; c < lim; ++c) {
            const uint8_t* p = &acc.raw[(static_cast<size_t>(i) * nc + c) * csz];
            double v;
            if (acc.comp_type == COMP_F32) v = rf32(p);
            else if (acc.comp_type == COMP_U16) v = le16(p);
            else v = *p;   // U8 / fallback
            int iv = static_cast<int>(std::nearbyint(v * scale));
            vals[c] = iv < 0 ? 0 : (iv > 255 ? 255 : iv);
        }
        int r = nc > 0 ? vals[0] : 255;
        int g = nc > 1 ? vals[1] : r;
        int b = nc > 2 ? vals[2] : g;
        int a = nc > 3 ? vals[3] : 255;
        out.push_back({r, g, b, a});
    }
    return out;
}

namespace {
// Flat unsigned value of element `idx` in an integer accessor (indices/joints).
uint32_t acc_uint(const AccessorData& a, size_t idx) {
    size_t o = idx * component_size(a.comp_type);
    switch (a.comp_type) {
        case COMP_U8: case COMP_I8:  return a.raw[o];
        case COMP_U16: case COMP_I16: return le16(&a.raw[o]);
        default: return le32(&a.raw[o], 0);   // U32
    }
}
}  // namespace

MeshData parse_glb_mesh(const uint8_t* d, size_t n) {
    GlbDoc doc = parse_glb(d, n);
    const json::Value& gltf = doc.gltf;
    MeshData md;

    const json::Value* meshes = gltf.find("meshes");
    if (!meshes || !meshes->is_arr() || meshes->arr.empty()) fail("GLB contains no meshes");
    const json::Value& mesh = meshes->arr[0];
    const json::Value* prims = mesh.find("primitives");
    if (!prims || !prims->is_arr()) fail("mesh has no primitives");

    std::map<int, Mat4> mw = mesh_world_matrices(gltf);
    Mat4 M = (mw.count(0) ? mw[0] : identity4());

    // Normal matrix N3 s.t. norm_w = norm @ N3^T. Python: N3 = inv(M3)^T, so
    // norm_w = norm @ inv(M3); on a singular M3 it falls back to N3 = M3.
    double N3[9];
    double im[9];
    bool inv_ok = mat3_inv(M, im);
    if (inv_ok) {
        // N3 = inv(M3)^T  ->  norm_w[j] = sum_k norm[k]*inv(M3)[k][j]
        for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) N3[j * 3 + k] = im[k * 3 + j];
    } else {
        // N3 = M3 -> norm_w[j] = sum_k norm[k]*M3[j][k]
        for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) N3[j * 3 + k] = M[j * 4 + k];
    }

    bool have_shared = false;
    for (size_t pi = 0; pi < prims->arr.size(); ++pi) {
        const json::Value& prim = prims->arr[pi];
        const json::Value* attrs = prim.find("attributes");
        const json::Value* idx_v = prim.find("indices");
        if (!attrs || !idx_v || !idx_v->is_num()) continue;

        if (!have_shared) {
            have_shared = true;
            const json::Value* pa = attrs->find("POSITION");
            if (!pa) fail("primitive has no POSITION");
            AccessorData pos = read_accessor(doc, static_cast<int>(pa->num));
            for (uint32_t i = 0; i < pos.count; ++i) {
                double x = rf32(&pos.raw[(static_cast<size_t>(i) * 3 + 0) * 4]);
                double y = rf32(&pos.raw[(static_cast<size_t>(i) * 3 + 1) * 4]);
                double z = rf32(&pos.raw[(static_cast<size_t>(i) * 3 + 2) * 4]);
                double wx = M[0] * x + M[1] * y + M[2] * z + M[3];
                double wy = M[4] * x + M[5] * y + M[6] * z + M[7];
                double wz = M[8] * x + M[9] * y + M[10] * z + M[11];
                md.vertices.push_back({wx, -wz, wy});   // glTF Y-up -> Jade Z-up
            }

            const json::Value* na = attrs->find("NORMAL");
            if (na) {
                AccessorData nd = read_accessor(doc, static_cast<int>(na->num));
                for (uint32_t i = 0; i < nd.count; ++i) {
                    double nx = rf32(&nd.raw[(static_cast<size_t>(i) * 3 + 0) * 4]);
                    double ny = rf32(&nd.raw[(static_cast<size_t>(i) * 3 + 1) * 4]);
                    double nz = rf32(&nd.raw[(static_cast<size_t>(i) * 3 + 2) * 4]);
                    double wx = nx * N3[0] + ny * N3[1] + nz * N3[2];
                    double wy = nx * N3[3] + ny * N3[4] + nz * N3[5];
                    double wz = nx * N3[6] + ny * N3[7] + nz * N3[8];
                    double len = std::sqrt(wx * wx + wy * wy + wz * wz);
                    double dn = len > 1e-9 ? len : 1e-9;
                    wx /= dn; wy /= dn; wz /= dn;
                    md.normals.push_back({wx, -wz, wy});
                }
            } else {
                md.normals.assign(md.vertices.size(), {0, 0, 0});
            }

            const json::Value* ta = attrs->find("TEXCOORD_0");
            if (ta) {
                AccessorData uv = read_accessor(doc, static_cast<int>(ta->num));
                for (uint32_t i = 0; i < uv.count; ++i)
                    md.uvs.push_back({rf32(&uv.raw[(static_cast<size_t>(i) * 2 + 0) * 4]),
                                      rf32(&uv.raw[(static_cast<size_t>(i) * 2 + 1) * 4])});
            } else {
                md.uvs.assign(md.vertices.size(), {0.0f, 0.0f});
            }

            const json::Value* ja = attrs->find("JOINTS_0");
            const json::Value* wa = attrs->find("WEIGHTS_0");
            AccessorData joints, weights;
            bool have_jw = ja && wa;
            if (have_jw) {
                joints = read_accessor(doc, static_cast<int>(ja->num));
                weights = read_accessor(doc, static_cast<int>(wa->num));
            }

            const json::Value* ca = attrs->find("COLOR_0");
            if (ca) {
                AccessorData col = read_accessor(doc, static_cast<int>(ca->num));
                md.colors = colors_to_rgba255(col);
                md.has_colors = !md.colors.empty();
            }

            // Skin reconstruction (after we know the vertex count).
            if (have_jw) {
                const json::Value* skins = gltf.find("skins");
                const json::Value* gnodes = gltf.find("nodes");
                md.skin.flags = 0;
                if (skins && skins->is_arr() && !skins->arr.empty() && gnodes && gnodes->is_arr()) {
                    const json::Value* joints_arr = skins->arr[0].find("joints");
                    if (joints_arr && joints_arr->is_arr()) {
                        for (const json::Value& jv : joints_arr->arr) {
                            int nidx = static_cast<int>(jv.num);
                            GltfBone bone;
                            bone.bone_idx = static_cast<int32_t>(md.skin.bones.size());
                            if (nidx >= 0 && static_cast<size_t>(nidx) < gnodes->arr.size()) {
                                const json::Value* ex = gnodes->arr[static_cast<size_t>(nidx)].find("extras");
                                if (ex) {
                                    bone.bone_idx = static_cast<int32_t>(
                                        ex->get_int("bone_idx", bone.bone_idx));
                                    bone.matrix_type = static_cast<int32_t>(ex->get_int("matrix_type", 0));
                                    const json::Value* bm = ex->find("bind_matrix");
                                    if (bm && bm->is_arr() && bm->arr.size() == 16)
                                        for (int k = 0; k < 16; ++k) bone.bind_matrix[static_cast<size_t>(k)] = bm->arr[static_cast<size_t>(k)].num;
                                }
                            }
                            md.skin.bones.push_back(std::move(bone));
                        }
                    }
                }
                size_t nv = md.vertices.size();
                for (size_t vi = 0; vi < nv; ++vi) {
                    for (int slot = 0; slot < 4; ++slot) {
                        double w = rf32(&weights.raw[(vi * 4 + static_cast<size_t>(slot)) * 4]);
                        if (w < 1e-6) continue;
                        uint32_t bi = acc_uint(joints, vi * 4 + static_cast<size_t>(slot));
                        if (bi < md.skin.bones.size())
                            md.skin.bones[bi].weights.push_back({static_cast<uint32_t>(vi), w});
                    }
                }
                for (size_t bli = 0; bli < md.skin.bones.size(); ++bli)
                    for (const auto& vw : md.skin.bones[bli].weights)
                        md.skin.vertex_weights[vw.first].push_back({static_cast<uint32_t>(bli), vw.second});
                md.has_skin = true;
            }
        }

        // Per-primitive indices -> element + faces (flat order).
        AccessorData idx = read_accessor(doc, static_cast<int>(idx_v->num));
        size_t n_idx = static_cast<size_t>(idx.count) * idx.n_comps;
        uint32_t n_tris = static_cast<uint32_t>(n_idx / 3);
        int32_t mat_id = static_cast<int32_t>(pi);
        const json::Value* ex = prim.find("extras");
        if (ex) mat_id = static_cast<int32_t>(ex->get_int("matId", mat_id));
        md.elements.push_back({n_tris, mat_id});
        for (uint32_t t = 0; t < n_tris; ++t) {
            uint32_t i0 = acc_uint(idx, static_cast<size_t>(t) * 3 + 0);
            uint32_t i1 = acc_uint(idx, static_cast<size_t>(t) * 3 + 1);
            uint32_t i2 = acc_uint(idx, static_cast<size_t>(t) * 3 + 2);
            md.faces.push_back({i0, i1, i2});
            md.face_uvs.push_back({i0, i1, i2});
            md.face_elems.push_back(static_cast<uint32_t>(pi));
        }
    }

    if (!have_shared) fail("GLB has no usable mesh data");
    md.ok = true;
    return md;
}

// ── GEO payload builder ────────────────────────────────────────────────────

namespace {

void put_u16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put_u32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void put_f32(std::vector<uint8_t>& v, double d) { float f = static_cast<float>(d); uint32_t b; std::memcpy(&b, &f, 4); put_u32(v, b); }

// _pack_vertex_colors: nb_pts D3DCOLOR (0xAARRGGBB) dwords, padded with white.
void append_colors(std::vector<uint8_t>& out, const std::vector<std::array<int, 4>>& colors, uint32_t nb_pts) {
    uint32_t nn = std::min(static_cast<uint32_t>(colors.size()), nb_pts);
    for (uint32_t i = 0; i < nn; ++i) {
        uint32_t r = uint32_t(colors[i][0]) & 0xFF, g = uint32_t(colors[i][1]) & 0xFF,
                 b = uint32_t(colors[i][2]) & 0xFF, a = uint32_t(colors[i][3]) & 0xFF;
        put_u32(out, (a << 24) | (r << 16) | (g << 8) | b);
    }
    for (uint32_t i = nn; i < nb_pts; ++i) put_u32(out, 0xFFFFFFFFu);
}

// _serialize_skin: [flags:H][nbones:H], per bone [bone_idx:H][nw:H][16 f32][mtype:i]
// then per weight [vert:H][pond:H], pond = min(int(w*65535+0.5), 65535).
void append_skin(std::vector<uint8_t>& out, const GltfSkin& skin) {
    put_u16(out, static_cast<uint16_t>(skin.flags));
    put_u16(out, static_cast<uint16_t>(skin.bones.size()));
    for (const GltfBone& b : skin.bones) {
        put_u16(out, static_cast<uint16_t>(b.bone_idx));
        put_u16(out, static_cast<uint16_t>(b.weights.size()));
        for (int k = 0; k < 16; ++k) put_f32(out, b.bind_matrix[static_cast<size_t>(k)]);
        put_u32(out, static_cast<uint32_t>(b.matrix_type));
        for (const auto& vw : b.weights) {
            int pond = static_cast<int>(vw.second * 65535.0 + 0.5);
            if (pond > 65535) pond = 65535;
            put_u16(out, static_cast<uint16_t>(vw.first & 0xFFFF));
            put_u16(out, static_cast<uint16_t>(pond));
        }
    }
}

// _compute_vertex_tangents — per-vertex tangents (Lengyel) for WW's TEXCOORD1
// slot. Unit tangent aligned with +U, Gram-Schmidt'd against the vertex normal;
// handedness is implicit in the shader's cross(tangent, normal).
std::vector<std::array<double, 3>> compute_vertex_tangents(const MeshData& md) {
    size_t n = md.vertices.size();
    std::vector<std::array<double, 3>> acc(n, {0, 0, 0});
    for (size_t f = 0; f < md.faces.size() && f < md.face_uvs.size(); ++f) {
        const auto& fa = md.faces[f];
        const auto& fu = md.face_uvs[f];
        if (fa[0] >= n || fa[1] >= n || fa[2] >= n) continue;
        const auto& p0 = md.vertices[fa[0]];
        const auto& p1 = md.vertices[fa[1]];
        const auto& p2 = md.vertices[fa[2]];
        auto uvat = [&](uint32_t i) -> std::array<double, 2> {
            if (i < md.uvs.size()) return {double(md.uvs[i][0]), double(md.uvs[i][1])};
            return {0.0, 0.0};
        };
        std::array<double, 2> w0 = uvat(fu[0]), w1 = uvat(fu[1]), w2 = uvat(fu[2]);
        double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        double dv1 = w1[1] - w0[1], du1 = w1[0] - w0[0];
        double dv2 = w2[1] - w0[1], du2 = w2[0] - w0[0];
        double denom = du1 * dv2 - du2 * dv1;
        if (std::fabs(denom) < 1e-12) continue;
        double t[3];
        for (int k = 0; k < 3; ++k) t[k] = (e1[k] * dv2 - e2[k] * dv1) / denom;
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k) acc[fa[static_cast<size_t>(c)]][static_cast<size_t>(k)] += t[k];
    }
    std::vector<std::array<double, 3>> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        std::array<double, 3> ni = i < md.normals.size() ? md.normals[i] : std::array<double, 3>{0, 0, 1};
        double d = ni[0] * acc[i][0] + ni[1] * acc[i][1] + ni[2] * acc[i][2];
        double ti[3] = {acc[i][0] - ni[0] * d, acc[i][1] - ni[1] * d, acc[i][2] - ni[2] * d};
        double L = std::sqrt(ti[0] * ti[0] + ti[1] * ti[1] + ti[2] * ti[2]);
        if (L < 1e-9) {                       // degenerate: any vector perp. to N
            double a[3] = {1, 0, 0};
            if (!(std::fabs(ni[0]) < 0.9)) { a[0] = 0; a[1] = 1; a[2] = 0; }
            double da = ni[0] * a[0] + ni[1] * a[1] + ni[2] * a[2];
            for (int k = 0; k < 3; ++k) ti[k] = a[k] - ni[static_cast<size_t>(k)] * da;
            L = std::sqrt(ti[0] * ti[0] + ti[1] * ti[1] + ti[2] * ti[2]);
            if (L == 0.0) L = 1.0;            // Python: `norm(ti) or 1.0`
        }
        out.push_back({ti[0] / L, ti[1] / L, ti[2] / L});
    }
    return out;
}

// _build_shipped_skinned_trailing — WW/T2T shipped skinned trailing. stride 52,
// or 64 with a per-vertex tangent in the TEXCOORD1 slot @52 when `tangents`.
std::vector<uint8_t> shipped_skinned_trailing(const MeshData& md,
                                              const std::vector<std::array<double, 3>>* tangents) {
    uint32_t stride_v = tangents ? 64u : 52u;
    uint32_t nb_elems = static_cast<uint32_t>(md.elements.size());
    std::vector<uint32_t> face_elems = md.face_elems;
    if (face_elems.size() != md.faces.size()) face_elems.assign(md.faces.size(), 0);

    std::vector<std::vector<size_t>> faces_by_elem(std::max<uint32_t>(nb_elems, 1));
    for (size_t fi = 0; fi < face_elems.size(); ++fi) {
        uint32_t ei = face_elems[fi];
        if (ei < nb_elems) faces_by_elem[ei].push_back(fi);
        else faces_by_elem[0].push_back(fi);
    }

    // influences: vertex -> list of (bone_idx, weight)
    std::map<uint32_t, std::vector<std::pair<int, double>>> influences;
    for (const GltfBone& b : md.skin.bones)
        for (const auto& vw : b.weights)
            influences[vw.first].push_back({b.bone_idx, vw.second});

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> corner_map;
    std::vector<std::pair<uint32_t, uint32_t>> vbuf;
    std::vector<uint32_t> ib;
    for (uint32_t ei = 0; ei < nb_elems; ++ei)
        for (size_t fi : faces_by_elem[ei]) {
            const auto& face = md.faces[fi];
            const auto& fuv = md.face_uvs[fi];
            for (int c = 0; c < 3; ++c) {
                std::pair<uint32_t, uint32_t> key{face[static_cast<size_t>(c)], fuv[static_cast<size_t>(c)]};
                auto it = corner_map.find(key);
                uint32_t si;
                if (it == corner_map.end()) { si = static_cast<uint32_t>(vbuf.size()); corner_map[key] = si; vbuf.push_back(key); }
                else si = it->second;
                ib.push_back(si);
            }
        }

    uint32_t n2 = static_cast<uint32_t>(vbuf.size());
    uint32_t stride = stride_v, magic = 3;
    uint32_t sec2_size = 8 + n2 * stride;

    std::vector<uint8_t> out;
    put_u32(out, 0); put_u32(out, 0); put_u32(out, nb_elems); put_u32(out, 0);
    for (uint32_t i = 0; i < nb_elems; ++i) {
        uint32_t x = (i < nb_elems - 1) ? static_cast<uint32_t>(md.elements[i + 1].second) : sec2_size;
        put_u32(out, static_cast<uint32_t>(faces_by_elem[i].size()));
        put_u32(out, x);
    }
    put_u32(out, magic); put_u32(out, n2); put_u32(out, stride);
    for (const auto& key : vbuf) {
        uint32_t pos_idx = key.first, uv_idx = key.second;
        std::array<double, 3> p = pos_idx < md.vertices.size() ? md.vertices[pos_idx] : std::array<double, 3>{0, 0, 0};
        std::array<double, 3> nrm = pos_idx < md.normals.size() ? md.normals[pos_idx] : std::array<double, 3>{0, 0, 1};
        std::array<float, 2> uv = uv_idx < md.uvs.size() ? md.uvs[uv_idx] : std::array<float, 2>{0, 0};
        std::vector<std::pair<int, double>> infl;
        auto iit = influences.find(pos_idx);
        if (iit != influences.end()) infl = iit->second;
        std::sort(infl.begin(), infl.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;   // -weight, +bone
        });
        // Magic-3 consumes three explicit weights and has no implicit fourth.
        // Match jgao_converter.py: keep the top three and renormalize them.
        if (infl.size() > 3) infl.resize(3);
        int bones[4] = {0, 0, 0, 0};
        double wf[3] = {0, 0, 0};
        for (size_t k = 0; k < infl.size(); ++k) { bones[k] = infl[k].first; wf[k] = infl[k].second; }
        double wsum = wf[0] + wf[1] + wf[2];
        if (wsum > 0.0) {
            wf[0] /= wsum;
            wf[1] /= wsum;
            wf[2] /= wsum;
        }
        uint32_t pack_a = (uint32_t(bones[0] * 3)) | (uint32_t(bones[1] * 3) << 16);
        uint32_t pack_b = (uint32_t(bones[2] * 3)) | (uint32_t(bones[3] * 3) << 16);
        put_f32(out, p[0]); put_f32(out, p[1]); put_f32(out, p[2]);
        put_f32(out, nrm[0]); put_f32(out, nrm[1]); put_f32(out, nrm[2]);
        put_u32(out, pack_a); put_u32(out, pack_b);
        put_f32(out, wf[0]); put_f32(out, wf[1]); put_f32(out, wf[2]);
        put_f32(out, uv[0]); put_f32(out, uv[1]);
        if (stride == 64) {                       // TEXCOORD1 slot @52 = tangent
            double tx = 1.0, ty = 0.0, tz = 0.0;
            if (tangents && pos_idx < tangents->size()) {
                tx = (*tangents)[pos_idx][0]; ty = (*tangents)[pos_idx][1]; tz = (*tangents)[pos_idx][2];
            }
            put_f32(out, tx); put_f32(out, ty); put_f32(out, tz);
        }
    }
    put_u32(out, static_cast<uint32_t>(ib.size()));
    for (uint32_t idx : ib) put_u16(out, static_cast<uint16_t>(idx & 0xFFFF));
    return out;
}

}  // namespace

uint32_t orig_skinned_vb_stride(const uint8_t* raw, size_t n) {
    if (!raw || n < 12) return 0;
    uint32_t best_n = 0, best_stride = 0;
    for (size_t off = 0; off + 12 <= n; off += 4) {
        uint32_t magic = le32(raw, off), cnt = le32(raw, off + 4), stride = le32(raw, off + 8);
        if ((magic == 2 || magic == 3) && (stride == 52 || stride == 64) &&
            cnt > 0 && cnt < 200000 &&
            off + 12 + static_cast<size_t>(cnt) * stride <= n) {
            if (best_stride == 0 || cnt > best_n) { best_n = cnt; best_stride = stride; }
        }
    }
    return best_stride;
}

std::vector<uint8_t> build_geo_payload(const MeshData& md, const GeoBuildOpts& opts) {
    uint32_t skin_ok3 = md.normals.empty() ? 0 : 1;
    uint32_t nb_pts = static_cast<uint32_t>(md.vertices.size());
    uint32_t nb_uvs = static_cast<uint32_t>(md.uvs.size());
    uint32_t nb_elems = static_cast<uint32_t>(md.elements.size());

    bool has_colors = opts.colors && !opts.colors->empty();
    uint32_t has_abs = has_colors ? 1 : 0;
    uint32_t abs_count = has_colors ? nb_pts : 0;

    std::vector<uint8_t> skin_bytes;
    uint32_t mrm_marker = 0;
    if (md.has_skin && !md.skin.bones.empty()) {
        mrm_marker = 0xC0DE2002;
        append_skin(skin_bytes, md.skin);
    }

    std::vector<uint8_t> data;
    put_u32(data, opts.version); put_u32(data, opts.flags1); put_u32(data, opts.flags2);
    put_u32(data, nb_pts); put_u32(data, abs_count); put_u32(data, has_abs);
    put_u32(data, nb_uvs); put_u32(data, nb_elems); put_u32(data, mrm_marker);
    if (!skin_bytes.empty()) {                       // 9-word header + skin + skin_ok3
        data.insert(data.end(), skin_bytes.begin(), skin_bytes.end());
        put_u32(data, skin_ok3);
    } else {                                         // 10-word header
        put_u32(data, skin_ok3);
    }

    for (const auto& v : md.vertices) { put_f32(data, v[0]); put_f32(data, v[1]); put_f32(data, v[2]); }
    if (skin_ok3)
        for (const auto& nrm : md.normals) { put_f32(data, nrm[0]); put_f32(data, nrm[1]); put_f32(data, nrm[2]); }
    if (has_colors) append_colors(data, *opts.colors, nb_pts);
    for (const auto& uv : md.uvs) { put_f32(data, uv[0]); put_f32(data, uv[1]); }
    for (const auto& e : md.elements) { put_u32(data, e.first); put_u32(data, static_cast<uint32_t>(e.second)); }
    for (size_t f = 0; f < md.faces.size(); ++f) {
        const auto& face = md.faces[f];
        const auto& fuv = md.face_uvs[f];
        put_u16(data, static_cast<uint16_t>(face[0])); put_u16(data, static_cast<uint16_t>(face[1])); put_u16(data, static_cast<uint16_t>(face[2]));
        put_u16(data, static_cast<uint16_t>(fuv[0])); put_u16(data, static_cast<uint16_t>(fuv[1])); put_u16(data, static_cast<uint16_t>(fuv[2]));
        put_u32(data, 1);   // triangle flags
    }

    if (opts.shipped_skinned) {
        // WW reads the per-vertex TANGENT from the TEXCOORD1 slot (stride-64
        // cooked VB) to build the tangent basis in-shader — it never derives
        // tangents from UVs. If the ORIGINAL actor shipped stride 64 it is
        // normal-mapped, so rebuild at 64 with computed tangents; a stride-52
        // rebuild would leave the engine reading the next vertex's position.
        std::vector<std::array<double, 3>> tangents;
        bool want_tangents = !md.normals.empty() && opts.orig_skinned_stride == 64;
        if (want_tangents) tangents = compute_vertex_tangents(md);
        std::vector<uint8_t> tr = shipped_skinned_trailing(md, want_tangents ? &tangents : nullptr);
        data.insert(data.end(), tr.begin(), tr.end());
        return data;
    }

    // Legacy trailing: end-marker + cooked VB/IB (dedup by (vi,ui)).
    put_u32(data, 0); put_u32(data, 0);
    uint32_t vb_magic = opts.vb_magic;
    uint32_t vb_stride = (vb_magic == 6 || vb_magic == 8) ? 44 : 32;

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vert_map;
    std::vector<std::pair<uint32_t, uint32_t>> expanded;   // (vi, ui)
    std::vector<uint32_t> ib;
    for (size_t f = 0; f < md.faces.size(); ++f) {
        const auto& face = md.faces[f];
        const auto& fuv = md.face_uvs[f];
        for (int c = 0; c < 3; ++c) {
            std::pair<uint32_t, uint32_t> key{face[static_cast<size_t>(c)], fuv[static_cast<size_t>(c)]};
            auto it = vert_map.find(key);
            uint32_t idx;
            if (it == vert_map.end()) { idx = static_cast<uint32_t>(expanded.size()); vert_map[key] = idx; expanded.push_back(key); }
            else idx = it->second;
            ib.push_back(idx);
        }
    }
    uint32_t nb_expanded = static_cast<uint32_t>(expanded.size());

    std::vector<uint8_t> tr;
    put_u32(tr, 0); put_u32(tr, 0);
    put_u32(tr, nb_elems);
    for (const auto& e : md.elements) { put_u32(tr, static_cast<uint32_t>(e.second)); put_u32(tr, e.first); }
    put_u32(tr, 8 + nb_expanded * vb_stride); put_u32(tr, vb_magic); put_u32(tr, nb_expanded); put_u32(tr, vb_stride);
    for (const auto& key : expanded) {
        uint32_t vi = key.first, ui = key.second;
        std::array<double, 3> p = vi < md.vertices.size() ? md.vertices[vi] : std::array<double, 3>{0, 0, 0};
        std::array<double, 3> nrm = vi < md.normals.size() ? md.normals[vi] : std::array<double, 3>{0, 0, 1};
        std::array<float, 2> uv = ui < md.uvs.size() ? md.uvs[ui] : std::array<float, 2>{0, 0};
        double nx = nrm[0], ny = nrm[1], nz = nrm[2];
        if (vb_magic == 8) { nx = ny = nz = 0.0; }
        put_f32(tr, p[0]); put_f32(tr, p[1]); put_f32(tr, p[2]);
        put_f32(tr, nx); put_f32(tr, ny); put_f32(tr, nz);
        put_f32(tr, uv[0]); put_f32(tr, uv[1]);
        if (vb_magic == 6 || vb_magic == 8) for (int z = 0; z < 12; ++z) tr.push_back(0);
    }
    put_u32(tr, static_cast<uint32_t>(ib.size()));
    for (uint32_t idx : ib) put_u16(tr, static_cast<uint16_t>(idx & 0xFFFF));
    if ((ib.size() * 2) % 4) { tr.push_back(0); tr.push_back(0); }

    data.insert(data.end(), tr.begin(), tr.end());
    return data;
}

}  // namespace gltf
}  // namespace jade
