// RegisterResource.cpp - offline registration-sidecar port.
#include "jade/RegisterResource.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "jade/Gao.hpp"
#include "jade/Geometry.hpp"
#include "jade/Material.hpp"
#include "jade/Texture.hpp"
#include "jade/WowStream.hpp"

namespace jade {
namespace register_resource {
namespace {

uint32_t rd32(const uint8_t* d, size_t o) {
    return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) |
           (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24);
}

void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    d[o] = uint8_t(v);
    d[o + 1] = uint8_t(v >> 8);
    d[o + 2] = uint8_t(v >> 16);
    d[o + 3] = uint8_t(v >> 24);
}

void append32(std::vector<uint8_t>& d, uint32_t v) {
    d.push_back(uint8_t(v));
    d.push_back(uint8_t(v >> 8));
    d.push_back(uint8_t(v >> 16));
    d.push_back(uint8_t(v >> 24));
}

uint32_t type_of(const SubEntry& entry) {
    if (!entry.ext.empty()) {
        uint32_t v = 0;
        for (size_t i = 0; i < std::min<size_t>(4, entry.ext.size()); ++i)
            v |= uint32_t(uint8_t(entry.ext[i])) << (8 * i);
        return v;
    }
    return entry.gro_type;
}

std::pair<std::vector<uint8_t>, int> rekey_stream(
    const std::vector<uint8_t>& data,
    const std::unordered_map<uint32_t, uint32_t>& key_map) {
    std::vector<uint8_t> out = data;
    int count = 0;
    size_t i = 0;
    while (i + 4 <= out.size()) {
        uint32_t value = rd32(out.data(), i);
        auto it = key_map.find(value);
        if (it == key_map.end()) {
            ++i;
            continue;
        }
        put32(out, i, it->second);
        ++count;
        i += 4;
    }
    return {std::move(out), count};
}

std::unordered_set<uint32_t> inwow_refs(
    const std::vector<uint8_t>& payload,
    const std::unordered_map<uint32_t, const SubEntry*>& by) {
    std::unordered_set<uint32_t> refs;
    for (size_t o = 0; o + 4 <= payload.size(); ++o) {
        uint32_t value = rd32(payload.data(), o);
        if (by.count(value)) refs.insert(value);
    }
    return refs;
}

bool keylike(uint32_t value) {
    return value != 0 && value != GRID_NULL &&
           (value >> 24) >= 0x01 && (value >> 24) <= 0xFE &&
           (value & 0x00F00000u) == 0;
}

std::optional<uint32_t> find_gog_key(
    const std::vector<SubEntry>& subs,
    const std::unordered_set<uint32_t>& gao_keys) {
    const size_t stop = std::min<size_t>(12, subs.size());
    for (size_t i = 0; i < stop; ++i) {
        const SubEntry& sub = subs[i];
        if (sub.ext == ".wow" || sub.data.size() < 4) continue;
        size_t count = gao_keys.count(sub.gro_type) ? 1 : 0;
        const size_t words = sub.data.size() / 4;
        for (size_t word = 0; word < words; ++word)
            if (gao_keys.count(rd32(sub.data.data(), word * 4))) ++count;
        if (count >= 2) return sub.key;
    }
    return std::nullopt;
}

std::vector<uint8_t> patch_wow_name(const std::vector<uint8_t>& data,
                                    const std::string& name) {
    std::vector<uint8_t> out = data;
    if (out.size() < 40) return out;
    std::string ascii;
    ascii.reserve(std::min<size_t>(31, name.size()));
    for (unsigned char c : name)
        if (c < 0x80 && ascii.size() < 31) ascii.push_back(char(c));
    std::fill(out.begin() + 8, out.begin() + 40, 0);
    std::copy(ascii.begin(), ascii.end(), out.begin() + 8);
    return out;
}

std::vector<uint8_t> zero_element_matids(const std::vector<uint8_t>& data) {
    // Exact live-Python behavior: _zero_element_matids asks each parsed
    // element dict for `matId_offset`, but geometry.parse_geometry currently
    // exposes only the aggregate `elements_offset`. Consequently no element is
    // rewritten. Keep this no-op until both implementations deliberately fix
    // that dormant cleanup path together.
    (void)parse_geometry(data.data(), data.size());
    return data;
}

std::string keys_repr(const std::vector<uint32_t>& keys) {
    std::string out = "[";
    char b[24];
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) out += ", ";
        std::snprintf(b, sizeof b, "'0x%x'", keys[i]);
        out += b;
    }
    out += "]";
    return out;
}

struct WalkDiagnostic {
    std::string error;
    size_t trailing = 0;
};

