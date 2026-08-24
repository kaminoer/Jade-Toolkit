// bf_wolinfo_digest — parse the WOLInfo.bin (key 0xFC000001) dep table, digest.
//
//   bf_wolinfo_digest <file.bf> [out] [limit-ignored]
//
// The WOLInfo entry is a single UNCOMPRESSED system table — read the raw entry
// bytes directly (no decompress) and emit the parsed table. Diffed against
// core.wolinfo.WolInfo.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Crc32.hpp"
#include "jade/WolInfo.hpp"

static void emit(std::ostream& os, const jade::BigFile& bf) {
    const char NL = '\n';
    os << "WOLINFO v1" << NL;

    const jade::BFFile* fi = nullptr;
    for (const auto& kv : bf.files)   // ascending index, like the Python next()
        if ((kv.second.key & 0xFFFFFFFFu) == jade::WOLINFO_KEY) { fi = &kv.second; break; }
    if (!fi) { os << "present 0" << NL; return; }

    std::vector<uint8_t> raw = bf.read_data(fi->index);   // RAW (uncompressed)
    jade::WolInfo w = jade::parse_wolinfo(raw);
    if (!w.ok) { os << "present 1" << NL << "parse_ok 0" << NL; return; }

    os << "present 1" << NL << "parse_ok 1" << NL
       << "total_len " << w.total_len << NL << "count " << w.count << NL
       << "entries " << w.entries.size() << NL << "trailing " << w.trailing_len << NL
       << "rt " << (w.to_bytes() == raw ? 1 : 0) << NL;
    for (size_t i = 0; i < w.entries.size(); ++i) {
        const jade::WolEntry& e = w.entries[i];
        std::string dcrc = jade::hex8(jade::crc32(
            reinterpret_cast<const uint8_t*>(e.deps.data()), e.deps.size() * 4));
        os << "E " << i << " wol=" << jade::hex8(e.wol_key) << " n=" << e.deps.size()
           << " dcrc=" << dcrc << NL;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_wolinfo_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_wolinfo_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf);
        } else {
            emit(std::cout, bf);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_wolinfo_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
