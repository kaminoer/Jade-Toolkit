// Exporter.cpp — implementation. Faithful port of the exporter's
// non-animation subset, including static scenes and character bundles.
#include "jade/Exporter.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "jade/Compression.hpp"
#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/GltfBuilder.hpp"
#include "jade/Image.hpp"
#include "jade/Json.hpp"
#include "jade/Material.hpp"
#include "jade/Skeleton.hpp"
#include "jade/Texture.hpp"

namespace jade {
namespace exporter {

namespace {

std::string jesc(const std::string& s) {
    std::string o = "\"";
    auto append_u16 = [&](uint32_t value) {
        char b[8];
        std::snprintf(b, sizeof b, "\\u%04x", unsigned(value));
        o += b;
    };
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        if (c < 0x80) {
            ++i;
            if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
            else if (c == '\b') o += "\\b";
            else if (c == '\f') o += "\\f";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\t') o += "\\t";
            else if (c < 0x20) append_u16(c);
            else o += char(c);
            continue;
        }

        // Python json.dump defaults to ensure_ascii=True. Decode valid UTF-8
        // and emit the same lowercase BMP or surrogate-pair escapes. Invalid
        // bytes use U+FFFD, matching BigFile's errors='replace' name decoder.
        uint32_t cp = 0xFFFD;
        size_t width = 1;
        if (c >= 0xC2 && c <= 0xDF && i + 1 < s.size() &&
            (uint8_t(s[i + 1]) & 0xC0) == 0x80) {
            cp = ((c & 0x1F) << 6) | (uint8_t(s[i + 1]) & 0x3F);
            width = 2;
        } else if (c >= 0xE0 && c <= 0xEF && i + 2 < s.size() &&
                   (uint8_t(s[i + 1]) & 0xC0) == 0x80 &&
                   (uint8_t(s[i + 2]) & 0xC0) == 0x80) {
            const uint8_t c1 = uint8_t(s[i + 1]);
            const uint32_t candidate = ((c & 0x0F) << 12) |
                ((c1 & 0x3F) << 6) | (uint8_t(s[i + 2]) & 0x3F);
            if (candidate >= 0x800 && !(candidate >= 0xD800 &&
                                       candidate <= 0xDFFF)) {
                cp = candidate;
                width = 3;
            }
        } else if (c >= 0xF0 && c <= 0xF4 && i + 3 < s.size() &&
                   (uint8_t(s[i + 1]) & 0xC0) == 0x80 &&
                   (uint8_t(s[i + 2]) & 0xC0) == 0x80 &&
                   (uint8_t(s[i + 3]) & 0xC0) == 0x80) {
            const uint32_t candidate = ((c & 0x07) << 18) |
                ((uint8_t(s[i + 1]) & 0x3F) << 12) |
                ((uint8_t(s[i + 2]) & 0x3F) << 6) |
                (uint8_t(s[i + 3]) & 0x3F);
            if (candidate >= 0x10000 && candidate <= 0x10FFFF) {
                cp = candidate;
                width = 4;
            }
        }
        i += width;
        if (cp <= 0xFFFF) {
            append_u16(cp);
        } else {
            cp -= 0x10000;
            append_u16(0xD800 + (cp >> 10));
            append_u16(0xDC00 + (cp & 0x3FF));
        }
    }
    return o + "\"";
}

// Reformat the already insertion-ordered JSON fragments like
// json.dump(..., indent=2), without parsing them through json::Value (whose
// object storage deliberately sorts keys).
std::string python_pretty_json(const std::string& compact) {
    std::string out;
    out.reserve(compact.size() + compact.size() / 2);
    int depth = 0;
    auto indent = [&] { out.append(size_t(depth) * 2, ' '); };
    for (size_t i = 0; i < compact.size(); ++i) {
        const char c = compact[i];
        if (c == '"') {
            out += c;
            for (++i; i < compact.size(); ++i) {
                const char string_char = compact[i];
                out += string_char;
                if (string_char == '\\' && i + 1 < compact.size())
                    out += compact[++i];
                else if (string_char == '"')
                    break;
            }
        } else if (c == '{' || c == '[') {
            size_t next = i + 1;
            while (next < compact.size() &&
                   std::isspace(uint8_t(compact[next]))) ++next;
            const char closing = c == '{' ? '}' : ']';
            if (next < compact.size() && compact[next] == closing) {
                out += c;
                out += closing;
                i = next;
            } else {
                out += c;
                out += '\n';
                ++depth;
                indent();
            }
        } else if (c == '}' || c == ']') {
            out += '\n';
            --depth;
            indent();
            out += c;
        } else if (c == ',') {
            out += ",\n";
            indent();
        } else if (c == ':') {
            out += ": ";
        } else if (!std::isspace(uint8_t(c))) {
            out += c;
        }
    }
    return out;
}

std::string hex8(uint32_t k) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", k);
    return b;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
}

std::string clean_name(std::string n) {   // strip NULs like the Python
    n.erase(std::remove(n.begin(), n.end(), '\0'), n.end());
    return n;
}

template <typename WorkFn, typename CompleteFn>
void run_parallel_indices(const std::vector<size_t>& indices,
                          uint32_t max_workers, WorkFn work,
                          CompleteFn complete) {
    if (indices.empty()) return;
    std::atomic<size_t> next{0};
    std::mutex mutex;
    std::condition_variable ready;
    struct Done {
        size_t index = 0;
        std::exception_ptr error;
    };
    std::deque<Done> completed;
    const size_t thread_count = std::min<size_t>(max_workers, indices.size());
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (size_t worker = 0; worker < thread_count; ++worker) {
        threads.emplace_back([&] {
            for (;;) {
                const size_t position = next.fetch_add(1);
                if (position >= indices.size()) break;
                const size_t index = indices[position];
                std::exception_ptr error;
                try {
                    work(index);
                } catch (...) {
                    error = std::current_exception();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    completed.push_back({index, error});
                }
                ready.notify_one();
            }
        });
    }
    std::exception_ptr callback_error;
    for (size_t count = 0; count < indices.size(); ++count) {
        Done item;
        {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [&] { return !completed.empty(); });
            item = std::move(completed.front());
            completed.pop_front();
        }
        if (!callback_error) {
            try {
                complete(item.index, item.error);
            } catch (...) {
                callback_error = std::current_exception();
            }
        }
    }
    for (std::thread& thread : threads) thread.join();
    if (callback_error) std::rethrow_exception(callback_error);
}

