# src/net: module `net` (target de_net), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME net
    LAYER gameplay
    SOURCES net_stub.c
    DEPS platform ecs reflect
    OPTIONAL_DEPS physics
    OPTIONAL_OPTION DE_WITH_NET
    DETERMINISTIC
)
