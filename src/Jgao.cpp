#include "jade/Jgao.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/Gltf.hpp"
#include "jade/LevelBlender.hpp"
#include "jade/Rli.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

namespace jade {
namespace jgao {
namespace {

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

bool take(const uint8_t* data, size_t size, size_t& off, size_t count,
          std::vector<uint8_t>& out) {
    if (off > size || count > size - off) return false;
    out.assign(data + off, data + off + count);
    off += count;
    return true;
}

uint32_t original_vb_magic(const std::vector<uint8_t>& geo) {
    CookedVbSection sec = cooked_vb_section(geo.data(), geo.size());
    if (!sec.ok || sec.data_start < 12) return 2;
    uint32_t magic = le32(geo.data() + sec.data_start - 12);
    return (magic == 2 || magic == 5 || magic == 6 || magic == 8) ? magic : 2;
}

void append_slice(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src,
                  size_t begin, size_t end) {
    begin = std::min(begin, src.size());
    end = std::min(end, src.size());
    if (end > begin) dst.insert(dst.end(), src.begin() + static_cast<long>(begin),
                                src.begin() + static_cast<long>(end));
}

}  // namespace

File parse(const uint8_t* data, size_t size) {
    File f;
    auto fail = [&](const std::string& message) {
        f.error = message;
        return f;
    };
    if (size < 36) return fail("Not a valid .jgao file: truncated header");
    if (data[0] != 'J' || data[1] != 'G' || data[2] != 'A' || data[3] != 'O')
        return fail("Not a valid .jgao file: bad magic");

    f.version = le32(data + 4);
    if (f.version > VERSION)
        return fail("Unsupported .jgao version " + std::to_string(f.version));
    f.gao_key = le32(data + 8);
    f.geo_key = le32(data + 12);
    f.mat_key = le32(data + 16);
    uint32_t gao_size = le32(data + 20);
    uint32_t geo_size = le32(data + 24);
    uint32_t mat_size = le32(data + 28);
    uint32_t children_count = le32(data + 32);

    size_t off = 36;
    if (!take(data, size, off, gao_size, f.gao_data) ||
        !take(data, size, off, geo_size, f.geo_data) ||
        !take(data, size, off, mat_size, f.mat_data))
        return fail("Not a valid .jgao file: truncated payload");

    f.mat_children.reserve(children_count);
    for (uint32_t i = 0; i < children_count; ++i) {
        if (off > size || size - off < 12)
            return fail("Not a valid .jgao file: truncated material child header");
        MaterialChild child;
        child.key = le32(data + off);
        child.gro_type = le32(data + off + 4);
        uint32_t child_size = le32(data + off + 8);
        off += 12;
        if (!take(data, size, off, child_size, child.data))
            return fail("Not a valid .jgao file: truncated material child");
        f.mat_children.push_back(std::move(child));
    }
    f.ok = true;
    return f;
}

std::vector<uint8_t> serialize(const File& f) {
    auto fits32 = [](size_t n) { return n <= std::numeric_limits<uint32_t>::max(); };
    if (!fits32(f.gao_data.size()) || !fits32(f.geo_data.size()) ||
        !fits32(f.mat_data.size()) || !fits32(f.mat_children.size()))
        throw std::runtime_error(".jgao payload is too large");
    size_t total = 36 + f.gao_data.size() + f.geo_data.size() + f.mat_data.size();
    for (const MaterialChild& child : f.mat_children) {
        if (!fits32(child.data.size()) || child.data.size() > std::numeric_limits<size_t>::max() - 12 - total)
            throw std::runtime_error(".jgao material child is too large");
        total += 12 + child.data.size();
    }

    std::vector<uint8_t> out;
    out.reserve(total);
    out.insert(out.end(), {'J', 'G', 'A', 'O'});
    put32(out, f.version);
    put32(out, f.gao_key);
    put32(out, f.geo_key);
    put32(out, f.mat_key);
    put32(out, static_cast<uint32_t>(f.gao_data.size()));
    put32(out, static_cast<uint32_t>(f.geo_data.size()));
    put32(out, static_cast<uint32_t>(f.mat_data.size()));
    put32(out, static_cast<uint32_t>(f.mat_children.size()));
    out.insert(out.end(), f.gao_data.begin(), f.gao_data.end());
    out.insert(out.end(), f.geo_data.begin(), f.geo_data.end());
    out.insert(out.end(), f.mat_data.begin(), f.mat_data.end());
    for (const MaterialChild& child : f.mat_children) {
        put32(out, child.key);
        put32(out, child.gro_type);
        put32(out, static_cast<uint32_t>(child.data.size()));
        out.insert(out.end(), child.data.begin(), child.data.end());
    }
    return out;
}

ExportResult export_gao(const std::vector<SubEntry>& subs, uint32_t gao_key) {
    ExportResult result;
    auto fail = [&](const std::string& message) {
        result.error = message;
        return result;
    };
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& sub : subs) by_key[sub.key] = &sub;
    auto git = by_key.find(gao_key);
    if (git == by_key.end() || git->second->ext != ".gao") {
        char message[96];
        std::snprintf(message, sizeof message, "GAO 0x%08X not found in entry", gao_key);
        return fail(message);
    }
    GaoInfo gao = parse_gao_full(git->second->data.data(), git->second->data.size());
    if (!gao.ok || !gao.vis_read) {
        char message[96];
        std::snprintf(message, sizeof message, "GAO 0x%08X has no visual section", gao_key);
        return fail(message);
    }
    auto geoit = by_key.find(gao.gro_key);
    if (geoit == by_key.end() || geoit->second->gro_null || geoit->second->gro_type != 1) {
        char message[96];
        std::snprintf(message, sizeof message, "GEO 0x%08X not found in entry", gao.gro_key);
        return fail(message);
    }

