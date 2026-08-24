// lzo_compress_cli — compress a raw file with the jade LZO framing.
//   lzo_compress_cli <in_raw> <out_bin> [level]
// Level 9 (default) = the vendored lzo1x_999 path (python-lzo compress_lzo_9).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "jade/Compression.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: lzo_compress_cli <in_raw> <out_bin> [level]\n");
        return 2;
    }
    int level = argc > 3 ? std::atoi(argv[3]) : 9;
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "ERROR cannot open input\n"); return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    std::vector<uint8_t> comp = jade::compress_lzo(data, level);
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(comp.data()),
              static_cast<std::streamsize>(comp.size()));
    std::printf("COMPRESS in=%zu out=%zu level=%d\n", data.size(), comp.size(), level);
    return 0;
}
