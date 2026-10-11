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

# Window, display and event-pump tests (DENG-245): their own executable, so they carry the
# `window` label (`ctest -L window`). They skip when no video driver can start. The tests that
# push SDL events use the internal header src/platform/sdl/sdl_internal.h and link SDL; a
# DE_HEADLESS build compiles them out and checks the stubs instead.
de_add_test(NAME window SOURCES test_window.c LIBS de_platform LABELS unit window)
if(TARGET test_window)
    target_include_directories(test_window PRIVATE "${DE_ROOT_DIR}/src")
    if(TARGET de_tp_sdl3)
        target_link_libraries(test_window PRIVATE de_tp_sdl3)
    endif()
endif()

# Crash handler (DENG-251): its own executable, so CTest names are crash.<test>. It uses the
# internal header src/platform/crash_internal.h and the alloc guard (handler_no_malloc).
de_add_test(NAME crash SOURCES test_crash.c LIBS de_platform de_test_support)
if(TARGET test_crash)
    target_include_directories(test_crash PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_crash PRIVATE "DE_TEST_BUILD_ID=\"${DE_BUILD_ID_VALUE}\"")
endif()

# Raw input devices (DENG-246): raw events, virtual gamepads, sensors, clipboard, cursor, text
# input and file dialogs. Its own executable, labelled `window` too (needs video for most tests,
# which skip without a driver). Uses the internal header to push SDL events and drive SDL virtual
# gamepads.
de_add_test(NAME devices SOURCES test_devices.c LIBS de_platform LABELS unit window)
if(TARGET test_devices)
    target_include_directories(test_devices PRIVATE "${DE_ROOT_DIR}/src")
    if(TARGET de_tp_sdl3)
        target_link_libraries(test_devices PRIVATE de_tp_sdl3)
    endif()
endif()

# File watching (DENG-248): its own executable, so CTest names are watch.<test>. Each test
# mounts its own directory under the build tree as scratch:/ and uses the internal header
# src/platform/watch_internal.h (pending-count hook). The tests wait for OS notifications in
# poll loops, hence the generous timeout.
de_add_test(NAME watch SOURCES test_watch.c LIBS de_platform TIMEOUT 120)
if(TARGET test_watch)
    target_include_directories(test_watch PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_watch PRIVATE
        DE_IO_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_watch_tmp")
endif()
