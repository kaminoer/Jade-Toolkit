#include "jade/CollisionFollow.hpp"

#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/ObjectPlacer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace jade {
namespace collision_follow {
namespace {

constexpr uint32_t FLAG_COLMAP = 0x00000100u;
constexpr uint32_t FLAG_VISUAL = 0x00004000u;
constexpr uint32_t INVALID_KEY = 0xFFFFFFFFu;

std::string hex_key(uint32_t key) {
    char text[11];
    std::snprintf(text, sizeof text, "0x%08X", key);
    return text;
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value)); out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value >> 16)); out.push_back(uint8_t(value >> 24));
}

JadeMatrix gao_matrix(const SubEntry& gao, bool* ok = nullptr) {
    JadeMatrix result{};
    const long long offset = global_matrix_offset(gao.data.data(), gao.data.size());
    const bool valid = offset >= 0 && size_t(offset) + 64 <= gao.data.size();
    if (ok) *ok = valid;
    if (!valid) return result;
    for (size_t i = 0; i < 16; ++i) {
        float value;
        std::memcpy(&value, gao.data.data() + size_t(offset) + i * 4, 4);
        result[i] = double(value);
    }
    return result;
}

std::unordered_map<uint32_t, const SubEntry*> by_key_of(
    const std::vector<SubEntry>& subs) {
    std::unordered_map<uint32_t, const SubEntry*> result;
    result.reserve(subs.size());
    for (const SubEntry& sub : subs) result.emplace(sub.key, &sub);
    return result;
}

std::vector<uint32_t> gao_cob_keys(
    const SubEntry& gao,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key) {
    std::vector<uint32_t> result;
    for (const auto& hit : jade::gao_colmap_key_offsets(gao.data, by_key)) {
        auto found = by_key.find(hit.second);
        const auto keys = jade::colmap_cob_keys(
            found == by_key.end() ? nullptr : found->second, by_key);
        result.insert(result.end(), keys.begin(), keys.end());
    }
    return result;
}

Aabb aabb_of_points(const std::vector<std::array<double, 3>>& points) {
    Aabb result;
    if (points.empty()) return result;
    result.ok = true;
    result.min = result.max = points.front();
    for (const auto& point : points)
        for (size_t axis = 0; axis < 3; ++axis) {
            result.min[axis] = std::min(result.min[axis], point[axis]);
            result.max[axis] = std::max(result.max[axis], point[axis]);
        }
    return result;
}

std::array<double, 3> center(const Aabb& aabb) {
    return {{(aabb.min[0] + aabb.max[0]) * 0.5,
             (aabb.min[1] + aabb.max[1]) * 0.5,
             (aabb.min[2] + aabb.max[2]) * 0.5}};
}

bool contains(const Aabb& aabb, const std::array<double, 3>& point) {
    if (!aabb.ok) return false;
    for (size_t axis = 0; axis < 3; ++axis)
        if (point[axis] < aabb.min[axis] || point[axis] > aabb.max[axis])
            return false;
    return true;
}

Aabb expanded(Aabb aabb, double fraction, double absolute) {
    if (!aabb.ok) return aabb;
    for (size_t axis = 0; axis < 3; ++axis) {
        const double amount = (aabb.max[axis] - aabb.min[axis]) * fraction
                            + absolute;
        aabb.min[axis] -= amount; aabb.max[axis] += amount;
    }
    return aabb;
}

double volume(const Aabb& aabb) {
    if (!aabb.ok) return 0.0;
    double result = 1.0;
    for (size_t axis = 0; axis < 3; ++axis)
        result *= std::max(0.0, aabb.max[axis] - aabb.min[axis]);
    return result;
}

std::array<double, 3> transform_point(const Matrix4& matrix,
                                      const std::array<double, 3>& point) {
    return {{matrix[0]*point[0] + matrix[1]*point[1] + matrix[2]*point[2] + matrix[3],
             matrix[4]*point[0] + matrix[5]*point[1] + matrix[6]*point[2] + matrix[7],
             matrix[8]*point[0] + matrix[9]*point[1] + matrix[10]*point[2] + matrix[11]}};
}