    File file;
    file.ok = true;
    file.version = VERSION;
    file.gao_key = gao_key;
    file.geo_key = gao.gro_key;
    file.mat_key = gao.grm_key;
    file.gao_data = git->second->data;
    file.geo_data = geoit->second->data;
    if (gao.grm_key != 0xFFFFFFFFu) {
        auto mit = by_key.find(gao.grm_key);
        if (mit != by_key.end()) {
            file.mat_data = mit->second->data;
            if (!mit->second->gro_null && mit->second->gro_type == 4 &&
                mit->second->data.size() >= 8) {
                uint32_t count = le32(mit->second->data.data() + 4);
                if (count <= 100) {
                    for (uint32_t i = 0; i < count; ++i) {
                        size_t off = 8 + static_cast<size_t>(i) * 4;
                        if (off + 4 > mit->second->data.size()) break;
                        uint32_t child_key = le32(mit->second->data.data() + off);
                        auto child = by_key.find(child_key);
                        if (child == by_key.end()) continue;
                        file.mat_children.push_back({
                            child_key,
                            child->second->gro_null ? 5u : child->second->gro_type,
                            child->second->data});
                    }
                }
            }
        }
    }
    try {
        result.jgao = serialize(file);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    result.name = gao.name.empty() ? ("0x" + [&]() {
        char key[9]; std::snprintf(key, sizeof key, "%08X", gao_key); return std::string(key);
    }()) : gao.name;
    result.ok = true;
    return result;
}

void adjust_material_for_elements(std::vector<uint8_t>& mat_data,
                                  std::vector<MaterialChild>& mat_children,
                                  uint32_t n_new, uint32_t n_old) {
    if (mat_data.size() < 8 || n_new == n_old) return;

    std::vector<uint8_t> adjusted;
    adjusted.insert(adjusted.end(), mat_data.begin(), mat_data.begin() + 4);
    put32(adjusted, n_new);
    if (n_new < n_old) {
        for (uint32_t i = 0; i < n_new; ++i) {
            size_t off = 8 + static_cast<size_t>(i) * 4;
            if (off + 4 <= mat_data.size()) append_slice(adjusted, mat_data, off, off + 4);
        }
        if (mat_children.size() > n_new) mat_children.resize(n_new);
    } else {
        for (uint32_t i = 0; i < n_new; ++i) {
            if (i < n_old) {
                size_t off = 8 + static_cast<size_t>(i) * 4;
                if (off + 4 <= mat_data.size())
                    append_slice(adjusted, mat_data, off, off + 4);
                else
                    append_slice(adjusted, mat_data, mat_data.size() - 4, mat_data.size());
            } else {
                // Mirrors mat_data[8 + (n_old-1)*4 : 8 + n_old*4], including
                // Python's n_old==0 slice (bytes 4..8).
                size_t begin = n_old == 0 ? 4 : 8 + static_cast<size_t>(n_old - 1) * 4;
                size_t end = 8 + static_cast<size_t>(n_old) * 4;
                append_slice(adjusted, mat_data, begin, end);
            }
        }
        if (!mat_children.empty()) {
            MaterialChild last = mat_children.back();
            while (mat_children.size() < n_new) mat_children.push_back(last);
        }
    }
    mat_data = std::move(adjusted);
}

ConvertResult glb_to_jgao(const uint8_t* glb_data, size_t glb_size,
                          const uint8_t* template_data, size_t template_size) {
    ConvertResult result;
    auto fail = [&](const std::string& message) {
        result.error = message;
        return result;
    };

    File templ = parse(template_data, template_size);
    if (!templ.ok) return fail(templ.error);
    GeoInfo original = parse_geometry(templ.geo_data.data(), templ.geo_data.size());
    if (!original.ok) return fail("Could not parse template GEO data");
    if (original.ps2) return fail("Could not parse template GEO data");

    gltf::MeshData mesh;
    try {
        mesh = gltf::parse_glb_mesh(glb_data, glb_size);
    } catch (const std::exception& e) {
        return fail(e.what());
    }

    gltf::GeoBuildOpts opts;
    opts.version = original.version;
    opts.flags1 = original.flags1;
    opts.flags2 = original.flags2;
    opts.vb_magic = original_vb_magic(templ.geo_data);
    // Python's top-level converter uses the legacy trailing layout and does
    // not import COLOR_0 here, even when the GLB contains it.
    opts.shipped_skinned = false;

    try {
        templ.geo_data = gltf::build_geo_payload(mesh, opts);
    } catch (const std::exception& e) {
        return fail(e.what());
    }

    uint32_t n_new = static_cast<uint32_t>(mesh.elements.size());
    if (n_new != original.nb_elements && !templ.mat_data.empty())
        adjust_material_for_elements(templ.mat_data, templ.mat_children,
                                     n_new, original.nb_elements);
    templ.version = VERSION;

    try {
        result.jgao = serialize(templ);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    result.vertices = static_cast<uint32_t>(mesh.vertices.size());
    result.faces = static_cast<uint32_t>(mesh.faces.size());
    result.elements = n_new;
    result.geo_size = static_cast<uint32_t>(templ.geo_data.size());
    result.bones = mesh.has_skin ? static_cast<uint32_t>(mesh.skin.bones.size()) : 0;
    result.ok = true;
    return result;
}

namespace {

std::string pyfloat(double value) {
    for (int precision = 1; precision <= 17; ++precision) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*g", precision, value);
        if (std::strtod(buf, nullptr) == value) {
            std::string out(buf);
            if (out.find('.') == std::string::npos && out.find('e') == std::string::npos)
                out += ".0";
            return out;
        }
    }
    return "0.0";
}

std::string jstr(const std::string& value) {
    static const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4]; out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) out += ',';
        out += value;
    }
    return out;
}

