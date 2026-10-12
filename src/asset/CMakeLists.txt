# src/asset: module `asset` (target de_asset), layer content. SPEC §11.
# Shared file (S07): later tasks append their sources to the SOURCES list, one per line, or add
# target_sources() below. Do not reorder or rewrite another task's lines.
de_add_module(
    NAME asset
    LAYER content
    SOURCES
        asset_internal.h
        asset_system.c
        manifest_internal.h
        manifest.c
        slots_internal.h
        slots.c
        asset_types.c
        typed_access.c
    DEPS core platform task reflect
    OPTIONAL_DEPS gpu
)

# DENG-138: the manifest parser reads assets.toml with the vendored tomlc17, through core's entry
# point (core/de_toml_internal.h; never toml_set_option). Recorded in DE_MODULE_LIBS too, so the
# aggregate `dengine` links it.
target_link_libraries(de_asset PRIVATE de_tp_tomlc17)
set_property(TARGET de_asset APPEND PROPERTY DE_MODULE_LIBS de_tp_tomlc17)

# DENG-139: asset IDs, globs on IDs, bundle resolution and validation (asset_id.c, glob.c,
# resolve.c).
target_sources(de_asset PRIVATE asset_id.c glob.c resolve.c resolve_internal.h)

# DENG-141: load requests, status and wait (load.c, status.c), the backend seam
# (backend_internal.h). de_asset_wait asserts outside ECS systems (ecs/in_schedule_internal.h),
# so the module links de_ecs (ecs is in a lower band).
target_sources(de_asset PRIVATE load.c load_internal.h status.c backend_internal.h)
target_link_libraries(de_asset PUBLIC de_ecs)
set_property(TARGET de_asset APPEND PROPERTY DE_MODULE_DEPS ecs)

# DENG-142: the load pipeline, the real backend (pipeline.c), its stages (io_stage.c: source
# reads on the process I/O threads; decompress_stage.c: zstd payloads; load_ctx.c: loaders and
# the de_asset_load_ctx accessors). zstd decodes in the runtime: de_tp_zstd, recorded in
# DE_MODULE_LIBS so the aggregate `dengine` links it.
target_sources(de_asset PRIVATE pipeline.c pipeline_internal.h io_stage.c decompress_stage.c
                                load_ctx.c)
target_link_libraries(de_asset PRIVATE de_tp_zstd)
set_property(TARGET de_asset APPEND PROPERTY DE_MODULE_LIBS de_tp_zstd)

# DENG-144: per-category budgets, residency accounting, LRU eviction of unreferenced assets,
# resident bundles, de_asset_flush_unused (budget.c) and de_asset_get_stats (stats.c).
target_sources(de_asset PRIVATE budget.c budget_internal.h stats.c)
