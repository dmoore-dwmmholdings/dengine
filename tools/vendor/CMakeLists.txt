# tools/vendor: vendored-library manager (third_party/vendor.toml, SPEC §22).
#
#   vendor_selftest           vendor.py against local file:// archives in a temporary fake
#                             repository (offline). The real `vendor.py verify` needs the pinned
#                             archives (network or $DE_VENDOR_CACHE), so CI runs it as its own
#                             check step, not through ctest.
#   vendor_prebuilt_selftest  the `kind = "prebuilt"` entries (DENG-390): fake per-host archives,
#                             fetch/verify/--all-hosts/extract, and a configure of a small project
#                             that imports one through cmake/DePrebuilt.cmake, with this build's
#                             cmake and generator. Offline too.
if(NOT DE_BUILD_TESTS)
    return()
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_test(NAME vendor_selftest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest.py")
set_tests_properties(vendor_selftest PROPERTIES
    LABELS check
    TIMEOUT 120)

set(_de_prebuilt_test_args --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}")
if(CMAKE_MAKE_PROGRAM)
    list(APPEND _de_prebuilt_test_args --make-program "${CMAKE_MAKE_PROGRAM}")
endif()
add_test(NAME vendor_prebuilt_selftest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/prebuilt_selftest.py"
            ${_de_prebuilt_test_args})
set_tests_properties(vendor_prebuilt_selftest PROPERTIES
    LABELS check
    TIMEOUT 300)
unset(_de_prebuilt_test_args)
