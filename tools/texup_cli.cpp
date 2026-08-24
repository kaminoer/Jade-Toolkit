// texup_cli — run the native texture upscale on a BF entry.
//   texup_cli <bf> <entry_idx> <tex_key_hex> <rgba_raw> <w> <h> [target_fmt|auto]
//   texup_cli <bf> batch <entry_idx> <specfile>
//   texup_cli <bf> image <entry_idx> <tex_key_hex> <image> [fmt|auto] [--backup]
//   texup_cli <bf> batch-images <entry_idx> <specfile> [--backup]
// Raw specfile lines are "<key> <rgba_raw> <w> <h> <fmt|auto>"; image
// specfile lines are "<key> <image> <fmt|auto>". Prints operation logs plus
// one UPSCALE/BATCH stats line; ERROR + exit 1 on failure.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Texture.hpp"
#include "jade/TextureUpscale.hpp"
#include "jade/Utf8Args.hpp"

static bool read_all(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

static void print_log(const std::string& line) {
    std::printf("%s\n", line.c_str());
}

static void print_single(const jade::texup::UpscaleStats& st) {
    std::printf("UPSCALE key=%08x origlog=%ux%u origact=%ux%u origfmt=%u "
                "new=%ux%u newfmt=%u dec=%zu stubs=%u\n",
                st.tex_key, st.orig_logical_w, st.orig_logical_h,
                st.orig_actual_w, st.orig_actual_h, st.orig_format,
                st.new_w, st.new_h, st.new_format,
                st.decompressed_size, st.stubs_synced);
}

static void print_batch(const jade::texup::BatchStats& st) {
    std::printf("BATCH n=%zu skipped=%u dec=%zu", st.entries.size(), st.skipped,
                st.decompressed_size);
    for (const auto& e : st.entries)
        std::printf(" %08x:%ux%u:fmt%u", e.tex_key, e.new_w, e.new_h,
                    e.new_format);
    std::printf("\n");
}

static int run_batch(char** argv) {
    const char* bf_path = argv[1];
    uint32_t entry_idx = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0));
    std::ifstream sf(std::filesystem::u8path(argv[4]));
    if (!sf) { std::fprintf(stderr, "ERROR cannot open specfile\n"); return 1; }

    std::vector<std::vector<uint8_t>> buffers;
    std::vector<jade::texup::BatchSpec> specs;
    std::string key_s, rgba_s, fmt_s;
    uint32_t w = 0, h = 0;
    while (sf >> key_s >> rgba_s >> w >> h >> fmt_s) {
        jade::texup::BatchSpec sp;
        sp.tex_key = static_cast<uint32_t>(std::strtoul(key_s.c_str(), nullptr, 16));
        sp.w = w; sp.h = h;
        if (fmt_s != "auto")
            sp.target_format = static_cast<uint32_t>(std::strtoul(fmt_s.c_str(), nullptr, 0));
        buffers.emplace_back();
        if (!read_all(rgba_s.c_str(), buffers.back()) ||
            buffers.back().size() != size_t(w) * h * 4) {
            std::fprintf(stderr, "ERROR bad rgba file %s\n", rgba_s.c_str());
            return 1;
        }
        specs.push_back(sp);
    }
    for (size_t i = 0; i < specs.size(); ++i) specs[i].rgba = buffers[i].data();

    jade::texup::BatchStats st = jade::texup::upscale_textures_in_bf(
        bf_path, entry_idx, specs, /*backup=*/false);
    if (!st.ok) { std::fprintf(stderr, "ERROR %s\n", st.error.c_str()); return 1; }
    print_batch(st);
    return 0;
}

static int run_image(int argc, char** argv) {
    if (argc < 6 || argc > 8) return 2;
    const uint32_t entry = uint32_t(std::strtoul(argv[3], nullptr, 0));
    const uint32_t key = uint32_t(std::strtoul(argv[4], nullptr, 16));
    uint32_t format = 0xFFFFFFFFu;
    bool backup = false;
    for (int index = 6; index < argc; ++index) {
        if (std::strcmp(argv[index], "--backup") == 0) backup = true;
        else if (std::strcmp(argv[index], "auto") != 0)
            format = uint32_t(std::strtoul(argv[index], nullptr, 0));
    }
    jade::texup::UpscaleStats st = jade::texup::upscale_texture_in_bf(
        argv[1], entry, key, std::string(argv[5]), format, backup, print_log);
    if (!st.ok) { std::fprintf(stderr, "ERROR %s\n", st.error.c_str()); return 1; }
    print_single(st);
    return 0;
}

static int run_batch_images(int argc, char** argv) {
    if (argc < 5 || argc > 6) return 2;
    const uint32_t entry = uint32_t(std::strtoul(argv[3], nullptr, 0));
    const bool backup = argc == 6 && std::strcmp(argv[5], "--backup") == 0;
    std::ifstream file(std::filesystem::u8path(argv[4]));
    if (!file) { std::fprintf(stderr, "ERROR cannot open specfile\n"); return 1; }
    std::vector<jade::texup::BatchSpec> specs;
    std::string key_text, image_path, format_text;
    while (file >> key_text >> image_path >> format_text) {
        jade::texup::BatchSpec spec;
        spec.tex_key = uint32_t(std::strtoul(key_text.c_str(), nullptr, 16));
        spec.image_path = image_path;
        if (format_text != "auto")
            spec.target_format =
                uint32_t(std::strtoul(format_text.c_str(), nullptr, 0));
        specs.push_back(std::move(spec));
    }
    jade::texup::BatchStats st = jade::texup::upscale_textures_in_bf(
        argv[1], entry, specs, backup, print_log);
    if (!st.ok) { std::fprintf(stderr, "ERROR %s\n", st.error.c_str()); return 1; }
    print_batch(st);
    return 0;
}

