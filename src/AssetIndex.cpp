// AssetIndex.cpp — implementation. Faithful port of core/asset_index.py
// (read path; classifier elif-chain order + ref extraction + index maps).
#include "jade/AssetIndex.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/Light.hpp"
#include "jade/Material.hpp"
#include "jade/ObjectKinds.hpp"
#include "jade/Texture.hpp"

namespace jade {

// ── Category constants ──
const char* const CAT_TEXTURE      = "Textures";
const char* const CAT_GEOMETRY     = "Geometry";
const char* const CAT_MATERIAL     = "Materials";
const char* const CAT_GAO          = "Game Objects";
const char* const CAT_LIGHT        = "Lights";
const char* const CAT_ANIMATION    = "Animations";
const char* const CAT_SOUND        = "Sounds";
const char* const CAT_AI           = "AI Scripts";
const char* const CAT_PALETTE      = "Palettes";
const char* const CAT_TEXTURE_DATA = "Texture data";
const char* const CAT_DATATABLE    = "Data tables";
const char* const CAT_REFERENCE    = "Resource Index";
const char* const CAT_OTHER        = "Other";
const char* const CAT_CAMERA       = "Cameras";
const char* const CAT_FX           = "FX / Particles";
const char* const CAT_TRIGGER      = "Triggers";
const char* const CAT_TRAP         = "Traps";
const char* const CAT_ACTOR        = "Actors / Enemies";
const char* const CAT_SPAWNER      = "Spawners";
const char* const CAT_WAYPOINT     = "Waypoints";
const char* const CAT_LOGIC        = "Logic / Managers";

namespace {

inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) |
           (static_cast<uint32_t>(p[o + 3]) << 24);
}

// True for a 4-byte buffer that is '.' + 3 ASCII-alpha bytes (Python's
// payload[1:4].isalpha(): all chars alphabetic, and non-empty).
inline bool is_ext_tag(const uint8_t* d, size_t n) {
    if (n < 4 || d[0] != '.') return false;
    for (int i = 1; i < 4; ++i) {
        unsigned char c = d[i];
        if (!std::isalpha(c)) return false;
    }
    return true;
}

// object_kinds category-kind -> browser category, mirroring MARKER_CATEGORY.
// Returns nullptr (the Python None) for "visual"/"bone"/"other" (GAO stays
// generic) — note kinds 'visual'/'bone' never reach this map in classify_sub.
const char* marker_category(const std::string& kind) {
    if (kind == "camera")   return CAT_CAMERA;
    if (kind == "light")    return CAT_LIGHT;
    if (kind == "sound")    return CAT_SOUND;
    if (kind == "fx")       return CAT_FX;
    if (kind == "trigger")  return CAT_TRIGGER;
    if (kind == "trap")     return CAT_TRAP;
    if (kind == "actor")    return CAT_ACTOR;
    if (kind == "spawner")  return CAT_SPAWNER;
    if (kind == "waypoint") return CAT_WAYPOINT;
    if (kind == "logic")    return CAT_LOGIC;
    return nullptr;  // "other" -> None (stays Game Objects)
}

// EXT_TYPE_INFO: declared extension -> (category-or-None, friendly name).
// cat CAT_OTHER here covers the Python tuples whose cat is None (record
// stays Other but gets a friendly, searchable name).
struct ExtInfo {
    bool found = false;
    const char* cat = CAT_OTHER;
    const char* label = "";
};
ExtInfo ext_type_info(const std::string& ext) {
    if (ext == ".ofc")  return {true, CAT_AI, "AI function (.ofc)"};
    if (ext == ".ova")  return {true, CAT_AI, "AI vars (.ova)"};
    if (ext == ".txi")  return {true, CAT_TEXTURE_DATA, "Texture info (.txi)"};
    if (ext == ".txs")  return {true, CAT_TEXTURE_DATA,
                                "Texture surface (.txs)"};
    if (ext == ".ttt")  return {true, CAT_DATATABLE, "Data table (.ttt)"};
    if (ext == ".txg")  return {true, CAT_OTHER, "Text/lang group (.txg)"};
    if (ext == ".gri")  return {true, CAT_OTHER, "Grid (.gri)"};
    if (ext == ".bra")  return {true, CAT_OTHER, "BRA resource (.bra)"};
    if (ext == ".nova") return {true, CAT_OTHER, "NOVA resource (.nova)"};
    return {false, CAT_OTHER, ""};
}

