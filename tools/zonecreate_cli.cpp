// zonecreate_cli -- file-oriented driver for core/zone_create.py parity.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "jade/ZoneCreate.hpp"

namespace {

bool read_file(const char* path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    data.assign(std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const char* path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    return bool(file);
}

uint32_t number(const std::string& text) {
    return static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 0));
}

std::vector<uint32_t> numbers(const std::string& text) {
    std::vector<uint32_t> result;
    std::istringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
        if (!item.empty()) result.push_back(number(item));
    return result;
}

void print_keys(const char* label, std::vector<uint32_t> keys) {
    std::sort(keys.begin(), keys.end());
    std::printf("%s", label);
    for (size_t index = 0; index < keys.size(); ++index)
        std::printf("%s0x%08x", index ? "," : "", keys[index]);
    std::printf("\n");
}

int usage() {
    std::fprintf(stderr,
        "usage: zonecreate_cli collect IN PREFIX | map KEYS OLD NEW | "
        "rekey IN OUT OLD:NEW,... | patch IN OUT NAME | patch-nonascii IN | "
        "clone WOW WOL WOWOUT WOLOUT NAME LOW16 PREFIX | "
        "deps IN OUT KEYS | alloc PREFIX BF_KEYS STAGED_KEYS\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace jade;
    using namespace jade::zonecreate;
    if (argc < 2) return usage();
    const std::string command = argv[1];
    try {
        if (command == "collect" && argc == 4) {
            std::vector<uint8_t> input;
            if (!read_file(argv[2], input)) return 3;
            auto found = collect_local_keys(input, number(argv[3]));
            print_keys("LOCAL ",
                       std::vector<uint32_t>(found.begin(), found.end()));
            return 0;
        }
        if (command == "map" && argc == 5) {
            const std::vector<uint32_t> raw = numbers(argv[2]);
            std::unordered_set<uint32_t> keys(raw.begin(), raw.end());
            const KeyMap mapped = build_key_map(
                keys, number(argv[3]), number(argv[4]));
            std::vector<uint32_t> old_keys;
            for (const auto& item : mapped) old_keys.push_back(item.first);
            std::sort(old_keys.begin(), old_keys.end());
            std::printf("MAP ");
            for (size_t index = 0; index < old_keys.size(); ++index)
                std::printf("%s0x%08x:0x%08x", index ? "," : "",
                            old_keys[index], mapped.at(old_keys[index]));
            std::printf("\n");
            return 0;
        }
        if (command == "rekey" && argc == 5) {
            std::vector<uint8_t> input;
            if (!read_file(argv[2], input)) return 3;
            KeyMap mapped;
            std::istringstream stream(argv[4]);
            std::string item;
            while (std::getline(stream, item, ',')) {
                const size_t colon = item.find(':');
                if (colon != std::string::npos)
                    mapped[number(item.substr(0, colon))] =
                        number(item.substr(colon + 1));
            }
            auto result = rekey_stream(input, mapped);
            if (!write_file(argv[3], result.first)) return 3;
            std::printf("RESULT count=%d bytes=%zu\n", result.second,
                        result.first.size());
            return 0;
        }
        if (command == "patch" && argc == 5) {
            std::vector<uint8_t> input;
            if (!read_file(argv[2], input)) return 3;
            const std::vector<uint8_t> result = patch_wow_name(input, argv[4]);
            if (!write_file(argv[3], result)) return 3;
            std::printf("RESULT bytes=%zu\n", result.size());
            return 0;
        }
        if (command == "patch-nonascii" && argc == 3) {
            std::vector<uint8_t> input;
            if (!read_file(argv[2], input)) return 3;
            (void)patch_wow_name(input, "A\xc5\xbb" "B");
            std::puts("RESULT no-error");
            return 0;
        }
        if (command == "clone" && argc == 9) {
            std::vector<uint8_t> wow, wol;
            if (!read_file(argv[2], wow) || !read_file(argv[3], wol)) return 3;
            const ClonedZone result = clone_zone(
                wow, wol, argv[6], number(argv[7]), {}, number(argv[8]));
            if (!write_file(argv[4], result.wow_bytes) ||
                !write_file(argv[5], result.wol_bytes)) return 3;
            std::printf("RESULT new_wow=0x%08x new_wol=0x%08x rekeyed=%d "
                        "prefix=0x%02x\n", result.new_wow_key,
                        result.new_wol_key, result.rekeyed_count,
                        result.new_map_prefix);
            print_keys("LOCAL ", std::vector<uint32_t>(
                                     result.local_keys_seen.begin(),
                                     result.local_keys_seen.end()));
            return 0;
        }
        if (command == "deps" && argc == 5) {
            std::vector<uint8_t> input;
            if (!read_file(argv[2], input)) return 3;
            auto result = add_wol_deps(input, numbers(argv[4]));
            if (!write_file(argv[3], result.first)) return 3;
            std::printf("RESULT added=%d bytes=%zu\n", result.second,
                        result.first.size());
            return 0;
        }
        if (command == "alloc" && argc == 5) {
            BigFile bf;
            int index = 0;
            for (uint32_t key : numbers(argv[3])) {
                BFFile file;
                file.key = key;
                bf.files[index++] = file;
            }
            const AllocatedZoneKeys result = alloc_new_zone_bf_keys(
                bf, number(argv[2]), numbers(argv[4]));
            std::printf("RESULT wow=0x%08x wol=0x%08x low16=0x%04x\n",
                        result.wow_key, result.wol_key, result.low16);
            return 0;
        }
        return usage();
    } catch (const Error& error) {
        std::printf("ERROR %s: %s\n", error.python_type.c_str(), error.what());
        return 0;
    } catch (const std::exception& error) {
        std::printf("ERROR RuntimeError: %s\n", error.what());
        return 0;
    }
}
