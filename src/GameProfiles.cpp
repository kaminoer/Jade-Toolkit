// GameProfiles.cpp — implementation. Faithful port of profiles.py.
#include "jade/GameProfiles.hpp"

#include <algorithm>
#include <filesystem>

#include "jade/BigFile.hpp"

namespace jade {
namespace gameprofiles {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// True if the archive's directory holds >=2 PS2 mirror BFs (prince01..05.bf),
// a reliable PS2-platform signal (PC builds never carry them).
bool has_ps2_mirror_siblings(const std::string& archive_path) {
    namespace fs = std::filesystem;
    static const char* pat[] = {"prince01.bf","prince02.bf","prince03.bf",
                                "prince04.bf","prince05.bf"};
    std::error_code ec;
    fs::path dir = fs::path(archive_path).parent_path();
    if (dir.empty()) dir = ".";
    int matches = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string nm = lower(it->path().filename().string());
        for (const char* p : pat) if (nm == p) { ++matches; break; }
    }
    return matches >= 2;
}

uint32_t bf_version(const std::string& path) {
    try {
        BigFile bf; bf.open(path);
        return bf.version;
    } catch (...) {
        return 0;
    }
}

}  // namespace

const std::vector<GameProfile>& all() {
    static const std::vector<GameProfile> profiles = {
        {"SoT", "Prince of Persia: Sands of Time", 37, {"prince.bf"},
         "Sands of Time", "PC", false, false},
        {"WW", "Prince of Persia: Warrior Within", 38,
         {"prince.bf", "prince_3lol.bf", "prince20thann.bf"},
         "Warrior Within", "PC", false, false},
        {"T2T", "Prince of Persia: The Two Thrones", 38, {"pop3.bf"},
         "The Two Thrones", "PC", false, false},
        {"WW_PS2", "Prince of Persia: Warrior Within (PS2)", 38, {"prince.bf"},
         "PS2_WW", "PS2", true, true},
    };
    return profiles;
}

const GameProfile* get(const std::string& code) {
    for (const GameProfile& p : all())
        if (p.code == code) return &p;
    return nullptr;
}

const GameProfile* detect(const std::string& archive_path) {
    namespace fs = std::filesystem;
    std::string name = lower(fs::path(archive_path).filename().string());
    std::string parent = lower(fs::path(archive_path).parent_path().string());

    auto by_code = [](const char* c) { return get(c); };

    // PS2 platform check first — both PS2 and PC WW name the archive prince.bf,
    // only PS2 carries the numbered mirror siblings.
    if (name == "prince.bf" && has_ps2_mirror_siblings(archive_path))
        return by_code("WW_PS2");

    if (parent.find("ps2_ww") != std::string::npos ||
        (parent.find("ps2") != std::string::npos &&
         parent.find("warrior") != std::string::npos)) {
        const GameProfile* p = by_code("WW_PS2");
        for (const std::string& fn : p->archive_filenames)
            if (name == fn) return p;
    }

    // Install-folder match (disambiguates the shared prince.bf name).
    for (const GameProfile& p : all()) {
        if (p.platform != "PC") continue;
        if (!p.game_dir_substr.empty() &&
            parent.find(lower(p.game_dir_substr)) != std::string::npos)
            return &p;
    }

    // prince.bf outside a known folder: disambiguate SoT (v37) vs WW (v38).
    if (name == "prince.bf") {
        uint32_t v = bf_version(archive_path);
        if (v == 0) return nullptr;
        return by_code(v == 37 ? "SoT" : "WW");
    }

    // Other filenames.
    for (const GameProfile& p : all()) {
        if (p.platform != "PC") continue;
        for (const std::string& fn : p.archive_filenames)
            if (name == fn) return &p;
    }

    // Last resort: BF version (37 unambiguous; 38 shared, so unknown).
    uint32_t v = bf_version(archive_path);
    if (v == 37) return by_code("SoT");
    return nullptr;
}

}  // namespace gameprofiles
}  // namespace jade
