// Project.hpp — the mod-project document model + builder (Phase 3).
//
// Ports project/serialize.py + project/project.py's data layer and
// build/{entryset,context,builder}.py. Operations are kept as RAW JSON
// values dispatched by op_type at apply time — an unknown op round-trips
// untouched and skips with a warning (the Python's UnknownOperation), and
// new native-first ops need no class boilerplate.  The dispatcher includes
// replace_animation's fixed-size TRL splice as well as the editing operations
// documented below.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Json.hpp"

namespace jade {
namespace project {

// ── document ──

struct BaseRef {
    std::string game;             // GameProfile code, e.g. "T2T"
    std::string archive_name;
    std::string archive_sha256;
    uint64_t archive_size = 0;
    // size + sha256 check against a real file.
    bool matches(const std::string& archive_path) const;
};

struct BuildSettings {
    std::string output_name;
    bool strict_inplace = true;
};

struct ModProject {
    bool ok = false;
    std::string error;            // load failure (the ProjectFormatError text)
    std::string error_type;       // Python exception class for load parity
    std::string name, author, description, created, modified;
    BaseRef base;
    BuildSettings build;
    std::string jmod_dir;         // the .jmod directory this was loaded from
    int next_op_serial = 1;
    std::vector<json::Value> operations;   // raw op dicts, in order
    // Exact ModProject.load(...).to_dict() result for a core-loaded document.
    // Qt-authored/modified documents intentionally rebuild from typed fields.
    json::Value loaded_dict;

    // Ops with enabled != false, in order.
    std::vector<const json::Value*> enabled_operations() const;
};

// Structured equivalent of Operation.conflict_signature() and
// ModProject.conflicts(). Integer atoms remain integers so callers can use
// the signatures as data instead of parsing validate.py's display text.
struct ConflictValue {
    enum class Type { Integer, String };
    Type type = Type::Integer;
    int64_t integer = 0;
    std::string string;

