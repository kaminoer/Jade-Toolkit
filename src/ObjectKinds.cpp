// ObjectKinds.cpp — implementation. Faithful port of core/object_kinds.py.
//
// The classification regexes are transcribed verbatim from the Python (raw
// strings, case-insensitive) and matched with std::regex (ECMAScript), which
// supports the same constructs the Python uses (\b, (?:^|_), lookahead, \d).
// Divergence from Python `re` would show up in the golden diff.
#include "jade/ObjectKinds.hpp"

#include <regex>

namespace jade {

namespace {

constexpr uint32_t ID_GROUP = 0x00800000;
constexpr uint32_t ID_SOUND = 0x08000000;

const std::regex BONE_RX(
    R"RX(\b(pelvis|spine\d*|clavicle|forearm|upperarm|thigh|calf|foot|hand|neck|head|finger\d*|toe\d*|metacarpal|knee|ankle|twist|roll|bip\b|l thigh|r thigh|l calf|r calf|footsteps)\b)RX",
    std::regex::icase);
const std::regex CAMERA_RX(
    R"RX((?:^|_)cam(?=[^a-zA-Z]|cut|alt|era|man|target|corder)|(?:^|_)cin[_\d]|cutman|camtarget)RX",
    std::regex::icase);
const std::regex SOUND_RX(
    R"RX((?:^|_)snd[_\d]|_sound|soundbank|sound_?actor|soundtrigger|reverb|musique|music|_dial|fireplace)RX",
    std::regex::icase);
const std::regex LIGHT_RX(
    R"RX((?:^|_)lum[_\d]|_light|lightbeam|_fog|ambient|ambiant|(?:^|_)omni|_sun\b)RX",
    std::regex::icase);
const std::regex TRAP_RX(
    R"RX(trp_|traps?_|blade|shredder|spike|crush|hacksaw|\bsaw\b|crunchblock|spinblade|impale|sawblade|deathtrap|hurttrap)RX",
    std::regex::icase);
const std::regex FX_RX(
    R"RX((?:^|_)gfx_|(?:^|_)sfx_|(?:^|_)fx_|par_|_particle|particle view|smoke|glow|spark|flame|\bfire\b|dust|splash|debris)RX",
    std::regex::icase);
const std::regex TRIGGER_RX(
    R"RX(portal|_sct_|sector|checkpoint|savepoint|trigger|_zone|warpref|_tec_|teleport|activation|desactivation|ini_pos|init_pos)RX",
    std::regex::icase);
const std::regex SPAWN_RX(
    R"RX(spawn|respawn|generator|_ai_|enemies|ennemis|_sdk\b|guy_)RX",
    std::regex::icase);
const std::regex ACTOR_RX(
    R"RX((?:^|_)act_|^m_|_guard|_indian|_bat\d|amir|twins|enemy|enemi|boss)RX",
    std::regex::icase);
const std::regex WAYPOINT_RX(
    R"RX(waypoint|_path\b|(?:^|_)net_|_wp\d|_swp\d|patrol|_node\b)RX",
    std::regex::icase);
const std::regex LOGIC_RX(
    R"RX(manager|_master\b|(?:^|_)gp\d|_grp|group|tut_|tutorial|actionkit|global)RX",
    std::regex::icase);
const std::regex ROOTMASTER_RX(R"RX(root|master)RX", std::regex::icase);

inline bool S(const std::regex& rx, const std::string& n) {
    return std::regex_search(n, rx);
}

}  // namespace

std::string classify_object(const std::string& name, uint32_t identity,
                            int gro_type, bool father_in_bin) {
    (void)father_in_bin;  // parity with the Python signature; only is_bone uses it
    const std::string& n = name;
    if (gro_type == GRO_CAMERA || S(CAMERA_RX, n)) return "camera";
    if (gro_type == GRO_FX || S(FX_RX, n)) return "fx";  // FX before sound/light
    if ((identity & ID_SOUND) || S(SOUND_RX, n)) return "sound";
    if (S(LIGHT_RX, n)) return "light";
    if (S(TRAP_RX, n)) return "trap";
    if (S(SPAWN_RX, n)) return "spawner";
    if (S(TRIGGER_RX, n)) return "trigger";
    if (S(ACTOR_RX, n)) return "actor";
    if (S(WAYPOINT_RX, n)) return "waypoint";
    if ((identity & ID_GROUP) || S(LOGIC_RX, n)) return "logic";
    return "other";
}

bool is_bone(const std::string& name, uint32_t identity, bool father_in_bin) {
    (void)identity;  // parity with the Python signature
    const std::string& n = name;
    if (S(BONE_RX, n)) return true;
    // "b_<actor> <joint>" children parented to another in-bin GAO, excluding
    // the rig's own root/master node.
    if (n.size() >= 2 && (n[0] == 'b' || n[0] == 'B') && n[1] == '_' &&
        father_in_bin && !S(ROOTMASTER_RX, n))
        return true;
    return false;
}

}  // namespace jade
