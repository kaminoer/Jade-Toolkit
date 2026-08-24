// ExePatch.cpp - standalone patcher authoring pipeline.
#include "jade/ExePatch.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept>

#include "jade/BytePatch.hpp"
#include "jade/Image.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace jade {
namespace exepatch {
namespace {

std::string comma(uint64_t value) {
    std::string s = std::to_string(value);
    for (long long i = static_cast<long long>(s.size()) - 3; i > 0; i -= 3)
        s.insert(size_t(i), 1, ',');
    return s;
}

std::filesystem::path make_work_dir() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path();
    uint64_t stamp = uint64_t(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    for (uint32_t attempt = 0; attempt < 100; ++attempt) {
        fs::path candidate =
            root / ("jade_exepatch_" + std::to_string(stamp) + "_" +
                    std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) return candidate;
    }
    throw std::runtime_error("could not create temporary EXE-patch directory");
}

std::string joined_log(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) out += '\n';
        out += line;
    }
    return out;
}

project::BuildIssue error_issue(const std::string& message) {
    project::BuildIssue issue;
    issue.level = "error";
    issue.message = message;
    return issue;
}

#ifdef _WIN32
void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(uint8_t(value)); out.push_back(uint8_t(value >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value)); out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value >> 16)); out.push_back(uint8_t(value >> 24));
}

bool iconized_stub(const std::string& source_stub,
                   const std::string& icon_path,
                   const std::filesystem::path& output,
                   std::string& error) {
    namespace fs = std::filesystem;
    RgbaImage source = load_rgba_image(icon_path);
    if (!source.ok) { error = source.error; return false; }
    std::error_code ec;
    fs::copy_file(source_stub, output, fs::copy_options::overwrite_existing,
                  ec);
    if (ec) { error = "could not copy bundled stub"; return false; }
    const std::wstring wide = output.wstring();
    if (wide.empty()) { error = "output path is empty"; return false; }
    HANDLE update = BeginUpdateResourceW(wide.c_str(), FALSE);
    if (!update) { error = "BeginUpdateResource failed"; return false; }

    const uint16_t sizes[] = {16, 24, 32, 48, 64, 128, 256};
    std::vector<std::vector<uint8_t>> images;
    images.reserve(sizeof sizes / sizeof sizes[0]);
    bool ok = true;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        std::vector<uint8_t> rgba = resize_rgba_lanczos(
            source.rgba.data(), source.width, source.height, sizes[i],
            sizes[i]);
        images.push_back(encode_png_rgba(rgba.data(), sizes[i], sizes[i]));
        if (images.back().empty()
            || !UpdateResourceW(update, MAKEINTRESOURCEW(3),
                                MAKEINTRESOURCEW(101 + int(i)),
                                0, images.back().data(),
                                DWORD(images.back().size()))) {
            ok = false;
            error = "could not write icon image resource";
            break;
        }
    }
    std::vector<uint8_t> group;
    if (ok) {
        put_u16(group, 0); put_u16(group, 1);
        put_u16(group, uint16_t(images.size()));
        for (size_t i = 0; i < images.size(); ++i) {
            group.push_back(sizes[i] == 256 ? 0 : uint8_t(sizes[i]));
            group.push_back(sizes[i] == 256 ? 0 : uint8_t(sizes[i]));
            group.push_back(0); group.push_back(0);
            put_u16(group, 1); put_u16(group, 32);
            put_u32(group, uint32_t(images[i].size()));
            put_u16(group, uint16_t(101 + i));
        }
        if (!UpdateResourceW(update, MAKEINTRESOURCEW(14),
                             MAKEINTRESOURCEW(1), 0,
                             group.data(), DWORD(group.size()))) {
            ok = false;
            error = "could not write icon group resource";
        }
    }
    if (!EndUpdateResourceW(update, ok ? FALSE : TRUE)) {
        if (ok) error = "EndUpdateResource failed";
        ok = false;
    }
    if (!ok) fs::remove(output, ec);
    return ok;
}
#endif

std::string format_report(const project::ModProject& project,
                          const std::string& exe,
                          const bytepatch::PayloadStats& stats,
                          const project::BuildResult& build) {
    std::ostringstream out;
    out << "EXE patch report \xe2\x80\x94 '" << project.name << "'\n"
        << "  patcher exe        : " << exe << "\n"
        << "  compiler           : bundled\n"
        << "  base size          : " << comma(stats.base_size) << " bytes\n"
        << "  patched size       : " << comma(stats.result_size) << " bytes\n"
        << "  fwd / rev segments : " << stats.fwd_segments << " / "
        << stats.rev_segments << "\n"
        << "  changed bytes      : " << comma(stats.changed_bytes) << "\n"
        << "  payload size       : " << comma(stats.payload_size) << " bytes\n\n"
        << "Underlying build:";
    for (const std::string& line : build.log) out << "\n  " << line;
    return out.str();
}

}  // namespace

