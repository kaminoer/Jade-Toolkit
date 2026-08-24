// MeshSwap.cpp — implementation. Faithful port of io_ops/mesh_swap.py's
// _clamp_matid / _grm_sub_material_keys / _owning_grm_key /
// resolve_element_texture_keys + the host-GAO RLI rewrite slice
// (_host_gao_data / _qpos / _original_rli_colors / _rewrite_gao_instance_colors).
#include "jade/MeshSwap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_set>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/GameProfiles.hpp"
#include "jade/Gao.hpp"
#include "jade/Gltf.hpp"
#include "jade/Material.hpp"
#include "jade/PatcherModel.hpp"
#include "jade/Rli.hpp"

namespace jade {

namespace {

std::string lower_ascii(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return text;
}

bool read_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return bool(f) || f.eof();
}

}  // namespace

int64_t clamp_matid(int64_t matid, int64_t n_sub) {
    if (n_sub <= 0) return 0;
    if (matid < 0) return 0;
    return matid < n_sub - 1 ? matid : n_sub - 1;
}

std::vector<uint32_t> grm_sub_material_keys(const SubEntry* grm_sub) {
    if (grm_sub == nullptr) return {};
    if (!grm_sub->gro_null &&
        grm_sub->gro_type == static_cast<uint32_t>(GRO_TYPE_MAT_MULTI)) {
        MatInfo mm = parse_material(grm_sub->data.data(), grm_sub->data.size(),
                                    GRO_TYPE_MAT_MULTI);
        return mm.ok ? mm.sub_material_keys : std::vector<uint32_t>{};
    }
    // A single material named directly: one-slot list of its own key.
    if (grm_sub->key != 0) return {grm_sub->key};
    return {};
}

