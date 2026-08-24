// patcher_cli - scan an export manifest and reimport its non-deferred changes.
// Usage: patcher_cli <archive.bf> <unpack_dir>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Exporter.hpp"
#include "jade/Patcher.hpp"
#include "jade/Utf8Args.hpp"

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc >= 6 && std::string(argv[1]) == "--texture") {
        try {
            std::ifstream in(std::filesystem::u8path(argv[2]),
                             std::ios::binary);
            if (!in) throw std::runtime_error("could not open input entry");
            std::vector<uint8_t> dec((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
            uint32_t key = uint32_t(std::strtoul(argv[3], nullptr, 0));
            std::string encode = argc > 6 ? argv[6] : "auto";
            std::vector<std::string> log;
            jade::patcher::PatchTextureResult result =
                jade::patcher::patch_texture(dec, key, argv[4], encode, log);
            std::ofstream out(std::filesystem::u8path(argv[5]),
                              std::ios::binary);
            if (!out) throw std::runtime_error("could not open output entry");
            out.write(reinterpret_cast<const char*>(result.patched.data()),
                      std::streamsize(result.patched.size()));
            for (const std::string& line : log)
                std::printf("LOG %s\n", line.c_str());
            std::printf("TEXTURE changed=%d bytes=%zu\n",
                        result.changed ? 1 : 0, result.patched.size());
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ERROR %s\n", e.what());
            return 1;
        }
    }
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: patcher_cli <archive.bf> <unpack_dir>\n"
                     "       patcher_cli --texture <entry.bin> <key> "
                     "<image> <output.bin> [encode]\n");
        return 2;
    }
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        std::vector<std::string> scan_log;
        std::vector<jade::exporter::ChangeRec> changes =
            jade::exporter::scan_changes(argv[2], scan_log);
        for (const std::string& line : scan_log)
            std::printf("LOG %s\n", line.c_str());
        jade::patcher::PatchBigFileStats stats = jade::patcher::patch_bigfile(
            bf, argv[1], argv[2], changes,
            [](const std::string& line) {
                std::printf("LOG %s\n", line.c_str());
            },
            [](size_t current, size_t total) {
                std::printf("PROGRESS %zu/%zu\n", current, total);
            });
        std::printf("PATCH groups=%zu written=%zu errors=%zu size_grs=%d "
                    "backup=%d\n",
                    stats.entry_groups, stats.entries_written,
                    stats.entry_errors, stats.size_grs_rows,
                    stats.backup_created ? 1 : 0);
        return stats.entry_errors == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }
}