WalkDiagnostic diagnose_walk(const std::vector<uint8_t>& dec) {
    WalkDiagnostic result;
    long long first = find_first_record(dec.data(), dec.size());
    if (first < 0) {
        result.error = "no record header found";
        result.trailing = dec.size();
        return result;
    }
    size_t off = size_t(first);
    while (off + 12 <= dec.size()) {
        uint32_t size = rd32(dec.data(), off);
        uint32_t magic = rd32(dec.data(), off + 4);
        if (magic != WOW_MAGIC) {
            char b[96];
            std::snprintf(b, sizeof b, "bad magic 0x%08x at offset 0x%zx",
                          magic, off);
            result.error = b;
            break;
        }
        if (off + 12 + size > dec.size()) {
            char b[96];
            std::snprintf(b, sizeof b,
                          "size overrun (%u) at offset 0x%zx", size, off);
            result.error = b;
            break;
        }
        off += 12 + size;
    }
    result.trailing = dec.size() - off;
    return result;
}

}  // namespace

std::vector<uint8_t> make_record(uint32_t key,
                                 std::optional<uint32_t> type_u32,
                                 const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    if (!type_u32.has_value()) {
        out.reserve(12);
        append32(out, 0);
        append32(out, JADE_COOKIE);
        append32(out, key);
        return out;
    }
    out.reserve(16 + data.size());
    append32(out, uint32_t(4 + data.size()));
    append32(out, JADE_COOKIE);
    append32(out, key);
    append32(out, *type_u32);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<uint8_t> record_bytes(const std::vector<uint8_t>& dec,
                                  const SubEntry& entry) {
    if (entry.offset < 4) return {};
    const size_t start = entry.offset - 4;
    const size_t end = start + 12 + entry.size;
    if (end > dec.size()) return {};
    return std::vector<uint8_t>(dec.begin() + long(start),
                                dec.begin() + long(end));
}

std::optional<DonorClosure> select_donor(
    const std::vector<SubEntry>& subs,
    const std::set<uint8_t>& valid_prefixes) {
    (void)valid_prefixes;  // Python currently computes this union but does not use it.
    std::unordered_map<uint32_t, const SubEntry*> by;
    std::unordered_set<uint32_t> gao_keys;
    for (const SubEntry& sub : subs) {
        by[sub.key] = &sub;  // dict comprehension: final duplicate wins
        if (sub.ext == ".gao") gao_keys.insert(sub.key);
    }
    auto full_present = [&](uint32_t key) {
        for (const SubEntry& sub : subs) {
            if (sub.key != key ||
                !is_texture_entry(sub.data.data(), sub.data.size()))
                continue;
            TexInfo ti = parse_texture(sub.data.data(), sub.data.size());
            if (ti.valid &&
                !is_placeholder(ti, sub.data.size() -
                                         std::min(sub.data.size(), ti.pix_start)))
                return true;
        }
        return false;
    };
    auto stub_present = [&](uint32_t key) {
        for (const SubEntry& sub : subs) {
            if (sub.key != key ||
                !is_texture_entry(sub.data.data(), sub.data.size()))
                continue;
            TexInfo ti = parse_texture(sub.data.data(), sub.data.size());
            if (ti.valid &&
                is_placeholder(ti, sub.data.size() -
                                        std::min(sub.data.size(), ti.pix_start)))
                return true;
        }
        return false;
    };

    std::optional<DonorClosure> best;
    size_t best_cost = 0;
    for (const SubEntry& sub : subs) {
        if (sub.ext != ".gao" || sub.data.size() < 12) continue;
        if ((rd32(sub.data.data(), 8) & BAD_GAO_FLAGS) != 0) continue;
        GaoInfo info = parse_gao_full(sub.data.data(), sub.data.size());
        if (!info.ok || !info.vis_read || info.gro_key == 0 ||
            info.grm_key == 0 || !by.count(info.gro_key) ||
            !by.count(info.grm_key))
            continue;
        std::unordered_set<uint32_t> refs = inwow_refs(sub.data, by);
        refs.erase(info.gro_key);
        refs.erase(info.grm_key);
        refs.erase(sub.key);
        if (!refs.empty()) continue;

        const SubEntry* material = by.at(info.grm_key);
        if (material->gro_null ||
            material->gro_type != uint32_t(GRO_TYPE_MAT_MULTI))
            continue;
        MatInfo multi = parse_material(material->data.data(),
                                       material->data.size(),
                                       GRO_TYPE_MAT_MULTI);
        if (!multi.ok) continue;
        uint32_t mat_kind = material->data.size() >= 4
                                ? rd32(material->data.data(), 0) : 0;
        for (uint32_t submat_key : multi.sub_material_keys) {
            if (submat_key == 0 || submat_key == GRID_NULL ||
                !by.count(submat_key))
                continue;
            const SubEntry* submat = by.at(submat_key);
            if (submat->data.size() < 4 ||
                texture_layer_offset(submat->data.data(),
                                     submat->data.size(), 0) < 0)
                continue;
            uint32_t texture_key = resolve_texture_key(
                submat->data.data(), submat->data.size());
            if (texture_key != 0 && full_present(texture_key) &&
                stub_present(texture_key)) {
                size_t cost = by.at(info.gro_key)->data.size();
                if (!best.has_value() || cost < best_cost) {
                    DonorClosure donor;
                    donor.gao_key = sub.key;
                    donor.geo_key = info.gro_key;
                    donor.mat_key = info.grm_key;
                    donor.submat_key = submat_key;
                    donor.submat_kind = rd32(submat->data.data(), 0);
                    donor.tex_key = texture_key;
                    donor.mat_data_kind = mat_kind;
                    best = donor;
                    best_cost = cost;
                }
                break;
            }
        }
    }
    return best;
}

SidecarBuild build_sidecar(const std::vector<uint8_t>& donor_dec,
                           const SidecarKeys& keys,
                           const std::string& name,
                           const std::vector<uint8_t>& k_new_stub,
                           const std::vector<uint8_t>& k_new_full,
                           const std::set<uint8_t>& valid_prefixes) {
    std::vector<SubEntry> subs = walk_sub_entries(donor_dec);
    std::unordered_map<uint32_t, const SubEntry*> by;
    std::unordered_set<uint32_t> gao_keys;
    for (const SubEntry& sub : subs) {
        by[sub.key] = &sub;
        if (sub.ext == ".gao") gao_keys.insert(sub.key);
    }
    std::optional<DonorClosure> selected =
        select_donor(subs, valid_prefixes);
    if (!selected.has_value())
        throw std::runtime_error(
            "no suitable donor GAO closure (refs-only-geo+mat) found");
    const DonorClosure donor = *selected;

    SidecarBuild build;
    build.keys = keys;
    build.donor = donor;
    build.name = name;
    char b[256];
    std::snprintf(
        b, sizeof b,
        "donor GAO=0x%08X GEO=0x%08X MAT=0x%08X SUBMAT=0x%08X(kind %u) "
        "TEX=0x%08X",
        donor.gao_key, donor.geo_key, donor.mat_key, donor.submat_key,
        donor.submat_kind, donor.tex_key);
    build.report.push_back(b);

    if (subs.empty() || subs.front().ext != ".wow")
        throw std::runtime_error("donor record0 is not .wow");
    const SubEntry& r0 = subs.front();
    const uint32_t wow_self = r0.key;
    std::optional<uint32_t> gog_old = find_gog_key(subs, gao_keys);
    if (!gog_old.has_value())
        throw std::runtime_error("could not locate donor GameObjectGroup record");
    std::snprintf(b, sizeof b, "donor wow=0x%08X GOG=0x%08X", wow_self,
                  *gog_old);
    build.report.push_back(b);

    std::unordered_map<uint32_t, uint32_t> r0_map = {
        {wow_self, keys.wow}, {*gog_old, keys.gog}};
    for (size_t off = 0; off + 4 <= r0.data.size(); off += 4) {
        uint32_t value = rd32(r0.data.data(), off);
        if (keylike(value) && value != wow_self && value != *gog_old)
            r0_map[value] = GRID_NULL;
    }
    auto r0_rekeyed = rekey_stream(r0.data, r0_map);
    size_t nulled = 0;
    for (const auto& item : r0_map)
        if (item.second == GRID_NULL) ++nulled;
    std::snprintf(b, sizeof b, "record0: nulled %zu sub-resource slot key(s)",
                  nulled);
    build.report.push_back(b);
    std::vector<uint8_t> r0_payload = patch_wow_name(r0_rekeyed.first, name);

    std::vector<uint8_t> rec_wow =
        make_record(keys.wow, type_of(r0), r0_payload);
    std::vector<uint8_t> rec_gog =
        make_record(keys.gog, keys.gao, {});

    std::unordered_map<uint32_t, uint32_t> gao_map = {
        {donor.gao_key, keys.gao}, {donor.geo_key, keys.geo},
        {donor.mat_key, keys.mat}};
    std::vector<uint8_t> gao_payload =
        rekey_stream(by.at(donor.gao_key)->data, gao_map).first;
    std::vector<uint8_t> rec_gao = make_record(
        keys.gao, type_of(*by.at(donor.gao_key)), gao_payload);

    std::vector<uint8_t> geo_payload = rekey_stream(
        by.at(donor.geo_key)->data, {{donor.geo_key, keys.geo}}).first;
    geo_payload = zero_element_matids(geo_payload);
    std::vector<uint8_t> rec_geo = make_record(
        keys.geo, type_of(*by.at(donor.geo_key)), geo_payload);

    std::vector<uint8_t> mat_data;
    append32(mat_data, donor.mat_data_kind);
    append32(mat_data, 1);
    append32(mat_data, keys.submat);
    std::vector<uint8_t> rec_mat = make_record(
        keys.mat, uint32_t(GRO_TYPE_MAT_MULTI), mat_data);

    const SubEntry* source_sm = by.at(donor.submat_key);
    std::vector<uint8_t> sm_payload = rekey_stream(
        source_sm->data, {{donor.submat_key, keys.submat}}).first;
    sm_payload = set_texture_key(sm_payload.data(), sm_payload.size(), keys.tex);
    if (sm_payload.empty())
        throw std::runtime_error("set_texture_key failed on donor submaterial");
    std::vector<uint8_t> rec_sm =
        make_record(keys.submat, type_of(*source_sm), sm_payload);
    std::vector<uint8_t> rec_tex_stub =
        make_record(keys.tex, keys.tex, k_new_stub);
    std::vector<uint8_t> rec_tex_full =
        make_record(keys.tex, keys.tex, k_new_full);

    for (const std::vector<uint8_t>* record :
         {&rec_wow, &rec_gog, &rec_gao, &rec_mat, &rec_geo, &rec_sm,
          &rec_tex_stub, &rec_tex_full})
        build.dec.insert(build.dec.end(), record->begin(), record->end());
    std::snprintf(
        b, sizeof b,
        "assembled 8 records, %zu bytes (order GAO,MAT,GEO,submat,tex)",
        build.dec.size());
    build.report.push_back(b);
    return build;
}

std::vector<std::string> validate_sidecar(const SidecarBuild& build) {
    std::vector<std::string> problems;
    WalkDiagnostic diag = diagnose_walk(build.dec);
    if (!diag.error.empty()) problems.push_back("walk error: " + diag.error);
    if (diag.trailing != 0)
        problems.push_back("trailing " + std::to_string(diag.trailing) +
                           " bytes after last record");

    std::vector<SubEntry> recs = walk_sub_entries(build.dec);
    std::vector<uint32_t> actual;
    for (const SubEntry& rec : recs) actual.push_back(rec.key);
    const SidecarKeys& k = build.keys;
    std::vector<uint32_t> expected = {
        k.wow, k.gog, k.gao, k.mat, k.geo, k.submat, k.tex, k.tex};
    if (actual != expected)
        problems.push_back("record key order " + keys_repr(actual) +
                           " != expected " + keys_repr(expected));
    if (recs.empty() || recs.front().ext != ".wow")
        problems.push_back("record0 is not .wow");
    if (!recs.empty()) {
        std::set<uint32_t> stray;
        const std::vector<uint8_t>& data = recs.front().data;
        for (size_t off = 0; off + 4 <= data.size(); off += 4) {
            uint32_t value = rd32(data.data(), off);
            if (keylike(value) && value != k.wow && value != k.gog)
                stray.insert(value);
        }
        if (!stray.empty())
            problems.push_back(
                "record0 has un-nulled sub-resource slot(s): " +
                keys_repr(std::vector<uint32_t>(stray.begin(), stray.end())));
    }

    const SubEntry* gog = nullptr;
    const SubEntry* mat = nullptr;
    const SubEntry* sm = nullptr;
    for (const SubEntry& rec : recs) {
        if (!gog && rec.key == k.gog) gog = &rec;
        if (!mat && rec.key == k.mat) mat = &rec;
        if (!sm && rec.key == k.submat) sm = &rec;
    }
    if (gog == nullptr || gog->gro_type != k.gao)
        problems.push_back("GOG does not reference the GAO key");
    if (mat != nullptr) {
        MatInfo multi = parse_material(mat->data.data(), mat->data.size(),
                                       GRO_TYPE_MAT_MULTI);
        if (!multi.ok || multi.sub_material_keys !=
                             std::vector<uint32_t>{k.submat}) {
            problems.push_back(
                "material submat list != [submat]: " +
                (multi.ok ? keys_repr(multi.sub_material_keys) : "None"));
        }
    }
    if (sm != nullptr &&
        resolve_texture_key(sm->data.data(), sm->data.size()) != k.tex)
        problems.push_back("submat texture pointer != K_new");
    return problems;
}

}  // namespace register_resource
}  // namespace jade