SkinValidationResult validate_glb_skin(
    const std::vector<uint8_t>& dec, uint32_t geo_key,
    const std::vector<uint8_t>& glb_bytes) {
    SkinValidationResult result;
    std::vector<SubEntry> subs = parse_sub_entries(dec);
    const SubEntry* target = pick_geo_sub(subs, geo_key);
    if (!target) {
        char text[96];
        std::snprintf(text, sizeof text,
                      "No .geo sub-entry with key 0x%08x", geo_key);
        result.error = text;
        return result;
    }
    const GeoInfo geo = parse_geometry(target->data.data(), target->data.size());
    if (!geo.ok) {
        result.error = "Could not parse original geometry";
        return result;
    }

    std::vector<bool> has_bone_idx;
    try {
        const gltf::GlbDoc doc = gltf::parse_glb(glb_bytes.data(),
                                                 glb_bytes.size());
        const json::Value* skins = doc.gltf.find("skins");
        const json::Value* nodes = doc.gltf.find("nodes");
        if (skins && skins->is_arr() && !skins->arr.empty()) {
            const json::Value* joints = skins->arr[0].find("joints");
            if (joints && joints->is_arr()) {
                for (const json::Value& joint : joints->arr) {
                    const long long index = joint.is_num()
                                                ? (long long)joint.num : -1;
                    std::string name = "node_" + std::to_string(index);
                    bool has_index = false;
                    if (nodes && nodes->is_arr() && index >= 0
                        && size_t(index) < nodes->arr.size()) {
                        const json::Value& node = nodes->arr[size_t(index)];
                        const json::Value* node_name = node.find("name");
                        if (node_name && node_name->is_str())
                            name = node_name->str;
                        const json::Value* extras = node.find("extras");
                        has_index = extras && extras->is_obj()
                                    && extras->find("bone_idx") != nullptr
                                    && extras->find("bone_idx")->type
                                           != json::Value::Type::Null;
                    }
                    result.glb_joint_names.push_back(name);
                    has_bone_idx.push_back(has_index);
                }
            }
        }
    } catch (const std::exception& e) {
        result.error = e.what();
        return result;
    }
    result.glb_has_skin = !result.glb_joint_names.empty();
    result.glb_joint_count = uint32_t(result.glb_joint_names.size());

    const std::vector<std::string> gizmos =
        patcher::resolve_skeleton_gizmos(subs, true, geo_key);
    for (const GeoBone& bone : geo.skin_bones) {
        std::string name;
        if (size_t(bone.bone_idx) < gizmos.size()
            && !gizmos[size_t(bone.bone_idx)].empty())
            name = gizmos[size_t(bone.bone_idx)];
        else {
            char fallback[32];
            std::snprintf(fallback, sizeof fallback, "bone_%03u",
                          unsigned(bone.bone_idx));
            name = fallback;
        }
        if (name.size() >= 4
            && name.compare(name.size() - 4, 4, ".gao") == 0)
            name.resize(name.size() - 4);
        result.orig_bone_names.push_back(name);
    }
    result.orig_bone_count = uint32_t(result.orig_bone_names.size());

    if (!result.glb_has_skin && result.orig_bone_count) {
        result.warnings.push_back(
            "GLB has no skin — the new mesh will load unskinned, but the "
            "original geometry is skinned to "
            + std::to_string(result.orig_bone_count)
            + " bones. The mesh will not deform with the skeleton.");
    }
    if (result.glb_has_skin) {
        if (result.glb_joint_count < result.orig_bone_count) {
            result.warnings.push_back(
                "GLB has only " + std::to_string(result.glb_joint_count)
                + " joints, but the original geometry binds to "
                + std::to_string(result.orig_bone_count)
                + " bones. Vertices weighted to missing bones will fall "
                  "back to bone 0.");
        } else if (result.glb_joint_count > result.orig_bone_count) {
            result.warnings.push_back(
                "GLB has " + std::to_string(result.glb_joint_count)
                + " joints, more than the original's "
                + std::to_string(result.orig_bone_count)
                + " bones. Extra joints will be ignored by the skeleton.");
        }
        std::map<std::string, size_t> original_by_name;
        for (size_t i = 0; i < result.orig_bone_names.size(); ++i)
            original_by_name[lower_ascii(result.orig_bone_names[i])] = i;
        for (const std::string& name : result.glb_joint_names) {
            auto found = original_by_name.find(lower_ascii(name));
            if (found != original_by_name.end())
                result.name_matches.push_back(
                    {name, result.orig_bone_names[found->second]});
        }
        if (!has_bone_idx.empty()
            && std::find(has_bone_idx.begin(), has_bone_idx.end(), true)
                   == has_bone_idx.end()) {
            result.warnings.push_back(
                "GLB joints have no embedded bone_idx (extras), so bone "
                "indices will default to joint position in the GLB. This "
                "usually means the GLB was authored from scratch in Blender "
                "rather than round-tripped through this toolkit's exporter. "
                "Joint order must match the original skeleton, or skinning "
                "will be wrong.");
        }
        if (result.name_matches.empty() && !result.orig_bone_names.empty())
            result.warnings.push_back(
                "No GLB joint name matches any original bone name. The mesh "
                "will likely deform incorrectly. Re-export from Blender "
                "preserving the source armature's bone names.");
    }
    result.ok = result.warnings.empty();
    return result;
}

SkinValidationResult validate_glb_skin(
    const std::string& glb_path, const std::string& bf_path,
    uint32_t entry_idx, uint32_t geo_key) {
    SkinValidationResult result;
    try {
        BigFile bf;
        bf.open(bf_path);
        auto found = bf.files.find(entry_idx);
        if (found == bf.files.end()) {
            result.error = "BF entry " + std::to_string(entry_idx)
                           + " not found";
            return result;
        }
        LzoResult dec = decompress_lzo(bf.read_data(entry_idx));
        if (!dec.ok) { result.error = "Could not decompress BF entry"; return result; }
        std::vector<uint8_t> glb;
        if (!read_bytes(glb_path, glb)) {
            result.error = "Could not read GLB file";
            return result;
        }
        return validate_glb_skin(dec.data, geo_key, glb);
    } catch (const std::exception& e) {
        result.error = e.what();
        return result;
    }
}

