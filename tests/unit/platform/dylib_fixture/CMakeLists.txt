# The shared library loaded by test_dylib.c (DENG-249): SHARED, so the file name follows the
# platform convention that de_dylib_platform_name() produces (libdylib_fixture.dylib,
# libdylib_fixture.so, dylib_fixture.dll). No engine code, so no DE_API or de_ module rules.
add_library(dylib_fixture SHARED fixture.c)
target_link_libraries(dylib_fixture PRIVATE de_build_flags)
set_target_properties(dylib_fixture PROPERTIES
    FOLDER "dengine/tests"
    C_VISIBILITY_PRESET hidden)

# ASan on Apple platforms (DENG-361): dlclose does not unregister the library's globals, so when
# dlopen maps the library again at the same address, ASan's default ODR check (shadow poisoning)
# sees the old registration and aborts with an odr-violation on `fixture_value`. The ODR
# indicator scheme keeps the "registered" flag in the library's own data, which a fresh mapping
# resets. Clang on Linux already uses indicators by default; GCC and MSVC have no such flag.
if("address" IN_LIST DE_SANITIZERS AND APPLE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(dylib_fixture PRIVATE -fsanitize-address-use-odr-indicator)
endif()
