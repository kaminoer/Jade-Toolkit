#include "jade/Prefab.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace jade {
namespace {

bool supported_kind(const std::string& kind) {
    return kind == "cube" || kind == "sphere" || kind == "cylinder"
        || kind == "model";
}

bool supported_collision_profile(const std::string& profile) {
    return profile == "none" || profile == "simple_box";
}

std::string lower(std::string value) {
    for (char& c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = char(u - 'A' + 'a');
    }
    return value;
}

std::string string_or(const json::Value* value, const std::string& fallback) {
    if (!value) return fallback;
    if (value->is_str()) return value->str;
    if (value->type == json::Value::Type::Null) return "None";
    if (value->type == json::Value::Type::Bool)
        return value->b ? "True" : "False";
    if (value->is_num()) return json::dump(*value);
    return json::dump(*value);
}

bool python_truthy(const json::Value* value, bool fallback) {
    if (!value) return fallback;
    switch (value->type) {
        case json::Value::Type::Null: return false;
        case json::Value::Type::Bool: return value->b;
        case json::Value::Type::Number: return value->num != 0.0;
        case json::Value::Type::String: return !value->str.empty();
        case json::Value::Type::Array: return !value->arr.empty();
        case json::Value::Type::Object: return !value->obj.empty();
    }
    return false;
}

uint32_t parse_hex_or_int(const json::Value& value) {
    if (value.type == json::Value::Type::Bool) return value.b ? 1u : 0u;
    if (value.is_num()) {
        if (!std::isfinite(value.num) || std::floor(value.num) != value.num)
            throw PrefabError("prefab malformed: material_key must be an int or None");
        double wrapped = std::fmod(value.num, 4294967296.0);
        if (wrapped < 0.0) wrapped += 4294967296.0;
        return uint32_t(wrapped);
    }
    if (value.is_str()) {
        try {
            size_t used = 0;
            const unsigned long long n = std::stoull(value.str, &used, 16);
            if (used != value.str.size()) throw std::invalid_argument("trailing");
            return uint32_t(n & 0xFFFFFFFFull);
        } catch (const std::exception&) {
            throw PrefabError("prefab malformed: invalid material_key");
        }
    }
    throw PrefabError("prefab malformed: material_key must be an int or None");
}

std::array<double, 3> vec3_or(const json::Value* value,
                              const std::array<double, 3>& fallback,
                              const char* field) {
    if (!value) return fallback;
    if (!value->is_arr() || value->arr.size() != 3)
        throw PrefabError(std::string(field) + " must be a 3-tuple of numbers");
    std::array<double, 3> result{};
    for (size_t i = 0; i < 3; ++i) {
        const json::Value& v = value->arr[i];
        if (v.is_num()) result[i] = v.num;
        else if (v.type == json::Value::Type::Bool) result[i] = v.b ? 1.0 : 0.0;
        else throw PrefabError(std::string(field) + " must be a 3-tuple of numbers");
    }
    return result;
}

json::Value vec3_json(const std::array<double, 3>& value) {
    json::Value out = json::make_arr();
    for (double component : value) out.arr.push_back(json::make_num(component));
    return out;
}

std::optional<uint32_t> optional_key(const json::Value* value) {
    if (!value || value->type == json::Value::Type::Null
        || (value->is_str() && value->str.empty()))
        return std::nullopt;
    return parse_hex_or_int(*value);
}

std::string key_hex(uint32_t key) {
    std::ostringstream text;
    text << "0x" << std::hex << std::nouppercase << std::setw(8)
         << std::setfill('0') << key;
    return text.str();
}

void require_valid(const Prefab& prefab) {
    const std::vector<std::string> errors = prefab.validate();
    if (errors.empty()) return;
    std::string joined;
    for (const std::string& error : errors) {
        if (!joined.empty()) joined += "; ";
        joined += error;
    }
    throw PrefabError(joined);
}

}  // namespace

