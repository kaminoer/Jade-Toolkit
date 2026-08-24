// glb_geo — parse a GLB and build a Jade GEO payload (for empirical validation).
//
//   glb_geo <in.glb> <out.bin> <version> <flags1> <flags2> <vb_magic> <shipped:0|1>
//
// Reads the GLB, runs parse_glb_mesh + build_geo_payload with the given original-
// GEO options, and writes the resulting GEO bytes to <out.bin>. Compared against
// core.jgao_converter._build_geo_payload by tests/gltf_geo_check.py.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "jade/Gltf.hpp"
#include "jade/Utf8Args.hpp"

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    if (argc < 8) {
        std::cerr << "usage: glb_geo <in.glb> <out.bin> <version> <flags1> <flags2>"
                     " <vb_magic> <shipped:0|1> [orig_skinned_stride]\n";
        return 2;
    }
    try {
        std::ifstream in(std::filesystem::u8path(argv[1]), std::ios::binary);
        if (!in) { std::cerr << "glb_geo: cannot open " << argv[1] << "\n"; return 2; }
        std::vector<uint8_t> glb((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());

        jade::gltf::MeshData md = jade::gltf::parse_glb_mesh(glb.data(), glb.size());

        jade::gltf::GeoBuildOpts opts;
        opts.version = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0));
        opts.flags1  = static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 0));
        opts.flags2  = static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 0));
        opts.vb_magic = static_cast<uint32_t>(std::strtoul(argv[6], nullptr, 0));
        opts.shipped_skinned = std::string(argv[7]) == "1";
        if (argc >= 9)
            opts.orig_skinned_stride = static_cast<uint32_t>(std::strtoul(argv[8], nullptr, 0));
        if (md.has_colors) opts.colors = &md.colors;

        std::vector<uint8_t> geo = jade::gltf::build_geo_payload(md, opts);

        std::ofstream out(std::filesystem::u8path(argv[2]), std::ios::binary);
        if (!out) { std::cerr << "glb_geo: cannot write " << argv[2] << "\n"; return 2; }
        out.write(reinterpret_cast<const char*>(geo.data()),
                  static_cast<std::streamsize>(geo.size()));
    } catch (const std::exception& e) {
        std::cerr << "glb_geo: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