uint32_t ltype(const JadeMatrix& matrix) {
    uint32_t result = 0;
    if (std::abs(matrix[12]) > 1e-5 || std::abs(matrix[13]) > 1e-5
        || std::abs(matrix[14]) > 1e-5) result |= 2;
    const bool identity = std::abs(matrix[0]-1) < 1e-5 && std::abs(matrix[1]) < 1e-5
        && std::abs(matrix[2]) < 1e-5 && std::abs(matrix[4]) < 1e-5
        && std::abs(matrix[5]-1) < 1e-5 && std::abs(matrix[6]) < 1e-5
        && std::abs(matrix[8]) < 1e-5 && std::abs(matrix[9]) < 1e-5
        && std::abs(matrix[10]-1) < 1e-5;
    if (!identity) result |= 4;
    if (std::abs(matrix[3]) > 1e-5 || std::abs(matrix[7]) > 1e-5
        || std::abs(matrix[11]) > 1e-5) result |= 8;
    return result;
}

std::vector<uint8_t> gao_with_matrix(const SubEntry& sub,
                                     const JadeMatrix& matrix) {
    std::vector<uint8_t> result = write_global_matrix(
        sub.data.data(), sub.data.size(), matrix);
    const long long offset = global_matrix_offset(result.data(), result.size());
    if (result.empty() || offset < 0 || size_t(offset) + 68 > result.size())
        return {};
    const uint32_t type = ltype(matrix);
    for (size_t i = 0; i < 4; ++i)
        result[size_t(offset) + 64 + i] = uint8_t(type >> (i * 8));
    return result;
}

std::vector<uint8_t> splice_sub_entry(const std::vector<uint8_t>& entry,
                                      const SubEntry& sub,
                                      const std::vector<uint8_t>& payload) {
    if (sub.offset < 4) throw std::runtime_error("collision-follow: invalid sub-entry offset");
    const size_t begin = sub.offset - 4;
    const size_t end = sub.offset + 8 + size_t(sub.size);
    if (begin > entry.size() || end > entry.size())
        throw std::runtime_error("collision-follow: sub-entry exceeds entry");
    std::vector<uint8_t> record;
    put32(record, uint32_t(4 + payload.size()));
    put32(record, JADE_COOKIE);
    put32(record, sub.key);
    if (!sub.ext.empty()) {
        for (size_t i = 0; i < 4; ++i)
            record.push_back(i < sub.ext.size() ? uint8_t(sub.ext[i]) : 0);
    } else put32(record, sub.gro_null ? 0u : sub.gro_type);
    record.insert(record.end(), payload.begin(), payload.end());
    std::vector<uint8_t> result;
    result.reserve(entry.size() - (end - begin) + record.size());
    result.insert(result.end(), entry.begin(), entry.begin() + std::ptrdiff_t(begin));
    result.insert(result.end(), record.begin(), record.end());
    result.insert(result.end(), entry.begin() + std::ptrdiff_t(end), entry.end());
    return result;
}

const SubEntry* find_sub(const std::vector<SubEntry>& subs, uint32_t key) {
    for (const SubEntry& sub : subs) if (sub.key == key) return &sub;
    return nullptr;
}

}  // namespace

Matrix4 matrix_multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            for (size_t inner = 0; inner < 4; ++inner)
                result[row*4+column] += a[row*4+inner] * b[inner*4+column];
    return result;
}

Matrix4 jade16_to_math(const JadeMatrix& matrix) {
    const double sx = std::abs(matrix[3]) > 1e-9 ? matrix[3] : 1.0;
    const double sy = std::abs(matrix[7]) > 1e-9 ? matrix[7] : 1.0;
    const double sz = std::abs(matrix[11]) > 1e-9 ? matrix[11] : 1.0;
    return {{matrix[0]*sx, matrix[4]*sy, matrix[8]*sz, matrix[12],
             matrix[1]*sx, matrix[5]*sy, matrix[9]*sz, matrix[13],
             matrix[2]*sx, matrix[6]*sy, matrix[10]*sz, matrix[14],
             0,0,0,1}};
}

JadeMatrix math_to_jade16(const Matrix4& matrix) {
    JadeMatrix result{};
    for (size_t column = 0; column < 3; ++column) {
        const double x = matrix[column], y = matrix[4+column], z = matrix[8+column];
        const double scale = std::sqrt(x*x + y*y + z*z);
        const double denominator = scale > 1e-12 ? scale : 1.0;
        result[column*4] = x / denominator;
        result[column*4+1] = y / denominator;
        result[column*4+2] = z / denominator;
        result[column*4+3] = std::abs(scale - 1.0) < 1e-5 ? 0.0 : scale;
    }
    result[12] = matrix[3]; result[13] = matrix[7]; result[14] = matrix[11];
    result[15] = 1.0;
    return result;
}

