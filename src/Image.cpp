#include "jade/Image.hpp"

#include "jade/Texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#endif

namespace jade {
namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

std::string lower_extension(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return ext;
}

uint16_t u16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

bool tga_colour(const uint8_t* p, unsigned bits, uint8_t* out) {
    if (bits == 32) {
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];
        return true;
    }
    if (bits == 24) {
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255;
        return true;
    }
    if (bits == 15 || bits == 16) {
        const uint16_t v = u16(p);
        const uint8_t r = uint8_t((v >> 10) & 31);
        const uint8_t g = uint8_t((v >> 5) & 31);
        const uint8_t b = uint8_t(v & 31);
        out[0] = uint8_t((r << 3) | (r >> 2));
        out[1] = uint8_t((g << 3) | (g >> 2));
        out[2] = uint8_t((b << 3) | (b >> 2));
        out[3] = bits == 16 ? ((v & 0x8000) ? 255 : 0) : 255;
        return true;
    }
    return false;
}

RgbaImage load_tga(const std::vector<uint8_t>& d) {
    RgbaImage out;
    if (d.size() < 18) { out.error = "TGA header is truncated"; return out; }
    const unsigned id_len = d[0];
    const unsigned cmap_type = d[1];
    const unsigned image_type = d[2];
    const unsigned cmap_first = u16(&d[3]);
    const unsigned cmap_len = u16(&d[5]);
    const unsigned cmap_bits = d[7];
    const uint32_t w = u16(&d[12]), h = u16(&d[14]);
    const unsigned pixel_bits = d[16];
    const unsigned desc = d[17];
    const bool mapped = image_type == 1 || image_type == 9;
    const bool truecolour = image_type == 2 || image_type == 10;
    const bool grey = image_type == 3 || image_type == 11;
    const bool rle = image_type == 9 || image_type == 10 || image_type == 11;
    if ((!mapped && !truecolour && !grey) || w == 0 || h == 0) {
        out.error = "unsupported TGA image type";
        return out;
    }
    if (uint64_t(w) * h > std::numeric_limits<size_t>::max() / 4) {
        out.error = "TGA dimensions are too large";
        return out;
    }
    size_t off = 18u + id_len;
    if (off > d.size()) { out.error = "TGA image ID is truncated"; return out; }

    std::vector<uint8_t> palette;
    if (mapped) {
        if (cmap_type != 1 || cmap_len == 0) {
            out.error = "TGA colour map is missing";
            return out;
        }
        const size_t cb = (cmap_bits + 7u) / 8u;
        if (cb < 2 || cb > 4 || size_t(cmap_len) > (d.size() - off) / cb) {
            out.error = "TGA colour map is truncated or unsupported";
            return out;
        }
        palette.resize(size_t(cmap_first + cmap_len) * 4, 0);
        for (unsigned i = 0; i < cmap_len; ++i) {
            if (!tga_colour(&d[off + size_t(i) * cb], cmap_bits,
                            &palette[size_t(cmap_first + i) * 4])) {
                out.error = "unsupported TGA colour-map depth";
                return out;
            }
        }
        off += size_t(cmap_len) * cb;
    }

    const size_t src_bpp = mapped ? (pixel_bits + 7u) / 8u
                                  : (grey ? (pixel_bits + 7u) / 8u
                                          : (pixel_bits + 7u) / 8u);
    if (src_bpp == 0 || src_bpp > 4 || (mapped && pixel_bits != 8 && pixel_bits != 16)
        || (grey && pixel_bits != 8 && pixel_bits != 16)
        || (truecolour && pixel_bits != 15 && pixel_bits != 16
            && pixel_bits != 24 && pixel_bits != 32)) {
        out.error = "unsupported TGA pixel depth";
        return out;
    }

    const size_t count = size_t(w) * h;
    std::vector<uint8_t> linear(count * 4);
    auto decode_one = [&](const uint8_t* src, uint8_t* dst) -> bool {
        if (mapped) {
            const unsigned idx = pixel_bits == 8 ? src[0] : u16(src);
            if (idx >= palette.size() / 4) return false;
            std::memcpy(dst, &palette[size_t(idx) * 4], 4);
            return true;
        }
        if (grey) {
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = pixel_bits == 16 ? src[1] : 255;
            return true;
        }
        return tga_colour(src, pixel_bits, dst);
    };

    size_t written = 0;
    while (written < count) {
        size_t run = 1;
        bool repeat = false;
        if (rle) {
            if (off >= d.size()) { out.error = "TGA RLE stream is truncated"; return out; }
            const uint8_t packet = d[off++];
            repeat = (packet & 0x80) != 0;
            run = size_t(packet & 0x7f) + 1;
            if (run > count - written) { out.error = "TGA RLE packet overruns image"; return out; }
        }
        if (repeat) {
            if (src_bpp > d.size() - off) { out.error = "TGA RLE pixel is truncated"; return out; }
            uint8_t px[4];
            if (!decode_one(&d[off], px)) { out.error = "invalid TGA palette index"; return out; }
            off += src_bpp;
            for (size_t i = 0; i < run; ++i)
                std::memcpy(&linear[(written + i) * 4], px, 4);
        } else {
            if (run > (d.size() - off) / src_bpp) { out.error = "TGA pixels are truncated"; return out; }
            for (size_t i = 0; i < run; ++i) {
                if (!decode_one(&d[off + i * src_bpp],
                                &linear[(written + i) * 4])) {
                    out.error = "invalid TGA palette index";
                    return out;
                }
            }
            off += run * src_bpp;
        }
        written += run;
    }

    out.rgba.resize(linear.size());
    const bool top = (desc & 0x20) != 0;
    const bool right = (desc & 0x10) != 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t dx = right ? w - 1 - x : x;
            const uint32_t dy = top ? y : h - 1 - y;
            std::memcpy(&out.rgba[(size_t(dy) * w + dx) * 4],
                        &linear[(size_t(y) * w + x) * 4], 4);
        }
    }
    out.ok = true; out.width = w; out.height = h;
    return out;
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                int(s.size()), nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {
        cp = CP_ACP; flags = 0;
        n = MultiByteToWideChar(cp, flags, s.data(), int(s.size()), nullptr, 0);
    }
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(cp, flags, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string hr_text(const char* what, HRESULT hr) {
    std::ostringstream ss;
    ss << what << " (HRESULT 0x" << std::hex << uint32_t(hr) << ')';
    return ss.str();
}

RgbaImage load_wic(const std::string& path) {
    RgbaImage out;
    const std::wstring wide = utf8_to_wide(path);
    if (wide.empty()) { out.error = "image path is not valid UTF-8"; return out; }
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        out.error = hr_text("could not initialize COM", init);
        return out;
    }
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->CreateDecoderFromFilename(
            wide.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
            &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0
        || uint64_t(w) * h > std::numeric_limits<size_t>::max() / 4))
        hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        out.rgba.resize(size_t(w) * h * 4);
        const uint64_t bytes = uint64_t(w) * h * 4;
        if (bytes > std::numeric_limits<UINT>::max()) hr = E_INVALIDARG;
        else hr = converter->CopyPixels(nullptr, w * 4, UINT(bytes),
                                        out.rgba.data());
    }
    if (SUCCEEDED(hr)) {
        out.ok = true; out.width = w; out.height = h;
    } else {
        out.rgba.clear();
        out.error = hr_text("could not decode image", hr);
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (uninitialize) CoUninitialize();
    return out;
}
#endif

