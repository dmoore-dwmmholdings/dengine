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
    DEPS core platform task reflect
    OPTIONAL_DEPS gpu
)

# DENG-138: the manifest parser reads assets.toml with the vendored tomlc17, through core's entry
# point (core/de_toml_internal.h; never toml_set_option). Recorded in DE_MODULE_LIBS too, so the
# aggregate `dengine` links it.
target_link_libraries(de_asset PRIVATE de_tp_tomlc17)
set_property(TARGET de_asset APPEND PROPERTY DE_MODULE_LIBS de_tp_tomlc17)
