# src/ecs: module `ecs` (target de_ecs), layer runtime.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME ecs
    LAYER runtime
    SOURCES ecs_stub.c
    DEPS core task reflect
    DETERMINISTIC
)
