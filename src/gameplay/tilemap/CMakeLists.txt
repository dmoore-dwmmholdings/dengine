# src/gameplay/tilemap: module `tilemap` (target de_tilemap), layer gameplay. SPEC §21.4, §12.12;
# DENG-80. The `tilemap` asset type (cooked DTMP, tilemap_cooked.h, written by the importer in
# tools/cook/importers/tilemap/), the DeTilemap component, chunked sprite rendering, 2D colliders
# and entity spawns. It finds DeSprite and DeCollider2D by name at run time, so it needs the
# renderer and 2D physics only for their headers; chain shapes call 2D physics when it is built.
de_add_module(
    NAME tilemap
    LAYER gameplay
    SOURCES
        tilemap_cooked.h
        tilemap_internal.h
        tilemap_runtime.c
        tilemap_chunks.c
        tilemap_collision.c
        tilemap_spawn.c
    DEPS ecs asset reflect
    OPTIONAL_DEPS render physics
)

# Debug and Dev builds with the tools (DE_HAS_COOKER): the runtime type takes the cooker library's
# built-in tilemap importer (de_cooker_builtin_importer), as src/gpu/shader/ does. Release and
# Shipping link no cooker. tools/ is added after src/, so the target is named in generator
# expressions.
target_link_libraries(de_tilemap PRIVATE
    "$<$<AND:$<CONFIG:Debug,Dev>,$<TARGET_EXISTS:de_cooker>>:de_cooker>")