std::string doubles_json(const double* values, size_t count) {
    std::string out = "[";
    for (size_t i = 0; i < count; ++i) {
        if (i) out += ',';
        out += pyfloat(values[i]);
    }
    return out + "]";
}

struct FlatBin {
    std::vector<uint8_t> data;
    std::vector<std::string> views;
    std::vector<std::string> accessors;

    int add_view(const uint8_t* bytes, size_t size, int target = 0) {
        while (data.size() % 4) data.push_back(0);
        size_t offset = data.size();
        data.insert(data.end(), bytes, bytes + size);
        std::string view = "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                           ",\"byteLength\":" + std::to_string(size);
        if (target) view += ",\"target\":" + std::to_string(target);
        view += '}';
        views.push_back(std::move(view));
        return static_cast<int>(views.size()) - 1;
    }

    int add_accessor(int view, uint32_t component, size_t count, const char* type,
                     const std::string& min_value = {},
                     const std::string& max_value = {}) {
        std::string acc = "{\"bufferView\":" + std::to_string(view) +
                          ",\"componentType\":" + std::to_string(component) +
                          ",\"count\":" + std::to_string(count) +
                          ",\"type\":" + jstr(type);
        if (!min_value.empty()) acc += ",\"min\":" + min_value;
        if (!max_value.empty()) acc += ",\"max\":" + max_value;
        acc += '}';
        accessors.push_back(std::move(acc));
        return static_cast<int>(accessors.size()) - 1;
    }
};

bool inverse4(const std::array<double, 16>& matrix, std::array<double, 16>& out) {
    double a[4][8];
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            a[row][col] = matrix[static_cast<size_t>(col * 4 + row)];
            a[row][col + 4] = row == col ? 1.0 : 0.0;
        }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 4; ++row)
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) pivot = row;
        if (a[pivot][col] == 0.0) return false;
        if (pivot != col)
            for (int k = 0; k < 8; ++k) std::swap(a[pivot][k], a[col][k]);
        double divisor = a[col][col];
        for (int k = 0; k < 8; ++k) a[col][k] /= divisor;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            double factor = a[row][col];
            for (int k = 0; k < 8; ++k) a[row][k] -= factor * a[col][k];
        }
    }
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            out[static_cast<size_t>(col * 4 + row)] = a[row][col + 4];
    return true;
}