// object_kinds.category_label — the marker detail string uses it
// ("Camera marker" / "Spawner marker" / …).
const char* kind_label(const std::string& kind) {
    if (kind == "camera")   return "Camera";
    if (kind == "light")    return "Light";
    if (kind == "sound")    return "Sound";
    if (kind == "fx")       return "FX / Particle";
    if (kind == "trigger")  return "Trigger / Sector";
    if (kind == "trap")     return "Trap";
    if (kind == "actor")    return "Actor / Enemy";
    if (kind == "spawner")  return "Spawner";
    if (kind == "waypoint") return "Waypoint / Path";
    if (kind == "logic")    return "Logic / Manager";
    return "Other";
}

// GAO_CATEGORIES: categories whose members are GAOs — generic game objects
// plus every marker kind. Drives the geo-group ref expansion and the
// referrer-name hop (a mesh reclassified as "Actors / Enemies" must still
// hand its name down to the geometry it references).
bool in_gao_categories(const std::string& cat) {
    return cat == CAT_GAO || cat == CAT_CAMERA || cat == CAT_LIGHT
        || cat == CAT_SOUND || cat == CAT_FX || cat == CAT_TRIGGER
        || cat == CAT_TRAP || cat == CAT_ACTOR || cat == CAT_SPAWNER
        || cat == CAT_WAYPOINT || cat == CAT_LOGIC;
}