bool owning_grm_key(const std::vector<SubEntry>& subs, uint32_t geo_key,
                    uint32_t& out) {
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;   // last wins

    // gro_type-1 key set, built lazily like the Python (only if a group is hit).
    std::unordered_set<uint32_t> geo_keys;
    bool geo_keys_built = false;

    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo info = parse_gao_full(s.data.data(), s.data.size());
        if (!info.ok || !info.vis_read) continue;
        uint32_t gk = info.gro_key;
        if (gk == 0 || gk == 0xFFFFFFFFu) continue;
        if (gk == geo_key) { out = info.grm_key; return true; }
        // The GAO might name a geometry group that lists geo_key.
        auto it = by_key.find(gk);
        if (it != by_key.end() && (it->second->gro_null || it->second->gro_type != 1)) {
            if (!geo_keys_built) {
                geo_keys_built = true;
                for (const auto& kv : by_key)
                    if (!kv.second->gro_null && kv.second->gro_type == 1)
                        geo_keys.insert(kv.first);
            }
            for (uint32_t member : geo_group_members(gk, by_key, &geo_keys)) {
                if (member == geo_key) { out = info.grm_key; return true; }
            }
        }
    }
    return false;
}

const SubEntry* host_gao_sub(const std::vector<SubEntry>& subs, uint32_t geo_key) {
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;
    std::unordered_set<uint32_t> geo_keys;
    bool geo_keys_built = false;

    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
        if (!gi.ok || !gi.vis_read) continue;
        uint32_t gk = gi.gro_key;
        if (gk == 0 || gk == 0xFFFFFFFFu) continue;
        if (gk == geo_key) return &s;
        auto it = by_key.find(gk);
        if (it != by_key.end() && (it->second->gro_null || it->second->gro_type != 1)) {
            if (!geo_keys_built) {
                geo_keys_built = true;
                for (const auto& kv : by_key)
                    if (!kv.second->gro_null && kv.second->gro_type == 1)
                        geo_keys.insert(kv.first);
            }
            for (uint32_t member : geo_group_members(gk, by_key, &geo_keys))
                if (member == geo_key) return &s;
        }
    }
    return nullptr;
}

namespace {

// _qpos's per-component round(x, 3): CPython rounds the double's DECIMAL
// representation (dtoa) at 3 digits, ties-to-even — reproduced by formatting at
// 3 decimals and parsing back. Used only as a position-grouping key.
inline double round3(double x) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", x);
    return std::strtod(buf, nullptr);
}
using QPos = std::array<double, 3>;   // std::map's < unifies -0.0/0.0 like Python

// _bgr / the table writer's channel op: int(round(c)) & 0xFF — WRAP, not clamp
// (unlike rli._pack_bgra). round() is half-to-even == nearbyint.
inline uint8_t wrap_round(double c) {
    return static_cast<uint8_t>(static_cast<long long>(std::nearbyint(c)) & 0xFF);
}

}  // namespace

