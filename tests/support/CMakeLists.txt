# tests/support: libraries for tests (not engine code, never installed).
#
# de_test_support (static): the alloc guard, de_alloc_guard.h (DENG-155). Any test links it with
# `de_add_test(NAME x SOURCES ... LIBS de_test_support)`; S06's zero-alloc frame test (DENG-97)
# does. It links de_core PUBLIC.
#
# Raw malloc-family calls are counted where the platform allows it (DE__GUARD_HAS_RAW=1):
#   - macOS: the shared library de_malloc_interpose (DYLD_INTERPOSE), linked PUBLIC, so every
#     executable that links de_test_support loads it at launch. Not in ASan or TSan builds: their
#     runtimes interpose malloc themselves, and a second interposer would hand them blocks they
#     did not allocate.
#   - Linux (GCC or Clang with GNU ld or lld): malloc_interpose.c inside de_test_support, and
#     `--wrap` for each function on the link line of every executable that links it.
#   - Elsewhere (Windows): not counted; de_alloc_guard_counts_raw() returns false.

add_library(de_test_support STATIC de_alloc_guard.c de_alloc_guard.h)
# tests/unit is where de_test.h lives (DE_ALLOC_GUARD_ASSERT_ZERO uses its FAIL).
target_include_directories(de_test_support
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${DE_ROOT_DIR}/tests/unit"
    PRIVATE "${DE_ROOT_DIR}/src")
target_link_libraries(de_test_support PUBLIC de_core PRIVATE de_build_flags)
set_target_properties(de_test_support PROPERTIES FOLDER "dengine/tests")

set(_de_guard_raw 0)
if(APPLE AND NOT "address" IN_LIST DE_SANITIZERS AND NOT "thread" IN_LIST DE_SANITIZERS)
    add_library(de_malloc_interpose SHARED malloc_interpose.c)
    target_include_directories(de_malloc_interpose PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${DE_ROOT_DIR}/include")
    target_link_libraries(de_malloc_interpose PRIVATE de_build_flags)
    set_target_properties(de_malloc_interpose PROPERTIES FOLDER "dengine/tests")
    target_link_libraries(de_test_support PUBLIC de_malloc_interpose)
    set(_de_guard_raw 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT DE_COMPILER_FRONTEND STREQUAL "MSVC")
    target_sources(de_test_support PRIVATE malloc_interpose.c)
    set(_de_wrap_opts "")
    foreach(_de_fn malloc calloc realloc posix_memalign aligned_alloc free)
        list(APPEND _de_wrap_opts "LINKER:--wrap=${_de_fn}")
    endforeach()
    # Pull malloc_interpose.o out of the archive even when nothing else in the executable
    # references it, so the wrapped references always resolve.
    list(APPEND _de_wrap_opts "LINKER:--undefined=__wrap_malloc")
    target_link_options(de_test_support INTERFACE ${_de_wrap_opts})
    set(_de_guard_raw 1)
endif()
target_compile_definitions(de_test_support PRIVATE DE__GUARD_HAS_RAW=${_de_guard_raw})
