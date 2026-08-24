// image_cli - small oracle bridge for project image decoding/resizing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "jade/Image.hpp"
#include "jade/Texture.hpp"

static void put32(std::ofstream& f, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8),
                          uint8_t(v >> 16), uint8_t(v >> 24)};
    f.write(reinterpret_cast<const char*>(b), 4);
}

int main(int argc, char** argv) {
    const bool mips = argc == 7 && std::string(argv[3]) == "--mips";
    const bool thumb = argc == 4 && std::string(argv[3]) == "--thumb";
    if (argc != 3 && argc != 5 && !mips && !thumb) {
        std::fprintf(stderr,
            "usage: image_cli <input> <output> [width height]\n"
            "       image_cli <input> <output> --thumb\n"
            "       image_cli <input> <output> --mips <fmt> <count> <target-len|-1>\n");
        return 2;
    }
    try {
        if (thumb) {
            std::string error;
            const std::vector<uint8_t> png =
                jade::image_to_square_thumbnail_png(argv[1], 128, &error);
            if (png.empty()) throw std::runtime_error(error);
            std::ofstream f(argv[2], std::ios::binary | std::ios::trunc);
            if (!f) throw std::runtime_error("could not create output");
            f.write(reinterpret_cast<const char*>(png.data()),
                    std::streamsize(png.size()));
            if (!f) throw std::runtime_error("could not write output");
            return 0;
        }
        jade::RgbaImage image = jade::load_rgba_image(argv[1]);
        if (!image.ok) throw std::runtime_error(image.error);
        if (mips) {
            const uint32_t fmt = uint32_t(std::strtoul(argv[4], nullptr, 10));
            const uint32_t count = uint32_t(std::strtoul(argv[5], nullptr, 10));
            const int64_t target = std::strtoll(argv[6], nullptr, 10);
            const std::vector<uint8_t> encoded = jade::generate_mipmaps(
                image.rgba.data(), image.width, image.height, fmt, count,
                target);
            std::ofstream f(argv[2], std::ios::binary | std::ios::trunc);
            if (!f) throw std::runtime_error("could not create output");
            f.write("MIPS", 4); put32(f, uint32_t(encoded.size()));
            f.write(reinterpret_cast<const char*>(encoded.data()),
                    std::streamsize(encoded.size()));
            if (!f) throw std::runtime_error("could not write output");
            return 0;
        }
        if (argc == 5) {
            const uint32_t w = uint32_t(std::strtoul(argv[3], nullptr, 10));
            const uint32_t h = uint32_t(std::strtoul(argv[4], nullptr, 10));
            std::vector<uint8_t> resized = jade::resize_rgba_lanczos(
                image.rgba.data(), image.width, image.height, w, h);
            if (resized.empty()) throw std::runtime_error("resize failed");
            image.width = w; image.height = h; image.rgba = std::move(resized);
        }
        std::ofstream f(argv[2], std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("could not create output");
        f.write("RGBA", 4);
        put32(f, image.width); put32(f, image.height);
        f.write(reinterpret_cast<const char*>(image.rgba.data()),
                std::streamsize(image.rgba.size()));
        if (!f) throw std::runtime_error("could not write output");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "image_cli: %s\n", e.what());
        return 1;
    }
}
