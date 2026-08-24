// Skeleton.cpp — see Skeleton.hpp. Faithful port of skeleton.py build_bone_nodes
// (bone-forest half).
#include "jade/Skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "jade/Gao.hpp"

namespace jade {

namespace {

constexpr uint32_t NO_FATHER = 0xFFFFFFFFu;

// _sane(vals): all finite && |v| < 1e6.
bool sane16(const float* v) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(v[i]) || std::fabs(v[i]) >= 1e6f) return false;
    }
    return true;
}

// Decode the leading 16 column-major floats from a 68-byte matrix blob.
void read16(const std::vector<uint8_t>& raw, float* out) {
    for (int i = 0; i < 16; ++i) {
        uint32_t bits = 0;
        std::memcpy(&bits, raw.data() + i * 4, 4);
        std::memcpy(&out[i], &bits, 4);
    }
}

}  // namespace

BoneForest build_bone_nodes(const std::vector<SubEntry>& subs) {
    BoneForest forest;
    if (subs.empty()) return forest;  // Python: not all_subs -> None

    // Set of all .gao keys (passed to parse_gao_full as all_gao_keys; the C++
    // parser ignores it but we mirror the gate exactly).
    std::unordered_set<uint32_t> all_gao_keys;
    for (const SubEntry& s : subs)
        if (s.ext == ".gao") all_gao_keys.insert(s.key);

    // gao_data: key -> parsed info, kept in walk (insertion) order for stable
    // sorting later. parse_gao_full returning ok=false mirrors Python None ->
    // not inserted.
    std::vector<uint32_t> order;                       // insertion order of keys
    std::unordered_map<uint32_t, GaoInfo> gao_data;
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo info = parse_gao_full(s.data.data(), s.data.size());
        if (!info.ok) continue;
        if (gao_data.find(s.key) == gao_data.end()) order.push_back(s.key);
        gao_data[s.key] = info;  // last write wins (matches Python dict assign)
    }
    if (gao_data.empty()) return forest;  // Python: not gao_data -> None

    // father_key as the Python sees it: present only when the hierarchy block
    // was read; otherwise treated as 0xFFFFFFFF (the .get default).
    auto father_of = [&](const GaoInfo& info) -> uint32_t {
        if (info.hier_read) return info.father_key;
        return NO_FATHER;
    };
    auto name_of = [&](uint32_t key) -> const std::string& {
        static const std::string empty;
        auto it = gao_data.find(key);
        return it == gao_data.end() ? empty : it->second.name;
    };

    // children_map (walk order) and root_keys (walk order).
    std::unordered_map<uint32_t, std::vector<uint32_t>> children_map;
    std::vector<uint32_t> root_keys;
    for (uint32_t key : order) {
        const GaoInfo& info = gao_data[key];
        uint32_t fk = father_of(info);
        if (fk != NO_FATHER && gao_data.find(fk) != gao_data.end())
            children_map[fk].push_back(key);
        if (fk == NO_FATHER || gao_data.find(fk) == gao_data.end())
            root_keys.push_back(key);
    }

    // bone_keys: any gao with Bone or Hierarchy identity flag.
    bool any_bone = false;
    for (uint32_t key : order) {
        uint32_t id = gao_data[key].identity;
        if (id & (GAO_FLAG_BONE | GAO_FLAG_HIERARCHY)) { any_bone = true; break; }
    }
    if (!any_bone) return forest;  // Python: not bone_keys -> None

    // Stable sort helper by gao name (matches Python's sorted(key=name), which
    // is stable; ties keep insertion order).
    auto by_name = [&](std::vector<uint32_t>& v) {
        std::stable_sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) {
            return name_of(a) < name_of(b);
        });
    };

    std::unordered_map<uint32_t, uint32_t> bone_node_indices;

    // add_bone: append a node for `key` if not yet present; return its index.
    auto add_bone = [&](uint32_t key) -> uint32_t {
        auto it = bone_node_indices.find(key);
        if (it != bone_node_indices.end()) return it->second;
        const GaoInfo& info = gao_data[key];
        uint32_t ni = static_cast<uint32_t>(forest.nodes.size());
        bone_node_indices[key] = ni;

        BoneNode node;
        node.key = key;
        // matrix: local preferred, else global; both _sane-gated.
        float m[16];
        if (info.lmat_present && info.lmat_raw.size() >= 68) {
            read16(info.lmat_raw, m);
            if (sane16(m)) { node.has_matrix = true; node.matrix.assign(m, m + 16); }
        }
        if (!node.has_matrix && info.gmat_present && info.gmat_raw.size() >= 68) {
            read16(info.gmat_raw, m);
            if (sane16(m)) { node.has_matrix = true; node.matrix.assign(m, m + 16); }
        }
        // name: gao name, ".gao" suffix stripped.
        //
        // NOTE: the Python is `name = info.get('name', f'bone_0x{key:08X}')`.
        // parse_gao_header ALWAYS sets info['name'] (to '' when the name slot is
        // empty/invalid), so the dict key is always present and the bone_0x
        // default NEVER fires. An empty parsed name therefore stays empty — it
        // does NOT become "bone_0x...". (Verified on prince_org.bf: node
        // 0x29001077 has name '' in Python.) Mirroring that here: no fallback.
        std::string name = info.name;
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".gao") == 0) {
            name = name.substr(0, name.size() - 4);
        }
        node.name = name;
        forest.nodes.push_back(std::move(node));
        return ni;
    };

    // add_tree: DFS; children sorted by name. Implemented with an explicit
    // recursion via std::function to mirror the Python closure.
    std::function<uint32_t(uint32_t)> add_tree = [&](uint32_t key) -> uint32_t {
        uint32_t ni = add_bone(key);
        std::vector<uint32_t> kids;
        auto cit = children_map.find(key);
        if (cit != children_map.end()) kids = cit->second;
        by_name(kids);
        for (uint32_t ck : kids) {
            uint32_t ci = add_tree(ck);
            forest.nodes[ni].children.push_back(ci);
        }
        return ni;
    };

    by_name(root_keys);
    for (uint32_t rk : root_keys) add_tree(rk);

    forest.ok = true;
    return forest;
}

}  // namespace jade