OrigRliColors original_rli_colors(const std::vector<SubEntry>& subs, uint32_t geo_key,
                                  const GeoInfo& orig_geo,
                                  const std::vector<std::array<double, 3>>& new_vertices) {
    OrigRliColors out;
    const SubEntry* host = host_gao_sub(subs, geo_key);
    if (host == nullptr) return out;
    PrimaryColors pc = read_primary_colors(host->data.data(), host->data.size(),
                                           orig_geo.nb_points);
    if (!pc.ok || pc.colors.empty()) return out;

    std::map<QPos, Rgba4> posmap;
    size_t nverts = orig_geo.vertices.size() / 3;
    for (size_t i = 0; i < nverts && i < pc.colors.size(); ++i) {
        QPos q{round3(orig_geo.vertices[i * 3]), round3(orig_geo.vertices[i * 3 + 1]),
               round3(orig_geo.vertices[i * 3 + 2])};
        posmap.emplace(q, Rgba4{double(pc.colors[i].r), double(pc.colors[i].g),
                                double(pc.colors[i].b), double(pc.colors[i].a)});
    }
    out.ok = true;
    out.colors.reserve(new_vertices.size());
    for (const auto& v : new_vertices) {
        QPos q{round3(v[0]), round3(v[1]), round3(v[2])};
        auto it = posmap.find(q);
        out.colors.push_back(it != posmap.end() ? it->second
                                                : Rgba4{255, 255, 255, 255});
    }
    return out;
}

std::vector<uint8_t> rewrite_gao_instance_colors(
    const uint8_t* gao, size_t n, uint32_t geo_key, uint32_t orig_nb_pts,
    const std::vector<Rgba4>& new_colors, const uint8_t* geo, size_t geo_n) {
    GaoInfo info = parse_gao_full(gao, n);
    if (!info.ok || !info.vis_read || info.gro_key != geo_key) return {};
    long long voff = visual_block_offset(gao, n);
    if (voff < 0) return {};

    // Locate [0xFFFF][u32 count == orig_nb_pts] within the visual block window.
    long long marker = -1;
    if (n >= 6) {
        size_t lo = static_cast<size_t>(voff);
        size_t hi = std::min(lo + 64, n - 6);
        for (size_t off = lo; off < hi; ++off) {
            if (gao[off] == 0xFF && gao[off + 1] == 0xFF) {
                uint32_t cnt = static_cast<uint32_t>(gao[off + 2]) |
                               (static_cast<uint32_t>(gao[off + 3]) << 8) |
                               (static_cast<uint32_t>(gao[off + 4]) << 16) |
                               (static_cast<uint32_t>(gao[off + 5]) << 24);
                if (cnt == orig_nb_pts) { marker = static_cast<long long>(off); break; }
            }
        }
    }
    if (marker < 0) return {};
    size_t color_start = static_cast<size_t>(marker) + 6;
    size_t old_end = color_start + static_cast<size_t>(orig_nb_pts) * 4;
    if (old_end > n) return {};

    // Fresh primary table: 0xFFFF + count + BGRA(alpha 0xFE) per colour.
    std::vector<uint8_t> result;
    result.reserve(n + new_colors.size() * 4);
    result.insert(result.end(), gao, gao + marker);
    result.push_back(0xFF);
    result.push_back(0xFF);
    uint32_t nc = static_cast<uint32_t>(new_colors.size());
    for (int i = 0; i < 4; ++i) result.push_back(uint8_t(nc >> (8 * i)));
    for (const Rgba4& c : new_colors) {
        result.push_back(wrap_round(c[2]));   // B
        result.push_back(wrap_round(c[1]));   // G
        result.push_back(wrap_round(c[0]));   // R
        result.push_back(0xFE);
    }
    result.insert(result.end(), gao + old_end, gao + n);

    // The engine streams the EXTRA block — refresh (or rebuild) it too.
    size_t es;
    uint32_t cnt;
    if (!find_extra_block(result.data(), result.size(),
                          color_start + new_colors.size() * 4, es, cnt))
        return result;

    ExpandedToBase e2b;
    if (geo != nullptr)
        e2b = expanded_to_base(geo, geo_n, static_cast<uint32_t>(new_colors.size()));

    auto color_at = [&](size_t bi) -> const Rgba4& {
        return bi < new_colors.size() ? new_colors[bi] : new_colors[0];
    };

    if (e2b.ok && e2b.map.size() != cnt) {
        // True swap: the cooked-VB count changed -> rebuild/resize the extra
        // block (keeping the leading marker word + the -1.0 sentinels).
        uint32_t new_cnt = static_cast<uint32_t>(e2b.map.size());
        size_t hdr = es - 16;
        uint32_t mk = static_cast<uint32_t>(result[hdr]) |
                      (static_cast<uint32_t>(result[hdr + 1]) << 8) |
                      (static_cast<uint32_t>(result[hdr + 2]) << 16) |
                      (static_cast<uint32_t>(result[hdr + 3]) << 24);
        std::vector<uint8_t> block;
        block.reserve(16 + static_cast<size_t>(new_cnt) * 12);
        uint32_t words[4] = {mk, 8 + new_cnt * 12, new_cnt, 12};
        for (uint32_t w : words)
            for (int i = 0; i < 4; ++i) block.push_back(uint8_t(w >> (8 * i)));
        for (uint32_t j = 0; j < new_cnt; ++j) {
            const Rgba4& c = color_at(e2b.map[j]);
            block.push_back(wrap_round(c[2]));
            block.push_back(wrap_round(c[1]));
            block.push_back(wrap_round(c[0]));
            block.push_back(0xFE);
            for (int rep = 0; rep < 2; ++rep) {   // two -1.0f sentinels
                float f = -1.0f;
                uint32_t b32;
                std::memcpy(&b32, &f, 4);
                for (int i = 0; i < 4; ++i) block.push_back(uint8_t(b32 >> (8 * i)));
            }
        }
        std::vector<uint8_t> out;
        out.reserve(hdr + block.size() + (result.size() - (es + size_t(cnt) * 12)));
        out.insert(out.end(), result.begin(), result.begin() + static_cast<long>(hdr));
        out.insert(out.end(), block.begin(), block.end());
        out.insert(out.end(), result.begin() + static_cast<long>(es + size_t(cnt) * 12),
                   result.end());
        return out;
    }

    if (e2b.ok) {
        if (e2b.map.size() != cnt) return result;   // unreachable; mirrors Python
        for (uint32_t j = 0; j < cnt; ++j) {
            const Rgba4& c = color_at(e2b.map[j]);
            size_t o = es + static_cast<size_t>(j) * 12;
            result[o] = wrap_round(c[2]);
            result[o + 1] = wrap_round(c[1]);
            result[o + 2] = wrap_round(c[0]);
        }
        return result;
    }
    if (cnt == new_colors.size()) {                 // legacy 1:1 fast path
        for (uint32_t j = 0; j < cnt; ++j) {
            const Rgba4& c = new_colors[j];
            size_t o = es + static_cast<size_t>(j) * 12;
            result[o] = wrap_round(c[2]);
            result[o + 1] = wrap_round(c[1]);
            result[o + 2] = wrap_round(c[0]);
        }
        return result;
    }
    return result;
}

