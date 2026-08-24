// bf_assetidx_digest — build the BigFile asset index (core/asset_index) and
// emit a sorted canonical digest of the catalog rows.
//
//   bf_assetidx_digest <file.bf> [out] [limit] [names]
//
// Builds the index over up to `limit` active entries (0 = all), then emits:
//   * a sorted category histogram (CAT <code> <count>)
//   * one row per record, sorted by (parent_index, sub_index):
//       R <parent_index> <sub_index> <key> cat=<code> gro=<hex8> ext=<e|->
//         psz=<payload_size> refs=<crc|->
// Cosmetic name/detail are NOT part of the default digest (the ANIMATION
// name still needs the un-ported Animation reader); the digest validates the
// catalog STRUCTURE — every classification + every dependency ref — which is
// what the read-path port reproduces byte-for-byte vs the Python oracle.
//
// With a 4th arg "names", additionally emits one resolved display-name row
// per record ("N <parent> <sub> <name>") after the R rows — used to spot-
// check the referrer-name resolution against the Python app (not part of
// the stored golden format).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "jade/AssetIndex.hpp"
#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Crc32.hpp"
#include "jade/Utf8Args.hpp"

// Stable short code for a category string (mirrored in gen_golden.dump_assetidx).
static std::string cat_code(const std::string& c) {
    using namespace jade;
    if (c == CAT_TEXTURE)      return "TEX";
    if (c == CAT_GEOMETRY)     return "GEO";
    if (c == CAT_MATERIAL)     return "MAT";
    if (c == CAT_GAO)          return "GAO";
    if (c == CAT_LIGHT)        return "LIGHT";
    if (c == CAT_ANIMATION)    return "ANIM";
    if (c == CAT_SOUND)        return "SND";
    if (c == CAT_AI)           return "AI";
    if (c == CAT_PALETTE)      return "PAL";
    if (c == CAT_TEXTURE_DATA) return "TEXDATA";
    if (c == CAT_DATATABLE)    return "DTAB";
    if (c == CAT_REFERENCE)    return "REF";
    if (c == CAT_CAMERA)       return "CAM";
    if (c == CAT_FX)           return "FX";
    if (c == CAT_TRIGGER)      return "TRIG";
    if (c == CAT_TRAP)         return "TRAP";
    if (c == CAT_ACTOR)        return "ACT";
    if (c == CAT_SPAWNER)      return "SPN";
    if (c == CAT_WAYPOINT)     return "WAY";
    if (c == CAT_LOGIC)        return "LOGIC";
    return "OTHER";
}

// CRC of a record's refs: for each ref-type (alpha-sorted), append the type's
// ASCII bytes + a NUL + the keys packed as little-endian u32. "-" when empty.
static std::string refs_crc(const jade::AssetRecord& r) {
    if (r.refs.empty()) return "-";
    bool any = false;
    std::vector<uint8_t> buf;
    for (const auto& kv : r.refs) {  // std::map -> alpha-sorted ref-type
        if (kv.second.empty()) continue;
        any = true;
        for (char ch : kv.first) buf.push_back(static_cast<uint8_t>(ch));
        buf.push_back(0);
        for (uint32_t k : kv.second) {
            buf.push_back(k & 0xFF);
            buf.push_back((k >> 8) & 0xFF);
            buf.push_back((k >> 16) & 0xFF);
            buf.push_back((k >> 24) & 0xFF);
        }
    }
    if (!any) return "-";
    return jade::hex8(jade::crc32(buf.data(), buf.size()));
}

static void emit(std::ostream& os, const jade::BigFile& bf, size_t limit,
                 bool names = false, bool details = false, bool trace = false,
                 size_t cancel_after = 0) {
    const char NL = '\n';

    // Active filter mirrors asset_index.iter_parent_subs: name + valid key + len>0.
    std::vector<uint32_t> active;
    for (const auto& kv : bf.files) {
        const jade::BFFile& e = kv.second;
        if (!e.name.empty() && e.key != jade::INVALID_KEY && e.length > 0)
            active.push_back(e.index);
    }
    size_t emit_n = (limit == 0) ? active.size() : std::min(limit, active.size());

    std::vector<std::pair<int, int>> progress;
    size_t cancel_polls = 0;
    std::function<void(int, int)> progress_fn;
    std::function<bool()> cancel_fn;
    if (trace) {
        progress_fn = [&](int done, int total) {
            progress.push_back({done, total});
        };
    }
    if (cancel_after != 0) {
        cancel_fn = [&] {
            ++cancel_polls;
            return cancel_polls >= cancel_after;
        };
    }
    jade::AssetIndex idx = jade::build_asset_index(
        bf, limit, progress_fn, cancel_fn);

    // Category histogram (std::map -> category-string sorted; we emit by code).
    std::map<std::string, size_t> hist = idx.count_by_category();

    // Rows sorted by (parent_index, sub_index).
    std::vector<const jade::AssetRecord*> rows;
    rows.reserve(idx.records.size());
    for (const auto& r : idx.records) rows.push_back(&r);
    std::sort(rows.begin(), rows.end(),
              [](const jade::AssetRecord* a, const jade::AssetRecord* b) {
                  if (a->parent_index != b->parent_index)
                      return a->parent_index < b->parent_index;
                  return a->sub_index < b->sub_index;
              });

    if (trace) {
        os << "CALLBACKS v1" << NL;
        for (const auto& item : progress)
            os << "P " << item.first << ' ' << item.second << NL;
        os << "cancel_polls " << cancel_polls << NL;
    }
    os << "ASSETIDX v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;
    os << "records " << idx.records.size() << NL;
    os << "categories " << hist.size() << NL;
    for (const auto& kv : hist)
        os << "CAT " << cat_code(kv.first) << ' ' << kv.second << NL;
    for (const jade::AssetRecord* r : rows) {
        std::ostringstream ln;
        ln << "R " << r->parent_index << ' ' << r->sub_index << ' '
           << jade::hex8(r->key) << " cat=" << cat_code(r->category)
           << " gro=" << jade::hex8(r->gro_type)
           << " ext=" << (r->ext.empty() ? std::string("-") : r->ext)
           << " psz=" << r->payload_size
           << " refs=" << refs_crc(*r);
        os << ln.str() << NL;
    }
    if (names)
        for (const jade::AssetRecord* r : rows)
            os << "N " << r->parent_index << ' ' << r->sub_index << ' '
               << r->name << NL;
    if (details)
        for (const jade::AssetRecord* r : rows)
            os << "D " << r->parent_index << ' ' << r->sub_index << ' '
               << r->detail << NL;
}

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc < 2) {
        std::cerr << "usage: bf_assetidx_digest <file.bf> [out] [limit] "
                     "[names] [details] [trace] [cancel=N]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));
    bool names = false, details = false, trace = false;
    size_t cancel_after = 0;
    for (int i = 4; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "names") names = true;
        else if (option == "details") details = true;
        else if (option == "trace") trace = true;
        else if (option.rfind("cancel=", 0) == 0) {
            cancel_after = static_cast<size_t>(
                std::strtoull(option.c_str() + 7, nullptr, 10));
            trace = true;
        }
    }
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(std::filesystem::u8path(argv[2]), std::ios::binary);
            if (!o) { std::cerr << "bf_assetidx_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit, names, details, trace, cancel_after);
        } else {
            emit(std::cout, bf, limit, names, details, trace, cancel_after);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_assetidx_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
