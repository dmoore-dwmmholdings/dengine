# tests/unit/core: unit tests of module `core` (de_core). Shared file: later S02 tasks append their
# test sources to the SOURCES list, one per line.
find_package(Threads REQUIRED)

de_add_test(NAME core
    SOURCES
        test_base.c
        test_error.c
        test_arena.c
        test_scratch.c
        test_hash_name.c
        test_str.c
        test_sort.c
        test_array.c
        test_hashmap.c
        test_bitset_ring.c
        test_log.c
        test_pool.c
        test_tlsf.c
        test_tracking.c
        test_cvar.c
        test_core_stress.c
        test_prof_off.c
        test_prof_tracy.c
    LIBS de_core Threads::Threads
)

# Lock-free queues (DENG-152): one executable per queue, so the CTest names are spsc.<test> and
# mpmc.<test>, and the label `tsan` marks them as the threading tests the TSan job must cover.
# de_platform (for de_thread and the de_sem blocking helpers) links de_core; naming both makes
# Apple ld warn about a duplicate library.
de_add_test(NAME spsc SOURCES test_spsc.c LIBS de_platform LABELS unit tsan)
de_add_test(NAME mpmc SOURCES test_mpmc.c LIBS de_platform LABELS unit tsan)
foreach(_de_queue_test test_spsc test_mpmc)
    if(TARGET ${_de_queue_test})
        # The queues are internal headers (core/de_spsc.h, core/de_mpmc.h) under src/.
        target_include_directories(${_de_queue_test} PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endforeach()
# DENG-370: under TSan, spsc.stress_1e7 took 33-50 s of its 60 s timeout on the GitHub runners
# and timed out when other stress tests ran beside it. TSan checks the access pattern, which 2e6
# elements through a 64-entry ring exercise as well; every other build keeps 1e7.
if(TARGET test_spsc AND "thread" IN_LIST DE_SANITIZERS)
    target_compile_definitions(test_spsc PRIVATE DE_SPSC_STRESS_COUNT=2000000ull)
endif()

# DENG-153: the name threads test starts threads with de_thread_create; the string and sort
# tests include the internal headers "core/de_strbuf.h" and "core/de_sort.h".
target_link_libraries(test_core PRIVATE de_platform)
target_include_directories(test_core PRIVATE "${DE_ROOT_DIR}/src")

# DENG-153: test_hash_name.c again, with src/core/name.c compiled in as Shipping (DE_SHIPPING=1:
# no intern table, de_name_str returns NULL), whatever the build type. Its name.o comes first, so
# the linker never takes de_core's copy (both define the same symbols).
de_add_test(NAME core_name_shipping
    SOURCES
        test_hash_name.c
        ${DE_ROOT_DIR}/src/core/name.c
    LIBS de_core de_platform Threads::Threads
)
if(TARGET test_core_name_shipping)
    target_compile_definitions(test_core_name_shipping PRIVATE DE_SHIPPING=1)
    target_include_directories(test_core_name_shipping PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-154: the cvar tests read tests/unit/core/data/cvars_*.toml and write their saves into the
# build directory (one prefix per executable, so parallel ctest runs do not share files).
target_compile_definitions(test_core PRIVATE
    DE_CVAR_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    DE_CVAR_TEST_OUT_PREFIX="${CMAKE_CURRENT_BINARY_DIR}/test_cvar_")

# DENG-154: test_cvar.c again, with src/core/cvar.c and cvar_toml.c compiled in as Shipping
# (DE_SHIPPING=1: DEV_ONLY cvars are not registered, CHEAT cvars need `cheats`), whatever the build
# type. Its objects come first, so the linker never takes de_core's copies.
de_add_test(NAME core_cvar_shipping
    SOURCES
        test_cvar.c
        ${DE_ROOT_DIR}/src/core/cvar.c
        ${DE_ROOT_DIR}/src/core/cvar_toml.c
    LIBS de_core de_platform de_tp_tomlc17 Threads::Threads
)
if(TARGET test_core_cvar_shipping)
    target_compile_definitions(test_core_cvar_shipping PRIVATE
        DE_SHIPPING=1
        DE_CVAR_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
        DE_CVAR_TEST_OUT_PREFIX="${CMAKE_CURRENT_BINARY_DIR}/test_cvar_shipping_")
    target_include_directories(test_core_cvar_shipping PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-155: the alloc guard of de_test_support (tests/support/), in its own executable so no
# thread or hook of another test runs during its regions. CTest names: guard.<test>.
de_add_test(NAME guard SOURCES test_alloc_guard.c LIBS de_test_support de_platform)

# DENG-94: the profiler. test_core runs test_prof_off.c (the macros without DE_PROFILE) and
# test_prof_tracy.c (the real backend: Tracy when de_tp_tracy is linked in this config).
if(TARGET test_core AND TARGET de_tp_tracy)
    target_compile_definitions(test_core PRIVATE
        "$<$<OR:$<CONFIG:Dev>,$<AND:$<CONFIG:Release>,$<BOOL:${DE_ENABLE_PROFILE}>>>:DE_PROF_TEST_TRACY>")
endif()
# test_prof.c again with src/core/prof/prof.c compiled in under DE_PROF_MOCK, so the front end
# calls the mock backend of the test. Its prof.o comes first, so the linker never takes de_core's
# copy (both define the same symbols). CTest names: core_prof.<suite>.<test>.
de_add_test(NAME core_prof
    SOURCES
        test_prof.c
        ${DE_ROOT_DIR}/src/core/prof/prof.c
    LIBS de_core de_platform Threads::Threads
)
if(TARGET test_core_prof)
    target_compile_definitions(test_core_prof PRIVATE DE_PROF_MOCK=1)
    target_include_directories(test_core_prof PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-94: prof_no_tracy_in_shipping (label `check`): no Tracy symbol in a Shipping `dengine`. In
# a Shipping build it checks this build's library; otherwise it configures and builds a Shipping
# `dengine` next to this build (build/<preset>/prof_shipping/, about 140 compile steps: SDL3 is
# not compiled). See the script header.
if(CMAKE_NM)
    set(_de_prof_nm "${CMAKE_NM}")
else()
    find_program(_de_prof_nm NAMES llvm-nm nm)
endif()
if(TARGET de_tp_tracy)
    set(_de_prof_expect_tracy 1)
else()
    set(_de_prof_expect_tracy 0)
endif()
if(DE_IS_SHIPPING)
    set(_de_prof_is_shipping 1)
else()
    set(_de_prof_is_shipping 0)
endif()
add_test(NAME prof_no_tracy_in_shipping
    COMMAND "${CMAKE_COMMAND}"
        "-DDE_SOURCE_DIR=${DE_ROOT_DIR}"
        "-DDE_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DDE_IS_SHIPPING=${_de_prof_is_shipping}"
        "-DDE_NM=${_de_prof_nm}"
        "-DDE_GENERATOR=${CMAKE_GENERATOR}"
        "-DDE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DDE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DDE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DDE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DDE_HEADLESS=${DE_HEADLESS}"
        "-DDE_PARENT_LIB=$<TARGET_FILE:dengine>"
        "-DDE_EXPECT_TRACY=${_de_prof_expect_tracy}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/prof_no_tracy_in_shipping.cmake")
set_tests_properties(prof_no_tracy_in_shipping PROPERTIES
    LABELS "check;core"
    SKIP_REGULAR_EXPRESSION "prof_no_tracy_in_shipping: SKIPPED"
    TIMEOUT 900)
if(DE_IS_SHIPPING)
    # It builds this build's dengine: no other test may drive the build at the same time.
    set_tests_properties(prof_no_tracy_in_shipping PROPERTIES RUN_SERIAL TRUE)
endif()

# DENG-96: the console command registry. Its own executable, so the CTest names are
# command.<test> and the process-wide registry and cvars start fresh in each test. It includes
# the internal header "core/command_internal.h"; exec files go to a build-tree directory.
de_add_test(NAME command SOURCES test_command.c LIBS de_platform)
if(TARGET test_command)
    target_include_directories(test_command PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_command PRIVATE
        DE_COMMAND_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_command_tmp")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_command_tmp")
endif()