Matrix4 matrix_inverse_affine(const Matrix4& matrix) {
    const double a=matrix[0], b=matrix[1], c=matrix[2];
    const double d=matrix[4], e=matrix[5], f=matrix[6];
    const double g=matrix[8], h=matrix[9], i=matrix[10];
    const double A=e*i-f*h, B=c*h-b*i, C=b*f-c*e;
    const double D=f*g-d*i, E=a*i-c*g, F=c*d-a*f;
    const double G=d*h-e*g, H=b*g-a*h, I=a*e-b*d;
    const double determinant = a*A + b*D + c*G;
    if (std::abs(determinant) < 1e-20)
        throw std::runtime_error("collision-follow: singular transform matrix");
    const double r = 1.0 / determinant;
    Matrix4 result{{A*r,B*r,C*r,0, D*r,E*r,F*r,0, G*r,H*r,I*r,0, 0,0,0,1}};
    const double tx=matrix[3], ty=matrix[7], tz=matrix[11];
    result[3] = -(result[0]*tx + result[1]*ty + result[2]*tz);
    result[7] = -(result[4]*tx + result[5]*ty + result[6]*tz);
    result[11] = -(result[8]*tx + result[9]*ty + result[10]*tz);
    return result;
}

Matrix4 world_delta(const JadeMatrix& old_matrix, const JadeMatrix& new_matrix) {
    return matrix_multiply(jade16_to_math(new_matrix),
                           matrix_inverse_affine(jade16_to_math(old_matrix)));
}

Aabb world_aabb_from_local_bounds(const JadeMatrix& matrix,
                                  const Aabb& local_bounds) {
    if (!local_bounds.ok) return {};
    const Matrix4 transform = jade16_to_math(matrix);
    std::vector<std::array<double,3>> points;
    points.reserve(8);
    for (double x : {local_bounds.min[0], local_bounds.max[0]})
        for (double y : {local_bounds.min[1], local_bounds.max[1]})
            for (double z : {local_bounds.min[2], local_bounds.max[2]})
                points.push_back(transform_point(transform, {{x,y,z}}));
    return aabb_of_points(points);
}

Aabb gao_local_bounds(const SubEntry& gao,
                      const std::vector<SubEntry>& subs) {
    const ObboxBounds obbox = obbox_local_bounds(gao.data.data(), gao.data.size());
    if (obbox.ok) return {true, obbox.mn, obbox.mx};
    const auto by_key = by_key_of(subs);
    const GaoInfo info = parse_gao_full(gao.data.data(), gao.data.size());
    if (!info.ok) return {};
    if (info.vis_read) {
        auto found = by_key.find(info.gro_key);
        if (found != by_key.end() && !found->second->gro_null
            && found->second->gro_type == 1) {
            const GeoInfo geometry = parse_geometry(found->second->data.data(),
                                                    found->second->data.size());
            std::vector<std::array<double,3>> points;
            if (geometry.ok && !geometry.ps2)
                for (size_t i = 0; i + 2 < geometry.vertices.size(); i += 3)
                    points.push_back({{geometry.vertices[i], geometry.vertices[i+1],
                                       geometry.vertices[i+2]}});
            const Aabb result = aabb_of_points(points);
            if (result.ok) return result;
        }
    }
    std::vector<std::array<double,3>> points;
    for (uint32_t key : gao_cob_keys(gao, by_key)) {
        auto found = by_key.find(key);
        if (found == by_key.end()) continue;
        const CobInfo cob = parse_cob(found->second->data.data(), found->second->data.size());
        if (!cob.ok) continue;
        for (size_t i = 0; i + 2 < cob.verts.size(); i += 3)
            points.push_back({{cob.verts[i], cob.verts[i+1], cob.verts[i+2]}});
    }
    return aabb_of_points(points);
}

Aabb gao_world_aabb(const SubEntry& gao,
                    const std::vector<SubEntry>& subs) {
    bool ok = false;
    const JadeMatrix matrix = gao_matrix(gao, &ok);
    return ok ? world_aabb_from_local_bounds(matrix, gao_local_bounds(gao, subs))
              : Aabb{};
}