const SubEntry* pick_geo_sub(const std::vector<SubEntry>& subs, uint32_t geo_key) {
    const SubEntry* best = nullptr;
    const uint32_t one = 1;
    for (const SubEntry& s : subs) {
        if (s.key != geo_key) continue;
        if (s.gro_null || s.gro_type != 1) continue;
        if (!is_geometry_entry(s.data.data(), s.data.size(), &one)) continue;
        if (best == nullptr || s.data.size() > best->data.size()) best = &s;
    }
    return best;
}

std::vector<uint8_t> splice_sub_entry(const std::vector<uint8_t>& dec,
                                      size_t sub_offset, uint32_t sub_size,
                                      const std::vector<uint8_t>& new_payload) {
    size_t payload_start = sub_offset + 12;
    size_t payload_end = payload_start + (sub_size - 4);
    uint32_t new_size = 4 + static_cast<uint32_t>(new_payload.size());
    std::vector<uint8_t> out;
    out.reserve(dec.size() - (sub_size - 4) + new_payload.size());
    out.insert(out.end(), dec.begin(), dec.begin() + static_cast<long>(sub_offset - 4));
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(new_size >> (8 * i)));
    out.insert(out.end(), dec.begin() + static_cast<long>(sub_offset),
               dec.begin() + static_cast<long>(payload_start));
    out.insert(out.end(), new_payload.begin(), new_payload.end());
    out.insert(out.end(), dec.begin() + static_cast<long>(payload_end), dec.end());
    return out;
}

