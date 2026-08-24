// Prefab.hpp - portable .jprefab game-object recipes.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "jade/Json.hpp"

namespace jade {

inline constexpr const char* PREFAB_FORMAT = "jade-prefab";
inline constexpr uint32_t PREFAB_FORMAT_VERSION = 1;
inline constexpr const char* PREFAB_EXT = ".jprefab";

class PrefabError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string prefab_iso_now();

struct Prefab {
    std::string name;
    std::string kind;
    std::array<double, 3> size{{1.0, 1.0, 1.0}};
    std::optional<uint32_t> material_key;
    bool collision = true;
    std::string collision_profile = "simple_box";
    std::string model_asset;
    std::string description;
    std::string created = prefab_iso_now();

    // Mirrors core/prefab.py::Prefab.validate. Empty means valid.
    std::vector<std::string> validate() const;

    json::Value to_json() const;
    static Prefab from_json(const json::Value& value);

    // save() appends .jprefab case-insensitively when it is absent and creates
    // the parent directory, matching the Python data model.
    void save(std::string path) const;
    static Prefab load(const std::string& path);
};

bool is_prefab_path(const std::string& path);

// Raw-JSON bridge for the native project's add_object representation. The
// per-placement entry, position, and optional name are supplied by the caller;
// portable prefab fields become the same target/params fields emitted by
// Python AddObject.from_prefab(...).to_dict().
json::Value prefab_to_add_object(
    const Prefab& prefab, uint32_t entry_key,
    const std::array<double, 3>& position = {{0.0, 0.0, 0.0}},
    const std::string& name = {}, const std::string& id = {},
    bool enabled = true, const std::string& label = {},
    const std::string& created = {});

// Extract the portable subset of an add_object JSON operation. Clone sources
// are zone-local and are rejected, exactly like AddObject.to_prefab().
Prefab prefab_from_add_object(const json::Value& operation,
                              const std::string& description = {});

}  // namespace jade
