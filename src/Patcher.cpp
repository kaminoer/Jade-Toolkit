// Patcher.cpp - public change-set reimport (io_ops/patcher.py).
#include "jade/Patcher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "jade/Compression.hpp"
#include "jade/Image.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/PatcherModel.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jade {
namespace patcher {
namespace {

void put16(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = uint8_t(x);
    v[o + 1] = uint8_t(x >> 8);
}

void put32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = uint8_t(x);
    v[o + 1] = uint8_t(x >> 8);
    v[o + 2] = uint8_t(x >> 16);
    v[o + 3] = uint8_t(x >> 24);
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("could not open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

bool ends_with_dds(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".dds";
}

void forward_log(const std::vector<std::string>& lines,
                 const PatchLogFn& log_fn) {
    for (const std::string& line : lines) log_fn(line);
}

struct ChangeGroup {
    uint32_t entry_index = 0;
    std::vector<const exporter::ChangeRec*> changes;
};

void patch_entry(BigFile& bf, std::fstream& f, uint32_t entry_idx,
                 const std::vector<const exporter::ChangeRec*>& changes,
                 const PatchLogFn& log_fn) {
    auto fit = bf.files.find(entry_idx);
    if (fit == bf.files.end()) {
        log_fn("  Entry " + std::to_string(entry_idx) + " not found in BF");
        return;
    }
    BFFile& fi = fit->second;

    std::vector<uint8_t> raw(fi.length);
    f.clear();
    f.seekg(static_cast<std::streamoff>(fi.pos), std::ios::beg);
    f.read(reinterpret_cast<char*>(raw.data()),
           static_cast<std::streamsize>(raw.size()));
    raw.resize(static_cast<size_t>(f.gcount()));
    LzoResult dr = decompress_lzo(raw);
    if (!dr.ok) throw std::runtime_error("could not decompress BF entry");
    const size_t original_size = dr.data.size();
    std::vector<uint8_t> patched = std::move(dr.data);

    for (const exporter::ChangeRec* ch : changes) {
        if (!std::filesystem::exists(
                std::filesystem::u8path(ch->full_path))) {
            log_fn("  File not found: " + ch->full_path);
            continue;
        }

        std::vector<std::string> lines;
        if (ch->category == "texture") {
            PatchTextureResult r = patch_texture(
                patched, ch->key, ch->full_path, "auto", lines);
            if (r.changed) patched = std::move(r.patched);
        } else if (ch->category == "scene") {
            PatchSceneResult r = patch_scene_glb(
                patched, read_file(ch->full_path), ch->full_path, lines);
            if (r.changed) patched = std::move(r.patched);
        } else if (ch->category == "model") {
            PatchModelResult r = patch_model_glb(
                patched, ch->key, read_file(ch->full_path),
                PatchModelOptions{}, lines);
            if (r.changed) patched = std::move(r.patched);
        } else if (ch->category == "animation") {
            // Explicitly deferred by the current porting scope.
            log_fn("  Skipping deferred category: animation");
            continue;
        } else {
            log_fn("  Skipping unsupported category: " + ch->category);
            continue;
        }
        forward_log(lines, log_fn);
    }

    std::vector<uint8_t> compressed = compress_lzo(patched, 9);
    uint32_t new_pos = bf.write_entry(f, compressed, fi);
    char b[128];
    std::snprintf(b, sizeof b, "  Entry %u: %zuB \xe2\x86\x92 %zuB at 0x%X",
                  entry_idx, original_size, compressed.size(), new_pos);
    log_fn(b);
}

}  // namespace

PatchTextureResult patch_texture(const std::vector<uint8_t>& dec,
                                 uint32_t sub_key,
                                 const std::string& image_path,
                                 const std::string& encode,
                                 std::vector<std::string>& log) {
    PatchTextureResult result;
    result.patched = dec;

    std::vector<SubEntry> subs = walk_sub_entries(dec);
    struct Occurrence {
        const SubEntry* sub = nullptr;
        TexInfo tex;
    };
    std::vector<Occurrence> occurrences;
    for (const SubEntry& sub : subs) {
        if (sub.key != sub_key ||
            !is_texture_entry(sub.data.data(), sub.data.size()))
            continue;
        TexInfo tex = parse_texture(sub.data.data(), sub.data.size());
        if (tex.valid) occurrences.push_back({&sub, std::move(tex)});
    }

    char b[256];
    if (occurrences.empty()) {
        std::snprintf(b, sizeof b,
                      "  Texture sub-entry 0x%08X not found", sub_key);
        log.push_back(b);
        return result;
    }

    const Occurrence* real = &occurrences.front();
    auto pix_len = [](const Occurrence& o) {
        return o.sub->data.size() -
               std::min(o.sub->data.size(), o.tex.pix_start);
    };
    for (const Occurrence& occurrence : occurrences)
        if (pix_len(occurrence) > pix_len(*real)) real = &occurrence;
    if (pix_len(*real) == 0) {
        std::snprintf(b, sizeof b,
                      "  Texture 0x%08X: only a header stub found "
                      "(no pixel data) \xe2\x80\x94 nothing to patch", sub_key);
        log.push_back(b);
        return result;
    }

    const uint32_t orig_fmt = real->tex.format;
    uint32_t target_fmt = orig_fmt;
    if (encode.empty() || encode == "auto" || encode == "Auto") {
        if (orig_fmt == 1 || orig_fmt == 11) target_fmt = 7;
    } else {
        char* end = nullptr;
        unsigned long parsed = std::strtoul(encode.c_str(), &end, 10);
        if (end != encode.c_str() && *end == '\0')
            target_fmt = uint32_t(parsed);
    }
    const bool fmt_changed = target_fmt != orig_fmt;

    RgbaImage image = load_rgba_image(image_path);
    if (!image.ok) {
        if (ends_with_dds(image_path)) {
            log.push_back("  Could not read DDS: " + image_path);
            return result;
        }
        throw std::runtime_error("could not read source image " + image_path +
                                 ": " + image.error);
    }

    const uint32_t source_w = image.width;
    const uint32_t source_h = image.height;
    const uint32_t new_w = nearest_power_of_two(source_w, 4, 2048);
    const uint32_t new_h = nearest_power_of_two(source_h, 4, 2048);
    if (new_w != source_w || new_h != source_h) {
        image.rgba = resize_rgba_lanczos(image.rgba.data(), source_w, source_h,
                                         new_w, new_h);
        if (image.rgba.empty())
            throw std::runtime_error("replace_texture: image resize failed");
        std::snprintf(b, sizeof b,
                      "  source snapped to %ux%u (power-of-two)", new_w,
                      new_h);
        log.push_back(b);
    }

    const uint32_t old_w = real->tex.width;
    const uint32_t old_h = real->tex.height;
    const bool resized = new_w != old_w || new_h != old_h;
    const size_t old_pix_len = pix_len(*real);
    uint32_t new_mip_count = real->tex.mip_count;
    std::vector<uint8_t> pixels;
    if (fmt_changed || target_fmt == 0 || target_fmt == 5 ||
        target_fmt == 6 || target_fmt == 7) {
        if (target_fmt == 0)
            pixels = encode_bgra(image.rgba.data(), new_w, new_h);
        else if (target_fmt == 5)
            pixels = encode_dxt1(image.rgba.data(), new_w, new_h);
        else if (target_fmt == 6 || target_fmt == 7)
            // Python deliberately writes DXT5 blocks for both fmt 6 and 7.
            pixels = encode_dxt5(image.rgba.data(), new_w, new_h);
        else
            pixels = generate_mipmaps(image.rgba.data(), new_w, new_h,
                                      target_fmt, real->tex.mip_count,
                                      resized ? -1 : int64_t(old_pix_len));
        new_mip_count = 0;
    } else {
        pixels = generate_mipmaps(image.rgba.data(), new_w, new_h, target_fmt,
                                  real->tex.mip_count,
                                  resized ? -1 : int64_t(old_pix_len));
    }

    // The Python helper assumes a complete 52-byte texture header after
    // parse_texture succeeds. Keep the same invariant explicit here.
    if (real->sub->data.size() < 52)
        throw std::runtime_error("texture header is truncated");
    std::vector<uint8_t> header(real->sub->data.begin(),
                                real->sub->data.begin() + 52);
    put16(header, 8, real->tex.logical_width);
    put16(header, 10, real->tex.logical_height);
    put32(header, 36, target_fmt);
    put32(header, 40, new_w);
    put32(header, 44, new_h);
    put32(header, 48, new_mip_count);

    std::vector<uint8_t> new_payload = std::move(header);
    if (target_fmt == 1 || target_fmt == 5) {
        if (orig_fmt == target_fmt && real->tex.prefix.size() == 4)
            new_payload.insert(new_payload.end(), real->tex.prefix.begin(),
                               real->tex.prefix.end());
        else
            new_payload.insert(new_payload.end(), {0, 0, 0, 0});
    }
    new_payload.insert(new_payload.end(), pixels.begin(), pixels.end());

    // Stub headers retain their record size. Splice the largest/full payload
    // last because doing so can invalidate every parsed offset.
    for (const Occurrence& occurrence : occurrences) {
        if (occurrence.sub == real->sub) continue;
        const size_t hp = occurrence.sub->offset + 12;
        put16(result.patched, hp + 8, real->tex.logical_width);
        put16(result.patched, hp + 10, real->tex.logical_height);
        put32(result.patched, hp + 36, target_fmt);
        put32(result.patched, hp + 40, new_w);
        put32(result.patched, hp + 44, new_h);
        put32(result.patched, hp + 48, new_mip_count);
    }

    const size_t old_len = real->sub->data.size();
    result.patched = splice_sub_entry(result.patched, real->sub->offset,
                                      real->sub->size, new_payload);
    result.changed = true;

    const std::string fmt_note = fmt_changed
                                     ? "fmt" + std::to_string(orig_fmt) + "->" +
                                           std::to_string(target_fmt)
                                     : "fmt" + std::to_string(target_fmt);
    const std::string dim_note = resized
                                     ? std::to_string(old_w) + "x" +
                                           std::to_string(old_h) + " -> " +
                                           std::to_string(new_w) + "x" +
                                           std::to_string(new_h)
                                     : std::to_string(new_w) + "x" +
                                           std::to_string(new_h);
    std::snprintf(b, sizeof b,
                  "  Patched TEX 0x%08X: %s %s (%zuB -> %zuB payload, %zu "
                  "stub header(s) synced)",
                  sub_key, dim_note.c_str(), fmt_note.c_str(), old_len,
                  new_payload.size(), occurrences.size() - 1);
    log.push_back(b);
    return result;
}

PatchBigFileStats patch_bigfile(BigFile& bf, const std::string& bf_path,
                                const std::string& out_dir,
                                const std::vector<exporter::ChangeRec>& changes,
                                PatchLogFn log_fn,
                                PatchProgressFn progress_fn) {
    (void)out_dir;  // Kept for exact API shape; ChangeRec already has full_path.
    if (!log_fn) log_fn = [](const std::string&) {};
    if (!progress_fn) progress_fn = [](size_t, size_t) {};

    PatchBigFileStats stats;
    if (changes.empty()) {
        log_fn("No changes to apply.");
        return stats;
    }

    namespace fs = std::filesystem;
    const fs::path archive = fs::u8path(bf_path);
    const fs::path backup = fs::u8path(bf_path + ".bak");
    if (!fs::exists(backup)) {
        log_fn("Creating backup: " + backup.u8string());
#ifdef _WIN32
        // shutil.copy2 retains Windows FILETIME precision. MinGW's
        // filesystem::last_write_time conversion rounds some timestamps to
        // whole seconds, whereas CopyFileW preserves the source metadata.
        if (!CopyFileW(archive.c_str(), backup.c_str(), TRUE)) {
            throw std::system_error(
                std::error_code(static_cast<int>(GetLastError()),
                                std::system_category()),
                "could not create backup");
        }
#else
        fs::copy_file(archive, backup);
        std::error_code ec;
        const fs::file_time_type t = fs::last_write_time(archive, ec);
        if (!ec) fs::last_write_time(backup, t, ec);
#endif
        stats.backup_created = true;
    } else {
        log_fn("Backup already exists: " + backup.u8string());
    }

    std::vector<ChangeGroup> groups;
    std::unordered_map<uint32_t, size_t> group_by_index;
    for (const exporter::ChangeRec& change : changes) {
        auto it = group_by_index.find(change.entry_index);
        if (it == group_by_index.end()) {
            const size_t pos = groups.size();
            group_by_index.emplace(change.entry_index, pos);
            groups.push_back({change.entry_index, {}});
            it = group_by_index.find(change.entry_index);
        }
        groups[it->second].changes.push_back(&change);
    }
    stats.entry_groups = groups.size();
    log_fn("Patching " + std::to_string(groups.size()) + " BF entries\xe2\x80\xa6");

    std::fstream f(archive, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) throw std::runtime_error("could not open BigFile for patching: " +
                                     bf_path);
    for (size_t i = 0; i < groups.size(); ++i) {
        const ChangeGroup& group = groups[i];
        progress_fn(i + 1, groups.size());
        try {
            patch_entry(bf, f, group.entry_index, group.changes, log_fn);
            if (bf.files.count(group.entry_index)) ++stats.entries_written;
        } catch (const std::exception& e) {
            ++stats.entry_errors;
            log_fn("  Error patching entry " +
                   std::to_string(group.entry_index) + ": " + e.what());
        }
    }
    stats.size_grs_rows = bf.flush_size_grs(f, log_fn);
    if (stats.size_grs_rows != 0)
        log_fn("size.grs: updated " + std::to_string(stats.size_grs_rows) +
               " declared stream length(s)");
    log_fn("Patch complete.");
    return stats;
}

}  // namespace patcher
}  // namespace jade
