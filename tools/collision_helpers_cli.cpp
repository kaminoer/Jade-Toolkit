// Canonical synthetic exercise of core/collision.py's reusable helper surface.
#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "jade/Collision.hpp"
#include "jade/SubEntry.hpp"

namespace {

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return out;
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 24));
}

}  // namespace

int main() {
    using namespace jade;
    const std::array<double, 3> lower{{-1.25, 2.5, -3.75}};
    const std::array<double, 3> upper{{4.5, 6.25, 8.0}};

    const CobProfileLookup simple_lookup = get_cob_profile("simple_box");
    const CobProfileLookup ledge_lookup = get_cob_profile("ledge_box");
    const CobProfileLookup open_lookup = get_cob_profile("ledge_openbox");
    if (!simple_lookup.ok || !ledge_lookup.ok || !open_lookup.ok) return 1;

    const std::vector<uint8_t> simple = build_cob_triangle_box(
        lower, upper, DEFAULT_COB_GAMEMAT_KEY, simple_lookup.profile);
    const std::vector<uint8_t> ledge = build_cob_triangle_box(
        lower, upper, DEFAULT_COB_GAMEMAT_KEY, ledge_lookup.profile);
    const std::vector<uint8_t> open = build_cob_triangle_box(
        lower, upper, 0xFFFFFFFFu, open_lookup.profile);
    const CobProfile game_profile = make_ledge_profile_for_game("WW_PS2");
    const std::vector<uint8_t> game = build_cob_triangle_box(
        lower, upper, DEFAULT_COB_GAMEMAT_KEY, game_profile);

    std::cout << "simple " << hex_bytes(simple) << '\n';
    std::cout << "ledge " << hex_bytes(ledge) << '\n';
    std::cout << "open_no_gmat " << hex_bytes(open) << '\n';
    std::cout << "game_name " << game_profile.name << '\n';
    std::cout << "game " << hex_bytes(game) << '\n';

    constexpr uint32_t cob_key = 0x7A000101u;
    constexpr uint32_t cob_key2 = 0x7A000102u;
    constexpr uint32_t colmap_key = 0x7A000201u;
    const std::vector<uint8_t> cob_record = make_cob_sub_entry(
        cob_key, DEFAULT_COB_GAMEMAT_KEY, simple);
    const std::vector<uint8_t> cob_record2 = make_cob_sub_entry(
        cob_key2, 0xFFFFFFFFu, simple);
    const std::vector<uint8_t> colmap_record =
        make_colmap_compact_sub_entry(colmap_key, cob_key);
    std::cout << "cob_record " << hex_bytes(cob_record) << '\n';
    std::cout << "colmap_record " << hex_bytes(colmap_record) << '\n';

    const std::vector<std::array<double, 3>> vertices{
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
    const std::vector<std::array<uint32_t, 3>> faces{{0, 1, 2}};
    const std::vector<uint8_t> extended_auto = extend_cob_triangle_mesh(
        simple.data(), simple.size(), vertices, faces, 65, 1);
    const std::vector<uint8_t> extended_custom = extend_cob_triangle_mesh(
        simple.data(), simple.size(), vertices, faces,
        std::vector<std::array<double, 3>>{{0.25, 0.5, 0.75}}, 65, 1);
    std::cout << "extend_auto " << hex_bytes(extended_auto) << '\n';
    std::cout << "extend_custom " << hex_bytes(extended_custom) << '\n';

    std::vector<uint8_t> stream = cob_record;
    stream.insert(stream.end(), cob_record2.begin(), cob_record2.end());
    stream.insert(stream.end(), colmap_record.begin(), colmap_record.end());
    std::vector<SubEntry> subs = walk_sub_entries(stream);
    if (subs.size() != 3) return 1;
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& sub : subs) by_key.emplace(sub.key, &sub);
    const std::vector<uint32_t> keys = colmap_cob_keys(&subs[2], by_key);
    std::cout << "classify cob=" << looks_like_cob_sub(&subs[0])
              << " colmap=" << is_colmap_sub(&subs[2], by_key)
              << " keys=";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << std::hex << keys[i] << std::dec;
    }
    std::cout << '\n';

    SubEntry truncated = subs[0];
    truncated.size = 9;
    std::cout << "truncated " << looks_like_cob_sub(&truncated) << '\n';
    subs[0].size = 100;
    subs[1].size = 200;
    const SubEntry* master = find_room_master_cob(subs);
    std::cout << "master " << std::hex << (master ? master->key : 0u)
              << std::dec << '\n';

    std::vector<uint8_t> gao{0xAA, 0xBB};
    put32(gao, colmap_key);
    gao.push_back(0xCC);
    const auto hits = gao_colmap_key_offsets(gao, by_key);
    std::cout << "offsets";
    for (const auto& hit : hits)
        std::cout << ' ' << hit.first << ':' << std::hex << hit.second << std::dec;
    std::cout << '\n';

    const CobInfo parsed = parse_cob(simple.data(), simple.size());
    const auto flat = cob_flat_faces(parsed);
    std::cout << "flat " << flat.size() << ' ' << flat.front()[0] << ','
              << flat.front()[1] << ',' << flat.front()[2] << ' '
              << flat.back()[0] << ',' << flat.back()[1] << ','
              << flat.back()[2] << '\n';
    return 0;
}
