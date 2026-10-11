# tests/unit/core: unit tests of module `core` (de_core). Shared file: later S02 tasks append their
# test sources to the SOURCES list, one per line.
find_package(Threads REQUIRED)

de_add_test(NAME core
    SOURCES
        test_base.c
        test_error.c
        test_arena.c
        test_scratch.c
    LIBS de_core Threads::Threads
)
