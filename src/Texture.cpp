// Texture.cpp — implementation. Faithful port of core/texture.py (read path)
// plus the BGRA/DXT encoders (write path). DXT encoding delegates to the vendored
// quicktex BC1/BC3 encoders (third_party/quicktex), matching the toolkit.
#include "jade/Texture.hpp"

#include <algorithm>
#include <cstring>

#include "jade/Image.hpp"
#include "jade/SubEntry.hpp"

// Vendored quicktex (Apache-2.0). Third-party templates: silence their warnings
// so they don't drown out our own under -Wall -Wextra.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#include "quicktex/Texture.h"
#include "quicktex/s3tc/bc1/BC1Encoder.h"
#include "quicktex/s3tc/bc3/BC3Encoder.h"
#pragma GCC diagnostic pop

namespace jade {

namespace {

inline uint16_t le16(const uint8_t* p, size_t o) {
    return static_cast<uint16_t>(p[o] | (p[o + 1] << 8));
}
inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}
inline uint64_t le48(const uint8_t* p, size_t o) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v |= static_cast<uint64_t>(p[o + i]) << (8 * i);
    return v;
}

struct RGB { int r, g, b; };

// Standard 565 -> 888 bit-replication (what texture2ddecoder uses for DXT1/5).
inline RGB expand_std(uint16_t c) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}
// Legacy *255/N rounding (texture.py _decode_rgb565, used by DXT3 + grading).
inline RGB expand_legacy(uint16_t c) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return {r * 255 / 31, g * 255 / 63, b * 255 / 31};
}
// texture.py _encode_rgb565.
inline int encode_rgb565(int r, int g, int b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
inline void put_le32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = x & 0xFF; v[o + 1] = (x >> 8) & 0xFF;
    v[o + 2] = (x >> 16) & 0xFF; v[o + 3] = (x >> 24) & 0xFF;
}
// texture.py _remap_lut: re-index a 2-bit/pixel LUT through mapping[old]=new.
inline uint32_t remap_lut(uint32_t lut, const int mapping[4]) {
    uint32_t out = 0;
    for (int i = 0; i < 16; ++i)
        out |= static_cast<uint32_t>(mapping[(lut >> (2 * i)) & 3]) << (2 * i);
    return out;
}

// BC1-style 4-colour palette. is3 (c0<=c1) -> idx2 is the average and idx3 is
// black. Endpoint expansion is selectable so DXT3 can use the legacy rounding.
void bc_colors(uint16_t c0, uint16_t c1, bool legacy, RGB col[4], bool* is3) {
    RGB a = legacy ? expand_legacy(c0) : expand_std(c0);
    RGB b = legacy ? expand_legacy(c1) : expand_std(c1);
    col[0] = a;
    col[1] = b;
    if (c0 > c1) {
        col[2] = {(2 * a.r + b.r) / 3, (2 * a.g + b.g) / 3, (2 * a.b + b.b) / 3};
        col[3] = {(a.r + 2 * b.r) / 3, (a.g + 2 * b.g) / 3, (a.b + 2 * b.b) / 3};
        *is3 = false;
    } else {
        col[2] = {(a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2};
        col[3] = {0, 0, 0};
        *is3 = true;
    }
}

inline void put(std::vector<uint8_t>& out, uint32_t w, uint32_t x, uint32_t y,
                int r, int g, int b, int a) {
    size_t o = (static_cast<size_t>(y) * w + x) * 4;
    out[o] = static_cast<uint8_t>(r);
    out[o + 1] = static_cast<uint8_t>(g);
    out[o + 2] = static_cast<uint8_t>(b);
    out[o + 3] = static_cast<uint8_t>(a);
}

std::vector<uint8_t> decode_bgra(const uint8_t* d, size_t n, uint32_t w, uint32_t h) {
    size_t need = static_cast<size_t>(w) * h * 4;
    if (n < need) return {};
    std::vector<uint8_t> out(need);
    for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p) {
        out[4 * p + 0] = d[4 * p + 2];
        out[4 * p + 1] = d[4 * p + 1];
        out[4 * p + 2] = d[4 * p + 0];
        out[4 * p + 3] = d[4 * p + 3];
    }
    return out;
}

