// CanonDump.hpp — canonical textual dump of a parsed BigFile.
//
// This is the golden-diff contract: bf_dump (C++) and tests/gen_golden.py
// (Python oracle) must emit byte-identical output for the same archive. The
// format is line-based ('\n' only — write the stream in binary), pure ASCII
// (names are escaped), and fully order-deterministic (entries sorted by index;
// std::map iterates ascending, the Python side sorts to match).
#pragma once

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"

namespace jade {

// 8-digit lowercase hex, matching Python's "{:08x}".
inline std::string hex8(uint32_t v) {
    static const char* h = "0123456789abcdef";
    char b[9];
    for (int i = 7; i >= 0; --i) { b[i] = h[v & 0xf]; v >>= 4; }
    b[8] = 0;
    return std::string(b, 8);
}

// Escape a name to pure ASCII: printable ASCII (0x20..0x7e) passes through
// except backslash; everything else (control bytes, the U+FFFD bytes from
// non-ASCII heap-fill, DEL) becomes \xNN. Deterministic and identical on both
// sides, so names never break the line-oriented diff.
inline void canon_name(std::ostream& os, const std::string& s) {
    static const char* h = "0123456789abcdef";
    for (unsigned char b : s) {
        if (b >= 0x20 && b <= 0x7e && b != '\\') {
            os.put(static_cast<char>(b));
        } else {
            os.put('\\'); os.put('x'); os.put(h[b >> 4]); os.put(h[b & 0xf]);
        }
    }
}

inline void dump_canonical(std::ostream& os, const BigFile& bf) {
    const char NL = '\n';
    os << "BF-DUMP v1" << NL;
    os << "version "         << bf.version          << NL;
    os << "max_file "        << bf.max_file         << NL;
    os << "max_dir "         << bf.max_dir          << NL;
    os << "max_key "         << bf.max_key          << NL;
    os << "root "            << bf.root             << NL;
    os << "first_free_file " << bf.first_free_file  << NL;
    os << "first_free_dir "  << bf.first_free_dir   << NL;
    os << "size_of_fat "     << bf.size_of_fat      << NL;
    os << "num_fat "         << bf.num_fat          << NL;
    os << "universe_key "    << bf.universe_key     << NL;
    os << "ext_size "        << bf.ext_size         << NL;
    os << "active_files "    << bf.active_file_count() << NL;
    os << "active_dirs "     << bf.active_dir_count()  << NL;

    os << "FATDESC " << bf.fat_list.size() << NL;
    for (const FatDesc& fd : bf.fat_list) {
        os << "fd " << fd.max_file << ' ' << fd.max_dir << ' ' << fd.pos_fat
           << ' ' << fd.next_pos_fat << ' ' << fd.first_index << ' '
           << fd.last_index << NL;
    }

    os << "FILES " << bf.files.size() << NL;  // std::map -> ascending index
    for (const auto& kv : bf.files) {
        const BFFile& e = kv.second;
        os << "F " << e.index << ' ' << e.pos << ' ' << hex8(e.key) << ' '
           << e.length << ' ' << e.prev << ' ' << e.next << ' ' << e.parent
           << ' ' << e.time << ' ';
        canon_name(os, e.name);
        os << NL;
    }

    os << "DIRS " << bf.dirs.size() << NL;
    for (const auto& kv : bf.dirs) {
        const BFDir& d = kv.second;
        os << "D " << d.index << ' ' << d.first_file << ' ' << d.first_subdir
           << ' ' << d.prev << ' ' << d.next << ' ' << d.parent << ' ';
        canon_name(os, d.name);
        os << NL;
    }

    // PATHS: every active file's full path (exercises parent linkage +
    // dir_path), sorted by (path, index) for determinism.
    struct PathRow { std::string path; uint32_t index, key, length; };
    std::vector<PathRow> rows;
    for (const auto& kv : bf.files) {
        const BFFile& e = kv.second;
        if (e.name.empty() || e.key == INVALID_KEY) continue;
        std::string base = bf.dir_path(e.parent);
        std::string full = base.empty() ? e.name : base + "/" + e.name;
        rows.push_back({full, e.index, e.key, e.length});
    }
    std::sort(rows.begin(), rows.end(), [](const PathRow& a, const PathRow& b) {
        if (a.path != b.path) return a.path < b.path;
        return a.index < b.index;
    });
    os << "PATHS " << rows.size() << NL;
    for (const PathRow& r : rows) {
        os << "P ";
        canon_name(os, r.path);
        os << '\t' << hex8(r.key) << '\t' << r.length << NL;
    }
}

}  // namespace jade