// dds mode: exercise write_dds / write_dds_raw / read_dds on a real texture.
//   texup_cli <bf> dds <entry_idx> <tex_key_hex>
// Prints CRCs of both DDS byte streams and of the read_dds round-trip RGBA.
static int run_dds(char** argv) {
    uint32_t entry = uint32_t(std::strtoul(argv[3], nullptr, 0));
    uint32_t key = uint32_t(std::strtoul(argv[4], nullptr, 16));
    jade::BigFile bf;
    try { bf.open(argv[1]); } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }
    jade::LzoResult dr = jade::decompress_lzo(bf.read_data(entry));
    if (!dr.ok) { std::printf("ERROR decompress\n"); return 0; }
    std::vector<jade::SubEntry> subs = jade::walk_sub_entries(dr.data);
    const jade::SubEntry* t = jade::texup::pick_texture_sub(subs, key);
    if (!t) { std::printf("ERROR no texture\n"); return 0; }
    jade::TexInfo ti = jade::parse_texture(t->data.data(), t->data.size());
    const std::vector<uint8_t>* pal = jade::palette_for_texture(ti, subs);
    std::vector<uint8_t> rgba = jade::decode_texture(
        t->data.data(), t->data.size(), ti, pal ? pal->data() : nullptr,
        pal ? pal->size() : 0);
    if (rgba.empty()) { std::printf("ERROR decode\n"); return 0; }
    std::vector<uint8_t> c = jade::write_dds(rgba.data(), ti.width, ti.height);
    std::vector<uint8_t> raw = jade::write_dds_raw(
        t->data.data(), t->data.size(), ti, pal ? pal->data() : nullptr,
        pal ? pal->size() : 0);
    jade::DdsImage rc = jade::read_dds(c.data(), c.size());
    jade::DdsImage rr = jade::read_dds(raw.data(), raw.size());
    std::printf("DDS fmt=%u dds=%zu:%08x raw=%zu:%08x rb=%08x rbraw=%08x\n",
                ti.format, c.size(), jade::crc32(c.data(), c.size()),
                raw.size(), jade::crc32(raw.data(), raw.size()),
                rc.ok ? jade::crc32(rc.rgba.data(), rc.rgba.size()) : 0,
                rr.ok ? jade::crc32(rr.rgba.data(), rr.rgba.size()) : 0);
    return 0;
}

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc >= 3 && std::strcmp(argv[2], "image") == 0)
        return run_image(argc, argv);
    if (argc >= 3 && std::strcmp(argv[2], "batch-images") == 0)
        return run_batch_images(argc, argv);
    if (argc == 5 && std::strcmp(argv[2], "batch") == 0) return run_batch(argv);
    if (argc == 5 && std::strcmp(argv[2], "dds") == 0) return run_dds(argv);
    if (argc < 7) {
        std::fprintf(stderr,
            "usage: texup_cli <bf> <entry_idx> <tex_key_hex> <rgba_raw> <w> <h> [fmt|auto]\n"
            "       texup_cli <bf> batch <entry_idx> <specfile>\n"
            "       texup_cli <bf> image <entry_idx> <tex_key_hex> <image> [fmt|auto] [--backup]\n"
            "       texup_cli <bf> batch-images <entry_idx> <specfile> [--backup]\n");
        return 2;
    }
    const char* bf_path = argv[1];
    uint32_t entry_idx = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));
    uint32_t tex_key = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16));
    uint32_t w = static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 0));
    uint32_t h = static_cast<uint32_t>(std::strtoul(argv[6], nullptr, 0));
    uint32_t fmt = 0xFFFFFFFFu;
    if (argc > 7 && std::strcmp(argv[7], "auto") != 0)
        fmt = static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 0));

    std::ifstream rf(std::filesystem::u8path(argv[4]), std::ios::binary);
    if (!rf) { std::fprintf(stderr, "ERROR cannot open rgba file\n"); return 1; }
    std::vector<uint8_t> rgba((std::istreambuf_iterator<char>(rf)),
                              std::istreambuf_iterator<char>());
    if (rgba.size() != size_t(w) * h * 4) {
        std::fprintf(stderr, "ERROR rgba size %zu != %ux%ux4\n", rgba.size(), w, h);
        return 1;
    }

    jade::texup::UpscaleStats st = jade::texup::upscale_texture_in_bf(
        bf_path, entry_idx, tex_key, rgba.data(), w, h, fmt, /*backup=*/false);
    if (!st.ok) { std::fprintf(stderr, "ERROR %s\n", st.error.c_str()); return 1; }
    print_single(st);
    return 0;
}