std::vector<uint8_t> decode_pal8(const uint8_t* d, size_t n, uint32_t w, uint32_t h,
                                 const uint8_t* pal, size_t pal_len) {
    size_t need = static_cast<size_t>(w) * h;
    if (n < need) return {};
    std::vector<uint8_t> out(need * 4);
    if (pal && pal_len >= 1024) {
        for (size_t p = 0; p < need; ++p) {
            uint8_t idx = d[p];
            const uint8_t* e = pal + static_cast<size_t>(idx) * 4;
            out[4 * p + 0] = e[2];  // BGRA -> RGBA
            out[4 * p + 1] = e[1];
            out[4 * p + 2] = e[0];
            out[4 * p + 3] = e[3];
        }
    } else {  // greyscale fallback
        for (size_t p = 0; p < need; ++p) {
            out[4 * p + 0] = out[4 * p + 1] = out[4 * p + 2] = d[p];
            out[4 * p + 3] = 255;
        }
    }
    return out;
}

std::vector<uint8_t> decode_4bpp(const uint8_t* d, size_t n, uint32_t w, uint32_t h) {
    size_t need = (static_cast<size_t>(w) * h + 1) / 2;
    if (n < need) return {};
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t bi = (static_cast<size_t>(y) * w + x) / 2;
            int v = (x % 2 == 0) ? (d[bi] & 0x0F) * 17 : ((d[bi] >> 4) & 0x0F) * 17;
            put(out, w, x, y, v, v, v, 255);
        }
    }
    return out;
}

// DXT1 (BC1): standard expand, idx3 transparent-black in c0<=c1 blocks
// (= texture2ddecoder 4-colour decode + the toolkit's punch-through overlay).
std::vector<uint8_t> decode_dxt1(const uint8_t* d, size_t n, uint32_t w, uint32_t h) {
    uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    size_t need = static_cast<size_t>(bw) * bh * 8;
    std::vector<uint8_t> buf(need, 0);
    std::copy(d, d + std::min(n, need), buf.begin());

    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            size_t off = (static_cast<size_t>(by) * bw + bx) * 8;
            uint16_t c0 = le16(buf.data(), off), c1 = le16(buf.data(), off + 2);
            uint32_t lut = le32(buf.data(), off + 4);
            RGB col[4];
            bool is3;
            bc_colors(c0, c1, /*legacy=*/false, col, &is3);
            for (uint32_t ty = 0; ty < 4; ++ty)
                for (uint32_t tx = 0; tx < 4; ++tx) {
                    uint32_t px = bx * 4 + tx, py = by * 4 + ty;
                    if (px >= w || py >= h) continue;
                    int idx = (lut >> (2 * (4 * ty + tx))) & 0x3;
                    int a = (is3 && idx == 3) ? 0 : 255;
                    put(out, w, px, py, col[idx].r, col[idx].g, col[idx].b, a);
                }
        }
    }
    return out;
}

// DXT5 (BC3): t2d color logic + floor-interpolated alpha block.
std::vector<uint8_t> decode_dxt5(const uint8_t* d, size_t n, uint32_t w, uint32_t h) {
    uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    size_t need = static_cast<size_t>(bw) * bh * 16;
    std::vector<uint8_t> buf(need, 0);
    std::copy(d, d + std::min(n, need), buf.begin());

    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            size_t off = (static_cast<size_t>(by) * bw + bx) * 16;
            int a0 = buf[off], a1 = buf[off + 1];
            int alut[8];
            alut[0] = a0;
            alut[1] = a1;
            if (a0 > a1) {
                for (int i = 1; i <= 6; ++i) alut[i + 1] = ((7 - i) * a0 + i * a1) / 7;
            } else {
                for (int i = 1; i <= 4; ++i) alut[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                alut[6] = 0;
                alut[7] = 255;
            }
            uint64_t abits = le48(buf.data(), off + 2);

            uint16_t c0 = le16(buf.data(), off + 8), c1 = le16(buf.data(), off + 10);
            uint32_t clut = le32(buf.data(), off + 12);
            RGB col[4];
            bool is3;
            bc_colors(c0, c1, /*legacy=*/false, col, &is3);

            for (uint32_t ty = 0; ty < 4; ++ty)
                for (uint32_t tx = 0; tx < 4; ++tx) {
                    uint32_t px = bx * 4 + tx, py = by * 4 + ty;
                    if (px >= w || py >= h) continue;
                    int p = 4 * ty + tx;
                    int alpha = alut[(abits >> (3 * p)) & 7];
                    int ci = (clut >> (2 * p)) & 0x3;
                    put(out, w, px, py, col[ci].r, col[ci].g, col[ci].b, alpha);
                }
        }
    }
    return out;
}

