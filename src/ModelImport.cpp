// ModelImport.cpp - dependency-free static model readers for object placement.
#include "jade/ObjectPlacer.hpp"

#include "jade/Gltf.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace jade {
namespace placer {
namespace {

struct MeshArrays {
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<uint32_t, 3>> faces;
    std::vector<std::array<double, 3>> normals;
    std::vector<std::array<double, 2>> uvs;
    std::vector<std::array<uint32_t, 3>> face_uvs;
    std::vector<std::array<int, 4>> colors;
};

struct CacheEntry {
    std::string normalized_path;
    uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
    MeshArrays arrays;
};

constexpr size_t MODEL_GEOMETRY_CACHE_LIMIT = 6;
std::vector<CacheEntry> model_cache;
std::mutex model_cache_mutex;

[[noreturn]] void import_fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

std::string lower_extension(const std::string& path) {
    std::string ext = std::filesystem::u8path(path).extension().u8string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string filename(const std::string& path) {
    return std::filesystem::u8path(path).filename().u8string();
}

std::string normalized_absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(
        absolute, error);
    if (!error) absolute = std::move(resolved);
    std::string result = absolute.lexically_normal().u8string();
#ifdef _WIN32
    for (char& c : result)
        c = char(std::tolower(static_cast<unsigned char>(c)));
#endif
    return result;
}

uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16)
         | (uint32_t(p[3]) << 24);
}

float f32(const uint8_t* p) {
    const uint32_t bits = le32(p);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

double f64(const uint8_t* p) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits |= uint64_t(p[i]) << (i * 8);
    double value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

double accessor_number(const gltf::AccessorData& accessor, size_t index) {
    const size_t size = gltf::component_size(accessor.comp_type);
    if (size == 0 || index >= size_t(accessor.count) * accessor.n_comps)
        import_fail("glTF accessor index is out of range");
    const uint8_t* p = accessor.raw.data() + index * size;
    switch (accessor.comp_type) {
        case gltf::COMP_F32: return f32(p);
        case gltf::COMP_U8: return p[0];
        case gltf::COMP_I8: return int8_t(p[0]);
        case gltf::COMP_U16: return le16(p);
        case gltf::COMP_I16: return int16_t(le16(p));
        case gltf::COMP_U32: return le32(p);
        default: import_fail("unsupported glTF accessor component type");
    }
}

uint32_t accessor_index(const gltf::AccessorData& accessor, size_t index) {
    const double value = accessor_number(accessor, index);
    if (value < 0.0 || value > double(std::numeric_limits<uint32_t>::max()))
        import_fail("glTF has an invalid triangle index");
    return uint32_t(value);
}

bool invert3(const std::array<double, 16>& m, double out[9]) {
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[4], e = m[5], f = m[6];
    const double g = m[8], h = m[9], i = m[10];
    const double A = e * i - f * h, B = c * h - b * i;
    const double C = b * f - c * e, D = f * g - d * i;
    const double E = a * i - c * g, F = c * d - a * f;
    const double G = d * h - e * g, H = b * g - a * h;
    const double I = a * e - b * d;
    const double det = a * A + b * D + c * G;
    if (std::abs(det) < 1e-20) return false;
    const double inv = 1.0 / det;
    const double values[9] = {A, B, C, D, E, F, G, H, I};
    for (int k = 0; k < 9; ++k) out[k] = values[k] * inv;
    return true;
}

MeshArrays load_gltf_document(const gltf::GlbDoc& doc) {
    const json::Value* meshes = doc.gltf.find("meshes");
    if (!meshes || !meshes->is_arr() || meshes->arr.empty())
        import_fail("glTF contains no mesh geometry");
    std::vector<std::pair<int, std::array<double, 16>>> instances =
        gltf::mesh_world_instances(doc.gltf);
    if (instances.empty()) {
        const std::array<double, 16> identity{{1,0,0,0, 0,1,0,0,
                                               0,0,1,0, 0,0,0,1}};
        for (size_t mesh = 0; mesh < meshes->arr.size(); ++mesh)
            instances.push_back({int(mesh), identity});
    }
    MeshArrays out;
    bool all_normals = true, all_uvs = true, all_colors = true;

    for (const auto& instance : instances) {
        const int mesh_index = instance.first;
        if (mesh_index < 0 || size_t(mesh_index) >= meshes->arr.size()) continue;
        const json::Value* primitives =
            meshes->arr[size_t(mesh_index)].find("primitives");
        if (!primitives || !primitives->is_arr()) continue;
        const std::array<double, 16>& matrix = instance.second;
        double inverse[9]{};
        const bool inverse_ok = invert3(matrix, inverse);

        for (const json::Value& primitive : primitives->arr) {
            if (primitive.get_int("mode", 4) != 4) continue;
            const json::Value* attrs = primitive.find("attributes");
            const json::Value* pos_value = attrs ? attrs->find("POSITION") : nullptr;
            if (!pos_value || !pos_value->is_num()) continue;
            const gltf::AccessorData positions =
                gltf::read_accessor(doc, int(pos_value->num));
            if (positions.n_comps < 3) import_fail("glTF POSITION is not VEC3");
            const uint32_t base = uint32_t(out.vertices.size());
            for (uint32_t index = 0; index < positions.count; ++index) {
                const double x = accessor_number(positions, size_t(index) * positions.n_comps);
                const double y = accessor_number(positions, size_t(index) * positions.n_comps + 1);
                const double z = accessor_number(positions, size_t(index) * positions.n_comps + 2);
                out.vertices.push_back({
                    matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3],
                    matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7],
                    matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11]});
            }

            const json::Value* normal_value = attrs->find("NORMAL");
            if (normal_value && normal_value->is_num()) {
                const gltf::AccessorData normals =
                    gltf::read_accessor(doc, int(normal_value->num));
                if (normals.count != positions.count || normals.n_comps < 3)
                    all_normals = false;
                else for (uint32_t index = 0; index < normals.count; ++index) {
                    const double x = accessor_number(normals, size_t(index) * normals.n_comps);
                    const double y = accessor_number(normals, size_t(index) * normals.n_comps + 1);
                    const double z = accessor_number(normals, size_t(index) * normals.n_comps + 2);
                    double nx, ny, nz;
                    if (inverse_ok) {
                        // row-vector normal @ inverse(linear), matching trimesh.
                        nx = x * inverse[0] + y * inverse[3] + z * inverse[6];
                        ny = x * inverse[1] + y * inverse[4] + z * inverse[7];
                        nz = x * inverse[2] + y * inverse[5] + z * inverse[8];
                    } else {
                        nx = matrix[0] * x + matrix[1] * y + matrix[2] * z;
                        ny = matrix[4] * x + matrix[5] * y + matrix[6] * z;
                        nz = matrix[8] * x + matrix[9] * y + matrix[10] * z;
                    }
                    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 1e-20) { nx /= len; ny /= len; nz /= len; }
                    out.normals.push_back({nx, ny, nz});
                }
            } else {
                all_normals = false;
            }

            const json::Value* uv_value = attrs->find("TEXCOORD_0");
            if (uv_value && uv_value->is_num()) {
                const gltf::AccessorData texcoords =
                    gltf::read_accessor(doc, int(uv_value->num));
                if (texcoords.count != positions.count || texcoords.n_comps < 2)
                    all_uvs = false;
                else for (uint32_t index = 0; index < texcoords.count; ++index)
                    out.uvs.push_back({
                        accessor_number(texcoords, size_t(index) * texcoords.n_comps),
                        accessor_number(texcoords, size_t(index) * texcoords.n_comps + 1)});
            } else {
                all_uvs = false;
            }

            const json::Value* color_value = attrs->find("COLOR_0");
            if (color_value && color_value->is_num()) {
                const gltf::AccessorData colors =
                    gltf::read_accessor(doc, int(color_value->num));
                const auto converted = gltf::colors_to_rgba255(colors);
                if (converted.size() != positions.count) all_colors = false;
                else out.colors.insert(out.colors.end(), converted.begin(), converted.end());
            } else {
                all_colors = false;
            }

            std::vector<uint32_t> indices;
            const json::Value* indices_value = primitive.find("indices");
            if (indices_value && indices_value->is_num()) {
                const gltf::AccessorData accessor =
                    gltf::read_accessor(doc, int(indices_value->num));
                const size_t count = size_t(accessor.count) * accessor.n_comps;
                indices.reserve(count);
                for (size_t index = 0; index < count; ++index)
                    indices.push_back(accessor_index(accessor, index));
            } else {
                indices.resize(positions.count);
                for (uint32_t i = 0; i < positions.count; ++i) indices[i] = i;
            }
            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                if (indices[i] >= positions.count || indices[i + 1] >= positions.count
                    || indices[i + 2] >= positions.count)
                    import_fail("glTF has an invalid triangle index");
                const std::array<uint32_t, 3> face{{
                    base + indices[i], base + indices[i + 1], base + indices[i + 2]}};
                out.faces.push_back(face);
                out.face_uvs.push_back(face);
            }
        }
    }
    if (!all_normals || out.normals.size() != out.vertices.size()) out.normals.clear();
    if (!all_uvs || out.uvs.size() != out.vertices.size()) {
        out.uvs.clear(); out.face_uvs.clear();
    }
    if (!all_colors || out.colors.size() != out.vertices.size()) out.colors.clear();
    return out;
}

MeshArrays load_glb(const std::vector<uint8_t>& bytes) {
    return load_gltf_document(gltf::parse_glb(bytes.data(), bytes.size()));
}

int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> decode_base64(const std::string& text) {
    std::vector<uint8_t> result;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (unsigned char c : text) {
        if (c == '=') break;
        if (std::isspace(c)) continue;
        const int value = base64_digit(c);
        if (value < 0) import_fail("glTF has an invalid base64 buffer URI");
        accumulator = (accumulator << 6) | uint32_t(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(uint8_t(accumulator >> bits));
            accumulator &= bits ? ((uint32_t(1) << bits) - 1) : 0;
        }
    }
    return result;
}

std::string percent_decode(std::string text) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = digit(text[i + 1]), low = digit(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(char((high << 4) | low));
                i += 2; continue;
            }
        }
        result.push_back(text[i]);
    }
    return result;
}

