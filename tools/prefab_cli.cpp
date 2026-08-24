// prefab_cli - oracle bridge for .jprefab and AddObject conversion.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "jade/Json.hpp"
#include "jade/Prefab.hpp"

namespace {

std::string read_text(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) throw std::runtime_error("could not open " + path);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

void write_json(const std::string& path, const jade::json::Value& value) {
    std::ofstream file(std::filesystem::u8path(path),
                       std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("could not create " + path);
    const std::string text = jade::json::dump(value, 2);
    file.write(text.data(), std::streamsize(text.size()));
    file.put('\n');
    if (!file) throw std::runtime_error("could not write " + path);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "roundtrip") {
            jade::Prefab::load(argv[2]).save(argv[3]);
            return 0;
        }
        if (argc == 9 && std::string(argv[1]) == "instantiate") {
            const jade::Prefab prefab = jade::Prefab::load(argv[2]);
            const uint32_t entry = uint32_t(std::stoull(argv[4], nullptr, 0));
            const std::array<double, 3> position{{
                std::strtod(argv[5], nullptr), std::strtod(argv[6], nullptr),
                std::strtod(argv[7], nullptr)}};
            write_json(argv[3], jade::prefab_to_add_object(
                prefab, entry, position, argv[8]));
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "extract") {
            const jade::json::Value operation = jade::json::parse(read_text(argv[2]));
            jade::prefab_from_add_object(operation, argv[4]).save(argv[3]);
            return 0;
        }
        std::fprintf(stderr,
            "usage: prefab_cli roundtrip <input.jprefab> <output.jprefab>\n"
            "       prefab_cli instantiate <input.jprefab> <output.json> "
            "<entry> <x> <y> <z> <name>\n"
            "       prefab_cli extract <add-object.json> <output.jprefab> "
            "<description>\n");
        return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "prefab_cli: %s\n", error.what());
        return 1;
    }
}