CollisionLinks detect_collision_links(uint32_t moved_gao_key,
                                      const std::vector<SubEntry>& subs,
                                      double margin_fraction,
                                      double margin_absolute) {
    CollisionLinks links;
    links.moved_gao_key = moved_gao_key;
    const auto by_key = by_key_of(subs);
    auto moved_found = by_key.find(moved_gao_key);
    if (moved_found == by_key.end() || moved_found->second->ext != ".gao")
        return links;
    const SubEntry& moved = *moved_found->second;
    const GaoInfo moved_info = parse_gao_full(moved.data.data(), moved.data.size());
    if (!moved_info.ok) return links;
    links.same_gao = (moved_info.identity & FLAG_COLMAP) != 0;
    links.moved_world_aabb = gao_world_aabb(moved, subs);
    if (!links.moved_world_aabb.ok) return links;
    const auto moved_center = center(links.moved_world_aabb);
    const Aabb query = expanded(links.moved_world_aabb,
                                margin_fraction, margin_absolute);
    const double moved_volume = volume(links.moved_world_aabb);

    std::unordered_set<uint32_t> moved_cobs;
    for (uint32_t key : gao_cob_keys(moved, by_key)) moved_cobs.insert(key);
    struct GaoRecord { const SubEntry* sub; GaoInfo info; };
    std::unordered_map<uint32_t, GaoRecord> gaos;
    std::vector<uint32_t> gao_order;
    std::unordered_map<uint32_t, uint32_t> cob_owner;
    for (const SubEntry& sub : subs) {
        if (sub.ext != ".gao") continue;
        GaoInfo info = parse_gao_full(sub.data.data(), sub.data.size());
        if (!info.ok) continue;
        gaos.emplace(sub.key, GaoRecord{&sub, std::move(info)});
        gao_order.push_back(sub.key);
        if (gaos.at(sub.key).info.identity & FLAG_COLMAP)
            for (uint32_t cob : gao_cob_keys(sub, by_key))
                cob_owner.emplace(cob, sub.key);
    }
    auto gao_aabb = [&](uint32_t key) {
        auto found = gaos.find(key);
        return found == gaos.end() ? Aabb{} : gao_world_aabb(*found->second.sub, subs);
    };

    constexpr double max_dedicated_volume_ratio = 30.0;
    std::unordered_set<uint32_t> dedicated_cobs;
    for (uint32_t key : gao_order) {
        const GaoRecord& record = gaos.at(key);
        if (key == moved_gao_key) continue;
        if (!(record.info.identity & FLAG_COLMAP)
            || (record.info.identity & FLAG_VISUAL)) continue;
        const Aabb candidate = gao_aabb(key);
        if (!candidate.ok) continue;
        const bool colocated = contains(query, center(candidate))
            || contains(expanded(candidate, margin_fraction, margin_absolute),
                        moved_center);
        if (!colocated) continue;
        if (moved_volume > 1e-9
            && volume(candidate) > max_dedicated_volume_ratio * moved_volume)
            continue;
        DedicatedLink link;
        link.gao_key = key;
        link.name = record.info.name.empty()
            ? "gao_0x" + [&]{ char b[9]; std::snprintf(b, sizeof b, "%08X", key); return std::string(b); }()
            : record.info.name;
        link.cob_keys = gao_cob_keys(*record.sub, by_key);
        link.world_aabb = candidate;
        for (uint32_t cob : link.cob_keys) dedicated_cobs.insert(cob);
        links.dedicated.push_back(std::move(link));
    }

    for (const SubEntry& cob_sub : subs) {
        if (!jade::looks_like_cob_sub(&cob_sub) || moved_cobs.count(cob_sub.key)
            || dedicated_cobs.count(cob_sub.key)) continue;
        const CobInfo cob = parse_cob(cob_sub.data.data(), cob_sub.data.size());
        if (!cob.ok || cob.verts.empty()) continue;
        auto owner_found = cob_owner.find(cob_sub.key);
        if (owner_found == cob_owner.end()) continue;
        auto gao_found = gaos.find(owner_found->second);
        if (gao_found == gaos.end()) continue;
        bool matrix_ok = false;
        const JadeMatrix owner_jade = gao_matrix(*gao_found->second.sub, &matrix_ok);
        if (!matrix_ok) continue;
        if (!(gao_found->second.info.identity & FLAG_VISUAL)) {
            const double owner_volume = volume(gao_aabb(owner_found->second));
            if (moved_volume > 1e-9
                && owner_volume <= max_dedicated_volume_ratio * moved_volume)
                continue;
        }
        const Matrix4 owner_matrix = jade16_to_math(owner_jade);
        std::vector<uint32_t> selected;
        uint32_t face_index = 0;
        for (const CobElement& element : cob.elements)
            for (size_t f = 0; f + 2 < element.faces.size(); f += 3, ++face_index) {
                const uint16_t a=element.faces[f], b=element.faces[f+1], c=element.faces[f+2];
                if (a >= cob.verts.size()/3 || b >= cob.verts.size()/3
                    || c >= cob.verts.size()/3) continue;
                std::array<double,3> centroid{};
                for (size_t axis = 0; axis < 3; ++axis)
                    centroid[axis] = (double(cob.verts[size_t(a)*3+axis])
                        + double(cob.verts[size_t(b)*3+axis])
                        + double(cob.verts[size_t(c)*3+axis])) / 3.0;
                if (contains(query, transform_point(owner_matrix, centroid)))
                    selected.push_back(face_index);
            }
        if (!selected.empty()) {
            CarveLink link;
            link.cob_key = cob_sub.key;
            link.owner_gao_key = owner_found->second;
            link.owner_name = gao_found->second.info.name.empty()
                ? "gao" : gao_found->second.info.name;
            link.face_indices = std::move(selected);
            link.n_total_faces = cob.total_tris;
            links.carves.push_back(std::move(link));
        }
    }
    return links;
}

