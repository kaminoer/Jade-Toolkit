// jgao_cli - inspect a JGAO or rebuild one from an edited GLB.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "jade/Jgao.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Utf8Args.hpp"

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    if (argc < 3) {
        std::cerr << "usage: jgao_cli info <file.jgao>\n"
                     "       jgao_cli jgao-to-glb <in.jgao> <out.glb> [entry.bin]\n"
                     "       jgao_cli glb-to-jgao <in.glb> <template.jgao> <out.jgao>\n";
        return 2;
    }
    if (std::string(argv[1]) == "jgao-to-glb" && (argc == 4 || argc == 5)) {
        std::vector<uint8_t> input, entry;
        if (!read_file(argv[2], input)) { std::cerr << "cannot open " << argv[2] << "\n"; return 2; }
        std::vector<jade::SubEntry> context;
        if (argc == 5) {
            if (!read_file(argv[4], entry)) { std::cerr << "cannot open " << argv[4] << "\n"; return 2; }
            context = jade::parse_sub_entries(entry);
        }
        jade::jgao::GlbResult r = jade::jgao::jgao_to_glb(
            input.data(), input.size(), argc == 5 ? &context : nullptr);
        if (!r.ok) { std::cerr << "jgao_cli: " << r.error << "\n"; return 1; }
        if (!write_file(argv[3], r.glb)) { std::cerr << "cannot write " << argv[3] << "\n"; return 2; }
        std::cout << "vertices=" << r.vertices << " faces=" << r.faces
                  << " elements=" << r.elements << " bones=" << r.bones
                  << " materials=" << r.materials << " textures=" << r.textures << "\n";
        return 0;
    }
    if (std::string(argv[1]) == "info" && argc == 3) {
        std::vector<uint8_t> data;
        if (!read_file(argv[2], data)) { std::cerr << "cannot open " << argv[2] << "\n"; return 2; }
        jade::jgao::File f = jade::jgao::parse(data.data(), data.size());
        if (!f.ok) { std::cerr << f.error << "\n"; return 1; }
        std::cout << "JGAO version=" << f.version << " gao=" << f.gao_key
                  << " geo=" << f.geo_key << " mat=" << f.mat_key
                  << " gao_size=" << f.gao_data.size()
                  << " geo_size=" << f.geo_data.size()
                  << " mat_size=" << f.mat_data.size()
                  << " children=" << f.mat_children.size() << "\n";
        return 0;
    }
    if (std::string(argv[1]) == "glb-to-jgao" && argc == 5) {
        std::vector<uint8_t> glb, templ;
        if (!read_file(argv[2], glb)) { std::cerr << "cannot open " << argv[2] << "\n"; return 2; }
        if (!read_file(argv[3], templ)) { std::cerr << "cannot open " << argv[3] << "\n"; return 2; }
        jade::jgao::ConvertResult r = jade::jgao::glb_to_jgao(
            glb.data(), glb.size(), templ.data(), templ.size());
        if (!r.ok) { std::cerr << "jgao_cli: " << r.error << "\n"; return 1; }
        if (!write_file(argv[4], r.jgao)) { std::cerr << "cannot write " << argv[4] << "\n"; return 2; }
        std::cout << "vertices=" << r.vertices << " faces=" << r.faces
                  << " elements=" << r.elements << " geo_size=" << r.geo_size
                  << " bones=" << r.bones << "\n";
        return 0;
    }
    std::cerr << "invalid arguments\n";
    return 2;
}