// DXT3 (BC2): the toolkit's hand-rolled path — legacy 565 rounding + 4-bit
// explicit alpha. Floor block dims, no padding (breaks on a short block).
std::vector<uint8_t> decode_dxt3(const uint8_t* d, size_t n, uint32_t w, uint32_t h) {
    uint32_t bw = std::max(1u, w / 4), bh = std::max(1u, h / 4);
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            size_t off = (static_cast<size_t>(by) * bw + bx) * 16;
            if (off + 16 > n) return out;  // Python: break out of both loops via short data
            int alpha[16];
            for (int row = 0; row < 4; ++row) {
                uint16_t rv = le16(d, off + row * 2);
                for (int coli = 0; coli < 4; ++coli)
                    alpha[row * 4 + coli] = ((rv >> (coli * 4)) & 0x0F) * 17;
            }
            uint16_t c0 = le16(d, off + 8), c1 = le16(d, off + 10);
            uint32_t clut = le32(d, off + 12);
            RGB col[4];
            bool is3;
            bc_colors(c0, c1, /*legacy=*/true, col, &is3);
            for (uint32_t ty = 0; ty < 4; ++ty)
                for (uint32_t tx = 0; tx < 4; ++tx) {
                    uint32_t px = bx * 4 + tx, py = by * 4 + ty;
                    if (px >= w || py >= h) continue;
                    int p = 4 * ty + tx;
                    int ci = (clut >> (2 * p)) & 0x3;
                    put(out, w, px, py, col[ci].r, col[ci].g, col[ci].b, alpha[p]);
                }
        }
    }
    return out;
}

}  // namespace

bool is_texture_entry(const uint8_t* d, size_t n) {
    if (n < TEX_HEADER_SIZE + 8) return false;
    return le32(d, 0) == 0xFFFFFFFFu && le32(d, 20) == TEX_MAGIC_1 &&
           le32(d, 28) == TEX_MAGIC_2;
}

TexInfo parse_texture(const uint8_t* d, size_t n) {
    TexInfo ti;
    if (!is_texture_entry(d, n)) return ti;

    uint32_t log_w = le16(d, 8), log_h = le16(d, 10);
    if (log_w == 0 || log_h == 0 || log_w > 4096 || log_h > 4096) return ti;

    uint32_t version = le32(d, 32), fmt = le32(d, 36), mip = le32(d, 48);
    if (mip > 20) return ti;

    uint32_t w = log_w, h = log_h;
    if (n >= 48) {  // HDR_ACTUAL_H_OFF (44) + 4
        uint32_t aw = le32(d, 40), ah = le32(d, 44);
        if (aw > 0 && aw <= 4096 && ah > 0 && ah <= 4096 && (aw != log_w || ah != log_h)) {
            w = aw;
            h = ah;
        }
    }

    size_t pix_start = TEX_PIXDATA_OFF;
    std::vector<uint8_t> prefix;
    if (fmt == 1 || fmt == 5) {
        size_t end = std::min(TEX_PIXDATA_OFF + 4, n);
        prefix.assign(d + std::min(TEX_PIXDATA_OFF, n), d + end);
        pix_start = TEX_PIXDATA_OFF + 4;
    } else if (fmt == 11 && version >= 4) {
        size_t end = std::min(TEX_PIXDATA_OFF + 8, n);
        prefix.assign(d + std::min(TEX_PIXDATA_OFF, n), d + end);
        pix_start = TEX_PIXDATA_OFF + 8;
    }

    ti.valid = true;
    ti.width = w;
    ti.height = h;
    ti.logical_width = log_w;
    ti.logical_height = log_h;
    ti.format = fmt;
    ti.version = version;
    ti.mip_count = mip;
    ti.pix_start = pix_start;
    ti.prefix = std::move(prefix);
    return ti;
}

