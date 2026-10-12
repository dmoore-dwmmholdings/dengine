# tests/unit/reflect: unit tests of module `reflect` (de_reflect). Shared file: later S04 tasks
# (DENG-295, DENG-296, DENG-297) append their own de_add_test() calls or SOURCES below.
#
# One executable per suite, so the CTest names are reg.<test> and path.<test>. test_types.h holds
# the shared test types (stable: add types, do not change them).
de_add_test(NAME reg
    SOURCES
        test_registry.c
        test_cxx_macros.cpp
    LIBS de_reflect
)
if(TARGET test_reg)
    # The C++ source checks the registration macros under C++17 (DE_TYPE_DESC has a C++ form).
    set_target_properties(test_reg PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
                                              CXX_EXTENSIONS OFF)
endif()

de_add_test(NAME path SOURCES test_path.c LIBS de_reflect)
