// ZoneTransplant.cpp — implementation. Faithful port of
// io_ops/zone_transplant.py (cross-game zone + dep-closure clone).
#include "jade/ZoneTransplant.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

#include "jade/Compression.hpp"
#include "jade/Keys.hpp"
#include "jade/Wol.hpp"
#include "jade/WowStream.hpp"
#include "jade/Zone.hpp"

namespace jade {
namespace zonetx {

namespace {
inline uint32_t get_u32(const uint8_t* d, size_t o) {
    return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
           (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}
inline void set_u32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = uint8_t(x); v[o + 1] = uint8_t(x >> 8);
    v[o + 2] = uint8_t(x >> 16); v[o + 3] = uint8_t(x >> 24);
}
constexpr char WOW_TAG[4] = {'.', 'w', 'o', 'w'};

std::string lower_ascii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string python_ascii_repr(const std::string& value) {
    const bool use_double = value.find('\'') != std::string::npos &&
                            value.find('"') == std::string::npos;
    const char quote = use_double ? '"' : '\'';
    std::string out(1, quote);
    char escaped[5];
    for (unsigned char ch : value) {
        if (ch == static_cast<unsigned char>(quote) || ch == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch < 0x20 || ch == 0x7f) {
            std::snprintf(escaped, sizeof escaped, "\\x%02x", ch);
            out += escaped;
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    out.push_back(quote);
    return out;
}

std::string comma_integer(uint64_t value) {
    std::string digits = std::to_string(value);
    for (long long pos = static_cast<long long>(digits.size()) - 3;
         pos > 0; pos -= 3)
        digits.insert(static_cast<size_t>(pos), 1, ',');
    return digits;
}
}  // namespace

std::pair<std::vector<uint8_t>, int> filter_wol_deps(
    const std::vector<uint8_t>& wol_bytes,
    const std::unordered_set<uint32_t>& exclude_keys) {
    if (wol_bytes.size() < 12 || exclude_keys.empty()) return {wol_bytes, 0};

    // first = wol_bytes.find(b'.wow', 12)
    long long first = -1;
    for (size_t i = 12; i + 4 <= wol_bytes.size(); ++i)
        if (std::memcmp(wol_bytes.data() + i, WOW_TAG, 4) == 0) {
            first = static_cast<long long>(i);
            break;
        }
    if (first < 0) return {wol_bytes, 0};

    std::vector<uint8_t> prefix(wol_bytes.begin(), wol_bytes.begin() + long(first));
    std::vector<uint8_t> kept;
    int removed = 0;
    size_t i = size_t(first);
    size_t last_dep_end = size_t(first);
    while (i + 8 <= wol_bytes.size() &&
           std::memcmp(wol_bytes.data() + i, WOW_TAG, 4) == 0) {
        uint32_t k = get_u32(wol_bytes.data(), i + 4);
        if (exclude_keys.count(k)) ++removed;
        else kept.insert(kept.end(), wol_bytes.begin() + long(i),
                         wol_bytes.begin() + long(i + 8));
        i += 8;
        last_dep_end = i;
    }
    if (removed == 0) return {wol_bytes, 0};

    std::vector<uint8_t> out = std::move(prefix);
    out.insert(out.end(), kept.begin(), kept.end());
    out.insert(out.end(), wol_bytes.begin() + long(last_dep_end), wol_bytes.end());
    uint32_t new_size = get_u32(out.data(), 0) - uint32_t(removed) * 8;
    set_u32(out, 0, new_size);
    return {std::move(out), removed};
}

std::string base_resource_name(const std::string& name) {
    for (const char* tag : {"_wow_", "_wol_"}) {
        size_t p = name.find(tag);
        if (p != std::string::npos) return name.substr(0, p);
    }
    return name;
}

std::vector<const DepEntry*> TransplantPlan::present() const {
    std::vector<const DepEntry*> result;
    for (const DepEntry& dep : deps)
        if (dep.status == DepStatus::Present) result.push_back(&dep);
    return result;
}

std::vector<const DepEntry*> TransplantPlan::to_copy() const {
    std::vector<const DepEntry*> result;
    for (const DepEntry& dep : deps)
        if (dep.status == DepStatus::Copy) result.push_back(&dep);
    return result;
}

std::vector<const DepEntry*> TransplantPlan::unresolved() const {
    std::vector<const DepEntry*> result;
    for (const DepEntry& dep : deps)
        if (dep.status == DepStatus::Unresolved) result.push_back(&dep);
    return result;
}

uint64_t TransplantPlan::total_copy_bytes() const {
    uint64_t total = 0;
    for (const CopyItem& item : copy_items) total += item.length;
    return total;
}

TransplantPlan plan_transplant(const BigFile& donor_bf, const BigFile& target_bf,
                               const std::string& zone_name,
                               const std::unordered_set<uint32_t>& exclude_keys,
                               const TrueWowIndex* donor_index,
                               const TrueWowIndex* target_index) {
    TransplantPlan plan;
    plan.zone_name = zone_name;
    auto fail = [&](const std::string& m) { plan.error = m; return plan; };

    std::vector<ZoneInfo> zones = discover_zones(donor_bf);
    const ZoneInfo* z = nullptr;
    for (const ZoneInfo& zi : zones)
        if (zi.name == zone_name) { z = &zi; break; }
    if (z == nullptr)
        return fail("zone " + python_ascii_repr(zone_name) +
                    " not found in donor archive");

    // Precomputed indexes ({internal_key: file}, dict-ordered).
    TrueWowIndex donor_index_owned;
    TrueWowIndex target_index_owned;
    if (donor_index == nullptr) {
        donor_index_owned = build_true_wow_index(donor_bf);
        donor_index = &donor_index_owned;
    }
    if (target_index == nullptr) {
        target_index_owned = build_true_wow_index(target_bf);
        target_index = &target_index_owned;
    }
    std::unordered_map<uint32_t, uint32_t> donor_map(donor_index->begin(),
                                                     donor_index->end());
    std::unordered_map<uint32_t, uint32_t> target_map(target_index->begin(),
                                                      target_index->end());

    // Zone identity = the wow's first-record key.
    LzoResult wr = decompress_lzo(donor_bf.read_data(z->wow_index),
                                  /*max_output=*/256);
    std::vector<WowRecord> recs = wr.ok ? walk_wow(wr.data)
                                        : std::vector<WowRecord>{};
    if (recs.empty())
        return fail("zone " + python_ascii_repr(zone_name) +
                    " wow stream is unwalkable");
    plan.zone_internal_key = recs.front().key;
    plan.wow_index = z->wow_index;
    plan.wol_index = z->wol_index;

    LzoResult lr = decompress_lzo(donor_bf.read_data(z->wol_index));
    Wol wl = lr.ok ? parse_wol(lr.data) : Wol{};
    const std::vector<uint32_t>& dep_keys = wl.deps;

    // {lower base name: [target files]} in target_index order.
    std::vector<std::pair<std::string, std::vector<uint32_t>>> target_by_name;
    for (const auto& kv : *target_index) {
        const BFFile& fi = target_bf.files.at(kv.second);
        std::string bn = lower_ascii(base_resource_name(fi.name));
        bool found = false;
        for (auto& e : target_by_name)
            if (e.first == bn) { e.second.push_back(kv.second); found = true; break; }
        if (!found) target_by_name.push_back({bn, {kv.second}});
    }

    std::unordered_set<uint32_t> target_bf_keys;
    for (const auto& kv : target_bf.files)
        if (kv.second.key != INVALID_KEY) target_bf_keys.insert(kv.second.key);

    const BFFile& wolf = donor_bf.files.at(z->wol_index);
    plan.wol_bf_key = wolf.key;

    for (uint32_t d : dep_keys) {
        bool is_self = (d == plan.zone_internal_key);
        auto tit = target_map.find(d);
        if (tit != target_map.end()) {
            const BFFile& tf = target_bf.files.at(tit->second);
            DepEntry de;
            de.dep_key = d;
            de.status = DepStatus::Present;
            de.name = tf.name;
            de.has_file = true;
            de.file_index = tf.index;
            de.length = tf.length;
            de.has_target_bf_key = true;
            de.target_bf_key = internal_to_bf_key(d);
            de.is_self_ref = is_self;
            plan.deps.push_back(std::move(de));
            continue;
        }
        auto dit = donor_map.find(d);
        if (dit == donor_map.end()) {
            DepEntry de;
            de.dep_key = d;
            de.status = DepStatus::Unresolved;
            de.is_self_ref = is_self;
            plan.deps.push_back(std::move(de));
            continue;
        }
        const BFFile& df = donor_bf.files.at(dit->second);
        uint32_t tbf = internal_to_bf_key(d);
        if (exclude_keys.count(d) && !is_self) {
            DepEntry de;
            de.dep_key = d;
            de.status = DepStatus::Excluded;
            de.name = df.name;
            de.has_file = true;
            de.file_index = df.index;
            de.length = df.length;
            de.has_target_bf_key = true;
            de.target_bf_key = tbf;
            de.is_self_ref = is_self;
            plan.deps.push_back(std::move(de));
            continue;
        }
        std::string note;
        std::string bn = lower_ascii(base_resource_name(df.name));
        for (const auto& e : target_by_name)
            if (e.first == bn && !e.second.empty()) {
                const BFFile& sf = target_bf.files.at(e.second.front());
                char buf[128];
                std::snprintf(buf, sizeof buf,
                              "target already has a same-named resource "
                              "(0x%08x '%s') at a different key",
                              sf.key, sf.name.c_str());
                note = buf;
                break;
            }
        DepEntry de;
        de.dep_key = d;
        de.status = DepStatus::Copy;
        de.name = df.name;
        de.has_file = true;
        de.file_index = df.index;
        de.length = df.length;
        de.has_target_bf_key = true;
        de.target_bf_key = tbf;
        de.is_self_ref = is_self;
        de.note = std::move(note);
        plan.deps.push_back(std::move(de));
    }

    // copy_items = every COPY dep (collision/dup filtered) + the wol itself.
    std::unordered_set<uint32_t> seen_keys;
    for (const DepEntry& d : plan.deps) {
        if (d.status != DepStatus::Copy) continue;
        if (target_bf_keys.count(d.target_bf_key)) {
            plan.collisions.push_back({d.target_bf_key, d.name});
            continue;
        }
        if (seen_keys.count(d.target_bf_key)) continue;
        seen_keys.insert(d.target_bf_key);
        plan.copy_items.push_back({d.target_bf_key, d.name, d.file_index, d.length});
    }

    plan.wol_name = wolf.name;
    uint32_t excluded = 0;
    std::unordered_set<uint32_t> excluded_keys_set;
    for (const DepEntry& d : plan.deps)
        if (d.status == DepStatus::Excluded) {
            ++excluded;
            excluded_keys_set.insert(d.dep_key);
        }
    plan.excluded_count = excluded;
    if (target_bf_keys.count(wolf.key)) {
        plan.collisions.push_back({wolf.key, wolf.name});
    } else if (excluded > 0) {
        auto fw = filter_wol_deps(lr.data, excluded_keys_set);
        plan.wol_dec = std::move(fw.first);
        plan.has_wol_dec = true;
    } else if (!seen_keys.count(wolf.key)) {
        plan.copy_items.push_back({wolf.key, wolf.name, wolf.index, wolf.length});
    }

    plan.ok = true;
    return plan;
}

TransplantStats execute_transplant(const BigFile& donor_bf,
                                   const std::string& output_path,
                                   const TransplantPlan& plan,
                                   long long parent_dir_idx,
                                   const BigFileLogFn& log) {
    TransplantStats st;
    auto fail = [&](const std::string& m) { st.error = m; return st; };

    BigFile out_bf;
    try { out_bf.open(output_path); } catch (const std::exception& e) {
        return fail(e.what());
    }
    if (parent_dir_idx < 0) {
        // _bin_parent_dir: the dir holding any existing _wow_ entry.
        parent_dir_idx = out_bf.root;
        for (const auto& kv : out_bf.files) {
            const BFFile& f = kv.second;
            if (f.key != INVALID_KEY && f.name.find("_wow_") != std::string::npos &&
                out_bf.dirs.count(f.parent)) {
                parent_dir_idx = f.parent;
                break;
            }
        }
    }

    std::unordered_set<uint32_t> existing;
    for (const auto& kv : out_bf.files)
        if (kv.second.key != INVALID_KEY) existing.insert(kv.second.key);

    for (const CopyItem& ci : plan.copy_items) st.added_keys.push_back(ci.bf_key);

    std::fstream f(output_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return fail("cannot open output archive for writing");
    try {
        for (const CopyItem& ci : plan.copy_items) {
            if (existing.count(ci.bf_key)) {
                ++st.skipped;
                if (log) {
                    char prefix[32];
                    std::snprintf(prefix, sizeof prefix, "  skip 0x%08x ",
                                  ci.bf_key);
                    log(std::string(prefix) + python_ascii_repr(ci.name) +
                        ": already in target");
                }
                continue;
            }
            std::vector<uint8_t> compressed = donor_bf.read_data(ci.donor_index);
            out_bf.add_entry(f, ci.name, ci.bf_key, uint32_t(parent_dir_idx),
                             compressed, log);
            existing.insert(ci.bf_key);
            ++st.added;
            st.added_bytes += compressed.size();
        }
        if (plan.has_wol_dec && !existing.count(plan.wol_bf_key)) {
            std::vector<uint8_t> comp = compress_lzo(plan.wol_dec, 9);
            out_bf.add_entry(f, plan.wol_name, plan.wol_bf_key,
                             uint32_t(parent_dir_idx), comp, log);
            existing.insert(plan.wol_bf_key);
            st.added_keys.push_back(plan.wol_bf_key);
            ++st.added;
            st.added_bytes += comp.size();
            if (log)
                log("  wol rebuilt: dropped " +
                    std::to_string(plan.excluded_count) + " excluded dep(s)");
        }
        st.size_grs_rows = out_bf.flush_size_grs(f, log);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    st.collisions = plan.collisions;
    st.excluded = plan.excluded_count;
    st.ok = true;
    if (log) {
        log("transplant: +" + std::to_string(st.added) + " entries (" +
            comma_integer(st.added_bytes) + " B compressed), " +
            std::to_string(st.skipped) +
            " already present, size.grs rows written=" +
            std::to_string(st.size_grs_rows));
    }
    return st;
}

}  // namespace zonetx
}  // namespace jade