MeshArrays load_text_gltf(const std::string& model_path,
                          const std::vector<uint8_t>& bytes) {
    gltf::GlbDoc doc;
    doc.gltf = json::parse(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size());
    json::Value* buffers = nullptr;
    auto buffers_it = doc.gltf.obj.find("buffers");
    if (buffers_it != doc.gltf.obj.end() && buffers_it->second.is_arr())
        buffers = &buffers_it->second;
    if (!buffers || buffers->arr.empty())
        import_fail("glTF contains no buffers");
    std::vector<size_t> bases;
    bases.reserve(buffers->arr.size());
    const std::filesystem::path parent =
        std::filesystem::u8path(model_path).parent_path();
    for (const json::Value& buffer : buffers->arr) {
        const json::Value* uri_value = buffer.find("uri");
        if (!uri_value || !uri_value->is_str())
            import_fail("textual glTF buffer has no URI");
        std::vector<uint8_t> content;
        const std::string& uri = uri_value->str;
        if (uri.rfind("data:", 0) == 0) {
            const size_t comma = uri.find(',');
            if (comma == std::string::npos
                || uri.substr(0, comma).find(";base64") == std::string::npos)
                import_fail("glTF data URI is not base64 encoded");
            content = decode_base64(uri.substr(comma + 1));
        } else {
            const std::filesystem::path buffer_path =
                parent / std::filesystem::u8path(percent_decode(uri));
            content = read_file(buffer_path.u8string());
            if (content.empty() && buffer.get_int("byteLength", 0) != 0)
                import_fail("glTF buffer file was not found: "
                            + buffer_path.filename().u8string());
        }
        bases.push_back(doc.bin.size());
        doc.bin.insert(doc.bin.end(), content.begin(), content.end());
    }
    auto views_it = doc.gltf.obj.find("bufferViews");
    if (views_it != doc.gltf.obj.end() && views_it->second.is_arr())
        for (json::Value& view : views_it->second.arr) {
            const size_t buffer_index = size_t(view.get_int("buffer", 0));
            if (buffer_index >= bases.size())
                import_fail("glTF bufferView has an invalid buffer index");
            const size_t offset = size_t(view.get_int("byteOffset", 0));
            view.obj["byteOffset"] = json::make_num(double(bases[buffer_index] + offset));
            view.obj["buffer"] = json::make_num(0);
        }
    return load_gltf_document(doc);
}

// COLLADA deliberately lives here instead of adding an XML/runtime dependency to
// the small model-import CLI and the placement GUI. The parser implements the XML
// constructs used by DAE files, including namespaces and numeric entities.
struct XmlNode {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::string text;
    std::vector<XmlNode> children;
};

