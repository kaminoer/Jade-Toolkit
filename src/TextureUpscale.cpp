// TextureUpscale.cpp — implementation. Faithful port of
// io_ops/texture_upscale.py (native-resolution replace; no mips).
#include "jade/TextureUpscale.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Image.hpp"
#include "jade/MeshSwap.hpp"     // splice_sub_entry
#include "jade/Texture.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jade {
namespace texup {

namespace {
namespace fs = std::filesystem;

void emit(const UpscaleLogFn& log, const std::string& message) {
    if (log) log(message);
}

std::string basename(const std::string& path) {
    const fs::path value = fs::u8path(path).filename();
#ifdef _WIN32
    return value.u8string();
#else
    return value.string();
#endif
}

bool copy2(const fs::path& source, const fs::path& destination,
           std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
        ec = std::error_code(static_cast<int>(GetLastError()),
                             std::system_category());
        return false;
    }
    return true;  // CopyFileW preserves NTFS timestamps exactly.
#else
    if (!fs::copy_file(source, destination, fs::copy_options::overwrite_existing,
                       ec))
        return false;
    const fs::file_status status = fs::status(source, ec);
    if (ec) return false;
    fs::permissions(destination, status.permissions(), fs::perm_options::replace,
                    ec);
    if (ec) return false;
    const fs::file_time_type time = fs::last_write_time(source, ec);
    if (ec) return false;
    fs::last_write_time(destination, time, ec);
    return !ec;
#endif
}

bool create_backup_once(const std::string& bf_path, const UpscaleLogFn& log,
                        std::string& error) {
    const std::string backup_path = bf_path + ".bak";
    std::error_code ec;
    if (fs::exists(fs::u8path(backup_path), ec)) return true;
    emit(log, "Creating backup: " + backup_path);
    if (!copy2(fs::u8path(bf_path), fs::u8path(backup_path), ec)) {
        error = "could not create backup: " + ec.message();
        return false;
    }
    return true;
}

inline uint32_t get_u32(const uint8_t* d, size_t o) {
    return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
           (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}
inline void set_u32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = x & 0xFF; v[o + 1] = (x >> 8) & 0xFF;
    v[o + 2] = (x >> 16) & 0xFF; v[o + 3] = (x >> 24) & 0xFF;
}
}  // namespace

const SubEntry* pick_texture_sub(const std::vector<SubEntry>& subs, uint32_t tex_key) {
    const SubEntry* best = nullptr;
    for (const SubEntry& s : subs) {
        if (s.key != tex_key) continue;
        if (!is_texture_entry(s.data.data(), s.data.size())) continue;
        if (best == nullptr || s.data.size() > best->data.size()) best = &s;
    }
    return best;
}

std::vector<uint8_t> sync_texture_stub(const std::vector<uint8_t>& data,
                                       uint32_t new_w, uint32_t new_h, uint32_t new_fmt) {
    std::vector<uint8_t> out = data;
    set_u32(out, HDR_FMT_OFF, new_fmt);
    set_u32(out, HDR_W_OFF, new_w);
    set_u32(out, HDR_H_OFF, new_h);
    set_u32(out, HDR_MIPCOUNT_OFF, 0);
    if (new_fmt != 1 && out.size() > TEX_PIXDATA_OFF) {
        size_t prefix_len = out.size() - TEX_PIXDATA_OFF;
        size_t end = TEX_PIXDATA_OFF + std::min<size_t>(prefix_len, 8);
        for (size_t i = TEX_PIXDATA_OFF; i < end; ++i) out[i] = 0;
    }
    return out;
}