size_t base_level_size(uint32_t fmt, uint32_t w, uint32_t h) {
    size_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    switch (fmt) {
        case 0:  return static_cast<size_t>(w) * h * 4;
        case 1:  return static_cast<size_t>(w) * h;
        case 5:  return bw * bh * 8;
        case 6:
        case 7:  return bw * bh * 16;
        case 11: return (static_cast<size_t>(w) * h + 1) / 2;
        default: return 0;
    }
}

bool is_placeholder(const TexInfo& ti, size_t pixdata_len) {
    if (!ti.valid) return true;
    size_t need = base_level_size(ti.format, ti.width, ti.height);
    return need == 0 || pixdata_len < need;
}

std::vector<uint8_t> decode_texture(const uint8_t* d, size_t n, const TexInfo& ti,
                                    const uint8_t* palette, size_t pal_len) {
    uint32_t w = ti.width, h = ti.height;
    const uint8_t* pix = d + std::min(ti.pix_start, n);
    size_t pixlen = (ti.pix_start <= n) ? n - ti.pix_start : 0;

    switch (ti.format) {
        case 0: return decode_bgra(pix, pixlen, w, h);
        case 1: return decode_pal8(pix, pixlen, w, h, palette, pal_len);
        case 5: {
            size_t base = std::max(1u, (w + 3) / 4) * static_cast<size_t>(std::max(1u, (h + 3) / 4)) * 8;
            return decode_dxt1(pix, std::min(pixlen, base), w, h);
        }
        case 6: {
            size_t base = std::max(1u, w / 4) * static_cast<size_t>(std::max(1u, h / 4)) * 16;
            return decode_dxt3(pix, pixlen >= base ? base : pixlen, w, h);
        }
        case 7: {
            size_t base = std::max(1u, w / 4) * static_cast<size_t>(std::max(1u, h / 4)) * 16;
            return decode_dxt5(pix, pixlen >= base ? base : pixlen, w, h);
        }
        case 11: return decode_4bpp(pix, pixlen, w, h);
        default: return {};
    }
}

std::vector<size_t> find_texture_offsets(const uint8_t* d, size_t n) {
    std::vector<size_t> out;
    if (n < TEX_HEADER_SIZE + 8) return out;
    size_t last = n - (TEX_HEADER_SIZE + 8);
    for (size_t o = 0; o <= last; ++o) {
        if (le32(d, o) == 0xFFFFFFFFu && le32(d, o + 20) == TEX_MAGIC_1 &&
            le32(d, o + 28) == TEX_MAGIC_2) {
            out.push_back(o);
        }
    }
    return out;
}

namespace {
// texture.py _grade_color_block: grade one 8-byte DXT colour sub-block, keeping
// the c0>c1 vs c0<=c1 block mode even when grading flips the endpoint order.
void grade_color_block(std::vector<uint8_t>& buf, size_t off, const ColorFn& fn) {
    int c0 = buf[off] | (buf[off + 1] << 8);
    int c1 = buf[off + 2] | (buf[off + 3] << 8);
    uint32_t lut = le32(buf.data(), off + 4);
    bool mode_gt = c0 > c1;
    RGB d0 = expand_legacy(static_cast<uint16_t>(c0));
    RGB d1 = expand_legacy(static_cast<uint16_t>(c1));
    std::array<int, 3> g0 = fn({d0.r, d0.g, d0.b});
    std::array<int, 3> g1 = fn({d1.r, d1.g, d1.b});
    int n0 = encode_rgb565(g0[0], g0[1], g0[2]);
    int n1 = encode_rgb565(g1[0], g1[1], g1[2]);
    if ((n0 > n1) != mode_gt) {                       // order flipped -> restore mode
        static const int M_gt[4] = {1, 0, 3, 2};      // {0:1,1:0,2:3,3:2}
        static const int M_le[4] = {1, 0, 2, 3};      // {0:1,1:0,2:2,3:3}
        lut = remap_lut(lut, mode_gt ? M_gt : M_le);
        std::swap(n0, n1);
        if (mode_gt && n0 <= n1) {                    // equal after swap -> force c0>c1
            if (n0 < 0xFFFF) n0 += 1;
            else if (n1 > 0) n1 -= 1;
        }
    }
    buf[off] = n0 & 0xFF; buf[off + 1] = (n0 >> 8) & 0xFF;
    buf[off + 2] = n1 & 0xFF; buf[off + 3] = (n1 >> 8) & 0xFF;
    put_le32(buf, off + 4, lut);
}
}  // namespace

