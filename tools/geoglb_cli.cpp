// geoglb_cli — the exporter's models/<sub>.glb path for one GEO.
//   geoglb_cli <bf> <entry_idx> <geo_key_hex> <out.glb> [name]
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Geometry.hpp"
#include "jade/GltfBuilder.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade;

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: geoglb_cli <bf> <entry_idx> <geo_key_hex> <out.glb> [name]\n");
        return 2;
    }
    uint32_t entry = uint32_t(std::strtoul(argv[2], nullptr, 0));
    uint32_t key = uint32_t(std::strtoul(argv[3], nullptr, 16));
    BigFile bf;
    try { bf.open(argv[1]); } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }
    LzoResult dr = decompress_lzo(bf.read_data(entry));
    if (!dr.ok) { std::printf("ERROR decompress\n"); return 0; }
    std::vector<SubEntry> subs = walk_sub_entries(dr.data);
    const SubEntry* target = nullptr;
    uint32_t gt1 = 1;
    for (const SubEntry& s : subs)
        if (s.key == key && !s.gro_null && s.gro_type == 1 &&
            is_geometry_entry(s.data.data(), s.data.size(), &gt1)) {
            target = &s;
            break;                       // first valid match, like the Python
        }
    if (!target) { std::printf("ERROR no geo\n"); return 0; }
    GeoInfo geo = parse_geometry(target->data.data(), target->data.size());
    if (!geo.ok) { std::printf("ERROR parse\n"); return 0; }
    char keyhex[16];
    std::snprintf(keyhex, sizeof keyhex, "0x%08X", key);
    std::string name = argc > 5 ? argv[5] : keyhex;
    std::vector<uint8_t> glb =
        gltfbuild::build_geo_model_glb(geo, name, keyhex, name);
    if (glb.empty()) { std::printf("ERROR empty\n"); return 0; }
    std::ofstream f(std::filesystem::u8path(argv[4]), std::ios::binary);
    f.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    std::printf("GLB bytes=%zu verts=%u tris=%u skinned=%d\n", glb.size(),
                geo.nb_points, geo.nb_tris, geo.skin_present ? 1 : 0);
    return 0;
}
