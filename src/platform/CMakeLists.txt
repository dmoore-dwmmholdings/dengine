# src/platform: module `platform` (target de_platform), layer foundation. SPEC §6.
#
# One source per OS family for each area: *_win32.c on Windows, *_posix.c on macOS and Linux.
# de_atomic.h is an internal header (no source); other modules include "platform/de_atomic.h".
if(WIN32)
    set(_de_platform_os win32)
else()
    set(_de_platform_os posix)
endif()

set(_de_platform_libs "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    # The aggregate `dengine` library (created in src/) links this module's LIBS too, so the
    # imported target must be visible outside this directory.
    set_target_properties(Threads::Threads PROPERTIES IMPORTED_GLOBAL TRUE)
    list(APPEND _de_platform_libs Threads::Threads)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # dlopen/dlsym (dylib_posix.c). glibc 2.34+ has them in libc; older ones need libdl.
    list(APPEND _de_platform_libs ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    # timeBeginPeriod (sleep_pacing.c), the fallback before high-resolution waitable timers.
    list(APPEND _de_platform_libs winmm)
endif()
if(WIN32)
    list(APPEND _de_platform_libs dbghelp) # MiniDumpWriteDump (crash_win32.c, DENG-251)
endif()

de_add_module(
    NAME platform
    LAYER foundation
    SOURCES
        de_atomic.h
        thread_${_de_platform_os}.c
        time_${_de_platform_os}.c
        dylib_${_de_platform_os}.c
        sleep_pacing.c
        vfs_internal.h
        vfs.c
        file_${_de_platform_os}.c
        known_dirs.c
        async_io.c
        crash_common.c
        crash_internal.h
        crash_${_de_platform_os}.c
    DEPS core
    LIBS ${_de_platform_libs}
)

# ---- Platform init, displays, windows, event pump (DENG-245) ----
# SDL3 backend in sdl/. A DE_HEADLESS build does not link SDL at all: sdl/platform_headless.c
# provides the same API (windows report DE_ERR_UNSUPPORTED). SDL headers stay inside sdl/.
if(TARGET de_platform)
    if(DE_HEADLESS)
        target_sources(de_platform PRIVATE sdl/platform_headless.c)
    else()
        target_sources(de_platform PRIVATE
            sdl/sdl_internal.h
            sdl/platform_sdl.c
            sdl/window_sdl.c
            sdl/display_sdl.c
            sdl/events_sdl.c)
        target_link_libraries(de_platform PRIVATE de_tp_sdl3)
        # The aggregate `dengine` library links each module's DE_MODULE_LIBS too.
        set_property(TARGET de_platform APPEND PROPERTY DE_MODULE_LIBS de_tp_sdl3)
    endif()
endif()

# Build ID for crash reports (DENG-251): `git rev-parse --short HEAD` at configure time, or
# "unknown" outside a git checkout. -DDE_BUILD_ID=<id> overrides it (CI, release builds). Only
# crash_common.c gets the definition, so a new commit rebuilds one file after reconfiguring.
set(DE_BUILD_ID "" CACHE STRING "Build ID written into crash reports (empty: git commit)")
set(_de_build_id "${DE_BUILD_ID}")
if(_de_build_id STREQUAL "")
    set(_de_build_id "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${DE_ROOT_DIR}"
            OUTPUT_VARIABLE _de_git_head
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _de_git_rc)
        if(_de_git_rc EQUAL 0 AND _de_git_head MATCHES "^[0-9a-f]+$")
            set(_de_build_id "${_de_git_head}")
        endif()
    endif()
endif()
if(NOT _de_build_id MATCHES "^[A-Za-z0-9._+-]+$")
    message(FATAL_ERROR "DE_BUILD_ID '${_de_build_id}' may only use A-Z a-z 0-9 . _ + -")
endif()
# The value the build uses, for tests (tests/unit/platform/CMakeLists.txt).
set(DE_BUILD_ID_VALUE "${_de_build_id}" CACHE INTERNAL "Build ID compiled into de_platform")
if(TARGET de_platform)
    set_property(SOURCE crash_common.c APPEND PROPERTY
        COMPILE_DEFINITIONS "DE_BUILD_ID=\"${_de_build_id}\"")
endif()

# ---- Raw input devices, gamepads, sensors, clipboard, cursor, text input, dialogs (DENG-246) ----
# Same split as above: the SDL files, or SDL-free stubs in a DE_HEADLESS build.
if(TARGET de_platform)
    if(DE_HEADLESS)
        target_sources(de_platform PRIVATE sdl/devices_headless.c)
    else()
        target_sources(de_platform PRIVATE
            sdl/input_sdl.c
            sdl/gamepad_sdl.c
            sdl/sensor_sdl.c
            sdl/clipboard_sdl.c
            sdl/cursor_sdl.c
            sdl/dialog_sdl.c)
    endif()
endif()

# ---- Native window handles and graphics surfaces (DENG-250) ----
# surface_sdl.c: Win32 window handle, Metal view, Vulkan surfaces through SDL (SDL opens the Vulkan
# loader at run time; nothing links it). surface_metal.m (macOS): the CAMetalLayer property that C
# cannot set, with ARC. Root CMakeLists.txt enables OBJC on Apple.
if(TARGET de_platform AND NOT DE_HEADLESS)
    target_sources(de_platform PRIVATE sdl/surface_sdl.c)
    if(APPLE)
        target_sources(de_platform PRIVATE sdl/surface_metal.m)
        set_source_files_properties(sdl/surface_metal.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set(_de_surface_frameworks
            "$<LINK_LIBRARY:FRAMEWORK,QuartzCore>" "$<LINK_LIBRARY:FRAMEWORK,Foundation>")
        target_link_libraries(de_platform PRIVATE ${_de_surface_frameworks})
        set_property(TARGET de_platform APPEND PROPERTY DE_MODULE_LIBS ${_de_surface_frameworks})
    endif()
endif()

# ---- File watching (DENG-248) ----
# watch_common.c (pending table, debounce, delivery) plus one backend per OS. Only Debug and
# Dev builds watch; the sources compile to stubs (DE_ERR_UNSUPPORTED) in Release and Shipping.
if(TARGET de_platform)
    target_sources(de_platform PRIVATE watch_internal.h watch_common.c)
    if(APPLE)
        target_sources(de_platform PRIVATE watch_fsevents.c)
        # FSEvents lives in CoreServices; CFArray/CFString in CoreFoundation.
        set(_de_watch_libs "-framework CoreServices" "-framework CoreFoundation")
        target_link_libraries(de_platform PRIVATE ${_de_watch_libs})
        # The aggregate `dengine` library links each module's DE_MODULE_LIBS too.
        set_property(TARGET de_platform APPEND PROPERTY DE_MODULE_LIBS ${_de_watch_libs})
    elseif(WIN32)
        target_sources(de_platform PRIVATE watch_win32.c)
    else()
        target_sources(de_platform PRIVATE watch_inotify.c)
    endif()
endif()
