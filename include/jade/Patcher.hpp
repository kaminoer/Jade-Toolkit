// Patcher.hpp - public manifest/change-set reimport workflow.
//
// Ports io_ops/patcher.py's non-deferred paths: texture replacement, scene
// GLB reimport, model GLB reimport, archive backup/writeback, progress/log
// callbacks, size.grs flushing, and per-entry partial failure.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Exporter.hpp"

namespace jade {
namespace patcher {

struct PatchTextureResult {
    bool changed = false;
    std::vector<uint8_t> patched;
};

// patcher._patch_texture. `encode` accepts auto/Auto/"" or a numeric Jade
// texture format string. Log lines intentionally match the Python helper.
PatchTextureResult patch_texture(const std::vector<uint8_t>& dec,
                                 uint32_t sub_key,
                                 const std::string& image_path,
                                 const std::string& encode,
                                 std::vector<std::string>& log);

using PatchLogFn = std::function<void(const std::string&)>;
using PatchProgressFn = std::function<void(size_t current, size_t total)>;

struct PatchBigFileStats {
    size_t entry_groups = 0;
    size_t entries_written = 0;
    size_t entry_errors = 0;
    int size_grs_rows = 0;
    bool backup_created = false;
};

// patcher.patch_bigfile. Changes are grouped in first-seen entry order and
// applied in their original order. An error abandons only that entry, then the
// remaining groups continue. The archive-open/backup failures propagate, as
// they do in Python. Animation changes are deliberately reported as skipped
// while that user-deferred vertical slice remains disabled.
PatchBigFileStats patch_bigfile(BigFile& bf, const std::string& bf_path,
                                const std::string& out_dir,
                                const std::vector<exporter::ChangeRec>& changes,
                                PatchLogFn log_fn = {},
                                PatchProgressFn progress_fn = {});

}  // namespace patcher
}  // namespace jade
