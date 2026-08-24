// bf_decomp_digest — decompress BigFile entries and emit a canonical digest.
//
//   bf_decomp_digest <file.bf> [out] [limit]
//
// For each active file entry (sorted by index, up to `limit`; 0 = all), reads
// its raw payload, runs decompress_lzo + lzo_terminator_offset, and emits one
// line carrying the terminator offset, total declared size, decompressed length
// and a CRC-32 of the decompressed bytes. tests/run_golden.py diffs this against
// the Python oracle (core.compression) byte-for-byte, so the CRC proves the
// decompressed payloads are identical without dumping megabytes.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"   // hex8
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"

static void emit(std::ostream& os, const jade::BigFile& bf, size_t limit) {
    const char NL = '\n';

    // Active entries (name set, key valid), in ascending index order.
    std::vector<uint32_t> active;
    for (const auto& kv : bf.files) {
        const jade::BFFile& e = kv.second;
        if (!e.name.empty() && e.key != jade::INVALID_KEY) active.push_back(e.index);
    }

    size_t emit_n = (limit == 0) ? active.size() : std::min(limit, active.size());

    os << "DECOMP v1" << NL;
    os << "limit " << limit << NL;
    os << "active_total " << active.size() << NL;
    os << "emitted " << emit_n << NL;

    for (size_t i = 0; i < emit_n; ++i) {
        uint32_t idx = active[i];
        const jade::BFFile& e = bf.files.at(idx);
        std::vector<uint8_t> raw = bf.read_data(idx);
        long long term = jade::lzo_terminator_offset(raw);
        jade::LzoResult r = jade::decompress_lzo(raw);

        os << "E " << e.index << ' ' << jade::hex8(e.key) << ' '
           << raw.size() << " term=" << term << " tot=" << r.total_dec << ' ';
        if (!r.ok) {
            os << "NULL";
        } else {
            os << "len=" << r.data.size() << " crc=" << jade::hex8(jade::crc32(r.data));
        }
        os << NL;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_decomp_digest <file.bf> [out] [limit]\n";
        return 2;
    }
    size_t limit = 0;
    if (argc >= 4) limit = static_cast<size_t>(std::strtoul(argv[3], nullptr, 10));

    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3 && std::string(argv[2]) != "-") {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) { std::cerr << "bf_decomp_digest: cannot write " << argv[2] << "\n"; return 2; }
            emit(o, bf, limit);
        } else {
            emit(std::cout, bf, limit);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_decomp_digest: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