std::vector<uint8_t> grade_texture(const uint8_t* d, size_t n, const ColorFn& color_fn) {
    TexInfo ti = parse_texture(d, n);
    if (!ti.valid) return {};
    size_t pix_start = ti.pix_start;     // = header(52) + prefix, like the Python
    std::vector<uint8_t> buf(d, d + n);

    if (ti.format == 5) {                              // DXT1: 8-byte colour blocks
        for (size_t o = pix_start; o + 8 <= n; o += 8) grade_color_block(buf, o, color_fn);
    } else if (ti.format == 6 || ti.format == 7) {     // DXT3/5: 8 alpha + 8 colour
        for (size_t o = pix_start; o + 16 <= n; o += 16) grade_color_block(buf, o + 8, color_fn);
    } else if (ti.format == 0) {                       // BGRA uncompressed
        for (size_t o = pix_start; o + 4 <= n; o += 4) {
            std::array<int, 3> ng = color_fn({buf[o + 2], buf[o + 1], buf[o]});
            buf[o] = ng[2] & 0xFF; buf[o + 1] = ng[1] & 0xFF; buf[o + 2] = ng[0] & 0xFF;
        }
    } else {
        return {};                                     // PAL8 / 4-bit: palette-indexed
    }
    if (std::equal(buf.begin(), buf.end(), d)) return {};  // no change -> None
    return buf;
}

std::vector<uint8_t> encode_bgra(const uint8_t* rgba, uint32_t w, uint32_t h) {
    size_t n = static_cast<size_t>(w) * h * 4;
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; i += 4) {
        out[i]     = rgba[i + 2];   // B
        out[i + 1] = rgba[i + 1];   // G
        out[i + 2] = rgba[i];       // R
        out[i + 3] = rgba[i + 3];   // A
    }
    return out;
}

namespace {

// Faithful port of texture.py _encode_dxt1_hand — the punch-through DXT1 fallback
// used (even with quicktex present) when the source carries alpha < 128. Pure
// integer min/max endpoints + legacy 565 decode for the palette, so byte-exact.
std::vector<uint8_t> encode_dxt1_hand(const uint8_t* px, uint32_t w, uint32_t h) {
    uint32_t bw = std::max(1u, w / 4), bh = std::max(1u, h / 4);
    std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * 8, 0);

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            int rgb[16][3];
            int alpha[16];
            for (int ty = 0; ty < 4; ++ty) {
                for (int tx = 0; tx < 4; ++tx) {
                    uint32_t pxx = std::min(bx * 4u + tx, w - 1);
                    uint32_t pyy = std::min(by * 4u + ty, h - 1);
                    const uint8_t* p = px + (static_cast<size_t>(pyy) * w + pxx) * 4;
                    int k = ty * 4 + tx;
                    rgb[k][0] = p[0]; rgb[k][1] = p[1]; rgb[k][2] = p[2];
                    alpha[k] = p[3];
                }
            }
            bool transp[16];
            bool has_t = false;
            for (int i = 0; i < 16; ++i) { transp[i] = alpha[i] < 128; has_t = has_t || transp[i]; }

            int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
            bool saw_opaque = false;
            for (int i = 0; i < 16; ++i) {
                if (transp[i]) continue;
                saw_opaque = true;
                for (int c = 0; c < 3; ++c) {
                    mn[c] = std::min(mn[c], rgb[i][c]);
                    mx[c] = std::max(mx[c], rgb[i][c]);
                }
            }
            size_t bo = (static_cast<size_t>(by) * bw + bx) * 8;
            if (!saw_opaque) {                       // wholly transparent: 0,0,0xFFFFFFFF
                out[bo + 4] = out[bo + 5] = out[bo + 6] = out[bo + 7] = 0xFF;
                continue;
            }

