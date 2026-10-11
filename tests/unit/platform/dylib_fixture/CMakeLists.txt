# The shared library loaded by test_dylib.c (DENG-249): SHARED, so the file name follows the
# platform convention that de_dylib_platform_name() produces (libdylib_fixture.dylib,
# libdylib_fixture.so, dylib_fixture.dll). No engine code, so no DE_API or de_ module rules.
add_library(dylib_fixture SHARED fixture.c)
target_link_libraries(dylib_fixture PRIVATE de_build_flags)
set_target_properties(dylib_fixture PROPERTIES
    FOLDER "dengine/tests"
    C_VISIBILITY_PRESET hidden)
