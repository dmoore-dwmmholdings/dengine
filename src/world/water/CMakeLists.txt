# src/world/water: module `water` (target de_water), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME water
    LAYER gameplay
    SOURCES water_stub.c
    DEPS ecs asset
    OPTIONAL_DEPS render physics
)