    static ConflictValue from_integer(int64_t value) {
        ConflictValue atom;
        atom.integer = value;
        return atom;
    }
    static ConflictValue from_string(std::string value) {
        ConflictValue atom;
        atom.type = Type::String;
        atom.string = std::move(value);
        return atom;
    }
    bool operator==(const ConflictValue& other) const {
        return type == other.type && integer == other.integer &&
               string == other.string;
    }
};

struct ConflictSignature {
    std::string kind;
    std::vector<ConflictValue> values;
    bool operator==(const ConflictSignature& other) const {
        return kind == other.kind && values == other.values;
    }
};

struct ProjectConflict {
    ConflictSignature signature;
    std::vector<std::string> op_ids;
};

std::vector<ConflictSignature> operation_conflict_signatures(
    const json::Value& operation);
std::vector<ProjectConflict> project_conflicts(const ModProject& project);
std::string format_conflict_signature(const ConflictSignature& signature);

// Exact Operation.target_summary() text used by the Python project panel.
// Unknown operation types use UnknownOperation's forward-compatible label.
std::string operation_target_summary(const json::Value& operation);

// Typed Operation.from_dict(...).to_dict() and ModProject.to_dict()
// equivalents. Known operations are normalized to their Python serialized
// shape; unknown operations preserve their raw fields while normalizing the
// four common editable fields.
json::Value operation_to_dict(const json::Value& operation);
json::Value project_to_dict(const ModProject& project);

// load_project_dict + ModProject.from_dict for a .jmod directory.
ModProject load_project(const std::string& jmod_dir);

// sha256 hex digest of a file (BaseRef guard).
std::string sha256_of_file(const std::string& path);

// ── build ──

struct BuildIssue {
    std::string level;            // "error" | "warning"
    std::string message;
    std::string op_id;
};

struct BuildResult {
    bool ok = false;
    std::string output_path;
    uint32_t entries_changed = 0;
    uint32_t entries_appended = 0;
    uint64_t bytes_appended = 0;
    std::vector<uint32_t> modified_keys;   // entry FAT keys written (or would be)
    std::vector<BuildIssue> issues;
    std::vector<std::string> log;
    std::string report;
};

// Python build.builder._format_report, exposed so exception adapters and
// headless front-ends can present the same result text as the Python toolkit.
std::string format_build_report(const ModProject& project,
                                const BuildResult& result);

// Python build.builder progress(done, total, phase) contract. The callback is
// optional and is invoked synchronously on the build thread.
using BuildProgressFn =
    std::function<void(uint64_t, uint64_t, const std::string&)>;

// Deterministic low-level failure hook used by the Python/native build I/O
// oracle. Zero disables injection. The GUI and production callers never set
// this; keeping the fault at the build boundary makes partial-output and
// continuation behavior testable without relying on filesystem timing.
struct BuildFaultInjection {
    size_t fail_write_entry_at = 0;  // one-based modified-entry write index
    std::string message = "injected write failure";
};

// build_project: copy base -> output, apply enabled ops in order, recompress
// modified entries at level 9 (in place when they fit, appended otherwise —
// strict_inplace makes a non-fit an error), stage new FAT entries, flush
// size.grs, and run the structural plus semantic post-build verifiers. All
// Python operation names are dispatched (the explicitly deferred animation
// operation remains a fixed-size raw splice); unknown operations skip with a
// warning like Python's UnknownOperation. Progress ticks match builder.py.
// dry_run is a native extension that applies and reports without writing.
BuildResult build_project(const ModProject& project,
                          const std::string& base_archive_path,
                          const std::string& output_path = "",
                          bool dry_run = false,
                          BuildProgressFn progress = {},
                          const BuildFaultInjection* fault = nullptr);

// export_jtmod (build/jtmod_export.py): apply the project against a
// read-only base view and emit the composable .jtmod delta (v3, sealed).
// created_iso is injected (Python stamps iso_now() — nondeterministic).
struct JtmodExportResult {
    bool ok = false;
    std::vector<uint8_t> blob;
    std::string output_path;
    uint32_t bins_subentry = 0, bins_wholebin = 0, bins_new = 0;
    std::vector<uint32_t> minted_keys;
    std::vector<BuildIssue> issues;
    std::vector<std::string> log;
    std::string report;
};

struct JtmodExportOptions {
    std::string title;
    std::string author;
    std::string version;
    std::string description;
    std::string image_path;
    std::string created_iso;
    bool validate = true;
};

JtmodExportResult export_jtmod(const ModProject& project,
                               const std::string& base_archive_path,
                               const std::string& out_path,
                               const JtmodExportOptions& options);

// Compatibility overload used by existing deterministic oracles.
inline JtmodExportResult export_jtmod(const ModProject& project,
                                     const std::string& base_archive_path,
                                     const std::string& out_path,
                                     const std::string& created_iso) {
    JtmodExportOptions options;
    options.created_iso = created_iso;
    options.validate = false;
    return export_jtmod(project, base_archive_path, out_path, options);
}

// validate_project (build/validate.py + per-op Operation.validate): pre-build
// checks against the BASE archive. Structural
// (entry/sub-entry existence, layer/slot ranges, asset presence), semantic
// (add-op ordering, no-op and dangerous-mask warnings), simulations
// (add_object placement + LOA stream-order, create_zone clone+rekey), and
// conflict-signature grouping ("later op wins" warnings). live=true keeps the
// same cheap early-return gates as Python's continuously refreshed project
// panel; build_project uses the complete mode and aborts on validation errors
// before copying anything.
std::vector<BuildIssue> validate_project(const ModProject& project,
                                         const std::string& base_archive_path,
                                         std::vector<std::string>& log,
                                         bool live = false);

// verify_build_output (build/verify.py): post-build integrity — FAT
// re-open, key sets, untouched entries byte-identical, size.grs row audit,
// touched entries round-trip. build_project runs it automatically (non-dry).
std::vector<BuildIssue> verify_build_output(
    const std::string& base_path, const std::string& output_path,
    const std::unordered_set<uint32_t>& touched_keys,
    const std::unordered_set<uint32_t>& added_keys,
    std::vector<std::string>& log);

// Dedicated semantic post-build checks from build/verify.py. Texture encoder
// differences are warnings; missing/misordered newly-added records and wrong
// material references are errors. build_project runs both after the structural
// verifier.
std::vector<BuildIssue> verify_replaced_textures(
    const ModProject& project, const std::string& output_path,
    std::vector<std::string>& log);

std::vector<BuildIssue> verify_added_assets(
    const ModProject& project, const std::string& base_path,
    const std::string& output_path, std::vector<std::string>& log);

// Python's verify.base_archive_hash spelling; sha256_of_file is the same
// implementation used by BaseRef.
inline std::string base_archive_hash(const std::string& path) {
    return sha256_of_file(path);
}

// Opt-in post-build spin check (roadmap B3, "load_sim as a build validator"):
// for every WOL whose dep closure contains a touched entry, simulate the
// zone load on the BASE and the OUTPUT archives and report NEW unresolved
// sub-entry refs (spin candidates = the black-screen freeze) as warnings.
// Heavy: builds the archive-wide provider index on both archives (WW/T2T
// only — SoT has no WOLInfo table; returns a single info issue there).
std::vector<BuildIssue> validate_spins(const std::string& base_archive_path,
                                       const std::string& output_path,
                                       const std::vector<uint32_t>& touched_keys,
                                       std::vector<std::string>& log);

}  // namespace project
}  // namespace jade
