# tests/unit/asset: unit tests of module `asset` (de_asset). Shared file (S07): later tasks
# append their own de_add_test() calls or SOURCES below; do not rewrite other tasks' lines.

# DENG-138: manifest parser and the de_asset_system skeleton (manifest.*, asset_system.*).
# data/ holds the fixtures (mounted as adata:/); examples/ is mounted as aex:/ so the shipped
# examples/assets.toml is what the example test parses.
de_add_test(NAME asset SOURCES test_manifest.c LIBS de_asset de_platform)
if(TARGET test_asset)
    # The tests read the internal headers under src/ ("asset/manifest_internal.h", ...).
    target_include_directories(test_asset PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_asset PRIVATE
        "DE_ASSET_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_ASSET_EXAMPLES_DIR=\"${DE_ROOT_DIR}/examples\"")
endif()

# DENG-139: asset IDs, globs on IDs, manifest resolution and validation. Own executable (CTest
# names asset_id.*, glob.*, resolve.*). data/ is mounted as rdata:/, examples/ as rex:/, and a
# scratch directory in the build tree as rtmp:/ (writable).
de_add_test(NAME asset_resolve SOURCES test_asset_id.c test_glob.c test_resolve.c
            LIBS de_asset de_platform)
if(TARGET test_asset_resolve)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_asset_resolve_tmp")
    target_include_directories(test_asset_resolve PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_asset_resolve PRIVATE
        "DE_ASSET_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_ASSET_EXAMPLES_DIR=\"${DE_ROOT_DIR}/examples\""
        "DE_ASSET_RESOLVE_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_asset_resolve_tmp\"")
endif()

# DENG-140: slots, typed handles, refcounts, asset type registry (slots.c, asset_types.c,
# typed_access.c). test_slots.c holds the `test_bytes` fixture type that later S07 tests reuse.
de_add_test(NAME asset_slots SOURCES test_slots.c test_asset_types.c LIBS de_asset de_platform
            TIMEOUT 300)