std::string exception_text(const std::exception_ptr& error) {
    if (!error) return {};
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}

std::string elapsed_text(std::chrono::steady_clock::time_point start) {
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", seconds);
    return buffer;
}

}  // namespace

uint32_t recommended_workers(uint32_t cpu_count) {
    if (cpu_count == 0) cpu_count = std::thread::hardware_concurrency();
    if (cpu_count == 0) cpu_count = 1;
    return std::min<uint32_t>(cpu_count, 8);
}

std::map<uint32_t, std::string> get_worker_presets(uint32_t cpu_count) {
    if (cpu_count == 0) cpu_count = std::thread::hardware_concurrency();
    if (cpu_count == 0) cpu_count = 1;
    static const std::pair<uint32_t, const char*> kPresets[] = {
        {1,  "~200 MB RAM, minimal CPU \xe2\x80\x94 safest, slowest"},
        {2,  "~400 MB RAM, 2 cores \xe2\x80\x94 light load"},
        {4,  "~800 MB RAM, 4 cores \xe2\x80\x94 balanced"},
        {8,  "~1.5 GB RAM, 8 cores \xe2\x80\x94 fast, recommended for 16 GB+ systems"},
        {12, "~2.2 GB RAM, 12 cores \xe2\x80\x94 aggressive, for 32 GB+ systems"},
        {16, "~3 GB RAM, 16 cores \xe2\x80\x94 maximum throughput, 32 GB+ RAM required"},
    };
    std::map<uint32_t, std::string> out;
    uint32_t recommended = recommended_workers(cpu_count);
    for (const auto& preset : kPresets) {
        if (preset.first > cpu_count) continue;
        std::string description = preset.second;
        if (preset.first == recommended) description += " [recommended]";
        out.emplace(preset.first, std::move(description));
    }
    return out;
}

std::vector<CharacterGroup> detect_character_groups(const BigFile& bf) {
    std::vector<CharacterGroup> groups;
    std::unordered_map<std::string, size_t> by_base;
    auto group_for = [&](const std::string& base) -> CharacterGroup& {
        auto it = by_base.find(base);
        if (it != by_base.end()) return groups[it->second];
        size_t index = groups.size();
        by_base.emplace(base, index);
        groups.push_back(CharacterGroup{});
        groups.back().base_name = base;
        return groups.back();
    };

    for (const auto& kv : bf.files) {
        const BFFile& fi = kv.second;
        if (fi.name.empty() || fi.key == INVALID_INDEX) continue;
        std::string lower = fi.name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        size_t pos = lower.find("_wow");
        if (pos != std::string::npos) {
            CharacterGroup& group = group_for(fi.name.substr(0, pos));
            group.has_skeleton = true;
            group.skeleton = {fi.index, fi.key, fi.name};  // last wins
            continue;  // Python uses elif for Costume detection
        }
        pos = lower.find("costume");
        if (pos != std::string::npos) {
            CharacterGroup& group = group_for(fi.name.substr(0, pos));
            group.costumes.push_back({fi.index, fi.key, fi.name});
        }
    }

    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [](const CharacterGroup& group) {
                                    return !group.has_skeleton;
                                }),
                 groups.end());
    return groups;
}

std::string sanitize_name(const std::string& name) {
    // strip + drop NULs, keep [alnum -_. ] (ASCII; the replacement chars a
    // non-ASCII byte decodes to are not alnum in Python either).
    std::string s = clean_name(name);
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
    std::string safe;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 128 && (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == ' '))
            safe += c;
    }
    b = safe.find_first_not_of(' ');
    e = safe.find_last_not_of(' ');
    safe = b == std::string::npos ? "" : safe.substr(b, e - b + 1);
    size_t p;
    while ((p = safe.find("  ")) != std::string::npos) safe.erase(p, 1);
    for (char& c : safe)
        if (c == ' ') c = '_';
    if (safe.size() > 80) safe.resize(80);
    return safe;
}

std::string infer_entry_name(const std::vector<SubEntry>& subs) {
    std::unordered_set<uint32_t> all_gao_keys;
    for (const SubEntry& s : subs)
        if (s.ext == ".gao") all_gao_keys.insert(s.key);

    std::vector<std::string> gao_names;
    std::string root_gao_name;
    bool have_geo = false;
    uint32_t geo_key = 0;
    for (const SubEntry& s : subs) {
        if (s.ext == ".gao") {
            GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
            if (gi.ok && !gi.name.empty()) {
                std::string cleanv = sanitize_name(gi.name);
                if (!cleanv.empty()) {
                    gao_names.push_back(cleanv);
                    uint32_t fk = gi.hier_read ? gi.father_key : INVALID_KEY;
                    bool has_visual = gi.vis_read;
                    if (has_visual || fk == INVALID_KEY || !all_gao_keys.count(fk))
                        root_gao_name = cleanv;
                }
            }
        }
        if (!s.gro_null && s.gro_type == 1 && !have_geo) {
            have_geo = true;
            geo_key = s.key;
        }
    }
    if (!root_gao_name.empty()) return root_gao_name;
    if (!gao_names.empty()) return gao_names.front();
    if (have_geo) return "mesh_" + hex8(geo_key);
    return "";
}

