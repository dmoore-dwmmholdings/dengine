# src/gameplay/nav: module `nav` (target de_nav), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME nav
    LAYER gameplay
    SOURCES nav_stub.c
    DEPS ecs
    OPTIONAL_DEPS physics
    OPTIONAL_OPTION DE_WITH_NAV
    DETERMINISTIC
)