if(TARGET test_asset_slots)
    target_include_directories(test_asset_slots PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-141: load API, status and wait over a scripted fake backend (fake_backend.h, implemented
# with the shared fixture in test_backend_fake.c). data/ is mounted as ldata:/, examples/ as
# lex:/. de_test_support: the alloc guard (status allocates nothing).
de_add_test(NAME asset_load SOURCES test_load_api.c test_backend_fake.c
            LIBS de_asset de_ecs de_reflect de_platform de_test_support)
if(TARGET test_asset_load)
    target_include_directories(test_asset_load PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_asset_load PRIVATE
        "DE_ASSET_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_ASSET_EXAMPLES_DIR=\"${DE_ROOT_DIR}/examples\"")
endif()

# DENG-142: the load pipeline (pipeline.c and its stages) on real files: the tests write their
# fixtures into a scratch directory of the build tree (mounted as pipe:/); data/pipeline/ holds
# the checked-in zstd payload (mounted as pdata:/). stress_10k is the TSan stress run.
de_add_test(NAME asset_pipeline SOURCES test_pipeline.c
            LIBS de_asset de_platform de_task de_test_support de_tp_zstd
            LABELS unit tsan TIMEOUT 300)
if(TARGET test_asset_pipeline)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_asset_pipeline_tmp")
    target_include_directories(test_asset_pipeline PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_asset_pipeline PRIVATE
        "DE_ASSET_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_ASSET_PIPELINE_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_asset_pipeline_tmp\"")
endif()

# DENG-144: budgets, LRU eviction, resident bundles, flush, stats (budget.c, stats.c). A scripted
# backend in the test file; budget.pipeline_reload runs the real pipeline on files written into a
# scratch directory of the build tree (mounted as bud:/).
de_add_test(NAME asset_budget SOURCES test_budget.c LIBS de_asset de_platform)
if(TARGET test_asset_budget)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_asset_budget_tmp")
    target_include_directories(test_asset_budget PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_asset_budget PRIVATE
        "DE_ASSET_BUDGET_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_asset_budget_tmp\"")
endif()

# DENG-143: asset ECS events and the DeAssetSystemRef resource (test_asset_events.c: a world, a
# scripted backend; data/events/ mounted as evdata:/), and real loads through the app host
# (test_asset_host.c: de__engine_create and de__loop_run with the test_bytes type; data/host/
# mounted as dev:/; links de_host and reads its internal headers).
de_add_test(NAME asset_events SOURCES test_asset_events.c
            LIBS de_asset de_ecs de_reflect de_platform de_test_support)
if(TARGET test_asset_events)
    target_include_directories(test_asset_events PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_asset_events PRIVATE
        "DE_ASSET_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
endif()
de_add_test(NAME asset_host SOURCES test_asset_host.c LIBS de_host TIMEOUT 120)
if(TARGET test_asset_host)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_asset_host_tmp")
    target_include_directories(test_asset_host PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_asset_host PRIVATE
        "DE_ASSET_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_ASSET_HOST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_asset_host_tmp\"")
endif()

# DENG-212: .dpak reader (test_dpak_reader.c) and the pack mount stack through the real pipeline
# (test_mount.c). The packs are built in-process with the DENG-211 writer (de_pack_lib), which is
# linked into this test only: de_asset never links it. The writer is a tool library, so these
# tests exist only when tools are built. Scratch directories of the build tree are mounted as
# dpr:/ and mnt:/. Run under ASan and TSan (labels): untrusted pack bytes, I/O and frame threads.
if(DE_BUILD_TOOLS)
    de_add_test(NAME asset_pack SOURCES test_dpak_reader.c test_mount.c pack_test_util.h
                LIBS de_asset de_platform de_task de_test_support de_pack_lib de_tp_zstd
                LABELS unit tsan TIMEOUT 300)
    if(TARGET test_asset_pack)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_asset_pack_dpr"
                            "${CMAKE_CURRENT_BINARY_DIR}/test_asset_pack_mnt")
        target_include_directories(test_asset_pack PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_definitions(test_asset_pack PRIVATE
            "DE_DPAK_READER_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_asset_pack_dpr\""
            "DE_MOUNT_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_asset_pack_mnt\"")
    endif()
endif()

# DENG-214: asset hot reload (test_asset_hot_reload.c: source edits, dependents, failures, extra
# sources, manifest edits, deferred unloads, pack overrides, the real file watcher) and the
# manifest diff (test_manifest_diff.c). The files go into a scratch directory of the build tree
# (mounted as hr:/). With tools, the pack writer (de_pack_lib) builds the override pack of
# hot_reload.pack_override_skipped (DE_HR_WITH_PACK).
de_add_test(NAME asset_hot_reload SOURCES test_asset_hot_reload.c test_manifest_diff.c
            LIBS de_asset de_platform de_task de_test_support LABELS unit tsan TIMEOUT 300)
if(TARGET test_asset_hot_reload)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_asset_hot_reload_tmp")
    target_include_directories(test_asset_hot_reload PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_asset_hot_reload PRIVATE
        "DE_ASSET_HOT_RELOAD_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_asset_hot_reload_tmp\"")
    if(DE_BUILD_TOOLS)
        target_link_libraries(test_asset_hot_reload PRIVATE de_pack_lib de_tp_zstd)
        target_compile_definitions(test_asset_hot_reload PRIVATE DE_HR_WITH_PACK=1)
    else()
        target_compile_definitions(test_asset_hot_reload PRIVATE DE_HR_WITH_PACK=0)
    endif()
endif()

# DENG-215: the built-in `blob`, `scene` and `prefab` types (src/asset/blob_type.c,
# src/asset/scene/): blob bytes, the TOML scene/prefab importer, the cooked format, the loader and
# de_prefab_instantiate. data/scenes/ is mounted read-only as scn:/. The cook tests run the
# importer through the runtime pipeline and through a de_cooker (as de-cook does); both need
# the cooker (DE_HAS_COOKER builds), and skip elsewhere.
de_add_test(NAME asset_scene SOURCES test_blob_type.c test_scene_cook.c
            LIBS de_asset de_ecs de_reflect de_task de_platform TIMEOUT 120)
if(TARGET test_asset_scene)
    target_include_directories(test_asset_scene PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_asset_scene PRIVATE
        "DE_ASSET_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
    if(DE_BUILD_TOOLS)
        target_link_libraries(test_asset_scene PRIVATE "$<$<CONFIG:Debug,Dev>:de_cooker>")
    endif()
endif()

# DENG-662: compiler location metadata used by editor reload notifications.
de_add_test(NAME asset_diagnostic SOURCES test_diagnostic.c LIBS de_asset)
if(TARGET test_asset_diagnostic)
    target_include_directories(test_asset_diagnostic PRIVATE "${DE_ROOT_DIR}/src")
endif()