std::string name_sub_entry(const SubEntry& sub, const std::vector<SubEntry>& subs) {
    uint32_t key = sub.key;
    const std::vector<uint8_t>& payload = sub.data;

    if (sub.ext == ".gao") {
        GaoInfo gi = parse_gao_full(payload.data(), payload.size());
        if (gi.ok && !gi.name.empty()) {
            std::string cleanv = sanitize_name(gi.name);
            if (!cleanv.empty()) return cleanv;
        }
        return "gao_" + hex8(key);
    }

    if (!payload.empty() && is_texture_entry(payload.data(), payload.size())) {
        TexInfo ti = parse_texture(payload.data(), payload.size());
        if (ti.valid) {
            const char* fmt = ti.format == 0 ? "bgra" : ti.format == 1 ? "pal8"
                              : ti.format == 5 ? "dxt1" : ti.format == 6 ? "dxt3"
                              : ti.format == 7 ? "dxt5" : ti.format == 11 ? "4bpp"
                                                        : "unk";
            return "tex_" + std::to_string(ti.width) + "x" +
                   std::to_string(ti.height) + "_" + fmt + "_" + hex8(key);
        }
        return "tex_" + hex8(key);
    }

    uint32_t gt1 = 1;
    if (!sub.gro_null && sub.gro_type == 1 && !payload.empty() &&
        is_geometry_entry(payload.data(), payload.size(), &gt1)) {
        GeoInfo geo = parse_geometry(payload.data(), payload.size());
        // Parent GAO whose visual references this GEO (directly or via group).
        std::string parent_name;
        std::unordered_map<uint32_t, const SubEntry*> subs_by_key;
        std::unordered_set<uint32_t> geo_keys_all;
        for (const SubEntry& s : subs) {
            subs_by_key[s.key] = &s;
            if (!s.gro_null && s.gro_type == 1) geo_keys_all.insert(s.key);
        }
        for (const SubEntry& s : subs) {
            if (s.ext != ".gao") continue;
            GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
            if (!gi.ok || !gi.vis_read) continue;
            uint32_t gro = gi.gro_key;
            bool match = gro == key;
            if (!match && gro != 0 && gro != INVALID_KEY) {
                std::vector<uint32_t> members =
                    geo_group_members(gro, subs_by_key, &geo_keys_all);
                match = std::find(members.begin(), members.end(), key) !=
                        members.end();
            }
            if (match) {
                parent_name = sanitize_name(gi.name);
                break;
            }
        }
        std::string skinned = geo.ok && geo.skin_present ? "_skinned" : "";
        std::string vcount = geo.ok ? std::to_string(geo.nb_points) : "?";
        std::string fcount = geo.ok ? std::to_string(geo.faces.size() / 7) : "?";
        if (!parent_name.empty()) return parent_name + "_mesh_" + vcount + "v" + skinned;
        return "mesh_" + vcount + "v_" + fcount + "f" + skinned + "_" + hex8(key);
    }

    // Animation naming inherits the exclusion (no anim files are written, so
    // this label is never used); materials + fallback below.
    if (!sub.gro_null &&
        (sub.gro_type == 3 || sub.gro_type == 4 || sub.gro_type == 5)) {
        MatInfo mi = parse_material(payload.data(), payload.size(), int(sub.gro_type));
        const char* tp = !mi.ok ? "unk"
                         : mi.type == 0 ? "single"
                         : mi.type == 1 ? "multitexture"
                         : mi.type == 2 ? "multi" : "unk";
        return std::string("mat_") + tp + "_" + hex8(key);
    }
    if (!sub.ext.empty()) return "res" + sub.ext + "_" + hex8(key);
    return "res_" + hex8(key);
}

