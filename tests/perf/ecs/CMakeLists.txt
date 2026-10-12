# tests/perf/ecs: the ECS performance baseline (DENG-353). `bench_ecs` times a world of 1M
# entities: spawn, pos += vel*dt with de_query_each and de_query_par_each, add/remove and
# spawn/despawn churn of 100k per frame, snapshot take/restore and the world checksum:
#     build/<preset>/tests/perf/ecs/bench_ecs            full run (use a Release build)
#     build/<preset>/tests/perf/ecs/bench_ecs --quick    what CTest runs (labels perf, bench)
# The CTest entry fails only when results disagree (each vs par_each checksums, restore vs take);
# timings are printed, never asserted. The full numbers are on the Trama page "Module: ecs".
if(NOT DE_BUILD_TESTS)
    return()
endif()
add_executable(bench_ecs bench_ecs.c)
target_link_libraries(bench_ecs PRIVATE de_ecs de_build_flags)
set_target_properties(bench_ecs PROPERTIES FOLDER "dengine/tests")
if(NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    add_test(NAME perf.ecs COMMAND bench_ecs --quick)
    set_tests_properties(perf.ecs PROPERTIES LABELS "perf;bench;ecs" TIMEOUT 120)
endif()
