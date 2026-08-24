// MeshSwapResolver.hpp — port of io_ops/mesh_swap.py CrossBinResolver.
//
// Resolves Jade material/texture keys across BigFile bins: PoP ships
// geometry + stub materials in a <X>_Shape bin and the real, fuller
// materials in <X>_MAT. Centralising the lookup + the "best covering family
// material" search keeps the Mesh Swap table, the Preview panel's 3D view
// and the operations that edit them in agreement with the engine's
// load-time wow merge.
//
// Shared by MeshSwapTab (material table) and PreviewPanel (per-element
// texture / render-mode resolution + cross-bin texture decode) — the same
// consumers the Python CrossBinResolver serves via get_cached_resolver.
#pragma once

#include <QtGlobal>

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "jade/AssetIndex.hpp"
#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Material.hpp"
#include "jade/SubEntry.hpp"

class MeshSwapResolver {
public:
    MeshSwapResolver(jade::BigFile* bf,
                     std::shared_ptr<jade::AssetIndex> index,
                     quint32 local_index,
                     std::vector<jade::SubEntry> local_subs)
        : bf_(bf), index_(std::move(index)) {
        cache_[local_index] = std::move(local_subs);
    }

    const std::vector<jade::SubEntry>& subs_of(quint32 parent_index) {
        auto it = cache_.find(parent_index);
        if (it != cache_.end()) return it->second;
        std::vector<jade::SubEntry> subs;
        try {
            const jade::LzoResult r =
                jade::decompress_lzo(bf_->read_data(parent_index));
            if (r.ok) subs = jade::walk_sub_entries(r.data);
        } catch (const std::exception&) {
        }
        return cache_.emplace(parent_index, std::move(subs)).first->second;
    }

    // (sub, parent_index) for `key` — the largest payload wins (Jade ships
    // textures and some materials twice: a stub then the real record).
    // {nullptr, -1} if not found.
    std::pair<const jade::SubEntry*, long long> lookup(quint32 key) {
        if (!key) return {nullptr, -1};
        auto hit = lookup_cache_.find(key);
        if (hit != lookup_cache_.end()) return hit->second;
        std::vector<quint32> parents;
        for (const auto& kv : cache_) parents.push_back(kv.first);
        if (index_) {
            auto it = index_->by_key.find(key);
            if (it != index_->by_key.end())
                for (size_t ri : it->second) {
                    const quint32 pidx =
                        index_->records[ri].parent_index;
                    if (!cache_.count(pidx)) parents.push_back(pidx);
                }
        }
        std::pair<const jade::SubEntry*, long long> best{nullptr, -1};
        size_t best_len = 0;
        for (quint32 pidx : parents)
            for (const jade::SubEntry& s : subs_of(pidx))
                if (s.key == key
                    && (!best.first || s.data.size() > best_len)) {
                    best = {&s, pidx};
                    best_len = s.data.size();
                }
        lookup_cache_[key] = best;
        return best;
    }

    // (sub_material_keys, texture per slot [0 == None]) for a container.
    std::pair<std::vector<quint32>, std::vector<quint32>>
    container_slot_textures(quint32 cont_key) {
        auto hit = slot_tex_cache_.find(cont_key);
        if (hit != slot_tex_cache_.end()) return hit->second;
        std::vector<quint32> sub_keys, texs;
        auto [s, pidx] = lookup(cont_key);
        if (s) {
            const jade::MatInfo mm = jade::parse_material(
                s->data.data(), s->data.size(),
                jade::GRO_TYPE_MAT_MULTI);
            if (mm.ok) {
                for (uint32_t sk : mm.sub_material_keys)
                    sub_keys.push_back(sk);
                for (quint32 sk : sub_keys) {
                    quint32 tk = 0;
                    if (sk && sk != 0xFFFFFFFFu) {
                        auto [sm, spidx] = lookup(sk);
                        if (sm)
                            tk = jade::resolve_texture_key(
                                sm->data.data(), sm->data.size());
                    }
                    texs.push_back(tk);
                }
            }
        }
        return slot_tex_cache_
            .emplace(cont_key, std::make_pair(sub_keys, texs))
            .first->second;
    }

