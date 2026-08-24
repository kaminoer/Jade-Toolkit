#include "jade/LoaValidate.hpp"

#include <cstdio>
#include <unordered_map>

#include "jade/BigFile.hpp"
#include "jade/Collision.hpp"
#include "jade/Gao.hpp"
#include "jade/ObjectPlacer.hpp"
#include "jade/SubEntry.hpp"

namespace jade::loa {

bool is_modded_key(uint32_t key) {
    return (key & 0x7F000000u) == 0x7A000000u;
}

std::vector<DepRef> collect_dep_refs(
    const std::vector<uint8_t>& dec_bytes) {
    // Python intentionally uses its legacy cookie scanner here, rather than
    // the robust size walker used by most archive readers.
    const std::vector<SubEntry> subs = parse_sub_entries(dec_bytes);
    if (subs.empty()) return {};
    const placer::WorldListKeysResult world =
        placer::world_object_list_keys(subs);
    if (!world.found) return {};

    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& sub : subs) by_key[sub.key] = &sub;

    std::vector<DepRef> refs;
    for (uint32_t gao_key : world.keys) {
        const auto it = by_key.find(gao_key);
        if (it == by_key.end() || it->second->ext != ".gao") continue;
        const SubEntry& sub = *it->second;
        GaoInfo info;
        try {
            info = parse_gao_full(sub.data.data(), sub.data.size());
        } catch (...) {
            continue;
        }
        if (!info.ok) continue;
        if (info.vis_read) {
            if (info.gro_key != 0 && info.gro_key != INVALID_KEY)
                refs.push_back(
                    {gao_key, sub.offset, info.gro_key, "gro"});
            if (info.grm_key != 0 && info.grm_key != INVALID_KEY)
                refs.push_back(
                    {gao_key, sub.offset, info.grm_key, "grm"});
        }
        for (const auto& [offset, colmap_key] :
             gao_colmap_key_offsets(sub.data, by_key)) {
            (void)offset;
            refs.push_back(
                {gao_key, sub.offset, colmap_key, "colmap"});
        }
    }
    return refs;
}

std::vector<Issue> validate_loa_stream(
    const std::vector<uint8_t>& dec_bytes, bool ignore_shipped) {
    std::vector<Issue> issues;
    const std::vector<SubEntry> subs = parse_sub_entries(dec_bytes);
    if (subs.empty()) return issues;

    const placer::WorldListKeysResult world =
        placer::world_object_list_keys(subs);
    if (!world.found) {
        issues.push_back({
            "warning",
            "LOA validation skipped: no world object list found in entry"});
        return issues;
    }

    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& sub : subs) by_key[sub.key] = &sub;

    char message[320];
    for (const DepRef& ref : collect_dep_refs(dec_bytes)) {
        if (ignore_shipped && !is_modded_key(ref.gao_key) &&
            !is_modded_key(ref.dep_key)) {
            continue;
        }
        const auto dep = by_key.find(ref.dep_key);
        if (dep == by_key.end()) {
            std::snprintf(message, sizeof message,
                          "GAO 0x%08x references missing %s 0x%08x",
                          ref.gao_key, ref.dep_kind.c_str(), ref.dep_key);
            issues.push_back({"warning", message});
            continue;
        }
        const size_t dep_offset = dep->second->offset;
        if (dep_offset > ref.gao_offset) continue;

        const bool cached = ref.dep_kind != "colmap";
        std::snprintf(
            message, sizeof message,
            "LOA backward reference: GAO 0x%08x @%zu -> %s 0x%08x @%zu%s",
            ref.gao_key, ref.gao_offset, ref.dep_kind.c_str(), ref.dep_key,
            dep_offset,
            cached ? " (resource is engine-cached; may load from cache, but "
                     "stream-order is not satisfied)"
                   : " (OnlyOneRef resource — will hang the speed-stream)");
        issues.push_back({cached ? "warning" : "error", message});
    }
    return issues;
}

}  // namespace jade::loa
