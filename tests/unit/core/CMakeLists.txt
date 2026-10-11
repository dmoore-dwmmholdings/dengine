# tests/unit/core: unit tests of module `core` (de_core). Shared file: later S02 tasks append their
# test sources to the SOURCES list, one per line.
find_package(Threads REQUIRED)

de_add_test(NAME core
    SOURCES
        test_base.c
        test_error.c
        test_arena.c
        test_scratch.c
        test_array.c
        test_hashmap.c
        test_bitset_ring.c
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

# DENG-151: the container tests include engine-internal headers ("core/de_array.h", ...).
if(TARGET test_core)
    target_include_directories(test_core PRIVATE "${DE_ROOT_DIR}/src")
endif()
