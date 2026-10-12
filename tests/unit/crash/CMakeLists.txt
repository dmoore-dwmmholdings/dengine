# tests/unit/crash: unit tests of module `crash` (de_crash, src/services/crash): the crash report
# bundle (log tail, cvar dump), the next-launch upload of src/platform/crash_common.c and the app
# host's crash wiring (src/host/host_crash.c). DENG-96. Shared file: later tasks append blocks.
#
# One executable, CTest names bundle.<test>. It runs a headless game through de_host_run, so it
# links de_host (which links de_crash, de_platform and de_core), and the alloc guard. It includes
# internal headers (src/ is an include root). DE_BUNDLE_TEST_TMP_DIR (build tree) holds the
# in-process tests' directories; the fork tests use mkdtemp under $TMPDIR.
de_add_test(NAME bundle SOURCES test_bundle.c LIBS de_host de_test_support TIMEOUT 120)
if(TARGET test_bundle)
    target_include_directories(test_bundle PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_bundle PRIVATE
        DE_BUNDLE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_bundle_tmp")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_bundle_tmp")
endif()