SceneExportResult export_entry_scene(const std::vector<SubEntry>& original_subs,
                                     const std::string& out_path,
                                     const std::string& entry_name,
                                     bool export_textures,
                                     const std::string& scene_type) {
    SceneExportResult result;
    if (original_subs.empty()) return result;
    std::vector<SubEntry> subs = original_subs;

    std::unordered_map<uint32_t, const SubEntry*> by_key;
    std::unordered_set<uint32_t> all_gao_keys, all_geo_keys;
    for (const SubEntry& sub : subs) {
        by_key[sub.key] = &sub;
        if (sub.ext == ".gao") all_gao_keys.insert(sub.key);
        if (!sub.gro_null && sub.gro_type == 1) all_geo_keys.insert(sub.key);
    }
    std::vector<uint32_t> gao_order;
    std::unordered_map<uint32_t, GaoInfo> gao_data;
    for (const SubEntry& sub : subs) {
        if (sub.ext != ".gao") continue;
        GaoInfo info = parse_gao_full(sub.data.data(), sub.data.size());
        if (!info.ok) continue;
        if (!gao_data.count(sub.key)) gao_order.push_back(sub.key);
        gao_data[sub.key] = std::move(info);
    }

    gltfbuild::SceneInput scene;
    struct RawTexture {
        uint32_t key = 0;
        TexInfo info;
        std::vector<uint8_t> rgba;
    };
    std::vector<RawTexture> raw_textures;
    std::unordered_map<uint32_t, int> texture_to_image;
    auto add_texture = [&](const SubEntry& sub) {
        if (sub.data.empty() ||
            !is_texture_entry(sub.data.data(), sub.data.size())) return;
        TexInfo info = parse_texture(sub.data.data(), sub.data.size());
        if (!info.valid) return;
        const std::vector<uint8_t>* palette = palette_for_texture(info, subs);
        std::vector<uint8_t> rgba = decode_texture(
            sub.data.data(), sub.data.size(), info,
            palette ? palette->data() : nullptr, palette ? palette->size() : 0);
        if (rgba.size() != size_t(info.width) * info.height * 4) return;
        texture_to_image[sub.key] = int(scene.images_png.size());
        scene.images_png.push_back(
            encode_png_rgba(rgba.data(), info.width, info.height));
        raw_textures.push_back({sub.key, info, std::move(rgba)});
    };
    for (const SubEntry& sub : subs) add_texture(sub);

    struct RawMaterial { uint32_t key; MatInfo info; };
    std::vector<RawMaterial> raw_materials;
    for (const SubEntry& sub : subs) {
        if (sub.gro_null || (sub.gro_type != 3 && sub.gro_type != 4 &&
                             sub.gro_type != 5)) continue;
        MatInfo info = parse_material(sub.data.data(), sub.data.size(),
                                      int(sub.gro_type));
        if (info.ok) raw_materials.push_back({sub.key, std::move(info)});
    }

    std::vector<RawMaterial> resolved;
    std::vector<std::map<uint32_t, uint32_t>> multi_maps;
    for (const RawMaterial& raw : raw_materials) {
        if (raw.info.type == 2) {
            std::map<uint32_t, uint32_t> index_map;
            for (size_t i = 0; i < raw.info.sub_material_keys.size(); ++i) {
                uint32_t sub_key = raw.info.sub_material_keys[i];
                index_map[uint32_t(i)] = sub_key;
                auto found = by_key.find(sub_key);
                if (found == by_key.end()) continue;
                const SubEntry& sub = *found->second;
                if (sub.gro_null) continue;
                MatInfo info = parse_material(sub.data.data(), sub.data.size(),
                                              int(sub.gro_type));
                if (!info.ok) continue;
                resolved.push_back({sub_key, info});
                for (uint32_t texture_key : info.texture_keys) {
                    if (texture_to_image.count(texture_key)) continue;
                    auto tex = by_key.find(texture_key);
                    if (tex != by_key.end()) add_texture(*tex->second);
                }
            }
            if (!index_map.empty()) multi_maps.push_back(std::move(index_map));
        } else {
            resolved.push_back(raw);
        }
    }
    std::unordered_set<uint32_t> seen_materials;
    std::vector<RawMaterial> deduped;
    for (const RawMaterial& material : resolved)
        if (seen_materials.insert(material.key).second)
            deduped.push_back(material);
    resolved = std::move(deduped);

    std::unordered_map<uint32_t, uint32_t> material_to_index;
    for (size_t i = 0; i < resolved.size(); ++i) {
        const RawMaterial& raw = resolved[i];
        material_to_index[raw.key] = uint32_t(i);
        gltfbuild::SceneMaterial material;
        material.name = "mat_" + hex8(raw.key);
        material.extras_json = "{\"jade_key\":" + jesc(hex8(raw.key)) + "}";
        for (uint32_t texture_key : raw.info.texture_keys) {
            auto found = texture_to_image.find(texture_key);
            if (found != texture_to_image.end()) {
                material.texture_idx = found->second;
                break;
            }
        }
        float opacity_f = 1.0f;
        std::memcpy(&opacity_f, &raw.info.opac_bits, sizeof opacity_f);
        double opacity = opacity_f;
        if (opacity < 0.001) opacity = 1.0;
        uint32_t diffuse = raw.info.diffuse;
        material.base_color = {{
            double((diffuse >> 16) & 0xFF) / 255.0,
            double((diffuse >> 8) & 0xFF) / 255.0,
            double(diffuse & 0xFF) / 255.0,
            opacity,
        }};
        scene.materials.push_back(std::move(material));
    }

    // Direct/group-member GEOs inherit the first owning GAO name.
    std::unordered_map<uint32_t, std::string> geo_owner_name;
    std::unordered_map<uint32_t, uint32_t> geo_owner_gao;
    for (uint32_t gao_key : gao_order) {
        const GaoInfo& info = gao_data[gao_key];
        if (!info.vis_read || info.gro_key == 0 || info.gro_key == INVALID_KEY)
            continue;
        std::string clean = sanitize_name(info.name);
        if (clean.empty()) continue;
        std::vector<uint32_t> members =
            geo_group_members(info.gro_key, by_key, &all_geo_keys);
        bool multi = members.size() > 1;
        for (size_t i = 0; i < members.size(); ++i) {
            if (!geo_owner_name.count(members[i])) {
                geo_owner_name[members[i]] =
                    multi ? clean + "_" + std::to_string(i) : clean;
                geo_owner_gao[members[i]] = gao_key;
            }
        }
    }

    std::vector<uint32_t> mesh_keys;
    for (const SubEntry& sub : subs) {
        uint32_t gt1 = 1;
        if (sub.gro_null || sub.gro_type != 1 || sub.data.empty() ||
            !is_geometry_entry(sub.data.data(), sub.data.size(), &gt1)) continue;
        GeoInfo geo = parse_geometry(sub.data.data(), sub.data.size());
        if (!geo.ok || geo.faces.empty()) continue;
        gltfbuild::SceneMesh mesh;
        auto owner_name = geo_owner_name.find(sub.key);
        mesh.name = owner_name == geo_owner_name.end()
            ? "mesh_" + hex8(sub.key) : owner_name->second;
        mesh.extras_json = "{\"jade_key\":" + jesc(hex8(sub.key)) + "}";
        mesh.geo = std::move(geo);

        const std::map<uint32_t, uint32_t>* best_map = nullptr;
        if (!multi_maps.empty()) {
            std::unordered_set<uint32_t> used;
            for (size_t ei = 0; ei + 1 < mesh.geo.elements.size(); ei += 2)
                used.insert(mesh.geo.elements[ei + 1]);
            int best_score = -1;
            for (const auto& candidate : multi_maps) {
                int score = 0;
                for (uint32_t material_id : used)
                    if (candidate.count(material_id)) ++score;
                if (score > best_score) {
                    best_score = score;
                    best_map = &candidate;
                }
            }
        }
        size_t face_count = mesh.geo.faces.size() / 7;
        mesh.material_indices.reserve(face_count);
        for (size_t fi = 0; fi < face_count; ++fi) {
            uint32_t element = mesh.geo.faces[fi * 7 + 6];
            uint32_t material_id = element * 2 + 1 < mesh.geo.elements.size()
                ? mesh.geo.elements[element * 2 + 1] : 0;
            uint32_t material_key = material_id;
            if (best_map) {
                auto mapped = best_map->find(material_id);
                if (mapped != best_map->end()) material_key = mapped->second;
            }
            auto resolved_index = material_to_index.find(material_key);
            mesh.material_indices.push_back(
                resolved_index == material_to_index.end() ? 0 : resolved_index->second);
        }

        auto owner = geo_owner_gao.find(sub.key);
        if (owner != geo_owner_gao.end() && mesh.geo.skin_present) {
            const GaoInfo& info = gao_data[owner->second];
            for (size_t i = 0; i + 1 < info.gizmo_flat.size(); i += 2)
                mesh.gizmo_gao_keys.push_back(info.gizmo_flat[i]);
        }
        mesh_keys.push_back(sub.key);
        scene.meshes.push_back(std::move(mesh));
    }
    BoneForest forest = build_bone_nodes(subs);
    if (scene.meshes.empty() &&
        (scene_type != "character_bundle" || !forest.ok)) return result;
    std::unordered_map<uint32_t, uint32_t> bone_node_index;
    if (forest.ok) {
        for (size_t i = 0; i < forest.nodes.size(); ++i) {
            const BoneNode& bone = forest.nodes[i];
            gltfbuild::SceneNode node;
            node.name = bone.name;
            node.children = bone.children;
            node.is_bone = true;
            node.jade_key = bone.key;
            node.extras_json = "{\"jade_key\":" + jesc(hex8(bone.key)) +
                               ",\"jade_type\":\"bone\"}";
            if (bone.has_matrix && bone.matrix.size() == 16) {
                node.has_matrix = true;
                for (size_t j = 0; j < 16; ++j) node.matrix[j] = bone.matrix[j];
            }
            bone_node_index[bone.key] = uint32_t(scene.nodes.size());
            scene.nodes.push_back(std::move(node));
        }
        for (size_t i = 0; i < scene.meshes.size(); ++i) {
            gltfbuild::SceneNode node;
            node.name = scene.meshes[i].name;
            node.mesh_idx = int(i);
            node.extras_json = scene.meshes[i].extras_json;
            uint32_t parent_key = INVALID_KEY;
            auto owner = geo_owner_gao.find(mesh_keys[i]);
            if (owner != geo_owner_gao.end()) {
                const GaoInfo& info = gao_data[owner->second];
                parent_key = info.hier_read ? info.father_key : INVALID_KEY;
                if (parent_key == INVALID_KEY || !gao_data.count(parent_key))
                    parent_key = owner->second;
            }
            uint32_t node_index = uint32_t(scene.nodes.size());
            scene.nodes.push_back(std::move(node));
            if (!scene.meshes[i].geo.skin_present && parent_key != INVALID_KEY) {
                auto parent = bone_node_index.find(parent_key);
                if (parent != bone_node_index.end())
                    scene.nodes[parent->second].children.push_back(node_index);
            }
        }
    }

    result.scene_name = !entry_name.empty() ? entry_name : infer_entry_name(subs);
    if (result.scene_name.empty()) result.scene_name = "JadeScene";
    scene.scene_name = result.scene_name;
    scene.extras_json = "{\"jade_source\":" + jesc(result.scene_name) +
        (scene_type.empty() ? "" : ",\"jade_type\":" + jesc(scene_type)) +
        ",\"mesh_count\":" + std::to_string(scene.meshes.size()) +
        ",\"material_count\":" + std::to_string(scene.materials.size()) +
        ",\"texture_count\":" + std::to_string(scene.images_png.size()) +
        ",\"animation_count\":0}";
    std::vector<uint8_t> glb = gltfbuild::build_scene_glb(scene);
    std::filesystem::path output = std::filesystem::u8path(out_path);
    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (glb.empty() || !write_file(output.u8string(), glb)) {
        result.error = "cannot write scene GLB";
        return result;
    }

    if (export_textures && !raw_textures.empty()) {
        std::filesystem::path texture_dir = output.parent_path() / "textures";
        std::filesystem::create_directories(texture_dir, ec);
        auto format_name = [](uint32_t format) -> const char* {
            switch (format) {
                case 0: return "bgra";
                case 1: return "pal8";
                case 5: return "dxt1";
                case 6: return "dxt3";
                case 7: return "dxt5";
                case 11: return "4bpp";
                default: return "unk";
            }
        };
        for (const RawTexture& texture : raw_textures) {
            std::string filename = "tex_" + std::to_string(texture.info.width) +
                "x" + std::to_string(texture.info.height) + "_" +
                format_name(texture.info.format) + "_" + hex8(texture.key) + ".png";
            std::vector<uint8_t> png = encode_png_rgba(
                texture.rgba.data(), texture.info.width, texture.info.height);
            if (write_file((texture_dir / filename).u8string(), png))
                result.texture_files.push_back(filename);
        }
    }
    result.ok = true;
    result.glb_path = out_path;
    result.meshes = uint32_t(scene.meshes.size());
    result.materials = uint32_t(scene.materials.size());
    result.textures = uint32_t(scene.images_png.size());
    result.bones = forest.ok ? uint32_t(forest.nodes.size()) : 0;
    return result;
}

