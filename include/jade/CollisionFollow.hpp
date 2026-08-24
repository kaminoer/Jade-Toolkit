// CollisionFollow.hpp - collision linkage and rewrite support for moved GAOs.
//
// Port of jade_explorer/core/collision_follow.py. COB vertices live in their
// owner GAO's local frame. A visual move therefore either needs no extra edit
// (same-GAO ColMap), a matching delta on a dedicated collision GAO, or a
// duplicate-and-transform carve of selected faces in a shared room COB.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "jade/Collision.hpp"
#include "jade/SubEntry.hpp"

namespace jade {
namespace collision_follow {

using Matrix4 = std::array<double, 16>;  // row-major mathematical 4x4
using JadeMatrix = std::array<double, 16>;

struct Aabb {
    bool ok = false;
    std::array<double, 3> min{}, max{};
};

struct DedicatedLink {
    uint32_t gao_key = 0;
    std::string name;
    std::vector<uint32_t> cob_keys;
    Aabb world_aabb;
};

struct CarveLink {
    uint32_t cob_key = 0;
    uint32_t owner_gao_key = 0;
    std::string owner_name;
    std::vector<uint32_t> face_indices;
    uint32_t n_total_faces = 0;
};

struct CollisionLinks {
    uint32_t moved_gao_key = 0;
    bool same_gao = false;
    std::vector<DedicatedLink> dedicated;
    std::vector<CarveLink> carves;
    Aabb moved_world_aabb;

    bool empty() const { return dedicated.empty() && carves.empty(); }
};

Matrix4 jade16_to_math(const JadeMatrix& matrix);
JadeMatrix math_to_jade16(const Matrix4& matrix);
Matrix4 matrix_multiply(const Matrix4& a, const Matrix4& b);
Matrix4 matrix_inverse_affine(const Matrix4& matrix);
Matrix4 world_delta(const JadeMatrix& old_matrix, const JadeMatrix& new_matrix);

Aabb world_aabb_from_local_bounds(const JadeMatrix& matrix,
                                  const Aabb& local_bounds);
Aabb gao_local_bounds(
    const SubEntry& gao,
    const std::vector<SubEntry>& subs);
Aabb gao_world_aabb(
    const SubEntry& gao,
    const std::vector<SubEntry>& subs);

CollisionLinks detect_collision_links(
    uint32_t moved_gao_key,
    const std::vector<SubEntry>& subs,
    double aabb_margin_frac = 0.05,
    double aabb_margin_abs = 0.1);

// Duplicate vertices used by selected flat faces, transform the duplicates,
// rotate their face normals, rebuild proximity, and discard stale OK3 data.
CobInfo carve_cob_faces(const CobInfo& parsed,
                        const std::vector<uint32_t>& face_indices,
                        const Matrix4& local_delta,
                        bool clear_ok3 = true);
Aabb cob_referenced_vertex_bounds(const CobInfo& parsed);

// Apply only the separate-collision portion. The moved visual GAO's matrix is
// expected to have already been written into `entry` by modify_transform.
std::vector<uint8_t> apply_collision_follow(
    const std::vector<uint8_t>& entry,
    const CollisionLinks& links,
    const JadeMatrix& old_matrix,
    const JadeMatrix& new_matrix,
    const std::function<void(const std::string&)>& log = {});

}  // namespace collision_follow
}  // namespace jade
