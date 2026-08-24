// bytepatch_cli - JADEBPT3 core oracle and headless utility.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "jade/BytePatch.hpp"
#include "jade/GameProfiles.hpp"

namespace {
std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("could not open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}
void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("could not open " + path);
    f.write(reinterpret_cast<const char*>(data.data()),
            std::streamsize(data.size()));
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--roundtrip") {
            jade::bytepatch::Patch patch =
                jade::bytepatch::unpack_patch(read_file(argv[2]));
            write_file(argv[3], jade::bytepatch::pack_patch(patch));
            std::printf("ROUNDTRIP fwd=%zu rev=%zu\n", patch.fwd_segs.size(),
                        patch.rev_segs.size());
            return 0;
        }
        if ((argc == 6 || argc == 7) &&
            std::string(argv[1]) == "--build") {
            jade::bytepatch::PayloadOptions options;
            options.title = "NativeTest";
            options.description = "byte patch parity";
            options.author = "Codex";
            options.version = "2.3";
            options.archive_name = "pop3.bf";
            options.accepted_names = {"pop3.bf", "Prince.bf"};
            options.created = argv[5];
            if (argc == 7) options.image_path = argv[6];
            jade::bytepatch::PayloadBuild built =
                jade::bytepatch::build_patch_payload(argv[2], argv[3],
                                                      options);
            write_file(argv[4], built.payload);
            std::printf("BUILD fwd=%zu rev=%zu changed=%llu bytes=%zu\n",
                        built.stats.fwd_segments, built.stats.rev_segments,
                        static_cast<unsigned long long>(
                            built.stats.changed_bytes),
                        built.payload.size());
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--apply") {
            jade::bytepatch::apply_patch_to_file(read_file(argv[2]), argv[3],
                                                 argv[4]);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--reverse") {
            jade::bytepatch::Patch patch =
                jade::bytepatch::unpack_patch(read_file(argv[2]));
            jade::bytepatch::apply_reverse_inplace(patch, argv[3]);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--classify") {
            jade::bytepatch::Patch patch =
                jade::bytepatch::unpack_patch(read_file(argv[2]));
            std::printf("%s\n",
                        jade::bytepatch::classify_target(patch, argv[3]).c_str());
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--embed") {
            jade::bytepatch::append_payload(argv[2], read_file(argv[3]),
                                            argv[4]);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--read-embedded") {
            write_file(argv[3],
                       jade::bytepatch::read_embedded_payload(argv[2]));
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--names") {
            for (const std::string& name :
                 jade::bytepatch::accepted_target_names(argv[2], argv[3]))
                std::printf("NAME %s\n", name.c_str());
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--profiles") {
            for (const jade::gameprofiles::GameProfile& p :
                 jade::gameprofiles::all()) {
                std::printf("PROFILE\t%s\t%s\t%u\t", p.code.c_str(),
                            p.name.c_str(), p.bf_version);
                for (size_t i = 0; i < p.archive_filenames.size(); ++i) {
                    if (i) std::putchar(',');
                    std::fputs(p.archive_filenames[i].c_str(), stdout);
                }
                std::printf("\t%s\t%s\t%d\t%d\n", p.game_dir_substr.c_str(),
                            p.platform.c_str(), int(p.paletted_textures),
                            int(p.ps2_geo_format));
            }
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--detect") {
            const jade::gameprofiles::GameProfile* p =
                jade::gameprofiles::detect(argv[2]);
            std::printf("%s\n", p ? p->code.c_str() : "NONE");
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--sha") {
            std::printf("%s\n", jade::bytepatch::sha256_file(argv[2]).c_str());
            return 0;
        }
        std::fprintf(stderr,
                     "usage: bytepatch_cli --roundtrip in out | "
                     "--build base result out created [image] | "
                     "--apply payload base out | --reverse payload target | "
                     "--classify payload target | --embed stub payload out | "
                     "--read-embedded exe out | --names game loaded | "
                     "--profiles | --detect archive | "
                     "--sha file\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }
}