std::string prefab_iso_now() {
    const std::time_t value = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char text[24]{};
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

std::vector<std::string> Prefab::validate() const {
    std::vector<std::string> errors;
    if (name.empty()) errors.push_back("name must be a non-empty string");
    if (!supported_kind(kind))
        errors.push_back("kind must be one of ('cube', 'sphere', 'cylinder', "
                         "'model'), got '" + kind + "'");
    if (std::any_of(size.begin(), size.end(),
                    [](double v) { return !std::isfinite(v) || v <= 0.0; }))
        errors.push_back("size components must be > 0");
    if (!supported_collision_profile(collision_profile))
        errors.push_back("collision_profile must be one of ('none', "
                         "'simple_box'), got '" + collision_profile + "'");
    if (kind == "model" && model_asset.empty())
        errors.push_back("model prefabs must set model_asset (asset:<hash> or "
                         "absolute path)");
    if (kind != "model" && !model_asset.empty())
        errors.push_back("model_asset is only meaningful for kind='model' "
                         "(got kind='" + kind + "')");
    if (collision && collision_profile == "none")
        errors.push_back("collision=True but collision_profile='none' — pick a "
                         "profile or set collision=False");
    return errors;
}

json::Value Prefab::to_json() const {
    json::Value out = json::make_obj();
    out.obj["format"] = json::make_str(PREFAB_FORMAT);
    out.obj["version"] = json::make_num(PREFAB_FORMAT_VERSION);
    out.obj["name"] = json::make_str(name);
    out.obj["kind"] = json::make_str(kind);
    out.obj["size"] = vec3_json(size);
    out.obj["material_key"] = material_key
        ? json::make_str(key_hex(*material_key)) : json::Value{};
    out.obj["collision"] = json::make_bool(collision);
    out.obj["collision_profile"] = json::make_str(collision_profile);
    out.obj["created"] = json::make_str(created);
    if (kind == "model") out.obj["model_asset"] = json::make_str(model_asset);
    if (!description.empty()) out.obj["description"] = json::make_str(description);
    return out;
}

Prefab Prefab::from_json(const json::Value& value) {
    if (!value.is_obj()) throw PrefabError("prefab data must be a dict");
    const json::Value* format = value.find("format");
    if (!format || !format->is_str() || format->str != PREFAB_FORMAT)
        throw PrefabError("not a jade-prefab document (format mismatch)");
    const json::Value* version = value.find("version");
    const bool version_ok = version
        && ((version->is_num() && version->num == double(PREFAB_FORMAT_VERSION))
            || (version->type == json::Value::Type::Bool && version->b));
    if (!version_ok)
        throw PrefabError("unsupported prefab version; this build understands "
                          "version 1");

    Prefab out;
    out.name = string_or(value.find("name"), "");
    out.kind = string_or(value.find("kind"), "cube");
    out.size = vec3_or(value.find("size"), {{1.0, 1.0, 1.0}}, "size");
    out.material_key = optional_key(value.find("material_key"));
    out.collision = python_truthy(value.find("collision"), true);
    out.collision_profile = string_or(value.find("collision_profile"),
                                      "simple_box");
    out.model_asset = string_or(value.find("model_asset"), "");
    out.description = string_or(value.find("description"), "");
    out.created = string_or(value.find("created"), prefab_iso_now());
    require_valid(out);
    return out;
}

void Prefab::save(std::string path) const {
    if (!is_prefab_path(path)) path += PREFAB_EXT;
    std::error_code ec;
    const std::filesystem::path fs_path = std::filesystem::u8path(path);
    const std::filesystem::path parent = fs_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) throw PrefabError("could not create prefab directory: "
                                  + ec.message());
    }
    std::ofstream file(fs_path, std::ios::binary | std::ios::trunc);
    if (!file) throw PrefabError("could not create prefab file: " + path);
    const std::string text = json::dump(to_json(), 2);
    file.write(text.data(), std::streamsize(text.size()));
    file.put('\n');
    if (!file) throw PrefabError("could not write prefab file: " + path);
}