            int c0 = encode_rgb565(mx[0], mx[1], mx[2]);
            int c1 = encode_rgb565(mn[0], mn[1], mn[2]);
            RGB pal[4];
            bool valid[4] = {true, true, true, true};
            if (has_t) {
                if (c0 > c1) std::swap(c0, c1);       // punch-through requires c0 <= c1
                RGB e0 = expand_legacy(static_cast<uint16_t>(c0));
                RGB e1 = expand_legacy(static_cast<uint16_t>(c1));
                pal[0] = e0; pal[1] = e1;
                pal[2] = {(e0.r + e1.r) / 2, (e0.g + e1.g) / 2, (e0.b + e1.b) / 2};
                valid[3] = false;                     // index 3 = transparent
            } else {
                if (c0 < c1) std::swap(c0, c1);
                RGB e0 = expand_legacy(static_cast<uint16_t>(c0));
                RGB e1 = expand_legacy(static_cast<uint16_t>(c1));
                pal[0] = e0; pal[1] = e1;
                pal[2] = {(2 * e0.r + e1.r) / 3, (2 * e0.g + e1.g) / 3, (2 * e0.b + e1.b) / 3};
                pal[3] = {(e0.r + 2 * e1.r) / 3, (e0.g + 2 * e1.g) / 3, (e0.b + 2 * e1.b) / 3};
            }

            uint32_t lut = 0;
            for (int i = 0; i < 16; ++i) {
                if (transp[i] && has_t) { lut |= 3u << (2 * i); continue; }
                int best = 0, best_d = 999999;
                for (int pi = 0; pi < 4; ++pi) {
                    if (!valid[pi]) continue;
                    int dr = rgb[i][0] - pal[pi].r, dg = rgb[i][1] - pal[pi].g, db = rgb[i][2] - pal[pi].b;
                    int d = dr * dr + dg * dg + db * db;
                    if (d < best_d) { best_d = d; best = pi; }
                }
                lut |= static_cast<uint32_t>(best) << (2 * i);
            }
            out[bo + 0] = c0 & 0xFF; out[bo + 1] = (c0 >> 8) & 0xFF;
            out[bo + 2] = c1 & 0xFF; out[bo + 3] = (c1 >> 8) & 0xFF;
            put_le32(out, bo + 4, lut);
        }
    }
    return out;
}

}  // namespace

std::vector<uint8_t> encode_dxt1(const uint8_t* rgba, uint32_t w, uint32_t h) {
    size_t npix = static_cast<size_t>(w) * h;
    bool has_alpha = false;
    for (size_t i = 0; i < npix; ++i)
        if (rgba[i * 4 + 3] < 128) { has_alpha = true; break; }
    if (has_alpha)
        return encode_dxt1_hand(rgba, w, h);          // quicktex ignores source alpha
    quicktex::RawTexture tex(static_cast<int>(w), static_cast<int>(h));
    std::memcpy(tex.Data(), rgba, npix * 4);          // Color == {r,g,b,a}
    quicktex::s3tc::BC1Encoder enc;                   // level 5, FourColor, Ideal
    auto blocks = enc.Encode(tex);
    return std::vector<uint8_t>(blocks.Data(), blocks.Data() + blocks.NBytes());
}

std::vector<uint8_t> encode_dxt5(const uint8_t* rgba, uint32_t w, uint32_t h) {
    size_t npix = static_cast<size_t>(w) * h;
    quicktex::RawTexture tex(static_cast<int>(w), static_cast<int>(h));
    std::memcpy(tex.Data(), rgba, npix * 4);
    quicktex::s3tc::BC3Encoder enc;                   // level 5, Ideal
    auto blocks = enc.Encode(tex);
    return std::vector<uint8_t>(blocks.Data(), blocks.Data() + blocks.NBytes());
}

namespace {
inline void dds_u32(std::vector<uint8_t>& h, size_t o, uint32_t v) {
    h[o] = uint8_t(v); h[o + 1] = uint8_t(v >> 8);
    h[o + 2] = uint8_t(v >> 16); h[o + 3] = uint8_t(v >> 24);
}
std::vector<uint8_t> dds_header() {
    std::vector<uint8_t> h(128, 0);
    h[0] = 'D'; h[1] = 'D'; h[2] = 'S'; h[3] = ' ';
    dds_u32(h, 4, 124);
    return h;
}
}  // namespace

