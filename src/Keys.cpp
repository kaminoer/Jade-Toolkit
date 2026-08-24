// Keys.cpp — implementation. Faithful port of core/keys.py (zone-analysis subset).
#include "jade/Keys.hpp"

#include <exception>
#include <unordered_map>

#include "jade/Compression.hpp"
#include "jade/WowStream.hpp"

namespace jade {

namespace {
inline uint32_t le32(const uint8_t* p, size_t o) {
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
           (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
}
}  // namespace

std::vector<const BFFile*> find_bf_files_for_internal(
    const BigFile& bf, uint32_t internal_key) {
    std::vector<const BFFile*> matches;
    const uint32_t low = internal_key & 0xFFFFu;
    for (const auto& [index, file] : bf.files) {
        (void)index;
        if (file.key != INVALID_KEY && (file.key & 0xFFFFu) == low)
            matches.push_back(&file);
    }
    return matches;
}

long long derive_map_prefix(const uint8_t* dec, size_t n) {
    long long first = find_first_record(dec, n);
    if (first < 0) return -1;
    uint32_t key = le32(dec, static_cast<size_t>(first) + 8);
    return static_cast<long long>(map_prefix_of(key));
}

std::unordered_set<uint32_t> build_wow_resource_index(
    const BigFile& bf, const std::function<void(int, int)>& progress) {
    std::vector<const BFFile*> wows;
    for (const auto& kv : bf.files) {
        const BFFile& f = kv.second;
        if (f.key == INVALID_KEY || f.name.find("_wow_") == std::string::npos) continue;
        wows.push_back(&f);
    }
    std::unordered_set<uint32_t> index;
    int done = 0;
    for (const BFFile* f : wows) {
        LzoResult r = decompress_lzo(bf.read_data(f->index), /*max_output=*/64);
        if (progress) progress(++done, static_cast<int>(wows.size()));
        if (!r.ok) continue;
        long long prefix = derive_map_prefix(r.data.data(), r.data.size());
        if (prefix >= 0)
            index.insert((static_cast<uint32_t>(prefix) << 24) | (f->key & 0xFFFF));
    }
    return index;
}

std::map<uint32_t, uint32_t> build_wow_resource_index_map(
    const BigFile& bf, const std::function<void(int, int)>& progress) {
    std::vector<const BFFile*> wows;
    for (const auto& kv : bf.files) {
        const BFFile& f = kv.second;
        if (f.key == INVALID_KEY || f.name.find("_wow_") == std::string::npos) continue;
        wows.push_back(&f);
    }
    std::map<uint32_t, uint32_t> index;
    int done = 0;
    for (const BFFile* f : wows) {
        LzoResult r = decompress_lzo(bf.read_data(f->index), /*max_output=*/64);
        if (progress) progress(++done, static_cast<int>(wows.size()));
        if (!r.ok) continue;
        long long prefix = derive_map_prefix(r.data.data(), r.data.size());
        if (prefix >= 0)
            index[(static_cast<uint32_t>(prefix) << 24) | (f->key & 0xFFFF)] =
                f->index;
    }
    return index;
}

std::vector<std::pair<uint32_t, uint32_t>> build_true_wow_index(
    const BigFile& bf, const std::function<void(int, int)>& progress,
    size_t max_output) {
    // Python-dict semantics: first-insert position kept, value overwritten.
    std::vector<std::pair<uint32_t, uint32_t>> index;
    std::unordered_map<uint32_t, size_t> pos;
    std::vector<const BFFile*> wows;
    for (const auto& kv : bf.files) {
        const BFFile& f = kv.second;
        if (f.key == INVALID_KEY || f.name.find("_wow_") == std::string::npos) continue;
        wows.push_back(&f);
    }
    int done = 0;
    for (const BFFile* f : wows) {
        try {
            LzoResult r = decompress_lzo(bf.read_data(f->index), max_output);
            if (r.ok) {
                std::vector<WowRecord> recs = walk_wow(r.data);
                if (!recs.empty()) {
                    uint32_t k = recs.front().key;
                    auto it = pos.find(k);
                    if (it != pos.end()) index[it->second].second = f->index;
                    else {
                        pos[k] = index.size();
                        index.push_back({k, f->index});
                    }
                }
            }
        } catch (const std::exception&) {
            // Python deliberately skips unreadable/decompression-failing
            // entries and still reports progress for them.
        }
        if (progress) progress(++done, static_cast<int>(wows.size()));
    }
    return index;
}

}  // namespace jade
