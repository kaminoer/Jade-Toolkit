#pragma once

#include <filesystem>
#include <string>

namespace jade::deploy {

enum class Error {
    None,
    BuiltArchiveMissing,
    GameDirectoryInvalid,
    BackupMissing,
    CreateBackupDirectoryFailed,
    BackupCopyFailed,
    DeployCopyFailed,
    RestoreCopyFailed,
};

const char* error_name(Error error);

std::string backup_name(const std::string& target_name,
                        const std::string& game_code);

// The Python toolkit places "backups" next to the toolkit. Native frontends
// may supply that toolkit directory explicitly; the no-argument form uses the
// executable directory.
std::filesystem::path default_backup_dir();
std::filesystem::path default_backup_dir(
    const std::filesystem::path& toolkit_dir);

struct Options {
    bool backup = true;
    std::filesystem::path backup_dir;
};

struct DeployResult {
    Error error = Error::None;
    std::string message;
    std::filesystem::path target;
    std::filesystem::path backup;
    bool has_backup = false;
    bool backed_up_now = false;

    bool ok() const { return error == Error::None; }
};

DeployResult deploy(const std::filesystem::path& built_archive_path,
                    const std::filesystem::path& game_dir,
                    const std::string& target_name,
                    const std::string& game_code,
                    const Options& options = Options{});

struct RestoreResult {
    Error error = Error::None;
    std::string message;
    std::filesystem::path target;

    bool ok() const { return error == Error::None; }
};

RestoreResult restore_stock(const std::filesystem::path& game_dir,
                            const std::string& target_name,
                            const std::string& game_code,
                            const std::filesystem::path& backup_dir = {});

}  // namespace jade::deploy
