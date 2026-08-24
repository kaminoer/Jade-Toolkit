// placer_apply — run apply_placements_to_dec on a BF entry (for the harness).
//
//   placer_apply <file.bf> <entry_idx> <seed> <ops.json> [patched_dec_out]
//   placer_apply <file.bf> plan <out.bf> <entry_idx> <ops.json> [strict 0|1]
//
// ops.json: {"ops":[{"kind":"cube","name":"...","position":[x,y,z],
//   "size":[..],"material_key":N,"rotation":[..],"scale":[..],
//   "source_key":N,"vertex_color":[r,g,b(,a)],"world_matrix":[16]}...]}
// Default mode prints the comparable result: patched dec len+CRC,
// per-addition key/len/CRC, world-list key + registered count. Plan mode runs
// the BF-level apply_placement_plan (copy + write + size.grs) and prints a
// PLAN stats line. Compared against the REAL Python by
// tests/placement_check.py.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Crc32.hpp"
#include "jade/Jgao.hpp"
#include "jade/Json.hpp"
#include "jade/ObjectPlacer.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade;
using namespace jade::placer;

static Vec3 vec3_of(const json::Value* v, Vec3 def) {
    if (!v || !v->is_arr()) return def;
    Vec3 out = def;
    for (size_t i = 0; i < 3 && i < v->arr.size(); ++i) out[i] = v->arr[i].num;
    return out;
}

static std::vector<PlaceOp> parse_ops(const char* json_path, bool& ok);

// Fill cross-bin clone fields: a clone op carrying source_entry_index +
// source_gao_key gets its source_gao_data + resource chain collected natively
// (the Python side embeds the same data via AddObject.apply's BFS walk).
static void collect_xbin_sources(BigFile& bf, std::vector<PlaceOp>& ops) {
    for (PlaceOp& op : ops) {
        if (op.kind != "clone" || !op.has_source_entry_index || !op.has_source_gao_key)
            continue;
        XbinSource xs = collect_xbin_source_chain(bf, op.source_entry_index,
                                                  op.source_gao_key);
        if (!xs.ok) {
            std::printf("ERROR %s\n", xs.error.c_str());
            std::exit(0);
        }
        op.has_source_gao_data = true;
        op.source_gao_data = std::move(xs.gao_data);
        op.source_resource_chain = std::move(xs.chain);
    }
}

// xchain/vchain modes: print the build-BFS or validation-LIFO collector
// digest for direct Python parity checks.
static int run_xchain(char** argv, bool validation) {
    uint32_t entry = uint32_t(std::strtoul(argv[3], nullptr, 10));
    uint32_t gao_key = uint32_t(std::strtoul(argv[4], nullptr, 16));
    BigFile bf;
    try { bf.open(argv[1]); } catch (const std::exception& e) {
        std::fprintf(stderr, "placer_apply: %s\n", e.what());
        return 1;
    }
    XbinSource xs = validation
        ? collect_xbin_validation_source_chain(bf, entry, gao_key)
        : collect_xbin_source_chain(bf, entry, gao_key);
    if (!xs.ok) { std::printf("ERROR %s\n", xs.error.c_str()); return 0; }
    std::printf("GAO len=%zu crc=%08x\n", xs.gao_data.size(),
                crc32(xs.gao_data.data(), xs.gao_data.size()));
    for (const ChainEntry& ce : xs.chain) {
        if (ce.has_gro_type)
            std::printf("CH %08x gt=%u len=%zu crc=%08x\n", ce.key, ce.gro_type,
                        ce.data.size(), crc32(ce.data.data(), ce.data.size()));
        else
            std::printf("CH %08x ext=%s len=%zu crc=%08x\n", ce.key, ce.ext.c_str(),
                        ce.data.size(), crc32(ce.data.data(), ce.data.size()));
    }
    return 0;
}

