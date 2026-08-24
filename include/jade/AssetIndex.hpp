// AssetIndex.hpp — flatten a BigFile into a typed sub-entry catalog (read path).
//
// Port of jade_explorer/core/asset_index.py. A BF entry is a container of one
// or more sub-entries (records in the _wow_ stream); each is a separately-keyed
// Jade resource. The asset index walks every active entry, classifies every
// sub-entry into a browser CATEGORY, collects its outgoing dependency refs, and
// builds reverse-reference / by-key / by-parent maps over the whole archive.
//
// This is the aggregator over the read layer: it leans on SubEntry, Texture,
// Geometry, Light, Material, Gao and ObjectKinds (all already ported).
//
// PORTED FAITHFULLY: the full classify_sub_entry elif-chain ORDER and every
//   CATEGORY decision, ref extraction (GAO geometry/material/parent/gizmo, mat
//   texture/sub_material), the dependency-list ref_ext typing, the GAO marker
//   refinement (classify_gao_marker -> object_kinds), placeholder-texture
//   filtering, geo-group ref expansion, and the index maps + reverse_refs.
//
// DISPLAY NAMES: AssetRecord.name + the referrer-name resolution
// (_resolve_referrer_names — GAO → geometry/material → sub-material →
// texture name propagation) are ported; like `detail` they are cosmetic and
// NOT part of the golden digest (added post-validation for the GUI).
//
// DELIBERATELY OMITTED (depends on un-ported core/animation.py): the
// ANIMATION branch's parse_trl track-count name/detail — animation records
// name as plain "Animation". The *category* of every record (incl.
// ANIMATION) is gro_type-driven and IS reproduced. The golden digest
// validates the catalog STRUCTURE (category + refs + tags), not the
// display strings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jade/SubEntry.hpp"

namespace jade {

// ── Categories (string constants, == the Python module-level names) ──
extern const char* const CAT_TEXTURE;
extern const char* const CAT_GEOMETRY;
extern const char* const CAT_MATERIAL;
extern const char* const CAT_GAO;
extern const char* const CAT_LIGHT;
extern const char* const CAT_ANIMATION;
extern const char* const CAT_SOUND;
extern const char* const CAT_AI;
extern const char* const CAT_PALETTE;
extern const char* const CAT_TEXTURE_DATA;
extern const char* const CAT_DATATABLE;
extern const char* const CAT_REFERENCE;
extern const char* const CAT_OTHER;
extern const char* const CAT_CAMERA;
extern const char* const CAT_FX;
extern const char* const CAT_TRIGGER;
extern const char* const CAT_TRAP;
extern const char* const CAT_ACTOR;
extern const char* const CAT_SPAWNER;
extern const char* const CAT_WAYPOINT;
extern const char* const CAT_LOGIC;

// ── helpers (1:1 with the Python free functions) ──

// True if payload is a packed [4-byte ASCII '.ext'][4-byte key] dependency list
// (begins with '.' then 3 alpha bytes, length >= 8).
bool looks_like_ref_list(const uint8_t* d, size_t n);

// Decode a dependency list into (ext, key) pairs; stops at the first non-ext slot.
struct RefEntry { std::string ext; uint32_t key; };
std::vector<RefEntry> decode_ref_list(const uint8_t* d, size_t n);

// The light-resource key a light-marker GAO points at = the last u32 of its
// payload (have_key=false when payload < 4 bytes).
struct LightKeyOpt { bool have = false; uint32_t key = 0; };
LightKeyOpt light_key_from_gao_payload(const uint8_t* d, size_t n);

// ── AssetRecord ──
struct AssetRecord {
    uint32_t    parent_index = 0;  // asset_id = (parent_index, sub_index)
    uint32_t    sub_index    = 0;
    uint32_t    key          = 0;
    std::string category;
    uint32_t    parent_key   = 0;
    std::string parent_name;
    uint32_t    gro_type     = 0;
    std::string ext;
    size_t      payload_size = 0;
    // Display label (GAO name if any, else derived — "Texture 512×256",
    // "Geometry 384v", …). build_asset_index then replaces generic
    // geometry/material/texture labels with the name of the GAO that
    // references them (referrer-name resolution). Not part of the golden
    // digest.
    std::string name;
    // Human detail string mirroring the Python index: "512×256 DXT1
    // mip4", "412v 220t skinned (28 bones)", light type, … ("" = none).
    // Not part of the golden digest (added post-validation for the GUI).
    std::string detail;
    // Outgoing dependency keys grouped by ref-type, mirroring the Python
    // refs dict; std::map keeps the ref-type order deterministic.
    std::map<std::string, std::vector<uint32_t>> refs;

    // Python AssetRecord.type_tag.
    std::string type_tag() const;
};

// ── sub-entry classifier (mirrors _classify_sub) ──
//
// `by_key` / `gao_keys` (the bin's key->sub map + .gao/.wol key set) and
// `ref_ext` (key->declared-extension) are optional sibling context — pass
// nullptr to classify a lone sub-entry (GAO stays generic, no ref_ext typing).
AssetRecord classify_sub_entry(
    uint32_t parent_index, uint32_t parent_key, uint32_t sub_index,
    const SubEntry& sub,
    const std::unordered_map<uint32_t, const SubEntry*>* by_key = nullptr,
    const std::unordered_set<uint32_t>* gao_keys = nullptr,
    const std::unordered_map<uint32_t, std::string>* ref_ext = nullptr,
    const std::string& parent_name = {});

// ── AssetIndex ──
struct AssetIndex {
    std::vector<AssetRecord> records;
    std::map<std::pair<uint32_t, uint32_t>, size_t> by_id; // asset_id -> record
    std::map<uint32_t, std::vector<size_t>> by_key;       // key -> record idxs
    std::map<uint32_t, std::vector<size_t>> by_parent;    // parent_index -> idxs
    std::map<uint32_t, std::set<std::pair<uint32_t, uint32_t>>> reverse_refs;

    void add(AssetRecord rec);
    std::map<std::string, size_t> count_by_category() const;
    const AssetRecord* find_by_id(uint32_t parent_index,
                                  uint32_t sub_index) const;
    // First record for a resource key, preserving Python by_key[key][0]
    // insertion-order semantics. The pointer remains valid until add().
    const AssetRecord* find_first_by_key(uint32_t key) const;
    std::vector<const AssetRecord*> find_resolved(uint32_t key) const;
    std::vector<const AssetRecord*> referrers(uint32_t key) const;
    // Python's lazy material_containers_by_family(): family prefix -> record
    // indices for gro-4 multi-material containers. The index representation
    // keeps pointers stable across callers and is invalidated by add().
    const std::map<uint32_t, std::vector<size_t>>&
    material_containers_by_family() const;

private:
    mutable bool material_families_valid_ = false;
    mutable std::map<uint32_t, std::vector<size_t>> material_families_;
};

class BigFile;  // fwd

// Build the full index by walking every readable BF entry. Malformed entries
// are skipped silently (one bad entry must not abort the scan). `limit` (0 =
// all) bounds the number of active entries scanned, for a fast/deterministic
// digest. Placeholder texture stubs are filtered out (as the Python does).
// `progress(done, total)` / `cancel() -> bool` mirror the Python's optional
// callbacks (the GUI's background scan drives a progress bar / cancel).
AssetIndex build_asset_index(
    const BigFile& bf, size_t limit = 0,
    const std::function<void(int, int)>& progress = {},
    const std::function<bool()>& cancel = {});

}  // namespace jade
