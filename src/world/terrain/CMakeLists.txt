# src/world/terrain: module `terrain` (target de_terrain), layer gameplay.
# The CPU terrain model (sculpt and paint strokes, auto-paint rules, bake, heightmap resampling,
# tiles) behind the DIDE editor IPC world.terrain, protocol 1.11 (DENG-580). DeTerrain is a render
# component. The module is DETERMINISTIC: all math is de_dmath. The streamed runtime terrain and its
# renderer are DENG-265 and DENG-266.
de_add_module(
    NAME terrain
    LAYER gameplay
    SOURCES terrain.c terrain.h
    DEPS core ecs asset
    OPTIONAL_DEPS render physics
    DETERMINISTIC
)