    // The MAT-bin material the engine substitutes for a Shape-bin stub —
    // RESTRICTED to the known TT Prince-loading stub so it never hijacks an
    // ordinary mesh (see the Python's _PRINCE_LOADING_STUB_GRMS note).
    // Returns true + outputs when the override applies.
    bool find_material_override(quint32 stub_key, qint64 max_mat,
                                quint32 geo_key, quint32& cont_key_out,
                                long long& pidx_out,
                                std::vector<quint32>& keys_out) {
        if (!index_) return false;
        if (stub_key != 0x6D002657u) return false;
        auto [stub_sub_keys, stub_texs] =
            container_slot_textures(stub_key);
        std::set<quint32> stub_real;
        for (quint32 t : stub_texs)
            if (t) stub_real.insert(t);
        const size_t stub_distinct = stub_real.size();
        // Trigger only when the stub is a placeholder or structurally too
        // small: a degenerate container has one texture dominating >= half
        // its slots.
        auto is_degenerate = [](const std::vector<quint32>& texs) {
            std::map<quint32, int> counts;
            int real = 0;
            for (quint32 t : texs)
                if (t) { ++counts[t]; ++real; }
            if (!real) return false;
            int most = 0;
            for (const auto& [t, n] : counts) most = std::max(most, n);
            return double(most) / double(texs.size()) >= 0.5;
        };
        if (!stub_sub_keys.empty()
            && qint64(stub_sub_keys.size()) > max_mat
            && !is_degenerate(stub_texs))
            return false;
        // Candidate families: the stub's own family, its slots' families,
        // and the geo's — pull only those families' gro-4 containers.
        std::set<quint32> fams;
        for (quint32 k : stub_sub_keys) fams.insert(k & 0xFFFF0000u);
        fams.insert(stub_key & 0xFFFF0000u);
        fams.insert(geo_key & 0xFFFF0000u);
        build_family_map();
        bool have_best = false;
        std::pair<size_t, size_t> best_score{0, 0};
        quint32 best_key = 0;
        long long best_pidx = -1;
        std::vector<quint32> best_keys;
        for (quint32 fam : fams) {
            auto fit = containers_by_family_.find(fam);
            if (fit == containers_by_family_.end()) continue;
            for (quint32 ckey : fit->second) {
                if (ckey == stub_key) continue;
                auto [sub_keys, texs] = container_slot_textures(ckey);
                if (qint64(sub_keys.size()) <= max_mat)
                    continue;  // doesn't cover the matId range
                std::set<quint32> real;
                for (quint32 t : texs)
                    if (t) real.insert(t);
                std::pair<size_t, size_t> score;
                if (!stub_real.empty()) {
                    size_t shared = 0;
                    for (quint32 t : stub_real)
                        if (real.count(t)) ++shared;
                    if (!shared || real.size() <= stub_distinct)
                        continue;  // not the stub's richer twin
                    score = {shared, real.size()};
                } else {
                    score = {0, real.size()};
                }
                if (!have_best || score > best_score) {
                    auto [s, pidx] = lookup(ckey);
                    have_best = true;
                    best_score = score;
                    best_key = ckey;
                    best_pidx = pidx;
                    best_keys = sub_keys;
                }
            }
        }
        if (!have_best) return false;
        cont_key_out = best_key;
        pidx_out = best_pidx;
        keys_out = best_keys;
        return true;
    }

private:
    // {family (key & 0xFFFF0000) -> gro-4 container keys} from the index —
    // the Python's index.material_containers_by_family() equivalent.
    void build_family_map() {
        if (family_map_built_) return;
        family_map_built_ = true;
        for (const auto& family : index_->material_containers_by_family())
            for (size_t record_index : family.second) {
                const jade::AssetRecord& rec = index_->records[record_index];
                containers_by_family_[rec.key & 0xFFFF0000u]
                    .insert(rec.key);
            }
    }

    jade::BigFile* bf_;
    std::shared_ptr<jade::AssetIndex> index_;
    std::map<quint32, std::vector<jade::SubEntry>> cache_;
    std::map<quint32, std::pair<const jade::SubEntry*, long long>>
        lookup_cache_;
    std::map<quint32,
             std::pair<std::vector<quint32>, std::vector<quint32>>>
        slot_tex_cache_;
    bool family_map_built_ = false;
    std::map<quint32, std::set<quint32>> containers_by_family_;
};
