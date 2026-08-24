// patchmodel_cli — drive the native _patch_model_glb port on one BF entry.
//
//   patchmodel_cli <bf> <entry_key_hex> <geo_key_hex> <glb_path> <out_dec>
//   patchmodel_cli --dec <entry.bin> <geo_key_hex> <glb_path> <out_dec>
//                  [--bone-map j:b,j:b] [--bone-map-source j:src,...]
//                  [--drops j,j] [--rigid-bind N] [--drop-targets j:b,...]
//                  [--auto-rig] [--diagnose] [--colors] [--keep-skin]
//                  [--scene]
//
// Decompresses the entry, runs patch_model_glb, writes the patched dec bytes
// to <out_dec> ("-" = don't write). Prints LOG lines and a final RESULT line:
//   RESULT changed=0|1 bytes=<n>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/GltfBuilder.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/PatcherModel.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade;

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return true;
}

static void parse_pairs(const char* arg, std::map<int, int>& out) {
    std::string s = arg;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        std::string tok = s.substr(i, j - i);
        size_t c = tok.find(':');
        if (c != std::string::npos)
            out[std::atoi(tok.substr(0, c).c_str())] =
                std::atoi(tok.substr(c + 1).c_str());
        i = j + 1;
    }
}

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: patchmodel_cli <bf> <entry_key> <geo_key> "
                     "<glb> <out_dec|-> [options]\n"
                     "       patchmodel_cli --dec <entry.bin> <geo_key> "
                     "<glb> <out_dec|-> [options]\n");
        return 2;
    }
    bool direct_dec = !std::strcmp(argv[1], "--dec");
    uint32_t entry_key = direct_dec
                             ? 0
                             : uint32_t(std::strtoul(argv[2], nullptr, 16));
    uint32_t geo_key = uint32_t(std::strtoul(argv[3], nullptr, 16));
    std::string out_path = argv[5];

    patcher::PatchModelOptions opts;
    for (int i = 6; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bone-map") && i + 1 < argc)
            parse_pairs(argv[++i], opts.bone_map);
        else if (!std::strcmp(argv[i], "--bone-map-source") && i + 1 < argc) {
            std::string s = argv[++i];
            size_t p = 0;
            while (p < s.size()) {
                size_t j = s.find(',', p);
                if (j == std::string::npos) j = s.size();
                std::string tok = s.substr(p, j - p);
                size_t c = tok.find(':');
                if (c != std::string::npos)
                    opts.bone_map_source[std::atoi(tok.substr(0, c).c_str())] =
                        tok.substr(c + 1);
                p = j + 1;
            }
        } else if (!std::strcmp(argv[i], "--drops") && i + 1 < argc) {
            std::string s = argv[++i];
            size_t p = 0;
            while (p < s.size()) {
                size_t j = s.find(',', p);
                if (j == std::string::npos) j = s.size();
                opts.bone_drops.insert(std::atoi(s.substr(p, j - p).c_str()));
                p = j + 1;
            }
        } else if (!std::strcmp(argv[i], "--rigid-bind") && i + 1 < argc) {
            opts.has_rigid_bind_bone = true;
            opts.rigid_bind_bone = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--drop-targets") && i + 1 < argc)
            parse_pairs(argv[++i], opts.drop_targets);
        else if (!std::strcmp(argv[i], "--auto-rig"))
            opts.auto_rig = true;
        else if (!std::strcmp(argv[i], "--diagnose"))
            opts.diagnose_rest_pose = true;
        else if (!std::strcmp(argv[i], "--colors"))
            opts.import_vertex_colors = true;
        else if (!std::strcmp(argv[i], "--keep-skin"))
            opts.keep_original_skin = true;
    }

    std::vector<uint8_t> dec_data;
    if (direct_dec) {
        if (!read_file(argv[2], dec_data)) {
            std::printf("ERROR cannot read decompressed entry %s\n", argv[2]);
            return 0;
        }
    } else {
        BigFile bf;
        try { bf.open(argv[1]); } catch (const std::exception& e) {
            std::printf("ERROR %s\n", e.what());
            return 0;
        }
        const BFFile* fi = nullptr;
        for (const auto& kv : bf.files)
            if (kv.second.key == entry_key) { fi = &kv.second; break; }
        if (fi == nullptr) {
            std::printf("ERROR entry 0x%08x not in archive\n", entry_key);
            return 0;
        }
        LzoResult dec = decompress_lzo(bf.read_data(fi->index));
        if (!dec.ok) {
            std::printf("ERROR entry did not decompress\n");
            return 0;
        }
        dec_data = std::move(dec.data);
    }
    std::vector<uint8_t> glb;
    if (!read_file(argv[4], glb)) {
        std::printf("ERROR cannot read GLB %s\n", argv[4]);
        return 0;
    }

    bool analyze = false, validate_skin = false, hier = false, scene = false;
    for (int i = 6; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--analyze")) analyze = true;
        if (!std::strcmp(argv[i], "--validate-skin")) validate_skin = true;
        if (!std::strcmp(argv[i], "--hier")) hier = true;
        if (!std::strcmp(argv[i], "--scene")) scene = true;
    }
    if (hier) {
        // Hierarchical GEO export; <glb> arg is ignored, out gets the GLB.
        std::vector<SubEntry> subs = walk_sub_entries(dec_data);
        gltfbuild::HierExportResult h =
            gltfbuild::build_hierarchical_geo_glb(subs, geo_key);
        if (!h.ok) {
            std::printf("HIER error=%s\n", h.error.c_str());
            return 0;
        }
        if (out_path != "-") {
            std::ofstream out(std::filesystem::u8path(out_path),
                              std::ios::binary);
            out.write(reinterpret_cast<const char*>(h.glb.data()),
                      long(h.glb.size()));
        }
        std::printf("HIER ok=1 points=%u tris=%u bones=%u roots=%u "
                    "bytes=%zu\n", h.points, h.tris, h.bones, h.roots,
                    h.glb.size());
        return 0;
    }
    if (analyze) {
        // Bone-remap dialog data dump (harness-comparable lines).
        patcher::AnalyzeBoneResult a =
            patcher::analyze_bone_mapping(dec_data, geo_key, glb);
        if (!a.ok) {
            std::printf("ANALYZE error=%s\n", a.error.c_str());
            return 0;
        }
        std::printf("foreign %d %s\n", a.is_foreign ? 1 : 0,
                    a.foreign_reason.c_str());
        for (const std::string& n : a.glb_joint_names)
            std::printf("jname %s\n", n.c_str());
        for (const std::string& n : a.dp_bone_names)
            std::printf("bname %s\n", n.c_str());
        for (const auto& kv : a.auto_map)
            std::printf("map %d %d %s\n", kv.first, kv.second,
                        a.auto_map_source.at(kv.first).c_str());
        for (size_t j = 0; j < a.joint_stats.size(); ++j)
            std::printf("jstat %zu %u %u %.17g\n", j,
                        a.joint_stats[j].vertex_count,
                        a.joint_stats[j].dominant_count,
                        a.joint_stats[j].weight_share);
        auto dump3 = [](const char* tag, size_t i,
                        const std::pair<bool, std::array<double, 3>>& p) {
            if (p.first)
                std::printf("%s %zu %.17g %.17g %.17g\n", tag, i,
                            p.second[0], p.second[1], p.second[2]);
            else
                std::printf("%s %zu None\n", tag, i);
        };
        for (size_t i = 0; i < a.joint_centroids.size(); ++i)
            dump3("jcent", i, a.joint_centroids[i]);
        for (size_t i = 0; i < a.bone_centroids.size(); ++i)
            dump3("bcent", i, a.bone_centroids[i]);
        std::printf("diag %.17g\n", a.mesh_diagonal);
        for (size_t i = 0; i < a.joint_rest_positions.size(); ++i)
            dump3("jrest", i, a.joint_rest_positions[i]);
        for (size_t i = 0; i < a.bone_rest_positions.size(); ++i)
            dump3("brest", i, a.bone_rest_positions[i]);
        for (size_t i = 0; i < a.orig_bone_weight_shares.size(); ++i)
            std::printf("bshare %zu %.17g\n", i,
                        a.orig_bone_weight_shares[i]);
        std::printf("ANALYZE ok\n");
        return 0;
    }
    if (validate_skin) {
        SkinValidationResult v = validate_glb_skin(dec_data, geo_key, glb);
        if (!v.error.empty()) {
            std::printf("SKINVALID error=%s\n", v.error.c_str());
            return 0;
        }
        std::printf("SKIN ok=%d has=%d joints=%u bones=%u matches=%zu "
                    "warnings=%zu\n", v.ok ? 1 : 0,
                    v.glb_has_skin ? 1 : 0, v.glb_joint_count,
                    v.orig_bone_count, v.name_matches.size(),
                    v.warnings.size());
        for (const std::string& name : v.glb_joint_names)
            std::printf("GNAME %s\n", name.c_str());
        for (const std::string& name : v.orig_bone_names)
            std::printf("ONAME %s\n", name.c_str());
        for (const SkinNameMatch& match : v.name_matches)
            std::printf("MATCH %s|%s\n", match.glb_name.c_str(),
                        match.original_name.c_str());
        for (const std::string& warning : v.warnings)
            std::printf("WARN %s\n", warning.c_str());
        return 0;
    }

    if (scene) {
        std::vector<std::string> log;
        patcher::PatchSceneResult r =
            patcher::patch_scene_glb(dec_data, glb, argv[4], log);
        for (const std::string& l : log) std::printf("LOG %s\n", l.c_str());
        if (r.changed && out_path != "-") {
            std::ofstream out(std::filesystem::u8path(out_path),
                              std::ios::binary);
            out.write(reinterpret_cast<const char*>(r.patched.data()),
                      long(r.patched.size()));
        }
        std::printf("SCENE changed=%d patched=%u bytes=%zu\n",
                    r.changed ? 1 : 0, r.patch_count, r.patched.size());
        return 0;
    }

    std::vector<std::string> log;
    patcher::PatchModelResult r =
        patcher::patch_model_glb(dec_data, geo_key, glb, opts, log);
    for (const std::string& l : log) std::printf("LOG %s\n", l.c_str());
    if (r.changed && out_path != "-") {
        std::ofstream out(std::filesystem::u8path(out_path),
                          std::ios::binary);
        out.write(reinterpret_cast<const char*>(r.patched.data()),
                  long(r.patched.size()));
    }
    std::printf("RESULT changed=%d bytes=%zu\n", r.changed ? 1 : 0,
                r.patched.size());
    return 0;
}