std::vector<uint8_t> write_dds(const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> dxt1 = encode_dxt1(rgba, w, h);
    std::vector<uint8_t> out = dds_header();
    dds_u32(out, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);
    dds_u32(out, 12, h);
    dds_u32(out, 16, w);
    dds_u32(out, 20, uint32_t(dxt1.size()));
    dds_u32(out, 76, 32);
    dds_u32(out, 80, 0x4);                 // DDPF_FOURCC
    out[84] = 'D'; out[85] = 'X'; out[86] = 'T'; out[87] = '1';
    dds_u32(out, 108, 0x1000);             // TEXTURE
    out.insert(out.end(), dxt1.begin(), dxt1.end());
    return out;
}

std::vector<uint8_t> write_dds_raw(const uint8_t* d, size_t n, const TexInfo& ti,
                                   const uint8_t* palette, size_t pal_len) {
    uint32_t w = ti.width, h = ti.height, fmt = ti.format;
    std::vector<uint8_t> out = dds_header();
    if (fmt == 5 || fmt == 6 || fmt == 7) {
        const char* fourcc = fmt == 5 ? "DXT1" : fmt == 6 ? "DXT3" : "DXT5";
        uint32_t bpb = fmt == 5 ? 8 : 16;
        uint32_t bw = std::max<uint32_t>(1, w / 4), bh = std::max<uint32_t>(1, h / 4);
        size_t base_size = size_t(bw) * bh * bpb;
        size_t avail = ti.pix_start < n ? n - ti.pix_start : 0;
        size_t take = std::min(base_size, avail);      // Python slice clamps
        dds_u32(out, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);
        dds_u32(out, 12, h);
        dds_u32(out, 16, w);
        dds_u32(out, 20, uint32_t(take));
        dds_u32(out, 76, 32);
        dds_u32(out, 80, 0x4);
        std::memcpy(out.data() + 84, fourcc, 4);
        dds_u32(out, 108, 0x1000);
        out.insert(out.end(), d + ti.pix_start, d + ti.pix_start + take);
        return out;
    }
    std::vector<uint8_t> rgba = decode_texture(d, n, ti, palette, pal_len);
    if (rgba.empty()) return {};                       // Python raises ValueError
    dds_u32(out, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8);
    dds_u32(out, 12, h);
    dds_u32(out, 16, w);
    dds_u32(out, 20, w * 4);
    dds_u32(out, 76, 32);
    dds_u32(out, 80, 0x41);                            // RGB | ALPHAPIXELS
    dds_u32(out, 88, 32);
    dds_u32(out, 92, 0x00FF0000);
    dds_u32(out, 96, 0x0000FF00);
    dds_u32(out, 100, 0x000000FF);
    dds_u32(out, 104, 0xFF000000);
    dds_u32(out, 108, 0x1000);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {  // RGBA -> BGRA
        out.push_back(rgba[i + 2]);
        out.push_back(rgba[i + 1]);
        out.push_back(rgba[i]);
        out.push_back(rgba[i + 3]);
    }
    return out;
}

DdsImage read_dds(const uint8_t* d, size_t n) {
    DdsImage img;
    if (n < 128 || std::memcmp(d, "DDS ", 4) != 0) return img;
    auto u32 = [&](size_t o) {
        return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) |
               (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24);
    };
    // Offsets below are header-relative like the Python (header = d+4..d+128).
    uint32_t height = u32(4 + 8), width = u32(4 + 12);
    uint32_t pf_flags = u32(4 + 76);
    const uint8_t* fourcc = d + 4 + 80;
    uint32_t bpp = u32(4 + 84);
    uint32_t rmask = u32(4 + 88), gmask = u32(4 + 92), bmask = u32(4 + 96),
             amask = u32(4 + 100);
    const uint8_t* data = d + 128;
    size_t dlen = n - 128;

    auto done = [&](std::vector<uint8_t> rgba) {
        img.rgba = std::move(rgba);
        img.width = width;
        img.height = height;
        img.ok = !img.rgba.empty();
        return img;
    };
    if (std::memcmp(fourcc, "DXT1", 4) == 0)
        return done(decode_dxt1(data, dlen, width, height));
    if (std::memcmp(fourcc, "DXT3", 4) == 0)
        return done(decode_dxt3(data, dlen, width, height));
    if (std::memcmp(fourcc, "DXT5", 4) == 0)
        return done(decode_dxt5(data, dlen, width, height));

    if ((pf_flags & 0x40) && bpp == 32) {
        size_t need = size_t(width) * height * 4;
        if (dlen < need) return img;
        auto pick = [](uint32_t mask) -> int {
            if (mask == 0x000000FF) return 0;
            if (mask == 0x0000FF00) return 1;
            if (mask == 0x00FF0000) return 2;
            if (mask == 0xFF000000) return 3;
            return -1;
        };
        int ri = pick(rmask), gi = pick(gmask), bi = pick(bmask);
        int ai = ((pf_flags & 0x1) && amask) ? pick(amask) : -1;
        if (ri < 0 || gi < 0 || bi < 0) return img;
        std::vector<uint8_t> rgba(need);
        for (size_t p = 0; p < need; p += 4) {
            rgba[p] = data[p + size_t(ri)];
            rgba[p + 1] = data[p + size_t(gi)];
            rgba[p + 2] = data[p + size_t(bi)];
            rgba[p + 3] = ai >= 0 ? data[p + size_t(ai)] : 255;
        }
        return done(std::move(rgba));
    }
    return img;
}

