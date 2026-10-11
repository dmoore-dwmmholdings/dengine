# src/physics: module `physics` (target de_physics), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
# 2D physics (src/physics/2d/*.c) is part of this deterministic module, not its own target.
file(GLOB de_physics_2d_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/2d/*.c")
de_add_module(
    NAME physics
    LAYER gameplay
    SOURCES physics_stub.c ${de_physics_2d_sources}
    DEPS ecs
    OPTIONAL_OPTION DE_WITH_PHYSICS
    DETERMINISTIC
)