std::string xml_local_name(const std::string& name) {
    const size_t colon = name.rfind(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

std::string xml_decode(const std::string& encoded) {
    std::string result;
    result.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '&') { result.push_back(encoded[i]); continue; }
        const size_t end = encoded.find(';', i + 1);
        if (end == std::string::npos) import_fail("COLLADA has an invalid XML entity");
        const std::string entity = encoded.substr(i + 1, end - i - 1);
        uint32_t code = 0;
        if (entity == "amp") code = '&';
        else if (entity == "lt") code = '<';
        else if (entity == "gt") code = '>';
        else if (entity == "quot") code = '"';
        else if (entity == "apos") code = '\'';
        else if (!entity.empty() && entity[0] == '#') {
            try {
                const bool hex = entity.size() > 1
                    && (entity[1] == 'x' || entity[1] == 'X');
                code = uint32_t(std::stoul(entity.substr(hex ? 2 : 1),
                                           nullptr, hex ? 16 : 10));
            } catch (...) { import_fail("COLLADA has an invalid XML entity"); }
        } else import_fail("COLLADA has an unsupported XML entity '&" + entity + ";'");
        if (code <= 0x7F) result.push_back(char(code));
        else if (code <= 0x7FF) {
            result.push_back(char(0xC0 | (code >> 6)));
            result.push_back(char(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            result.push_back(char(0xE0 | (code >> 12)));
            result.push_back(char(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(char(0x80 | (code & 0x3F)));
        } else {
            result.push_back(char(0xF0 | (code >> 18)));
            result.push_back(char(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(char(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(char(0x80 | (code & 0x3F)));
        }
        i = end;
    }
    return result;
}

class XmlParser {
public:
    explicit XmlParser(const std::string& source) : source_(source) {}

    XmlNode document() {
        while (true) {
            skip_space();
            if (starts("<?")) skip_until("?>");
            else if (starts("<!--")) skip_until("-->");
            else if (starts("<!DOCTYPE")) skip_doctype();
            else break;
        }
        if (position_ >= source_.size() || source_[position_] != '<')
            import_fail("COLLADA XML has no root element");
        return node();
    }

private:
    const std::string& source_;
    size_t position_ = 0;

    bool starts(const char* value) const {
        const size_t length = std::strlen(value);
        return position_ + length <= source_.size()
            && source_.compare(position_, length, value) == 0;
    }
    void skip_space() {
        while (position_ < source_.size()
               && std::isspace(static_cast<unsigned char>(source_[position_])))
            ++position_;
    }
    void skip_until(const char* ending) {
        const size_t found = source_.find(ending, position_);
        if (found == std::string::npos) import_fail("COLLADA XML is truncated");
        position_ = found + std::strlen(ending);
    }
    void skip_doctype() {
        int bracket_depth = 0;
        char quote = 0;
        while (position_ < source_.size()) {
            const char c = source_[position_++];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '\'' || c == '"') quote = c;
            else if (c == '[') ++bracket_depth;
            else if (c == ']') --bracket_depth;
            else if (c == '>' && bracket_depth == 0) return;
        }
        import_fail("COLLADA XML has a truncated doctype");
    }
    std::string name() {
        const size_t begin = position_;
        while (position_ < source_.size()) {
            const unsigned char c = static_cast<unsigned char>(source_[position_]);
            if (!(std::isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.')) break;
            ++position_;
        }
        if (position_ == begin) import_fail("COLLADA XML has an invalid name");
        return source_.substr(begin, position_ - begin);
    }
    XmlNode node() {
        if (position_ >= source_.size() || source_[position_++] != '<')
            import_fail("COLLADA XML has an invalid element");
        XmlNode result;
        result.name = name();
        while (true) {
            skip_space();
            if (starts("/>")) { position_ += 2; return result; }
            if (position_ < source_.size() && source_[position_] == '>') {
                ++position_; break;
            }
            const std::string attribute_name = name();
            skip_space();
            if (position_ >= source_.size() || source_[position_++] != '=')
                import_fail("COLLADA XML attribute has no value");
            skip_space();
            if (position_ >= source_.size()
                || (source_[position_] != '\'' && source_[position_] != '"'))
                import_fail("COLLADA XML attribute is not quoted");
            const char quote = source_[position_++];
            const size_t begin = position_;
            const size_t end = source_.find(quote, begin);
            if (end == std::string::npos) import_fail("COLLADA XML is truncated");
            result.attributes[xml_local_name(attribute_name)] =
                xml_decode(source_.substr(begin, end - begin));
            position_ = end + 1;
        }
        while (position_ < source_.size()) {
            if (starts("</")) {
                position_ += 2;
                const std::string close_name = name();
                skip_space();
                if (position_ >= source_.size() || source_[position_++] != '>'
                    || close_name != result.name)
                    import_fail("COLLADA XML has mismatched elements");
                return result;
            }
            if (starts("<!--")) { skip_until("-->"); continue; }
            if (starts("<?")) { skip_until("?>"); continue; }
            if (starts("<![CDATA[")) {
                position_ += 9;
                const size_t end = source_.find("]]>", position_);
                if (end == std::string::npos) import_fail("COLLADA XML has truncated CDATA");
                result.text.append(source_, position_, end - position_);
                position_ = end + 3; continue;
            }
            if (source_[position_] == '<') result.children.push_back(node());
            else {
                const size_t end = source_.find('<', position_);
                if (end == std::string::npos) import_fail("COLLADA XML is truncated");
                result.text += xml_decode(source_.substr(position_, end - position_));
                position_ = end;
            }
        }
        import_fail("COLLADA XML is truncated");
    }
};

const XmlNode* xml_child(const XmlNode& node, const std::string& name) {
    for (const XmlNode& child : node.children)
        if (xml_local_name(child.name) == name) return &child;
    return nullptr;
}

std::vector<const XmlNode*> xml_children(const XmlNode& node,
                                         const std::string& name) {
    std::vector<const XmlNode*> result;
    for (const XmlNode& child : node.children)
        if (xml_local_name(child.name) == name) result.push_back(&child);
    return result;
}

std::string xml_attribute(const XmlNode& node, const std::string& name,
                          const std::string& fallback = {}) {
    auto found = node.attributes.find(name);
    return found == node.attributes.end() ? fallback : found->second;
}

std::string dae_ref(std::string value) {
    if (!value.empty() && value[0] == '#') value.erase(value.begin());
    return value;
}

std::vector<double> dae_doubles(const std::string& text) {
    std::istringstream input(text);
    std::vector<double> values;
    double value;
    while (input >> value) values.push_back(value);
    return values;
}

std::vector<int> dae_integers(const std::string& text) {
    std::istringstream input(text);
    std::vector<int> values;
    long long value;
    while (input >> value) {
        if (value < 0 || value > std::numeric_limits<int>::max())
            import_fail("COLLADA has an invalid mesh index");
        values.push_back(int(value));
    }
    return values;
}

struct DaeSource {
    std::vector<double> values;
    size_t offset = 0;
    size_t stride = 1;
    size_t count = 0;
};

std::array<double, 4> dae_source_value(const DaeSource& source, int index) {
    if (index < 0 || size_t(index) >= source.count)
        import_fail("COLLADA has an out-of-range source index");
    const size_t base = source.offset + size_t(index) * source.stride;
    std::array<double, 4> result{{0, 0, 0, 1}};
    for (size_t i = 0; i < std::min<size_t>(4, source.stride); ++i) {
        if (base + i >= source.values.size())
            import_fail("COLLADA source accessor exceeds its float array");
        result[i] = source.values[base + i];
    }
    return result;
}

using Matrix4 = std::array<double, 16>;

Matrix4 matrix_identity() {
    return {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
}

Matrix4 matrix_multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            for (size_t inner = 0; inner < 4; ++inner)
                result[row * 4 + column] +=
                    a[row * 4 + inner] * b[inner * 4 + column];
    return result;
}

Matrix4 dae_node_transform(const XmlNode& node) {
    Matrix4 result = matrix_identity();
    constexpr double pi = 3.14159265358979323846;
    for (const XmlNode& child : node.children) {
        const std::string kind = xml_local_name(child.name);
        const std::vector<double> values = dae_doubles(child.text);
        Matrix4 transform = matrix_identity();
        if (kind == "matrix" && values.size() >= 16) {
            // COLLADA serializes matrices column-major.
            for (size_t row = 0; row < 4; ++row)
                for (size_t column = 0; column < 4; ++column)
                    transform[row * 4 + column] = values[column * 4 + row];
        } else if (kind == "translate" && values.size() >= 3) {
            transform[3] = values[0]; transform[7] = values[1]; transform[11] = values[2];
        } else if (kind == "scale" && values.size() >= 3) {
            transform[0] = values[0]; transform[5] = values[1]; transform[10] = values[2];
        } else if (kind == "rotate" && values.size() >= 4) {
            double x = values[0], y = values[1], z = values[2];
            const double length = std::sqrt(x*x + y*y + z*z);
            if (length <= 1e-20) continue;
            x /= length; y /= length; z /= length;
            const double angle = values[3] * pi / 180.0;
            const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
            transform = {{t*x*x+c, t*x*y-s*z, t*x*z+s*y, 0,
                          t*x*y+s*z, t*y*y+c, t*y*z-s*x, 0,
                          t*x*z-s*y, t*y*z+s*x, t*z*z+c, 0,
                          0,0,0,1}};
        } else continue;
        result = matrix_multiply(result, transform);
    }
    return result;
}

MeshArrays parse_dae_geometry(const XmlNode& geometry) {
    const XmlNode* mesh = xml_child(geometry, "mesh");
    if (!mesh) return {};
    std::map<std::string, DaeSource> sources;
    for (const XmlNode* source_node : xml_children(*mesh, "source")) {
        const std::string id = xml_attribute(*source_node, "id");
        const XmlNode* array = xml_child(*source_node, "float_array");
        if (id.empty() || !array) continue;
        DaeSource source;
        source.values = dae_doubles(array->text);
        source.count = source.values.size();
        const XmlNode* common = xml_child(*source_node, "technique_common");
        const XmlNode* accessor = common ? xml_child(*common, "accessor") : nullptr;
        if (accessor) {
            source.offset = size_t(std::stoull(xml_attribute(*accessor, "offset", "0")));
            source.stride = size_t(std::stoull(xml_attribute(*accessor, "stride", "1")));
            source.count = size_t(std::stoull(xml_attribute(*accessor, "count", "0")));
            if (source.stride == 0) import_fail("COLLADA source has zero stride");
        }
        sources[id] = std::move(source);
    }
    std::map<std::string, std::map<std::string, std::string>> vertices_inputs;
    for (const XmlNode* vertices : xml_children(*mesh, "vertices")) {
        auto& semantics = vertices_inputs[xml_attribute(*vertices, "id")];
        for (const XmlNode* input : xml_children(*vertices, "input"))
            semantics[xml_attribute(*input, "semantic")] =
                dae_ref(xml_attribute(*input, "source"));
    }

    struct Input { std::string semantic, source; int offset = 0, set = 0; };
    struct Corner {
        int position = -1, normal = -1, uv = -1, color = -1;
        std::string position_source, normal_source, uv_source, color_source;
    };
    using CornerKey = std::tuple<std::string,int,std::string,int,
                                 std::string,int,std::string,int>;
    MeshArrays out;
    std::map<CornerKey, uint32_t> expanded;
    bool all_normals = true, all_uvs = true, all_colors = true;

    auto source_for = [&](const std::string& id) -> const DaeSource& {
        auto found = sources.find(id);
        if (found == sources.end()) import_fail("COLLADA mesh references a missing source");
        return found->second;
    };
    auto append_corner = [&](const Corner& corner) -> uint32_t {
        if (corner.position < 0 || corner.position_source.empty())
            import_fail("COLLADA primitive has no POSITION input");
        const CornerKey key{corner.position_source, corner.position,
                            corner.normal_source, corner.normal,
                            corner.uv_source, corner.uv,
                            corner.color_source, corner.color};
        auto found = expanded.find(key);
        if (found != expanded.end()) return found->second;
        const uint32_t index = uint32_t(out.vertices.size());
        expanded[key] = index;
        const auto position = dae_source_value(source_for(corner.position_source), corner.position);
        out.vertices.push_back({position[0], position[1], position[2]});
        if (corner.normal >= 0 && !corner.normal_source.empty()) {
            const auto normal = dae_source_value(source_for(corner.normal_source), corner.normal);
            out.normals.push_back({normal[0], normal[1], normal[2]});
        } else { all_normals = false; out.normals.push_back({0,0,0}); }
        if (corner.uv >= 0 && !corner.uv_source.empty()) {
            const auto uv = dae_source_value(source_for(corner.uv_source), corner.uv);
            out.uvs.push_back({uv[0], uv[1]});
        } else { all_uvs = false; out.uvs.push_back({0,0}); }
        if (corner.color >= 0 && !corner.color_source.empty()) {
            const auto color = dae_source_value(source_for(corner.color_source), corner.color);
            auto channel = [](double value) {
                if (value >= 0.0 && value <= 1.0) value *= 255.0;
                return int(std::max(0.0, std::min(255.0, std::nearbyint(value))));
            };
            out.colors.push_back({channel(color[0]), channel(color[1]),
                                  channel(color[2]), channel(color[3])});
        } else { all_colors = false; out.colors.push_back({255,255,255,255}); }
        return index;
    };

    for (const XmlNode& primitive : mesh->children) {
        const std::string kind = xml_local_name(primitive.name);
        if (kind != "triangles" && kind != "polylist" && kind != "polygons"
            && kind != "trifans" && kind != "tristrips") continue;
        std::vector<Input> inputs;
        int stride = 0;
        for (const XmlNode* input_node : xml_children(primitive, "input")) {
            Input input;
            input.semantic = xml_attribute(*input_node, "semantic");
            input.source = dae_ref(xml_attribute(*input_node, "source"));
            input.offset = std::stoi(xml_attribute(*input_node, "offset", "0"));
            input.set = std::stoi(xml_attribute(*input_node, "set", "0"));
            if (input.offset < 0) import_fail("COLLADA has a negative input offset");
            stride = std::max(stride, input.offset + 1);
            inputs.push_back(std::move(input));
        }
        if (stride == 0) continue;
        auto decode_corner = [&](const std::vector<int>& indices, size_t base) {
            if (base + size_t(stride) > indices.size())
                import_fail("COLLADA primitive index data is truncated");
            Corner corner;
            for (const Input& input : inputs) {
                const int index = indices[base + size_t(input.offset)];
                auto assign = [&](const std::string& semantic, const std::string& source) {
                    if (semantic == "POSITION") {
                        corner.position = index; corner.position_source = source;
                    } else if (semantic == "NORMAL") {
                        corner.normal = index; corner.normal_source = source;
                    } else if (semantic == "TEXCOORD" && (corner.uv < 0 || input.set == 0)) {
                        corner.uv = index; corner.uv_source = source;
                    } else if (semantic == "COLOR" && (corner.color < 0 || input.set == 0)) {
                        corner.color = index; corner.color_source = source;
                    }
                };
                if (input.semantic == "VERTEX") {
                    auto vertex = vertices_inputs.find(input.source);
                    if (vertex == vertices_inputs.end())
                        import_fail("COLLADA primitive references missing <vertices>");
                    for (const auto& semantic : vertex->second)
                        assign(semantic.first, semantic.second);
                } else assign(input.semantic, input.source);
            }
            return corner;
        };
        auto triangulate = [&](const std::vector<int>& indices, size_t first,
                               size_t vertex_count, bool strip = false) {
            if (vertex_count < 3) return;
            std::vector<uint32_t> polygon;
            polygon.reserve(vertex_count);
            for (size_t i = 0; i < vertex_count; ++i)
                polygon.push_back(append_corner(decode_corner(indices,
                    first + i * size_t(stride))));
            for (size_t i = 1; i + 1 < polygon.size(); ++i) {
                std::array<uint32_t, 3> face;
                if (strip) face = (i & 1)
                    ? std::array<uint32_t,3>{{polygon[i], polygon[i-1], polygon[i+1]}}
                    : std::array<uint32_t,3>{{polygon[i-1], polygon[i], polygon[i+1]}};
                else face = {{polygon[0], polygon[i], polygon[i+1]}};
                out.faces.push_back(face); out.face_uvs.push_back(face);
            }
        };
        if (kind == "polygons" || kind == "trifans") {
            for (const XmlNode* p : xml_children(primitive, "p")) {
                const std::vector<int> indices = dae_integers(p->text);
                if (indices.size() % size_t(stride))
                    import_fail("COLLADA primitive index count is invalid");
                triangulate(indices, 0, indices.size() / size_t(stride));
            }
        } else if (kind == "tristrips") {
            for (const XmlNode* p : xml_children(primitive, "p")) {
                const std::vector<int> indices = dae_integers(p->text);
                if (indices.size() % size_t(stride))
                    import_fail("COLLADA primitive index count is invalid");
                triangulate(indices, 0, indices.size() / size_t(stride), true);
            }
        } else {
            const XmlNode* p = xml_child(primitive, "p");
            if (!p) continue;
            const std::vector<int> indices = dae_integers(p->text);
            if (kind == "triangles") {
                if (indices.size() % (size_t(stride) * 3))
                    import_fail("COLLADA triangle index count is invalid");
                for (size_t first = 0; first < indices.size(); first += size_t(stride) * 3)
                    triangulate(indices, first, 3);
            } else {
                const XmlNode* vcount_node = xml_child(primitive, "vcount");
                if (!vcount_node) import_fail("COLLADA polylist has no vcount");
                const std::vector<int> counts = dae_integers(vcount_node->text);
                size_t first = 0;
                for (int count : counts) {
                    const size_t consumed = size_t(count) * size_t(stride);
                    if (first + consumed > indices.size())
                        import_fail("COLLADA polylist index data is truncated");
                    triangulate(indices, first, size_t(count)); first += consumed;
                }
                if (first != indices.size())
                    import_fail("COLLADA polylist has unused index data");
            }
        }
    }
    if (!all_normals) out.normals.clear();
    if (!all_uvs) { out.uvs.clear(); out.face_uvs.clear(); }
    if (!all_colors) out.colors.clear();
    return out;
}

void append_dae_instance(MeshArrays& out, const MeshArrays& mesh,
                         const Matrix4& matrix,
                         bool& all_normals, bool& all_uvs, bool& all_colors) {
    if (mesh.vertices.empty() || mesh.faces.empty()) return;
    const uint32_t base = uint32_t(out.vertices.size());
    for (const auto& vertex : mesh.vertices) {
        const double x = vertex[0], y = vertex[1], z = vertex[2];
        out.vertices.push_back({matrix[0]*x + matrix[1]*y + matrix[2]*z + matrix[3],
                                matrix[4]*x + matrix[5]*y + matrix[6]*z + matrix[7],
                                matrix[8]*x + matrix[9]*y + matrix[10]*z + matrix[11]});
    }
    double inverse[9]{};
    const bool inverse_ok = invert3(matrix, inverse);
    if (mesh.normals.size() == mesh.vertices.size()) {
        for (const auto& normal : mesh.normals) {
            const double x = normal[0], y = normal[1], z = normal[2];
            double nx, ny, nz;
            if (inverse_ok) {
                nx = x*inverse[0] + y*inverse[3] + z*inverse[6];
                ny = x*inverse[1] + y*inverse[4] + z*inverse[7];
                nz = x*inverse[2] + y*inverse[5] + z*inverse[8];
            } else {
                nx = matrix[0]*x + matrix[1]*y + matrix[2]*z;
                ny = matrix[4]*x + matrix[5]*y + matrix[6]*z;
                nz = matrix[8]*x + matrix[9]*y + matrix[10]*z;
            }
            const double length = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (length > 1e-20) { nx /= length; ny /= length; nz /= length; }
            out.normals.push_back({nx, ny, nz});
        }
    } else all_normals = false;
    if (mesh.uvs.size() == mesh.vertices.size())
        out.uvs.insert(out.uvs.end(), mesh.uvs.begin(), mesh.uvs.end());
    else all_uvs = false;
    if (mesh.colors.size() == mesh.vertices.size())
        out.colors.insert(out.colors.end(), mesh.colors.begin(), mesh.colors.end());
    else all_colors = false;
    for (const auto& face : mesh.faces) {
        const std::array<uint32_t,3> shifted{{face[0]+base, face[1]+base, face[2]+base}};
        out.faces.push_back(shifted); out.face_uvs.push_back(shifted);
    }
}

MeshArrays load_dae(const std::vector<uint8_t>& bytes) {
    const std::string text(bytes.begin(), bytes.end());
    XmlNode root = XmlParser(text).document();
    if (xml_local_name(root.name) != "COLLADA")
        import_fail("DAE root element is not COLLADA");
    std::map<std::string, MeshArrays> geometries;
    if (const XmlNode* library = xml_child(root, "library_geometries"))
        for (const XmlNode* geometry : xml_children(*library, "geometry")) {
            const std::string id = xml_attribute(*geometry, "id");
            if (!id.empty()) geometries[id] = parse_dae_geometry(*geometry);
        }
    if (geometries.empty()) import_fail("COLLADA contains no mesh geometry");

    std::map<std::string, const XmlNode*> visual_scenes, library_nodes;
    if (const XmlNode* library = xml_child(root, "library_visual_scenes"))
        for (const XmlNode* scene : xml_children(*library, "visual_scene"))
            visual_scenes[xml_attribute(*scene, "id")] = scene;
    if (const XmlNode* library = xml_child(root, "library_nodes"))
        for (const XmlNode* node : xml_children(*library, "node"))
            library_nodes[xml_attribute(*node, "id")] = node;

    MeshArrays out;
    bool all_normals = true, all_uvs = true, all_colors = true;
    size_t instance_count = 0;
    std::function<void(const XmlNode&, const Matrix4&, int)> visit;
    visit = [&](const XmlNode& node, const Matrix4& parent, int depth) {
        if (depth > 128) import_fail("COLLADA node graph is recursive");
        const Matrix4 world = matrix_multiply(parent, dae_node_transform(node));
        for (const XmlNode* instance : xml_children(node, "instance_geometry")) {
            auto found = geometries.find(dae_ref(xml_attribute(*instance, "url")));
            if (found == geometries.end())
                import_fail("COLLADA scene references missing geometry");
            append_dae_instance(out, found->second, world,
                                all_normals, all_uvs, all_colors);
            ++instance_count;
        }
        for (const XmlNode* instance : xml_children(node, "instance_node")) {
            auto found = library_nodes.find(dae_ref(xml_attribute(*instance, "url")));
            if (found == library_nodes.end())
                import_fail("COLLADA scene references missing library node");
            visit(*found->second, world, depth + 1);
        }
        for (const XmlNode* child : xml_children(node, "node"))
            visit(*child, world, depth + 1);
    };

    const XmlNode* scene = xml_child(root, "scene");
    const XmlNode* instance_scene = scene ? xml_child(*scene, "instance_visual_scene") : nullptr;
    if (instance_scene) {
        auto found = visual_scenes.find(dae_ref(xml_attribute(*instance_scene, "url")));
        if (found == visual_scenes.end()) import_fail("COLLADA references missing visual scene");
        for (const XmlNode* node : xml_children(*found->second, "node"))
            visit(*node, matrix_identity(), 0);
    }
    if (instance_count == 0) {
        const Matrix4 identity = matrix_identity();
        for (const auto& geometry : geometries)
            append_dae_instance(out, geometry.second, identity,
                                all_normals, all_uvs, all_colors);
    }
    if (out.faces.empty()) import_fail("COLLADA contains no triangle geometry");
    if (!all_normals || out.normals.size() != out.vertices.size()) out.normals.clear();
    if (!all_uvs || out.uvs.size() != out.vertices.size()) {
        out.uvs.clear(); out.face_uvs.clear();
    }
    if (!all_colors || out.colors.size() != out.vertices.size()) out.colors.clear();
    return out;
}

int obj_index(const std::string& text, size_t count) {
    if (text.empty()) return -1;
    const long value = std::stol(text);
    const long index = value > 0 ? value - 1 : long(count) + value;
    if (index < 0 || index >= long(count)) import_fail("OBJ has an invalid face index");
    return int(index);
}

MeshArrays load_obj(const std::string& text) {
    std::vector<std::array<double, 3>> raw_vertices, raw_normals;
    std::vector<std::array<double, 2>> raw_uvs;
    std::vector<std::array<int, 4>> raw_colors;
    struct Corner { int v = -1, vt = -1, vn = -1; };
    std::vector<std::vector<Corner>> polygons;
    bool any_vt = false, any_vn = false, have_all_colors = true;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        std::string tag;
        if (!(words >> tag)) continue;
        if (tag == "v") {
            double x, y, z;
            if (!(words >> x >> y >> z)) continue;
            raw_vertices.push_back({x, y, z});
            double r, g, b, a = 1.0;
            if (words >> r >> g >> b) {
                if (!(words >> a)) a = 1.0;
                auto channel = [](double v) {
                    if (v >= 0.0 && v <= 1.0) v *= 255.0;
                    return int(std::max(0.0, std::min(255.0, std::round(v))));
                };
                raw_colors.push_back({channel(r), channel(g), channel(b), channel(a)});
            } else {
                have_all_colors = false;
                raw_colors.push_back({255, 255, 255, 255});
            }
        } else if (tag == "vn") {
            double x, y, z;
            if (words >> x >> y >> z) raw_normals.push_back({x, y, z});
        } else if (tag == "vt") {
            double u, v;
            if (words >> u >> v) raw_uvs.push_back({u, v});
        } else if (tag == "f") {
            std::vector<Corner> polygon;
            std::string token;
            while (words >> token) {
                std::array<std::string, 3> fields;
                size_t start = 0;
                for (int i = 0; i < 3; ++i) {
                    const size_t slash = token.find('/', start);
                    fields[size_t(i)] = token.substr(
                        start, slash == std::string::npos ? slash : slash - start);
                    if (slash == std::string::npos) break;
                    start = slash + 1;
                }
                Corner c;
                c.v = obj_index(fields[0], raw_vertices.size());
                if (!fields[1].empty()) {
                    c.vt = obj_index(fields[1], raw_uvs.size()); any_vt = true;
                }
                if (!fields[2].empty()) {
                    c.vn = obj_index(fields[2], raw_normals.size()); any_vn = true;
                }
                polygon.push_back(c);
            }
            if (polygon.size() >= 3) polygons.push_back(std::move(polygon));
        }
    }

    MeshArrays out;
    if (!any_vt && !any_vn) {
        out.vertices = raw_vertices;
        if (have_all_colors) out.colors = raw_colors;
        for (const auto& polygon : polygons)
            for (size_t i = 1; i + 1 < polygon.size(); ++i)
                out.faces.push_back({uint32_t(polygon[0].v),
                                     uint32_t(polygon[i].v),
                                     uint32_t(polygon[i + 1].v)});
        return out;
    }

    std::map<std::tuple<int, int, int>, uint32_t> expanded;
    bool complete_uv = any_vt, complete_normals = any_vn;
    for (const auto& polygon : polygons) {
        std::vector<uint32_t> face_vertices;
        for (const Corner& corner : polygon) {
            complete_uv = complete_uv && corner.vt >= 0;
            complete_normals = complete_normals && corner.vn >= 0;
            const auto key = std::make_tuple(corner.v, corner.vt, corner.vn);
            auto found = expanded.find(key);
            if (found == expanded.end()) {
                const uint32_t index = uint32_t(out.vertices.size());
                expanded[key] = index;
                out.vertices.push_back(raw_vertices[size_t(corner.v)]);
                out.uvs.push_back(corner.vt >= 0 ? raw_uvs[size_t(corner.vt)]
                                                 : std::array<double, 2>{0, 0});
                out.normals.push_back(corner.vn >= 0 ? raw_normals[size_t(corner.vn)]
                                                      : std::array<double, 3>{0, 0, 0});
                if (have_all_colors) out.colors.push_back(raw_colors[size_t(corner.v)]);
                face_vertices.push_back(index);
            } else {
                face_vertices.push_back(found->second);
            }
        }
        for (size_t i = 1; i + 1 < face_vertices.size(); ++i) {
            const std::array<uint32_t, 3> face{{face_vertices[0],
                                                face_vertices[i],
                                                face_vertices[i + 1]}};
            out.faces.push_back(face);
            out.face_uvs.push_back(face);
        }
    }
    if (!complete_uv) { out.uvs.clear(); out.face_uvs.clear(); }
    if (!complete_normals) out.normals.clear();
    return out;
}

// trimesh.exchange.off.load_off + geometry.triangulate_quads.  OFF is not
// named in the placement dialog's preferred filter, but Python's public model
// loader accepts it through trimesh (and the dialog exposes "All Files").
// Keep trimesh's grouping order: existing triangles, every quad's first half,
// every quad's second half, then polygon fans.
MeshArrays load_off(const std::vector<uint8_t>& bytes) {
    std::istringstream input(std::string(bytes.begin(), bytes.end()));
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const size_t last = line.find_last_not_of(" \t\r\n");
        lines.push_back(line.substr(first, last - first + 1));
    }
    if (lines.empty()) import_fail("OFF file is empty");

    std::string header;
    std::string counts_text;
    {
        std::istringstream words(lines[0]);
        words >> header;
        std::getline(words, counts_text);
    }
    if (header != "OFF" && header != "COFF")
        import_fail("not an OFF file");

    size_t line_index = 1;
    if (counts_text.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (line_index >= lines.size()) import_fail("OFF counts are missing");
        counts_text = lines[line_index++];
    }
    size_t vertex_count = 0, face_count = 0;
    {
        std::istringstream counts(counts_text);
        if (!(counts >> vertex_count >> face_count))
            import_fail("OFF counts are invalid");
    }
    if (line_index + vertex_count + face_count > lines.size())
        import_fail("OFF data is truncated");

    MeshArrays out;
    out.vertices.reserve(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
        std::istringstream values(lines[line_index++]);
        std::array<double, 3> vertex{};
        if (!(values >> vertex[0] >> vertex[1] >> vertex[2]))
            import_fail("OFF vertex is invalid");
        out.vertices.push_back(vertex);
    }

    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<std::array<uint32_t, 3>> quad_first;
    std::vector<std::array<uint32_t, 3>> quad_second;
    std::vector<std::array<uint32_t, 3>> polygon_fans;
    for (size_t i = 0; i < face_count; ++i) {
        std::istringstream values(lines[line_index++]);
        size_t count = 0;
        if (!(values >> count)) import_fail("OFF face is invalid");
        std::vector<uint32_t> indices(count);
        for (uint32_t& index : indices) {
            long long parsed = -1;
            if (!(values >> parsed) || parsed < 0
                || uint64_t(parsed) > uint64_t(std::numeric_limits<uint32_t>::max()))
                import_fail("OFF face is invalid");
            index = uint32_t(parsed);
        }
        if (count == 3) {
            triangles.push_back({indices[0], indices[1], indices[2]});
        } else if (count == 4) {
            quad_first.push_back({indices[0], indices[1], indices[2]});
            quad_second.push_back({indices[2], indices[3], indices[0]});
        } else if (count > 4) {
            for (size_t corner = 1; corner + 1 < count; ++corner)
                polygon_fans.push_back(
                    {indices[0], indices[corner], indices[corner + 1]});
        }
    }
    out.faces.reserve(triangles.size() + quad_first.size()
                      + quad_second.size() + polygon_fans.size());
    out.faces.insert(out.faces.end(), triangles.begin(), triangles.end());
    out.faces.insert(out.faces.end(), quad_first.begin(), quad_first.end());
    out.faces.insert(out.faces.end(), quad_second.begin(), quad_second.end());
    out.faces.insert(out.faces.end(), polygon_fans.begin(), polygon_fans.end());
    return out;
}

MeshArrays load_stl(const std::vector<uint8_t>& bytes) {
    MeshArrays out;
    const bool binary = bytes.size() >= 84
        && uint64_t(84) + uint64_t(le32(bytes.data() + 80)) * 50 <= bytes.size();
    if (binary) {
        const uint32_t count = le32(bytes.data() + 80);
        size_t offset = 84;
        for (uint32_t tri = 0; tri < count; ++tri, offset += 50) {
            const std::array<double, 3> normal{{f32(&bytes[offset]),
                                                f32(&bytes[offset + 4]),
                                                f32(&bytes[offset + 8])}};
            const uint32_t base = uint32_t(out.vertices.size());
            for (int corner = 0; corner < 3; ++corner) {
                const size_t p = offset + 12 + size_t(corner) * 12;
                out.vertices.push_back({f32(&bytes[p]), f32(&bytes[p + 4]),
                                        f32(&bytes[p + 8])});
                out.normals.push_back(normal);
            }
            out.faces.push_back({base, base + 1, base + 2});
        }
        return out;
    }

    const std::string text(bytes.begin(), bytes.end());
    std::istringstream input(text);
    std::string word;
    std::array<double, 3> normal{{0, 0, 0}};
    std::vector<std::array<double, 3>> triangle;
    while (input >> word) {
        if (word == "facet") {
            input >> word;
            if (word == "normal") input >> normal[0] >> normal[1] >> normal[2];
        } else if (word == "vertex") {
            std::array<double, 3> vertex{};
            if (input >> vertex[0] >> vertex[1] >> vertex[2])
                triangle.push_back(vertex);
            if (triangle.size() == 3) {
                const uint32_t base = uint32_t(out.vertices.size());
                out.vertices.insert(out.vertices.end(), triangle.begin(), triangle.end());
                out.normals.insert(out.normals.end(), 3, normal);
                out.faces.push_back({base, base + 1, base + 2});
                triangle.clear();
            }
        }
    }
    return out;
}

enum class PlyFormat { Ascii, Little, Big };
struct PlyProperty {
    bool list = false;
    std::string count_type, value_type, name;
};
struct PlyElement {
    std::string name;
    size_t count = 0;
    std::vector<PlyProperty> properties;
};

size_t ply_type_size(const std::string& type) {
    if (type == "char" || type == "int8" || type == "uchar" || type == "uint8") return 1;
    if (type == "short" || type == "int16" || type == "ushort" || type == "uint16") return 2;
    if (type == "int" || type == "int32" || type == "uint" || type == "uint32"
        || type == "float" || type == "float32") return 4;
    if (type == "double" || type == "float64") return 8;
    import_fail("PLY uses unsupported property type '" + type + "'");
}

uint64_t read_unsigned(const uint8_t* p, size_t size, bool big) {
    uint64_t value = 0;
    if (big) for (size_t i = 0; i < size; ++i) value = (value << 8) | p[i];
    else for (size_t i = 0; i < size; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}

double ply_binary_scalar(const std::vector<uint8_t>& bytes, size_t& offset,
                         const std::string& type, bool big) {
    const size_t size = ply_type_size(type);
    if (offset + size > bytes.size()) import_fail("PLY data is truncated");
    const uint64_t raw = read_unsigned(bytes.data() + offset, size, big);
    offset += size;
    if (type == "float" || type == "float32") {
        const uint32_t bits = uint32_t(raw); float value;
        std::memcpy(&value, &bits, 4); return value;
    }
    if (type == "double" || type == "float64") {
        uint64_t bits = raw; double value;
        std::memcpy(&value, &bits, 8); return value;
    }
    const bool sign = type == "char" || type == "int8" || type == "short"
                   || type == "int16" || type == "int" || type == "int32";
    if (!sign) return double(raw);
    if (size == 1) return int8_t(raw);
    if (size == 2) return int16_t(raw);
    return int32_t(raw);
}

MeshArrays load_ply(const std::vector<uint8_t>& bytes) {
    const std::string whole(bytes.begin(), bytes.end());
    const size_t marker = whole.find("end_header");
    if (marker == std::string::npos) import_fail("PLY header is missing end_header");
    size_t data_offset = marker + 10;
    if (data_offset < bytes.size() && bytes[data_offset] == '\r') ++data_offset;
    if (data_offset < bytes.size() && bytes[data_offset] == '\n') ++data_offset;
    std::istringstream header(whole.substr(0, marker));
    std::string line;
    PlyFormat format = PlyFormat::Ascii;
    std::vector<PlyElement> elements;
    PlyElement* current = nullptr;
    while (std::getline(header, line)) {
        std::istringstream words(line);
        std::string tag;
        words >> tag;
        if (tag == "format") {
            std::string value; words >> value;
            if (value == "binary_little_endian") format = PlyFormat::Little;
            else if (value == "binary_big_endian") format = PlyFormat::Big;
            else if (value != "ascii") import_fail("unsupported PLY format");
        } else if (tag == "element") {
            PlyElement element;
            words >> element.name >> element.count;
            elements.push_back(std::move(element)); current = &elements.back();
        } else if (tag == "property" && current) {
            PlyProperty property;
            std::string first; words >> first;
            if (first == "list") {
                property.list = true;
                words >> property.count_type >> property.value_type >> property.name;
            } else {
                property.value_type = first; words >> property.name;
            }
            current->properties.push_back(std::move(property));
        }
    }

    MeshArrays out;
    std::istringstream ascii(whole.substr(data_offset));
    size_t binary_offset = data_offset;
    const bool big = format == PlyFormat::Big;
    auto scalar = [&](const std::string& type) {
        if (format == PlyFormat::Ascii) {
            double value;
            if (!(ascii >> value)) import_fail("PLY data is truncated");
            return value;
        }
        return ply_binary_scalar(bytes, binary_offset, type, big);
    };

    for (const PlyElement& element : elements) {
        for (size_t row = 0; row < element.count; ++row) {
            std::map<std::string, double> values;
            std::vector<uint32_t> vertex_indices;
            for (const PlyProperty& property : element.properties) {
                if (!property.list) {
                    values[property.name] = scalar(property.value_type);
                } else {
                    const size_t count = size_t(std::max(0.0, scalar(property.count_type)));
                    std::vector<uint32_t> list;
                    list.reserve(count);
                    for (size_t i = 0; i < count; ++i)
                        list.push_back(uint32_t(scalar(property.value_type)));
                    if (property.name == "vertex_indices" || property.name == "vertex_index")
                        vertex_indices = std::move(list);
                }
            }
            if (element.name == "vertex") {
                out.vertices.push_back({values["x"], values["y"], values["z"]});
                if (values.count("nx") && values.count("ny") && values.count("nz"))
                    out.normals.push_back({values["nx"], values["ny"], values["nz"]});
                const char* uk = values.count("u") ? "u" : (values.count("s") ? "s" : nullptr);
                const char* vk = values.count("v") ? "v" : (values.count("t") ? "t" : nullptr);
                if (uk && vk) out.uvs.push_back({values[uk], values[vk]});
                if (values.count("red") && values.count("green") && values.count("blue"))
                    out.colors.push_back({int(values["red"]), int(values["green"]),
                                          int(values["blue"]),
                                          values.count("alpha") ? int(values["alpha"]) : 255});
            } else if (element.name == "face" && vertex_indices.size() >= 3) {
                for (size_t i = 1; i + 1 < vertex_indices.size(); ++i)
                    out.faces.push_back({vertex_indices[0], vertex_indices[i],
                                         vertex_indices[i + 1]});
            }
        }
    }
    if (out.normals.size() != out.vertices.size()) out.normals.clear();
    if (out.uvs.size() == out.vertices.size()) out.face_uvs = out.faces;
    else out.uvs.clear();
    if (out.colors.size() != out.vertices.size()) out.colors.clear();
    return out;
}

using FbxProperty = std::variant<int64_t, double, bool, std::string,
                                 std::vector<uint8_t>, std::vector<double>,
                                 std::vector<int64_t>>;
struct FbxNode {
    std::string name;
    std::vector<FbxProperty> properties;
    std::vector<FbxNode> children;
};

uint64_t fbx_u64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}

class DeflateBits {
public:
    DeflateBits(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    uint32_t read(unsigned count) {
        uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (byte_ >= size_) import_fail("FBX compressed array is truncated");
            result |= uint32_t((data_[byte_] >> bit_) & 1u) << i;
            if (++bit_ == 8) { bit_ = 0; ++byte_; }
        }
        return result;
    }
    void align_byte() {
        if (bit_ != 0) { bit_ = 0; ++byte_; }
    }
    size_t byte_offset() const { return byte_; }
    void advance(size_t count) {
        if (bit_ != 0 || byte_ + count > size_)
            import_fail("FBX compressed array is truncated");
        byte_ += count;
    }
private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0, byte_ = 0;
    unsigned bit_ = 0;
};

uint32_t reverse_code(uint32_t value, unsigned length) {
    uint32_t result = 0;
    for (unsigned i = 0; i < length; ++i) {
        result = (result << 1) | (value & 1u);
        value >>= 1;
    }
    return result;
}

struct DeflateHuffman {
    struct Code { uint32_t bits = 0; uint16_t symbol = 0; uint8_t length = 0; };
    std::vector<Code> codes;
    unsigned maximum = 0;

    explicit DeflateHuffman(const std::vector<uint8_t>& lengths) {
        std::array<uint32_t, 16> counts{}, next{};
        for (uint8_t length : lengths) {
            if (length > 15) import_fail("FBX DEFLATE code is invalid");
            if (length) { ++counts[length]; maximum = std::max(maximum, unsigned(length)); }
        }
        uint32_t code = 0;
        for (unsigned bits = 1; bits <= 15; ++bits) {
            code = (code + counts[bits - 1]) << 1;
            next[bits] = code;
        }
        for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const uint8_t length = lengths[symbol];
            if (!length) continue;
            codes.push_back({reverse_code(next[length]++, length),
                             uint16_t(symbol), length});
        }
    }

    uint32_t decode(DeflateBits& bits) const {
        uint32_t value = 0;
        for (unsigned length = 1; length <= maximum; ++length) {
            value |= bits.read(1) << (length - 1);
            for (const Code& code : codes)
                if (code.length == length && code.bits == value)
                    return code.symbol;
        }
        import_fail("FBX DEFLATE Huffman code is invalid");
    }
};

std::pair<DeflateHuffman, DeflateHuffman> deflate_tables(DeflateBits& bits,
                                                         uint32_t type) {
    if (type == 1) {
        std::vector<uint8_t> literal(288, 8), distance(32, 5);
        for (size_t i = 144; i < 256; ++i) literal[i] = 9;
        for (size_t i = 256; i < 280; ++i) literal[i] = 7;
        return {DeflateHuffman(literal), DeflateHuffman(distance)};
    }
    const uint32_t literal_count = bits.read(5) + 257;
    const uint32_t distance_count = bits.read(5) + 1;
    const uint32_t code_count = bits.read(4) + 4;
    static const uint8_t order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    std::vector<uint8_t> code_lengths(19, 0);
    for (uint32_t i = 0; i < code_count; ++i)
        code_lengths[order[i]] = uint8_t(bits.read(3));
    const DeflateHuffman code_table(code_lengths);
    std::vector<uint8_t> lengths;
    lengths.reserve(literal_count + distance_count);
    while (lengths.size() < literal_count + distance_count) {
        const uint32_t symbol = code_table.decode(bits);
        if (symbol <= 15) {
            lengths.push_back(uint8_t(symbol));
        } else if (symbol == 16) {
            if (lengths.empty()) import_fail("FBX DEFLATE repeat has no predecessor");
            const size_t repeat = bits.read(2) + 3;
            lengths.insert(lengths.end(), repeat, lengths.back());
        } else if (symbol == 17) {
            lengths.insert(lengths.end(), bits.read(3) + 3, uint8_t(0));
        } else if (symbol == 18) {
            lengths.insert(lengths.end(), bits.read(7) + 11, uint8_t(0));
        } else {
            import_fail("FBX DEFLATE code-length symbol is invalid");
        }
        if (lengths.size() > literal_count + distance_count)
            import_fail("FBX DEFLATE code-length table is invalid");
    }
    std::vector<uint8_t> literal(lengths.begin(),
                                 lengths.begin() + literal_count);
    std::vector<uint8_t> distance(lengths.begin() + literal_count,
                                  lengths.end());
    return {DeflateHuffman(literal), DeflateHuffman(distance)};
}

std::vector<uint8_t> inflate_zlib(const uint8_t* data, size_t size,
                                  size_t expected) {
    if (size < 6 || (data[0] & 0x0F) != 8
        || (uint32_t(data[0]) * 256 + data[1]) % 31 != 0
        || (data[1] & 0x20) != 0)
        import_fail("FBX compressed array has an invalid zlib header");
    DeflateBits bits(data + 2, size - 6);
    std::vector<uint8_t> output;
    output.reserve(expected);
    bool final = false;
    static const uint16_t length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,
        99,115,131,163,195,227,258};
    static const uint8_t length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t distance_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,
        12,12,13,13};
    while (!final) {
        final = bits.read(1) != 0;
        const uint32_t type = bits.read(2);
        if (type == 0) {
            bits.align_byte();
            const size_t at = bits.byte_offset();
            if (at + 4 > size - 6) import_fail("FBX stored block is truncated");
            const uint16_t length = le16(data + 2 + at);
            const uint16_t complement = le16(data + 2 + at + 2);
            if (uint16_t(~length) != complement)
                import_fail("FBX stored block length is invalid");
            bits.advance(4);
            const size_t content = bits.byte_offset();
            if (content + length > size - 6)
                import_fail("FBX stored block is truncated");
            output.insert(output.end(), data + 2 + content,
                          data + 2 + content + length);
            bits.advance(length);
            continue;
        }
        if (type == 3) import_fail("FBX DEFLATE block type is invalid");
        auto tables = deflate_tables(bits, type);
        while (true) {
            const uint32_t symbol = tables.first.decode(bits);
            if (symbol < 256) {
                output.push_back(uint8_t(symbol));
            } else if (symbol == 256) {
                break;
            } else {
                if (symbol < 257 || symbol > 285)
                    import_fail("FBX DEFLATE length symbol is invalid");
                const size_t li = symbol - 257;
                const size_t length = length_base[li] + bits.read(length_extra[li]);
                const uint32_t distance_symbol = tables.second.decode(bits);
                if (distance_symbol >= 30)
                    import_fail("FBX DEFLATE distance symbol is invalid");
                const size_t distance = distance_base[distance_symbol]
                                      + bits.read(distance_extra[distance_symbol]);
                if (distance == 0 || distance > output.size())
                    import_fail("FBX DEFLATE distance is out of range");
                for (size_t i = 0; i < length; ++i)
                    output.push_back(output[output.size() - distance]);
            }
            if (output.size() > expected)
                import_fail("FBX compressed array expands past its declared size");
        }
    }
    if (output.size() != expected)
        import_fail("FBX compressed array has the wrong expanded size");
    return output;
}

std::vector<uint8_t> fbx_array_bytes(const std::vector<uint8_t>& bytes,
                                     size_t& offset, uint32_t length,
                                     uint32_t encoding, uint32_t stored,
                                     size_t item_size) {
    if (offset + stored > bytes.size()) import_fail("FBX property is truncated");
    std::vector<uint8_t> result;
    if (encoding == 0) {
        result.assign(bytes.begin() + std::ptrdiff_t(offset),
                      bytes.begin() + std::ptrdiff_t(offset + stored));
    } else if (encoding == 1) {
        result = inflate_zlib(bytes.data() + offset, stored,
                              size_t(length) * item_size);
    } else {
        import_fail("FBX uses an unsupported array encoding");
    }
    offset += stored;
    if (result.size() < size_t(length) * item_size)
        import_fail("FBX array is truncated");
    return result;
}

FbxProperty read_fbx_property(const std::vector<uint8_t>& bytes,
                              size_t& offset) {
    if (offset >= bytes.size()) import_fail("FBX property is truncated");
    const char code = char(bytes[offset++]);
    auto require = [&](size_t count) {
        if (offset + count > bytes.size()) import_fail("FBX property is truncated");
    };
    switch (code) {
        case 'Y': {
            require(2); const int16_t value = int16_t(le16(bytes.data() + offset));
            offset += 2; return int64_t(value);
        }
        case 'C': require(1); return bool(bytes[offset++]);
        case 'I': {
            require(4); const int32_t value = int32_t(le32(bytes.data() + offset));
            offset += 4; return int64_t(value);
        }
        case 'F': {
            require(4); const float value = f32(bytes.data() + offset);
            offset += 4; return double(value);
        }
        case 'D': {
            require(8); const double value = f64(bytes.data() + offset);
            offset += 8; return value;
        }
        case 'L': {
            require(8); const int64_t value = int64_t(fbx_u64(bytes.data() + offset));
            offset += 8; return value;
        }
        case 'R': case 'S': {
            require(4); const uint32_t length = le32(bytes.data() + offset);
            offset += 4; require(length);
            if (code == 'S') {
                std::string result(reinterpret_cast<const char*>(bytes.data() + offset),
                                   length);
                offset += length; return result;
            }
            std::vector<uint8_t> result(
                bytes.begin() + std::ptrdiff_t(offset),
                bytes.begin() + std::ptrdiff_t(offset + length));
            offset += length; return result;
        }
        case 'f': case 'd': case 'l': case 'i': case 'b': case 'c': {
            require(12);
            const uint32_t length = le32(bytes.data() + offset);
            const uint32_t encoding = le32(bytes.data() + offset + 4);
            const uint32_t stored = le32(bytes.data() + offset + 8);
            offset += 12;
            const size_t item_size = (code == 'd' || code == 'l') ? 8
                                   : (code == 'f' || code == 'i') ? 4 : 1;
            const std::vector<uint8_t> raw = fbx_array_bytes(
                bytes, offset, length, encoding, stored, item_size);
            if (code == 'f' || code == 'd') {
                std::vector<double> values;
                values.reserve(length);
                for (uint32_t i = 0; i < length; ++i)
                    values.push_back(code == 'f' ? double(f32(raw.data() + size_t(i) * 4))
                                                  : f64(raw.data() + size_t(i) * 8));
                return values;
            }
            if (code == 'i' || code == 'l') {
                std::vector<int64_t> values;
                values.reserve(length);
                for (uint32_t i = 0; i < length; ++i)
                    values.push_back(code == 'i'
                        ? int64_t(int32_t(le32(raw.data() + size_t(i) * 4)))
                        : int64_t(fbx_u64(raw.data() + size_t(i) * 8)));
                return values;
            }
            return raw;
        }
        default:
            import_fail(std::string("Unsupported FBX property type '") + code + "'");
    }
}

bool read_fbx_node(const std::vector<uint8_t>& bytes, size_t offset,
                   size_t limit, bool large, FbxNode& node, size_t& next) {
    const size_t start = offset;
    const size_t sentinel = large ? 25 : 13;
    const size_t header = sentinel;
    if (offset + header > limit || offset + header > bytes.size()) {
        next = limit; return false;
    }
    uint64_t end = 0, property_count = 0, property_length = 0;
    if (large) {
        end = fbx_u64(bytes.data() + offset);
        property_count = fbx_u64(bytes.data() + offset + 8);
        property_length = fbx_u64(bytes.data() + offset + 16);
        offset += 24;
    } else {
        end = le32(bytes.data() + offset);
        property_count = le32(bytes.data() + offset + 4);
        property_length = le32(bytes.data() + offset + 8);
        offset += 12;
    }
    const uint8_t name_length = bytes[offset++];
    if (end == 0) { next = start + sentinel; return false; }
    if (end > bytes.size() || end > limit || offset + name_length > end)
        import_fail("FBX node has invalid bounds");
    node.name.assign(reinterpret_cast<const char*>(bytes.data() + offset),
                     name_length);
    offset += name_length;
    const size_t property_end = offset + size_t(property_length);
    if (property_end > end) import_fail("FBX property list has invalid bounds");
    node.properties.reserve(size_t(property_count));
    for (uint64_t i = 0; i < property_count; ++i)
        node.properties.push_back(read_fbx_property(bytes, offset));
    offset = std::max(offset, property_end);
    const size_t child_limit = size_t(end) >= sentinel ? size_t(end) - sentinel : 0;
    while (offset < child_limit) {
        FbxNode child;
        size_t child_next = offset;
        if (!read_fbx_node(bytes, offset, child_limit, large, child, child_next)) {
            offset = child_next; break;
        }
        if (child_next <= offset) import_fail("FBX child node did not advance");
        node.children.push_back(std::move(child));
        offset = child_next;
    }
    next = size_t(end);
    return true;
}

void walk_fbx_nodes(const std::vector<FbxNode>& nodes,
                    std::vector<const FbxNode*>& out) {
    for (const FbxNode& node : nodes) {
        out.push_back(&node);
        walk_fbx_nodes(node.children, out);
    }
}

const FbxNode* fbx_child(const FbxNode& node, const std::string& name) {
    for (const FbxNode& child : node.children)
        if (child.name == name) return &child;
    return nullptr;
}

std::vector<const FbxNode*> fbx_children(const FbxNode& node,
                                         const std::string& name) {
    std::vector<const FbxNode*> result;
    for (const FbxNode& child : node.children)
        if (child.name == name) result.push_back(&child);
    return result;
}

int64_t fbx_integer(const FbxNode& node, size_t index, int64_t fallback = 0) {
    if (index >= node.properties.size()) return fallback;
    if (const auto* value = std::get_if<int64_t>(&node.properties[index]))
        return *value;
    if (const auto* value = std::get_if<double>(&node.properties[index]))
        return int64_t(*value);
    return fallback;
}

double fbx_number(const FbxProperty& property) {
    if (const auto* value = std::get_if<double>(&property)) return *value;
    if (const auto* value = std::get_if<int64_t>(&property)) return double(*value);
    if (const auto* value = std::get_if<bool>(&property)) return *value ? 1.0 : 0.0;
    import_fail("FBX property is not numeric");
}

const std::vector<double>* fbx_double_array(const FbxNode& node,
                                            const std::string& name) {
    const FbxNode* child = fbx_child(node, name);
    if (!child || child->properties.empty()) return nullptr;
    return std::get_if<std::vector<double>>(&child->properties[0]);
}

const std::vector<int64_t>* fbx_integer_array(const FbxNode& node,
                                              const std::string& name) {
    const FbxNode* child = fbx_child(node, name);
    if (!child || child->properties.empty()) return nullptr;
    return std::get_if<std::vector<int64_t>>(&child->properties[0]);
}

struct FbxTransform {
    std::array<double, 3> translation{{0, 0, 0}};
    std::array<double, 3> rotation{{0, 0, 0}};
    std::array<double, 3> scaling{{1, 1, 1}};
};

void apply_fbx_transform(std::vector<std::array<double, 3>>& vertices,
                         const FbxTransform& transform) {
    const float degree_to_radian = float(3.14159265358979323846 / 180.0);
    const float rx = float(transform.rotation[0]) * degree_to_radian;
    const float ry = float(transform.rotation[1]) * degree_to_radian;
    const float rz = float(transform.rotation[2]) * degree_to_radian;
    const float cx = float(std::cos(double(rx))), sx = float(std::sin(double(rx)));
    const float cy = float(std::cos(double(ry))), sy = float(std::sin(double(ry)));
    const float cz = float(std::cos(double(rz))), sz = float(std::sin(double(rz)));
    const float rotation[9] = {
        cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
        sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
        -sy, cy * sx, cy * cx};
    for (auto& vertex : vertices) {
        const float x = float(vertex[0]) * float(transform.scaling[0]);
        const float y = float(vertex[1]) * float(transform.scaling[1]);
        const float z = float(vertex[2]) * float(transform.scaling[2]);
        vertex = {
            double(float(rotation[0] * x + rotation[1] * y + rotation[2] * z
                         + float(transform.translation[0]))),
            double(float(rotation[3] * x + rotation[4] * y + rotation[5] * z
                         + float(transform.translation[1]))),
            double(float(rotation[6] * x + rotation[7] * y + rotation[8] * z
                         + float(transform.translation[2])))};
    }
}

MeshArrays fbx_geometry(const FbxNode& geometry) {
    const std::vector<double>* vertex_values = fbx_double_array(geometry, "Vertices");
    const std::vector<int64_t>* index_values =
        fbx_integer_array(geometry, "PolygonVertexIndex");
    if (!vertex_values || !index_values || vertex_values->size() % 3 != 0)
        import_fail("FBX vertices array is not divisible by 3");
    MeshArrays out;
    for (size_t i = 0; i < vertex_values->size(); i += 3)
        out.vertices.push_back({double(float((*vertex_values)[i])),
                                double(float((*vertex_values)[i + 1])),
                                double(float((*vertex_values)[i + 2]))});

    const std::vector<double>* uv_values = nullptr;
    const std::vector<int64_t>* uv_indices = nullptr;
    for (const FbxNode* layer : fbx_children(geometry, "LayerElementUV")) {
        uv_values = fbx_double_array(*layer, "UV");
        if (uv_values) {
            uv_indices = fbx_integer_array(*layer, "UVIndex");
            break;
        }
    }
    if (uv_values && uv_values->size() % 2 == 0)
        for (size_t i = 0; i < uv_values->size(); i += 2)
            out.uvs.push_back({double(float((*uv_values)[i])),
                               double(float((*uv_values)[i + 1]))});

    for (const FbxNode* layer : fbx_children(geometry, "LayerElementNormal")) {
        const std::vector<double>* normal_values = fbx_double_array(*layer, "Normals");
        if (!normal_values || normal_values->size() != out.vertices.size() * 3)
            continue;
        for (size_t i = 0; i < normal_values->size(); i += 3)
            out.normals.push_back({double(float((*normal_values)[i])),
                                   double(float((*normal_values)[i + 1])),
                                   double(float((*normal_values)[i + 2]))});
        break;
    }

    std::vector<uint32_t> polygon, polygon_uvs;
    size_t corner = 0;
    for (int64_t raw : *index_values) {
        const bool end = raw < 0;
        const int64_t vertex = end ? -raw - 1 : raw;
        if (vertex < 0 || vertex >= int64_t(out.vertices.size()))
            import_fail("FBX has an invalid polygon vertex index");
        polygon.push_back(uint32_t(vertex));
        if (uv_indices && corner < uv_indices->size())
            polygon_uvs.push_back(uint32_t((*uv_indices)[corner]));
        else if (!out.uvs.empty() && out.uvs.size() == out.vertices.size())
            polygon_uvs.push_back(uint32_t(vertex));
        else
            polygon_uvs.push_back(0);
        ++corner;
        if (!end) continue;
        for (size_t i = 1; i + 1 < polygon.size(); ++i) {
            out.faces.push_back({polygon[0], polygon[i], polygon[i + 1]});
            out.face_uvs.push_back({polygon_uvs[0], polygon_uvs[i],
                                    polygon_uvs[i + 1]});
        }
        polygon.clear(); polygon_uvs.clear();
    }
    if (out.faces.empty()) import_fail("FBX mesh has no polygon faces");
    return out;
}

MeshArrays load_fbx(const std::vector<uint8_t>& bytes,
                    const std::string& label) {
    static const uint8_t signature[] = {
        'K','a','y','d','a','r','a',' ','F','B','X',' ','B','i','n','a','r','y',' ',' ',0,0x1a,0};
    if (bytes.size() < 27 || std::memcmp(bytes.data(), signature, sizeof signature) != 0)
        import_fail("only binary FBX files are supported by the built-in importer");
    const uint32_t version = le32(bytes.data() + 23);
    const bool large = version >= 7500;
    std::vector<FbxNode> roots;
    size_t offset = 27;
    while (offset < bytes.size()) {
        FbxNode node;
        size_t next = offset;
        if (!read_fbx_node(bytes, offset, bytes.size(), large, node, next)) break;
        roots.push_back(std::move(node));
        offset = next;
    }
    std::vector<const FbxNode*> nodes;
    walk_fbx_nodes(roots, nodes);

    std::map<int64_t, std::vector<int64_t>> connections;
    std::map<int64_t, FbxTransform> transforms;
    for (const FbxNode* node : nodes) {
        if (node->name == "Connections") {
            for (const FbxNode* connection : fbx_children(*node, "C")) {
                if (connection->properties.size() < 3) continue;
                const auto* relation =
                    std::get_if<std::string>(&connection->properties[0]);
                if (!relation || (*relation != "OO" && *relation != "oo")) continue;
                connections[fbx_integer(*connection, 1)].push_back(
                    fbx_integer(*connection, 2));
            }
        } else if (node->name == "Model" && !node->properties.empty()) {
            FbxTransform transform;
            for (const FbxNode* properties : fbx_children(*node, "Properties70"))
                for (const FbxNode* property : fbx_children(*properties, "P")) {
                    if (property->properties.size() < 7) continue;
                    const auto* name =
                        std::get_if<std::string>(&property->properties[0]);
                    if (!name) continue;
                    std::array<double, 3> values{{
                        fbx_number(property->properties[property->properties.size() - 3]),
                        fbx_number(property->properties[property->properties.size() - 2]),
                        fbx_number(property->properties[property->properties.size() - 1])}};
                    if (*name == "Lcl Translation") transform.translation = values;
                    else if (*name == "Lcl Rotation") transform.rotation = values;
                    else if (*name == "Lcl Scaling") transform.scaling = values;
                }
            transforms[fbx_integer(*node, 0)] = transform;
        }
    }

    MeshArrays result;
    bool all_normals = true, all_uvs = true;
    size_t geometry_count = 0;
    for (const FbxNode* node : nodes) {
        if (node->name != "Geometry" || !fbx_double_array(*node, "Vertices"))
            continue;
        ++geometry_count;
        MeshArrays part = fbx_geometry(*node);
        const int64_t geometry_id = fbx_integer(*node, 0);
        auto linked = connections.find(geometry_id);
        if (linked != connections.end() && !linked->second.empty()) {
            auto transform = transforms.find(linked->second[0]);
            if (transform != transforms.end())
                apply_fbx_transform(part.vertices, transform->second);
        }
        const uint32_t vertex_offset = uint32_t(result.vertices.size());
        const uint32_t uv_offset = uint32_t(result.uvs.size());
        result.vertices.insert(result.vertices.end(), part.vertices.begin(), part.vertices.end());
        for (const auto& face : part.faces)
            result.faces.push_back({face[0] + vertex_offset, face[1] + vertex_offset,
                                    face[2] + vertex_offset});
        if (part.normals.size() == part.vertices.size())
            result.normals.insert(result.normals.end(), part.normals.begin(),
                                  part.normals.end());
        else all_normals = false;
        if (!part.uvs.empty() && part.face_uvs.size() == part.faces.size()) {
            result.uvs.insert(result.uvs.end(), part.uvs.begin(), part.uvs.end());
            for (const auto& face : part.face_uvs)
                result.face_uvs.push_back({face[0] + uv_offset, face[1] + uv_offset,
                                           face[2] + uv_offset});
        } else all_uvs = false;
    }
    if (geometry_count == 0)
        import_fail("FBX " + label + " contains no mesh geometry");
    if (result.vertices.empty() || result.faces.empty())
        import_fail("FBX " + label + " contains no usable mesh geometry");
    if (!all_normals || result.normals.size() != result.vertices.size())
        result.normals.clear();
    if (!all_uvs || result.face_uvs.size() != result.faces.size()) {
        result.uvs.clear(); result.face_uvs.clear();
    }
    return result;
}

void compute_normals(MeshArrays& mesh) {
    std::vector<std::array<float, 3>> normals(mesh.vertices.size(), {0, 0, 0});
    for (const auto& face : mesh.faces) {
        const auto& ad = mesh.vertices[face[0]];
        const auto& bd = mesh.vertices[face[1]];
        const auto& cd = mesh.vertices[face[2]];
        const float ax = float(ad[0]), ay = float(ad[1]), az = float(ad[2]);
        const float bx = float(bd[0]), by = float(bd[1]), bz = float(bd[2]);
        const float cx = float(cd[0]), cy = float(cd[1]), cz = float(cd[2]);
        const float ux = bx - ax, uy = by - ay, uz = bz - az;
        const float vx = cx - ax, vy = cy - ay, vz = cz - az;
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) {
            nx /= len; ny /= len; nz /= len;
            for (uint32_t index : face) {
                normals[index][0] += nx;
                normals[index][1] += ny;
                normals[index][2] += nz;
            }
        }
    }
    mesh.normals.clear();
    mesh.normals.reserve(normals.size());
    for (auto normal : normals) {
        const float len = std::sqrt(normal[0] * normal[0]
                                  + normal[1] * normal[1]
                                  + normal[2] * normal[2]);
        if (len > 1e-8f)
            mesh.normals.push_back({double(normal[0] / len),
                                    double(normal[1] / len),
                                    double(normal[2] / len)});
        else
            mesh.normals.push_back({0, 0, 1});
    }
}

PlacerGeo finalize(MeshArrays mesh, const Vec3& scale, uint32_t material_key,
                   const std::string& label, bool import_colors) {
    if (mesh.vertices.empty()) import_fail("Model " + label + " has no vertices");
    if (mesh.faces.empty()) import_fail("Model " + label + " has no triangular faces");
    if (mesh.vertices.size() > 65535)
        import_fail("Model " + label + " has " + std::to_string(mesh.vertices.size())
                    + " vertices; this simple GEO writer supports at most 65,535.");
    for (const auto& face : mesh.faces)
        for (uint32_t index : face)
            if (index >= mesh.vertices.size())
                import_fail("Model " + label + " has invalid face indices");

    const Vec3 dimensions = vec3_min(scale, 0.001);
    for (auto& vertex : mesh.vertices)
        for (size_t axis = 0; axis < 3; ++axis)
            vertex[axis] = double(float(float(vertex[axis]) * float(dimensions[axis])));
    if (mesh.normals.size() != mesh.vertices.size()) compute_normals(mesh);
    else for (auto& normal : mesh.normals)
        for (double& component : normal) component = double(float(component));

    if (mesh.uvs.empty()) {
        mesh.uvs.assign(mesh.vertices.size(), {0, 0});
        mesh.face_uvs = mesh.faces;
    } else {
        if (mesh.uvs.size() > 65535)
            import_fail("Model " + label + " has too many UV coordinates");
        for (auto& uv : mesh.uvs)
            for (double& component : uv) component = double(float(component));
        bool valid = mesh.face_uvs.size() == mesh.faces.size();
        if (valid)
            for (const auto& face : mesh.face_uvs)
                for (uint32_t index : face)
                    if (index >= mesh.uvs.size()) valid = false;
        if (!valid) {
            if (mesh.uvs.size() == mesh.vertices.size()) mesh.face_uvs = mesh.faces;
            else mesh.face_uvs.assign(mesh.faces.size(), {0, 0, 0});
        }
    }

    PlacerGeo result;
    result.vertices = std::move(mesh.vertices);
    result.normals = std::move(mesh.normals);
    result.uvs = std::move(mesh.uvs);
    result.faces = std::move(mesh.faces);
    result.face_uvs = std::move(mesh.face_uvs);
    result.material_key = material_key;
    result.elements = {{uint32_t(result.faces.size()), material_key}};
    if (import_colors && mesh.colors.size() == result.vertices.size()) {
        result.has_colors = true;
        result.colors = std::move(mesh.colors);
        for (auto& color : result.colors)
            for (int& channel : color) channel &= 0xFF;
    }
    return result;
}

}  // namespace

PlacerGeo load_model_geometry(const std::string& model_path, const Vec3& scale,
                              uint32_t material_key,
                              bool import_vertex_colors) {
    const std::filesystem::path path = std::filesystem::u8path(model_path);
    std::error_code error;
    if (model_path.empty() || !std::filesystem::is_regular_file(path, error))
        import_fail("Model file was not found: " + model_path);
    MeshArrays arrays;
    const std::string cache_path = normalized_absolute_path(path);
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error) import_fail("Model file was not found: " + model_path);
    const std::filesystem::file_time_type modified =
        std::filesystem::last_write_time(path, error);
    if (error) import_fail("Model file was not found: " + model_path);
    bool cache_hit = false;
    {
        std::lock_guard<std::mutex> lock(model_cache_mutex);
        for (size_t i = 0; i < model_cache.size(); ++i) {
            const CacheEntry& entry = model_cache[i];
            if (entry.normalized_path != cache_path || entry.size != size
                || entry.modified != modified)
                continue;
            arrays = entry.arrays;
            CacheEntry recent = std::move(model_cache[i]);
            model_cache.erase(model_cache.begin() + std::ptrdiff_t(i));
            model_cache.push_back(std::move(recent));
            cache_hit = true;
            break;
        }
    }
    if (!cache_hit) {
        const std::vector<uint8_t> bytes = read_file(model_path);
        if (bytes.empty())
            import_fail("Could not import model " + filename(model_path)
                        + ": file is empty");
        const std::string extension = lower_extension(model_path);
        try {
            if (extension == ".fbx") arrays = load_fbx(bytes, filename(model_path));
            else if (extension == ".glb") arrays = load_glb(bytes);
            else if (extension == ".gltf") arrays = load_text_gltf(model_path, bytes);
            else if (extension == ".dae") arrays = load_dae(bytes);
            else if (extension == ".obj")
                arrays = load_obj(std::string(bytes.begin(), bytes.end()));
            else if (extension == ".off") arrays = load_off(bytes);
            else if (extension == ".stl") arrays = load_stl(bytes);
            else if (extension == ".ply") arrays = load_ply(bytes);
            else
                import_fail("unsupported model format '" + extension
                            + "' (native import currently supports DAE, FBX, glTF/GLB, OBJ, OFF, STL, and PLY)");
        } catch (const std::runtime_error& exception) {
            const std::string message = exception.what();
            if (message.rfind("Model ", 0) == 0
                || message.rfind("Model file was not found", 0) == 0
                || message.rfind("unsupported model format", 0) == 0)
                throw;
            import_fail("Could not import model " + filename(model_path) + ": "
                        + message);
        }
        {
            std::lock_guard<std::mutex> lock(model_cache_mutex);
            model_cache.push_back({cache_path, size, modified, arrays});
            while (model_cache.size() > MODEL_GEOMETRY_CACHE_LIMIT)
                model_cache.erase(model_cache.begin());
        }
    }
    return finalize(std::move(arrays), scale, material_key,
                    filename(model_path), import_vertex_colors);
}

void clear_model_geometry_cache() {
    std::lock_guard<std::mutex> lock(model_cache_mutex);
    model_cache.clear();
}

}  // namespace placer
}  // namespace jade