struct AxisCoefficients {
    int ksize = 0;
    std::vector<int> starts;
    std::vector<int> counts;
    std::vector<int32_t> coeffs;
};

double lanczos3(double x) {
    x = std::abs(x);
    if (x == 0.0) return 1.0;
    if (x >= 3.0) return 0.0;
    constexpr double pi = 3.14159265358979323846264338327950288;
    const double pix = pi * x;
    return (std::sin(pix) / pix) * (std::sin(pix / 3.0) / (pix / 3.0));
}

AxisCoefficients coefficients(int in_size, int out_size) {
    AxisCoefficients out;
    const double scale = double(in_size) / out_size;
    const double filter_scale = std::max(1.0, scale);
    const double support = 3.0 * filter_scale;
    out.ksize = int(std::ceil(support)) * 2 + 1;
    out.starts.resize(size_t(out_size));
    out.counts.resize(size_t(out_size));
    out.coeffs.assign(size_t(out_size) * out.ksize, 0);
    constexpr double fixed_scale = double(1 << 22);
    for (int dst = 0; dst < out_size; ++dst) {
        const double center = (double(dst) + 0.5) * scale;
        int xmin = int(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = int(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        const int count = std::max(0, xmax - xmin);
        out.starts[size_t(dst)] = xmin;
        out.counts[size_t(dst)] = count;
        std::vector<double> weights(static_cast<size_t>(count));
        double sum = 0.0;
        for (int i = 0; i < count; ++i) {
            const double x = (double(i + xmin) - center + 0.5) / filter_scale;
            weights[size_t(i)] = lanczos3(x);
            sum += weights[size_t(i)];
        }
        if (sum == 0.0) continue;
        for (int i = 0; i < count; ++i) {
            const double v = weights[size_t(i)] / sum * fixed_scale;
            out.coeffs[size_t(dst) * out.ksize + i] =
                int32_t(v < 0.0 ? v - 0.5 : v + 0.5);
        }
    }
    return out;
}

uint8_t clip8(int64_t value) {
    value >>= 22;
    if (value < 0) return 0;
    if (value > 255) return 255;
    return uint8_t(value);
}

std::vector<uint8_t> resize_axis(const std::vector<uint8_t>& src,
                                 int src_w, int src_h, int dst_w, int dst_h,
                                 bool horizontal) {
    const AxisCoefficients co = coefficients(horizontal ? src_w : src_h,
                                             horizontal ? dst_w : dst_h);
    std::vector<uint8_t> dst(size_t(dst_w) * dst_h * 4);
    constexpr int64_t rounding = int64_t(1) << 21;
    if (horizontal) {
        for (int y = 0; y < src_h; ++y) {
            for (int x = 0; x < dst_w; ++x) {
                const int start = co.starts[size_t(x)], count = co.counts[size_t(x)];
                for (int c = 0; c < 4; ++c) {
                    int64_t sum = rounding;
                    const int32_t* k = &co.coeffs[size_t(x) * co.ksize];
                    for (int i = 0; i < count; ++i)
                        sum += int64_t(src[(size_t(y) * src_w + start + i) * 4 + c]) * k[i];
                    dst[(size_t(y) * dst_w + x) * 4 + c] = clip8(sum);
                }
            }
        }
    } else {
        for (int y = 0; y < dst_h; ++y) {
            const int start = co.starts[size_t(y)], count = co.counts[size_t(y)];
            const int32_t* k = &co.coeffs[size_t(y) * co.ksize];
            for (int x = 0; x < src_w; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int64_t sum = rounding;
                    for (int i = 0; i < count; ++i)
                        sum += int64_t(src[(size_t(start + i) * src_w + x) * 4 + c]) * k[i];
                    dst[(size_t(y) * src_w + x) * 4 + c] = clip8(sum);
                }
            }
        }
    }
    return dst;
}

uint8_t premultiply(uint8_t colour, uint8_t alpha) {
    const unsigned t = unsigned(colour) * alpha + 128;
    return uint8_t((t + (t >> 8)) >> 8);
}

uint8_t unpremultiply(uint8_t colour, uint8_t alpha) {
    if (alpha == 0) return 0;
    // Pillow's RGBa -> RGBA conversion uses the integer division lookup table
    // (floor), not round-to-nearest.
    return uint8_t(std::min(255u,
        (unsigned(colour) * 255u) / unsigned(alpha)));
}

}  // namespace

