// GameProfiles.hpp — per-game identity + archive detection.
//
// Port of jade_explorer/gameprofiles/profiles.py. The .jtmod engine uses
// detect() to identify the game an archive belongs to (so it can refuse a
// cross-game apply, e.g. a SoT mod onto T2T's pop3.bf). Filename alone can't
// identify the game — SoT, WW and WW_PS2 all ship as prince.bf — so detection
// also uses the BigFile version (37=SoT, 38=WW/T2T) and the install folder.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jade {
namespace gameprofiles {

struct GameProfile {
    std::string code;                          // "SoT" | "WW" | "T2T" | "WW_PS2"
    std::string name;                          // display name
    uint32_t bf_version = 0;                   // BigFile header version
    std::vector<std::string> archive_filenames;  // canonical names (lowercase)
    std::string game_dir_substr;               // substring of the install folder
    std::string platform = "PC";               // "PC" | "PS2"
    bool paletted_textures = false;             // PS2 PAL8; PC uses DXT
    bool ps2_geo_format = false;                // PS2 VIF-packet GEO layout
};

// All known profiles (SoT, WW, T2T, WW_PS2).
const std::vector<GameProfile>& all();

// Profile for a code, or nullptr if unknown.
const GameProfile* get(const std::string& code);

// Best-effort detection from an archive path (filename + sibling files + BF
// version + install folder). Returns nullptr if unknown.
const GameProfile* detect(const std::string& archive_path);

}  // namespace gameprofiles
}  // namespace jade
