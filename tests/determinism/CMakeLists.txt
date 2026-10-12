# tests/determinism/: the determinism harness and its scenarios (SPEC §19.4, §5.7 rule 7;
# DENG-353).
#
#   de_det_harness   static library (de_det_harness.{h,c}): runs a scenario for N FixedUpdate
#                    ticks and records de_world_checksum after each one; compares runs (first
#                    divergent tick) and reads / writes `<scenario>.checksums` files.
#   <module>/        one directory per deterministic module, added automatically in sorted order.
#                    Each builds its scenarios into a test executable that links de_det_harness
#                    and registers CTest entries `det.<scenario>.<test>` with the label
#                    `determinism` (`ctest -L determinism`).
#
# Scenario tests write what they computed to <build>/determinism/ (DE_DET_OUT_DIR); the
# `determinism` CI workflow (.github/workflows/determinism.yml) uploads those files from three
# platforms and diffs them. Physics (DENG-136), animation and netcode add scenarios here.
if(NOT DE_BUILD_TESTS)
    return()
endif()

set(DE_DET_OUT_DIR "${CMAKE_BINARY_DIR}/determinism")
file(MAKE_DIRECTORY "${DE_DET_OUT_DIR}")

add_library(de_det_harness STATIC de_det_harness.c de_det_harness.h)
target_include_directories(de_det_harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(de_det_harness PUBLIC de_ecs PRIVATE de_build_flags)
target_compile_options(de_det_harness PRIVATE ${DE_DETERMINISTIC_FLAGS})
set_target_properties(de_det_harness PROPERTIES FOLDER "dengine/tests")

# Builds every scenario executable (de_det_add_tests adds them), so the determinism CI jobs
# build only these: `cmake --build --preset <p> --target de_determinism_tests`.
add_custom_target(de_determinism_tests)
set_target_properties(de_determinism_tests PROPERTIES FOLDER "dengine/tests")

# de_det_add_tests(TARGET <exe> TESTS <ctest-name>=<runner-filter>... [TIMEOUT <s>]): one CTest
# entry per test of a scenario executable built with de_test.h, with the labels `determinism` and
# the directory name; also adds <exe> to de_determinism_tests. Explicit names (not de_add_test discovery): the `det.` prefix is taken by the
# task-system suite test_det in tests/unit/task/, and the entries keep the names of the issue's
# test plan.
function(de_det_add_tests)
    cmake_parse_arguments(PARSE_ARGV 0 D "" "TARGET;TIMEOUT" "TESTS")
    add_dependencies(de_determinism_tests ${D_TARGET})
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        return()
    endif()
    if(NOT D_TIMEOUT)
        set(D_TIMEOUT 300)
    endif()
    cmake_path(GET CMAKE_CURRENT_SOURCE_DIR FILENAME module)
    foreach(entry IN LISTS D_TESTS)
        string(REPLACE "=" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 filter)
        add_test(NAME ${name} COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:${D_TARGET}>
                 --filter=${filter})
        set_tests_properties(${name} PROPERTIES
            LABELS "determinism;${module}" TIMEOUT ${D_TIMEOUT} SKIP_RETURN_CODE 77)
    endforeach()
endfunction()

de_add_subdirectories()
