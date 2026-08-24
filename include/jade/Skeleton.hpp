// Skeleton.hpp — Jade bone hierarchy builder (read path).
//
// Port of jade_explorer/core/skeleton.py :func:`build_bone_nodes`. The Python
// function builds a glTF node tree from GAO bone data and then attaches mesh
// nodes pulled from the (un-ported) collect_assets / GLB pipeline. The
// mesh-attachment half depends on data outside this read-path port; the
// *bone forest* itself is fully determined by the sub-entry stream alone and is
// what this module reproduces byte-for-byte.
//
// Algorithm (mirrors the Python exactly):
//   * Parse every ".gao" sub-entry with parse_gao_full -> gao_data{key->info}.
//   * children_map: for each gao whose father_key is present and itself a gao,
//     record key under father_key.
//   * root_keys: gaos whose father_key is 0xFFFFFFFF or not a gao in this bin.
//   * bone_keys: gaos with the Bone (identity&0x1) or Hierarchy (identity&
//     0x400000) flag. If empty -> no skeleton (ok=false).
//   * Walk the forest depth-first from each root (roots sorted by gao name,
//     children sorted by gao name), assigning node indices in visit order.
//   * Each node carries: name (".gao" suffix stripped; an empty parsed name
//     stays empty — the Python's bone_0x default never fires because
//     parse_gao_header always sets info['name']), a 16-float column-major
//     matrix (local_matrix preferred,
//     else global_matrix) gated by _sane (finite && |v|<1e6, else no matrix),
//     the source gao key, and child node indices.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "jade/SubEntry.hpp"

namespace jade {

// Identity bits that mark a GAO as a bone/joint (from gao.py GAO_FLAGS).
constexpr uint32_t GAO_FLAG_BONE      = 0x00000001;
constexpr uint32_t GAO_FLAG_HIERARCHY = 0x00400000;

struct BoneNode {
    std::string           name;            // ".gao"-stripped gao name ('' if empty)
    uint32_t              key = 0;         // source gao key
    bool                  has_matrix = false;
    std::vector<float>    matrix;          // 16 col-major floats when has_matrix
    std::vector<uint32_t> children;        // child node indices (visit order)
};

struct BoneForest {
    bool                  ok = false;      // false mirrors the Python None
    std::vector<BoneNode> nodes;
};

// Build the bone-node forest from a bin's sub-entry stream. ok == false when
// there are no GAOs, or no bone-flagged GAOs (Python returns None in both).
BoneForest build_bone_nodes(const std::vector<SubEntry>& subs);

}  // namespace jade
