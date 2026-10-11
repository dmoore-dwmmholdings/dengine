# Unit tests of the platform module (src/platform/): threads, sync, TLS, atomics, time.
de_add_test(NAME platform SOURCES test_thread.c test_time.c LIBS de_platform)
if(TARGET test_platform)
    # The atomics test uses the internal header src/platform/de_atomic.h.
    target_include_directories(test_platform PRIVATE "${DE_ROOT_DIR}/src")
endif()

# Dynamic libraries (DENG-249): test_dylib loads the fixture library from dylib_fixture/.
add_subdirectory(dylib_fixture)
de_add_test(NAME dylib SOURCES test_dylib.c LIBS de_platform)
if(TARGET test_dylib)
    add_dependencies(test_dylib dylib_fixture)
    target_compile_definitions(test_dylib PRIVATE
        "DE_DYLIB_FIXTURE_PATH=\"$<TARGET_FILE:dylib_fixture>\""
        "DE_DYLIB_FIXTURE_DIR=\"$<TARGET_FILE_DIR:dylib_fixture>\"")
endif()

# Precise sleep and frame pacing (DENG-249). Label `timing`: wall-clock bounds, loosened when
# DE_CI or CI is set (shared runners).
de_add_test(NAME pacing SOURCES test_pacing.c LIBS de_platform LABELS unit timing)
if(TARGET test_pacing AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    # Run the timing tests alone: under `ctest -j` the stress tests of other modules preempt the
    # spin for tens of milliseconds. de_add_test() registers one CTest entry per TEST from a
    # generated file whose path its include file leaves in `_de_tests`; this file runs after it
    # (TEST_INCLUDE_FILES keeps the order) and marks every entry in it RUN_SERIAL.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test_pacing_serial.cmake" [=[
if(EXISTS "${_de_tests}")
    file(STRINGS "${_de_tests}" _de_pacing_adds REGEX "^add_test\\(")
    foreach(_de_line IN LISTS _de_pacing_adds)
        if(_de_line MATCHES "^add_test\\(\\[==\\[([^]]+)\\]==\\]")
            set_tests_properties("${CMAKE_MATCH_1}" PROPERTIES RUN_SERIAL TRUE)
        endif()
    endforeach()
endif()
]=])
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                 "${CMAKE_CURRENT_BINARY_DIR}/test_pacing_serial.cmake")
endif()

# DENG-247: virtual file system and async I/O, one executable each, so the CTest names are
# vfs.<test> and aio.<test>. The tests mount directories under the build tree as user:/ and
# never write into the real user profile. aio links de_test_support for the alloc guard.
de_add_test(NAME vfs SOURCES test_vfs.c LIBS de_platform)
de_add_test(NAME aio SOURCES test_async_io.c LIBS de_platform de_test_support
    LABELS unit tsan TIMEOUT 120)
foreach(_de_io_test test_vfs test_aio)
    if(TARGET ${_de_io_test})
        target_include_directories(${_de_io_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_definitions(${_de_io_test} PRIVATE
            DE_IO_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/${_de_io_test}_tmp")
    endif()
endforeach()
