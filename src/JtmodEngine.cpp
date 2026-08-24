// JtmodEngine.cpp — implementation. Port of jade_explorer/build/jtmod_apply.py.
#include "jade/JtmodEngine.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/GameProfiles.hpp"
#include "jade/Sha256.hpp"

namespace jade {
namespace jtmod {

namespace {

// WOLInfo.bin — the engine's precomputed WOL->dependency table. Like size.grs it
// is a container SYSTEM TABLE: the engine loads it RAW (not through the LZO
// streaming path), so it must be stored uncompressed and kept out of size.grs.
// A mod that registers new zones (e.g. the Xbox DLC) rewrites it, so the apply
// must write it the same way the cooker / toolkit does (write_raw_entry), not
// compress it like an ordinary resource bin. (WW/T2T key; same across both.)
constexpr uint32_t kWolTableKey = 0xFC000001u;

std::string game_name(const std::string& code) {
    const gameprofiles::GameProfile* p = gameprofiles::get(code);
    return p ? p->name : (code.empty() ? "an unknown game" : code);
}

std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

}  // namespace

std::string Conflict::str() const {
    std::string where = has_bin ? (" bin " + hex8(bin_key)) : "";
    std::string mods;
    for (size_t i = 0; i < mod_ids.size(); ++i) {
        if (i) mods += ", ";
        mods += mod_ids[i];
    }
    return "[" + kind + where + "] " + detail + " — mods: " + mods;
}

std::vector<std::string> precheck(const std::vector<Mod>& mods,
                                  const std::string& base_path) {
    namespace fs = std::filesystem;
    std::vector<std::string> problems;
    std::error_code ec;
    if (!fs::is_regular_file(base_path, ec)) {
        problems.push_back("base archive not found: " + base_path);
        return problems;
    }
    uint64_t size = static_cast<uint64_t>(fs::file_size(base_path, ec));
    const gameprofiles::GameProfile* target = gameprofiles::detect(base_path);

    std::set<std::string> games;
    for (const Mod& m : mods)
        if (!m.data.game.empty()) games.insert(m.data.game);
    if (games.size() > 1) {
        std::string names;
        for (const std::string& g : games) {
            if (!names.empty()) names += ", ";
            names += game_name(g);
        }
        problems.push_back("these mods are for different games (" + names +
                           ") and cannot be applied together");
    }

    std::string digest;
    bool digest_done = false;
    for (const Mod& m : mods) {
        const std::string& mg = m.data.game;
        if (!mg.empty() && target && mg != target->code) {
            problems.push_back(m.id + ": this mod is for " + game_name(mg) +
                               ", but the selected archive is " + target->name +
                               " — refusing to cross-patch");
            continue;
        }
        // v3+ mods are portable: they carry per-bin base hashes verified at apply
        // time (apply onto a different version / externally-modded BF), so we skip
        // the whole-archive size/sha pin. (v1/v2 mods still require an exact base.)
        if (m.data.format_version >= 3) continue;
        if (m.data.base_size && m.data.base_size != size) {
            problems.push_back(m.id + ": built against a " +
                               std::to_string(m.data.base_size) + "-byte " +
                               game_name(mg) + " archive; this file is " +
                               std::to_string(size) + " bytes");
            continue;
        }
        if (!m.data.base_sha256.empty()) {
            if (!digest_done) { digest = sha256_file_hex(base_path); digest_done = true; }
            if (m.data.base_sha256 != digest)
                problems.push_back(m.id + ": does not match the stock " +
                                   game_name(mg) + " archive (sha256 differs) — "
                                   "wrong version, or already modified");
        }
    }
    return problems;
}

std::vector<Conflict> detect_conflicts(const std::vector<Mod>& mods) {
    std::vector<Conflict> out;

    // (Minted-key 0x7A collisions are NOT a conflict — resolve_collisions()
    // renumbers them. Only contention over EXISTING resources is reported.)

    // Per-bin contention.
    std::map<uint32_t, std::vector<std::pair<std::string, const BinDelta*>>> by_bin;
    for (const Mod& m : mods)
        for (const BinDelta& b : m.data.bins)
            by_bin[b.bin_key].push_back({m.id, &b});

    for (const auto& kv : by_bin) {
        const auto& lst = kv.second;
        if (lst.size() < 2) continue;
        std::vector<std::string> ids;
        bool any_whole = false, all_new = true;
        for (const auto& p : lst) {
            ids.push_back(p.first);
            if (p.second->mode == MODE_WHOLEBIN || p.second->mode == MODE_NEWBIN)
                any_whole = true;
            if (p.second->mode != MODE_NEWBIN) all_new = false;
        }
        if (any_whole) {
            Conflict c;
            c.kind = all_new ? "new_bin" : "whole_bin";
            c.mod_ids = ids;
            c.has_bin = true;
            c.bin_key = kv.first;
            c.detail = "multiple mods replace this bin wholesale (cannot be merged)";
            out.push_back(std::move(c));
            continue;
        }
        for (size_t i = 0; i < lst.size(); ++i) {
            for (size_t j = i + 1; j < lst.size(); ++j) {
                bool ov = false;
                for (const Hunk& ha : lst[i].second->hunks) {
                    for (const Hunk& hb : lst[j].second->hunks)
                        if (hunks_overlap(ha, hb)) { ov = true; break; }
                    if (ov) break;
                }
                if (ov) {
                    Conflict c;
                    c.kind = "overlap";
                    c.mod_ids = {lst[i].first, lst[j].first};
                    c.has_bin = true;
                    c.bin_key = kv.first;
                    c.detail = "both mods change the same part of this bin";
                    out.push_back(std::move(c));
                }
            }
        }
    }
    return out;
}

namespace {

struct NewEntry { std::string name; uint32_t parent_dir_idx; std::vector<uint8_t> data; };

void merge_bins(const std::vector<Mod>& mods, const BigFile& base,
                std::map<uint32_t, std::vector<uint8_t>>& modified,
                std::map<uint32_t, NewEntry>& new_entries) {
    std::map<uint32_t, std::vector<const BinDelta*>> by_bin;
    for (const Mod& m : mods)
        for (const BinDelta& b : m.data.bins) by_bin[b.bin_key].push_back(&b);

    std::map<uint32_t, uint32_t> key2idx;
    for (const auto& kv : base.files)
        if (kv.second.key != INVALID_KEY) key2idx[kv.second.key] = kv.second.index;

    for (const auto& kv : by_bin) {
        uint32_t bin_key = kv.first;
        const auto& lst = kv.second;
        const BinDelta* nb = nullptr;
        const BinDelta* wb = nullptr;
        for (const BinDelta* b : lst) {
            if (b->mode == MODE_NEWBIN) nb = b;
            else if (b->mode == MODE_WHOLEBIN) wb = b;
        }
        if (nb) { new_entries[bin_key] = {nb->name, nb->parent_dir_idx, nb->payload}; continue; }
        if (wb) { modified[bin_key] = wb->payload; continue; }

        auto it = key2idx.find(bin_key);
        if (it == key2idx.end()) continue;  // shouldn't happen
        LzoResult dec = decompress_lzo(base.read_data(it->second));
        SplitResult s = split_records(dec.data);
        std::vector<Hunk> all_hunks;
        for (const BinDelta* b : lst)
            for (const Hunk& h : b->hunks) all_hunks.push_back(h);
        std::vector<Record> merged = apply_hunks(s.records, all_hunks);
        std::vector<uint8_t> outbin = s.prefix;
        for (const Record& r : merged) outbin.insert(outbin.end(), r.begin(), r.end());
        modified[bin_key] = std::move(outbin);
    }
}

// Per-bin base check (v3 portable mods): each sub-entry delta carries the sha256
// of the bin it was authored against. Verify that bin matches in the actual base
// archive, so positional hunks are only applied where they're valid (enables
// apply onto a different version / externally-modded BF; refuses where the target
// bin differs instead of misapplying). Returns human-readable incompatibilities.
std::vector<std::string> check_base_compat(BigFile& base, const std::vector<Mod>& mods) {
    std::map<uint32_t, uint32_t> key2idx;
    for (const auto& kv : base.files)
        if (kv.second.key != INVALID_KEY) key2idx[kv.second.key] = kv.second.index;
    std::map<uint32_t, std::string> hash_cache;
    std::vector<std::string> issues;
    for (const Mod& m : mods) {
        for (const BinDelta& b : m.data.bins) {
            if (b.mode != MODE_SUBENTRY || b.base_hash.size() != 32) continue;  // only v3 subentry
            auto it = key2idx.find(b.bin_key);
            if (it == key2idx.end()) {
                issues.push_back(m.id + ": the file it changes (" + hex8(b.bin_key) +
                                 ") isn't in this archive");
                continue;
            }
            auto ci = hash_cache.find(b.bin_key);
            std::string have;
            if (ci != hash_cache.end()) have = ci->second;
            else {
                LzoResult d = decompress_lzo(base.read_data(it->second));
                have = sha256_hex(d.data);
                hash_cache[b.bin_key] = have;
            }
            if (have != hex32(b.base_hash.data()))
                issues.push_back(m.id + ": was made for a different version of " +
                                 hex8(b.bin_key) + " (your game files differ here)");
        }
    }
    return issues;
}

}  // namespace

namespace {

// ── minted-key relocation (resolve cross-mod 0x7A collisions) ──
uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
    return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) |
           (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
}
void wr32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = uint8_t(v); b[o + 1] = uint8_t(v >> 8);
    b[o + 2] = uint8_t(v >> 16); b[o + 3] = uint8_t(v >> 24);
}

// Rewrite one sub-entry record: its identity (key @8) + type (@12) fields, plus
// any cross-reference to oldK in the payload. A texture carries its key in BOTH
// the key and type fields and holds no further references (its payload is pixel
// data), so when key==type==oldK we skip the payload scan — that avoids a stray
// pixel word that happens to equal the key.
void relocate_record(Record& r, uint32_t oldK, uint32_t newK) {
    if (r.size() < 12) return;
    uint32_t key  = rd32(r, 8);
    bool has_type = r.size() >= 16;
    uint32_t type = has_type ? rd32(r, 12) : 0;
    if (key == oldK) wr32(r, 8, newK);
    if (has_type && type == oldK) wr32(r, 12, newK);
    bool texture = (key == oldK && has_type && type == oldK);
    if (!texture)
        for (size_t o = 16; o + 4 <= r.size(); o += 4)
            if (rd32(r, o) == oldK) wr32(r, o, newK);
}

// Rewrite every key field in a whole-bin / new-bin payload: split into records,
// relocate each, reassemble. If the payload isn't a clean record concatenation,
// fall back to a 4-byte-aligned scan of the blob for the exact key value.
void relocate_payload(std::vector<uint8_t>& p, uint32_t oldK, uint32_t newK) {
    SplitResult s = split_records(p);
    if (!s.clean) {
        for (size_t o = 0; o + 4 <= p.size(); o += 4)
            if (rd32(p, o) == oldK) wr32(p, o, newK);
        return;
    }
    for (size_t o = 0; o + 4 <= s.prefix.size(); o += 4)
        if (rd32(s.prefix, o) == oldK) wr32(s.prefix, o, newK);
    for (Record& r : s.records) relocate_record(r, oldK, newK);
    std::vector<uint8_t> out = s.prefix;
    for (const Record& r : s.records) out.insert(out.end(), r.begin(), r.end());
    p = std::move(out);
}

// Renumber one minted key throughout a mod: its minted list, any new-bin FAT key,
// and every record / payload that names it.
void relocate_in_mod(Mod& m, uint32_t oldK, uint32_t newK) {
    for (uint32_t& k : m.data.minted_keys) if (k == oldK) k = newK;
    for (BinDelta& b : m.data.bins) {
        if (b.bin_key == oldK) b.bin_key = newK;
        if (b.mode == MODE_SUBENTRY) {
            for (Hunk& h : b.hunks)
                for (Record& r : h.records) relocate_record(r, oldK, newK);
        } else {
            relocate_payload(b.payload, oldK, newK);
        }
    }
}

}  // namespace

