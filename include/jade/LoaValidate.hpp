#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jade::loa {

struct DepRef {
    uint32_t gao_key = 0;
    size_t gao_offset = 0;
    uint32_t dep_key = 0;
    std::string dep_kind;  // "gro", "grm", or "colmap"
};

struct Issue {
    std::string level;  // "error" or "warning"
    std::string message;
};

bool is_modded_key(uint32_t key);

// Python build/loa_validate.py callable surface.
std::vector<DepRef> collect_dep_refs(const std::vector<uint8_t>& dec_bytes);

std::vector<Issue> validate_loa_stream(
    const std::vector<uint8_t>& dec_bytes, bool ignore_shipped = true);

}  // namespace jade::loa
