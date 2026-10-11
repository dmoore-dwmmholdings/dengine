# src/core: module `core` (target de_core), layer foundation.
# Shared file: later S02 tasks append their sources to the SOURCES list, one per line.
de_add_module(
    NAME core
    LAYER foundation
    SOURCES
        base.c
        error.c
        panic.c
)