const std::vector<uint8_t>* palette_for_texture(
    const TexInfo& ti, const std::vector<SubEntry>& subs) {
    if (!ti.valid || ti.format != 1) return nullptr;
    bool has_key = ti.prefix.size() == 4;
    uint32_t pal_key = 0;
    if (has_key)
        pal_key = static_cast<uint32_t>(ti.prefix[0]) |
                  (static_cast<uint32_t>(ti.prefix[1]) << 8) |
                  (static_cast<uint32_t>(ti.prefix[2]) << 16) |
                  (static_cast<uint32_t>(ti.prefix[3]) << 24);
    const std::vector<uint8_t>* first = nullptr;
    for (const SubEntry& s : subs) {
        const std::vector<uint8_t>& d = s.data;
        if (d.size() != 1024 || is_texture_entry(d.data(), d.size())) continue;
        if (first == nullptr) first = &d;
        if (has_key && s.key == pal_key) return &d;
    }
    return first;
}

std::vector<std::vector<uint8_t>> find_palettes(
    const std::vector<SubEntry>& subs) {
    std::vector<std::vector<uint8_t>> out;
    for (const SubEntry& sub : subs)
        if (sub.data.size() == 1024
            && !is_texture_entry(sub.data.data(), sub.data.size()))
            out.push_back(sub.data);
    return out;
}

std::vector<uint8_t> generate_mipmaps(const uint8_t* rgba, uint32_t w,
                                      uint32_t h, uint32_t fmt,
                                      uint32_t mip_count,
                                      int64_t pixdata_len) {
    if (!rgba || w == 0 || h == 0) return {};
    const size_t base_bytes = size_t(w) * h * 4;
    std::vector<uint8_t> base(rgba, rgba + base_bytes);
    std::vector<uint8_t> result;
    auto append_level = [&](const std::vector<uint8_t>& level,
                            uint32_t lw, uint32_t lh) {
        std::vector<uint8_t> encoded;
        if (fmt == 0) encoded = encode_bgra(level.data(), lw, lh);
        else if (fmt == 5) encoded = encode_dxt1(level.data(), lw, lh);
        else if (fmt == 6 || fmt == 7)
            encoded = encode_dxt5(level.data(), lw, lh);
        else if (fmt == 1) {
            encoded.resize(size_t(lw) * lh);
            for (size_t i = 0; i < encoded.size(); ++i) {
                const unsigned sum = unsigned(level[i * 4])
                                   + unsigned(level[i * 4 + 1])
                                   + unsigned(level[i * 4 + 2]);
                encoded[i] = uint8_t(sum / 3u);
            }
        }
        result.insert(result.end(), encoded.begin(), encoded.end());
    };

    append_level(base, w, h);
    uint32_t mw = w, mh = h;
    for (uint32_t level = 0; level < mip_count; ++level) {
        mw = std::max<uint32_t>(1, mw / 2);
        mh = std::max<uint32_t>(1, mh / 2);
        const std::vector<uint8_t> resized =
            resize_rgba_lanczos(base.data(), w, h, mw, mh);
        append_level(resized, mw, mh);
    }
    if (pixdata_len >= 0) result.resize(size_t(pixdata_len), 0);
    return result;
}

}  // namespace jade
