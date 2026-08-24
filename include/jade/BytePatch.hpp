// BytePatch.hpp - JADEBPT3 byte-patch codec, diff, and runtime.
// Ports build/patcher_runtime.py and the data half of build/exe_patch.py.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jade {
namespace bytepatch {

constexpr uint32_t FORMAT_VERSION = 3;
constexpr size_t MERGE_GAP = 1024;
constexpr size_t IO_CHUNK = 1u << 20;

struct Segment {
    uint64_t offset = 0;
    uint32_t length = 0;
    bool operator==(const Segment& other) const {
        return offset == other.offset && length == other.length;
    }
};

struct Patch {
    uint64_t base_size = 0;
    std::string base_sha256;
    uint64_t result_size = 0;
    std::string result_sha256;
    std::string title;
    std::string description;
    std::string author;
    std::string version_text;
    std::string created;
    std::string tool;
    std::string default_target;
    std::vector<std::string> accepted_names;
    std::vector<Segment> fwd_segs;
    std::vector<uint8_t> fwd_body;
    std::vector<Segment> rev_segs;
    std::vector<uint8_t> rev_body;
    uint32_t image_w = 0;
    uint32_t image_h = 0;
    std::vector<uint8_t> image;
};

std::vector<uint8_t> pack_patch(const Patch& patch);
Patch unpack_patch(const std::vector<uint8_t>& data);

using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

std::string sha256_file(const std::string& path, ProgressFn progress = {});

struct DataSegment {
    uint64_t offset = 0;
    std::vector<uint8_t> data;
};
struct DiffResult {
    std::vector<DataSegment> segments;
    uint64_t result_size = 0;
    uint64_t changed_bytes = 0;
};
DiffResult compute_segments(const std::string& base_path,
                            const std::string& result_path,
                            size_t merge_gap = MERGE_GAP,
                            ProgressFn progress = {});

struct PayloadOptions {
    std::string title;
    std::string description;
    std::string author;
    std::string version;
    std::string archive_name;
    std::vector<std::string> accepted_names;
    std::string image_path;
    // Empty uses the current UTC timestamp. Exposed for deterministic tests.
    std::string created;
};
struct PayloadStats {
    size_t fwd_segments = 0;
    size_t rev_segments = 0;
    uint64_t changed_bytes = 0;
    size_t payload_size = 0;
    uint64_t base_size = 0;
    uint64_t result_size = 0;
    std::string base_sha256;
    std::string result_sha256;
};
struct PayloadBuild {
    std::vector<uint8_t> payload;
    PayloadStats stats;
};
PayloadBuild build_patch_payload(const std::string& base_path,
                                 const std::string& result_path,
                                 const PayloadOptions& options,
                                 ProgressFn progress = {});

void apply_forward_inplace(const Patch& patch, const std::string& target);
void apply_reverse_inplace(const Patch& patch, const std::string& target);
Patch apply_patch_to_file(const std::vector<uint8_t>& payload,
                          const std::string& base_path,
                          const std::string& out_path,
                          ProgressFn progress = {});

// "base" | "patched" | "size-mismatch" | "unknown" | "missing".
std::string classify_target(const Patch& patch, const std::string& path);

// Payload appended to a binary as payload + "JDPATFTR" + u64 length.
std::vector<uint8_t> read_embedded_payload(const std::string& executable_path);
void append_payload(const std::string& stub_path,
                    const std::vector<uint8_t>& payload,
                    const std::string& output_path);

// build.exe_patch.accepted_target_names, case-insensitive/order-preserving.
std::vector<std::string> accepted_target_names(
    const std::string& game, const std::string& loaded_archive_name);

}  // namespace bytepatch
}  // namespace jade