int resolve_collisions(std::vector<Mod>& mods) {
    // Every key currently minted by any mod — never hand one out as a fresh target.
    std::set<uint32_t> used;
    for (const Mod& m : mods)
        for (uint32_t k : m.data.minted_keys) used.insert(k);

    uint32_t next = 0x7A000001u;
    auto fresh = [&]() -> uint32_t {
        while (used.count(next)) ++next;   // 16M-wide 0x7A space; never exhausted
        uint32_t k = next++;
        used.insert(k);
        return k;
    };

    std::set<uint32_t> claimed;   // keys an earlier (higher-priority) mod owns
    int moved = 0;
    for (Mod& m : mods) {
        std::vector<uint32_t> keys = m.data.minted_keys;   // copy: relocate mutates
        for (uint32_t k : keys) {
            if (claimed.count(k)) {                        // collision -> renumber
                uint32_t nk = fresh();
                relocate_in_mod(m, k, nk);
                claimed.insert(nk);
                ++moved;
            } else {
                claimed.insert(k);
            }
        }
    }
    return moved;
}

ApplyResult apply(const std::string& base_path, const std::vector<Mod>& mods,
                  const std::string& out_path, bool allow_conflicts,
                  bool allow_base_mismatch, ProgressFn progress) {
    namespace fs = std::filesystem;
    ApplyResult res;
    auto report = [&](int pct, const char* phase) { if (progress) progress(pct, phase); };

    report(2, "Checking base archive");
    std::vector<std::string> problems = precheck(mods, base_path);
    if (!problems.empty()) {
        res.issues = problems;
        res.report = "cannot apply to this archive";
        return res;
    }

    // Auto-resolve minted-key collisions (renumber later mods' new-asset keys),
    // then merge the relocated set. Relocation can't introduce / remove overlap
    // or whole-bin contention, so the remaining conflicts are the real ones.
    std::vector<Mod> work = mods;
    res.keys_relocated = resolve_collisions(work);
    res.conflicts = detect_conflicts(work);
    if (!res.conflicts.empty() && !allow_conflicts) {
        for (const Conflict& c : res.conflicts) res.issues.push_back(c.str());
        res.report = "conflicts detected — resolve and re-apply";
        return res;
    }

    report(8, "Merging mods");
    BigFile base;
    base.open(base_path);

    // Portable (v3) mods: verify each touched bin matches the version it was made
    // for. Mismatch -> refuse cleanly (before any write) rather than misapply,
    // unless force mode is on (then overwrite that bin anyway).
    std::vector<std::string> incompat = check_base_compat(base, work);
    if (!incompat.empty()) {
        if (!allow_base_mismatch) {
            res.issues = incompat;
            res.report = "this mod doesn't match your game files — not applied";
            return res;
        }
        res.forced_bins = static_cast<int>(incompat.size());
    }

    std::map<uint32_t, std::vector<uint8_t>> modified;
    std::map<uint32_t, NewEntry> new_entries;
    merge_bins(work, base, modified, new_entries);

    std::error_code ec;
    // Copy the pristine base to the output in chunks (so the UI can show
    // progress; this is the dominant cost for a ~500 MB archive). Remove the
    // destination first — MinGW's copy_file(overwrite_existing) can report
    // "File exists" on some builds, and the live archive is normally present.
    if (fs::exists(out_path, ec)) fs::remove(out_path, ec);
    {
        std::ifstream in(base_path, std::ios::binary);
        std::ofstream of(out_path, std::ios::binary | std::ios::trunc);
        if (!in || !of) { res.issues.push_back("could not open files to copy the base archive"); return res; }
        uint64_t total = static_cast<uint64_t>(fs::file_size(base_path, ec));
        std::vector<char> buf(1u << 20);   // 1 MB chunks
        uint64_t done = 0; int last = -1;
        while (true) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            of.write(buf.data(), n);
            if (!of) { res.issues.push_back("write error copying the base archive"); return res; }
            done += static_cast<uint64_t>(n);
            int pct = total ? int(10 + (50 * done) / total) : 10;
            if (pct != last) { report(pct, "Copying base archive"); last = pct; }
        }
    }

    BigFile out;
    out.open(out_path);
    std::map<uint32_t, uint32_t> key2idx;
    for (const auto& kv : out.files)
        if (kv.second.key != INVALID_KEY) key2idx[kv.second.key] = kv.second.index;

    {
        std::fstream f(out_path, std::ios::in | std::ios::out | std::ios::binary);
        if (!f) { res.issues.push_back("cannot open output for writing"); return res; }
        size_t total_w = modified.size() + new_entries.size(), done_w = 0;
        for (const auto& kv : modified) {  // std::map -> ascending key order
            auto it = key2idx.find(kv.first);
            if (it == key2idx.end()) {
                res.issues.push_back("entry " + hex8(kv.first) + " vanished from output");
                continue;
            }
            if (kv.first == kWolTableKey) {
                // System table loaded raw by the engine: store the bytes verbatim,
                // uncompressed and out of size.grs.
                out.write_entry(f, kv.second, out.files.at(it->second), /*skip_size_grs=*/true);
            } else {
                std::vector<uint8_t> comp = compress_lzo(kv.second);
                out.write_entry(f, comp, out.files.at(it->second));
            }
            ++res.bins_modified;
            report(total_w ? int(60 + (28 * ++done_w) / total_w) : 88, "Writing changes");
        }
        for (const auto& kv : new_entries) {
            std::vector<uint8_t> comp = compress_lzo(kv.second.data);
            try {
                out.add_entry(f, kv.second.name, kv.first, kv.second.parent_dir_idx, comp);
                ++res.bins_added;
            } catch (const std::exception& e) {
                res.issues.push_back(std::string("add_entry failed: ") + e.what());
            }
            report(total_w ? int(60 + (28 * ++done_w) / total_w) : 88, "Writing changes");
        }
        out.flush_size_grs(f);
        f.flush();
    }

    report(90, "Verifying");

    // Self-verify: every touched/new bin must decompress to its intended bytes.
    {
        BigFile chk;
        chk.open(out_path);
        std::map<uint32_t, uint32_t> k2i;
        for (const auto& kv : chk.files)
            if (kv.second.key != INVALID_KEY) k2i[kv.second.key] = kv.second.index;
        auto verify = [&](uint32_t key, const std::vector<uint8_t>& want) -> bool {
            auto it = k2i.find(key);
            if (it == k2i.end()) return false;
            if (key == kWolTableKey)              // stored raw — compare verbatim
                return chk.read_data(it->second) == want;
            LzoResult d = decompress_lzo(chk.read_data(it->second));
            return d.ok && d.data == want;
        };
        for (const auto& kv : modified)
            if (!verify(kv.first, kv.second))
                res.issues.push_back("verify failed for modified bin " + hex8(kv.first));
        for (const auto& kv : new_entries)
            if (!verify(kv.first, kv.second.data))
                res.issues.push_back("verify failed for new bin " + hex8(kv.first));
    }

    report(100, "Done");
    res.ok = res.issues.empty();
    res.report = "applied " + std::to_string(work.size()) + " mod(s): " +
                 std::to_string(res.bins_modified) + " bin(s) modified, " +
                 std::to_string(res.bins_added) + " added";
    if (res.keys_relocated)
        res.report += " (relocated " + std::to_string(res.keys_relocated) +
                      " colliding key(s))";
    if (res.forced_bins)
        res.report += " (forced over " + std::to_string(res.forced_bins) +
                      " file(s) modded elsewhere — other changes there may be lost)";
    return res;
}

size_t max_modified_bin_size(const std::string& base_path, const std::vector<Mod>& mods) {
    BigFile base;
    try { base.open(base_path); } catch (...) { return 0; }
    std::map<uint32_t, std::vector<uint8_t>> modified;
    std::map<uint32_t, NewEntry> new_entries;
    merge_bins(mods, base, modified, new_entries);   // relocation doesn't affect sizes
    size_t mx = 0;
    for (const auto& kv : modified)    mx = std::max(mx, kv.second.size());
    for (const auto& kv : new_entries) mx = std::max(mx, kv.second.data.size());
    return mx;
}

Mod load_mod(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    Mod m;
    m.data = parse(blob);
    m.id = std::filesystem::path(path).stem().string();
    return m;
}

}  // namespace jtmod
}  // namespace jade