// binrep mode: role-matching visual transplant between two entries.
static int run_binrep(int argc, char** argv) {
    uint32_t tgt = uint32_t(std::strtoul(argv[4], nullptr, 10));
    uint32_t src = uint32_t(std::strtoul(argv[5], nullptr, 10));
    bool strict = argc > 6 ? (std::strtoul(argv[6], nullptr, 10) != 0) : true;
    PlacementSummary s = apply_bin_replacement(argv[1], argv[3], tgt, src, strict);
    if (!s.ok) { std::printf("ERROR %s\n", s.error.c_str()); return 0; }
    std::printf("BINREP entry=%u orig=%u new=%u inplace=%d replaced=%u\n",
                s.entry_index, s.original_compressed_size, s.new_compressed_size,
                s.wrote_in_place ? 1 : 0, s.meshes_replaced);
    return 0;
}

static int run_plan(int argc, char** argv) {
    uint32_t entry = uint32_t(std::strtoul(argv[4], nullptr, 10));
    bool strict = argc > 6 ? (std::strtoul(argv[6], nullptr, 10) != 0) : true;
    bool ok = false;
    std::vector<PlaceOp> ops = parse_ops(argv[5], ok);
    if (!ok) return 2;
    {
        BigFile bf;
        try { bf.open(argv[1]); } catch (const std::exception& e) {
            std::fprintf(stderr, "placer_apply: %s\n", e.what());
            return 1;
        }
        collect_xbin_sources(bf, ops);
    }
    PlacementSummary s = apply_placement_plan(argv[1], argv[3], entry, ops, strict);
    if (!s.ok) { std::printf("ERROR %s\n", s.error.c_str()); return 0; }
    std::printf("PLAN entry=%u added=%u subs=%u reg=%u orig=%u new=%u "
                "inplace=%d wl=%08x replaced=%u\n",
                s.entry_index, s.objects_added, s.sub_entries_added,
                s.objects_registered, s.original_compressed_size,
                s.new_compressed_size, s.wrote_in_place ? 1 : 0,
                s.has_world_list_key ? s.world_list_key : 0, s.meshes_replaced);
    return 0;
}

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc >= 6 && std::string(argv[2]) == "plan") return run_plan(argc, argv);
    if (argc >= 5 && std::string(argv[2]) == "xchain")
        return run_xchain(argv, false);
    if (argc >= 5 && std::string(argv[2]) == "vchain")
        return run_xchain(argv, true);
    if (argc >= 6 && std::string(argv[2]) == "binrep") return run_binrep(argc, argv);
    if (argc < 5) {
        std::cerr << "usage: placer_apply <file.bf> <entry_idx> <seed> <ops.json>\n"
                     "       placer_apply <file.bf> plan <out.bf> <entry_idx> <ops.json> [strict]\n";
        return 2;
    }
    uint32_t entry = uint32_t(std::strtoul(argv[2], nullptr, 10));
    uint32_t seed = uint32_t(std::strtoul(argv[3], nullptr, 0));

    bool ops_ok = false;
    std::vector<PlaceOp> ops = parse_ops(argv[4], ops_ok);
    if (!ops_ok) return 2;

    try {
        BigFile bf;
        bf.open(argv[1]);
        collect_xbin_sources(bf, ops);
        LzoResult dr = decompress_lzo(bf.read_data(entry));
        if (!dr.ok) { std::cerr << "placer_apply: decompress failed\n"; return 1; }

        PlacementResult r = apply_placements_to_dec(dr.data, ops, 0, {}, seed);
        if (!r.ok) {
            std::printf("ERROR %s\n", r.error.c_str());
            return 0;   // errors compare as lines too
        }
        std::printf("APPLY dec=%zu crc=%08x wl=%08x reg=%u adds=%zu\n",
                    r.patched.size(), crc32(r.patched.data(), r.patched.size()),
                    r.has_world_list_key ? r.world_list_key : 0,
                    r.objects_registered, r.additions.size());
        for (const auto& kv : r.additions)
            std::printf("ADD %08x len=%zu crc=%08x\n", kv.first, kv.second.size(),
                        crc32(kv.second.data(), kv.second.size()));
        if (argc >= 6) {
            std::ofstream o(std::filesystem::u8path(argv[5]),
                            std::ios::binary);
            o.write(reinterpret_cast<const char*>(r.patched.data()),
                    std::streamsize(r.patched.size()));
        }
    } catch (const std::exception& e) {
        std::cerr << "placer_apply: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

static std::vector<PlaceOp> parse_ops(const char* json_path, bool& ok) {
    ok = false;
    std::vector<PlaceOp> ops;
    std::ifstream jf(std::filesystem::u8path(json_path), std::ios::binary);
    std::string jtxt((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
    json::Value doc;
    try { doc = json::parse(jtxt); } catch (const std::exception& e) {
        std::cerr << "placer_apply: bad ops json: " << e.what() << "\n";
        return ops;
    }
    const json::Value* arr = doc.find("ops");
    if (arr && arr->is_arr()) {
        for (const json::Value& o : arr->arr) {
            PlaceOp op;
            const json::Value* v;
            if ((v = o.find("kind")) && v->is_str()) op.kind = v->str;
            if ((v = o.find("name")) && v->is_str()) op.name = v->str;
            op.position = vec3_of(o.find("position"), {0, 0, 0});
            op.rotation_euler_deg = vec3_of(o.find("rotation_euler_deg"), {0, 0, 0});
            op.scale_xform = vec3_of(o.find("scale_xform"), {1, 1, 1});
            op.size = vec3_of(o.find("size"), {1, 1, 1});
            if ((v = o.find("source_key")) && v->is_num())
                op.source_key = uint32_t(v->num);
            if ((v = o.find("material_key")) && v->is_num()) {
                op.has_material_key = true;
                op.material_key = uint32_t(v->num);
            }
            if ((v = o.find("collision"))) op.collision = v->b;
            if ((v = o.find("collision_profile")) && v->is_str())
                op.collision_profile = v->str;
            if ((v = o.find("room_cob_key")) && v->is_num()) {
                op.has_room_cob_key = true;
                op.room_cob_key = uint32_t(v->num);
            }
            if ((v = o.find("vertex_color")) && v->is_arr()) {
                op.has_vertex_color = true;
                op.vertex_color = {0, 0, 0, 255};
                for (size_t i = 0; i < 4 && i < v->arr.size(); ++i)
                    op.vertex_color[i] = int(v->arr[i].num);
            }
            if ((v = o.find("model_path")) && v->is_str())
                op.model_path = v->str;
            if ((v = o.find("imported_jgao_path")) && v->is_str()) {
                std::ifstream input(std::filesystem::u8path(v->str),
                                    std::ios::binary);
                std::vector<uint8_t> bytes(
                    (std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
                jgao::File file = jgao::parse(bytes.data(), bytes.size());
                if (!file.ok) {
                    std::cerr << "placer_apply: " << file.error << "\n";
                    return {};
                }
                op.has_imported_jgao = true;
                op.imported_mat_key = file.mat_key;
                op.imported_geo_data = std::move(file.geo_data);
                op.imported_mat_data = std::move(file.mat_data);
                for (jgao::MaterialChild& child : file.mat_children) {
                    ChainEntry entry;
                    entry.key = child.key;
                    entry.has_gro_type = true;
                    entry.gro_type = child.gro_type;
                    entry.data = std::move(child.data);
                    op.imported_mat_children.push_back(std::move(entry));
                }
            }
            if ((v = o.find("import_vertex_colors"))
                && v->type == json::Value::Type::Bool)
                op.import_vertex_colors = v->b;
            if ((v = o.find("world_matrix")) && v->is_arr() && v->arr.size() == 16) {
                op.has_world_matrix = true;
                for (size_t i = 0; i < 16; ++i) op.world_matrix[i] = v->arr[i].num;
            }
            if ((v = o.find("target_key")) && v->is_num()) {
                op.has_target_key = true;
                op.target_key = uint32_t(v->num);
            }
            if ((v = o.find("source_gao_key")) && v->is_num()) {
                op.has_source_gao_key = true;
                op.source_gao_key = uint32_t(v->num);
            }
            if ((v = o.find("source_entry_index")) && v->is_num()) {
                op.has_source_entry_index = true;
                op.source_entry_index = uint32_t(v->num);
            }
            ops.push_back(std::move(op));
        }
    }
    ok = true;
    return ops;
}
