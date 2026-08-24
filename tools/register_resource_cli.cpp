// register_resource_cli - offline registration-sidecar oracle/utility.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "jade/RegisterResource.hpp"
#include "jade/Utf8Args.hpp"

namespace {
std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("could not open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}
uint32_t key(const char* text) {
    return uint32_t(std::strtoul(text, nullptr, 0));
}
}  // namespace

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc == 10 && std::string(argv[1]) == "--validate") {
        jade::register_resource::SidecarBuild build;
        build.dec = read_file(argv[2]);
        build.keys.wow = key(argv[3]);
        build.keys.gog = key(argv[4]);
        build.keys.gao = key(argv[5]);
        build.keys.geo = key(argv[6]);
        build.keys.mat = key(argv[7]);
        build.keys.submat = key(argv[8]);
        build.keys.tex = key(argv[9]);
        std::vector<std::string> problems =
            jade::register_resource::validate_sidecar(build);
        for (const std::string& line : problems)
            std::printf("PROBLEM %s\n", line.c_str());
        std::printf("VALIDATE problems=%zu\n", problems.size());
        return 0;
    }
    if (argc != 13) {
        std::fprintf(
            stderr,
            "usage: register_resource_cli <donor.bin> <name> <wow> <gog> "
            "<gao> <geo> <mat> <submat> <tex> <stub.bin> <full.bin> "
            "<output.bin>\n");
        return 2;
    }
    try {
        jade::register_resource::SidecarKeys keys;
        keys.wow = key(argv[3]);
        keys.gog = key(argv[4]);
        keys.gao = key(argv[5]);
        keys.geo = key(argv[6]);
        keys.mat = key(argv[7]);
        keys.submat = key(argv[8]);
        keys.tex = key(argv[9]);
        jade::register_resource::SidecarBuild build =
            jade::register_resource::build_sidecar(
                read_file(argv[1]), keys, argv[2], read_file(argv[10]),
                read_file(argv[11]));
        std::ofstream out(std::filesystem::u8path(argv[12]),
                          std::ios::binary);
        if (!out) throw std::runtime_error("could not open output");
        out.write(reinterpret_cast<const char*>(build.dec.data()),
                  std::streamsize(build.dec.size()));
        const auto& d = build.donor;
        std::printf("DONOR %08x|%08x|%08x|%08x|%u|%08x|%u\n",
                    d.gao_key, d.geo_key, d.mat_key, d.submat_key,
                    d.submat_kind, d.tex_key, d.mat_data_kind);
        for (const std::string& line : build.report)
            std::printf("REPORT %s\n", line.c_str());
        std::vector<std::string> problems =
            jade::register_resource::validate_sidecar(build);
        for (const std::string& line : problems)
            std::printf("PROBLEM %s\n", line.c_str());
        std::printf("RESULT bytes=%zu problems=%zu\n", build.dec.size(),
                    problems.size());
        return problems.empty() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }
}
