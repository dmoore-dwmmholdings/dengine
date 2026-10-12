# tools/cook/lib: the cooker library `de_cooker` (module `cook` in tools/check_layers/layers.toml;
# SPEC §11.5, §11.6; DENG-209). Its users:
#   - `de-cook` (tools/cook/, DENG-213) cooks a project into packs;
#   - the runtime asset module in Debug and Dev builds (DE_HAS_COOKER): de_asset links it and
#     cooks source assets in-process (src/asset/CMakeLists.txt, cook_stage.c). Release and
#     Shipping never link it (checked by the CTest cooker_link_check, tests/unit/cook/).
#
# Not a de_add_module() module: the aggregate `dengine` library holds module objects in every
# configuration, and the cooker must stay out of Release and Shipping. de_asset records it in
# DE_MODULE_LIBS (per configuration), which is how `dengine` consumers link it.
#
# Include it as "cook/lib/de_cooker.h" (the tools/ include root). It reads the asset module's
# manifest types ("asset/manifest_internal.h", the src/ include root), and the asset module calls
# it, so `asset` and `cook` are one group in layers.toml.
add_library(de_cooker STATIC
    de_cooker.h
    cooker_internal.h
    cooker.c
    importer_registry.c
    import_ctx.c
    settings_hash.c)
target_include_directories(de_cooker
    PUBLIC "${DE_ROOT_DIR}/tools" "${DE_ROOT_DIR}/src")
target_link_libraries(de_cooker
    PUBLIC de_asset de_task de_platform de_core
    PRIVATE de_tp_xxhash de_build_flags)
set_target_properties(de_cooker PROPERTIES FOLDER "dengine/tools")
if(DE_BUILD_SHARED)
    # Linked into the shared `dengine` in Debug and Dev: position-independent, and only the DE_API
    # functions (de_asset_import_*) exported, like the module objects (cmake/DeModule.cmake).
    target_compile_definitions(de_cooker PRIVATE DE_BUILD_SHARED)
    set_target_properties(de_cooker PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
endif()

# DENG-210: the cook cache `.decache` (decache.h): content-addressed entries compressed with zstd,
# and the warm-path index (decache_index.c). zstd is in the runtime already (de_asset).
target_sources(de_cooker PRIVATE decache.h decache.c decache_index.c)
target_link_libraries(de_cooker PRIVATE de_tp_zstd)
