// CharacterSwap.cpp — implementation. Faithful port of
// io_ops/character_swap.py (batch mesh/texture/stub application, one write).
#include "jade/CharacterSwap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Geometry.hpp"
#include "jade/Gltf.hpp"
#include "jade/Image.hpp"
#include "jade/Json.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"
#include "jade/TextureUpscale.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jade {
namespace charswap {

namespace {

namespace fs = std::filesystem;

void emit(const SwapLogFn& log, const std::string& message) {
    if (log) log(message);
}

std::string hex8(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "%08x", value);
    return text;
}

std::string path_text(const fs::path& path) {
#ifdef _WIN32
    return path.u8string();
#else
    return path.string();
#endif
}

fs::path resolved_asset_path(const fs::path& base, const std::string& name) {
    fs::path path = fs::u8path(name);
    if (!path.is_absolute()) path = base / path;
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        resolved = fs::absolute(path, ec);
    }
    return (ec ? path : resolved).lexically_normal();
}

bool small_python_int(const json::Value* value, int base, uint32_t& out) {
    if (!value) return false;
    if (value->type == json::Value::Type::Bool) {
        out = value->b ? 1u : 0u;
        return true;
    }
    if (value->is_num()) {
        if (value->number_is_integer && !value->integer_text.empty()) {
            char* end = nullptr;
            const unsigned long long parsed =
                std::strtoull(value->integer_text.c_str(), &end, 10);
            if (!end || *end) return false;
            out = uint32_t(parsed);
            return true;
        }
        out = uint32_t(static_cast<long long>(value->num));
        return true;
    }
    if (!value->is_str()) return false;
    const char* begin = value->str.c_str();
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(begin, &end, base);
    if (end == begin || !end || *end) return false;
    out = uint32_t(parsed);
    return true;
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
    // CopyFileW already preserves the NTFS timestamps. MinGW's
    // filesystem::last_write_time conversion rounds them to whole seconds.
    return true;
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

inline uint32_t get_u32(const uint8_t* d, size_t o) {
    return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
           (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}

// mesh_swap.STUB_TEMPLATE_48: the 48-byte null-resource template third-party
// mods use to disable body-part sub-resources (version=8, identity=0x1012,
// no name, a lone "present but empty" flag).
const uint8_t STUB_TEMPLATE_48[48] = {
    0x08, 0x00, 0x00, 0x00, 0x12, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// _apply_stub: shrink the largest occurrence to the 48-byte null template.
std::vector<uint8_t> apply_stub(std::vector<uint8_t> dec, uint32_t sub_key,
                                const SwapLogFn& log) {
    std::vector<SubEntry> subs = parse_sub_entries(dec);
    const SubEntry* target = nullptr;
    for (const SubEntry& s : subs)
        if (s.key == sub_key &&
            (target == nullptr || s.data.size() > target->data.size()))
            target = &s;
    if (target == nullptr) {
        emit(log, "  stub skip 0x" + hex8(sub_key) + ": not found");
        return dec;
    }
    if (target->data.size() <= 48) {
        emit(log, "  stub skip 0x" + hex8(sub_key) + ": already small (" +
                  std::to_string(target->data.size()) + "B)");
        return dec;
    }
    emit(log, "  stub 0x" + hex8(sub_key) + ": " +
              std::to_string(target->data.size()) + "B -> 48B");
    std::vector<uint8_t> tmpl(STUB_TEMPLATE_48, STUB_TEMPLATE_48 + 48);
    return splice_sub_entry(dec, target->offset, target->size, tmpl);
}

// _apply_mesh: GLB -> GEO payload splice (no colour policy / GAO-RLI rewrite —
// unlike swap_mesh_in_bf, _build_geo_payload runs with colors=None here).
std::vector<uint8_t> apply_mesh(std::vector<uint8_t> dec, const MeshOp& op,
                                const std::string& bf_path, std::string& err,
                                const SwapLogFn& log) {
    std::vector<SubEntry> subs = parse_sub_entries(dec);
    const SubEntry* target = pick_geo_sub(subs, op.key);
    if (target == nullptr) {
        emit(log, "  mesh skip 0x" + hex8(op.key) +
                  ": no parseable .geo found");
        return dec;
    }
    GeoInfo orig_geo = parse_geometry(target->data.data(), target->data.size());
    if (!orig_geo.ok) {
        emit(log, "  mesh skip 0x" + hex8(op.key) +
                  ": original geo failed to parse");
        return dec;
    }

    std::ifstream gin(fs::u8path(op.glb_path), std::ios::binary);
    if (!gin) { err = "Mesh GLB not found: " + op.glb_path; return dec; }
    std::vector<uint8_t> glb((std::istreambuf_iterator<char>(gin)),
                             std::istreambuf_iterator<char>());
    gltf::MeshData md = gltf::parse_glb_mesh(glb.data(), glb.size());

    // skin = mesh skin or the ORIGINAL GEO's retained skin.
    gltf::MeshData build_md = md;
    if (!md.has_skin && orig_geo.skin_present) {
        build_md.skin = geo_skin_to_gltf(orig_geo);
        build_md.has_skin = true;
    }
    bool skin_flag = build_md.has_skin && !build_md.skin.bones.empty();
    bool shipped = shipped_skinned_strategy(bf_path, skin_flag);

    gltf::GeoBuildOpts opts;
    opts.version = orig_geo.version;
    opts.flags1 = orig_geo.flags1;
    opts.flags2 = orig_geo.flags2;
    opts.vb_magic = vb_magic_of(target->data.data(), target->data.size());
    opts.shipped_skinned = shipped;
    opts.orig_skinned_stride = gltf::orig_skinned_vb_stride(target->data.data(),
                                                            target->data.size());
    std::vector<uint8_t> new_payload = gltf::build_geo_payload(build_md, opts);
    const std::string label = op.label.empty()
        ? path_text(fs::u8path(op.glb_path).filename()) : op.label;
    emit(log, "  mesh 0x" + hex8(op.key) + ": " +
              std::to_string(orig_geo.nb_points) + "v -> " +
              std::to_string(md.vertices.size()) + "v, " +
              std::to_string(target->data.size()) + "B -> " +
              std::to_string(new_payload.size()) + "B [" + label + ", " +
              (shipped ? "shipped_skinned" : "legacy") + "]");
    return splice_sub_entry(dec, target->offset, target->size, new_payload);
}

// _apply_texture: replacement + stub sync (texture_upscale's helpers).
std::vector<uint8_t> apply_texture(std::vector<uint8_t> dec, const TexOp& op,
                                   std::string& err, const SwapLogFn& log) {
    std::vector<SubEntry> subs = parse_sub_entries(dec);
    const SubEntry* target = texup::pick_texture_sub(subs, op.key);
    if (target == nullptr) {
        emit(log, "  tex skip 0x" + hex8(op.key) +
                  ": no texture sub-entry found");
        return dec;
    }
    const std::vector<uint8_t>* rgba = &op.rgba;
    uint32_t width = op.w, height = op.h;
    RgbaImage loaded;
    if (!op.image_path.empty()) {
        loaded = load_rgba_image(op.image_path);
        if (!loaded.ok) {
            err = loaded.error.empty() ? "could not decode image: " + op.image_path
                                       : loaded.error;
            return dec;
        }
        rgba = &loaded.rgba;
        width = loaded.width;
        height = loaded.height;
    }
    if (width == 0 || height == 0 ||
        rgba->size() != size_t(width) * height * 4) {
        err = "RGBA image size does not match its dimensions";
        return dec;
    }
    texup::UpscalePayload up = texup::build_upscaled_payload(
        target->data, rgba->data(), width, height, op.target_format);
    if (!up.ok) { err = up.error; return dec; }              // ValueError aborts
    uint32_t new_fmt = get_u32(up.payload.data(), texup::HDR_FMT_OFF);
    dec = splice_sub_entry(dec, target->offset, target->size, up.payload);

    std::vector<SubEntry> occs_all = parse_sub_entries(dec);
    std::vector<const SubEntry*> occs;
    for (const SubEntry& s : occs_all)
        if (s.key == op.key && is_texture_entry(s.data.data(), s.data.size()))
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
            std::vector<uint8_t> patched =
                texup::sync_texture_stub(s->data, width, height, new_fmt);
            dec = splice_sub_entry(dec, s->offset, s->size, patched);
        }
        const std::string label = op.label.empty()
            ? path_text(fs::u8path(op.image_path).filename()) : op.label;
        emit(log, "  tex 0x" + hex8(op.key) + ": " +
                  std::to_string(target->data.size()) + "B -> " +
                  std::to_string(up.payload.size()) + "B (" +
                  std::to_string(width) + "x" + std::to_string(height) +
                  " fmt" + std::to_string(new_fmt) + ", " +
                  std::to_string(stubs.size()) + " stub(s) synced)  [" +
                  label + "]");
    } else {
        const std::string label = op.label.empty()
            ? path_text(fs::u8path(op.image_path).filename()) : op.label;
        emit(log, "  tex 0x" + hex8(op.key) + ": " +
                  std::to_string(target->data.size()) + "B -> " +
                  std::to_string(up.payload.size()) + "B (" +
                  std::to_string(width) + "x" + std::to_string(height) +
                  " fmt" + std::to_string(new_fmt) + ")  [" + label + "]");
    }
    return dec;
}

}  // namespace

