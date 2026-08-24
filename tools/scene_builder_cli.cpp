// Synthetic driver for the generic non-animated glTF builder parity oracle.
#include <filesystem>
#include <fstream>
#include <iostream>

#include "jade/GltfBuilder.hpp"
#include "jade/Image.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade;

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: scene_builder_cli <out.glb>\n";
        return 2;
    }

    gltfbuild::SceneInput scene;
    scene.scene_name = "GeneralScene";
    scene.extras_json =
        R"({"jade_source":"fixture","mesh_count":2,"material_count":2,"texture_count":1,"animation_count":0})";

    const uint8_t rgba[] = {
        255, 0, 0, 255,   0, 255, 0, 128,
        0, 0, 255, 255,   255, 255, 0, 64,
    };
    scene.images_png.push_back(encode_png_rgba(rgba, 2, 2));

    gltfbuild::SceneMaterial mat0;
    mat0.name = "mat_0x13000001";
    mat0.base_color = {{0.25, 0.5, 0.75, 1.0}};
    mat0.texture_idx = 0;
    mat0.extras_json = R"({"jade_key":"0x13000001"})";
    scene.materials.push_back(mat0);
    gltfbuild::SceneMaterial mat1;
    mat1.name = "mat_0x13000002";
    mat1.base_color = {{0.8, 0.7, 0.6, 0.5}};
    mat1.extras_json = R"({"jade_key":"0x13000002"})";
    scene.materials.push_back(mat1);

    gltfbuild::SceneMesh skinned;
    skinned.name = "Skinned";
    skinned.extras_json = R"({"jade_key":"0x1A000001"})";
    skinned.geo.ok = true;
    skinned.geo.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    skinned.geo.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    skinned.geo.uvs = {0, 0, 1, 0, 0, 1, 0.5f, 0.5f};
    skinned.geo.faces = {0, 1, 2, 0, 1, 2, 0};
    skinned.geo.skin_present = true;
    GeoBone bone0;
    bone0.bone_idx = 0;
    bone0.bind_matrix = {{1, 0, 0, 0, 0, 1, 0, 0,
                          0, 0, 1, 0, 0, 0, 0, 1}};
    bone0.weights = {{0, 65535}, {1, 32768}};
    GeoBone bone1;
    bone1.bone_idx = 1;
    bone1.bind_matrix = {{1, 0, 0, 0, 0, 1, 0, 0,
                          0, 0, 1, 0, 2, 3, 4, 1}};
    bone1.weights = {{1, 32767}, {2, 65535}};
    skinned.geo.skin_bones = {bone0, bone1};
    skinned.material_indices = {0};
    skinned.gizmo_gao_keys = {0x14000001, 0x14000002};
    scene.meshes.push_back(skinned);

    gltfbuild::SceneMesh stat;
    stat.name = "Static";
    stat.extras_json = R"({"jade_key":"0x1A000002"})";
    stat.geo.ok = true;
    stat.geo.vertices = {-1, 0, 0, 1, 0, 0, 1, 2, 0, -1, 2, 0};
    stat.geo.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    stat.geo.uvs = {0, 0, 1, 0, 1, 1, 0, 1};
    stat.geo.faces = {
        0, 1, 2, 0, 1, 2, 0,
        0, 2, 3, 0, 2, 3, 1,
    };
    stat.material_indices = {1, 0};
    scene.meshes.push_back(stat);

    gltfbuild::SceneNode root;
    root.name = "RootBone";
    root.children = {1, 3};
    root.extras_json = R"({"jade_key":"0x14000001","jade_type":"bone"})";
    root.is_bone = true;
    root.jade_key = 0x14000001;
    root.has_matrix = true;
    root.matrix = {{1, 0, 0, 0, 0, 1, 0, 0,
                    0, 0, 1, 0, 1, 2, 3, 1}};
    scene.nodes.push_back(root);
    gltfbuild::SceneNode child;
    child.name = "ChildBone";
    child.extras_json = R"({"jade_key":"0x14000002","jade_type":"bone"})";
    child.is_bone = true;
    child.jade_key = 0x14000002;
    scene.nodes.push_back(child);
    gltfbuild::SceneNode skinned_node;
    skinned_node.name = "Skinned";
    skinned_node.mesh_idx = 0;
    skinned_node.extras_json = R"({"jade_key":"0x1A000001"})";
    scene.nodes.push_back(skinned_node);
    gltfbuild::SceneNode static_node;
    static_node.name = "Static";
    static_node.mesh_idx = 1;
    static_node.extras_json = R"({"jade_key":"0x1A000002"})";
    scene.nodes.push_back(static_node);

    std::vector<uint8_t> glb = gltfbuild::build_scene_glb(scene);
    std::ofstream out(std::filesystem::u8path(argv[1]), std::ios::binary);
    out.write(reinterpret_cast<const char*>(glb.data()),
              std::streamsize(glb.size()));
    if (!out) return 1;
    std::cout << "GLB bytes=" << glb.size() << "\n";
    return 0;
}
