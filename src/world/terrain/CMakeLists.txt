# src/world/terrain: module `terrain` (target de_terrain), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME terrain
    LAYER gameplay
    SOURCES terrain_stub.c
    DEPS ecs asset
    OPTIONAL_DEPS render physics
)
