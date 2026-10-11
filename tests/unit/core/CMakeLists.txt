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
