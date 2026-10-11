# src/input: module `input` (target de_input), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME input
    LAYER gameplay
    SOURCES input_stub.c
    DEPS platform ecs
    DETERMINISTIC
)