// Python f"{n:,}" — thousands separators for the size fallback detail.
std::string comma_grouped(size_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

// ── helpers ──

bool looks_like_ref_list(const uint8_t* d, size_t n) {
    return n >= 8 && is_ext_tag(d, n);
}

std::vector<RefEntry> decode_ref_list(const uint8_t* d, size_t n) {
    std::vector<RefEntry> out;
    if (n < 8) return out;
    for (size_t off = 0; off + 8 <= n; off += 8) {
        // Python loops range(0, len-7, 8): off+8 <= n covers the same window.
        if (!is_ext_tag(d + off, 4)) break;
        std::string ext(reinterpret_cast<const char*>(d + off), 4);
        uint32_t key = le32(d, off + 4);
        out.push_back({ext, key});
    }
    return out;
}

LightKeyOpt light_key_from_gao_payload(const uint8_t* d, size_t n) {
    LightKeyOpt r;
    if (d == nullptr || n < 4) return r;
    r.have = true;
    r.key = le32(d, n - 4);
    return r;
}

// classify_gao_marker -> object_kinds kind string ('visual'/'bone'/category).
static std::string classify_gao_marker_kind(
    const SubEntry& sub, const GaoInfo& full,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key,
    const std::unordered_set<uint32_t>& gao_keys) {
    int gro_type = -1;  // Python None
    if (full.vis_flag && full.vis_read) {
        auto it = by_key.find(full.gro_key);
        if (it != by_key.end()) {
            const SubEntry* gsub = it->second;
            gro_type = gsub->gro_null ? -1 : static_cast<int>(gsub->gro_type);
            if (gro_type == 1) {
                GeoInfo g = parse_geometry(gsub->data.data(), gsub->data.size());
                if (g.ok && g.nb_tris > 0) return "visual";
            }
        }
    }
    const std::string& name = full.name;
    uint32_t father = full.hier_flag ? full.father_key : 0xFFFFFFFFu;
    bool father_in_bin = gao_keys.count(father) != 0;
    if (is_bone(name, full.identity, father_in_bin)) return "bone";
    std::string kind = classify_object(name, full.identity, gro_type, father_in_bin);
    // Light-marker promotion: the GAO owns a GRO_Light keyed by its last u32.
    LightKeyOpt cand = light_key_from_gao_payload(sub.data.data(), sub.data.size());
    if (cand.have) {
        auto it = by_key.find(cand.key);
        if (it != by_key.end()) {
            const SubEntry* lsub = it->second;
            if (!lsub->gro_null && lsub->gro_type == GRO_LIGHT) {
                LightInfo li = parse_light(lsub->data.data(), lsub->data.size());
                if (li.ok) kind = "light";
            }
        }
    }
    return kind;
}

// ── classifier ──

std::string AssetRecord::type_tag() const {
    if (!ext.empty()) return ext;
    if (gro_type != 0) {
        char text[24];
        std::snprintf(text, sizeof text, "gro 0x%08X", gro_type);
        return text;
    }
    return "(none)";
}

AssetRecord classify_sub_entry(
    uint32_t parent_index, uint32_t parent_key, uint32_t sub_index,
    const SubEntry& sub,
    const std::unordered_map<uint32_t, const SubEntry*>* by_key,
    const std::unordered_set<uint32_t>* gao_keys,
    const std::unordered_map<uint32_t, std::string>* ref_ext,
    const std::string& parent_name) {

    const uint8_t* payload = sub.data.data();
    size_t plen = sub.data.size();
    // Python: gro_type = sub.get('gro_type') or 0  (None -> 0, also 0 -> 0).
    uint32_t gro_type = sub.gro_null ? 0u : sub.gro_type;
    std::string ext = sub.ext;
    uint32_t key = sub.key;

    AssetRecord rec;
    rec.parent_index = parent_index;
    rec.sub_index    = sub_index;
    rec.key          = key;
    rec.parent_key   = parent_key;
    rec.parent_name  = parent_name;
    rec.gro_type     = gro_type;
    rec.ext          = ext;
    rec.payload_size = plen;

    std::string category = CAT_OTHER;

    auto add_ref = [&](const char* t, uint32_t k) {
        if (k && k != 0xFFFFFFFFu) rec.refs[t].push_back(k);
    };

    bool is_tex = (plen > 0 && is_texture_entry(payload, plen));
    uint32_t geo_gt = 1;
    bool is_geo = (gro_type == 1 && is_geometry_entry(payload, plen, &geo_gt));

    if (is_tex) {
        category = CAT_TEXTURE;
        // Detail mirrors _detail_for_texture: "512×256 DXT1 mip4".
        TexInfo ti = parse_texture(payload, plen);
        if (ti.valid) {
            static const std::map<uint32_t, const char*> kFmt = {
                {0, "BGRA"}, {1, "PAL8"}, {5, "DXT1"},
                {6, "DXT3"}, {7, "DXT5"}, {11, "4bpp"}};
            char b[64];
            auto it = kFmt.find(ti.format);
            if (it != kFmt.end())
                std::snprintf(b, sizeof b, "%u\xC3\x97%u %s", ti.width,
                              ti.height, it->second);
            else
                std::snprintf(b, sizeof b, "%u\xC3\x97%u fmt%u", ti.width,
                              ti.height, ti.format);
            rec.detail = b;
            if (ti.mip_count > 0) {
                std::snprintf(b, sizeof b, " mip%u", ti.mip_count);
                rec.detail += b;
            }
            std::snprintf(b, sizeof b, "Texture %u\xC3\x97%u", ti.width,
                          ti.height);
            rec.name = b;
        } else {
            rec.name = "Texture";
        }
    } else if (plen == 1024 && gro_type != 1 && gro_type != 2 && gro_type != 3 &&
               gro_type != 4 && gro_type != 5 &&
               !looks_like_ref_list(payload, plen)) {
        category = CAT_PALETTE;
        rec.name = "Palette";
        rec.detail = "256-colour BGRA LUT";
    } else if (is_geo) {
        category = CAT_GEOMETRY;
        // Detail mirrors _detail_for_geometry: "412v 220t skinned (28)".
        uint32_t nb_points = 0;
        try {
            GeoInfo geo = parse_geometry(payload, plen);
            if (geo.ok) {
                nb_points = geo.nb_points;
                char b[64];
                std::snprintf(b, sizeof b, "%uv %zut", geo.nb_points,
                              geo.faces.size() / 7);
                rec.detail = b;
                if (geo.skin_present) {
                    std::snprintf(b, sizeof b, " skinned (%zu bones)",
                                  geo.skin_bones.size());
                    rec.detail += b;
                }
            }
        } catch (...) {}
        char b[32];
        std::snprintf(b, sizeof b, "Geometry %uv", nb_points);
        rec.name = b;
    } else if (gro_type == GRO_LIGHT && is_light_payload(payload, plen)) {
        category = CAT_LIGHT;
        try {
            LightInfo li = parse_light(payload, plen);
            if (li.ok) rec.detail = li.type_name;
        } catch (...) {}
        rec.name = rec.detail.empty() ? "Light" : "Light (" + rec.detail + ")";
    } else if (gro_type == 6) {
        category = CAT_CAMERA;
        rec.name = "Camera";
        rec.detail = "camera resource";
    } else if (gro_type == 8) {
        category = CAT_GEOMETRY;
        rec.name = "Geometry LOD group";
        rec.detail = "static LOD group";
    } else if (gro_type == 11) {
        category = CAT_FX;
        rec.name = "Particle generator";
        rec.detail = "particle resource";
    } else if (ext == ".gao" || ext == ".wol") {
        category = CAT_GAO;
        GaoInfo full = parse_gao_full(payload, plen);
        rec.name = full.name;
        if (rec.name.empty() && plen > 16) {
            // _name_for_gao / parse_gao_header fallback: the NUL-terminated
            // name at offset 16 is readable even when the full parse fails.
            size_t end = 16;
            while (end < plen && payload[end] != 0) ++end;
            if (end > 16 && end < 16 + 120 && end < plen)
                rec.name.assign(reinterpret_cast<const char*>(payload) + 16,
                                end - 16);
        }
        if (rec.name.empty())
            rec.name = ext == ".gao" ? "Game Object" : "World";
        rec.detail = ext;
        if (full.ok) {
            if (full.vis_flag && full.vis_read) {
                add_ref("geometry", full.gro_key);
                add_ref("material", full.grm_key);
            }
            if (full.hier_flag) add_ref("parent_gao", full.father_key);
            // gizmo_flat = (gao_key, mat_id) pairs; ref the gao_key of each.
            for (size_t i = 0; i + 1 < full.gizmo_flat.size(); i += 2)
                add_ref("gizmo", full.gizmo_flat[i]);
        }
        // Marker refinement (needs sibling context).
        if (ext == ".gao" && full.ok && by_key && gao_keys) {
            std::string kind = classify_gao_marker_kind(sub, full, *by_key, *gao_keys);
            const char* mcat = marker_category(kind);
            if (mcat) {
                category = mcat;
                rec.detail = std::string(kind_label(kind)) + " marker";
            }
        }
    } else if (gro_type == 3 || gro_type == 4 || gro_type == 5) {
        MatInfo mat = parse_material(payload, plen, static_cast<int>(gro_type));
        category = CAT_MATERIAL;
        if (mat.ok) {
            for (uint32_t tk : mat.texture_keys) add_ref("texture", tk);
            for (uint32_t sk : mat.sub_material_keys) add_ref("sub_material", sk);
            rec.detail = mat.type == 0   ? "single"
                         : mat.type == 1 ? "multitexture"
                         : mat.type == 2 ? "multi"
                                         : "";
        }
        // name = f"Material {detail}".strip()
        rec.name = rec.detail.empty() ? "Material" : "Material " + rec.detail;
    } else if (gro_type == 7) {
        category = CAT_OTHER;  // Waypoint
        rec.name = "Waypoint";
        rec.detail = "waypoint (GRO type 7)";
    } else if ((gro_type & 0xFFu) == 0x07u && gro_type > 0xFFu) {
        MatInfo mat = parse_material(payload, plen, static_cast<int>(gro_type));
        category = CAT_MATERIAL;
        rec.name = "Sub-material";
        rec.detail = "sub-material";
        if (mat.ok)
            for (uint32_t tk : mat.texture_keys) add_ref("texture", tk);
    } else if ((gro_type >> 16) == 0x0002u && (gro_type & 0xFFu) != 0x30u &&
               (gro_type & 0xFFu) != 0x24u) {
        category = CAT_ANIMATION;
        // PORT GAP: the "(Nt)" track-count name/detail needs parse_trl
        // (core/animation.py, unported) — plain "Animation" instead.
        rec.name = "Animation";
    } else if ((gro_type & 0xFFu) == 0x24u) {
        category = CAT_SOUND;
        rec.name = "Sound";
    } else if ((gro_type & 0xFFu) == 0x30u) {
        category = CAT_AI;
        rec.name = "AI Script";
    } else if (ext == ".wow") {
        category = CAT_GAO;
        rec.name = "World";
        rec.detail = ".wow (world)";
    } else if (looks_like_ref_list(payload, plen) ||
               (ext.empty() && gro_type > 0xFFFFu)) {
        category = CAT_REFERENCE;
        if (looks_like_ref_list(payload, plen)) {
            rec.name = "Dependency list";
            char b[48];
            std::snprintf(b, sizeof b, "[ext][key] refs (%zu)", plen / 8);
            rec.detail = b;
        } else {
            rec.name = "Index record";
            char b[32];
            std::snprintf(b, sizeof b, "link 0x%08X", gro_type);
            rec.detail = b;
        }
    } else {
        // The body 'type' word didn't identify this record. Fall back to
        // the extension it's DECLARED under in the bin's dependency lists
        // — the engine's real type tag. .ofc/.ova are AI resources; the
        // rest get a friendly, searchable name but stay Other.
        category = CAT_OTHER;
        std::string decl;
        if (ref_ext) {
            auto it = ref_ext->find(key);
            if (it != ref_ext->end()) decl = it->second;
        }
        ExtInfo info = decl.empty() ? ExtInfo{} : ext_type_info(decl);
        if (info.found) {
            category = info.cat;
            rec.name = info.label;
            rec.detail = decl + " (declared)";
        } else {
            // Genuinely unidentified — keep visible under "Other" with a
            // tag the user can search/filter on. Drives the RE backlog.
            char b[32];
            std::snprintf(b, sizeof b, "sub#%u", sub_index);
            rec.name = b;
            if (!decl.empty()) {
                rec.name = decl + " resource";
                rec.detail = decl + " (declared)";
            } else if (gro_type) {
                std::snprintf(b, sizeof b, "type 0x%08X", gro_type);
                rec.detail = b;
            } else if (!ext.empty()) {
                rec.detail = ext;
            } else {
                rec.detail = comma_grouped(plen) + "B";
            }
        }
    }

    if (rec.name.empty()) {
        char b[32];
        std::snprintf(b, sizeof b, "sub#%u", sub_index);
        rec.name = b;
    }
    rec.category = category;
    return rec;
}

// ── referrer-name resolution (_resolve_referrer_names) ──
//
// Replace generic "Geometry 384v" / "Material" labels with the name of the
// GAO that references them. In Jade each visual GAO names what it carries
// (M_Prince_Face_Highres.gao), so propagating that label down the reference
// chain (GAO → material → texture, GAO → geometry) is far more useful in
// the asset browser than a vertex count or "Texture 256×256". Runs
// per-parent over that bin's freshly-added records ([first, end)).
static void resolve_referrer_names(std::vector<AssetRecord>& records,
                                   size_t first) {
    auto fmt = [](const std::vector<std::string>& names) -> std::string {
        if (names.empty()) return "";
        if (names.size() == 1) return names.front();
        return names.front() + " (+" + std::to_string(names.size() - 1) + ")";
    };

    // Hop 1: GAO → geometry, GAO → material. Any GAO-kind record (generic
    // Game Object *or* a marker/actor/camera reclassification) can name
    // what it carries — a mesh reclassified as "Actors / Enemies" (SoT's
    // M_Farah_Body) must still hand its name down to the geometry it
    // references, or the mesh shows as "Geometry 984v".
    std::map<uint32_t, std::vector<std::string>> geo_refs, mat_refs;
    for (size_t i = first; i < records.size(); ++i) {
        const AssetRecord& r = records[i];
        if (!in_gao_categories(r.category) || r.name.empty()) continue;
        std::string nm = r.name;
        if (nm.size() >= 4 && nm.compare(nm.size() - 4, 4, ".gao") == 0)
            nm.resize(nm.size() - 4);
        auto git = r.refs.find("geometry");
        if (git != r.refs.end())
            for (uint32_t gk : git->second) geo_refs[gk].push_back(nm);
        auto mit = r.refs.find("material");
        if (mit != r.refs.end())
            for (uint32_t mk : mit->second) mat_refs[mk].push_back(nm);
    }
    for (size_t i = first; i < records.size(); ++i) {
        AssetRecord& r = records[i];
        if (r.category == CAT_GEOMETRY) {
            auto it = geo_refs.find(r.key);
            if (it != geo_refs.end()) {
                std::string nm = fmt(it->second);
                if (!nm.empty()) r.name = std::move(nm);
            }
        } else if (r.category == CAT_MATERIAL) {
            auto it = mat_refs.find(r.key);
            if (it != mat_refs.end()) {
                std::string nm = fmt(it->second);
                if (!nm.empty()) r.name = std::move(nm);
            }
        }
    }

    // Hop 2+: walk the material chain (multi → sub-material → ...) so a
    // sub-material inherits its multi-material's resolved name. Loop until
    // nothing else changes; depth is bounded by the chain length
    // (typically 2).
    auto unresolved = [](const std::string& n) {
        return n.rfind("Material", 0) == 0 || n.rfind("Sub-material", 0) == 0;
    };
    std::map<uint32_t, std::vector<size_t>> mats_by_key;
    for (size_t i = first; i < records.size(); ++i)
        if (records[i].category == CAT_MATERIAL)
            mats_by_key[records[i].key].push_back(i);
    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (size_t i = first; i < records.size(); ++i) {
            const AssetRecord& r = records[i];
            auto sit = r.refs.find("sub_material");
            if (sit == r.refs.end()) continue;
            // Only a record whose own name is resolved (carries a GAO
            // name) propagates it to its still-generic sub-materials.
            if (unresolved(r.name)) continue;
            for (uint32_t sk : sit->second) {
                auto mit = mats_by_key.find(sk);
                if (mit == mats_by_key.end()) continue;
                for (size_t si : mit->second) {
                    if (unresolved(records[si].name)) {
                        records[si].name = r.name;
                        changed = true;
                    }
                }
            }
        }
        if (!changed) break;
    }

    // Final hop: material → texture (use the material's now-resolved name).
    std::map<uint32_t, std::vector<std::string>> tex_refs;
    for (size_t i = first; i < records.size(); ++i) {
        const AssetRecord& r = records[i];
        if (r.category != CAT_MATERIAL) continue;
        auto tit = r.refs.find("texture");
        if (tit != r.refs.end())
            for (uint32_t tk : tit->second) tex_refs[tk].push_back(r.name);
    }
    for (size_t i = first; i < records.size(); ++i) {
        AssetRecord& r = records[i];
        if (r.category != CAT_TEXTURE) continue;
        auto it = tex_refs.find(r.key);
        if (it != tex_refs.end()) {
            std::string nm = fmt(it->second);
            if (!nm.empty()) r.name = std::move(nm);
        }
    }
}