CobInfo carve_cob_faces(const CobInfo& parsed,
                        const std::vector<uint32_t>& face_indices,
                        const Matrix4& local_delta,
                        bool clear_ok3) {
    if (!parsed.ok) throw std::runtime_error("collision-follow: unparseable COB");
    CobInfo result = parsed;
    std::unordered_set<uint32_t> selected(face_indices.begin(), face_indices.end());
    std::unordered_map<uint16_t, uint16_t> duplicates;
    auto moved_vertex = [&](uint16_t original) -> uint16_t {
        auto found = duplicates.find(original);
        if (found != duplicates.end()) return found->second;
        if (size_t(original) * 3 + 2 >= parsed.verts.size())
            throw std::runtime_error("collision-follow: COB face has invalid vertex index");
        if (result.verts.size() / 3 >= 65536)
            throw std::runtime_error("collision-follow: carved COB exceeds 16-bit vertices");
        const std::array<double,3> point{{parsed.verts[size_t(original)*3],
                                          parsed.verts[size_t(original)*3+1],
                                          parsed.verts[size_t(original)*3+2]}};
        const auto transformed = transform_point(local_delta, point);
        const uint16_t index = uint16_t(result.verts.size() / 3);
        for (double value : transformed) result.verts.push_back(float(value));
        duplicates.emplace(original, index);
        return index;
    };
    uint32_t global_face = 0;
    for (CobElement& element : result.elements)
        for (size_t f = 0; f + 2 < element.faces.size(); f += 3, ++global_face)
            if (selected.count(global_face)) {
                element.faces[f] = moved_vertex(element.faces[f]);
                element.faces[f+1] = moved_vertex(element.faces[f+1]);
                element.faces[f+2] = moved_vertex(element.faces[f+2]);
                if (size_t(global_face) * 3 + 2 < result.normals.size()) {
                    const double x=result.normals[size_t(global_face)*3];
                    const double y=result.normals[size_t(global_face)*3+1];
                    const double z=result.normals[size_t(global_face)*3+2];
                    double nx=local_delta[0]*x+local_delta[1]*y+local_delta[2]*z;
                    double ny=local_delta[4]*x+local_delta[5]*y+local_delta[6]*z;
                    double nz=local_delta[8]*x+local_delta[9]*y+local_delta[10]*z;
                    const double length=std::sqrt(nx*nx+ny*ny+nz*nz);
                    if (length > 1e-9) { nx/=length; ny/=length; nz/=length; }
                    result.normals[size_t(global_face)*3]=float(nx);
                    result.normals[size_t(global_face)*3+1]=float(ny);
                    result.normals[size_t(global_face)*3+2]=float(nz);
                }
            }
    std::vector<std::array<uint16_t,3>> faces;
    for (const CobElement& element : result.elements)
        for (size_t f = 0; f + 2 < element.faces.size(); f += 3)
            faces.push_back({{element.faces[f], element.faces[f+1], element.faces[f+2]}});
    result.proximity.clear();
    for (const auto& row : triangle_proximity(faces))
        result.proximity.insert(result.proximity.end(), row.begin(), row.end());
    result.n_verts = uint32_t(result.verts.size()/3);
    if (clear_ok3) {
        result.flag &= uint8_t(~COL_C_Cob_OK3);
        result.trailing = {0,0,0,0};
    }
    return result;
}

