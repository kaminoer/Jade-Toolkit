#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "jade/ProjectAssets.hpp"
#include "jade/Utf8Args.hpp"

namespace {

std::vector<uint8_t> unhex(const std::string& text) {
    if (text.size() % 2) throw std::runtime_error("odd hex length");
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t offset = 0; offset < text.size(); offset += 2) {
        const std::string byte_text = text.substr(offset, 2);
        char* end = nullptr;
        const unsigned long value =
            std::strtoul(byte_text.c_str(), &end, 16);
        if (end == nullptr || *end != '\0') throw std::runtime_error("bad hex");
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: project_assets_cli <assets-dir> <command> [...]\n");
        return 2;
    }
    try {
        jade::project_assets::AssetStore store(argv[1]);
        const std::string command = argv[2];
        if (command == "add" && argc == 4) {
            std::printf("REF %s\n", store.add(argv[3]).c_str());
        } else if (command == "add-bytes" && argc == 5) {
            std::printf("REF %s\n",
                        store.add_bytes(unhex(argv[4]), argv[3]).c_str());
        } else if (command == "resolve" && argc == 4) {
            std::printf("PATH %s\n", store.resolve(argv[3]).c_str());
        } else if (command == "exists" && argc == 4) {
            std::printf("EXISTS %d\n", store.exists(argv[3]) ? 1 : 0);
        } else if (command == "list" && argc == 3) {
            for (const std::string& ref : store.all_refs())
                std::printf("REF %s\n", ref.c_str());
        } else if (command == "gc") {
            std::vector<std::string> refs;
            for (int i = 3; i < argc; ++i) refs.push_back(argv[i]);
            std::printf("REMOVED %zu\n", store.gc(refs));
        } else {
            std::fprintf(stderr, "bad command or argument count\n");
            return 2;
        }
    } catch (const std::exception& error) {
        std::printf("ERROR %s\n", error.what());
    }
    return 0;
}