// ── AssetIndex ──

void AssetIndex::add(AssetRecord rec) {
    size_t at = records.size();
    std::pair<uint32_t, uint32_t> aid{rec.parent_index, rec.sub_index};
    by_id[aid] = at;
    by_key[rec.key].push_back(at);
    by_parent[rec.parent_index].push_back(at);
    for (const auto& kv : rec.refs)
        for (uint32_t k : kv.second)
            if (k && k != 0xFFFFFFFFu) reverse_refs[k].insert(aid);
    records.push_back(std::move(rec));
    material_families_valid_ = false;
}

std::map<std::string, size_t> AssetIndex::count_by_category() const {
    std::map<std::string, size_t> out;
    for (const auto& r : records) out[r.category] += 1;
    return out;
}

const AssetRecord* AssetIndex::find_by_id(uint32_t parent_index,
                                          uint32_t sub_index) const {
    auto it = by_id.find({parent_index, sub_index});
    return it == by_id.end() ? nullptr : &records[it->second];
}

const AssetRecord* AssetIndex::find_first_by_key(uint32_t key) const {
    const auto it = by_key.find(key);
    return (it == by_key.end() || it->second.empty())
               ? nullptr
               : &records[it->second.front()];
}

std::vector<const AssetRecord*> AssetIndex::find_resolved(uint32_t key) const {
    std::vector<const AssetRecord*> out;
    auto it = by_key.find(key);
    if (it != by_key.end())
        for (size_t i : it->second) out.push_back(&records[i]);
    return out;
}

