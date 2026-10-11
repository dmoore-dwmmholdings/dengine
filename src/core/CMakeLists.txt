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
        math/math_simd.h
        math/simd_kernels.h
        math/simd_dispatch.c
        math/simd_sse.c
        math/simd_avx2.c
        math/simd_neon.c
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

# DENG-242: fast-math SIMD paths. simd_sse.c and simd_avx2.c are built for their ISA only when the
# target is x86-64 (on arm64 they compile to nothing); simd_dispatch.c checks cpuid before it
# selects them, so the rest of the engine keeps the x86-64 baseline. simd_neon.c needs no flag.
# -ffp-contract=off: the SIMD frustum culls rely on separate multiplies and adds to match the
# scalar reference bit for bit; the kernels request FMA explicitly where they want it.
# Never pass these files to de_mark_deterministic_sources (SPEC §5.7): they are fast math.
set(_de_simd_x86 OFF)
if(APPLE)
    # CMAKE_SYSTEM_PROCESSOR is the host's on macOS; the target is CMAKE_OSX_ARCHITECTURES. An
    # x86_64-only build (the Rosetta check) gets the flags. A universal build does not: per-arch
    # flags cannot be passed per source file, so its x86_64 slice falls back to scalar (the ISA
    # files check __SSE4_2__ / __AVX2__). Intel Macs are not a v1 target (DENG-2 Q3).
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        set(_de_simd_x86 ON)
    elseif("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        message(STATUS "dengine: universal build; the x86_64 slice uses scalar fast math")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64|X64)$")
    set(_de_simd_x86 ON)
endif()
set(_de_simd_files math/simd_sse.c math/simd_avx2.c math/simd_neon.c)
if(DE_COMPILER_FRONTEND STREQUAL "GNU")
    set_property(SOURCE ${_de_simd_files} APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang") # clang-cl; MSVC cl does not contract by default
    set_property(SOURCE ${_de_simd_files} APPEND PROPERTY COMPILE_OPTIONS /clang:-ffp-contract=off)
endif()
if(_de_simd_x86)
    if(DE_COMPILER_FRONTEND STREQUAL "MSVC")
        # MSVC cl accepts SSE4.2 intrinsics without a flag; clang-cl needs the target feature.
        if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
            set_property(SOURCE math/simd_sse.c APPEND PROPERTY COMPILE_OPTIONS -msse4.2)
        endif()
        set_property(SOURCE math/simd_avx2.c APPEND PROPERTY COMPILE_OPTIONS /arch:AVX2)
    else()
        set_property(SOURCE math/simd_sse.c APPEND PROPERTY COMPILE_OPTIONS -msse4.2)
        set_property(SOURCE math/simd_avx2.c APPEND PROPERTY COMPILE_OPTIONS -mavx2 -mfma)
    endif()
endif()
# tests/unit/math registers an extra whole-binary run of test_simd at the SSE4.2 level on x86-64.
set_property(GLOBAL PROPERTY DE_MATH_SIMD_X86 ${_de_simd_x86})

# DENG-243: the deterministic math (SPEC §5.7). Core is not a deterministic module, so these
# files get the deterministic flags (-ffp-contract=off, /fp:precise) one by one.
de_mark_deterministic_sources(math/dmath.c math/dmath64.c math/rng.c math/fpenv.c)