SceneExportResult export_character_bundle(
    const std::vector<SubEntry>& skeleton_subs,
    const std::vector<SubEntry>& costume_subs,
    const std::string& out_path,
    const std::string& bundle_name) {
    if (costume_subs.empty()) {
        if (skeleton_subs.empty()) return {};
        return export_entry_scene(skeleton_subs, out_path, bundle_name, true,
                                  "character_bundle");
    }
    std::vector<SubEntry> merged = costume_subs;
    std::unordered_set<uint32_t> primary_keys;
    for (const SubEntry& sub : costume_subs) primary_keys.insert(sub.key);
    for (const SubEntry& sub : skeleton_subs) {
        if (primary_keys.count(sub.key) || sub.data.empty()) continue;
        if (is_texture_entry(sub.data.data(), sub.data.size()))
            merged.push_back(sub);
    }
    return export_entry_scene(merged, out_path, bundle_name, true,
                              "character_bundle");
}

std::string process_entry(const std::vector<uint8_t>& dec, uint32_t fi_index,
                          uint32_t fi_key, const std::string& fi_name,
                          const std::string& out_dir, bool scene_mode,
                          std::vector<std::string>* logs) {
    if (dec.empty()) return "";
    std::vector<SubEntry> subs = parse_sub_entries(dec);
    if (subs.empty()) return "";

    std::string entry_name = infer_entry_name(subs);
    if (entry_name.empty()) entry_name = "entry_" + hex8(fi_key);

    std::string subs_json;
    bool exported_something = false;
    uint32_t gt1 = 1;
    std::string scene_fields;
    if (scene_mode) {
        bool has_geometry = false;
        for (const SubEntry& sub : subs) {
            if (sub.gro_null || sub.gro_type != 1 || sub.data.empty()) continue;
            if (is_geometry_entry(sub.data.data(), sub.data.size(), &gt1)) {
                has_geometry = true;
                break;
            }
        }
        if (has_geometry) {
            std::string safe = sanitize_name(entry_name);
            if (safe.empty()) safe = "entry_" + hex8(fi_key);
            std::filesystem::path scene_dir =
                std::filesystem::u8path(out_dir) / "scenes";
            std::string filename = safe + ".glb";
            std::filesystem::path scene_path = scene_dir / filename;
            uint32_t counter = 1;
            while (std::filesystem::exists(scene_path)) {
                filename = safe + "_" + std::to_string(counter++) + ".glb";
                scene_path = scene_dir / filename;
            }
            SceneExportResult scene_result =
                export_entry_scene(subs, scene_path.u8string(), entry_name, true);
            if (scene_result.ok) {
                scene_fields = ", \"scene_glb\": " + jesc("scenes/" + filename) +
                    ", \"scene_info\": {\"meshes\": " +
                    std::to_string(scene_result.meshes) +
                    ", \"materials\": " + std::to_string(scene_result.materials) +
                    ", \"textures\": " + std::to_string(scene_result.textures) +
                    ", \"animations\": 0, \"bones\": " +
                    std::to_string(scene_result.bones) + "}";
                exported_something = true;
                if (logs) {
                    logs->push_back(
                        "  SCENE " + hex8(fi_key) + ": " + entry_name + " (" +
                        std::to_string(scene_result.meshes) + "M " +
                        std::to_string(scene_result.textures) + "T " +
                        std::to_string(scene_result.bones) + "B 0A) \xe2\x86\x92 " +
                        filename);
                }
            } else if (logs && !scene_result.error.empty()) {
                logs->push_back("  Scene export failed for " + hex8(fi_key) +
                                ": " + scene_result.error);
            }
        }
    }
    for (const SubEntry& s : subs) {
        const std::vector<uint8_t>& payload = s.data;
        uint32_t gro_type = s.gro_null ? 0 : s.gro_type;
        std::string ext = s.ext;
        if (s.size < 4 && s.offset + 12 <= dec.size()) {
            // Python reads the type field even for header-only (size<4) subs.
            const uint8_t* t = dec.data() + s.offset + 8;
            bool is_ext = t[0] == '.' && std::isalpha(t[1]) && std::isalpha(t[2]) &&
                          std::isalpha(t[3]);
            if (is_ext)
                ext.assign(reinterpret_cast<const char*>(t), 4);
            else
                gro_type = uint32_t(t[0]) | (uint32_t(t[1]) << 8) |
                           (uint32_t(t[2]) << 16) | (uint32_t(t[3]) << 24);
        }
        std::string info = "{\"key\": " + jesc(hex8(s.key)) + ", \"gro_type\": " +
                           (gro_type ? jesc(hex8(gro_type)) : "null") +
                           ", \"ext\": " + jesc(ext) + ", \"size\": " +
                           std::to_string(payload.size()) + ", \"offset\": " +
                           std::to_string(s.offset);
        std::string label = name_sub_entry(s, subs);

        if (!payload.empty() && is_texture_entry(payload.data(), payload.size())) {
            TexInfo ti = parse_texture(payload.data(), payload.size());
            if (ti.valid) {
                const std::vector<uint8_t>* pal = palette_for_texture(ti, subs);
                // The exporter re-encodes to DXT1 (write_dds), NOT the raw path.
                std::vector<uint8_t> rgba = decode_texture(
                    payload.data(), payload.size(), ti,
                    pal ? pal->data() : nullptr, pal ? pal->size() : 0);
                std::vector<uint8_t> dds;
                if (!rgba.empty()) dds = write_dds(rgba.data(), ti.width, ti.height);
                if (!dds.empty() &&
                    write_file(out_dir + "/textures/" + label + ".dds", dds)) {
                    info += ", \"exported_as\": " + jesc("textures/" + label + ".dds") +
                            ", \"tex_format\": " + std::to_string(ti.format) +
                            ", \"tex_width\": " + std::to_string(ti.width) +
                            ", \"tex_height\": " + std::to_string(ti.height) +
                            ", \"tex_mip_count\": " + std::to_string(ti.mip_count) +
                            ", \"tex_prefix_len\": " + std::to_string(ti.prefix.size()) +
                            ", \"tex_pixdata_len\": " +
                            std::to_string(payload.size() - ti.pix_start);
                    exported_something = true;
                }
            }
        } else if (!payload.empty() && !s.gro_null && s.gro_type == 1 &&
                   is_geometry_entry(payload.data(), payload.size(), &gt1)) {
            GeoInfo geo = parse_geometry(payload.data(), payload.size());
            if (geo.ok && !geo.faces.empty()) {
                std::vector<uint8_t> glb = gltfbuild::build_geo_model_glb(
                    geo, label, hex8(s.key), label);
                if (!glb.empty() &&
                    write_file(out_dir + "/models/" + label + ".glb", glb)) {
                    info += ", \"exported_as\": " + jesc("models/" + label + ".glb") +
                            ", \"nb_points\": " + std::to_string(geo.nb_points) +
                            ", \"nb_faces\": " + std::to_string(geo.faces.size() / 7) +
                            ", \"skinned\": " +
                            (geo.skin_present ? "true" : "false");
                    exported_something = true;
                }
            }
        }
        // Animation exports inherit the exclusion (no animations/ files).

        info += "}";
        if (!subs_json.empty()) subs_json += ", ";
        subs_json += info;
    }
    if (!exported_something) return "";

    return "{\"index\": " + std::to_string(fi_index) + ", \"key\": " +
           jesc(hex8(fi_key)) + ", \"name\": " + jesc(fi_name) +
           ", \"entry_name\": " + jesc(entry_name) + ", \"dec_size\": " +
           std::to_string(dec.size()) + ", \"sub_entries\": [" + subs_json +
           "]" + scene_fields + "}";
}

