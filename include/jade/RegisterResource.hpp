// RegisterResource.hpp - offline sidecar WOW construction and validation.
// Port of io_ops/register_resource.py.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "jade/SubEntry.hpp"

namespace jade {
namespace register_resource {

constexpr uint32_t GRID_NULL = 0xFFFFFFFFu;
constexpr uint32_t BAD_GAO_FLAGS =
    0x10u | 0x2u | 0x100u | 0x600u | 0x40000u | 0x800000u |
    0x2000000u | 0x400000u | 0x200000u;

struct DonorClosure {
    uint32_t gao_key = 0;
    uint32_t geo_key = 0;
    uint32_t mat_key = 0;
    uint32_t submat_key = 0;
    uint32_t submat_kind = 0;
    uint32_t tex_key = 0;
    uint32_t mat_data_kind = 0;
};

struct SidecarKeys {
    uint32_t wow = 0;
    uint32_t gog = 0;
    uint32_t gao = 0;
    uint32_t geo = 0;
    uint32_t mat = 0;
    uint32_t submat = 0;
    uint32_t tex = 0;
};

struct SidecarBuild {
    std::vector<uint8_t> dec;
    SidecarKeys keys;
    DonorClosure donor;
    std::string name;
    std::vector<std::string> report;
};

// Low-level Python-callable surface.
std::vector<uint8_t> make_record(uint32_t key,
                                 std::optional<uint32_t> type_u32,
                                 const std::vector<uint8_t>& data);
std::vector<uint8_t> record_bytes(const std::vector<uint8_t>& dec,
                                  const SubEntry& entry);

// Pick the smallest safe static GAO closure from a walked donor stream.
std::optional<DonorClosure> select_donor(
    const std::vector<SubEntry>& subs,
    const std::set<uint8_t>& valid_prefixes = {});

// Assemble the exact eight-record sidecar stream. Throws std::runtime_error
// with the Python ValueError text when the donor is unusable.
SidecarBuild build_sidecar(const std::vector<uint8_t>& donor_dec,
                           const SidecarKeys& keys,
                           const std::string& name,
                           const std::vector<uint8_t>& k_new_stub,
                           const std::vector<uint8_t>& k_new_full,
                           const std::set<uint8_t>& valid_prefixes = {});

// Empty means valid. Problem order and text follow validate_sidecar.
std::vector<std::string> validate_sidecar(const SidecarBuild& build);

}  // namespace register_resource
}  // namespace jade
