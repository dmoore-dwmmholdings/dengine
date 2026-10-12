# examples/: the flat samples (one source file each). The root CMakeLists.txt adds this file when
# DE_BUILD_EXAMPLES is ON (default ON, OFF in Shipping); it adds every examples/<name>/ project
# itself, so this file must not add subdirectories.

# ---- minimal_game (DENG-95) --------------------------------------------------------------------
# examples/minimal_game.c: the game API, the asset load hook and status polling, a main-thread
# Update system and a deterministic FixedUpdate system. Built with de_add_game()
# (cmake/DeAddGame.cmake): a game library run by `dengine --game`, or (Shipping, Windows without
# DE_BUILD_SHARED) a static executable.
de_add_game(NAME minimal_game SOURCES minimal_game.c)
target_link_libraries(minimal_game PRIVATE de_build_flags)
if(TARGET minimal_game_static)
    target_link_libraries(minimal_game_static PRIVATE de_build_flags)
endif()
# The game loads `dev:/assets.toml` through the host's real asset system (DENG-143). Run it with
# `--dev-dir <project dir>` so dev:/ names a directory with an assets.toml; the smoke tests below
# use tests/smoke/data.

# The Shipping-style static build of the same code, also in the library configurations: one
# executable (main() from src/host/exe/static_main.c.in, de_host_desc.static_api =
# de_game_get_api()), no library loaded. In the static form `minimal_game` is that executable
# already.
if(DE_GAME_DYLIB)
    set(_de_mg_static_dir "${CMAKE_CURRENT_BINARY_DIR}/minimal_game_static")
    set(DE_GAME_NAME minimal_game)
    configure_file("${DE__STATIC_MAIN_TEMPLATE}"
                   "${CMAKE_CURRENT_BINARY_DIR}/minimal_game_static_main.c" @ONLY)
    add_executable(minimal_game_static_exe minimal_game.c
                   "${CMAKE_CURRENT_BINARY_DIR}/minimal_game_static_main.c")
    target_link_libraries(minimal_game_static_exe PRIVATE dengine de_build_flags)
    set_target_properties(minimal_game_static_exe PROPERTIES
        OUTPUT_NAME minimal_game
        RUNTIME_OUTPUT_DIRECTORY "$<1:${_de_mg_static_dir}>"
        FOLDER "dengine/games")
endif()

# Smoke tests: 300 headless frames through tests/smoke/minimal_game.cmake (exit 0, `level_01
# ready` exactly once, 300 frames and at least one FixedUpdate step, headless mode on).
# smoke_minimal_game runs the configuration's normal form (`dengine --game <library>`, or the
# static executable); smoke_minimal_game_static runs the static executable. Registered only when
# the example exists, so -DDE_BUILD_EXAMPLES=OFF (no examples/) configures without them.
# DENG-143: the real asset system loads `dev:/assets.toml`; `--dev-dir` mounts tests/smoke/data as
# dev:/ (its manifest has the empty bundles `common` and `level_01`). Shipping has no dev:/ (the
# host refuses --dev-dir there), so the smoke tests are not registered in Shipping.
if(DE_BUILD_TESTS AND TARGET minimal_game AND NOT DE_IS_SHIPPING)
    set(_de_mg_frames 300)
    set(_de_mg_static_form "attached .statically linked.")
    if(DE_GAME_DYLIB)
        set(_de_mg_cmd "$<TARGET_FILE:dengine_exe>|--headless|--game|$<TARGET_FILE:minimal_game>")
        set(_de_mg_form "attached from library")
        set(_de_mg_static_cmd "$<TARGET_FILE:minimal_game_static_exe>|--headless")
    else()
        set(_de_mg_cmd "$<TARGET_FILE:minimal_game>|--headless")
        set(_de_mg_form "${_de_mg_static_form}")
        set(_de_mg_static_cmd "${_de_mg_cmd}")
    endif()
    # User directories inside the build tree, so no run writes into the real profile.
    set(_de_mg_home "${CMAKE_CURRENT_BINARY_DIR}/smoke_home")
    file(MAKE_DIRECTORY "${_de_mg_home}")
    set(_de_mg_env
        "HOME=${_de_mg_home}" "XDG_DATA_HOME=${_de_mg_home}/data"
        "XDG_CACHE_HOME=${_de_mg_home}/cache" "APPDATA=${_de_mg_home}/appdata"
        "LOCALAPPDATA=${_de_mg_home}/localappdata")
    set(_de_mg_script "${DE_ROOT_DIR}/tests/smoke/minimal_game.cmake")
    set(_de_mg_dev "--dev-dir|${DE_ROOT_DIR}/tests/smoke/data")
    add_test(NAME smoke_minimal_game COMMAND "${CMAKE_COMMAND}"
        "-DCMD=${_de_mg_cmd}|${_de_mg_dev}|--frames|${_de_mg_frames}" "-DFRAMES=${_de_mg_frames}"
        "-DFORM=${_de_mg_form}" -P "${_de_mg_script}")
    add_test(NAME smoke_minimal_game_static COMMAND "${CMAKE_COMMAND}"
        "-DCMD=${_de_mg_static_cmd}|${_de_mg_dev}|--frames|${_de_mg_frames}"
        "-DFRAMES=${_de_mg_frames}"
        "-DFORM=${_de_mg_static_form}" "-DREJECT=attached from library" -P "${_de_mg_script}")
    set_tests_properties(smoke_minimal_game smoke_minimal_game_static PROPERTIES
        LABELS "smoke;host" TIMEOUT 150 ENVIRONMENT "${_de_mg_env}")
endif()
