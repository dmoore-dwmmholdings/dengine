# src/services/steam: module `steam` (target de_steam), layer host.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME steam
    LAYER host
    SOURCES steam_stub.c
    DEPS core platform
    OPTIONAL_DEPS net
    OPTIONAL_OPTION DE_WITH_STEAM
)
