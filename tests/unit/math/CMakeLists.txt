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

# DENG-243: deterministic math (de_d*), de_rng and the FP environment. Separate executables, so
# test_dmath and test_rng can be built for x86-64 and run under Rosetta 2 against the same golden
# files (see the Rosetta note on the Trama page "core: Math").
de_add_test(NAME dmath SOURCES test_dmath.c LIBS de_core)
de_add_test(NAME rng SOURCES test_rng.c LIBS de_core)
de_add_test(NAME fpenv SOURCES test_fpenv.c LIBS de_core)
if(TARGET test_dmath)
    target_compile_definitions(test_dmath PRIVATE
        "DE_DMATH_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/vectors/dmath_golden.txt\"")
    target_compile_definitions(test_rng PRIVATE
        "DE_RNG_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/vectors/rng_golden.txt\"")
    # Static check (SPEC §5.7 rule 2): the deterministic sources call no <math.h> function except
    # sqrt / sqrtf.
    add_test(NAME math.dmath_no_libm
        COMMAND "${CMAKE_COMMAND}" "-DDE_ROOT_DIR=${DE_ROOT_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_dmath_no_libm.cmake")
    set_tests_properties(math.dmath_no_libm PROPERTIES LABELS "unit;math")
endif()
