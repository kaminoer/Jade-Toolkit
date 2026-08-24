// Zone.cpp — implementation. Faithful port of core/zone.py.
#include "jade/Zone.hpp"

#include <algorithm>
#include <map>

#include "jade/Compression.hpp"
#include "jade/Gao.hpp"
#include "jade/Keys.hpp"
#include "jade/Wol.hpp"
#include "jade/WowStream.hpp"

namespace jade {

namespace {

std::string to_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

}  // namespace

std::vector<ZoneInfo> discover_zones(const BigFile& bf) {
    std::map<std::string, const BFFile*> wow, wol;
    for (const auto& kv : bf.files) {       // ascending index == insertion order
        const BFFile& f = kv.second;
        if (f.key == INVALID_KEY || f.name.empty()) continue;
        size_t pw = f.name.find("_wow_");
        if (pw != std::string::npos) {
            std::string name = f.name.substr(0, pw);
            wow.emplace(name, &f);          // setdefault: first wins
        } else {
            size_t pl = f.name.find("_wol_");
            if (pl != std::string::npos) {
                std::string name = f.name.substr(0, pl);
                wol.emplace(name, &f);
            }
        }
    }
    std::vector<ZoneInfo> zones;
    for (const auto& w : wow) {              // std::map iterates name-sorted
        auto it = wol.find(w.first);
        if (it == wol.end()) continue;
        ZoneInfo z;
        z.name = w.first;
        z.wow_index = w.second->index;
        z.wol_index = it->second->index;
        z.wol_key = it->second->key;
        zones.push_back(std::move(z));
    }
    return zones;
}

void analyze_zone(const BigFile& bf, ZoneInfo& z,
                  const std::unordered_set<uint32_t>& resource_index) {
    LzoResult wr = decompress_lzo(bf.read_data(z.wow_index));
    std::vector<WowRecord> recs = walk_wow(wr.data);
    z.record_count = static_cast<uint32_t>(recs.size());
    if (!recs.empty()) z.map_prefix = static_cast<long long>(map_prefix_of(recs[0].key));

    for (const WowRecord& rec : recs) {
        if (rec.tag != ".gao") continue;
        ++z.gao_count;
        if (z.has_checkpoint) continue;
        GaoInfo info = parse_gao_record(wr.data.data() + rec.body_off, rec.size);
        if (info.ok && to_lower(info.name).find("checkpoint") != std::string::npos) {
            z.checkpoint_key = static_cast<long long>(rec.key);
            z.checkpoint_name = info.name;
            z.has_checkpoint = true;
        }
    }

    LzoResult lr = decompress_lzo(bf.read_data(z.wol_index));
    Wol wl = parse_wol(lr.data);
    if (wl.ok) {
        z.dep_total = static_cast<uint32_t>(wl.deps.size());
        z.dep_missing = static_cast<uint32_t>(wl.missing_deps(resource_index).size());
        if (z.map_prefix >= 0)
            z.wol_internal_key =
                (z.map_prefix << 24) | (static_cast<long long>(z.wol_key) & 0xFFFF);
    }
}

}  // namespace jade
