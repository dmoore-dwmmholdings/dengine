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

# DENG-295: the TOML text serializer. Golden files are in data/ (text_*.toml); check_toml.py
# parses the same files with Python's tomllib (an independent TOML parser), and the C tests
# compare the writer's output with them byte for byte.
de_add_test(NAME text SOURCES test_ser_text.c LIBS de_reflect)
if(TARGET test_text)
    target_compile_definitions(test_text PRIVATE
        "DE_REFLECT_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
endif()
if(DE_BUILD_TESTS)
    find_package(Python3 3.10 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # Exit code 77 (skipped) when the interpreter is older than 3.11 (no tomllib).
        add_test(NAME text.check_toml
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_toml.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/data")
        set_tests_properties(text.check_toml PROPERTIES
            LABELS "unit;reflect" SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()
endif()
