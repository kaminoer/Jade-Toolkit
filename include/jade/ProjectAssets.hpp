// ProjectAssets.hpp - content-addressed .jmod asset store.
// Ports jade_explorer/project/assets.py without Qt dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jade {
namespace project_assets {

bool is_asset_ref(const std::string& value);
std::string parse_asset_ref(const std::string& ref);

class AssetStore {
public:
    explicit AssetStore(std::string assets_dir);

    const std::string& directory() const { return assets_dir_; }

    // Copy a file into the store. The source extension is lower-cased, while
    // the content SHA-256 is the canonical reference identity.
    std::string add(const std::string& source_path) const;

    // Store caller-produced bytes. Like Python, ext is appended verbatim.
    std::string add_bytes(const std::vector<uint8_t>& data,
                          const std::string& ext = "") const;

    // Resolve raises when ref is malformed or absent; exists never raises.
    std::string resolve(const std::string& ref) const;
    bool exists(const std::string& ref) const;

    // Delete stored digest files not referenced by live_refs. Invalid refs
    // are ignored, and deletion failures do not contribute to the count.
    size_t gc(const std::vector<std::string>& live_refs) const;
    std::vector<std::string> all_refs() const;

private:
    std::string find(const std::string& digest) const;
    std::string assets_dir_;
};

}  // namespace project_assets
}  // namespace jade
