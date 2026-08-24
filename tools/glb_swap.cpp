// glb_swap — run the native mesh swap (for the empirical vs-Python harness).
//
//   glb_swap <file.bf> <entry_idx> <geo_key_hex> <in.glb> [import_colors 0|1]
//
// Performs swap_mesh_in_bf (no backup — the harness works on scratch copies)
// and prints the comparable stats as one line. compressed_size/bf_entry_pos are
// intentionally NOT printed (LZO level differs from Python by design).
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "jade/MeshSwap.hpp"
#include "jade/Utf8Args.hpp"

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    if (argc < 5) {
        std::cerr << "usage: glb_swap <file.bf> <entry_idx> <geo_key_hex> <in.glb>"
                     " [import_colors 0|1]\n";
        return 2;
    }
    uint32_t entry = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    uint32_t key = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16));
    bool import_colors = argc >= 6 && std::string(argv[5]) == "1";

    jade::SwapStats st = jade::swap_mesh_in_bf(argv[1], entry, key, argv[4],
                                               import_colors, /*backup=*/false);
    if (!st.ok) {
        std::cerr << "glb_swap: error: " << st.error << "\n";
        return 1;
    }
    std::printf("SWAP key=%08x orig=%u/%u/%u/%zu new=%u/%u/%u/%zu dec=%zu "
                "skin=%d colors=%u src=%s gao_updates=%u orig_colors=%d\n",
                st.geo_key, st.orig_points, st.orig_uvs, st.orig_tris, st.orig_payload,
                st.new_points, st.new_uvs, st.new_tris, st.new_payload,
                st.decompressed_size, st.had_skin ? 1 : 0, st.wrote_colors,
                st.color_source.c_str(), st.gao_color_updates,
                st.orig_had_colors ? 1 : 0);
    return 0;
}
