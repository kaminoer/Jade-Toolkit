// LoadSim.cpp — implementation. Faithful port of io_ops/load_sim.py.
#include "jade/LoadSim.hpp"

#include <unordered_set>

#include "jade/Compression.hpp"
#include "jade/Keys.hpp"
#include "jade/SubEntry.hpp"
#include "jade/WolInfo.hpp"

namespace jade {
namespace loadsim {

namespace {
inline uint32_t get_u32(const uint8_t* d, size_t o) {
    return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
           (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}

// Zone base name for referrer display: name.split("_wow")[0].
std::string zone_base(const std::string& name) {
    size_t p = name.find("_wow");
    return p == std::string::npos ? name : name.substr(0, p);
}
}  // namespace

std::vector<uint32_t> extract_refs(const SubEntry& s) {
    if (!s.gro_null && s.gro_type == 5 && s.data.size() >= 4) {
        const uint32_t value = get_u32(s.data.data(), s.data.size() - 4);
        if (keyshaped(value)) return {value};
    }
    return {};
}

KeyIndex build_key_index(const BigFile& bf) {
    KeyIndex idx;
    for (const auto& kv : bf.files) {
        const BFFile& f = kv.second;
        if (f.name.empty() || f.key == INVALID_KEY) continue;
        LzoResult r = decompress_lzo(bf.read_data(f.index));
        if (!r.ok) continue;
        std::vector<SubEntry> subs = walk_sub_entries(r.data);
        for (const SubEntry& s : subs) {
            auto it = idx.pos.find(s.key);
            size_t slot;
            if (it == idx.pos.end()) {
                slot = idx.items.size();
                idx.pos[s.key] = slot;
                idx.items.push_back({s.key, {}});
            } else {
                slot = it->second;
            }
            auto& provs = idx.items[slot].second;
            bool present = false;
            for (const auto& p : provs)
                if (p.first == f.key && p.second == f.name) { present = true; break; }
            if (!present) provs.push_back({f.key, f.name});
        }
    }
    return idx;
}

std::vector<DepBin> zone_dep_bins(const BigFile& bf, uint32_t wol_key) {
    std::vector<DepBin> out;
    const BFFile* wolinfo_fi = nullptr;
    for (const auto& kv : bf.files)
        if (kv.second.key == WOLINFO_KEY) { wolinfo_fi = &kv.second; break; }
    if (wolinfo_fi == nullptr) return out;
    WolInfo wi = parse_wolinfo(bf.read_data(wolinfo_fi->index));
    if (!wi.ok) return out;
    const WolEntry* e = wi.find(wol_key);
    if (e == nullptr) return out;

    std::unordered_map<uint32_t, const BFFile*> fat_by_key;
    for (const auto& kv : bf.files)
        if (!kv.second.name.empty()) fat_by_key[kv.second.key] = &kv.second;
    for (uint32_t d : e->deps) {
        DepBin db;
        db.internal_key = d;
        db.bf_key = internal_to_bf_key(d);
        auto it = fat_by_key.find(db.bf_key);
        if (it != fat_by_key.end()) {
            db.name = it->second->name;
            db.has_name = true;
        }
        out.push_back(std::move(db));
    }
    return out;
}

SimReport simulate_zone(const BigFile& bf, uint32_t wol_key,
                        const KeyIndex& key_index) {
    SimReport rep;
    rep.wol_key = wol_key;

    std::vector<DepBin> deps = zone_dep_bins(bf, wol_key);
    rep.n_deps = uint32_t(deps.size());

    std::unordered_map<uint32_t, const BFFile*> fat_by_key;
    for (const auto& kv : bf.files)
        if (!kv.second.name.empty()) fat_by_key[kv.second.key] = &kv.second;

    // closure = every sub-entry key across the dep bins (the load cache).
    std::unordered_set<uint32_t> closure;
    struct ZoneBin { uint32_t bf_key; std::string name; std::vector<SubEntry> subs; };
    std::vector<ZoneBin> zone_bins;
    for (const DepBin& d : deps) {
        auto it = fat_by_key.find(d.bf_key);
        if (it == fat_by_key.end()) {
            rep.missing_dep_bins.push_back({d.internal_key, d.bf_key, false});
            continue;
        }
        LzoResult r = decompress_lzo(bf.read_data(it->second->index));
        // Python only records "decompress-fail" when decompress_lzo throws.
        // A genuine corrupt-stream result is (None, n), then _safe_walk(None)
        // yields an empty list and the bin still counts as loaded.
        std::vector<SubEntry> subs;
        if (r.ok) subs = walk_sub_entries(r.data);
        for (const SubEntry& s : subs) closure.insert(s.key);
        zone_bins.push_back({d.bf_key, d.has_name ? it->second->name : "",
                             std::move(subs)});
    }
    rep.closure_size = closure.size();

    // Spin candidates: structured refs outside the closure that resolve to a
    // real provider bin elsewhere in the archive.
    std::unordered_map<uint32_t, size_t> unres_pos;
    for (const ZoneBin& zb : zone_bins) {
        for (const SubEntry& s : zb.subs) {
            for (uint32_t v : extract_refs(s)) {
                if (closure.count(v) || fat_by_key.count(v)) continue;
                const auto* prov = key_index.find(v);
                if (prov == nullptr || prov->empty()) continue;
                size_t slot;
                auto it = unres_pos.find(v);
                if (it == unres_pos.end()) {
                    slot = rep.unresolved.size();
                    unres_pos[v] = slot;
                    Unresolved u;
                    u.ref = v;
                    u.providers = *prov;
                    rep.unresolved.push_back(std::move(u));
                } else {
                    slot = it->second;
                }
                auto ref_tuple = std::make_tuple(zone_base(zb.name), s.key,
                                                 s.gro_null ? 0u : s.gro_type);
                auto& refs = rep.unresolved[slot].referrers;
                bool present = false;
                for (const auto& t : refs)
                    if (t == ref_tuple) { present = true; break; }
                if (!present) refs.push_back(std::move(ref_tuple));
            }
        }
    }
    rep.deps_loaded = uint32_t(zone_bins.size());
    return rep;
}

}  // namespace loadsim
}  // namespace jade