bool shipped_skinned_strategy(const std::string& bf_path, bool has_skin) {
    if (!has_skin) return false;
    const gameprofiles::GameProfile* p = gameprofiles::detect(bf_path);
    return p != nullptr && p->platform == "PC" &&
           (p->code == "SoT" || p->code == "WW" || p->code == "T2T");
}

// _build_geo_payload's vb_magic derivation: preserve the original cooked-VB tag
// (2/5/6/8; anything else -> 2) so the engine keeps its draw path.
uint32_t vb_magic_of(const uint8_t* raw, size_t n) {
    CookedVbSection sec = cooked_vb_section(raw, n);
    if (sec.ok && sec.data_start >= 12) {
        size_t o = sec.data_start - 12;
        uint32_t m = static_cast<uint32_t>(raw[o]) | (static_cast<uint32_t>(raw[o + 1]) << 8) |
                     (static_cast<uint32_t>(raw[o + 2]) << 16) | (static_cast<uint32_t>(raw[o + 3]) << 24);
        if (m == 2 || m == 5 || m == 8 || m == 6) return m;
    }
    return 2;
}

// A GEO-retained skin -> the builder's skin shape (weight = pond / 65535.0,
// matching geometry.py's parse; _serialize_skin's repack restores the pond).
gltf::GltfSkin geo_skin_to_gltf(const GeoInfo& geo) {
    gltf::GltfSkin skin;
    skin.flags = geo.skin_flags;
    for (const GeoBone& b : geo.skin_bones) {
        gltf::GltfBone gb;
        gb.bone_idx = b.bone_idx;
        for (int k = 0; k < 16; ++k) gb.bind_matrix[static_cast<size_t>(k)] = double(b.bind_matrix[static_cast<size_t>(k)]);
        gb.matrix_type = b.matrix_type;
        gb.weights.reserve(b.weights.size());
        for (const auto& w : b.weights)
            gb.weights.push_back({w.first, double(w.second) / 65535.0});
        skin.bones.push_back(std::move(gb));
    }
    return skin;
}

