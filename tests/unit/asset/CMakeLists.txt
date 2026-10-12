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
