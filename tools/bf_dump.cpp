// bf_dump — open a BigFile and emit its canonical dump (see CanonDump.hpp).
//
//   bf_dump <file.bf>          dump to stdout
//   bf_dump <file.bf> <out>    dump to <out> (binary, '\n' line endings)
//
// The <out> form is what tests/run_golden.py diffs against the Python oracle.
#include <fstream>
#include <iostream>

#include "jade/BigFile.hpp"
#include "jade/CanonDump.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bf_dump <file.bf> [out]\n";
        return 2;
    }
    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (argc >= 3) {
            std::ofstream o(argv[2], std::ios::binary);
            if (!o) {
                std::cerr << "bf_dump: cannot write " << argv[2] << "\n";
                return 2;
            }
            jade::dump_canonical(o, bf);
        } else {
            jade::dump_canonical(std::cout, bf);
        }
    } catch (const std::exception& e) {
        std::cerr << "bf_dump: error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