ExportStats export_bigfile(const BigFile& bf, const std::string& bf_path,
                           const std::string& out_dir, bool scene_mode,
                           ExportLogFn log_fn, ExportProgressFn progress_fn,
                           int32_t max_workers) {
    ExportStats st;
    std::error_code ec;
    const std::filesystem::path output_dir = std::filesystem::u8path(out_dir);
    std::filesystem::create_directories(output_dir, ec);
    for (const char* d : {"textures", "models", "scenes", "characters",
                          "animations", "raw"})
        std::filesystem::create_directories(
            output_dir / d, ec);

    std::vector<const BFFile*> active_files;
    for (const auto& kv : bf.files) {
        const BFFile& fi = kv.second;
        if (!fi.name.empty() && fi.key != INVALID_KEY)
            active_files.push_back(&fi);
    }
    const uint32_t total = uint32_t(active_files.size());
    uint32_t cpu_count = std::thread::hardware_concurrency();
    if (cpu_count == 0) cpu_count = 1;
    uint32_t workers = max_workers < 0
        ? recommended_workers(cpu_count)
        : uint32_t(std::max<int32_t>(1, max_workers));
    workers = std::min(workers, cpu_count);

    auto log = [&](const std::string& line) {
        if (log_fn) log_fn(line);
    };
    auto progress = [&](uint32_t current) {
        if (progress_fn) progress_fn(current, total);
    };
    log("Exporting " + std::to_string(total) + " entries with " +
        std::to_string(workers) + " workers\xe2\x80\xa6");
    const auto start = std::chrono::steady_clock::now();

    // Phase 1: read compressed bytes, then decompress with bounded workers.
    log("Phase 1: Decompressing\xe2\x80\xa6");
    std::vector<std::vector<uint8_t>> raw(total), decompressed(total);
    for (size_t i = 0; i < active_files.size(); ++i)
        raw[i] = bf.read_data(active_files[i]->index);
    if (workers > 1) {
        std::vector<size_t> pending;
        std::vector<bool> has_result(total, false);
        std::vector<uint8_t> decode_ok(total, 0);
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i].empty()) has_result[i] = true;
            else pending.push_back(i);
        }
        run_parallel_indices(
            pending, workers,
            [&](size_t i) {
                LzoResult result = decompress_lzo(raw[i]);
                if (result.ok) {
                    decompressed[i] = std::move(result.data);
                    decode_ok[i] = 1;
                }
            },
            [&](size_t i, const std::exception_ptr& error) {
                if (error) {
                    log("  Decompress error on entry " + std::to_string(i) +
                        ": " + exception_text(error));
                    // Python stores b"" for a failed future, so the slot is
                    // still included in its phase-1 completed-result count.
                    has_result[i] = true;
                } else if (decode_ok[i]) {
                    has_result[i] = true;
                }
                progress(uint32_t(std::count(has_result.begin(),
                                             has_result.end(), true)));
            });
    } else {
        for (size_t i = 0; i < raw.size(); ++i) {
            try {
                if (!raw[i].empty()) {
                    LzoResult result = decompress_lzo(raw[i]);
                    if (result.ok) decompressed[i] = std::move(result.data);
                }
            } catch (const std::exception& e) {
                log("  Decompress error on entry " + std::to_string(i) +
                    ": " + e.what());
            }
            progress(uint32_t(i + 1));
        }
    }
    log("Phase 1 done in " + elapsed_text(start) + "s");

    // Phase 2: parse and export. Parallel completion order intentionally
    // drives callbacks and manifest entry order, like as_completed in Python.
    log("Phase 2: Parsing and exporting with " + std::to_string(workers) +
        " workers\xe2\x80\xa6");
    const auto phase2_start = std::chrono::steady_clock::now();
    struct EntryResult {
        std::string info;
        std::vector<std::string> logs;
    };
    std::vector<EntryResult> results(total);
    std::string entries;
    auto accept_entry = [&](size_t i) {
        for (const std::string& line : results[i].logs) log(line);
        if (!results[i].info.empty()) {
            if (!entries.empty()) entries += ", ";
            entries += results[i].info;
            ++st.entries;
        }
    };
    if (workers > 1) {
        std::vector<size_t> pending;
        for (size_t i = 0; i < decompressed.size(); ++i)
            if (!decompressed[i].empty()) pending.push_back(i);
        uint32_t done_count = 0;
        run_parallel_indices(
            pending, workers,
            [&](size_t i) {
                const BFFile& fi = *active_files[i];
                results[i].info = process_entry(
                    decompressed[i], fi.index, fi.key, fi.name, out_dir,
                    scene_mode, &results[i].logs);
            },
            [&](size_t i, const std::exception_ptr& error) {
                progress(++done_count);
                if (error) {
                    log("  Error on entry " + std::to_string(i) + ": " +
                        exception_text(error));
                } else {
                    accept_entry(i);
                }
            });
    } else {
        for (size_t i = 0; i < decompressed.size(); ++i) {
            progress(uint32_t(i + 1));
            if (decompressed[i].empty()) continue;
            const BFFile& fi = *active_files[i];
            try {
                results[i].info = process_entry(
                    decompressed[i], fi.index, fi.key, fi.name, out_dir,
                    scene_mode, &results[i].logs);
                accept_entry(i);
            } catch (const std::exception& e) {
                log("  Error on " + hex8(fi.key) + ": " + e.what());
            }
        }
    }
    decompressed.clear();
    decompressed.shrink_to_fit();
    log("Phase 2 done in " + elapsed_text(phase2_start) + "s");

    auto scene_result_json = [&](const SceneExportResult& result) {
        std::string texture_files;
        for (const std::string& name : result.texture_files) {
            if (!texture_files.empty()) texture_files += ", ";
            texture_files += jesc(name);
        }
        return "{\"glb_path\": " + jesc(result.glb_path) +
            ", \"scene_name\": " + jesc(result.scene_name) +
            ", \"meshes\": " + std::to_string(result.meshes) +
            ", \"materials\": " + std::to_string(result.materials) +
            ", \"textures\": " + std::to_string(result.textures) +
            ", \"animations\": 0, \"bones\": " +
            std::to_string(result.bones) + ", \"texture_files\": [" +
            texture_files + "]}";
    };

    // Phase 3: character bundles remain sequential in Python and native.
    log("Phase 3: Detecting character groups\xe2\x80\xa6");
    const std::vector<CharacterGroup> groups = detect_character_groups(bf);
    if (groups.empty()) {
        log("No character groups detected");
    } else {
        std::string names;
        for (const CharacterGroup& group : groups) {
            if (!names.empty()) names += ", ";
            names += group.base_name;
        }
        log("Found " + std::to_string(groups.size()) +
            " character group(s): " + names);
    }
    std::string character_bundles;
    const std::filesystem::path character_dir =
        output_dir / "characters";
    for (const CharacterGroup& group : groups) {
        try {
            std::vector<SubEntry> skeleton_subs;
            if (group.has_skeleton) {
                const std::vector<uint8_t> raw_skeleton =
                    bf.read_data(group.skeleton.index);
                if (!raw_skeleton.empty()) {
                    LzoResult decoded = decompress_lzo(raw_skeleton);
                    if (decoded.ok) {
                        skeleton_subs = parse_sub_entries(decoded.data);
                        log("  Skeleton: " + group.skeleton.name + " (" +
                            std::to_string(skeleton_subs.size()) +
                            " sub-entries)");
                    }
                }
            }
            std::vector<std::string> bundle_files;
            if (group.costumes.empty()) {
                if (!skeleton_subs.empty()) {
                    const std::string bundle_name =
                        group.base_name + "_skeleton";
                    const std::filesystem::path output = character_dir /
                        (sanitize_name(bundle_name) + ".glb");
                    SceneExportResult exported = export_character_bundle(
                        skeleton_subs, {}, output.u8string(), bundle_name);
                    if (exported.ok)
                        bundle_files.push_back(scene_result_json(exported));
                }
            } else {
                for (const CharacterEntry& costume : group.costumes) {
                    const std::vector<uint8_t> raw_costume =
                        bf.read_data(costume.index);
                    if (raw_costume.empty()) continue;
                    LzoResult decoded = decompress_lzo(raw_costume);
                    if (!decoded.ok) continue;
                    std::vector<SubEntry> costume_subs =
                        parse_sub_entries(decoded.data);
                    log("  Costume: " + costume.name + " (" +
                        std::to_string(costume_subs.size()) +
                        " sub-entries)");
                    std::string bundle_name = sanitize_name(costume.name);
                    if (bundle_name.empty())
                        bundle_name = group.base_name + "_costume";
                    std::filesystem::path output = character_dir /
                        (bundle_name + ".glb");
                    uint32_t counter = 1;
                    while (std::filesystem::exists(output)) {
                        output = character_dir /
                            (bundle_name + "_" +
                             std::to_string(counter++) + ".glb");
                    }
                    SceneExportResult exported = export_character_bundle(
                        skeleton_subs, costume_subs, output.u8string(),
                        bundle_name);
                    if (exported.ok) {
                        bundle_files.push_back(scene_result_json(exported));
                        log("  \xe2\x86\x92 " + output.filename().u8string() + " (" +
                            std::to_string(exported.meshes) + "M " +
                            std::to_string(exported.textures) + "T " +
                            std::to_string(exported.bones) + "B 0A)");
                    }
                }
            }
            if (bundle_files.empty()) continue;
            std::string costumes;
            for (const CharacterEntry& costume : group.costumes) {
                if (!costumes.empty()) costumes += ", ";
                costumes += jesc(costume.name);
            }
            std::string files;
            for (const std::string& file : bundle_files) {
                if (!files.empty()) files += ", ";
                files += file;
            }
            const std::string bundle =
                "{\"base_name\": " + jesc(group.base_name) +
                ", \"skeleton_entry\": " + jesc(group.skeleton.name) +
                ", \"costume_entries\": [" + costumes +
                "], \"bundle_files\": [" + files + "]}";
            if (!character_bundles.empty()) character_bundles += ", ";
            character_bundles += bundle;
            ++st.character_bundles;
        } catch (const std::exception& e) {
            log("  Character group '" + group.base_name + "' failed: " +
                e.what());
        }
    }

    log("Export complete in " + elapsed_text(start) + "s: " +
        std::to_string(st.entries) + " entries, " +
        std::to_string(st.character_bundles) + " character bundles");
    const std::string manifest =
        "{\"bf_path\": " + jesc(bf_path) +
        ", \"version\": 3, \"scene_mode\": " +
        (scene_mode ? "true" : "false") + ", \"entries\": [" + entries +
        "], \"character_bundles\": [" + character_bundles + "]}";
    const std::filesystem::path manifest_path =
        output_dir / "manifest.json";
    // Python opens this as text; on Windows its indent newlines are CRLF.
    std::ofstream f(manifest_path);
    if (!f) {
        st.error = "cannot write manifest";
        return st;
    }
    f << python_pretty_json(manifest);
    if (!f) {
        st.error = "cannot write manifest";
        return st;
    }
    st.ok = true;
    log("Manifest written: " + manifest_path.u8string());
    return st;
}

