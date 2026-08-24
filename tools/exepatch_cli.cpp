// exepatch_cli - end-to-end native standalone-patcher authoring harness.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/ExePatch.hpp"
#include "jade/GameProfiles.hpp"
#include "jade/Json.hpp"
#include "jade/Project.hpp"

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::fprintf(stderr,
                     "usage: exepatch_cli <base.bf> <replacement.bin> "
                     "<stub.exe> <output.exe> [icon]\n");
        return 2;
    }
    try {
        namespace fs = std::filesystem;
        fs::path effective_base = fs::absolute(argv[1]);
        jade::BigFile bf;
        bf.open(effective_base.string());
        const jade::BFFile* target = nullptr;
        for (const auto& item : bf.files) {
            const jade::BFFile& file = item.second;
            if (file.key == jade::INVALID_KEY || file.name == "size.grs")
                continue;
            jade::LzoResult dec = jade::decompress_lzo(bf.read_data(file.index));
            if (dec.ok) { target = &file; break; }
        }
        // The checked-in popsot.bf fixture intentionally has an empty file
        // table. Seed an isolated copy with one ordinary compressed entry so
        // the input archive is never mutated and the test stays small.
        if (!target && bf.files.empty()) {
            fs::path fixture_dir =
                fs::path(argv[4]).parent_path() / "native_exepatch_input";
            fs::create_directories(fixture_dir);
            effective_base = fixture_dir / effective_base.filename();
            fs::copy_file(argv[1], effective_base,
                          fs::copy_options::overwrite_existing);
            bf.open(effective_base.string());
            std::vector<uint8_t> seed(8192);
            for (size_t i = 0; i < seed.size(); ++i)
                seed[i] = static_cast<uint8_t>((i * 13 + i / 11 + 19) & 0xff);
            std::fstream stream(effective_base,
                                std::ios::in | std::ios::out |
                                    std::ios::binary);
            if (!stream) throw std::runtime_error("cannot seed empty fixture");
            bf.add_entry(stream, "native_patch_fixture.bin", 0x13579bdfu,
                         bf.root, jade::compress_lzo(seed, 9));
            stream.close();
            bf.open(effective_base.string());
            for (const auto& item : bf.files) {
                jade::LzoResult dec =
                    jade::decompress_lzo(bf.read_data(item.second.index));
                if (dec.ok) { target = &item.second; break; }
            }
        }
        if (!target) throw std::runtime_error("no decompressible target entry");

        jade::project::ModProject project;
        project.ok = true;
        project.name = "NativeExePatch";
        project.author = "Codex";
        project.description = "native authoring regression";
        project.base.archive_name = effective_base.filename().string();
        project.base.archive_size = fs::file_size(effective_base);
        project.base.archive_sha256 =
            jade::project::sha256_of_file(effective_base.string());
        if (const jade::gameprofiles::GameProfile* profile =
                jade::gameprofiles::detect(effective_base.string()))
            project.base.game = profile->code;
        project.build.strict_inplace = false;

        fs::path jmod = fs::path(argv[4]).parent_path() / "native_exepatch.jmod";
        fs::create_directories(jmod / "assets");
        std::string digest = jade::project::sha256_of_file(argv[2]);
        fs::path asset = jmod / "assets" / (digest + ".bin");
        fs::copy_file(argv[2], asset, fs::copy_options::overwrite_existing);
        project.jmod_dir = jmod.string();
        char op[1024];
        std::snprintf(
            op, sizeof op,
            "{\"id\":\"op-0001\",\"op\":\"replace_entry_raw\","
            "\"enabled\":true,\"target\":{\"entry_key\":\"0x%08x\"},"
            "\"params\":{\"source\":\"asset:%s\"}}",
            target->key, digest.c_str());
        project.operations.push_back(jade::json::parse(op));

        jade::exepatch::Options options;
        options.title = "Native E2E";
        options.description = "standalone patch test";
        options.author = "Codex";
        options.version = "1.0";
        options.out_exe_path = argv[4];
        options.stub_exe_path = argv[3];
        if (argc == 6) options.icon_path = argv[5];
        options.accepted_names = {project.base.archive_name};
        jade::exepatch::Result result = jade::exepatch::make_exe_patch(
            project, effective_base.string(), options,
            [](uint64_t done, uint64_t total, const std::string& phase) {
                std::printf("PROGRESS %llu/%llu %s\n",
                            static_cast<unsigned long long>(done),
                            static_cast<unsigned long long>(total),
                            phase.c_str());
            },
            [](const std::string& line) {
                std::printf("LOG %s\n", line.c_str());
            });
        std::printf("BASE %s\n", effective_base.string().c_str());
        std::printf("ENTRY %u|%08x\n", target->index, target->key);
        for (const auto& issue : result.issues)
            std::printf("ISSUE %s|%s\n", issue.level.c_str(),
                        issue.message.c_str());
        std::printf("REPORT %s\n", result.report.c_str());
        std::printf("RESULT ok=%d path=%s payload=%zu fwd=%zu rev=%zu "
                    "changed=%llu compiler=%s\n",
                    result.ok ? 1 : 0, result.exe_path.c_str(),
                    result.patch_size, result.fwd_segments,
                    result.rev_segments,
                    static_cast<unsigned long long>(result.changed_bytes),
                    result.compiler.c_str());
        return result.ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }
}
