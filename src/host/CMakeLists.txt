# src/host: module `host` (target de_host), layer host. SPEC §10: the app host (engine
# create/destroy, game.toml, de_host_run, main loop, game attachment).
# Shared file (S06, append-only): later tasks add their sources with target_sources() below, one
# block per task. Do not reorder or rewrite another task's lines.
de_add_module(
    NAME host
    LAYER host
    SOURCES
        engine_internal.h
        host_internal.h
        engine.c
        game_toml.c
        host_run.c
        loop.c
        game_module.c
    DEPS
        core platform task reflect ecs asset anim input text sky terrain water decal streaming
        tilemap crash mods save
    OPTIONAL_DEPS gpu gpu_d3d12 gpu_vulkan gpu_metal render physics audio net ui vfx nav video steam
)

# DENG-90: game.toml is read with the vendored tomlc17, through core's entry point
# (core/de_toml_internal.h; never toml_set_option). Recorded in DE_MODULE_LIBS too, so the
# aggregate `dengine` links it.
target_link_libraries(de_host PRIVATE de_tp_tomlc17)
set_property(TARGET de_host APPEND PROPERTY DE_MODULE_LIBS de_tp_tomlc17)
