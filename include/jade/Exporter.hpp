// Exporter.hpp — BigFile unpack (io_ops/exporter.py, non-animation subset).
//
// Per entry: textures -> DDS, gro-1 geometry -> model GLBs, optional complete
// static scene GLBs, character grouping/bundles, and manifest.json round-trip
// scanning. Only TRL animation channels are excluded. Both export phases use
// bounded workers and expose Python-compatible log/progress callbacks.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/SubEntry.hpp"

namespace jade {
namespace exporter {

using ExportLogFn = std::function<void(const std::string&)>;
using ExportProgressFn = std::function<void(uint32_t, uint32_t)>;

// exporter.get_worker_presets / _recommended_workers. Passing cpu_count=0
// uses std::thread::hardware_concurrency (with a one-worker fallback).
uint32_t recommended_workers(uint32_t cpu_count = 0);
std::map<uint32_t, std::string> get_worker_presets(uint32_t cpu_count = 0);

// exporter.detect_character_groups. Detection is name/FAT-only: it does not
// parse or decompress an entry and is independent of animation decoding.
struct CharacterEntry {
    uint32_t index = 0;
    uint32_t key = 0;
    std::string name;
};
struct CharacterGroup {
    std::string base_name;
    bool has_skeleton = false;
    CharacterEntry skeleton;
    std::vector<CharacterEntry> costumes;
};
std::vector<CharacterGroup> detect_character_groups(const BigFile& bf);

// scene_export._sanitize_name / infer_entry_name / name_sub_entry.
std::string sanitize_name(const std::string& name);
std::string infer_entry_name(const std::vector<SubEntry>& subs);   // "" = None
std::string name_sub_entry(const SubEntry& sub, const std::vector<SubEntry>& subs);

struct SceneExportResult {
    bool ok = false;
    std::string error;
    std::string glb_path;
    std::string scene_name;
    uint32_t meshes = 0;
    uint32_t materials = 0;
    uint32_t textures = 0;
    uint32_t animations = 0;  // remains zero while animation is deferred
    uint32_t bones = 0;
    std::vector<std::string> texture_files;
};

// scene_export.export_entry_scene, excluding only animation channels.
SceneExportResult export_entry_scene(const std::vector<SubEntry>& subs,
                                     const std::string& out_path,
                                     const std::string& entry_name = "",
                                     bool export_textures = true,
                                     const std::string& scene_type = "");

// scene_export.export_character_bundle without deferred TRL channels. Costume
// data remains authoritative; the `_wow` donor contributes extra textures but
// never contributes its placeholder GAOs to the costume skeleton.
SceneExportResult export_character_bundle(
    const std::vector<SubEntry>& skeleton_subs,
    const std::vector<SubEntry>& costume_subs,
    const std::string& out_path,
    const std::string& bundle_name);

// One entry's export: writes textures/ + models/ files, returns the
// manifest entry_info as JSON text ("" when nothing was exported).
std::string process_entry(const std::vector<uint8_t>& dec, uint32_t fi_index,
                          uint32_t fi_key, const std::string& fi_name,
                          const std::string& out_dir,
                          bool scene_mode = false,
                          std::vector<std::string>* logs = nullptr);

// Export every active entry in Python's three observable phases, with the
// same worker-count clamp, log/progress callbacks, optional scene GLBs,
// detected character bundles, and manifest.json version 3. max_workers < 0
// selects the recommended count; zero is clamped to one.
struct ExportStats {
    bool ok = false;
    std::string error;
    uint32_t entries = 0;
    uint32_t character_bundles = 0;
};
ExportStats export_bigfile(const BigFile& bf, const std::string& bf_path,
                           const std::string& out_dir,
                           bool scene_mode = true,
                           ExportLogFn log_fn = {},
                           ExportProgressFn progress_fn = {},
                           int32_t max_workers = -1);

// importer.scan_changes: list the unpack dir's exported files against
// manifest.json (the GUI's re-import candidates). Empty when no manifest.
struct ChangeRec {
    std::string path;            // manifest-relative
    std::string full_path;
    uint32_t key = 0;            // sub key (or entry key for scenes)
    uint32_t entry_index = 0;
    uint32_t entry_key = 0;
    std::string category;        // texture|model|scene|animation|raw
    std::string entry_name;      // scenes only
    std::string status = "modified";
    std::string sub_info_json;   // exact manifest object; scene_info for scenes
};
std::vector<ChangeRec> scan_changes(const std::string& out_dir,
                                    std::vector<std::string>& log);

}  // namespace exporter
}  // namespace jade
