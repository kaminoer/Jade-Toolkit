#include "jade/LoaValidate.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: loa_validate_cli <decompressed-entry> "
                     "[ignore-shipped: 0|1]\n";
        return 2;
    }
    std::ifstream input(std::filesystem::u8path(argv[1]), std::ios::binary);
    if (!input) {
        std::cerr << "could not open " << argv[1] << '\n';
        return 1;
    }
    const std::vector<uint8_t> dec{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    const bool ignore_shipped = argc != 3 || std::string(argv[2]) != "0";

    for (const jade::loa::DepRef& ref : jade::loa::collect_dep_refs(dec)) {
        std::cout << "REF " << std::hex << std::setfill('0') << std::setw(8)
                  << ref.gao_key << std::dec << ' ' << ref.gao_offset << ' '
                  << std::hex << std::setw(8) << ref.dep_key << std::dec << ' '
                  << ref.dep_kind << '\n';
    }
    for (const jade::loa::Issue& issue :
         jade::loa::validate_loa_stream(dec, ignore_shipped)) {
        std::cout << "ISSUE " << issue.level << ' ' << issue.message << '\n';
    }
    return 0;
}