RgbaImage load_rgba_image(const std::string& path) {
    const std::string ext = lower_extension(path);
    if (ext == ".dds") {
        const std::vector<uint8_t> d = read_file(path);
        if (d.empty()) return {false, 0, 0, {}, "could not read DDS file"};
        const DdsImage image = read_dds(d.data(), d.size());
        if (!image.ok) return {false, 0, 0, {}, "unsupported DDS layout"};
        return {true, image.width, image.height, image.rgba, {}};
    }
    if (ext == ".tga") {
        const std::vector<uint8_t> d = read_file(path);
        if (d.empty()) return {false, 0, 0, {}, "could not read TGA file"};
        return load_tga(d);
    }
#ifdef _WIN32
    return load_wic(path);
#else
    return {false, 0, 0, {},
            "PNG/JPEG/BMP decoding is currently available only on Windows"};
#endif
}

uint32_t nearest_power_of_two(uint32_t value, uint32_t min_dim,
                              uint32_t max_dim) {
    if (min_dim == 0) min_dim = 1;
    value = std::max(value, min_dim);
    if (max_dim != 0) value = std::min(value, max_dim);
    uint32_t lo = 1;
    while (lo <= value / 2 && lo <= (uint32_t(1) << 30)) lo <<= 1;
    if (lo == value || lo > (uint32_t(1) << 30)) return lo;
    const uint32_t hi = lo << 1;
    uint32_t result = value - lo < hi - value ? lo : hi;
    if (result < min_dim) result = min_dim;
    if (max_dim != 0 && result > max_dim) result = max_dim;
    return result;
}

