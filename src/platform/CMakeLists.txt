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