std::array<double, 16> jade_to_gltf_matrix(const std::array<double, 16>& input) {
    static const double s[4][4] = {
        {1, 0, 0, 0}, {0, 0, 1, 0}, {0, -1, 0, 0}, {0, 0, 0, 1}};
    static const double si[4][4] = {
        {1, 0, 0, 0}, {0, 0, -1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}};
    double r[4][4], tmp[4][4], result[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r[i][j] = input[static_cast<size_t>(j * 4 + i)];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double value = 0.0;
            for (int k = 0; k < 4; ++k) value += s[i][k] * r[k][j];
            tmp[i][j] = value;
        }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double value = 0.0;
            for (int k = 0; k < 4; ++k) value += tmp[i][k] * si[k][j];
            result[i][j] = value;
        }
    std::array<double, 16> out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[static_cast<size_t>(j * 4 + i)] = result[i][j];
    return out;
}

std::array<double, 16> identity4() {
    return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
}

struct FlatMaterial {
    std::string name;
    uint32_t key = 0;
    uint32_t element = 0;
    uint32_t mat_id = 0;
    bool placeholder = false;
    int texture = -1;
};

struct ContextTexture {
    const SubEntry* sub = nullptr;
    TexInfo info;
};

std::vector<std::string> resolve_bone_names(const File& file, const GeoInfo& geo,
                                            const std::vector<SubEntry>* context) {
    std::vector<std::string> names;
    GaoInfo gao = parse_gao_full(file.gao_data.data(), file.gao_data.size());
    if (!gao.ok) {
        for (size_t i = 0; i < geo.skin_bones.size(); ++i) {
            char name[32]; std::snprintf(name, sizeof name, "bone_%03zu", i);
            names.emplace_back(name);
        }
        return names;
    }
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    if (context)
        for (const SubEntry& sub : *context) by_key[sub.key] = &sub;
    for (const GeoBone& bone : geo.skin_bones) {
        std::string resolved;
        size_t pair = static_cast<size_t>(bone.bone_idx) * 2;
        if (pair < gao.gizmo_flat.size()) {
            uint32_t key = gao.gizmo_flat[pair];
            auto it = by_key.find(key);
            if (it != by_key.end() && it->second->ext == ".gao") {
                GaoInfo bg = parse_gao_full(it->second->data.data(), it->second->data.size());
                if (bg.ok && !bg.name.empty()) {
                    resolved = bg.name;
                    resolved.erase(std::remove(resolved.begin(), resolved.end(), '\0'), resolved.end());
                    size_t a = resolved.find_first_not_of(" \t\r\n");
                    size_t b = resolved.find_last_not_of(" \t\r\n");
                    resolved = a == std::string::npos ? "" : resolved.substr(a, b - a + 1);
                    if (resolved.size() >= 4 && resolved.substr(resolved.size() - 4) == ".gao")
                        resolved.resize(resolved.size() - 4);
                }
            }
        }
        if (resolved.empty()) {
            char name[32];
            std::snprintf(name, sizeof name, "bone_%03u", uint32_t(bone.bone_idx));
            resolved = name;
        }
        names.push_back(std::move(resolved));
    }
    return names;
}

}  // namespace

