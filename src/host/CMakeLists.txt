# src/host: module `host` (target de_host), layer host. SPEC §10: the app host (engine
# create/destroy, game.toml, de_host_run, main loop, game attachment).
# Shared file (S06, append-only): later tasks add their sources with target_sources() below, one
# block per task. Do not reorder or rewrite another task's lines.
de_add_module(
    NAME host
    LAYER host
    SOURCES
        engine_internal.h
        host_internal.h
        engine.c
        game_toml.c
        host_run.c
        loop.c
        game_module.c
    DEPS
        core platform task reflect ecs asset anim input text sky terrain water decal streaming
        tilemap crash mods save
    OPTIONAL_DEPS gpu gpu_d3d12 gpu_vulkan gpu_metal render physics audio net ui vfx nav video steam
)

# DENG-90: game.toml is read with the vendored tomlc17, through core's entry point
# (core/de_toml_internal.h; never toml_set_option). Recorded in DE_MODULE_LIBS too, so the
# aggregate `dengine` links it.
target_link_libraries(de_host PRIVATE de_tp_tomlc17)
set_property(TARGET de_host APPEND PROPERTY DE_MODULE_LIBS de_tp_tomlc17)

# DENG-92: game attachment (game_module.c is in the list above), the `dengine` host executable
# and de_add_game() (cmake/DeAddGame.cmake).
target_sources(de_host PRIVATE game_module.h)

# The `dengine` executable loads a game library (`dengine --game <lib>`). The target is
# `dengine_exe` because `dengine` is the aggregate engine library; the file is `dengine`.
# Shipping has no development host: each game is its own static executable (de_add_game).
if(NOT DE_IS_SHIPPING)
    add_executable(dengine_exe exe/dengine_main.c)
    target_link_libraries(dengine_exe PRIVATE de_build_flags)
    set_target_properties(dengine_exe PROPERTIES OUTPUT_NAME dengine FOLDER "dengine/host")
    if(DE_BUILD_SHARED)
        # The game library links the same shared dengine; nothing to export from here.
        target_link_libraries(dengine_exe PRIVATE dengine)
        if(WIN32)
            add_custom_command(TARGET dengine_exe POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_RUNTIME_DLLS:dengine_exe>" "$<TARGET_FILE_DIR:dengine_exe>"
                COMMAND_EXPAND_LISTS)
        endif()
    elseif(WIN32)
        # A Windows game DLL cannot bind to a static engine: de_add_game() builds static
        # executables here (DE_GAME_DYLIB is FALSE), and this host loads only libraries that
        # need nothing from the engine.
        target_link_libraries(dengine_exe PRIVATE dengine)
    else()
        # Every engine object, exported, so a game library can call any DE_API function: the
        # library leaves them undefined and binds them here when it is loaded.
        target_link_libraries(dengine_exe PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,dengine>")
        set_target_properties(dengine_exe PROPERTIES ENABLE_EXPORTS ON)
        if(APPLE)
            target_link_options(dengine_exe PRIVATE "LINKER:-export_dynamic")
        endif()
    endif()
endif()

include(DeAddGame)

# DENG-91: the main loop's timing core (fixed/variable steps, catch-up, alpha) and the loop's
# internal types; loop.c itself is in the list above.
target_sources(de_host PRIVATE timing.h timing.c loop_internal.h)
