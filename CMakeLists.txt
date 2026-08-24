cmake_minimum_required(VERSION 3.16)
project(jade_native CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()

# --- libjade: the ported core + vendored miniLZO codec ---
add_library(jade STATIC
  src/BigFile.cpp
  src/Compression.cpp
  src/Image.cpp
  src/Prefab.cpp
  src/Texture.cpp
  src/TextureUpscale.cpp
  src/CharacterSwap.cpp
  src/ZoneTransplant.cpp
  src/ZoneCreate.cpp
  src/LoadSim.cpp
  src/LoadSimCache.cpp
  src/LevelBlender.cpp
  src/GltfBuilder.cpp
  src/Exporter.cpp
  src/Deploy.cpp
  src/LoaValidate.cpp
  src/Project.cpp
  src/ProjectAssets.cpp
  src/SubEntry.cpp
  src/Geometry.cpp
  src/Collision.cpp
  src/CollisionFollow.cpp
  src/Light.cpp
  src/Material.cpp
  src/Gao.cpp
  src/JadeObject.cpp
  src/ObjectKinds.cpp
  src/WolInfo.cpp
  src/Skeleton.cpp
  src/Rli.cpp
  src/AssetIndex.cpp
  src/Json.cpp
  src/MeshSwap.cpp
  src/PatcherModel.cpp
  src/Patcher.cpp
  src/RegisterResource.cpp
  src/BytePatch.cpp
  src/ExePatch.cpp
  src/RliRescale.cpp
  src/ObjectPlacer.cpp
  src/ModelImport.cpp
  src/Gltf.cpp
  src/Jgao.cpp
  src/WowStream.cpp
  src/Wol.cpp
  src/Keys.cpp
  src/Zone.cpp
  src/Jtmod.cpp
  src/GameProfiles.cpp
  src/JtmodEngine.cpp
  third_party/minilzo/minilzo.c   # LZO1X codec (compiled as C)
)
target_include_directories(jade PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# minilzo headers are an internal detail of Compression.cpp, not part of the
# public jade API.
target_include_directories(jade PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minilzo)

# --- vendored lzo1x_999 optimizer (LZO 2.10 subset, GPLv2) ---
# python-lzo's level-9 compressor (compress_lzo_9). Pure integer code; exposed
# to libjade only through the size_t wrapper in lzo999_wrap.c.
add_library(lzo999 STATIC
  third_party/lzo999/lzo1x_9x.c
  third_party/lzo999/lzo999_wrap.c
)
target_include_directories(lzo999 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lzo999)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lzo999 PRIVATE -w)
endif()
target_link_libraries(jade PUBLIC lzo999)
if(WIN32)
  target_link_libraries(jade PUBLIC windowscodecs ole32 shell32)
endif()
# Disable FMA contraction so computed-float math (the glTF import transforms)
# matches the Python oracle, which never fuses multiply-add. No existing jade
# code does float multiply-add, so this is a no-op for every current golden.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(jade PRIVATE -ffp-contract=off)
endif()

# --- vendored quicktex 0.3.1 BC1/BC3/BC4 encoder (Apache-2.0) ---
# The toolkit's texture.encode_dxt1/encode_dxt5 use quicktex; we vendor its exact
# source (minus the pybind glue) so the native encoder matches byte-for-byte.
# Compiled with warnings off (third-party) and -ffp-contract=off (the BC1 PCA path
# is float; disabling FMA fusion maximizes cross-compiler determinism vs the wheel).
file(GLOB_RECURSE QUICKTEX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quicktex/*.cpp)
add_library(quicktex STATIC ${QUICKTEX_SRC})
target_include_directories(quicktex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_features(quicktex PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(quicktex PRIVATE -w -ffp-contract=off)
endif()
target_link_libraries(jade PUBLIC quicktex)

# --- CLIs (golden-diff producers) ---
add_executable(popmm tools/popmm.cpp)
target_link_libraries(popmm PRIVATE jade)
set_target_properties(popmm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(bf_dump tools/bf_dump.cpp)
target_link_libraries(bf_dump PRIVATE jade)
set_target_properties(bf_dump PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_decomp_digest tools/bf_decomp_digest.cpp)
target_link_libraries(bf_decomp_digest PRIVATE jade)
set_target_properties(bf_decomp_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_extract tools/bf_extract.cpp)
target_link_libraries(bf_extract PRIVATE jade)
set_target_properties(bf_extract PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_tex_digest tools/bf_tex_digest.cpp)
target_link_libraries(bf_tex_digest PRIVATE jade)
set_target_properties(bf_tex_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_subentry_digest tools/bf_subentry_digest.cpp)
target_link_libraries(bf_subentry_digest PRIVATE jade)
set_target_properties(bf_subentry_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_geo_digest tools/bf_geo_digest.cpp)
target_link_libraries(bf_geo_digest PRIVATE jade)
set_target_properties(bf_geo_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_cob_digest tools/bf_cob_digest.cpp)
target_link_libraries(bf_cob_digest PRIVATE jade)
set_target_properties(bf_cob_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_light_digest tools/bf_light_digest.cpp)
target_link_libraries(bf_light_digest PRIVATE jade)
set_target_properties(bf_light_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_mat_digest tools/bf_mat_digest.cpp)
target_link_libraries(bf_mat_digest PRIVATE jade)
set_target_properties(bf_mat_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_gao_digest tools/bf_gao_digest.cpp)
target_link_libraries(bf_gao_digest PRIVATE jade)
set_target_properties(bf_gao_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_zone_digest tools/bf_zone_digest.cpp)
target_link_libraries(bf_zone_digest PRIVATE jade)
set_target_properties(bf_zone_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_geogroup_digest tools/bf_geogroup_digest.cpp)
target_link_libraries(bf_geogroup_digest PRIVATE jade)
set_target_properties(bf_geogroup_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_matkind_digest tools/bf_matkind_digest.cpp)
target_link_libraries(bf_matkind_digest PRIVATE jade)
set_target_properties(bf_matkind_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_grade_digest tools/bf_grade_digest.cpp)
target_link_libraries(bf_grade_digest PRIVATE jade)
set_target_properties(bf_grade_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_objkind_digest tools/bf_objkind_digest.cpp)
target_link_libraries(bf_objkind_digest PRIVATE jade)
set_target_properties(bf_objkind_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_wolinfo_digest tools/bf_wolinfo_digest.cpp)
target_link_libraries(bf_wolinfo_digest PRIVATE jade)
set_target_properties(bf_wolinfo_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_skeleton_digest tools/bf_skeleton_digest.cpp)
target_link_libraries(bf_skeleton_digest PRIVATE jade)
set_target_properties(bf_skeleton_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_rli_digest tools/bf_rli_digest.cpp)
target_link_libraries(bf_rli_digest PRIVATE jade)
set_target_properties(bf_rli_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_assetidx_digest tools/bf_assetidx_digest.cpp)
target_link_libraries(bf_assetidx_digest PRIVATE jade)
set_target_properties(bf_assetidx_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_cobwrite_digest tools/bf_cobwrite_digest.cpp)
target_link_libraries(bf_cobwrite_digest PRIVATE jade)
set_target_properties(bf_cobwrite_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(collision_helpers_cli tools/collision_helpers_cli.cpp)
target_link_libraries(collision_helpers_cli PRIVATE jade)
set_target_properties(collision_helpers_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_texwrite_digest tools/bf_texwrite_digest.cpp)
target_link_libraries(bf_texwrite_digest PRIVATE jade)
set_target_properties(bf_texwrite_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_dxtenc_digest tools/bf_dxtenc_digest.cpp)
target_link_libraries(bf_dxtenc_digest PRIVATE jade)
set_target_properties(bf_dxtenc_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_rliwrite_digest tools/bf_rliwrite_digest.cpp)
target_link_libraries(bf_rliwrite_digest PRIVATE jade)
set_target_properties(bf_rliwrite_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_lightwrite_digest tools/bf_lightwrite_digest.cpp)
target_link_libraries(bf_lightwrite_digest PRIVATE jade)
set_target_properties(bf_lightwrite_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_matresolve_digest tools/bf_matresolve_digest.cpp)
target_link_libraries(bf_matresolve_digest PRIVATE jade)
set_target_properties(bf_matresolve_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_gaorli_digest tools/bf_gaorli_digest.cpp)
target_link_libraries(bf_gaorli_digest PRIVATE jade)
set_target_properties(bf_gaorli_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bf_gaowrite_digest tools/bf_gaowrite_digest.cpp)
target_link_libraries(bf_gaowrite_digest PRIVATE jade)
set_target_properties(bf_gaowrite_digest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(placer_apply tools/placer_apply.cpp)
target_link_libraries(placer_apply PRIVATE jade)
set_target_properties(placer_apply PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(placer_leaf tools/placer_leaf.cpp)
target_link_libraries(placer_leaf PRIVATE jade)
set_target_properties(placer_leaf PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(rli_rescale_cli tools/rli_rescale_cli.cpp)
target_link_libraries(rli_rescale_cli PRIVATE jade)
set_target_properties(rli_rescale_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(patchmodel_cli tools/patchmodel_cli.cpp)
target_link_libraries(patchmodel_cli PRIVATE jade)
set_target_properties(patchmodel_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(patcher_cli tools/patcher_cli.cpp)
target_link_libraries(patcher_cli PRIVATE jade)
set_target_properties(patcher_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(register_resource_cli tools/register_resource_cli.cpp)
target_link_libraries(register_resource_cli PRIVATE jade)
set_target_properties(register_resource_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(bytepatch_cli tools/bytepatch_cli.cpp)
target_link_libraries(bytepatch_cli PRIVATE jade)
set_target_properties(bytepatch_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(exepatch_cli tools/exepatch_cli.cpp)
target_link_libraries(exepatch_cli PRIVATE jade)
set_target_properties(exepatch_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

# The standalone patcher is the exact same dependency-free Win32 source used
# by Python's build.exe_patch. Building it once with jade_native removes the
# Python workflow's runtime-compiler dependency while keeping artifact/runtime
# behavior identical. The source remains shared while both implementations
# live in this repository, so fixes cannot silently diverge.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../jade_explorer/build/cpp/patcher.cpp")
  add_executable(jade_bytepatch_stub WIN32
    ${CMAKE_CURRENT_SOURCE_DIR}/../jade_explorer/build/cpp/patcher.cpp)
  if(MINGW)
    target_link_options(jade_bytepatch_stub PRIVATE -municode -static)
  endif()
  target_link_libraries(jade_bytepatch_stub PRIVATE
    comctl32 comdlg32 gdi32 user32 shell32)
  set_target_properties(jade_bytepatch_stub PROPERTIES
    OUTPUT_NAME jade_bytepatch_stub
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/app)
endif()

add_executable(glb_swap tools/glb_swap.cpp)
target_link_libraries(glb_swap PRIVATE jade)
set_target_properties(glb_swap PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(lzo_compress_cli tools/lzo_compress_cli.cpp)
target_link_libraries(lzo_compress_cli PRIVATE jade)
set_target_properties(lzo_compress_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(project_cli tools/project_cli.cpp)
target_link_libraries(project_cli PRIVATE jade)
set_target_properties(project_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(project_assets_cli tools/project_assets_cli.cpp)
target_link_libraries(project_assets_cli PRIVATE jade)
set_target_properties(project_assets_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(deploy_cli tools/deploy_cli.cpp)
target_link_libraries(deploy_cli PRIVATE jade)
set_target_properties(deploy_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(loa_validate_cli tools/loa_validate_cli.cpp)
target_link_libraries(loa_validate_cli PRIVATE jade)
set_target_properties(loa_validate_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(exporter_cli tools/exporter_cli.cpp)
target_link_libraries(exporter_cli PRIVATE jade)
set_target_properties(exporter_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(geoglb_cli tools/geoglb_cli.cpp)
target_link_libraries(geoglb_cli PRIVATE jade)
set_target_properties(geoglb_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(scene_builder_cli tools/scene_builder_cli.cpp)
target_link_libraries(scene_builder_cli PRIVATE jade)
set_target_properties(scene_builder_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(levelblend_cli tools/levelblend_cli.cpp)
target_link_libraries(levelblend_cli PRIVATE jade)
set_target_properties(levelblend_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(loadsim_cli tools/loadsim_cli.cpp)
target_link_libraries(loadsim_cli PRIVATE jade)
set_target_properties(loadsim_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(zonetx_cli tools/zonetx_cli.cpp)
target_link_libraries(zonetx_cli PRIVATE jade)
set_target_properties(zonetx_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(zonecreate_cli tools/zonecreate_cli.cpp)
target_link_libraries(zonecreate_cli PRIVATE jade)
set_target_properties(zonecreate_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(charswap_cli tools/charswap_cli.cpp)
target_link_libraries(charswap_cli PRIVATE jade)
set_target_properties(charswap_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(texup_cli tools/texup_cli.cpp)
target_link_libraries(texup_cli PRIVATE jade)
set_target_properties(texup_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(glb_geo tools/glb_geo.cpp)
target_link_libraries(glb_geo PRIVATE jade)
set_target_properties(glb_geo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(jgao_cli tools/jgao_cli.cpp)
target_link_libraries(jgao_cli PRIVATE jade)
set_target_properties(jgao_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(image_cli tools/image_cli.cpp)
target_link_libraries(image_cli PRIVATE jade)
set_target_properties(image_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(prefab_cli tools/prefab_cli.cpp)
target_link_libraries(prefab_cli PRIVATE jade)
set_target_properties(prefab_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(model_import_cli tools/model_import_cli.cpp)
target_link_libraries(model_import_cli PRIVATE jade)
set_target_properties(model_import_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

add_executable(collision_follow_cli tools/collision_follow_cli.cpp)
target_link_libraries(collision_follow_cli PRIVATE jade)
set_target_properties(collision_follow_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

# --- tests ---
enable_testing()

add_executable(unit_bigfile tests/unit_bigfile.cpp)
target_link_libraries(unit_bigfile PRIVATE jade)
add_test(NAME unit_bigfile COMMAND unit_bigfile)

add_executable(unit_bigfile_write tests/unit_bigfile_write.cpp)
target_link_libraries(unit_bigfile_write PRIVATE jade)
add_test(NAME unit_bigfile_write COMMAND unit_bigfile_write)

add_executable(unit_zonetx tests/unit_zonetx.cpp)
target_link_libraries(unit_zonetx PRIVATE jade)
add_test(NAME unit_zonetx COMMAND unit_zonetx)

add_executable(unit_jtmod tests/unit_jtmod.cpp)
target_link_libraries(unit_jtmod PRIVATE jade)
add_test(NAME unit_jtmod COMMAND unit_jtmod)

add_executable(unit_jtmod_engine tests/unit_jtmod_engine.cpp)
target_link_libraries(unit_jtmod_engine PRIVATE jade)
add_test(NAME unit_jtmod_engine COMMAND unit_jtmod_engine)

add_executable(unit_compression tests/unit_compression.cpp)
target_link_libraries(unit_compression PRIVATE jade)
add_test(NAME unit_compression COMMAND unit_compression)

add_executable(unit_compress tests/unit_compress.cpp)
target_link_libraries(unit_compress PRIVATE jade)
add_test(NAME unit_compress COMMAND unit_compress)

add_executable(unit_texture tests/unit_texture.cpp)
target_link_libraries(unit_texture PRIVATE jade)
add_test(NAME unit_texture COMMAND unit_texture)

add_executable(unit_subentry tests/unit_subentry.cpp)
target_link_libraries(unit_subentry PRIVATE jade)
add_test(NAME unit_subentry COMMAND unit_subentry)

add_executable(unit_geometry tests/unit_geometry.cpp)
target_link_libraries(unit_geometry PRIVATE jade)
add_test(NAME unit_geometry COMMAND unit_geometry)

add_executable(unit_collision tests/unit_collision.cpp)
target_link_libraries(unit_collision PRIVATE jade)
add_test(NAME unit_collision COMMAND unit_collision)

add_executable(unit_collision_follow tests/unit_collision_follow.cpp)
target_link_libraries(unit_collision_follow PRIVATE jade)
add_test(NAME unit_collision_follow COMMAND unit_collision_follow)

add_executable(unit_light tests/unit_light.cpp)
target_link_libraries(unit_light PRIVATE jade)
add_test(NAME unit_light COMMAND unit_light)

add_executable(unit_material tests/unit_material.cpp)
target_link_libraries(unit_material PRIVATE jade)
add_test(NAME unit_material COMMAND unit_material)

add_executable(unit_gao tests/unit_gao.cpp)
target_link_libraries(unit_gao PRIVATE jade)
add_test(NAME unit_gao COMMAND unit_gao)

add_executable(unit_objkind tests/unit_objkind.cpp)
target_link_libraries(unit_objkind PRIVATE jade)
add_test(NAME unit_objkind COMMAND unit_objkind)

add_executable(unit_wolinfo tests/unit_wolinfo.cpp)
target_link_libraries(unit_wolinfo PRIVATE jade)
add_test(NAME unit_wolinfo COMMAND unit_wolinfo)

add_executable(unit_skeleton tests/unit_skeleton.cpp)
target_link_libraries(unit_skeleton PRIVATE jade)
add_test(NAME unit_skeleton COMMAND unit_skeleton)

add_executable(unit_rli tests/unit_rli.cpp)
target_link_libraries(unit_rli PRIVATE jade)
add_test(NAME unit_rli COMMAND unit_rli)

add_executable(unit_assetidx tests/unit_assetidx.cpp)
target_link_libraries(unit_assetidx PRIVATE jade)
add_test(NAME unit_assetidx COMMAND unit_assetidx)

add_executable(unit_cobwrite tests/unit_cobwrite.cpp)
target_link_libraries(unit_cobwrite PRIVATE jade)
add_test(NAME unit_cobwrite COMMAND unit_cobwrite)

add_executable(unit_texwrite tests/unit_texwrite.cpp)
target_link_libraries(unit_texwrite PRIVATE jade)
add_test(NAME unit_texwrite COMMAND unit_texwrite)

add_executable(unit_dxtenc tests/unit_dxtenc.cpp)
target_link_libraries(unit_dxtenc PRIVATE jade)
add_test(NAME unit_dxtenc COMMAND unit_dxtenc)

add_executable(unit_rliwrite tests/unit_rliwrite.cpp)
target_link_libraries(unit_rliwrite PRIVATE jade)
add_test(NAME unit_rliwrite COMMAND unit_rliwrite)

add_executable(unit_lightwrite tests/unit_lightwrite.cpp)
target_link_libraries(unit_lightwrite PRIVATE jade)
add_test(NAME unit_lightwrite COMMAND unit_lightwrite)

add_executable(unit_matresolve tests/unit_matresolve.cpp)
target_link_libraries(unit_matresolve PRIVATE jade)
add_test(NAME unit_matresolve COMMAND unit_matresolve)

add_executable(unit_gaorli tests/unit_gaorli.cpp)
target_link_libraries(unit_gaorli PRIVATE jade)
add_test(NAME unit_gaorli COMMAND unit_gaorli)

add_executable(unit_keyalloc tests/unit_keyalloc.cpp)
target_link_libraries(unit_keyalloc PRIVATE jade)
add_test(NAME unit_keyalloc COMMAND unit_keyalloc)

add_executable(unit_keys tests/unit_keys.cpp)
target_link_libraries(unit_keys PRIVATE jade)
add_test(NAME unit_keys COMMAND unit_keys)

add_executable(unit_patcher tests/unit_patcher.cpp)
target_link_libraries(unit_patcher PRIVATE jade)
add_test(NAME unit_patcher COMMAND unit_patcher)

add_executable(unit_rescale tests/unit_rescale.cpp)
target_link_libraries(unit_rescale PRIVATE jade)
add_test(NAME unit_rescale COMMAND unit_rescale)

add_executable(unit_swap tests/unit_swap.cpp)
target_link_libraries(unit_swap PRIVATE jade)
add_test(NAME unit_swap COMMAND unit_swap)

add_executable(unit_json tests/unit_json.cpp)
target_link_libraries(unit_json PRIVATE jade)
add_test(NAME unit_json COMMAND unit_json)

add_executable(unit_prefab tests/unit_prefab.cpp)
target_link_libraries(unit_prefab PRIVATE jade)
add_test(NAME unit_prefab COMMAND unit_prefab)

add_executable(unit_gltfgeo tests/unit_gltfgeo.cpp)
target_link_libraries(unit_gltfgeo PRIVATE jade)
add_test(NAME unit_gltfgeo COMMAND unit_gltfgeo)

add_executable(unit_gltf tests/unit_gltf.cpp)
target_link_libraries(unit_gltf PRIVATE jade)
add_test(NAME unit_gltf COMMAND unit_gltf)

add_executable(unit_jgao tests/unit_jgao.cpp)
target_link_libraries(unit_jgao PRIVATE jade)
add_test(NAME unit_jgao COMMAND unit_jgao)

add_executable(unit_jadeobject tests/unit_jadeobject.cpp)
target_link_libraries(unit_jadeobject PRIVATE jade)
add_test(NAME unit_jadeobject COMMAND unit_jadeobject)

add_executable(unit_wowstream tests/unit_wowstream.cpp)
target_link_libraries(unit_wowstream PRIVATE jade)
add_test(NAME unit_wowstream COMMAND unit_wowstream)

add_executable(unit_exporter tests/unit_exporter.cpp)
target_link_libraries(unit_exporter PRIVATE jade)
add_test(NAME unit_exporter COMMAND unit_exporter)

add_executable(unit_deploy tests/unit_deploy.cpp)
target_link_libraries(unit_deploy PRIVATE jade)
add_test(NAME unit_deploy COMMAND unit_deploy)

# Golden diff vs the Python oracle (skipped if no python on PATH). Covers both
# the structural dump and decompression (CRC of decompressed payloads).
# A moved/removed interpreter can otherwise remain pinned in an old build
# tree's cache and turn every Python oracle into a misleading "Not Run".
if(PYTHON_EXE AND NOT EXISTS "${PYTHON_EXE}")
  unset(PYTHON_EXE CACHE)
endif()
find_program(PYTHON_EXE NAMES python python3)
if(PYTHON_EXE)
  add_test(NAME jgao_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/jgao_check.py
            $<TARGET_FILE:jgao_cli>)
  add_test(NAME jgao_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/jgao_retail_check.py
            $<TARGET_FILE:jgao_cli>)
  set_tests_properties(jgao_retail_python_parity PROPERTIES TIMEOUT 600)
  add_test(NAME influence3_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/influence3_check.py
            $<TARGET_FILE:glb_geo>)
  add_test(NAME image_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/image_check.py
            $<TARGET_FILE:image_cli>)
  add_test(NAME asset_index_callbacks_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_index_callbacks_check.py
            $<TARGET_FILE:bf_assetidx_digest>)
  set_tests_properties(asset_index_callbacks_python_parity PROPERTIES
    TIMEOUT 300)
  add_test(NAME collision_helpers_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/collision_helpers_check.py
            $<TARGET_FILE:collision_helpers_cli>)
  add_test(NAME prefab_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/prefab_check.py
            $<TARGET_FILE:prefab_cli>)
  add_test(NAME model_import_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/model_import_check.py
            $<TARGET_FILE:model_import_cli>)
  add_test(NAME mesh_swap_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_swap_retail_check.py
            $<TARGET_FILE:glb_swap>)
  set_tests_properties(mesh_swap_retail_python_parity PROPERTIES TIMEOUT 1200)
  add_test(NAME collision_follow_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/collision_follow_check.py
            $<TARGET_FILE:collision_follow_cli>)
  add_test(NAME collision_follow_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/collision_follow_retail_check.py
            $<TARGET_FILE:collision_follow_cli>)
  set_tests_properties(collision_follow_retail_python_parity PROPERTIES
    TIMEOUT 600)
  add_test(NAME placement_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/placement_retail_check.py
            $<TARGET_FILE:placer_apply>)
  set_tests_properties(placement_retail_python_parity PROPERTIES TIMEOUT 1200)
  add_test(NAME patcher_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/patcher_check.py
            $<TARGET_FILE:patcher_cli>)
  add_test(NAME patcher_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/patcher_retail_check.py
            $<TARGET_FILE:patcher_cli>)
  set_tests_properties(patcher_retail_python_parity PROPERTIES TIMEOUT 1200)
  add_test(NAME patcher_retail_texture_variants_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/patcher_retail_texture_variants_check.py
            $<TARGET_FILE:patcher_cli>)
  set_tests_properties(patcher_retail_texture_variants_python_parity PROPERTIES
    TIMEOUT 300)
  add_test(NAME patcher_retail_model_scene_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/patcher_retail_model_scene_check.py
            $<TARGET_FILE:patcher_cli>)
  set_tests_properties(patcher_retail_model_scene_python_parity PROPERTIES
    TIMEOUT 1800)
  add_test(NAME patchmodel_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/patchmodel_retail_check.py
            $<TARGET_FILE:patchmodel_cli>)
  set_tests_properties(patchmodel_retail_python_parity PROPERTIES TIMEOUT 600)
  add_test(NAME patchmodel_stride64_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/patchmodel_stride64_check.py
            $<TARGET_FILE:patchmodel_cli>)
  set_tests_properties(patchmodel_stride64_python_parity PROPERTIES TIMEOUT 300)
  add_test(NAME register_resource_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/register_resource_check.py
            $<TARGET_FILE:register_resource_cli>)
  add_test(NAME register_resource_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/register_resource_retail_check.py
            $<TARGET_FILE:register_resource_cli>)
  set_tests_properties(register_resource_retail_python_parity PROPERTIES
    TIMEOUT 600)
  add_test(NAME bytepatch_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytepatch_check.py
            $<TARGET_FILE:bytepatch_cli>)
  add_test(NAME gameprofiles_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gameprofiles_check.py
            $<TARGET_FILE:bytepatch_cli>)
  add_test(NAME character_swap_workflow_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/character_swap_workflow_check.py
            $<TARGET_FILE:charswap_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME character_swap_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/charswap_retail_check.py
            $<TARGET_FILE:charswap_cli>)
  set_tests_properties(character_swap_retail_python_parity PROPERTIES
    TIMEOUT 600)
  add_test(NAME texture_upscale_workflow_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_upscale_workflow_check.py
            $<TARGET_FILE:texup_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME texture_upscale_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_upscale_retail_check.py
            $<TARGET_FILE:texup_cli>)
  set_tests_properties(texture_upscale_retail_python_parity PROPERTIES
    TIMEOUT 600)
  add_test(NAME loadsim_workflow_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/loadsim_workflow_check.py
            $<TARGET_FILE:loadsim_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME rli_rescale_workflow_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/rli_rescale_workflow_check.py
            $<TARGET_FILE:rli_rescale_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME levelblend_workflow_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/levelblend_workflow_check.py
            $<TARGET_FILE:levelblend_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME levelblend_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/levelblend_retail_check.py
            $<TARGET_FILE:levelblend_cli>)
  set_tests_properties(levelblend_retail_python_parity PROPERTIES TIMEOUT 1800)
  add_test(NAME exporter_workflow_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/exporter_workflow_check.py
            $<TARGET_FILE:exporter_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME jtmod_metadata_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/jtmod_metadata_check.py
            $<TARGET_FILE:project_cli>
            ${CMAKE_CURRENT_SOURCE_DIR}/../popsot.bf)
  add_test(NAME exporter_surface_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/exporter_surface_check.py
            $<TARGET_FILE:exporter_cli>)
  add_test(NAME scene_builder_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/scene_builder_check.py
            $<TARGET_FILE:scene_builder_cli>)
  add_test(NAME scene_export_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/scene_export_check.py
            $<TARGET_FILE:exporter_cli>)
  add_test(NAME character_bundle_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/character_bundle_retail_check.py
            $<TARGET_FILE:exporter_cli>)
  set_tests_properties(character_bundle_retail_python_parity PROPERTIES
    TIMEOUT 300)
  add_test(NAME deploy_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/deploy_check.py
            $<TARGET_FILE:deploy_cli>)
  add_test(NAME loa_validate_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/loa_validate_check.py
            $<TARGET_FILE:loa_validate_cli>)
  add_test(NAME project_verify_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_verify_check.py
            $<TARGET_FILE:project_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME project_assets_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_assets_check.py
            $<TARGET_FILE:project_assets_cli>)
  add_test(NAME project_conflicts_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_conflicts_check.py
            $<TARGET_FILE:project_cli>)
  add_test(NAME project_summaries_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_summaries_check.py
            $<TARGET_FILE:project_cli>)
  add_test(NAME project_roundtrip_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_roundtrip_check.py
            $<TARGET_FILE:project_cli>)
  add_test(NAME project_errors_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_errors_check.py
            $<TARGET_FILE:project_cli>)
  add_test(NAME project_nested_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_nested_check.py
            $<TARGET_FILE:project_cli>)
  add_test(NAME project_operation_malformed_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_operation_malformed_check.py
            $<TARGET_FILE:project_cli>)
  add_test(NAME project_numeric_extreme_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_numeric_extreme_check.py
            $<TARGET_FILE:project_cli>)
  add_test(NAME project_build_io_failure_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_build_io_failure_check.py
            $<TARGET_FILE:project_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME project_validation_python_parity
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_validation_check.py
            $<TARGET_FILE:project_cli>
            $<TARGET_FILE:lzo_compress_cli>)
  add_test(NAME project_validation_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_validation_retail_check.py
            $<TARGET_FILE:project_cli>
            $<TARGET_FILE:bf_extract>)
  add_test(NAME project_build_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/project_retail_build_check.py
            $<TARGET_FILE:project_cli>)
  set_tests_properties(project_build_retail_python_parity PROPERTIES
    TIMEOUT 3600)
  add_test(NAME asset_add_retail_variants_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_add_retail_variants_check.py
            $<TARGET_FILE:project_cli>)
  set_tests_properties(asset_add_retail_variants_python_parity PROPERTIES
    TIMEOUT 1800)
  add_test(NAME zone_transplant_retail_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/zonetx_check.py
            $<TARGET_FILE:zonetx_cli>
            $<TARGET_FILE:bf_extract>
            $<TARGET_FILE:lzo_compress_cli>)
  set_tests_properties(zone_transplant_retail_python_parity PROPERTIES
    TIMEOUT 1800)
  add_test(NAME zone_create_python_parity
    COMMAND ${PYTHON_EXE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/zonecreate_check.py
            $<TARGET_FILE:zonecreate_cli>)
  if(TARGET jade_bytepatch_stub)
    add_test(NAME exepatch_authoring
      COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/exepatch_check.py
              $<TARGET_FILE:exepatch_cli>
              $<TARGET_FILE:jade_bytepatch_stub>
              ${CMAKE_CURRENT_SOURCE_DIR}/../popsot.bf)
  endif()

  # Sampled per-entry limit keeps routine runs quick; run_golden.py --limit 0
  # does an exhaustive pass. Structural check is always full (all entries).
  add_test(NAME golden
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_golden.py
            --bf-dump $<TARGET_FILE:bf_dump>
            --decomp-tool $<TARGET_FILE:bf_decomp_digest>
            --tex-tool $<TARGET_FILE:bf_tex_digest>
            --subent-tool $<TARGET_FILE:bf_subentry_digest>
            --geo-tool $<TARGET_FILE:bf_geo_digest>
            --cob-tool $<TARGET_FILE:bf_cob_digest>
            --light-tool $<TARGET_FILE:bf_light_digest>
            --mat-tool $<TARGET_FILE:bf_mat_digest>
            --gao-tool $<TARGET_FILE:bf_gao_digest>
            --zone-tool $<TARGET_FILE:bf_zone_digest>
            --geogroup-tool $<TARGET_FILE:bf_geogroup_digest>
            --matkind-tool $<TARGET_FILE:bf_matkind_digest>
            --grade-tool $<TARGET_FILE:bf_grade_digest>
            --objkind-tool $<TARGET_FILE:bf_objkind_digest>
            --wolinfo-tool $<TARGET_FILE:bf_wolinfo_digest>
            --skeleton-tool $<TARGET_FILE:bf_skeleton_digest>
            --rli-tool $<TARGET_FILE:bf_rli_digest>
            --assetidx-tool $<TARGET_FILE:bf_assetidx_digest>
            --cobwrite-tool $<TARGET_FILE:bf_cobwrite_digest>
            --texwrite-tool $<TARGET_FILE:bf_texwrite_digest>
            --dxtenc-tool $<TARGET_FILE:bf_dxtenc_digest>
            --rliwrite-tool $<TARGET_FILE:bf_rliwrite_digest>
            --lightwrite-tool $<TARGET_FILE:bf_lightwrite_digest>
            --matresolve-tool $<TARGET_FILE:bf_matresolve_digest>
            --gaorli-tool $<TARGET_FILE:bf_gaorli_digest>
            --gaowrite-tool $<TARGET_FILE:bf_gaowrite_digest>
            --limit 200)
endif()

# ── native GUI (P0 shell) — built only when a Qt6 kit is on the prefix path ──
find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6_FOUND)
  add_subdirectory(app)
  message(STATUS "Qt6 found — building jade_gui")
else()
  message(STATUS "Qt6 not found — jade_gui skipped (core targets unaffected)")
endif()

# ── VTK viewport spike (D1) — built only when VTK is on the prefix path ──
find_package(VTK QUIET COMPONENTS CommonCore RenderingCore RenderingOpenGL2
  InteractionStyle IOImage)
if(VTK_FOUND)
  add_executable(vtk_spike tools/vtk_spike.cpp)
  target_link_libraries(vtk_spike PRIVATE jade ${VTK_LIBRARIES})
  vtk_module_autoinit(TARGETS vtk_spike MODULES ${VTK_LIBRARIES})
  set_target_properties(vtk_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
  message(STATUS "VTK found — building vtk_spike")
endif()