std::vector<const AssetRecord*> AssetIndex::referrers(uint32_t key) const {
    std::vector<const AssetRecord*> out;
    auto it = reverse_refs.find(key);
    if (it == reverse_refs.end()) return out;
    for (const auto& aid : it->second) {
        const AssetRecord* rec = find_by_id(aid.first, aid.second);
        if (rec != nullptr) out.push_back(rec);
    }
    return out;
}

const std::map<uint32_t, std::vector<size_t>>&
AssetIndex::material_containers_by_family() const {
    if (material_families_valid_) return material_families_;
    material_families_.clear();
    for (size_t index = 0; index < records.size(); ++index) {
        const AssetRecord& rec = records[index];
        if (rec.category == CAT_MATERIAL && rec.gro_type == 4)
            material_families_[rec.key & 0xFFFF0000u].push_back(index);
    }
    material_families_valid_ = true;
    return material_families_;
}

AssetIndex build_asset_index(const BigFile& bf, size_t limit,
                             const std::function<void(int, int)>& progress,
                             const std::function<bool()>& cancel) {
    AssetIndex idx;
    // Active entries with non-empty name, valid key, and length > 0 — sorted by
    // index (std::map is ascending), matching iter_parent_subs' filter.
    std::vector<uint32_t> active;
    for (const auto& kv : bf.files) {
        const BFFile& f = kv.second;
        if (!f.name.empty() && f.key != INVALID_KEY && f.length > 0)
            active.push_back(f.index);
    }
    size_t scan_n = (limit == 0) ? active.size() : std::min(limit, active.size());

    bool cancelled = false;
    for (size_t ci = 0; ci < scan_n; ++ci) {
        // iter_parent_subs reports before attempting entries 0, 50, 100, …;
        // this intentionally gives the GUI a zero-based first callback.
        if (progress && ci % 50 == 0)
            progress(int(ci), int(scan_n));
        uint32_t fidx = active[ci];
        const BFFile& fi = bf.files.at(fidx);
        std::vector<uint8_t> raw = bf.read_data(fidx);
        if (raw.empty()) continue;
        LzoResult dec = decompress_lzo(raw);
        if (!dec.ok) continue;
        std::vector<SubEntry> subs;
        try {
            subs = walk_sub_entries(dec.data);
        } catch (...) {
            continue;
        }
        // Python polls cancel in build_asset_index only after its generator
        // has successfully yielded a decompressed parent, before classifying
        // that parent's records. Unreadable parents therefore do not consume
        // a cancellation poll.
        if (cancel && cancel()) {
            cancelled = true;
            break;
        }

        // Per-bin maps (stable: first occurrence of each key wins, like dict).
        std::unordered_map<uint32_t, const SubEntry*> by_key;
        for (const SubEntry& s : subs)
            by_key[s.key] = &s;  // LAST wins, matching Python dict-comprehension
        // geo_keys / gao_keys are derived FROM the dedup'd by_key (so a key that
        // appears twice is classified by its last occurrence), exactly as the
        // Python set-comprehensions over by_key.items() do.
        std::unordered_set<uint32_t> geo_keys, gao_keys;
        for (const auto& kv : by_key) {
            const SubEntry* s = kv.second;
            if (!s->gro_null && s->gro_type == 1) geo_keys.insert(kv.first);
            if (s->ext == ".gao" || s->ext == ".wol") gao_keys.insert(kv.first);
        }
        // ref_ext: key -> declared extension, from dependency-list sub-entries.
        std::unordered_map<uint32_t, std::string> ref_ext;
        for (const SubEntry& s : subs) {
            uint32_t gt = s.gro_null ? 0u : s.gro_type;
            const uint8_t* d = s.data.data();
            size_t dn = s.data.size();
            if (s.ext.empty() && gt > 0xFFFFu && is_ext_tag(d, dn)) {
                for (const RefEntry& e : decode_ref_list(d, dn))
                    ref_ext.emplace(e.key, e.ext);  // setdefault: first wins
            }
        }

        size_t first_new = idx.records.size();
        for (size_t si = 0; si < subs.size(); ++si) {
            const SubEntry& s = subs[si];
            // Placeholder-texture filtering.
            if (!s.data.empty() && is_texture_entry(s.data.data(), s.data.size())) {
                TexInfo ti = parse_texture(s.data.data(), s.data.size());
                if (!ti.valid) continue;
                size_t pixlen = (s.data.size() > ti.pix_start)
                                    ? s.data.size() - ti.pix_start : 0;
                if (is_placeholder(ti, pixlen)) continue;
            }
            AssetRecord rec = classify_sub_entry(
                fi.index, fi.key, static_cast<uint32_t>(si), s,
                &by_key, &gao_keys, &ref_ext, fi.name);
            // Expand a GAO's geometry refs into real member GEOs (geo
            // groups) for ANY GAO-kind category — a mesh reclassified as
            // a marker kind still names a group. Done before add() so
            // reverse_refs records the members, not the group key.
            if (in_gao_categories(rec.category)) {
                auto git = rec.refs.find("geometry");
                if (git != rec.refs.end()) {
                    std::vector<uint32_t> expanded;
                    for (uint32_t gk : git->second) {
                        std::vector<uint32_t> mem =
                            geo_group_members(gk, by_key, &geo_keys);
                        expanded.insert(expanded.end(), mem.begin(), mem.end());
                    }
                    git->second = std::move(expanded);
                }
            }
            idx.add(std::move(rec));
        }
        resolve_referrer_names(idx.records, first_new);
    }
    // Exhausting iter_parent_subs emits the terminal callback. Breaking the
    // consumer on cancellation closes the generator and skips this callback.
    if (progress && !cancelled)
        progress(int(scan_n), int(scan_n));
    return idx;
}

}  // namespace jade