SwapStats swap_mesh_in_bf(const std::string& bf_path, uint32_t entry_idx,
                          uint32_t geo_key, const std::string& glb_path,
                          bool import_vertex_colors, bool backup) {
    SwapStats st;
    st.geo_key = geo_key;
    auto fail = [&](const std::string& msg) { st.error = msg; return st; };

    std::error_code ec;
    const std::filesystem::path archive_fs = std::filesystem::u8path(bf_path);
    const std::filesystem::path glb_fs = std::filesystem::u8path(glb_path);
    if (!std::filesystem::exists(glb_fs, ec))
        return fail("GLB not found: " + glb_path);
    if (backup) {
        std::string bak = bf_path + ".bak";
        const std::filesystem::path backup_fs = std::filesystem::u8path(bak);
        if (!std::filesystem::exists(backup_fs, ec))
            std::filesystem::copy_file(archive_fs, backup_fs, ec);
    }

    BigFile bf;
    try { bf.open(bf_path); } catch (const std::exception& e) { return fail(e.what()); }
    auto fit = bf.files.find(entry_idx);
    if (fit == bf.files.end()) return fail("BF entry not found");
    BFFile& fi = fit->second;

    LzoResult dr = decompress_lzo(bf.read_data(entry_idx));
    if (!dr.ok) return fail("could not decompress BF entry");
    const std::vector<uint8_t>& dec = dr.data;

    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = pick_geo_sub(subs, geo_key);
    if (target == nullptr) return fail("no .geo sub-entry with that key");

    GeoInfo orig_geo = parse_geometry(target->data.data(), target->data.size());
    if (!orig_geo.ok) return fail("could not parse original GEO");
    if (orig_geo.ps2) return fail("GEO is in PS2 format (VIF)");

    std::ifstream gin(glb_fs, std::ios::binary);
    std::vector<uint8_t> glb((std::istreambuf_iterator<char>(gin)),
                             std::istreambuf_iterator<char>());
    gltf::MeshData md;
    try { md = gltf::parse_glb_mesh(glb.data(), glb.size()); }
    catch (const std::exception& e) { return fail(std::string("GLB parse: ") + e.what()); }

    st.orig_points = orig_geo.nb_points;
    st.orig_uvs = orig_geo.nb_uvs;
    st.orig_tris = orig_geo.nb_tris;
    st.orig_payload = target->data.size();
    st.had_skin = md.has_skin;
    st.orig_had_colors = !orig_geo.colors.empty();

    // skin_for_build = mesh skin or the ORIGINAL GEO's retained skin.
    gltf::MeshData build_md = md;
    if (!md.has_skin && orig_geo.skin_present) {
        build_md.skin = geo_skin_to_gltf(orig_geo);
        build_md.has_skin = true;
    }
    bool skin_flag = build_md.has_skin && !build_md.skin.bones.empty();
    bool shipped = shipped_skinned_strategy(bf_path, skin_flag);

    // Vertex-colour policy (+ the original-GAO-RLI fallback for static meshes).
    std::vector<std::array<int, 4>> orig_colors;
    for (size_t i = 0; i + 3 < orig_geo.colors.size(); i += 4)
        orig_colors.push_back({orig_geo.colors[i], orig_geo.colors[i + 1],
                               orig_geo.colors[i + 2], orig_geo.colors[i + 3]});
    bool count_matches = md.vertices.size() == orig_geo.nb_points;

    std::vector<std::array<int, 4>> colors_for_build;
    std::string src = "none";
    if (import_vertex_colors && md.has_colors) { colors_for_build = md.colors; src = "glb"; }
    else if (!orig_colors.empty() && count_matches) { colors_for_build = orig_colors; src = "original"; }
    else if (!orig_colors.empty()) { src = "dropped"; }
    else if (md.has_colors) { colors_for_build = md.colors; src = "glb"; }

    if (src == "glb" && !import_vertex_colors && orig_colors.empty()) {
        OrigRliColors orl = original_rli_colors(subs, geo_key, orig_geo, md.vertices);
        if (orl.ok && !orl.colors.empty()) {
            colors_for_build.clear();
            for (const Rgba4& c : orl.colors)
                colors_for_build.push_back({int(c[0]), int(c[1]), int(c[2]), int(c[3])});
            src = "original_rli";
        }
    }

    gltf::GeoBuildOpts opts;
    opts.version = orig_geo.version;
    opts.flags1 = orig_geo.flags1;
    opts.flags2 = orig_geo.flags2;
    opts.vb_magic = vb_magic_of(target->data.data(), target->data.size());
    opts.shipped_skinned = shipped;
    opts.orig_skinned_stride = gltf::orig_skinned_vb_stride(target->data.data(),
                                                            target->data.size());
    // Only meshes that ORIGINALLY had GEO dul_PointColors get body colours; a
    // static-RLI mesh keeps its body colourless (colours live in the GAO RLI).
    bool body_colors = !orig_colors.empty() && !colors_for_build.empty();
    if (body_colors) opts.colors = &colors_for_build;

    std::vector<uint8_t> new_payload = gltf::build_geo_payload(build_md, opts);
    st.new_points = static_cast<uint32_t>(md.vertices.size());
    st.new_uvs = static_cast<uint32_t>(md.uvs.size());
    st.new_tris = static_cast<uint32_t>(md.faces.size());
    st.new_payload = new_payload.size();

    std::vector<uint8_t> new_dec =
        splice_sub_entry(dec, target->offset, target->size, new_payload);

    // Rewrite each host GAO's per-instance colour table for glb/original_rli.
    if ((src == "glb" || src == "original_rli") && !colors_for_build.empty()) {
        std::vector<Rgba4> rc;
        rc.reserve(colors_for_build.size());
        for (const auto& c : colors_for_build)
            rc.push_back({double(c[0]), double(c[1]), double(c[2]), double(c[3])});
        std::vector<SubEntry> subs2 = walk_sub_entries(new_dec);
        struct Upd { size_t off; uint32_t size; std::vector<uint8_t> payload; };
        std::vector<Upd> updates;
        for (const SubEntry& s2 : subs2) {
            if (s2.ext != ".gao") continue;
            GaoInfo gi = parse_gao_full(s2.data.data(), s2.data.size());
            if (!gi.ok || !gi.vis_read || gi.gro_key != geo_key) continue;
            std::vector<uint8_t> ng = rewrite_gao_instance_colors(
                s2.data.data(), s2.data.size(), geo_key, st.orig_points, rc,
                new_payload.data(), new_payload.size());
            if (!ng.empty() && ng != s2.data)
                updates.push_back({s2.offset, s2.size, std::move(ng)});
        }
        std::sort(updates.begin(), updates.end(),
                  [](const Upd& a, const Upd& b) { return a.off > b.off; });
        for (const Upd& u : updates)
            new_dec = splice_sub_entry(new_dec, u.off, u.size, u.payload);
        st.gao_color_updates = static_cast<uint32_t>(updates.size());
    }
    st.wrote_colors = static_cast<uint32_t>(colors_for_build.size());
    st.color_source = src;
    st.decompressed_size = new_dec.size();

    // Recompress + write back. Level 9 = the vendored lzo1x_999 — the same
    // optimizer python-lzo runs for compress_lzo_9, so the archive bytes now
    // match the Python op exactly (not just the decompressed content).
    std::vector<uint8_t> compressed = compress_lzo(new_dec, 9);
    std::fstream f(archive_fs,
                   std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return fail("cannot open archive for writing");
    st.bf_entry_pos = bf.write_entry(f, compressed, fi);
    st.compressed_size = compressed.size();
    bf.flush_size_grs(f);
    st.ok = true;
    return st;
}

std::vector<ResolvedTex> resolve_element_texture_keys(
    const std::vector<SubEntry>& subs, const GeoInfo& geo, uint32_t geo_key) {
    size_t nb_elems = geo.elements.size() / 2;   // flat (nTri, matId) pairs
    if (nb_elems == 0) return {};

    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;

    uint32_t grm_key = 0;
    bool have_grm = owning_grm_key(subs, geo_key, grm_key);
    std::vector<uint32_t> sub_mat_keys;
    if (have_grm) {
        auto it = by_key.find(grm_key);
        sub_mat_keys = grm_sub_material_keys(it == by_key.end() ? nullptr : it->second);
    }

    int64_t n = static_cast<int64_t>(sub_mat_keys.size());
    std::vector<ResolvedTex> out;
    out.reserve(nb_elems);
    for (size_t e = 0; e < nb_elems; ++e) {
        ResolvedTex r;
        if (n > 0) {
            int64_t mat_id = geo.elements[e * 2 + 1];   // element matId (engine clamps)
            uint32_t matk = sub_mat_keys[static_cast<size_t>(clamp_matid(mat_id, n))];
            auto it = by_key.find(matk);
            if (it != by_key.end()) {
                uint32_t texk = resolve_texture_key(it->second->data.data(),
                                                    it->second->data.size());
                if (texk != 0) { r.ok = true; r.key = texk; }
            }
        }
        out.push_back(r);
    }
    return out;
}

}  // namespace jade
