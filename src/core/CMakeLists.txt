# src/core: module `core` (target de_core), layer foundation.
# Shared file: later S02 tasks append their sources to the SOURCES list, one per line.
de_add_module(
    NAME core
    LAYER foundation
    SOURCES
        base.c
        error.c
        panic.c
        arena.c
        frame_arena.c
        scratch.c
        vmem_posix.c
        vmem_win32.c
)

# DENG-148: thread scratch uses a pthread key destructor; glibc < 2.34 keeps it in libpthread.
# src/platform finds Threads on Linux and makes Threads::Threads global (and links it into the
# aggregate `dengine`), so link it here when it exists at generate time. Calling find_package
# here as well would create the imported target in this directory first, and platform's
# IMPORTED_GLOBAL promotion then fails.
target_link_libraries(de_core PRIVATE "$<TARGET_NAME_IF_EXISTS:Threads::Threads>")
