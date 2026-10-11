# tools/vendor: vendored-library manager (third_party/vendor.toml, SPEC §22).
#
#   vendor_selftest  vendor.py against local file:// archives in a temporary fake repository
#                    (offline). The real `vendor.py verify` needs the pinned archives (network or
#                    $DE_VENDOR_CACHE), so CI runs it as its own check step, not through ctest.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_test(NAME vendor_selftest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest.py")
set_tests_properties(vendor_selftest PROPERTIES
    LABELS check
    TIMEOUT 120)
