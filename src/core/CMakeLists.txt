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
        de_spsc.h
        spsc.c
        de_mpmc.h
        mpmc.c
        hash.c
        name.c
        str.c
        sort.c
        de_strbuf.h
        de_sort.h
        de_array.h
        array.c
        de_hashmap.h
        hashmap.c
        de_bitset.h
        de_ring.h
        log.c
        log_sinks.c
        mem_internal.h
        tlsf.c
        heap.c
        pool.c
        mem_tags.c
        tracking_alloc.c
        de_cvar_internal.h
        cvar.c
        cvar_toml.c
        math/math_internal.h
        math/vec.c
        math/quat.c
        math/mat.c
        math/affine.c
        math/geom.c
        math/proj.c
        math/dmath_reduce.h
        math/dmath.c
        math/dmath64.c
        math/rng.c
        math/fpenv.c
)

# DENG-148: thread scratch uses a pthread key destructor; glibc < 2.34 keeps it in libpthread.
# src/platform finds Threads on Linux and makes Threads::Threads global (and links it into the
# aggregate `dengine`), so link it here when it exists at generate time. Calling find_package
# here as well would create the imported target in this directory first, and platform's
# IMPORTED_GLOBAL promotion then fails.
target_link_libraries(de_core PRIVATE "$<TARGET_NAME_IF_EXISTS:Threads::Threads>")

# DENG-153: XXH3 hashing comes from de_tp_xxhash.
# de_tp_xxhash is recorded in DE_MODULE_LIBS too, so the aggregate `dengine` library links it.
# The dev intern table locks a de_mutex, so core links platform (core and platform form one
# layering group; CMake resolves the static-library cycle with platform's DEPS core).
target_link_libraries(de_core PRIVATE de_tp_xxhash de_platform)
# DENG-150: logging (log.c) also needs platform through this link: the de-log thread, TLS keys,
# semaphores and de_time_now_ns.
set_property(TARGET de_core APPEND PROPERTY DE_MODULE_LIBS de_tp_xxhash)
if(APPLE)
    # CMake repeats the core <-> platform static-library cycle on every link line, because GNU ld
    # needs the second scan. Apple's linker resolves archives in any order and warns about the
    # repeat ("ignoring duplicate libraries"); silence that warning for everything linking core.
    include(CheckLinkerFlag)
    check_linker_flag(C "LINKER:-no_warn_duplicate_libraries" DE_LD_NO_WARN_DUP_LIBS)
    if(DE_LD_NO_WARN_DUP_LIBS)
        target_link_options(de_core INTERFACE "LINKER:-no_warn_duplicate_libraries")
    endif()
endif()

# DENG-154: cvars read and write config.toml through the vendored tomlc17 parser. Recorded in
# DE_MODULE_LIBS too, so the aggregate `dengine` library links it.
target_link_libraries(de_core PRIVATE de_tp_tomlc17)
set_property(TARGET de_core APPEND PROPERTY DE_MODULE_LIBS de_tp_tomlc17)

# DENG-241: the fast math (src/core/math/) calls <math.h> (sqrtf, sinf, tanf, ...). glibc keeps
# those in libm, which is not linked by default; macOS and Windows have them in the C library.
# Recorded in DE_MODULE_LIBS too, so the aggregate `dengine` library links it.
if(UNIX AND NOT APPLE)
    target_link_libraries(de_core PRIVATE m)
    set_property(TARGET de_core APPEND PROPERTY DE_MODULE_LIBS m)
endif()

# DENG-243: the deterministic math (SPEC §5.7). Core is not a deterministic module, so these
# files get the deterministic flags (-ffp-contract=off, /fp:precise) one by one.
de_mark_deterministic_sources(math/dmath.c math/dmath64.c math/rng.c math/fpenv.c)
