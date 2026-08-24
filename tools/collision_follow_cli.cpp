#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "jade/CollisionFollow.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Utf8Args.hpp"

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) throw std::runtime_error("could not open input");
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream output(std::filesystem::u8path(path),
                         std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()),
                 std::streamsize(data.size()));
    if (!output) throw std::runtime_error("could not write output");
}

uint32_t parse_key(const char* text) {
    return uint32_t(std::strtoul(text, nullptr, 0));
}

void print_u32_list(const std::vector<uint32_t>& values) {
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) std::putchar(',');
        std::printf("%u", values[i]);
    }
}

void print_links(const jade::collision_follow::CollisionLinks& links) {
    std::printf("LINKS moved=%08x same=%d dedicated=%zu carves=%zu\n",
                links.moved_gao_key, links.same_gao ? 1 : 0,
                links.dedicated.size(), links.carves.size());
    for (const auto& link : links.dedicated) {
        std::printf("DED gao=%08x name=%s cobs=", link.gao_key,
                    link.name.c_str());
        print_u32_list(link.cob_keys);
        std::putchar('\n');
    }
    for (const auto& link : links.carves) {
        std::printf("CARVE cob=%08x owner=%08x name=%s total=%u faces=",
                    link.cob_key, link.owner_gao_key, link.owner_name.c_str(),
                    link.n_total_faces);
        print_u32_list(link.face_indices);
        std::putchar('\n');
    }
}

int run_detect(const char* input_path, const char* key_text) {
    const std::vector<uint8_t> entry = read_file(input_path);
    const auto links = jade::collision_follow::detect_collision_links(
        parse_key(key_text), jade::walk_sub_entries(entry));
    print_links(links);
    return 0;
}

int run_apply(char** argv) {
    const std::vector<uint8_t> entry = read_file(argv[2]);
    const uint32_t moved_key = parse_key(argv[4]);
    jade::collision_follow::JadeMatrix old_matrix{}, new_matrix{};
    for (size_t i = 0; i < 16; ++i) {
        old_matrix[i] = std::stod(argv[5 + i]);
        new_matrix[i] = std::stod(argv[21 + i]);
    }
    const auto links = jade::collision_follow::detect_collision_links(
        moved_key, jade::walk_sub_entries(entry));
    print_links(links);
    const std::vector<uint8_t> patched =
        jade::collision_follow::apply_collision_follow(
            entry, links, old_matrix, new_matrix,
            [](const std::string& line) {
                std::printf("LOG %s\n", line.c_str());
            });
    write_file(argv[3], patched);
    std::printf("APPLY bytes=%zu\n", patched.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    try {
        if (argc == 4 && std::string(argv[1]) == "detect")
            return run_detect(argv[2], argv[3]);
        if (argc == 37 && std::string(argv[1]) == "apply")
            return run_apply(argv);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    if (argc != 20) {
        std::cerr <<
            "usage: collision_follow_cli <input.cob> <output.cob> "
            "<comma-separated-face-indices> <16 row-major matrix values>\n"
            "       collision_follow_cli detect <entry.bin> <gao-key>\n"
            "       collision_follow_cli apply <entry.bin> <output.bin> "
            "<gao-key> <16 old Jade values> <16 new Jade values>\n";
        return 2;
    }
    const std::vector<uint8_t> bytes = read_file(argv[1]);
    jade::CobInfo cob = jade::parse_cob(bytes.data(), bytes.size());
    if (!cob.ok) { std::cerr << "input is not a triangle COB\n"; return 1; }
    std::vector<uint32_t> faces;
    std::istringstream list(argv[3]);
    std::string item;
    while (std::getline(list, item, ','))
        if (!item.empty()) faces.push_back(uint32_t(std::stoul(item)));
    jade::collision_follow::Matrix4 transform{};
    for (size_t i = 0; i < 16; ++i) transform[i] = std::stod(argv[4+i]);
    try {
        cob = jade::collision_follow::carve_cob_faces(cob, faces, transform);
        write_file(argv[2], jade::serialize_cob(cob));
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n'; return 1;
    }
    return 0;
}
