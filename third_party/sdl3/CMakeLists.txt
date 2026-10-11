# third_party/sdl3: SDL3 (Zlib), pinned in third_party/vendor.toml. Platform layer (SPEC §2, §6).
#
# de_tp_sdl3: SDL's own CMake build (static library only), wrapped in an INTERFACE target so
# engine modules link `de_tp_sdl3` like every other vendored library. The upstream tree keeps its
# root CMakeLists.txt, so vendor.py writes it to SDL3-<version>/ (`strip = 0`), apart from this
# engine-owned file. Public include: `#include <SDL3/SDL.h>`, only inside src/platform/sdl/.
#
# Subsystems the engine does not use are switched off (SPEC §2): SDL audio, SDL_GPU, the 2D
# renderer and the camera. Video, events, joystick/gamepad, haptic, sensor, HIDAPI and dialogs
# stay on (DENG-245, DENG-246, DENG-250). SDL's tests, examples, test library and install rules
# are off.
#
# DE_HEADLESS builds do not build or link SDL at all: src/platform/ then compiles SDL-free stubs.
if(DE_HEADLESS)
    message(STATUS "dengine: SDL3 not built (DE_HEADLESS)")
    return()
endif()

file(GLOB _de_sdl3_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/SDL3-*")
list(LENGTH _de_sdl3_dirs _de_sdl3_count)
if(NOT _de_sdl3_count EQUAL 1)
    message(FATAL_ERROR "third_party/sdl3: expected exactly one SDL3-<version>/ directory, found "
        "'${_de_sdl3_dirs}'. Run `python3 tools/vendor/vendor.py fetch sdl3`.")
endif()

# SDL options are plain variables in this directory scope (policy CMP0077: option() keeps an
# existing normal variable), so they reach SDL's subdirectory without touching the user's cache.
set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_STATIC_PIC ${DE_BUILD_SHARED})
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
set(SDL_INSTALL_DOCS OFF)
set(SDL_UNINSTALL OFF)
set(SDL_DISABLE_INSTALL ON)
set(SDL_AUDIO OFF)
set(SDL_GPU OFF)
set(SDL_RENDER OFF)
set(SDL_CAMERA OFF)
set(SDL_OPENVR OFF)
set(SDL_TRAY OFF)
# The engine has its own RHI backends; SDL only creates the window and the surface (DENG-250).
set(SDL_OPENGL OFF)
set(SDL_OPENGLES OFF)
# Never let SDL add its own warning-as-error or sanitizer flags; DE_SANITIZE covers sanitizers.
set(SDL_WERROR OFF)
set(SDL_ASAN OFF)
set(SDL_CCACHE OFF)
set(SDL_CLANG_TIDY OFF)

# Linux/BSD: SDL refuses to configure without X11 or Wayland development headers. A machine without
# them (a bare CI image, a build container) still builds the engine: SDL is then built without a
# windowing backend, video init fails at run time, and de_platform_init() reports
# DE_ERR_UNSUPPORTED (the window tests skip). Shipping desktop builds need the headers.
if(UNIX AND NOT APPLE AND NOT DEFINED SDL_UNIX_CONSOLE_BUILD)
    find_path(DE_SDL3_X11_INCLUDE X11/Xlib.h)
    find_path(DE_SDL3_WAYLAND_INCLUDE wayland-client.h)
    mark_as_advanced(DE_SDL3_X11_INCLUDE DE_SDL3_WAYLAND_INCLUDE)
    if(NOT DE_SDL3_X11_INCLUDE AND NOT DE_SDL3_WAYLAND_INCLUDE)
        message(WARNING "dengine: no X11 or Wayland development headers found: SDL3 is built "
            "without a windowing backend, so this build cannot open windows. For a desktop "
            "build install them (Debian/Ubuntu: libx11-dev libxext-dev libxrandr-dev "
            "libxcursor-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev "
            "wayland-protocols).")
        set(SDL_UNIX_CONSOLE_BUILD ON)
    endif()
endif()

add_subdirectory("${_de_sdl3_dirs}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL SYSTEM)

# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(SDL3-static PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:$<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>>)
set_target_properties(SDL3-static PROPERTIES FOLDER "dengine/third_party")

add_library(de_tp_sdl3 INTERFACE)
target_link_libraries(de_tp_sdl3 INTERFACE SDL3::SDL3-static)

# Smoke test, built with the engine's strict flags: proves the SDL headers compile cleanly in
# engine code and that the static library links. CTest entries: de_tp_smoke_sdl3.<test>.
de_add_test(NAME de_tp_smoke_sdl3
    SOURCES smoke/smoke_sdl3.c
    LIBS de_tp_sdl3
    LABELS unit third_party)