ManifestResult load_manifest(const std::string& manifest_path) {
    ManifestResult result;
    auto fail = [&](const std::string& message) {
        result.error = message;
        return result;
    };

    const fs::path source = fs::u8path(manifest_path);
    std::ifstream file(source, std::ios::binary);
    if (!file) return fail("could not open manifest: " + manifest_path);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    json::Value doc;
    try {
        doc = json::parse_strict(text);
    } catch (const std::exception& error) {
        return fail(error.what());
    }

    if (!small_python_int(doc.find("bf_entry"), 10, result.spec.bf_entry))
        return fail("Manifest missing 'bf_entry' (integer BF entry index)");

    const fs::path base = source.parent_path().empty()
        ? fs::path(".") : source.parent_path();
    const json::Value* meshes = doc.find("meshes");
    if (meshes && meshes->type != json::Value::Type::Null) {
        if (!meshes->is_arr()) return fail("Manifest 'meshes' must be an array");
        for (const json::Value& item : meshes->arr) {
            MeshOp op;
            if (!small_python_int(item.find("key"), 0, op.key))
                return fail("Manifest mesh has an invalid or missing 'key'");
            const json::Value* asset = item.find("glb");
            if (!asset || !asset->is_str())
                return fail("Manifest mesh has an invalid or missing 'glb'");
            const fs::path resolved = resolved_asset_path(base, asset->str);
            std::error_code ec;
            if (!fs::exists(resolved, ec) || ec)
                return fail("Mesh GLB not found: " + path_text(resolved));
            op.glb_path = path_text(resolved);
            op.label = path_text(resolved.filename());
            result.spec.meshes.push_back(std::move(op));
        }
    }

    const json::Value* textures = doc.find("textures");
    if (textures && textures->type != json::Value::Type::Null) {
        if (!textures->is_arr())
            return fail("Manifest 'textures' must be an array");
        for (const json::Value& item : textures->arr) {
            TexOp op;
            if (!small_python_int(item.find("key"), 0, op.key))
                return fail("Manifest texture has an invalid or missing 'key'");
            const json::Value* asset = item.find("image");
            if (!asset || !asset->is_str())
                return fail("Manifest texture has an invalid or missing 'image'");
            const fs::path resolved = resolved_asset_path(base, asset->str);
            std::error_code ec;
            if (!fs::exists(resolved, ec) || ec)
                return fail("Texture image not found: " + path_text(resolved));
            op.image_path = path_text(resolved);
            op.label = path_text(resolved.filename());
            const json::Value* format = item.find("format");
            if (format && format->type != json::Value::Type::Null &&
                !small_python_int(format, 10, op.target_format))
                return fail("Manifest texture has an invalid 'format'");
            result.spec.textures.push_back(std::move(op));
        }
    }

    const json::Value* stubs = doc.find("stubs");
    if (stubs && stubs->type != json::Value::Type::Null) {
        if (!stubs->is_arr()) return fail("Manifest 'stubs' must be an array");
        for (const json::Value& item : stubs->arr) {
            uint32_t key = 0;
            if (!small_python_int(&item, 0, key))
                return fail("Manifest stub has an invalid key");
            result.spec.stubs.push_back(key);
        }
    }
    result.ok = true;
    return result;
}