GlbResult jgao_to_glb(const uint8_t* jgao_data, size_t jgao_size,
                      const std::vector<SubEntry>* context) {
    GlbResult result;
    auto fail = [&](const std::string& message) {
        result.error = message;
        return result;
    };
    File file = parse(jgao_data, jgao_size);
    if (!file.ok) return fail(file.error);
    GeoInfo geo = parse_geometry(file.geo_data.data(), file.geo_data.size());
    if (!geo.ok || geo.ps2) return fail("Could not parse GEO data from JGAO");
    size_t original_vertices = geo.vertices.size() / 3;
    size_t face_count = geo.faces.size() / 7;
    if (original_vertices == 0 || face_count == 0)
        return fail("Could not build GLB from empty GEO data");

    // Resolve optional material textures from the same BF-entry context.
    std::map<uint32_t, ContextTexture> textures_by_key;
    if (context) {
        for (const SubEntry& sub : *context) {
            if (!is_texture_entry(sub.data.data(), sub.data.size())) continue;
            TexInfo ti = parse_texture(sub.data.data(), sub.data.size());
            if (ti.valid) textures_by_key[sub.key] = ContextTexture{&sub, ti};
        }
    }
    std::map<uint32_t, int> image_by_key;
    std::vector<std::vector<uint8_t>> png_images;
    std::vector<FlatMaterial> materials;
    for (size_t ci = 0; ci < file.mat_children.size(); ++ci) {
        const MaterialChild& child = file.mat_children[ci];
        FlatMaterial material;
        char name[64];
        std::snprintf(name, sizeof name, "mat_%zu_0x%08X", ci, child.key);
        material.name = name;
        material.key = child.key;
        material.element = static_cast<uint32_t>(ci);
        uint32_t texture_key = 0;
        for (size_t off = 0; off + 4 <= child.data.size(); ++off) {
            uint32_t candidate = le32(child.data.data() + off);
            if (textures_by_key.find(candidate) != textures_by_key.end()) {
                texture_key = candidate;
                break;
            }
        }
        if (texture_key != 0) {
            auto cached = image_by_key.find(texture_key);
            if (cached != image_by_key.end()) {
                material.texture = cached->second;
            } else {
                const ContextTexture& tx = textures_by_key[texture_key];
                const std::vector<uint8_t>* palette =
                    context ? palette_for_texture(tx.info, *context) : nullptr;
                std::vector<uint8_t> rgba = decode_texture(
                    tx.sub->data.data(), tx.sub->data.size(), tx.info,
                    palette ? palette->data() : nullptr, palette ? palette->size() : 0);
                if (rgba.size() == static_cast<size_t>(tx.info.width) * tx.info.height * 4) {
                    std::vector<uint8_t> png = levelblend::png_encode_rgba_pub(
                        rgba.data(), tx.info.width, tx.info.height);
                    material.texture = static_cast<int>(png_images.size());
                    image_by_key[texture_key] = material.texture;
                    png_images.push_back(std::move(png));
                }
            }
        }
        materials.push_back(std::move(material));
    }

    struct V3 { float x, y, z; };
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> combo;
    std::vector<V3> vertices, normals;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<float, 4>> colors;
    std::vector<uint32_t> original_indices, face_elements;
    std::vector<std::array<uint32_t, 3>> faces;
    bool have_colors = !geo.colors.empty();
    size_t uv_count = geo.uvs.size() / 2;
    for (size_t fi = 0; fi < face_count; ++fi) {
        std::array<uint32_t, 3> split_face{};
        for (int corner = 0; corner < 3; ++corner) {
            uint32_t vi = geo.faces[fi * 7 + static_cast<size_t>(corner)];
            uint32_t ui = geo.faces[fi * 7 + 3 + static_cast<size_t>(corner)];
            auto key = std::make_pair(vi, ui);
            auto found = combo.find(key);
            uint32_t split_index;
            if (found == combo.end()) {
                split_index = static_cast<uint32_t>(vertices.size());
                combo[key] = split_index;
                if (vi < original_vertices) {
                    vertices.push_back({geo.vertices[vi * 3], geo.vertices[vi * 3 + 1],
                                        geo.vertices[vi * 3 + 2]});
                    if (geo.normals.size() == geo.vertices.size())
                        normals.push_back({geo.normals[vi * 3], geo.normals[vi * 3 + 1],
                                           geo.normals[vi * 3 + 2]});
                    else normals.push_back({0, 0, 1});
                    if (have_colors && vi * 4 + 3 < geo.colors.size())
                        colors.push_back({geo.colors[vi * 4] / 255.0f,
                                          geo.colors[vi * 4 + 1] / 255.0f,
                                          geo.colors[vi * 4 + 2] / 255.0f,
                                          geo.colors[vi * 4 + 3] / 255.0f});
                } else {
                    vertices.push_back({0, 0, 0}); normals.push_back({0, 0, 1});
                    if (have_colors) colors.push_back({1, 1, 1, 1});
                }
                if (ui < uv_count)
                    uvs.push_back({geo.uvs[ui * 2], geo.uvs[ui * 2 + 1]});
                else uvs.push_back({0, 0});
                original_indices.push_back(vi);
            } else split_index = found->second;
            split_face[static_cast<size_t>(corner)] = split_index;
        }
        faces.push_back(split_face);
        face_elements.push_back(geo.faces[fi * 7 + 6]);
    }
    for (V3& vertex : vertices) vertex = {vertex.x, vertex.z, -vertex.y};
    for (V3& normal : normals) normal = {normal.x, normal.z, -normal.y};

    FlatBin bin;
    std::vector<float> packed_positions;
    packed_positions.reserve(vertices.size() * 3);
    float min_pos[3]{}, max_pos[3]{};
    for (size_t i = 0; i < vertices.size(); ++i) {
        float values[3] = {vertices[i].x, vertices[i].y, vertices[i].z};
        for (int k = 0; k < 3; ++k) {
            packed_positions.push_back(values[k]);
            if (i == 0) min_pos[k] = max_pos[k] = values[k];
            else { min_pos[k] = std::min(min_pos[k], values[k]);
                   max_pos[k] = std::max(max_pos[k], values[k]); }
        }
    }
    double min_d[3] = {min_pos[0], min_pos[1], min_pos[2]};
    double max_d[3] = {max_pos[0], max_pos[1], max_pos[2]};
    int position_accessor = bin.add_accessor(
        bin.add_view(reinterpret_cast<const uint8_t*>(packed_positions.data()),
                     packed_positions.size() * 4, 34962),
        5126, vertices.size(), "VEC3", doubles_json(min_d, 3), doubles_json(max_d, 3));

    std::vector<float> packed_normals;
    for (const V3& normal : normals) {
        packed_normals.push_back(normal.x); packed_normals.push_back(normal.y);
        packed_normals.push_back(normal.z);
    }
    int normal_accessor = bin.add_accessor(
        bin.add_view(reinterpret_cast<const uint8_t*>(packed_normals.data()),
                     packed_normals.size() * 4, 34962), 5126, vertices.size(), "VEC3");
    std::vector<float> packed_uvs;
    for (const auto& uv : uvs) { packed_uvs.push_back(uv[0]); packed_uvs.push_back(uv[1]); }
    int uv_accessor = bin.add_accessor(
        bin.add_view(reinterpret_cast<const uint8_t*>(packed_uvs.data()),
                     packed_uvs.size() * 4, 34962), 5126, vertices.size(), "VEC2");
    int color_accessor = -1;
    if (have_colors && colors.size() == vertices.size()) {
        std::vector<float> packed_colors;
        for (const auto& color : colors)
            packed_colors.insert(packed_colors.end(), color.begin(), color.end());
        color_accessor = bin.add_accessor(
            bin.add_view(reinterpret_cast<const uint8_t*>(packed_colors.data()),
                         packed_colors.size() * 4, 34962), 5126, vertices.size(), "VEC4");
    }

    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, double>>> vertex_weights;
    for (size_t bi = 0; bi < geo.skin_bones.size(); ++bi)
        for (const auto& weight : geo.skin_bones[bi].weights)
            vertex_weights[weight.first].push_back(
                {static_cast<uint32_t>(bi), double(weight.second) / 65535.0});
    int joints_accessor = -1, weights_accessor = -1;
    if (!vertex_weights.empty()) {
        std::vector<uint8_t> joints(vertices.size() * 4, 0);
        std::vector<float> weights(vertices.size() * 4, 0.0f);
        for (size_t svi = 0; svi < vertices.size(); ++svi) {
            auto found = vertex_weights.find(original_indices[svi]);
            if (found == vertex_weights.end()) continue;
            auto influences = found->second;
            std::stable_sort(influences.begin(), influences.end(),
                             [](const auto& a, const auto& b) { return a.second > b.second; });
            if (influences.size() > 4) influences.resize(4);
            double total = 0;
            for (const auto& influence : influences) total += influence.second;
            for (size_t ii = 0; ii < influences.size(); ++ii) {
                joints[svi * 4 + ii] = static_cast<uint8_t>(
                    std::min<uint32_t>(influences[ii].first, 255));
                weights[svi * 4 + ii] = total > 0 ? float(influences[ii].second / total) : 0.0f;
            }
        }
        joints_accessor = bin.add_accessor(bin.add_view(joints.data(), joints.size(), 34962),
                                            5121, vertices.size(), "VEC4");
        weights_accessor = bin.add_accessor(
            bin.add_view(reinterpret_cast<const uint8_t*>(weights.data()), weights.size() * 4, 34962),
            5126, vertices.size(), "VEC4");
    }

    std::vector<std::pair<uint32_t, std::vector<size_t>>> face_groups;
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        uint32_t element = fi < face_elements.size() ? face_elements[fi] : 0;
        auto found = std::find_if(face_groups.begin(), face_groups.end(),
                                  [&](const auto& group) { return group.first == element; });
        if (found == face_groups.end()) {
            face_groups.push_back({element, {}});
            found = face_groups.end() - 1;
        }
        found->second.push_back(fi);
    }
    uint32_t max_needed = 0;
    for (const auto& group : face_groups) max_needed = std::max(max_needed, group.first + 1);
    while (materials.size() < max_needed) {
        uint32_t index = static_cast<uint32_t>(materials.size());
        uint32_t mat_id = index * 2 + 1 < geo.elements.size() ? geo.elements[index * 2 + 1] : index;
        FlatMaterial material;
        material.name = "jade_elem_" + std::to_string(index) + "_matId_" + std::to_string(mat_id);
        material.element = index; material.mat_id = mat_id; material.placeholder = true;
        materials.push_back(std::move(material));
    }

    std::vector<std::string> primitives;
    for (const auto& group : face_groups) {
        std::vector<uint32_t> indices;
        for (size_t fi : group.second)
            indices.insert(indices.end(), faces[fi].begin(), faces[fi].end());
        uint32_t max_index = *std::max_element(indices.begin(), indices.end());
        uint32_t component = max_index <= 0xFFFF ? 5123 : 5125;
        std::vector<uint8_t> packed_indices;
        for (uint32_t index : indices) {
            packed_indices.push_back(static_cast<uint8_t>(index));
            packed_indices.push_back(static_cast<uint8_t>(index >> 8));
            if (component == 5125) {
                packed_indices.push_back(static_cast<uint8_t>(index >> 16));
                packed_indices.push_back(static_cast<uint8_t>(index >> 24));
            }
        }
        int index_accessor = bin.add_accessor(
            bin.add_view(packed_indices.data(), packed_indices.size(), 34963),
            component, indices.size(), "SCALAR");
        std::string attrs = "{\"POSITION\":" + std::to_string(position_accessor) +
                            ",\"NORMAL\":" + std::to_string(normal_accessor) +
                            ",\"TEXCOORD_0\":" + std::to_string(uv_accessor);
        if (color_accessor >= 0) attrs += ",\"COLOR_0\":" + std::to_string(color_accessor);
        if (joints_accessor >= 0) attrs += ",\"JOINTS_0\":" + std::to_string(joints_accessor);
        if (weights_accessor >= 0) attrs += ",\"WEIGHTS_0\":" + std::to_string(weights_accessor);
        attrs += '}';
        std::string primitive = "{\"attributes\":" + attrs +
                                ",\"indices\":" + std::to_string(index_accessor);
        if (group.first < geo.elements.size() / 2)
            primitive += ",\"extras\":{\"element_index\":" + std::to_string(group.first) +
                         ",\"matId\":" + std::to_string(geo.elements[group.first * 2 + 1]) + '}';
        if (group.first < materials.size())
            primitive += ",\"material\":" + std::to_string(group.first);
        primitive += '}';
        primitives.push_back(std::move(primitive));
    }

    bool has_skin = geo.skin_present && !geo.skin_bones.empty();
    std::vector<std::string> bone_names = resolve_bone_names(file, geo, context);
    std::vector<std::string> nodes;
    nodes.push_back(has_skin ? "{\"name\":\"mesh\",\"mesh\":0,\"skin\":0}"
                             : "{\"name\":\"mesh\",\"mesh\":0}");
    std::vector<uint32_t> joint_nodes;
    int ibm_accessor = -1;
    uint32_t armature_index = 0;
    if (has_skin) {
        std::vector<float> ibms;
        for (size_t bi = 0; bi < geo.skin_bones.size(); ++bi) {
            const GeoBone& bone = geo.skin_bones[bi];
            std::array<double, 16> bind{};
            for (int k = 0; k < 16; ++k) bind[static_cast<size_t>(k)] = bone.bind_matrix[static_cast<size_t>(k)];
            std::array<double, 16> world;
            if (!inverse4(bind, world)) world = identity4();
            world = jade_to_gltf_matrix(world);
            std::string extras_bind = doubles_json(bind.data(), bind.size());
            uint32_t node_index = static_cast<uint32_t>(nodes.size());
            joint_nodes.push_back(node_index);
            std::string node = "{\"name\":" + jstr(bi < bone_names.size() ? bone_names[bi]
                                                                             : "bone_" + std::to_string(bi)) +
                               ",\"extras\":{\"bone_idx\":" + std::to_string(bone.bone_idx) +
                               ",\"bind_matrix\":" + extras_bind +
                               ",\"matrix_type\":" + std::to_string(bone.matrix_type) +
                               "},\"matrix\":" + doubles_json(world.data(), world.size()) + '}';
            nodes.push_back(std::move(node));
            std::array<double, 16> gltf_bind = jade_to_gltf_matrix(bind);
            const double fmax = 3.4028235e+38;
            for (double value : gltf_bind)
                ibms.push_back(static_cast<float>(std::max(-fmax, std::min(fmax, value))));
        }
        armature_index = static_cast<uint32_t>(nodes.size());
        std::string children_json = "[";
        for (size_t i = 0; i < joint_nodes.size(); ++i) {
            if (i) children_json += ',';
            children_json += std::to_string(joint_nodes[i]);
        }
        children_json += ']';
        nodes.push_back("{\"name\":\"Armature\",\"children\":" + children_json + '}');
        ibm_accessor = bin.add_accessor(
            bin.add_view(reinterpret_cast<const uint8_t*>(ibms.data()), ibms.size() * 4),
            5126, geo.skin_bones.size(), "MAT4");
    }

    std::vector<int> image_views;
    for (const auto& png : png_images)
        image_views.push_back(bin.add_view(png.data(), png.size()));

    std::string scene_nodes = "[0";
    if (has_skin) scene_nodes += ',' + std::to_string(armature_index);
    scene_nodes += ']';
    std::string mesh_json = "{\"name\":\"mesh\",\"primitives\":[" +
                            join(primitives) + "]}";
    std::string gltf =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"JadeExplorer_jgao_converter\"},"
        "\"scene\":0,\"scenes\":[{\"name\":\"Scene\",\"nodes\":" + scene_nodes +
        "}],\"nodes\":[" + join(nodes) + "],\"meshes\":[" + mesh_json +
        "],\"accessors\":[" + join(bin.accessors) + "],\"bufferViews\":[" +
        join(bin.views) + "],\"buffers\":[{\"byteLength\":" +
        std::to_string(bin.data.size()) + "}]";
    if (has_skin) {
        std::string joints = "[";
        for (size_t i = 0; i < joint_nodes.size(); ++i) {
            if (i) joints += ',';
            joints += std::to_string(joint_nodes[i]);
        }
        joints += ']';
        gltf += ",\"skins\":[{\"name\":\"skeleton\",\"joints\":" + joints +
                ",\"inverseBindMatrices\":" + std::to_string(ibm_accessor) +
                ",\"skeleton\":" + std::to_string(armature_index) + "}]";
    }
    if (!png_images.empty()) {
        std::vector<std::string> images, textures;
        for (size_t i = 0; i < png_images.size(); ++i) {
            images.push_back("{\"bufferView\":" + std::to_string(image_views[i]) +
                             ",\"mimeType\":\"image/png\"}");
            textures.push_back("{\"source\":" + std::to_string(i) + '}');
        }
        gltf += ",\"images\":[" + join(images) + "],\"textures\":[" + join(textures) + ']';
    }
    if (!materials.empty()) {
        std::vector<std::string> material_json;
        for (const FlatMaterial& material : materials) {
            std::string m = "{\"name\":" + jstr(material.name) +
                ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.8,0.8,1.0],"
                "\"metallicFactor\":0.0,\"roughnessFactor\":1.0";
            if (material.texture >= 0)
                m += ",\"baseColorTexture\":{\"index\":" + std::to_string(material.texture) + '}';
            m += "},\"extensions\":{\"KHR_materials_unlit\":{}}";
            if (material.placeholder)
                m += ",\"extras\":{\"jade_matId\":" + std::to_string(material.mat_id) +
                     ",\"jade_element_index\":" + std::to_string(material.element) + '}';
            else {
                char key[16]; std::snprintf(key, sizeof key, "0x%08X", material.key);
                m += ",\"extras\":{\"jade_key\":" + jstr(key) +
                     ",\"element_index\":" + std::to_string(material.element) + '}';
            }
            m += '}';
            material_json.push_back(std::move(m));
        }
        gltf += ",\"materials\":[" + join(material_json) +
                "],\"extensionsUsed\":[\"KHR_materials_unlit\"]";
    }
    gltf += ",\"extras\":{\"jade_elements\":[";
    for (size_t i = 0; i < geo.elements.size() / 2; ++i) {
        if (i) gltf += ',';
        gltf += "{\"nTri\":" + std::to_string(geo.elements[i * 2]) +
                ",\"matId\":" + std::to_string(geo.elements[i * 2 + 1]) + '}';
    }
    gltf += "],\"jade_n_original_verts\":" + std::to_string(original_vertices) + "}}";

    std::vector<uint8_t> json(gltf.begin(), gltf.end());
    while (json.size() % 4) json.push_back(' ');
    std::vector<uint8_t> out;
    auto write32 = [&](uint32_t value) { put32(out, value); };
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    write32(2);
    write32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.data.size()));
    write32(static_cast<uint32_t>(json.size()));
    out.insert(out.end(), {'J', 'S', 'O', 'N'});
    out.insert(out.end(), json.begin(), json.end());
    write32(static_cast<uint32_t>(bin.data.size()));
    out.insert(out.end(), {'B', 'I', 'N', 0});
    out.insert(out.end(), bin.data.begin(), bin.data.end());

    result.glb = std::move(out);
    result.vertices = static_cast<uint32_t>(original_vertices);
    result.faces = static_cast<uint32_t>(face_count);
    result.elements = static_cast<uint32_t>(geo.elements.size() / 2);
    result.bones = static_cast<uint32_t>(bone_names.size());
    result.materials = static_cast<uint32_t>(materials.size());
    result.textures = static_cast<uint32_t>(png_images.size());
    result.ok = true;
    return result;
}

}  // namespace jgao
}  // namespace jade
