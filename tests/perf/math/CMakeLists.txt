# tests/perf/math: fast-math SIMD benchmark (DENG-242). `bench_math` times every available ISA
# against the scalar reference and prints the speedups:
#     build/<preset>/tests/perf/math/bench_math            full run (a few seconds)
#     build/<preset>/tests/perf/math/bench_math --quick    what CTest runs (label perf)
# The CTest entry fails only when an ISA's cull result differs from the scalar one; timings are
# printed, never asserted, so a loaded machine cannot fail the build.
if(NOT DE_BUILD_TESTS)
    return()
endif()
add_executable(bench_math bench_math.c)
target_link_libraries(bench_math PRIVATE de_core de_build_flags)
target_include_directories(bench_math PRIVATE "${DE_ROOT_DIR}/src")
set_target_properties(bench_math PROPERTIES FOLDER "dengine/tests")
if(NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    add_test(NAME perf.math COMMAND bench_math --quick)
    set_tests_properties(perf.math PROPERTIES LABELS "perf;math" TIMEOUT 120)
endif()
