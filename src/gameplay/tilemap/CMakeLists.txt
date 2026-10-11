# src/gameplay/tilemap: module `tilemap` (target de_tilemap), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME tilemap
    LAYER gameplay
    SOURCES tilemap_stub.c
    DEPS ecs asset
    OPTIONAL_DEPS render physics
)
