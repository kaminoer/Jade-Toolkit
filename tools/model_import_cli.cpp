// model_import_cli - oracle bridge for static AddObject model geometry.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "jade/ObjectPlacer.hpp"
#include "jade/Utf8Args.hpp"

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    if (argc != 9) {
        std::fprintf(stderr,
            "usage: model_import_cli <input> <output> <sx> <sy> <sz> "
            "<material-key> <import-colors:0|1> <platform-flags>\n");
        return 2;
    }
    try {
        const jade::placer::Vec3 scale = {
            std::stod(argv[3]), std::stod(argv[4]), std::stod(argv[5])};
        const uint32_t material =
            uint32_t(std::strtoul(argv[6], nullptr, 0));
        const bool colors = std::strtoul(argv[7], nullptr, 0) != 0;
        const uint32_t platform_flags =
            uint32_t(std::strtoul(argv[8], nullptr, 0));
        const jade::placer::PlacerGeo geo = jade::placer::load_model_geometry(
            argv[1], scale, material, colors);
        const std::vector<uint8_t> payload =
            jade::placer::geometry_to_payload_with_vb(
                geo, /*geo_version=*/7, platform_flags);
        std::ofstream output(std::filesystem::u8path(argv[2]),
                             std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not create output");
        output.write(reinterpret_cast<const char*>(payload.data()),
                     std::streamsize(payload.size()));
        if (!output) throw std::runtime_error("could not write output");
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "model_import_cli: %s\n", exception.what());
        return 1;
    }
}
