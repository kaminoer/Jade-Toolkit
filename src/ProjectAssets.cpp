#include "jade/ProjectAssets.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "jade/Sha256.hpp"

namespace jade {
namespace project_assets {
namespace {

constexpr const char* kPrefix = "asset:";

bool stored_digest(const std::string& name, std::string* digest) {
    if (name.size() < 64) return false;
    const std::string candidate = name.substr(0, 64);
    if (!std::all_of(candidate.begin(), candidate.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) {
        return false;
    }
    *digest = candidate;
    return true;
}

std::string lower_extension(const std::filesystem::path& source) {
    std::string ext = source.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

}  // namespace

bool is_asset_ref(const std::string& value) {
    return value.rfind(kPrefix, 0) == 0;
}

std::string parse_asset_ref(const std::string& ref) {
    if (!is_asset_ref(ref))
        throw std::invalid_argument("not an asset ref: '" + ref + "'");
    return ref.substr(6);
}

AssetStore::AssetStore(std::string assets_dir)
    : assets_dir_(std::move(assets_dir)) {
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::u8path(assets_dir_), error);
    if (error)
        throw std::runtime_error("cannot create asset store: " +
                                 error.message());
}

std::string AssetStore::add(const std::string& source_path) const {
    const std::filesystem::path source = std::filesystem::u8path(source_path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error))
        throw std::runtime_error(source_path);
    const std::string digest = sha256_file_hex(source_path);
    if (digest.empty()) throw std::runtime_error(source_path);
    const std::filesystem::path destination =
        std::filesystem::u8path(assets_dir_) /
        std::filesystem::u8path(digest + lower_extension(source));
    if (!std::filesystem::exists(destination, error)) {
        error.clear();
        if (!std::filesystem::copy_file(source, destination,
                                        std::filesystem::copy_options::none,
                                        error)) {
            throw std::runtime_error("cannot copy asset: " + error.message());
        }
        // shutil.copy2 preserves metadata. Mirror the portable pieces and
        // ignore filesystems which do not support either attribute.
        std::error_code metadata_error;
        std::filesystem::permissions(
            destination, std::filesystem::status(source, metadata_error).permissions(),
            metadata_error);
        metadata_error.clear();
        const auto timestamp =
            std::filesystem::last_write_time(source, metadata_error);
        if (!metadata_error)
            std::filesystem::last_write_time(destination, timestamp,
                                             metadata_error);
    }
    return std::string(kPrefix) + digest;
}

std::string AssetStore::add_bytes(const std::vector<uint8_t>& data,
                                  const std::string& ext) const {
    const std::string digest = sha256_hex(data);
    const std::filesystem::path destination =
        std::filesystem::u8path(assets_dir_) /
        std::filesystem::u8path(digest + ext);
    std::error_code error;
    if (!std::filesystem::exists(destination, error)) {
        std::ofstream output(destination, std::ios::binary);
        if (!output) throw std::runtime_error("cannot write asset");
        output.write(reinterpret_cast<const char*>(data.data()),
                     std::streamsize(data.size()));
        if (!output) throw std::runtime_error("cannot write asset");
    }
    return std::string(kPrefix) + digest;
}

std::string AssetStore::find(const std::string& digest) const {
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::u8path(assets_dir_);
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code file_error;
        if (!it->is_regular_file(file_error)) continue;
        const std::string name = it->path().filename().u8string();
        if (name.rfind(digest, 0) == 0) return it->path().u8string();
    }
    return "";
}

std::string AssetStore::resolve(const std::string& ref) const {
    const std::string digest = parse_asset_ref(ref);
    const std::string path = find(digest);
    if (path.empty())
        throw std::runtime_error("asset " + digest.substr(0, 12) +
                                 "\xE2\x80\xA6 not in store");
    return path;
}

bool AssetStore::exists(const std::string& ref) const {
    try {
        return !find(parse_asset_ref(ref)).empty();
    } catch (const std::invalid_argument&) {
        return false;
    }
}

std::vector<std::string> AssetStore::all_refs() const {
    std::vector<std::string> refs;
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::u8path(assets_dir_);
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code file_error;
        if (!it->is_regular_file(file_error)) continue;
        std::string digest;
        if (stored_digest(it->path().filename().u8string(), &digest))
            refs.push_back(std::string(kPrefix) + digest);
    }
    return refs;
}

size_t AssetStore::gc(const std::vector<std::string>& live_refs) const {
    std::unordered_set<std::string> live;
    for (const std::string& ref : live_refs)
        if (is_asset_ref(ref)) live.insert(parse_asset_ref(ref));

    size_t removed = 0;
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::u8path(assets_dir_);
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code file_error;
        if (!it->is_regular_file(file_error)) continue;
        std::string digest;
        if (!stored_digest(it->path().filename().u8string(), &digest) ||
            live.count(digest)) {
            continue;
        }
        if (std::filesystem::remove(it->path(), file_error) && !file_error)
            ++removed;
    }
    return removed;
}

}  // namespace project_assets
}  // namespace jade
