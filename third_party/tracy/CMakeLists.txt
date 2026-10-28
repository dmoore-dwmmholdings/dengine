# third_party/tracy: the Tracy profiler client (BSD-3-Clause), pinned in third_party/vendor.toml.
#
# de_tp_tracy: static library from public/TracyClient.cpp, the client's single TU (C++). Only
# src/core/prof/prof_tracy.cpp uses it, through the C API in <tracy/TracyC.h> (DENG-2 Q1, DENG-94).
#
# It exists only when DE_TRACY is on and the build can define DE_PROFILE: Dev, Release with
# DE_ENABLE_PROFILE, or any multi-config generator (src/core links it for those configs only). In
# Debug, Shipping and Release without DE_ENABLE_PROFILE it is not built at all.
#
# Client configuration (PUBLIC, so prof_tracy.cpp sees the same TracyCZoneCtx layout):
#   TRACY_ENABLE            the client is compiled in
#   TRACY_ON_DEMAND         nothing is recorded until a profiler connects
#   TRACY_MANUAL_LIFETIME   started and stopped by de_prof_init / de_prof_shutdown, not by static
#   TRACY_DELAYED_INIT        constructors; a process that never calls de_prof_init (every test
#                             but test_prof) starts no Tracy thread and opens no socket
#   TRACY_NO_FRAME_IMAGE    no frame screenshots
#   TRACY_NO_CRASH_HANDLER  the engine's crash handler (DENG-251) keeps the signal handlers
#   TRACY_ONLY_LOCALHOST    listen on 127.0.0.1 only, and
#   TRACY_NO_BROADCAST        announce nothing on the network: a Release game run by a player
#                             opens no outside port (no firewall prompt); profile on the same
#                             machine, or tunnel the port
#   TRACY_NO_DLCLOSE_OVERRIDE  (local patch) Linux: dlclose() unloads libraries, as de_dylib_close
#                           and the game hot reload (DENG-93) need. Tracy may then fail to name
#                           callstack frames of a library that was unloaded; the engine records
#                           no callstacks by default.
if(NOT DE_TRACY)
    return()
endif()
if(NOT DE_MULTI_CONFIG)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _de_tracy_bt)
    if(NOT (_de_tracy_bt STREQUAL "DEV" OR (_de_tracy_bt STREQUAL "RELEASE" AND DE_ENABLE_PROFILE)))
        return()
    endif()
endif()

add_library(de_tp_tracy STATIC public/TracyClient.cpp)
target_include_directories(de_tp_tracy SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/public")
target_compile_definitions(de_tp_tracy PUBLIC
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_MANUAL_LIFETIME
    TRACY_DELAYED_INIT
    TRACY_NO_FRAME_IMAGE
    TRACY_NO_CRASH_HANDLER
    TRACY_ONLY_LOCALHOST
    TRACY_NO_BROADCAST
    TRACY_NO_DLCLOSE_OVERRIDE)
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_tracy PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
# The client's threads, dladdr (callstack decoding) and, on Windows, sockets and symbol lookup.
# TracyClient.cpp names the Windows libraries with #pragma comment(lib) for MSVC only; clang-cl
# and the xwin cross build get them here as well.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(de_tp_tracy PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(de_tp_tracy PUBLIC ws2_32 advapi32 user32 dbghelp)
endif()
set_target_properties(de_tp_tracy PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: proves <tracy/TracyC.h> compiles cleanly as
# C11 with the definitions above, and that the client links. CTest: de_tp_smoke_tracy.<test>.
de_add_test(NAME de_tp_smoke_tracy
    SOURCES smoke/smoke_tracy.c
    LIBS de_tp_tracy
    LABELS unit third_party)