// importer.scan_changes + _categorize.
namespace {
std::string categorize_path(const std::string& p) {
    if (p.rfind("textures/", 0) == 0) return "texture";
    if (p.rfind("models/", 0) == 0) return "model";
    if (p.rfind("scenes/", 0) == 0) return "scene";
    if (p.rfind("animations/", 0) == 0) return "animation";
    return "raw";
}
uint32_t hex_key(const json::Value* v) {
    if (v == nullptr) return 0;
    if (v->is_num()) return uint32_t(v->num);
    if (v->is_str()) return uint32_t(std::strtoul(v->str.c_str(), nullptr, 16));
    return 0;
}
bool file_exists(const std::string& p) {
    std::ifstream f(std::filesystem::u8path(p), std::ios::binary);
    return bool(f);
}
}  // namespace

std::vector<ChangeRec> scan_changes(const std::string& out_dir,
                                    std::vector<std::string>& log) {
    std::vector<ChangeRec> changes;
    const std::filesystem::path output_dir =
        std::filesystem::u8path(out_dir);
    const std::filesystem::path manifest_path = output_dir / "manifest.json";
    std::ifstream mf(manifest_path, std::ios::binary);
    if (!mf) {
        log.push_back("No manifest.json found \xe2\x80\x94 run Unpack first.");
        return changes;
    }
    std::string text((std::istreambuf_iterator<char>(mf)),
                     std::istreambuf_iterator<char>());
    json::Value manifest;
    try { manifest = json::parse(text); } catch (...) { return changes; }

    const json::Value* entries = manifest.find("entries");
    if (entries != nullptr && entries->is_arr())
        for (const json::Value& entry : entries->arr) {
            uint32_t ekey = hex_key(entry.find("key"));
            uint32_t eidx = 0;
            const json::Value* iv = entry.find("index");
            if (iv != nullptr && iv->is_num()) eidx = uint32_t(iv->num);

            const json::Value* sg = entry.find("scene_glb");
            if (sg != nullptr && sg->is_str() && !sg->str.empty()) {
                std::string full =
                    (output_dir / std::filesystem::u8path(sg->str)).u8string();
                if (file_exists(full)) {
                    ChangeRec c;
                    c.path = sg->str;
                    c.full_path = full;
                    c.key = ekey;
                    c.entry_index = eidx;
                    c.entry_key = ekey;
                    c.category = "scene";
                    const json::Value* en = entry.find("entry_name");
                    if (en != nullptr && en->is_str()) c.entry_name = en->str;
                    const json::Value* si = entry.find("scene_info");
                    c.sub_info_json = si != nullptr ? json::dump(*si) : "{}";
                    changes.push_back(c);
                }
            }
            const json::Value* subsv = entry.find("sub_entries");
            if (subsv != nullptr && subsv->is_arr())
                for (const json::Value& sub : subsv->arr) {
                    const json::Value* ea = sub.find("exported_as");
                    if (ea == nullptr || !ea->is_str() || ea->str.empty())
                        continue;
                    std::string full =
                        (output_dir / std::filesystem::u8path(ea->str))
                            .u8string();
                    if (!file_exists(full)) continue;
                    ChangeRec c;
                    c.path = ea->str;
                    c.full_path = full;
                    c.key = hex_key(sub.find("key"));
                    c.entry_index = eidx;
                    c.entry_key = ekey;
                    c.category = categorize_path(ea->str);
                    c.sub_info_json = json::dump(sub);
                    changes.push_back(c);
                }
        }
    char b[64];
    std::snprintf(b, sizeof b, "Found %zu exported file(s)", changes.size());
    log.push_back(b);
    return changes;
}

}  // namespace exporter
}  // namespace jade
