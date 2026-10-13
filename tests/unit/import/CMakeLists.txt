# tests/unit/import: the content importers of the cooker library (tools/cook/importers/, S11).
# Importers are cooker code: these tests exist only when tools are built. tests/ is added before
# tools/, so test for the option, not for the target. Later S11 tasks append their tests below.
if(NOT DE_BUILD_TOOLS)
    return()
endif()

# DENG-101: the glTF model importer (tools/cook/importers/model/): the front end into the model
# IR, axis conversion, sub-asset names, the cooked mesh/material/model formats, the prefab TOML,
# determinism and corrupt input. CTest names import.gltf.<test>. data/gltf/ is mounted read-only
# as gltf:/ (Khronos samples and gen_fixtures.py output); test_import_tmp/ in the build tree is
# gtmp:/. The prefab tests use the runtime's prefab importer (de_asset, de_ecs).
de_add_test(NAME import SOURCES test_gltf_import.c
            LIBS de_cooker de_asset de_ecs de_reflect de_task de_platform TIMEOUT 120)
if(TARGET test_import)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_import_tmp")
    target_include_directories(test_import PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_import PRIVATE
        "DE_IMPORT_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_IMPORT_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_import_tmp\"")
endif()

# DENG-102: the FBX front end of the model importer (fbx_import.c, ufbx): units and axes against
# the glTF reference, the axis overrides, n-gons and the material split, PBR and legacy
# materials, skins, baked animation stacks, determinism and corrupt input. CTest names
# import_fbx.fbx.<test>. data/fbx/ is mounted read-only as fbx:/ (gen_fixtures.py output);
# test_import_fbx_tmp/ in the build tree is ftmp:/.
de_add_test(NAME import_fbx SOURCES test_fbx_import.c
            LIBS de_cooker de_asset de_ecs de_reflect de_task de_platform TIMEOUT 120)
if(TARGET test_import_fbx)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_import_fbx_tmp")
    target_include_directories(test_import_fbx PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_import_fbx PRIVATE
        "DE_IMPORT_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_IMPORT_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_import_fbx_tmp\"")
endif()

# DENG-98: the texture importer (tools/cook/importers/texture/): PNG/JPEG/EXR/KTX2 decode, linear
# mips, the DTEX layout, determinism, rejection, the settings hash, model textures cooked to DTEX,
# the Dev in-process cook and `de-cook cook`. In the same executable, CTest names
# import.texture.<test>. data/texture/ is mounted read-only as texd:/ (gen_fixtures.py output),
# data/gltf/ as texg:/, and test_import_tmp/ as ttmp:/. Links de_cook_cmd for `de-cook cook`.
if(TARGET test_import)
    target_sources(test_import PRIVATE test_texture_import.c)
    target_link_libraries(test_import PRIVATE de_cook_cmd)
endif()

# DENG-103: the mesh stage of the model importer (meshlets, quantization, the LOD chain; DMSH v2).
# Built into test_import, so they share its data mount setup; CTest names import.mesh.<test>.
# mesh_fixtures.h generates the bunny-class mesh. test_mesh_lod.c reproduces the LOD error metric
# with meshoptimizer directly, so the test links it.
if(TARGET test_import)
    target_sources(test_import PRIVATE mesh_fixtures.h test_meshlets.c test_mesh_lod.c)
    target_link_libraries(test_import PRIVATE de_tp_meshoptimizer)
endif()

# DENG-104: skeletons and animation clips of the model importer (anim_skeleton.c,
# anim_clip_compress.c): skeleton order and inverse binds, clip round trip through the reference
# sampler (de_anim, src/anim/formats/clip_decode.c), constant tracks, smallest-three rotations,
# hemisphere flips, step and cubic resampling, compression ratio, determinism and settings. CTest
# names import_anim.<skeleton|clip>.<test>. data/anim/ (Khronos CesiumMan and RiggedFigure) is
# mounted as animdata:/, data/gltf/ as animgltf:/.
de_add_test(NAME import_anim SOURCES test_skeleton_clip.c
            LIBS de_cooker de_anim de_asset de_platform TIMEOUT 120)
if(TARGET test_import_anim)
    target_include_directories(test_import_anim PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_import_anim PRIVATE
        "DE_IMPORT_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
endif()

# DENG-100: HDR environments (env_prefilter.c, env_sh.c): source layouts and face directions,
# the GGX prefilter of a constant environment, mip sizes, SH9 irradiance (constant and
# directional), rotation, byte-identical output across worker counts, the cooked DTEX + ENVS blob
# and its parser. Built into test_import (its texd:/ and ttmp:/ mounts); CTest names
# import.env.<test>. Faces are 8 to 32 texels with 64 samples or fewer, so each test stays fast
# under the sanitizers.
if(TARGET test_import)
    target_sources(test_import PRIVATE test_env_prefilter.c)
endif()