UpscalePayload build_upscaled_payload(const std::vector<uint8_t>& orig_payload,
                                      const uint8_t* rgba, uint32_t w, uint32_t h,
                                      uint32_t target_format) {
    UpscalePayload res;
    if (!is_texture_entry(orig_payload.data(), orig_payload.size())) {
        res.error = "Source payload is not a recognized texture header";
        return res;
    }
    uint32_t orig_fmt = get_u32(orig_payload.data(), HDR_FMT_OFF);
    if (target_format == 0xFFFFFFFFu)
        target_format = (orig_fmt == 1 || orig_fmt == 11) ? 7 : orig_fmt;
    if ((target_format == 5 || target_format == 7) && ((w % 4) || (h % 4))) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "DXT formats require dims divisible by 4 (got %ux%u)", w, h);
        res.error = buf;
        return res;
    }

    std::vector<uint8_t> pixdata;
    if (target_format == 0) pixdata = encode_bgra(rgba, w, h);
    else if (target_format == 5) pixdata = encode_dxt1(rgba, w, h);
    else if (target_format == 7) pixdata = encode_dxt5(rgba, w, h);
    else {
        char buf[80];
        std::snprintf(buf, sizeof buf,
                      "Unsupported target format: %u (use 0=BGRA, 5=DXT1, 7=DXT5)",
                      target_format);
        res.error = buf;
        return res;
    }

    std::vector<uint8_t> out(orig_payload.begin(),
                             orig_payload.begin() + long(TEX_PIXDATA_OFF));
    set_u32(out, HDR_FMT_OFF, target_format);
    set_u32(out, HDR_W_OFF, w);
    set_u32(out, HDR_H_OFF, h);
    set_u32(out, HDR_MIPCOUNT_OFF, 0);
    if (target_format == 5)                         // DXT1 pixels start at 56
        out.insert(out.end(), {0, 0, 0, 0});
    out.insert(out.end(), pixdata.begin(), pixdata.end());
    res.ok = true;
    res.payload = std::move(out);
    return res;
}

