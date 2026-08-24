// JtmodEngine.hpp — merge + conflict + apply for .jtmod mods (Mod Manager core).
//
// Port of jade_explorer/build/jtmod_apply.py. Container-only: verify a pristine
// base, detect conflicts, merge per-bin deltas, and repack the touched bins once
// via the BigFile write path + compress_lzo. Carries no Jade format knowledge.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "jade/Jtmod.hpp"

namespace jade {
namespace jtmod {

// A loaded .jtmod plus the id the manager knows it by (load order).
struct Mod {
    std::string id;
    Jtmod data;
};

// Two or more mods contending for the same resource.
struct Conflict {
    std::string kind;                 // minted_key | whole_bin | new_bin | overlap
    std::vector<std::string> mod_ids;
    bool has_bin = false;
    uint32_t bin_key = 0;
    std::string detail;
    std::string str() const;
};

struct ApplyResult {
    bool ok = false;
    std::vector<Conflict> conflicts;
    std::vector<std::string> issues;
    int bins_modified = 0;
    int bins_added = 0;
    int keys_relocated = 0;   // minted 0x7A keys renumbered to dodge cross-mod collisions
    int forced_bins = 0;      // bins overwritten despite a base mismatch (force mode)
    std::string report;
};

// Reasons the mods can't be applied to base_path (empty = ok): cross-game,
// cross-mod game mix, and the exact base sha256/size pin.
std::vector<std::string> precheck(const std::vector<Mod>& mods,
                                  const std::string& base_path);

// Mods contending for the same EXISTING resource — same bin replaced wholesale,
// or hunks touching the same base records (never auto-resolved; the user picks).
// Minted-key (0x7A) collisions are NOT reported here: they're a numbering clash
// between two mods' *new* assets and are auto-resolved by resolve_collisions().
std::vector<Conflict> detect_conflicts(const std::vector<Mod>& mods);

// Auto-resolve cross-mod minted-key (0x7A) collisions in load order: the first
// mod to mint a key keeps it; any later mod that minted the same key is renumbered
// to a fresh 0x7A key, rewriting every reference (record identity/type fields,
// new-bin FAT key, and cross-references in payloads). Mutates `mods` in place;
// returns the number of keys moved. Deterministic (stable across re-applies).
int resolve_collisions(std::vector<Mod>& mods);

// Progress callback: (percent 0..100, short phase label). Called on the thread
// that invokes apply(); a UI passing one must marshal to its own thread.
using ProgressFn = std::function<void(int, const std::string&)>;

// Verify base, detect conflicts (block unless allow_conflicts), merge, and write
// the combined archive to out_path. Self-verifies that every touched/new bin
// decompresses to its intended bytes. `progress` (optional) reports the phases:
// checking base / merging / copying / writing / verifying.
//
// `allow_base_mismatch`: when false (default), a v3 mod whose touched bin differs
// from the version it was authored against is refused. When true, the mod is
// force-applied anyway (overwrites/overlays that bin) — this may discard other
// tools' changes to it and can misbehave if that bin's layout changed.
ApplyResult apply(const std::string& base_path, const std::vector<Mod>& mods,
                  const std::string& out_path, bool allow_conflicts = false,
                  bool allow_base_mismatch = false, ProgressFn progress = nullptr);

// Largest decompressed bin the mods would produce in `base` (the modified + new
// bins; untouched stock bins are ignored — they already load). Used to warn when
// a modded asset exceeds the game's fixed streaming-decompression buffer. Returns
// 0 if the base can't be opened.
size_t max_modified_bin_size(const std::string& base_path, const std::vector<Mod>& mods);

// Load a .jtmod file into a Mod (id = filename stem). Throws on parse failure.
Mod load_mod(const std::string& path);

}  // namespace jtmod
}  // namespace jade
