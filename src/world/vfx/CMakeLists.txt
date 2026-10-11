# src/world/vfx: module `vfx` (target de_vfx), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME vfx
    LAYER gameplay
    SOURCES vfx_stub.c
    DEPS ecs asset
    OPTIONAL_DEPS render physics
    OPTIONAL_OPTION DE_WITH_VFX
)