Aabb cob_referenced_vertex_bounds(const CobInfo& parsed) {
    std::unordered_set<uint16_t> used;
    for (const CobElement& element : parsed.elements)
        used.insert(element.faces.begin(), element.faces.end());
    std::vector<std::array<double,3>> points;
    points.reserve(used.size());
    for (uint16_t index : used)
        if (size_t(index)*3+2 < parsed.verts.size())
            points.push_back({{parsed.verts[size_t(index)*3],
                               parsed.verts[size_t(index)*3+1],
                               parsed.verts[size_t(index)*3+2]}});
    return aabb_of_points(points);
}

std::vector<uint8_t> apply_collision_follow(
    const std::vector<uint8_t>& entry,
    const CollisionLinks& links,
    const JadeMatrix& old_matrix,
    const JadeMatrix& new_matrix,
    const std::function<void(const std::string&)>& log) {
    std::vector<uint8_t> result = entry;
    const Matrix4 delta = world_delta(old_matrix, new_matrix);
    if (!links.dedicated.empty()) {
        std::vector<SubEntry> subs = walk_sub_entries(result);
        for (const DedicatedLink& link : links.dedicated) {
            const SubEntry* sub = find_sub(subs, link.gao_key);
            if (!sub || sub->ext != ".gao") continue;
            bool ok = false;
            const JadeMatrix old_collision = gao_matrix(*sub, &ok);
            if (!ok) continue;
            const JadeMatrix moved = math_to_jade16(
                matrix_multiply(delta, jade16_to_math(old_collision)));
            const std::vector<uint8_t> payload = gao_with_matrix(*sub, moved);
            if (payload.empty()) continue;
            const long long offset = global_matrix_offset(sub->data.data(), sub->data.size());
            if (offset < 0) continue;
            const size_t entry_offset = sub->offset + 12 + size_t(offset);
            if (entry_offset + 68 > result.size()) continue;
            std::copy(payload.begin() + offset, payload.begin() + offset + 68,
                      result.begin() + std::ptrdiff_t(entry_offset));
            if (log) log("collision-follow: rigid-moved dedicated COB host "
                         + hex_key(link.gao_key) + " (" + link.name + ")");
        }
    }
    for (const CarveLink& link : links.carves) {
        if (link.face_indices.empty()) continue;
        std::vector<SubEntry> subs = walk_sub_entries(result);
        const SubEntry* cob_sub = find_sub(subs, link.cob_key);
        const SubEntry* owner = find_sub(subs, link.owner_gao_key);
        if (!cob_sub) continue;
        const CobInfo parsed = parse_cob(cob_sub->data.data(), cob_sub->data.size());
        bool owner_ok = false;
        const JadeMatrix owner_jade = owner ? gao_matrix(*owner, &owner_ok) : JadeMatrix{};
        if (!parsed.ok) {
            if (log) log("collision-follow: SKIP carve "
                         + hex_key(link.cob_key) + " (unparseable COB)");
            continue;
        }
        if (!owner_ok) {
            if (log) log("collision-follow: SKIP carve "
                         + hex_key(link.cob_key) + " (no owner matrix)");
            continue;
        }
        const Matrix4 owner_matrix = jade16_to_math(owner_jade);
        const Matrix4 local_delta = matrix_multiply(
            matrix_inverse_affine(owner_matrix), matrix_multiply(delta, owner_matrix));
        const CobInfo carved = carve_cob_faces(parsed, link.face_indices, local_delta);
        result = splice_sub_entry(result, *cob_sub, serialize_cob(carved));
        if (log) log("collision-follow: carved "
                     + std::to_string(link.face_indices.size())
                     + " triangle(s) from " + hex_key(link.cob_key)
                     + " (" + link.owner_name + ")");

        const Aabb bounds = cob_referenced_vertex_bounds(carved);
        if (!bounds.ok) continue;
        subs = walk_sub_entries(result);
        owner = find_sub(subs, link.owner_gao_key);
        if (!owner) continue;
        const std::vector<uint8_t> widened = extend_obbox_to_include(
            owner->data.data(), owner->data.size(), bounds.min, bounds.max);
        if (!widened.empty() && widened != owner->data)
            result = splice_sub_entry(result, *owner, widened);
    }
    return result;
}

}  // namespace collision_follow
}  // namespace jade
