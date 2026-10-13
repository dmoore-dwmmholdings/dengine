# tools/caps: `de-caps`, the GPU capability probe (DENG-290; SPEC §3.3, §12.1, §24 R5/R7).
# Auto-discovered by tools/CMakeLists.txt. Prints de_gpu_caps and the de_gpu_evaluate_tier()
# decision (src/gpu/tier.c) for every adapter of every backend in the build; --json writes the
# format of caps_schema.json. The gpu module does not exist in DE_HEADLESS builds, so neither does
# the tool.
if(NOT TARGET de_gpu)
    return()
endif()

add_executable(de-caps main.c)
target_link_libraries(de-caps PRIVATE de_gpu de_core de_build_flags)
set_target_properties(de-caps PROPERTIES FOLDER "dengine/tools")
# main.c includes src/gpu/d3d12/d3d12_agility_exports.h on Windows: the exe exports the Agility
# SDK version and path, and the runtime DLLs are copied to <exe dir>/D3D12/.
target_include_directories(de-caps PRIVATE "${DE_ROOT_DIR}/src")
if(TARGET de_gpu_d3d12)
    target_link_libraries(de-caps PRIVATE de_tp_d3d12_agility)
    de_d3d12_agility_deploy(de-caps)
endif()

# test_de_caps_json_schema (the issue's test plan): run the tool and validate its text report and
# --json output against caps_schema.json (check_caps_json.py, stdlib Python). Two runs: the null
# backend (always available, exact values checked) and the default probe (every backend in the
# build; Metal on macOS, D3D12 and Vulkan on Windows, Vulkan on Linux; a backend that cannot run
# on the machine must still be reported, and the tool must still exit 0). Not added when cross-
# compiling: the tool cannot run on the build host.
if(DE_BUILD_TESTS AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(_de_caps_check
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_caps_json.py"
        --exe "$<TARGET_FILE:de-caps>" --schema "${CMAKE_CURRENT_SOURCE_DIR}/caps_schema.json")
    add_test(NAME de_caps_json_schema_null COMMAND ${_de_caps_check} --backend null --null)
    # The platform's own backend must work on every machine that runs the tests: D3D12 has WARP
    # on every Windows machine (so the Windows CI jobs run de-caps with D3D12), and every Mac has
    # a Metal device (a paravirtual one on CI). Vulkan is not required: no loader is a valid
    # report (macOS without the SDK, Linux runners without a driver).
    set(_de_caps_expect)
    if(TARGET de_gpu_d3d12)
        list(APPEND _de_caps_expect --expect-ok d3d12)
    endif()
    if(TARGET de_gpu_metal)
        list(APPEND _de_caps_expect --expect-ok metal)
    endif()
    add_test(NAME de_caps_json_schema_default COMMAND ${_de_caps_check} ${_de_caps_expect})
    set_tests_properties(de_caps_json_schema_null de_caps_json_schema_default PROPERTIES
        LABELS "unit;gpu;caps"
        TIMEOUT 300)
    unset(_de_caps_check)
    unset(_de_caps_expect)
endif()
