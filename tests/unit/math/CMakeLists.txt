# tests/unit/math: unit tests of the fast-math scalar reference in src/core/math/ (de_core).
# Shared file: later S03 tasks (DENG-242 SIMD, DENG-243 dmath/RNG, DENG-244 large-world) append
# their test sources to the SOURCES list or add their own de_add_test() below.
de_add_test(NAME math
    SOURCES
        test_vec.c
        test_quat.c
        test_mat.c
        test_geom.c
    LIBS de_core
)
