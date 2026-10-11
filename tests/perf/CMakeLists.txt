# tests/perf/: benchmarks, one directory per module (tests/perf/<module>/). Each one registers its
# benchmark as a short CTest entry with the label `perf` (it checks results, never timings), and
# can be run by hand for the full numbers. Directories are added automatically in sorted order.
de_add_subdirectories()
