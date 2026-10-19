# tests/golden: the golden-image harness (DENG-329, SPEC §19.2). docs/testing/golden.md explains it.
#
#   de_golden_flip   flip/flip.c: LDR-FLIP in C (no engine dependencies).
#   test_flip        flip/test_flip.c: FLIP against the official tool's values (CTest flip.<test>,
#                    labels unit;golden).
#   de_golden        the executable `de-golden` (harness/, scenes/*.c), and one CTest per scene
#                    and backend: golden.<backend>.<scene> (labels golden;gpu). Exit 77 = SKIPPED:
#                    no device for the backend here, the device is below the scene's tier or
#                    lacks a cap it requires, or the build cannot cook its shaders.
#
# A new scene file goes in DE_GOLDEN_SCENE_SOURCES; a new scene is a [scene.<name>] table in
# scenes.toml, which this file reads for the CTest names (re-run CMake after editing it).
if(NOT DE_BUILD_TESTS)
    return()
endif()

add_library(de_golden_flip STATIC flip/flip.c flip/flip.h)
target_include_directories(de_golden_flip PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/flip")
target_link_libraries(de_golden_flip PRIVATE de_build_flags)
if(NOT DE_COMPILER_FRONTEND STREQUAL "MSVC")
    target_link_libraries(de_golden_flip PRIVATE m)
endif()
set_target_properties(de_golden_flip PROPERTIES FOLDER "dengine/tests")

de_add_test(NAME flip
    SOURCES flip/test_flip.c
    LIBS de_golden_flip de_tp_stb
    LABELS golden)
if(TARGET test_flip)
    target_compile_definitions(test_flip PRIVATE
        "DE_FLIP_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/flip/data\"")
endif()

# ---- de-golden ---------------------------------------------------------------------------------
# Needs the GPU module (not in DE_HEADLESS builds).
if(NOT DE_HAS_GPU OR NOT TARGET de_gpu)
    return()
endif()

set(DE_GOLDEN_SCENE_SOURCES
    scenes/rhi_basic.c
    render/views/views_scenes.c
    visbuffer/vis_golden.c
    visbuffer/vis_golden.h
    visbuffer/vis_scenes.c
    materials/materials_scenes.c
    base_tier/base_tier_scenes.c)

add_executable(de_golden harness/main.c harness/golden.c harness/golden.h
                         ${DE_GOLDEN_SCENE_SOURCES})
set_target_properties(de_golden PROPERTIES OUTPUT_NAME de-golden FOLDER "dengine/tests")
target_include_directories(de_golden PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/harness"
                                             "${DE_ROOT_DIR}/src")
target_link_libraries(de_golden PRIVATE de_golden_flip de_gpu de_platform de_core de_tp_stb
                                        de_tp_tomlc17 de_build_flags)
target_compile_definitions(de_golden PRIVATE
    "DE_GOLDEN_REFS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/refs\""
    "DE_GOLDEN_SCENES_TOML=\"${CMAKE_CURRENT_SOURCE_DIR}/scenes.toml\""
    "DE_GOLDEN_SHADER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders\"")
# Shaders are cooked in-process by the S09 importer, as in the S10 pipeline tests: it needs the
# tools and Slang (tests/ is added before tools/, so test for the option and the Slang target).
if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
    target_link_libraries(de_golden PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
    target_include_directories(de_golden PRIVATE "${DE_ROOT_DIR}/tools")
    target_compile_definitions(de_golden PRIVATE DE_GOLDEN_COOK=1)
endif()
# DENG-176: renderer scenes (render/views/) run the renderer, its ECS world and Extract, and include
# renderer internals (src/ is on the include path).
target_link_libraries(de_golden PRIVATE de_render de_ecs de_reflect de_task de_asset)
# DENG-240: the vis-buffer scenes (visbuffer/) drive the S13 vis frame directly and cook their
# procedural meshes through the model importer (cook/importers/model/, part of de_cooker; with
# DE_GOLDEN_COOK only, else they skip).
target_include_directories(de_golden PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/visbuffer")
# DENG-307: the materials gallery (materials/) runs the renderer and the asset system in SOURCE mode
# over materials/assets/ (cooked on load with DE_GOLDEN_COOK only, else the scenes skip), with a
# cook cache per backend in the build tree.
target_compile_definitions(de_golden PRIVATE
    "DE_GOLDEN_MATERIALS_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/materials/assets\""
    "DE_GOLDEN_MATERIALS_CACHE=\"${CMAKE_CURRENT_BINARY_DIR}/materials_cache\"")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/materials_cache")
if(TARGET de_gpu_d3d12)
    # D3D12: the Agility runtime next to the executable and its exports in it (DENG-285), and the
    # vendored WARP (test-only, third_party/README.md rule 10; de-golden is a test executable).
    target_compile_definitions(de_golden PRIVATE DE_GOLDEN_D3D12_AGILITY=1)
    if(COMMAND de_d3d12_agility_deploy)
        de_d3d12_agility_deploy(de_golden)
    endif()
    if(DE_D3D12_WARP_DLL)
        de_d3d12_warp_deploy(de_golden)
    endif()
endif()

# ---- CTests without a GPU -----------------------------------------------------------------------
# golden.config: scenes.toml and the registered scenes match both ways (--list loads and checks
# the configuration, then lists). golden.update_refs_refused_on_ci: --update-refs with CI set must
# refuse before it touches anything (it points at a scratch directory all the same).
add_test(NAME golden.config COMMAND de_golden --list)
add_test(NAME golden.update_refs_refused_on_ci
    COMMAND de_golden --update-refs --refs "${CMAKE_CURRENT_BINARY_DIR}/refs_ci_check")
set_tests_properties(golden.config PROPERTIES LABELS "golden" TIMEOUT 60)
set_tests_properties(golden.update_refs_refused_on_ci PROPERTIES
    LABELS "golden" TIMEOUT 60
    ENVIRONMENT "CI=true"
    PASS_REGULAR_EXPRESSION "--update-refs refused: CI is set")

# ---- CTests: golden.<backend>.<scene> ----------------------------------------------------------
# Scene names come from the [scene.<name>] tables of scenes.toml. Every test writes its image (and
# for a failure the reference and the FLIP heat map) under <build>/golden/<backend>/<tier>/, which
# CI uploads.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scenes.toml")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/scenes.toml" _golden_tables REGEX "^\\[scene\\.[a-z0-9_]+\\]")
set(_golden_out "${CMAKE_BINARY_DIR}/golden")
file(MAKE_DIRECTORY "${_golden_out}")
foreach(_golden_backend IN ITEMS metal vulkan d3d12)
    if(NOT TARGET de_gpu_${_golden_backend})
        continue()
    endif()
    set(_golden_args --backend ${_golden_backend})
    if(_golden_backend STREQUAL "d3d12" AND DE_D3D12_WARP_DLL)
        list(APPEND _golden_args --warp) # the D3D12 references are WARP images
    endif()
    foreach(_golden_line IN LISTS _golden_tables)
        string(REGEX REPLACE "^\\[scene\\.([a-z0-9_]+)\\].*" "\\1" _golden_scene "${_golden_line}")
        set(_golden_test golden.${_golden_backend}.${_golden_scene})
        set(_golden_scene_args ${_golden_args})
        if(_golden_scene MATCHES "^rt_")
            list(REMOVE_ITEM _golden_scene_args --warp) # actual hardware for S17 RT/Base variants
        endif()
        add_test(NAME ${_golden_test}
            COMMAND de_golden ${_golden_scene_args} --scene ${_golden_scene} --out "${_golden_out}")
        set_tests_properties(${_golden_test} PROPERTIES
            LABELS "golden;gpu"
            TIMEOUT 300 # cooks its shaders (Slang) first
            SKIP_RETURN_CODE 77
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
    endforeach()
endforeach()

# DENG-343 production Base-tier scene sources and isolated cook cache.
target_compile_definitions(de_golden PRIVATE
    "DE_GOLDEN_BASE_TIER_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/base_tier/assets\""
    "DE_GOLDEN_BASE_TIER_BAKES=\"${CMAKE_CURRENT_BINARY_DIR}/base_tier_bakes\""
    "DE_GOLDEN_BASE_TIER_CACHE=\"${CMAKE_CURRENT_BINARY_DIR}/base_tier_cache\"")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/base_tier_cache")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/base_tier_bakes")

# S17 reference scenes use the existing asset loader and original S14 gallery assets.
target_sources(de_golden PRIVATE pathtracer/pathtracer_scenes.c)
target_compile_definitions(de_golden PRIVATE
    "DE_GOLDEN_PT_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/pathtracer/assets\"")

# S17 actual RT-effect and Base-fallback goldens share the original production asset loader.
target_sources(de_golden PRIVATE rt/rt_scenes.c)
target_compile_definitions(de_golden PRIVATE
    "DE_GOLDEN_RT_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
foreach(_golden_backend IN ITEMS metal vulkan d3d12)
    foreach(_effect IN ITEMS shadows_pole restir_many_lights ao_corner reflections_mirror_rough gi_cornell refraction_glass foliage_shadow)
        if(TEST golden.${_golden_backend}.rt_${_effect})
            set_tests_properties(golden.${_golden_backend}.rt_${_effect} PROPERTIES LABELS "golden;rt;needs-hw;gpu")
            set_tests_properties(golden.${_golden_backend}.rt_${_effect}_base PROPERTIES LABELS "golden;base;gpu")
        endif()
    endforeach()
endforeach()

# DENG-262 text scenes use production cooking, atlas upload and screen/world stages.
target_sources(de_golden PRIVATE text/text_scenes.c)
target_link_libraries(de_golden PRIVATE de_text)
target_compile_definitions(de_golden PRIVATE "DE_GOLDEN_TEXT_ROOT=\"${DE_ROOT_DIR}\"")

# DENG-202 stable side-view pixel-art reference scenes.
add_subdirectory(pixel2d)
