// levelblend_cli — baked-RLI reimport (level_blender import half).
//   levelblend_cli <bf> <entry_idx> <baked.glb|dump.json> [district]
//                  [srgb 0|1] [refresh 0|1] [--log]
// Reads the entry's subs, imports baked colours from GLB or JSON, prints one
// UPDATE line per changed GAO (key + payload CRC), applies them to every
// district bin, and prints the APPLY stats.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/LevelBlender.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade;

static int run_export(int argc, char** argv) {
    uint32_t entry = uint32_t(std::strtoul(argv[3], nullptr, 10));
    bool lights = argc > 5 ? (std::strtoul(argv[5], nullptr, 10) != 0) : true;
    bool usebf = argc > 6 ? (std::strtoul(argv[6], nullptr, 10) != 0) : true;
    BigFile bf;
    try { bf.open(argv[1]); } catch (const std::exception& e) {
        std::fprintf(stderr, "levelblend_cli: %s\n", e.what());
        return 1;
    }
    LzoResult dr = decompress_lzo(bf.read_data(entry));
    if (!dr.ok) { std::printf("ERROR entry did not decompress\n"); return 0; }
    std::vector<SubEntry> subs = parse_sub_entries(dr.data);
    const bool logging = argc > 7 && std::string(argv[7]) == "--log";
    levelblend::LevelBlendLogFn log_fn;
    if (logging)
        log_fn = [](const std::string& line) {
            std::printf("%s\n", line.c_str());
        };
    levelblend::ExportManifest m = levelblend::export_level_to_glb(
        subs, argv[4], lights, usebf ? &bf : nullptr, /*srgb=*/false, log_fn);
    if (!m.ok) { std::printf("ERROR %s\n", m.error.c_str()); return 0; }
    std::string lt;
    for (const auto& kv : m.light_types) {
        if (!lt.empty()) lt += ",";
        lt += kv.first + ":" + std::to_string(kv.second);
    }
    std::printf("EXPORT objects=%u lights=%u types=%s exported=%zu\n", m.objects,
                m.lights, lt.c_str(), m.exported.size());
    return 0;
}

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    if (argc >= 5 && std::string(argv[2]) == "export") return run_export(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: levelblend_cli <bf> <entry_idx> <baked.glb|dump.json> "
            "[district] [srgb 0|1] [refresh 0|1] [--log]\n"
            "       levelblend_cli <bf> export <entry_idx> <out.glb> "
            "[lights 0|1] [usebf 0|1] [--log]\n");
        return 2;
    }
    uint32_t entry = uint32_t(std::strtoul(argv[2], nullptr, 10));
    std::string district = argc > 4 ? argv[4] : "500";
    bool srgb = argc > 5 && std::strtoul(argv[5], nullptr, 10) != 0;
    bool refresh = argc > 6 ? (std::strtoul(argv[6], nullptr, 10) != 0) : true;

    BigFile bf;
    try { bf.open(argv[1]); } catch (const std::exception& e) {
        std::fprintf(stderr, "levelblend_cli: %s\n", e.what());
        return 1;
    }
    LzoResult dr = decompress_lzo(bf.read_data(entry));
    if (!dr.ok) { std::printf("ERROR entry did not decompress\n"); return 0; }
    std::vector<SubEntry> subs = parse_sub_entries(dr.data);
    const bool logging = argc > 7 && std::string(argv[7]) == "--log";
    levelblend::LevelBlendLogFn log_fn;
    if (logging)
        log_fn = [](const std::string& line) {
            std::printf("%s\n", line.c_str());
        };

    levelblend::BakedImport bi =
        levelblend::import_baked_rli(argv[3], subs, refresh, srgb, log_fn);
    if (!bi.ok) { std::printf("ERROR %s\n", bi.error.c_str()); return 0; }
    for (const auto& kv : bi.updates)
        std::printf("UPDATE %08x len=%zu crc=%08x\n", kv.first, kv.second.size(),
                    crc32(kv.second.data(), kv.second.size()));
    std::printf("IMPORT updates=%zu misses=%u\n", bi.updates.size(), bi.misses);

    levelblend::ApplyStats st =
        levelblend::apply_baked_rli_to_bf(argv[1], bi.updates, district, log_fn);
    if (!st.ok) { std::printf("ERROR %s\n", st.error.c_str()); return 0; }
    std::printf("APPLY bins=%u gaos=%u\n", st.bins, st.gaos);
    return 0;
}
