// exporter_cli — BigFile unpack (non-animation subset).
//   exporter_cli <bf> <out_dir> [entry_idx]
// With entry_idx: single-entry mode, prints "ENTRY <json>" (or SKIP).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Exporter.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade;

namespace {

std::string json_string(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(c));
            out += buf;
        } else {
            out += char(c);
        }
    }
    return out + "\"";
}

void print_groups(const std::vector<exporter::CharacterGroup>& groups) {
    std::printf("GROUPS [");
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& group = groups[gi];
        if (gi) std::printf(",");
        std::string base = json_string(group.base_name);
        std::string skeleton = json_string(group.skeleton.name);
        std::printf("{\"base_name\":%s,\"skeleton\":%s,\"costumes\":[",
                    base.c_str(), skeleton.c_str());
        for (size_t ci = 0; ci < group.costumes.size(); ++ci) {
            if (ci) std::printf(",");
            std::string costume = json_string(group.costumes[ci].name);
            std::printf("%s", costume.c_str());
        }
        std::printf("]}");
    }
    std::printf("]\n");
}

}  // namespace

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    if (argc == 6 && std::string(argv[1]) == "--character-dec") {
        auto read_subs = [](const std::string& path) {
            std::vector<SubEntry> subs;
            if (path == "-") return subs;
            std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)),
                                      std::istreambuf_iterator<char>());
            return parse_sub_entries(data);
        };
        auto skeleton = read_subs(argv[2]);
        auto costume = read_subs(argv[3]);
        auto result = exporter::export_character_bundle(
            skeleton, costume, argv[4], argv[5]);
        if (!result.ok) {
            std::printf("SKIP %s\n", result.error.c_str());
            return 0;
        }
        std::printf("BUNDLE meshes=%u materials=%u textures=%u animations=%u bones=%u\n",
                    result.meshes, result.materials, result.textures,
                    result.animations, result.bones);
        return 0;
    }
    if (argc >= 7 && std::string(argv[1]) == "--entry-dec") {
        std::ifstream input(std::filesystem::u8path(argv[2]), std::ios::binary);
        if (!input) {
            std::fprintf(stderr, "ERROR cannot read decompressed entry\n");
            return 1;
        }
        std::vector<uint8_t> dec((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
        std::string out_dir = argv[3];
        uint32_t index = uint32_t(std::strtoul(argv[4], nullptr, 0));
        uint32_t key = uint32_t(std::strtoul(argv[5], nullptr, 0));
        bool scene_mode = argc > 7 && std::string(argv[7]) == "--scene";
        std::error_code ec;
        for (const char* dir : {"textures", "models", "scenes"})
            std::filesystem::create_directories(
                std::filesystem::u8path(out_dir) / dir, ec);
        std::string info = exporter::process_entry(
            dec, index, key, argv[6], out_dir, scene_mode);
        if (info.empty()) std::printf("SKIP\n");
        else std::printf("ENTRY %s\n", info.c_str());
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--presets") {
        uint32_t cpus = argc > 2 ? uint32_t(std::strtoul(argv[2], nullptr, 0)) : 0;
        auto presets = exporter::get_worker_presets(cpus);
        std::printf("PRESETS {");
        bool first = true;
        for (const auto& preset : presets) {
            if (!first) std::printf(",");
            first = false;
            std::string description = json_string(preset.second);
            std::printf("\"%u\":%s", preset.first, description.c_str());
        }
        std::printf("}\n");
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--detect") {
        BigFile bf;
        try { bf.open(argv[2]); } catch (const std::exception& e) {
            std::fprintf(stderr, "ERROR %s\n", e.what());
            return 1;
        }
        print_groups(exporter::detect_character_groups(bf));
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--detect-names") {
        BigFile bf;
        for (int i = 2; i < argc; ++i) {
            BFFile fi;
            fi.index = uint32_t(i - 2);
            fi.key = 0x10000000u + uint32_t(i - 2);
            fi.name = argv[i];
            bf.files.emplace(fi.index, std::move(fi));
        }
        print_groups(exporter::detect_character_groups(bf));
        return 0;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: exporter_cli <bf> <out_dir> [entry_idx [--scene]]\n"
                             "       exporter_cli <bf> <out_dir> [--no-scenes] [--log] [--workers N]\n"
                             "       exporter_cli --scan <out_dir>\n"
                             "       exporter_cli --entry-dec <dec> <out_dir> <idx> <key> <name> [--scene]\n"
                             "       exporter_cli --character-dec <skeleton.dec|-> <costume.dec|-> <out.glb> <name>\n"
                             "       exporter_cli --detect <bf>\n"
                             "       exporter_cli --detect-names [name ...]\n"
                             "       exporter_cli --presets [cpu_count]\n");
        return 2;
    }
    if (std::string(argv[1]) == "--scan") {
        // importer.scan_changes over an unpack dir's manifest.
        std::vector<std::string> log;
        auto changes = exporter::scan_changes(argv[2], log);
        for (const std::string& l : log) std::printf("LOG %s\n", l.c_str());
        for (const auto& c : changes)
            std::printf("CHANGE %s|%08x|%u|%08x|%s|%s|%s|%s\n", c.path.c_str(),
                        c.key, c.entry_index, c.entry_key,
                        c.category.c_str(), c.entry_name.c_str(),
                        c.status.c_str(), c.sub_info_json.c_str());
        std::printf("SCAN count=%zu\n", changes.size());
        return 0;
    }
    BigFile bf;
    try { bf.open(argv[1]); } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }
    if (argc > 3 && std::string(argv[3]).rfind("--", 0) != 0) {
        uint32_t idx = uint32_t(std::strtoul(argv[3], nullptr, 0));
        bool scene_mode = argc > 4 && std::string(argv[4]) == "--scene";
        auto it = bf.files.find(idx);
        if (it == bf.files.end()) { std::printf("SKIP\n"); return 0; }
        std::error_code ec;
        for (const char* d : {"textures", "models", "scenes"})
            std::filesystem::create_directories(
                std::filesystem::u8path(argv[2]) / d, ec);
        LzoResult r = decompress_lzo(bf.read_data(idx));
        if (!r.ok) { std::printf("SKIP\n"); return 0; }
        std::string info = exporter::process_entry(r.data, it->second.index,
                                                   it->second.key,
                                                   it->second.name, argv[2],
                                                   scene_mode);
        if (info.empty()) std::printf("SKIP\n");
        else std::printf("ENTRY %s\n", info.c_str());
        return 0;
    }
    bool scene_mode = true;
    bool trace = false;
    int32_t max_workers = -1;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-scenes") scene_mode = false;
        else if (arg == "--log") trace = true;
        else if (arg == "--workers" && i + 1 < argc)
            max_workers = int32_t(std::strtol(argv[++i], nullptr, 10));
        else {
            std::fprintf(stderr, "ERROR unknown export option: %s\n", arg.c_str());
            return 2;
        }
    }
    exporter::ExportLogFn log_fn;
    exporter::ExportProgressFn progress_fn;
    if (trace) {
        log_fn = [](const std::string& line) {
            std::printf("LOG %s\n", line.c_str());
        };
        progress_fn = [](uint32_t current, uint32_t total) {
            std::printf("PROGRESS %u %u\n", current, total);
        };
    }
    exporter::ExportStats st =
        exporter::export_bigfile(bf, argv[1], argv[2], scene_mode,
                                 log_fn, progress_fn, max_workers);
    if (!st.ok) { std::printf("ERROR %s\n", st.error.c_str()); return 0; }
    std::printf("EXPORTED entries=%u\n", st.entries);
    return 0;
}