static UpscaleStats upscale_texture_impl(
        const std::string& bf_path, uint32_t entry_idx, uint32_t tex_key,
        const uint8_t* input_rgba, uint32_t input_w, uint32_t input_h,
        const std::string& image_path, uint32_t target_format, bool backup,
        const UpscaleLogFn& log) {
    UpscaleStats st;
    st.tex_key = tex_key;
    auto fail = [&](const std::string& m) { st.error = m; return st; };

    if (backup && !create_backup_once(bf_path, log, st.error)) return st;

    BigFile bf;
    try { bf.open(bf_path); } catch (const std::exception& e) { return fail(e.what()); }
    auto fit = bf.files.find(entry_idx);
    if (fit == bf.files.end())
        return fail("BF entry " + std::to_string(entry_idx) + " not found");
    BFFile& fi = fit->second;

    RgbaImage loaded;
    const uint8_t* rgba = input_rgba;
    uint32_t w = input_w, h = input_h;
    if (!image_path.empty()) {
        loaded = load_rgba_image(image_path);
        if (!loaded.ok) return fail(loaded.error);
        rgba = loaded.rgba.data();
        w = loaded.width;
        h = loaded.height;
    }
    if (!rgba || w == 0 || h == 0)
        return fail("RGBA image size does not match its dimensions");

    LzoResult dr = decompress_lzo(bf.read_data(entry_idx));
    if (!dr.ok) return fail("Could not decompress BF entry");
    const std::vector<uint8_t>& dec = dr.data;

    std::vector<SubEntry> subs = parse_sub_entries(dec);
    const SubEntry* target = pick_texture_sub(subs, tex_key);
    if (target == nullptr)
        return fail("No texture sub-entry with key 0x" + [&] {
            char text[16]; std::snprintf(text, sizeof text, "%08x", tex_key);
            return std::string(text);
        }() + " found in entry " + std::to_string(entry_idx));

    TexInfo orig_tex = parse_texture(target->data.data(), target->data.size());
    // Despite the public field's historical name, Python returns
    // parse_texture()['width'/'height'], i.e. effective (actual when valid).
    st.orig_logical_w = orig_tex.width;
    st.orig_logical_h = orig_tex.height;
    st.orig_actual_w = get_u32(target->data.data(), HDR_W_OFF);
    st.orig_actual_h = get_u32(target->data.data(), HDR_H_OFF);
    st.orig_format = orig_tex.format;

    UpscalePayload up = build_upscaled_payload(target->data, rgba, w, h, target_format);
    if (!up.ok) return fail(up.error);
    uint32_t new_fmt = get_u32(up.payload.data(), HDR_FMT_OFF);

    std::vector<uint8_t> new_dec =
        splice_sub_entry(dec, target->offset, target->size, up.payload);

    // Sync every stub occurrence's header (highest offset first).
    std::vector<SubEntry> occs_all = parse_sub_entries(new_dec);
    std::vector<const SubEntry*> occs;
    for (const SubEntry& s : occs_all)
        if (s.key == tex_key && is_texture_entry(s.data.data(), s.data.size()))
            occs.push_back(&s);
    if (occs.size() > 1) {
        const SubEntry* largest = occs[0];
        for (const SubEntry* s : occs)
            if (s->data.size() > largest->data.size()) largest = s;
        std::vector<const SubEntry*> stubs;
        for (const SubEntry* s : occs)
            if (s->offset != largest->offset &&
                s->data.size() <= TEX_PIXDATA_OFF + 16)
                stubs.push_back(s);
        std::sort(stubs.begin(), stubs.end(),
                  [](const SubEntry* a, const SubEntry* b) { return a->offset > b->offset; });
        for (const SubEntry* s : stubs) {
            std::vector<uint8_t> patched = sync_texture_stub(s->data, w, h, new_fmt);
            new_dec = splice_sub_entry(new_dec, s->offset, s->size, patched);
        }
        st.stubs_synced = static_cast<uint32_t>(stubs.size());
        if (!stubs.empty()) {
            char key_text[16];
            std::snprintf(key_text, sizeof key_text, "%08x", tex_key);
            emit(log, "  Synced " + std::to_string(stubs.size()) +
                      " stub occurrence(s) for 0x" + key_text);
        }
    }

    st.new_w = w;
    st.new_h = h;
    st.new_format = new_fmt;
    st.decompressed_size = new_dec.size();

    {
        char key_text[16];
        std::snprintf(key_text, sizeof key_text, "%08x", tex_key);
        emit(log, "Texture 0x" + std::string(key_text) + ": " +
                  std::to_string(st.orig_actual_w) + "x" +
                  std::to_string(st.orig_actual_h) + " fmt" +
                  std::to_string(st.orig_format) + " (" +
                  std::to_string(target->data.size()) + " bytes) -> " +
                  std::to_string(w) + "x" + std::to_string(h) + " fmt" +
                  std::to_string(new_fmt) + " (" +
                  std::to_string(up.payload.size()) + " bytes)");
    }
    emit(log, "BF entry " + std::to_string(entry_idx) + ": decompressed " +
              std::to_string(dec.size()) + " -> " +
              std::to_string(new_dec.size()) + " bytes");

    std::vector<uint8_t> compressed = compress_lzo(new_dec, 9);
    std::fstream f(fs::u8path(bf_path),
                   std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return fail("cannot open archive for writing");
    try {
        st.bf_entry_pos = bf.write_entry(f, compressed, fi);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    st.compressed_size = compressed.size();
    {
        std::ostringstream message;
        message << "Wrote compressed entry: " << compressed.size()
                << " bytes at 0x" << std::hex << st.bf_entry_pos;
        emit(log, message.str());
    }
    // NOTE: the Python op does NOT flush size.grs here (unlike swap_mesh_in_bf)
    // — mirrored for parity; flagged as a possible upstream quirk.
    st.ok = true;
    return st;
}

UpscaleStats upscale_texture_in_bf(const std::string& bf_path,
                                   uint32_t entry_idx, uint32_t tex_key,
                                   const uint8_t* rgba, uint32_t w, uint32_t h,
                                   uint32_t target_format, bool backup,
                                   UpscaleLogFn log) {
    return upscale_texture_impl(bf_path, entry_idx, tex_key, rgba, w, h, {},
                                target_format, backup, log);
}

UpscaleStats upscale_texture_in_bf(const std::string& bf_path,
                                   uint32_t entry_idx, uint32_t tex_key,
                                   const std::string& image_path,
                                   uint32_t target_format, bool backup,
                                   UpscaleLogFn log) {
    return upscale_texture_impl(bf_path, entry_idx, tex_key, nullptr, 0, 0,
                                image_path, target_format, backup, log);
}

BatchStats upscale_textures_in_bf(const std::string& bf_path, uint32_t entry_idx,
                                  const std::vector<BatchSpec>& specs, bool backup,
                                  UpscaleLogFn log) {
    BatchStats st;
    auto fail = [&](const std::string& m) { st.error = m; return st; };

    if (backup && !create_backup_once(bf_path, log, st.error)) return st;

    BigFile bf;
    try { bf.open(bf_path); } catch (const std::exception& e) { return fail(e.what()); }
    auto fit = bf.files.find(entry_idx);
    if (fit == bf.files.end())
        return fail("BF entry " + std::to_string(entry_idx) + " not found");
    BFFile& fi = fit->second;

    // Python opens and converts every image before it opens/decompresses the
    // BF entry, including images for keys that will later be skipped.
    struct DecodedSpec {
        uint32_t key = 0, w = 0, h = 0, format = 0xFFFFFFFFu;
        std::vector<uint8_t> rgba;
        std::string label;
    };
    std::vector<DecodedSpec> decoded_specs;
    decoded_specs.reserve(specs.size());
    for (const BatchSpec& spec : specs) {
        DecodedSpec decoded;
        decoded.key = spec.tex_key;
        decoded.format = spec.target_format;
        decoded.label = spec.label;
        if (!spec.image_path.empty()) {
            RgbaImage image = load_rgba_image(spec.image_path);
            if (!image.ok) return fail(image.error);
            decoded.w = image.width;
            decoded.h = image.height;
            decoded.rgba = std::move(image.rgba);
            if (decoded.label.empty()) decoded.label = basename(spec.image_path);
        } else {
            decoded.w = spec.w;
            decoded.h = spec.h;
            if (!spec.rgba || spec.w == 0 || spec.h == 0)
                return fail("RGBA image size does not match its dimensions");
            decoded.rgba.assign(spec.rgba,
                                spec.rgba + size_t(spec.w) * spec.h * 4);
        }
        decoded_specs.push_back(std::move(decoded));
    }

    LzoResult dr = decompress_lzo(bf.read_data(entry_idx));
    if (!dr.ok) return fail("Could not decompress BF entry");
    std::vector<uint8_t> dec = std::move(dr.data);

    for (const DecodedSpec& sp : decoded_specs) {
        std::vector<SubEntry> subs = parse_sub_entries(dec);
        const SubEntry* target = pick_texture_sub(subs, sp.key);
        if (target == nullptr) {
            ++st.skipped;
            char key_text[16];
            std::snprintf(key_text, sizeof key_text, "%08x", sp.key);
            emit(log, "  skip 0x" + std::string(key_text) +
                      ": no texture sub-entry found");
            continue;
        }

        UpscalePayload up = build_upscaled_payload(
            target->data, sp.rgba.data(), sp.w, sp.h, sp.format);
        if (!up.ok) return fail(up.error);   // Python: ValueError aborts unwritten
        uint32_t new_fmt = get_u32(up.payload.data(), HDR_FMT_OFF);

        char key_text[16];
        std::snprintf(key_text, sizeof key_text, "%08x", sp.key);
        emit(log, "  0x" + std::string(key_text) + ": " +
                  std::to_string(target->data.size()) + "B -> " +
                  std::to_string(up.payload.size()) + "B (" +
                  std::to_string(sp.w) + "x" + std::to_string(sp.h) +
                  " fmt" + std::to_string(new_fmt) + ")  [" + sp.label + "]");

        dec = splice_sub_entry(dec, target->offset, target->size, up.payload);

        std::vector<SubEntry> occs_all = parse_sub_entries(dec);
        std::vector<const SubEntry*> occs;
        for (const SubEntry& s : occs_all)
            if (s.key == sp.key && is_texture_entry(s.data.data(), s.data.size()))
                occs.push_back(&s);
        if (occs.size() > 1) {
            const SubEntry* largest = occs[0];
            for (const SubEntry* s : occs)
                if (s->data.size() > largest->data.size()) largest = s;
            std::vector<const SubEntry*> stubs;
            for (const SubEntry* s : occs)
                if (s->offset != largest->offset &&
                    s->data.size() <= TEX_PIXDATA_OFF + 16)
                    stubs.push_back(s);
            std::sort(stubs.begin(), stubs.end(),
                      [](const SubEntry* a, const SubEntry* b) { return a->offset > b->offset; });
            for (const SubEntry* s : stubs) {
                std::vector<uint8_t> patched = sync_texture_stub(s->data, sp.w, sp.h, new_fmt);
                dec = splice_sub_entry(dec, s->offset, s->size, patched);
            }
        }
        st.entries.push_back({sp.key, sp.w, sp.h, new_fmt});
    }

    st.decompressed_size = dec.size();
    emit(log, "BF entry " + std::to_string(entry_idx) +
              ": final decompressed size = " + std::to_string(dec.size()) +
              " bytes");
    std::vector<uint8_t> compressed = compress_lzo(dec, 9);
    std::fstream f(fs::u8path(bf_path),
                   std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return fail("cannot open archive for writing");
    try {
        st.bf_entry_pos = bf.write_entry(f, compressed, fi);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    st.compressed_size = compressed.size();
    {
        std::ostringstream message;
        message << "Wrote " << compressed.size() << "-byte entry at 0x"
                << std::hex << st.bf_entry_pos;
        emit(log, message.str());
    }
    // NOTE: no size.grs flush — mirrors the Python batch op (same quirk as above).
    st.ok = true;
    return st;
}

}  // namespace texup
}  // namespace jade
