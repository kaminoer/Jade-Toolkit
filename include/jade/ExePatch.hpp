// ExePatch.hpp - standalone byte-patcher authoring orchestration.
// Port of build/exe_patch.py over the native Project + BytePatch cores.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "jade/Project.hpp"

namespace jade {
namespace exepatch {

using LogFn = std::function<void(const std::string&)>;
using ProgressFn =
    std::function<void(uint64_t done, uint64_t total,
                       const std::string& phase)>;

struct Options {
    std::string title;
    std::string description;
    std::string author;
    std::string version;
    std::string out_exe_path;
    std::vector<std::string> accepted_names;
    std::string icon_path;
    std::string image_path;
    // Precompiled build/cpp/patcher.cpp stub. jade_native builds this as
    // app/jade_bytepatch_stub.exe.
    std::string stub_exe_path;
    std::string work_dir;
    bool keep_work = false;
};

struct Result {
    bool ok = false;
    std::string exe_path;
    size_t patch_size = 0;
    size_t fwd_segments = 0;
    size_t rev_segments = 0;
    uint64_t changed_bytes = 0;
    uint64_t base_size = 0;
    uint64_t result_size = 0;
    std::string compiler;
    std::vector<project::BuildIssue> issues;
    std::string report;
};

// Build archive -> bidirectional diff -> self-verify -> optionally replace the
// bundled stub's Win32 icon resources -> append the payload.
Result make_exe_patch(const project::ModProject& project,
                      const std::string& base_archive_path,
                      const Options& options,
                      ProgressFn progress = {}, LogFn log = {});

}  // namespace exepatch
}  // namespace jade