std::vector<uint8_t> resize_rgba_lanczos(const uint8_t* rgba,
                                         uint32_t src_w, uint32_t src_h,
                                         uint32_t dst_w, uint32_t dst_h) {
    if (!rgba || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0
        || uint64_t(src_w) * src_h > std::numeric_limits<size_t>::max() / 4
        || uint64_t(dst_w) * dst_h > std::numeric_limits<size_t>::max() / 4)
        return {};
    std::vector<uint8_t> work(rgba, rgba + size_t(src_w) * src_h * 4);
    if (src_w == dst_w && src_h == dst_h) return work;
    for (size_t i = 0; i < work.size(); i += 4) {
        work[i + 0] = premultiply(work[i + 0], work[i + 3]);
        work[i + 1] = premultiply(work[i + 1], work[i + 3]);
        work[i + 2] = premultiply(work[i + 2], work[i + 3]);
    }
    if (src_w != dst_w)
        work = resize_axis(work, int(src_w), int(src_h), int(dst_w), int(src_h), true);
    if (src_h != dst_h)
        work = resize_axis(work, int(dst_w), int(src_h), int(dst_w), int(dst_h), false);
    for (size_t i = 0; i < work.size(); i += 4) {
        work[i + 0] = unpremultiply(work[i + 0], work[i + 3]);
        work[i + 1] = unpremultiply(work[i + 1], work[i + 3]);
        work[i + 2] = unpremultiply(work[i + 2], work[i + 3]);
    }
    return work;
}

namespace {

void be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 24));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

uint32_t png_crc(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & uint32_t(0 - (crc & 1)));
    }
    return crc ^ 0xffffffffu;
}

void png_chunk(std::vector<uint8_t>& out, const char type[4],
               const std::vector<uint8_t>& data) {
    be32(out, uint32_t(data.size()));
    const size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, png_crc(out.data() + crc_start, 4 + data.size()));
}

}  // namespace

std::vector<uint8_t> encode_png_rgba(const uint8_t* rgba,
                                     uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0
        || uint64_t(width) * height >
               (std::numeric_limits<size_t>::max() - height) / 4)
        return {};
    std::vector<uint8_t> raw;
    raw.reserve((size_t(width) * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);  // PNG filter type None
        const uint8_t* row = rgba + size_t(y) * width * 4;
        raw.insert(raw.end(), row, row + size_t(width) * 4);
    }

    std::vector<uint8_t> zlib = {0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t count = std::min<size_t>(65535, raw.size() - pos);
        const bool final = pos + count == raw.size();
        zlib.push_back(final ? 1 : 0);  // BFINAL + stored BTYPE
        const uint16_t len = uint16_t(count);
        const uint16_t inv = uint16_t(~len);
        zlib.push_back(uint8_t(len)); zlib.push_back(uint8_t(len >> 8));
        zlib.push_back(uint8_t(inv)); zlib.push_back(uint8_t(inv >> 8));
        zlib.insert(zlib.end(), raw.begin() + std::ptrdiff_t(pos),
                    raw.begin() + std::ptrdiff_t(pos + count));
        pos += count;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t value : raw) {
        a = (a + value) % 65521u;
        b = (b + a) % 65521u;
    }
    be32(zlib, (b << 16) | a);

    std::vector<uint8_t> out = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    be32(ihdr, width); be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", zlib);
    png_chunk(out, "IEND", {});
    return out;
}

std::vector<uint8_t> image_to_square_thumbnail_png(
    const std::string& path, uint32_t size, std::string* error) {
    RgbaImage source = load_rgba_image(path);
    if (!source.ok || size == 0) {
        if (error) *error = source.ok ? "invalid thumbnail size" : source.error;
        return {};
    }
    const uint32_t side = std::min(source.width, source.height);
    const uint32_t left = (source.width - side) / 2;
    const uint32_t top = (source.height - side) / 2;
    std::vector<uint8_t> square(size_t(side) * side * 4);
    for (uint32_t y = 0; y < side; ++y) {
        const uint8_t* row = source.rgba.data()
            + (size_t(top + y) * source.width + left) * 4;
        std::copy(row, row + size_t(side) * 4,
                  square.begin() + size_t(y) * side * 4);
    }
    std::vector<uint8_t> resized = resize_rgba_lanczos(
        square.data(), side, side, size, size);
    std::vector<uint8_t> png = encode_png_rgba(resized.data(), size, size);
    if (png.empty() && error) *error = "could not encode PNG thumbnail";
    return png;
}

}  // namespace jade