Result make_exe_patch(const project::ModProject& project,
                      const std::string& base_archive_path,
                      const Options& options,
                      ProgressFn progress, LogFn log) {
    if (!progress) progress = [](uint64_t, uint64_t, const std::string&) {};
    if (!log) log = [](const std::string&) {};
    Result result;

    namespace fs = std::filesystem;
    if (!fs::is_regular_file(options.stub_exe_path)) {
        result.issues.push_back(error_issue(
            "Bundled C++ patcher stub not found: " + options.stub_exe_path));
        result.report = "no C++ patcher stub";
        return result;
    }
    const bool own_work = options.work_dir.empty();
    fs::path work;
    try {
        work = own_work ? make_work_dir() : fs::path(options.work_dir);
        fs::create_directories(work);
        std::string archive_name = project.base.archive_name.empty()
                                       ? fs::path(base_archive_path)
                                             .filename().string()
                                       : project.base.archive_name;
        std::vector<std::string> accepted = options.accepted_names.empty()
            ? bytepatch::accepted_target_names(project.base.game, archive_name)
            : options.accepted_names;
        fs::path built = work / ("built_" +
                                 (archive_name.empty() ? std::string("out.bf")
                                                       : archive_name));

        progress(0, 1, "Building patched archive");
        log("building patched archive...");
        project::BuildResult build = project::build_project(
            project, base_archive_path, built.string(), false,
            [&](uint64_t done, uint64_t total, const std::string& phase) {
                progress(done, total, "Build: " + phase);
            });
        if (!build.ok) {
            result.issues = build.issues;
            result.report = "build failed:\n" + joined_log(build.log);
            if (own_work && !options.keep_work) {
                std::error_code ec; fs::remove_all(work, ec);
            }
            return result;
        }
        log("built patched archive (" + comma(fs::file_size(built)) +
            " bytes)");

        progress(0, 1, "Computing byte diff");
        log("computing byte diff (original vs modded, both directions)...");
        bytepatch::PayloadOptions payload_options;
        payload_options.title = options.title;
        payload_options.description = options.description;
        payload_options.author = options.author;
        payload_options.version = options.version;
        payload_options.archive_name = archive_name;
        payload_options.accepted_names = accepted;
        payload_options.image_path = options.image_path;
        bytepatch::PayloadBuild payload = bytepatch::build_patch_payload(
            base_archive_path, built.string(), payload_options,
            [&](uint64_t done, uint64_t total) {
                progress(done, total, "Diffing");
            });
        log("diff: " + std::to_string(payload.stats.fwd_segments) +
            " fwd / " + std::to_string(payload.stats.rev_segments) +
            " rev segment(s), " + comma(payload.stats.changed_bytes) +
            " changed byte(s), payload " +
            comma(payload.stats.payload_size) + " bytes");

        progress(0, 1, "Verifying patch");
        log("self-verifying patch (forward and reverse)...");
        bytepatch::Patch meta = bytepatch::unpack_patch(payload.payload);
        fs::path forward = work / "verify_fwd.bin";
        bytepatch::apply_patch_to_file(payload.payload, base_archive_path,
                                       forward.string());
        if (bytepatch::sha256_file(forward.string()) !=
            payload.stats.result_sha256) {
            result.issues.push_back(error_issue("forward self-verify failed"));
            result.report = "self-verify";
            if (own_work && !options.keep_work) {
                std::error_code ec; fs::remove_all(work, ec);
            }
            return result;
        }
        fs::path reverse = work / "verify_rev.bin";
        fs::copy_file(built, reverse, fs::copy_options::overwrite_existing);
        bytepatch::apply_reverse_inplace(meta, reverse.string());
        if (bytepatch::sha256_file(reverse.string()) !=
            payload.stats.base_sha256) {
            result.issues.push_back(
                error_issue("reverse (unpatch) self-verify failed"));
            result.report = "self-verify";
            if (own_work && !options.keep_work) {
                std::error_code ec; fs::remove_all(work, ec);
            }
            return result;
        }
        std::error_code remove_ec;
        fs::remove(forward, remove_ec);
        fs::remove(reverse, remove_ec);
        log("self-verify OK \xe2\x80\x94 forward reproduces the mod, reverse "
            "restores the original");

        progress(0, 1, "Assembling patcher (C++)");
        fs::path output = fs::absolute(options.out_exe_path);
        if (!output.parent_path().empty())
            fs::create_directories(output.parent_path());
        fs::path selected_stub = options.stub_exe_path;
        if (!options.icon_path.empty()) {
#ifdef _WIN32
            fs::path with_icon = work / "patcher_icon_stub.exe";
            std::string icon_error;
            if (iconized_stub(options.stub_exe_path, options.icon_path,
                              with_icon, icon_error)) {
                selected_stub = with_icon;
                log("embedded custom multi-size executable icon");
            } else {
                log("icon skipped (" + icon_error + ")");
            }
#else
            log("icon skipped (executable resource updates require Windows)");
#endif
        }
        bytepatch::append_payload(selected_stub.string(), payload.payload,
                                  output.string());
        log("wrote patcher exe -> " + output.string() + " (" +
            comma(fs::file_size(output)) + " bytes)");

        result.ok = true;
        result.exe_path = output.string();
        result.patch_size = payload.stats.payload_size;
        result.fwd_segments = payload.stats.fwd_segments;
        result.rev_segments = payload.stats.rev_segments;
        result.changed_bytes = payload.stats.changed_bytes;
        result.base_size = payload.stats.base_size;
        result.result_size = payload.stats.result_size;
        result.compiler = "bundled";
        result.issues = build.issues;
        result.report = format_report(project, output.string(), payload.stats,
                                      build);
    } catch (const std::exception& e) {
        result.issues.push_back(
            error_issue("exe patch failed: " + std::string(e.what())));
        result.report = "unexpected exception: " + std::string(e.what());
    }
    if (own_work && !options.keep_work && !work.empty()) {
        // `work` was created directly under temp_directory_path by this call;
        // user-supplied work directories are never removed.
        std::error_code ec;
        fs::path canonical_work = fs::weakly_canonical(work, ec);
        fs::path canonical_temp = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (!ec && canonical_work.parent_path() == canonical_temp)
            fs::remove_all(canonical_work, ec);
    }
    return result;
}

}  // namespace exepatch
}  // namespace jade
