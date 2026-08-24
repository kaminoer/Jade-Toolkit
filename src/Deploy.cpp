#include "jade/Deploy.hpp"

#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jade::deploy {
namespace fs = std::filesystem;

namespace {

bool is_file(const fs::path& path) {
    std::error_code ec;
    const bool result = fs::is_regular_file(path, ec);
    return !ec && result;
}

bool path_is_directory(const fs::path& path) {
    std::error_code ec;
    const bool result = fs::is_directory(path, ec);
    return !ec && result;
}

std::string path_text(const fs::path& path) {
#ifdef _WIN32
    // C++17's u8string is a std::string and retains non-ASCII Windows paths.
    return path.u8string();
#else
    return path.string();
#endif
}

// shutil.copy2 copies file data and then metadata. copy_file's metadata
// guarantees vary by standard-library implementation, so copy the portable
// mode and last-write timestamp explicitly as well.
bool copy2(const fs::path& source, const fs::path& destination,
           std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    // libstdc++/MinGW's copy_file currently reports ERROR_FILE_EXISTS even
    // with overwrite_existing. CopyFileW is also the closest native match to
    // the copy operation used by Python's shutil on Windows.
    if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
        ec = std::error_code(static_cast<int>(GetLastError()),
                             std::system_category());
        return false;
    }
#else
    if (!fs::copy_file(source, destination, fs::copy_options::overwrite_existing,
                       ec)) {
        return false;
    }
#endif

    const fs::file_status source_status = fs::status(source, ec);
    if (ec) return false;
    fs::permissions(destination, source_status.permissions(),
                    fs::perm_options::replace, ec);
    if (ec) return false;

    const fs::file_time_type write_time = fs::last_write_time(source, ec);
    if (ec) return false;
    fs::last_write_time(destination, write_time, ec);
    return !ec;
}

}  // namespace

const char* error_name(Error error) {
    switch (error) {
        case Error::None: return "none";
        case Error::BuiltArchiveMissing: return "built_archive_missing";
        case Error::GameDirectoryInvalid: return "game_directory_invalid";
        case Error::BackupMissing: return "backup_missing";
        case Error::CreateBackupDirectoryFailed:
            return "create_backup_directory_failed";
        case Error::BackupCopyFailed: return "backup_copy_failed";
        case Error::DeployCopyFailed: return "deploy_copy_failed";
        case Error::RestoreCopyFailed: return "restore_copy_failed";
    }
    return "unknown";
}

std::string backup_name(const std::string& target_name,
                        const std::string& game_code) {
    return target_name + "." + game_code + ".stock";
}

fs::path default_backup_dir(const fs::path& toolkit_dir) {
    return toolkit_dir / "backups";
}

fs::path default_backup_dir() {
#ifdef _WIN32
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) break;
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return default_backup_dir(fs::path(buffer).parent_path());
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
    std::error_code ec;
    const fs::path current = fs::current_path(ec);
    return default_backup_dir(ec ? fs::path(".") : current);
}

DeployResult deploy(const fs::path& built_archive_path,
                    const fs::path& game_dir,
                    const std::string& target_name,
                    const std::string& game_code,
                    const Options& options) {
    DeployResult result;
    if (!is_file(built_archive_path)) {
        result.error = Error::BuiltArchiveMissing;
        result.message = path_text(built_archive_path);
        return result;
    }
    if (!path_is_directory(game_dir)) {
        result.error = Error::GameDirectoryInvalid;
        result.message = path_text(game_dir);
        return result;
    }

    result.target = game_dir / fs::u8path(target_name);
    const fs::path backup_dir = options.backup_dir.empty()
                                    ? default_backup_dir()
                                    : options.backup_dir;
    result.backup = backup_dir / fs::u8path(backup_name(target_name, game_code));

    if (options.backup && is_file(result.target)) {
        std::error_code ec;
        fs::create_directories(backup_dir, ec);
        if (ec) {
            result.error = Error::CreateBackupDirectoryFailed;
            result.message = "could not create backup directory " +
                             path_text(backup_dir) + ": " + ec.message();
            return result;
        }
        if (!is_file(result.backup)) {
            if (!copy2(result.target, result.backup, ec)) {
                result.error = Error::BackupCopyFailed;
                result.message = "could not back up stock to " +
                                 path_text(result.backup) + ": " + ec.message();
                return result;
            }
            result.backed_up_now = true;
        }
    }

    std::error_code ec;
    if (!copy2(built_archive_path, result.target, ec)) {
        result.error = Error::DeployCopyFailed;
        result.message = "copy failed -> " + path_text(result.target) + ": " +
                         ec.message();
        return result;
    }
    result.has_backup = is_file(result.backup) || result.backed_up_now;
    return result;
}

RestoreResult restore_stock(const fs::path& game_dir,
                            const std::string& target_name,
                            const std::string& game_code,
                            const fs::path& backup_dir) {
    RestoreResult result;
    const fs::path effective_backup_dir =
        backup_dir.empty() ? default_backup_dir() : backup_dir;
    const fs::path backup = effective_backup_dir /
                            fs::u8path(backup_name(target_name, game_code));
    if (!is_file(backup)) {
        result.error = Error::BackupMissing;
        result.message = "no stock backup for " + target_name + " (" +
                         game_code + ") at " + path_text(backup);
        return result;
    }

    result.target = game_dir / fs::u8path(target_name);
    std::error_code ec;
    if (!copy2(backup, result.target, ec)) {
        result.error = Error::RestoreCopyFailed;
        result.message = "copy failed -> " + path_text(result.target) + ": " +
                         ec.message();
    }
    return result;
}

}  // namespace jade::deploy
