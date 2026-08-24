// charswap_cli — apply a character-pack manifest to a BF entry.
//   charswap_cli <bf> <manifest.json>
// Manifest = the toolkit's regular mod.json schema. Image paths are decoded by
// the shared native image loader; no native-only rgba/w/h fields are needed.
// No .bak sidecar by default (harnesses run on scratch copies).
#include <cstdio>
#include <string>

#include "jade/CharacterSwap.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade;

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc == 3 && std::string(argv[1]) == "--check-manifest") {
        charswap::ManifestResult manifest = charswap::load_manifest(argv[2]);
        if (!manifest.ok) {
            std::printf("ERROR %s\n", manifest.error.c_str());
            return 0;
        }
        std::printf("MANIFEST entry=%u meshes=%zu textures=%zu stubs=%zu\n",
                    manifest.spec.bf_entry, manifest.spec.meshes.size(),
                    manifest.spec.textures.size(), manifest.spec.stubs.size());
        return 0;
    }
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: charswap_cli <bf> <manifest.json> | "
                             "--check-manifest <manifest.json>\n");
        return 2;
    }
    const bool backup = argc == 4 && std::string(argv[3]) == "--backup";
    if (argc == 4 && !backup) {
        std::fprintf(stderr, "ERROR unknown option: %s\n", argv[3]);
        return 2;
    }
    charswap::SwapResult r = charswap::apply_character_swap(
        argv[1], argv[2], backup,
        [](const std::string& line) { std::printf("%s\n", line.c_str()); });
    if (!r.ok) { std::printf("ERROR %s\n", r.error.c_str()); return 0; }
    std::printf("CHARSWAP entry=%u meshes=%u textures=%u stubs=%u dec=%zu\n",
                r.bf_entry, r.meshes_applied, r.textures_applied, r.stubs_applied,
                r.decompressed_size);
    return 0;
}
