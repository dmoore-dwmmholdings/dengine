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