Prefab Prefab::load(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) throw PrefabError("could not open prefab file: " + path);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    try {
        return from_json(json::parse_strict(text));
    } catch (const PrefabError&) {
        throw;
    } catch (const std::exception& error) {
        throw PrefabError(std::string("prefab malformed: ") + error.what());
    }
}

bool is_prefab_path(const std::string& path) {
    if (path.size() < std::char_traits<char>::length(PREFAB_EXT)) return false;
    const std::string suffix = path.substr(
        path.size() - std::char_traits<char>::length(PREFAB_EXT));
    return lower(suffix) == PREFAB_EXT;
}

json::Value prefab_to_add_object(const Prefab& prefab, uint32_t entry_key,
                                 const std::array<double, 3>& position,
                                 const std::string& name, const std::string& id,
                                 bool enabled, const std::string& label,
                                 const std::string& created) {
    require_valid(prefab);
    json::Value operation = json::make_obj();
    operation.obj["id"] = json::make_str(id);
    operation.obj["op"] = json::make_str("add_object");
    operation.obj["enabled"] = json::make_bool(enabled);
    operation.obj["label"] = json::make_str(label);
    operation.obj["created"] = json::make_str(created.empty()
                                               ? prefab_iso_now() : created);
    json::Value target = json::make_obj();
    target.obj["entry_key"] = json::make_str(key_hex(entry_key));
    operation.obj["target"] = std::move(target);

    json::Value params = json::make_obj();
    params.obj["kind"] = json::make_str(prefab.kind);
    params.obj["name"] = json::make_str(name.empty() ? prefab.name : name);
    params.obj["position"] = vec3_json(position);
    params.obj["size"] = vec3_json(prefab.size);
    params.obj["collision"] = json::make_bool(prefab.collision);
    if (prefab.material_key)
        params.obj["material_key"] = json::make_str(key_hex(*prefab.material_key));
    if (prefab.kind == "model" && !prefab.model_asset.empty()) {
        if (prefab.model_asset.rfind("asset:", 0) == 0)
            params.obj["source"] = json::make_str(prefab.model_asset);
        else
            params.obj["model_path"] = json::make_str(prefab.model_asset);
    }
    operation.obj["params"] = std::move(params);
    return operation;
}

Prefab prefab_from_add_object(const json::Value& operation,
                              const std::string& description) {
    if (!operation.is_obj()) throw PrefabError("add_object operation must be an object");
    const json::Value* op = operation.find("op");
    if (op && (!op->is_str() || op->str != "add_object"))
        throw PrefabError("operation is not add_object");
    const json::Value* params = operation.find("params");
    if (!params || !params->is_obj())
        throw PrefabError("add_object operation has no params object");

    const std::string kind = lower(string_or(params->find("kind"), "cube"));
    if (kind == "clone")
        throw PrefabError("Clone ops are zone-local and cannot be saved as "
                          "prefabs; save a primitive or imported-model "
                          "placement instead");
    Prefab prefab;
    prefab.kind = kind;
    prefab.name = string_or(params->find("name"), "");
    if (prefab.name.empty()) prefab.name = "JadePlaced_" + kind;
    prefab.size = vec3_or(params->find("size"), {{1.0, 1.0, 1.0}}, "size");
    prefab.material_key = optional_key(params->find("material_key"));
    prefab.collision = python_truthy(params->find("collision"), true);
    prefab.collision_profile = prefab.collision ? "simple_box" : "none";
    prefab.description = description;
    if (kind == "model") {
        prefab.model_asset = string_or(params->find("source"), "");
        if (prefab.model_asset.empty())
            prefab.model_asset = string_or(params->find("model_path"), "");
    }
    require_valid(prefab);
    return prefab;
}

}  // namespace jade
