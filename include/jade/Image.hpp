// Image.hpp - project-facing image loading and Pillow-compatible resizing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jade {

struct RgbaImage {
    bool ok = false;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    std::string error;
};

// Load the image formats accepted by the Python project operations and native
// GUI. DDS uses jade::read_dds; PNG/JPEG/BMP use the platform image codec on
// Windows; TGA has a small built-in decoder because Windows Imaging Component
// does not ship one.
RgbaImage load_rgba_image(const std::string& path);

// Python's nearest-power-of-two rule. The input is clamped to [min_dim,
// max_dim] first when max_dim is non-zero; ties choose the larger power.
uint32_t nearest_power_of_two(uint32_t value, uint32_t min_dim = 1,
                              uint32_t max_dim = 0);

// Separable Lanczos-3 resize for 8-bit RGBA. RGB is premultiplied by alpha
// while filtering, matching Pillow's RGBA resize behavior. Empty means invalid
// dimensions/input.
std::vector<uint8_t> resize_rgba_lanczos(const uint8_t* rgba,
                                         uint32_t src_w, uint32_t src_h,
                                         uint32_t dst_w, uint32_t dst_h);

// Deterministic standards-compliant RGBA PNG encoder. The DEFLATE stream uses
// stored blocks; compressed bytes need not match Pillow, but decoded pixels
// and metadata do.
std::vector<uint8_t> encode_png_rgba(const uint8_t* rgba,
                                     uint32_t width, uint32_t height);

// Load, centre-crop to a square, Lanczos-resize, and encode as PNG. This is
// build/jtmod_export.py::image_to_thumb.
std::vector<uint8_t> image_to_square_thumbnail_png(
    const std::string& path, uint32_t size = 128,
    std::string* error = nullptr);

}  // namespace jade
