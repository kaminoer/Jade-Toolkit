// Jtmod.hpp — parse a .jtmod composable mod delta + the three container helpers.
//
// Port of the read/merge primitives in jade_explorer/build/jtmod.py. A .jtmod
// records, per touched BigFile bin, what its decompressed sub-entry RECORD
// SEQUENCE changes to (as 3-way-merge hunks), plus whole-bin / new-bin payloads
// and the 0x7A keys the mod mints. This file owns only the *container* view —
// no Jade format knowledge — so the shipped Mod Manager never reveals it.
//
// On-disk format "JADEJTM1" (little-endian); see jtmod.py for the byte layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jade {
namespace jtmod {

constexpr uint8_t MODE_SUBENTRY = 1;
constexpr uint8_t MODE_WHOLEBIN = 2;
constexpr uint8_t MODE_NEWBIN   = 3;

using Record = std::vector<uint8_t>;  // a full [size][cookie][key][type][payload]

// One hunk: replace base_records[base_start, base_end) with `records`.
struct Hunk {
    uint32_t base_start = 0;
    uint32_t base_end   = 0;
    std::vector<Record> records;
};

// One bin's delta. `mode` selects which fields are meaningful.
struct BinDelta {
    uint32_t bin_key = 0;
    uint8_t  mode    = 0;
    // MODE_SUBENTRY:
    uint32_t base_record_count = 0;
    std::vector<uint8_t> base_hash;   // v3: sha256 of the authored base bin (32B; empty pre-v3)
    std::vector<Hunk> hunks;
    // MODE_WHOLEBIN / MODE_NEWBIN:
    std::vector<uint8_t> payload;
    // MODE_NEWBIN:
    std::string name;
    uint32_t parent_dir_idx = 0;
};

// A parsed .jtmod.
struct Jtmod {
    uint32_t format_version = 0;
    std::string game;            // GameProfile code, e.g. "T2T"
    std::string archive_name;    // e.g. "pop3.bf"
    uint64_t    base_size = 0;
    std::string base_sha256;     // 64-char lowercase hex
    std::string name, author, version_text, description, created, tool;
    std::vector<uint8_t> image;   // v2: small square PNG card thumbnail (may be empty)
    std::vector<uint32_t> minted_keys;
    std::vector<BinDelta> bins;
};

// Parse a .jtmod blob. Throws std::runtime_error on bad magic / unsupported
// version / truncation. Tolerates a present integrity trailer (ignored here).
Jtmod parse(const uint8_t* data, size_t len);
inline Jtmod parse(const std::vector<uint8_t>& v) { return parse(v.data(), v.size()); }

// Integrity (tamper-evidence) check of a published .jtmod's appended trailer.
// Ok = trailer present and matches; Mismatch = present but content changed (hex
// edit / corruption); Missing = no trailer (old tool or stripped).
enum class Integrity { Ok, Missing, Mismatch };
Integrity check_integrity(const uint8_t* data, size_t len);
inline Integrity check_integrity(const std::vector<uint8_t>& v) {
    return check_integrity(v.data(), v.size());
}

// ── container helpers (the whole C++ merge contract; no format knowledge) ──

struct SplitResult {
    std::vector<uint8_t> prefix;    // bytes before the first record
    std::vector<Record>  records;   // full record byte-strings, in order
    bool clean = false;             // prefix + concat(records) == dec exactly
};

// Split a decompressed bin into prefix + ordered records. `clean` is false when
// the bin isn't a pure record concatenation (then the caller must treat it as
// an opaque whole-bin payload).
SplitResult split_records(const uint8_t* dec, size_t n);
inline SplitResult split_records(const std::vector<uint8_t>& v) {
    return split_records(v.data(), v.size());
}

// Apply non-overlapping hunks to a base record list (high index first).
std::vector<Record> apply_hunks(const std::vector<Record>& base_records,
                                const std::vector<Hunk>& hunks);

// True if two hunks touch the same base-record range (half-open; two inserts at
// the same index overlap, an insert abutting an edge does not).
bool hunks_overlap(const Hunk& a, const Hunk& b);

}  // namespace jtmod
}  // namespace jade