SwapResult apply_character_swap(const std::string& bf_path, const SwapSpec& spec,
                                bool backup, SwapLogFn log) {
    SwapResult res;
    res.bf_entry = spec.bf_entry;
    auto fail = [&](const std::string& m) { res.error = m; return res; };

    std::error_code ec;
    if (backup) {
        const std::string backup_path = bf_path + ".bak";
        const fs::path bak = fs::u8path(backup_path);
        if (!fs::exists(bak, ec)) {
            emit(log, "Creating backup: " + backup_path);
            if (!copy2(fs::u8path(bf_path), bak, ec))
                return fail("could not create backup: " + ec.message());
        }
    }

    BigFile bf;
    try { bf.open(bf_path); } catch (const std::exception& e) { return fail(e.what()); }
    auto fit = bf.files.find(spec.bf_entry);
    if (fit == bf.files.end())
        return fail("BF entry " + std::to_string(spec.bf_entry) + " not found in " +
                    bf_path);
    BFFile& fi = fit->second;

    emit(log, "Applying character swap to entry " +
              std::to_string(spec.bf_entry) + " (" + fi.name + "): " +
              std::to_string(spec.meshes.size()) + " mesh(es), " +
              std::to_string(spec.textures.size()) + " texture(s), " +
              std::to_string(spec.stubs.size()) + " stub(s).");

    LzoResult dr = decompress_lzo(bf.read_data(spec.bf_entry));
    if (!dr.ok) return fail("Could not decompress BF entry");
    std::vector<uint8_t> dec = std::move(dr.data);
    const size_t original_decompressed_size = dec.size();

    // Order matters: stubs first (shrinks), then meshes, then textures.
    std::string err;
    try {
        for (uint32_t sk : spec.stubs)
            dec = apply_stub(std::move(dec), sk, log);
        for (const MeshOp& op : spec.meshes) {
            dec = apply_mesh(std::move(dec), op, bf_path, err, log);
            if (!err.empty()) return fail(err);
        }
        for (const TexOp& op : spec.textures) {
            dec = apply_texture(std::move(dec), op, err, log);
            if (!err.empty()) return fail(err);
        }
    } catch (const std::exception& e) {
        return fail(e.what());                     // GLB parse errors etc.
    }

    emit(log, "Entry " + std::to_string(spec.bf_entry) + ": decompressed " +
              std::to_string(original_decompressed_size) + " -> " +
              std::to_string(dec.size()) + " bytes");
    std::vector<uint8_t> compressed = compress_lzo(dec, 9);
    std::fstream f(fs::u8path(bf_path),
                   std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return fail("cannot open archive for writing");
    try {
        res.bf_entry_pos = bf.write_entry(f, compressed, fi);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    {
        std::ostringstream line;
        line << "Wrote " << compressed.size() << "-byte entry at 0x"
             << std::hex << res.bf_entry_pos;
        emit(log, line.str());
    }
    // NOTE: no size.grs flush — mirrors the Python (same quirk as
    // texture_upscale; flagged upstream).
    res.meshes_applied = uint32_t(spec.meshes.size());
    res.textures_applied = uint32_t(spec.textures.size());
    res.stubs_applied = uint32_t(spec.stubs.size());
    res.decompressed_size = dec.size();
    res.compressed_size = compressed.size();
    res.ok = true;
    return res;
}

SwapResult apply_character_swap(const std::string& bf_path,
                                const std::string& manifest_path,
                                bool backup, SwapLogFn log) {
    ManifestResult manifest = load_manifest(manifest_path);
    if (!manifest.ok) {
        SwapResult result;
        result.error = manifest.error;
        return result;
    }
    return apply_character_swap(bf_path, manifest.spec, backup, std::move(log));
}

}  // namespace charswap
}  // namespace jade
