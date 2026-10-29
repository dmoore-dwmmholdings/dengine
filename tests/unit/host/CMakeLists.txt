# tests/unit/host: unit tests of module `host` (de_host, SPEC §10). Shared file (S06,
# append-only): later S06 tasks add their own de_add_test() blocks below, one block per task.
#
# The tests include the internal headers "host/host_internal.h" and "host/engine_internal.h"
# (src/ is an include root). DE_HOST_TEST_DATA_DIR holds the game.toml fixtures (data/), which
# the tests mount as game:/; DE_HOST_TEST_TMP_DIR (build tree) holds user:/ and cache:/, so no
# test writes into the real user profile.

# DENG-90: engine create/destroy, de_host_run, game.toml. Every test runs headless, except
# engine_window (label `window`), which checks that a zeroed desc opens a window and skips
# without a video driver.
de_add_test(NAME engine SOURCES test_engine.c LIBS de_host TIMEOUT 300)
de_add_test(NAME engine_offscreen SOURCES test_engine_offscreen.c LIBS de_host TIMEOUT 300)
de_add_test(NAME game_toml SOURCES test_game_toml.c LIBS de_host)
de_add_test(NAME engine_window SOURCES test_engine_window.c LIBS de_host LABELS unit window)
foreach(_de_host_test test_engine test_engine_offscreen test_game_toml test_engine_window)
    if(TARGET ${_de_host_test})
        target_include_directories(${_de_host_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_definitions(${_de_host_test} PRIVATE
            DE_HOST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
            DE_HOST_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/${_de_host_test}_tmp")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_de_host_test}_tmp")
    endif()
endforeach()
if(TARGET test_engine_offscreen AND TARGET de_tp_sdl3)
    target_link_libraries(test_engine_offscreen PRIVATE de_tp_sdl3)
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" _de_offscreen_build_type)
if(DE_HAS_RENDER AND DE_BUILD_TOOLS AND
   (DE_MULTI_CONFIG OR _de_offscreen_build_type MATCHES "^(DEBUG|DEV)$"))
    # Deliberately not registered as an automatic native test. The public-only fixture
    # requires --allow-native for real hardware; no window-capable SDL video driver is used.
    # It exercises Debug/Dev source cooking. Release/Shipping retain packed-only policy;
    # this fixture supplies no pre-cooked pack and cannot exercise those configurations.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(de_offscreen_capture offscreen_capture.c)
    target_link_libraries(de_offscreen_capture PRIVATE de_host de_build_flags)
    if(TARGET de_gpu_d3d12)
        # Native offscreen capture needs the same SDK exports/runtime as other D3D12 fixtures.
        if(COMMAND de_d3d12_agility_deploy)
            de_d3d12_agility_deploy(de_offscreen_capture)
        endif()
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(de_offscreen_capture)
        endif()
    endif()
    set(_de_offscreen_data "${CMAKE_CURRENT_BINARY_DIR}/offscreen-data")
    file(MAKE_DIRECTORY "${_de_offscreen_data}/assets")
    configure_file(data/offscreen-assets.toml "${_de_offscreen_data}/offscreen-assets.toml" COPYONLY)
    configure_file(data/offscreen.toml "${_de_offscreen_data}/offscreen.toml" COPYONLY)
    # Reuse the original2x2 texture oracle byte-for-byte; do not weaken VFS path containment.
    configure_file("${DE_ROOT_DIR}/tests/unit/import/data/texture/checker.png"
        "${_de_offscreen_data}/assets/checker.png" COPYONLY)
    target_compile_definitions(de_offscreen_capture PRIVATE
        DE_OFFSCREEN_DATA_DIR="${_de_offscreen_data}")
    set_target_properties(de_offscreen_capture PROPERTIES OUTPUT_NAME de-offscreen-capture)
    set(_de_offscreen_test_configs)
    if(DE_MULTI_CONFIG)
        set(_de_offscreen_test_configs CONFIGURATIONS Debug Dev)
    endif()
    add_test(NAME offscreen_capture_null COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/check_offscreen_capture.py"
        "$<TARGET_FILE:de_offscreen_capture>" --backend null ${_de_offscreen_test_configs})
    set_tests_properties(offscreen_capture_null PROPERTIES LABELS "unit;host" TIMEOUT 180)
    if(UNIX)
        add_test(NAME offscreen_capture_cancel COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/check_offscreen_cancel.py" ${_de_offscreen_test_configs})
        set_tests_properties(offscreen_capture_cancel PROPERTIES LABELS "unit;host" TIMEOUT 30)
    endif()
    if(APPLE)
        add_executable(de_offscreen_observe offscreen_desktop_observer.m)
        target_link_libraries(de_offscreen_observe PRIVATE de_build_flags
            "-framework AppKit" "-framework CoreGraphics")
        set_source_files_properties(offscreen_desktop_observer.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set_target_properties(de_offscreen_observe PROPERTIES OUTPUT_NAME de-offscreen-observe)
    endif()
endif()

# DENG-92: game library loading. fixtures/ builds test_game with de_add_game() and the plain
# libraries of test_game_module.c (see fixtures/CMakeLists.txt). The host_game_* entries run
# the `dengine` executable (or, in the static form, test_game itself) through
# fixtures/check_run.cmake, which checks the exit code and the log; HOME and the other user
# directory variables point into the build tree, so no run writes into the real user profile.
add_subdirectory(fixtures)
de_add_test(NAME game_module SOURCES test_game_module.c LIBS de_host)
if(TARGET test_game_module)
    target_include_directories(test_game_module PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_game_module PRIVATE
        DE_HOST_PLAIN_GAME_DIR="${DE_HOST_PLAIN_GAME_DIR}")
    add_dependencies(test_game_module
        test_game_standalone test_game_bad_version test_game_no_callback test_game_no_symbol)
endif()

if(DE_BUILD_TESTS)
    set(_de_hg_home "${CMAKE_CURRENT_BINARY_DIR}/host_game_home")
    file(MAKE_DIRECTORY "${_de_hg_home}")
    set(_de_hg_env
        "HOME=${_de_hg_home}" "XDG_DATA_HOME=${_de_hg_home}/data"
        "XDG_CACHE_HOME=${_de_hg_home}/cache" "APPDATA=${_de_hg_home}/appdata"
        "LOCALAPPDATA=${_de_hg_home}/localappdata")
    set(_de_hg_script "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/check_run.cmake")
    set(_de_hg_plain "${DE_HOST_PLAIN_GAME_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}test_game")
    set(_de_hg_suffix "${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_de_hg_tests "")

    # The fixture game runs: its callbacks in order, one frame, exit 0. game.toml came from
    # de_add_game(GAME_TOML), found through game:/ = the game's directory.
    set(_de_hg_expect "test_game: calls init,register_types,register_systems,update,shutdown|Fixture Game exited normally")
    if(DE_GAME_DYLIB)
        if(TARGET dengine_exe)
            add_test(NAME host_game_run COMMAND "${CMAKE_COMMAND}"
                "-DCMD=$<TARGET_FILE:dengine_exe>|--headless|--game|$<TARGET_FILE:test_game>"
                -DEXPECT_EXIT=0
                "-DEXPECT=game 'test_game' attached from library|${_de_hg_expect}"
                -P "${_de_hg_script}")
            list(APPEND _de_hg_tests host_game_run)
        endif()
    else()
        # Static form (Shipping, or Windows without DE_BUILD_SHARED): one executable, nothing
        # loaded (de_host_desc.game_library is NULL, static_api is set).
        add_test(NAME host_game_run COMMAND "${CMAKE_COMMAND}"
            "-DCMD=$<TARGET_FILE:test_game>|--headless"
            -DEXPECT_EXIT=0
            "-DEXPECT=game 'test_game' attached .statically linked.|${_de_hg_expect}"
            "-DREJECT=attached from library"
            -P "${_de_hg_script}")
        list(APPEND _de_hg_tests host_game_run)
    endif()

    # Every way a library can be refused: exit code 1 and the error in the log.
    if(TARGET dengine_exe)
        set(_de_hg_cases
            "missing_library|${DE_HOST_PLAIN_GAME_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}no_such_game${_de_hg_suffix}|no such file.*DE_ERR_NOT_FOUND"
            "missing_symbol|${_de_hg_plain}_no_symbol${_de_hg_suffix}|does not export de_game_get_api.. .DE_ERR_NOT_FOUND"
            "bad_version|${_de_hg_plain}_bad_version${_de_hg_suffix}|DE_ERR_VERSION_MISMATCH"
            "missing_callback|${_de_hg_plain}_no_callback${_de_hg_suffix}|de_game_api.register_systems callback .DE_ERR_INVALID_ARGUMENT")
        foreach(_de_case IN LISTS _de_hg_cases)
            string(REPLACE "|" ";" _de_case "${_de_case}")
            list(GET _de_case 0 _de_name)
            list(GET _de_case 1 _de_lib)
            list(GET _de_case 2 _de_re)
            add_test(NAME host_game_${_de_name} COMMAND "${CMAKE_COMMAND}"
                "-DCMD=$<TARGET_FILE:dengine_exe>|--headless|--game|${_de_lib}"
                -DEXPECT_EXIT=1
                "-DEXPECT=${_de_re}|de_host_run failed"
                "-DREJECT=test_game: calls"
                -P "${_de_hg_script}")
            list(APPEND _de_hg_tests host_game_${_de_name})
        endforeach()
    endif()

    # Only de_game_get_api is exported (macOS nm -gU; Linux nm -D).
    if(DE_GAME_DYLIB AND NOT WIN32 AND CMAKE_NM)
        add_test(NAME host_game_exports COMMAND "${CMAKE_COMMAND}"
            "-DNM=${CMAKE_NM}" "-DLIB=$<TARGET_FILE:test_game>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/check_exports.cmake")
        list(APPEND _de_hg_tests host_game_exports)
    endif()

    if(_de_hg_tests)
        set_tests_properties(${_de_hg_tests} PROPERTIES
            LABELS "unit;host" TIMEOUT 150 ENVIRONMENT "${_de_hg_env}")
    endif()
endif()

# DENG-91: the main loop. `timing` tests the pure timing core (src/host/timing.c). `loop` runs the
# loop on a fake clock, and once through de_host_run; it links de_test_support for the alloc
# guard. `loop_timing` is test_loop.c built with DE_LOOP_TIMING_TESTS: only the wall-clock
# frame-cap test. It has the label `timing` and every entry is RUN_SERIAL (see Trama "Decision:
# Timing tests run serially and loosen their bounds on CI"; same snippet as
# tests/unit/platform/CMakeLists.txt).
de_add_test(NAME timing SOURCES test_timing.c LIBS de_host)
de_add_test(NAME loop SOURCES test_loop.c LIBS de_host de_test_support TIMEOUT 300)
de_add_test(NAME loop_timing SOURCES test_loop.c LIBS de_host LABELS unit timing TIMEOUT 120)
foreach(_de_host_test test_timing test_loop test_loop_timing)
    if(TARGET ${_de_host_test})
        target_include_directories(${_de_host_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_definitions(${_de_host_test} PRIVATE
            DE_HOST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
            DE_HOST_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/${_de_host_test}_tmp")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_de_host_test}_tmp")
    endif()
endforeach()
if(TARGET test_loop_timing)
    target_compile_definitions(test_loop_timing PRIVATE DE_LOOP_TIMING_TESTS=1)
    if(NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test_loop_timing_serial.cmake" [=[
if(EXISTS "${_de_tests}")
    file(STRINGS "${_de_tests}" _de_loop_timing_adds REGEX "^add_test\\(")
    foreach(_de_line IN LISTS _de_loop_timing_adds)
        if(_de_line MATCHES "^add_test\\(\\[==\\[([^]]+)\\]==\\]")
            set_tests_properties("${CMAKE_MATCH_1}" PROPERTIES RUN_SERIAL TRUE)
        endif()
    endforeach()
endif()
]=])
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                     "${CMAKE_CURRENT_BINARY_DIR}/test_loop_timing_serial.cmake")
    endif()
endif()

# DENG-95: headless mode and examples/minimal_game.c in-process. The executable compiles
# examples/minimal_game.c (its de_game_get_api() is the static game of minimal_game.*). Since
# DENG-143 the game runs against the host's real asset system: the tests mount tests/smoke/data
# (the smoke manifest) as dev:/. The smoke tests of the example itself are in
# examples/CMakeLists.txt.
de_add_test(NAME headless
    SOURCES test_headless.c "${DE_ROOT_DIR}/examples/minimal_game.c"
    LIBS de_host TIMEOUT 300)
if(TARGET test_headless)
    target_include_directories(test_headless PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_headless PRIVATE
        DE_HOST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
        DE_HOST_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_headless_tmp"
        DE_HOST_SMOKE_DATA_DIR="${DE_ROOT_DIR}/tests/smoke/data")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_headless_tmp")
endif()

# DENG-93: game library hot reload. The reload_game_v1..v5 fixtures (fixtures/CMakeLists.txt)
# call the engine, so test_hot_reload links the whole static engine and exports it, as the
# `dengine` executable does (Trama "Decision: A game library binds to the engine in the host
# process"), and they bind to it when they are loaded. It also calls the internal de__ / ecs_
# functions, which a shared engine does not export: built only with a static engine where
# de_add_game() builds libraries.
if(DE_BUILD_TESTS AND DE_GAME_DYLIB AND NOT DE_BUILD_SHARED)
    de_add_test(NAME hot_reload SOURCES test_hot_reload.c TIMEOUT 300)
    if(TARGET test_hot_reload)
        target_link_libraries(test_hot_reload PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,dengine>")
        set_target_properties(test_hot_reload PROPERTIES ENABLE_EXPORTS ON)
        if(APPLE)
            target_link_options(test_hot_reload PRIVATE "LINKER:-export_dynamic")
        endif()
        target_include_directories(test_hot_reload PRIVATE "${DE_ROOT_DIR}/src"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
        target_compile_definitions(test_hot_reload PRIVATE
            DE_HOST_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_hot_reload_tmp"
            DE_HOST_RELOAD_V1="$<TARGET_FILE:reload_game_v1>"
            DE_HOST_RELOAD_V2="$<TARGET_FILE:reload_game_v2>"
            DE_HOST_RELOAD_V3="$<TARGET_FILE:reload_game_v3>"
            DE_HOST_RELOAD_V4="$<TARGET_FILE:reload_game_v4>"
            DE_HOST_RELOAD_V5="$<TARGET_FILE:reload_game_v5>")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_hot_reload_tmp")
        add_dependencies(test_hot_reload ${DE_HOST_RELOAD_GAMES})
    endif()
endif()

# DENG-97: the zero-alloc frame test (SPEC §5.3). test_zero_alloc_frames.c runs the fixture game
# fixtures/alloc_game.c headless through the host internals for 1060 frames and checks with the
# alloc guard (de_test_support, DENG-155) that the last 1000 allocate nothing. Label `alloc-guard`.
# `zero_alloc_selftest` is the same with ALLOC_GUARD_SELFTEST: the fixture makes one DE_ALLOC per
# frame, so its test must fail (WILL_FAIL), and zero_alloc_selftest_names_tag checks that the
# failure names the allocation's memory tag. The CI headless and sanitizer jobs run every CTest
# entry, these included.
foreach(_de_za zero_alloc zero_alloc_selftest)
    de_add_test(NAME ${_de_za}
        SOURCES test_zero_alloc_frames.c fixtures/alloc_game.c
        LIBS de_host de_test_support LABELS unit alloc-guard TIMEOUT 300)
    if(TARGET test_${_de_za})
        target_include_directories(test_${_de_za} PRIVATE "${DE_ROOT_DIR}/src"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
        target_compile_definitions(test_${_de_za} PRIVATE
            DE_HOST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
            DE_HOST_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_${_de_za}_tmp")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_${_de_za}_tmp")
    endif()
endforeach()
if(TARGET test_zero_alloc_selftest)
    target_compile_definitions(test_zero_alloc_selftest PRIVATE ALLOC_GUARD_SELFTEST=1)
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        # One entry for the whole executable (no discovery in a cross build).
        set_tests_properties(zero_alloc_selftest PROPERTIES WILL_FAIL TRUE)
    else()
        # Discovered entries are added at CTest time from the file that de_add_test's include
        # script names in `_de_tests` (same pattern as loop_timing above): mark each WILL_FAIL.
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test_zero_alloc_selftest_will_fail.cmake" [=[
if(EXISTS "${_de_tests}")
    file(STRINGS "${_de_tests}" _de_za_adds REGEX "^add_test\\(")
    foreach(_de_line IN LISTS _de_za_adds)
        if(_de_line MATCHES "^add_test\\(\\[==\\[([^]]+)\\]==\\]")
            set_tests_properties("${CMAKE_MATCH_1}" PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()
endif()
]=])
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                     "${CMAKE_CURRENT_BINARY_DIR}/test_zero_alloc_selftest_will_fail.cmake")
        # WILL_FAIL passes on any failure; this entry checks that it is the guard's failure,
        # with the tag name (exit code 1 from the runner, the site line, the FAIL line).
        add_test(NAME zero_alloc_selftest_names_tag COMMAND "${CMAKE_COMMAND}"
            "-DCMD=$<TARGET_FILE:test_zero_alloc_selftest>"
            -DEXPECT_EXIT=1
            "-DEXPECT=engine allocation, 64 bytes, tag [0-9]+ .alloc_game_selftest.|alloc guard: heap allocation in a guarded region"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/check_run.cmake")
        set_tests_properties(zero_alloc_selftest_names_tag PROPERTIES
            LABELS "unit;alloc-guard;host" TIMEOUT 300)
    endif()
endif()

# DENG-510: public-only callbacks transfer modules to the host, including partial init unwind.
de_add_test(NAME owned_game SOURCES test_owned_game.c fixtures/owned_game.c
    LABELS unit host gpu TIMEOUT 180)
if(TARGET test_owned_game)
    target_compile_definitions(test_owned_game PRIVATE
        DE_HOST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
        DE_HOST_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_owned_game_tmp")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_owned_game_tmp")
endif()

if(TARGET test_owned_game)
    if(DE_GAME_DYLIB AND NOT DE_BUILD_SHARED)
        target_link_libraries(test_owned_game PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,dengine>")
        set_target_properties(test_owned_game PROPERTIES ENABLE_EXPORTS ON)
        if(APPLE)
            target_link_options(test_owned_game PRIVATE "LINKER:-export_dynamic")
        endif()
    else()
        target_link_libraries(test_owned_game PRIVATE dengine)
    endif()
    if(DE_GAME_DYLIB)
        get_target_property(_owned_dir test_owned_game_module DE_GAME_DIR)
        target_compile_definitions(test_owned_game PRIVATE
            DE_HOST_OWNED_MODULE="$<TARGET_FILE:test_owned_game_module>"
            DE_HOST_OWNED_MODULE_DIR="${_owned_dir}")
        add_dependencies(test_owned_game test_owned_game_module)
    endif()
endif()

# DENG-691: the allocation-bounded editor model adapter, without a GPU or live editor session.
if(NOT DE_IS_SHIPPING AND DE_HAS_RENDER AND TARGET de_tp_dide_protocol)
    de_add_test(NAME editor_model_inspection
        SOURCES test_model_inspection.c
            "${DE_ROOT_DIR}/src/host/editor/editor_model_inspection.c"
        LIBS de_render de_tp_dide_protocol)
    target_include_directories(test_editor_model_inspection PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-695: validate bounded preview PNGs with an independent image decoder.
if(NOT DE_IS_SHIPPING AND DE_HAS_RENDER AND TARGET de_tp_dide_protocol)
    de_add_test(NAME editor_preview_image SOURCES test_preview_image.c
        "${DE_ROOT_DIR}/src/host/editor/editor_preview_image.c" LIBS de_core de_tp_stb)
    target_include_directories(test_editor_preview_image PRIVATE "${DE_ROOT_DIR}/src")
endif()

if(NOT DE_IS_SHIPPING AND DE_HAS_RENDER AND TARGET de_tp_dide_protocol)
    de_add_test(NAME editor_settings_preview SOURCES test_settings_preview.c
        "${DE_ROOT_DIR}/src/host/editor/editor_settings_value.c"
        "${DE_ROOT_DIR}/src/host/editor/editor_settings_image.c"
        LIBS de_core de_tp_dide_protocol de_tp_bcdec)
    target_include_directories(test_editor_settings_preview PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-698: pure transformed bounds, inverse-bind and clipped diagnostic geometry.
if(NOT DE_IS_SHIPPING AND DE_HAS_RENDER AND TARGET de_tp_dide_protocol)
    de_add_test(NAME editor_model_frames SOURCES test_model_frames.c
        "${DE_ROOT_DIR}/src/host/editor/editor_model_frames.c" LIBS de_core)
    target_include_directories(test_editor_model_frames PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-699: fixed preview geometry through the actual production mesh cooker.
if(NOT DE_IS_SHIPPING AND DE_HAS_RENDER AND DE_BUILD_TOOLS AND TARGET de_tp_dide_protocol)
    de_add_test(NAME editor_preview_geometry SOURCES test_preview_geometry.c
        "${DE_ROOT_DIR}/src/render/loaders/preview_geometry.c" LIBS de_cooker de_render)
    target_include_directories(test_editor_preview_geometry PRIVATE
        "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
endif()

if(NOT DE_IS_SHIPPING AND DE_HAS_RENDER AND DE_BUILD_TOOLS AND TARGET de_tp_dide_protocol)
    de_add_test(NAME editor_preview_material SOURCES test_preview_material.c
        "${DE_ROOT_DIR}/src/render/loaders/preview_geometry.c"
        "${DE_ROOT_DIR}/src/host/editor/editor_preview_material.c" LIBS de_cooker de_render)
    target_include_directories(test_editor_preview_material PRIVATE
        "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
endif()

# DENG-701: qualify the fatal serializer with Debug mpack allocation tracking even in Dev.
# Separate protocol objects keep the writer ABI consistent; never mix Debug/non-Debug mpack.
if(NOT DE_IS_SHIPPING AND TARGET de_tp_dide_protocol)
    add_library(de_test_crash_protocol STATIC
        "${DE_ROOT_DIR}/third_party/dide_protocol/de_editor_protocol.c"
        "${DE_ROOT_DIR}/third_party/dide_protocol/de_editor_transport.c"
        "${DE_ROOT_DIR}/third_party/dide_protocol/third_party/mpack/mpack.c")
    target_include_directories(de_test_crash_protocol SYSTEM PUBLIC
        "${DE_ROOT_DIR}/third_party/dide_protocol")
    target_compile_definitions(de_test_crash_protocol PUBLIC MPACK_DEBUG=1 MPACK_WRITE_TRACKING=1)
    target_compile_options(de_test_crash_protocol PRIVATE
        $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
    if(WIN32)
        target_compile_definitions(de_test_crash_protocol PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(de_test_crash_protocol PUBLIC advapi32)
    endif()
    de_add_test(NAME editor_crash SOURCES test_editor_crash.c
        "${DE_ROOT_DIR}/src/host/editor/editor_crash.c"
        LIBS de_platform de_test_support de_test_crash_protocol TIMEOUT 20)
    if(TARGET test_editor_crash)
        target_include_directories(test_editor_crash PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endif()
