# tests/determinism/physics: the physics per-tick checksum harness and its scenarios (DENG-136;
# SPEC §13.2, §19.4, risk R6). See harness.h.
#
#   test_det_physics   one executable: harness.c (also its main(), which adds --update-golden
#                      to the de_test runner options), the scenario files, test_harness.c.
#
# CTest entries (labels `determinism;physics`):
#   determinism_physics_<scenario>.workers_<n>   n = 0, 1, 4: the run with n frame workers equals
#                                                workers 0 tick for tick and golden/<scenario>.checksums
#   determinism_physics_harness.<test>           harness self tests (test_harness.c)
#   determinism_physics.registered               every de_test case of the executable has a CTest
#                                                entry above (a forgotten entry would not run)
#
# Shared file (append-only): S23 (DENG-224) and S24 (DENG-260) add their scenario files with
# their own de_phys_det_add_scenarios() call at the end. Regenerate goldens only after a
# deliberate change, on one platform, in a Debug or Dev build:
#     build/dev/tests/determinism/physics/test_det_physics --update-golden
if(NOT TARGET de_physics)
    return()
endif()

add_executable(test_det_physics harness.c harness.h test_harness.c
    "${DE_ROOT_DIR}/tests/unit/de_test.c")
target_link_libraries(test_det_physics PRIVATE de_det_harness de_physics de_build_flags)
# src/ is an include root for the physics internals (body pool, shapes, narrowphase).
target_include_directories(test_det_physics PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tests/unit")
target_compile_options(test_det_physics PRIVATE ${DE_DETERMINISTIC_FLAGS})
target_compile_definitions(test_det_physics PRIVATE
    DE_TEST_NO_MAIN
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\""
    "DE_PHYS_DET_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\"")
set_target_properties(test_det_physics PROPERTIES FOLDER "dengine/tests")

# Every scenario runs with these worker counts (DE_PHYS_DET_REGISTER in harness.h defines the
# same three cases).
set(DE_PHYS_DET_WORKERS 0 1 4)

# de_phys_det_add_scenarios(SOURCES <file>... SCENARIOS <name>...): adds the scenario sources to
# test_det_physics and registers determinism_physics_<name>.workers_<n> for every name.
function(de_phys_det_add_scenarios)
    cmake_parse_arguments(PARSE_ARGV 0 S "" "" "SOURCES;SCENARIOS")
    target_sources(test_det_physics PRIVATE ${S_SOURCES})
    set(tests "")
    foreach(name IN LISTS S_SCENARIOS)
        foreach(w IN LISTS DE_PHYS_DET_WORKERS)
            list(APPEND tests "determinism_physics_${name}.workers_${w}=${name}.workers_${w}")
        endforeach()
    endforeach()
    de_det_add_tests(TARGET test_det_physics TESTS ${tests})
    set_property(DIRECTORY APPEND PROPERTY DE_PHYS_DET_CASES ${tests})
endfunction()

# Harness self tests.
set(_de_phys_det_self
    determinism_physics_harness.detects_one_ulp=harness.detects_one_ulp
    determinism_physics_harness.workers_field_level=harness.workers_field_level
    determinism_physics_harness.scenarios_collide=harness.scenarios_collide
    determinism_physics_harness.begin_errors=harness.begin_errors)
de_det_add_tests(TARGET test_det_physics TESTS ${_de_phys_det_self})
set_property(DIRECTORY APPEND PROPERTY DE_PHYS_DET_CASES ${_de_phys_det_self})

# determinism_physics.registered: `test_det_physics --list` must equal the registered cases.
# Added at the end of this directory, after every de_phys_det_add_scenarios() call.
function(de__phys_det_registry_test)
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        return()
    endif()
    get_property(cases DIRECTORY PROPERTY DE_PHYS_DET_CASES)
    set(filters "")
    foreach(entry IN LISTS cases)
        string(REPLACE "=" ";" parts "${entry}")
        list(GET parts 1 filter)
        list(APPEND filters "${filter}")
    endforeach()
    list(SORT filters)
    list(JOIN filters "," filters) # one COMMAND argument; the script splits it again
    set(script "${CMAKE_CURRENT_BINARY_DIR}/phys_det_registered.cmake")
    file(WRITE "${script}" [=[
execute_process(COMMAND ${DE_EMULATOR} "${DE_EXE}" --list
    OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${DE_EXE} --list failed (${rc})")
endif()
string(REGEX REPLACE "\r" "" out "${out}")
string(REGEX REPLACE "\n$" "" out "${out}")
string(REPLACE "\n" ";" listed "${out}")
list(SORT listed)
string(REPLACE "," ";" DE_EXPECTED "${DE_EXPECTED}")
if(NOT listed STREQUAL DE_EXPECTED)
    message(FATAL_ERROR "test_det_physics cases and CTest entries differ (tests/determinism/physics/"
        "CMakeLists.txt registers them; DE_PHYS_DET_REGISTER defines them):\n"
        "  cases:      ${listed}\n  registered: ${DE_EXPECTED}")
endif()
list(LENGTH listed n)
message(STATUS "test_det_physics: ${n} cases, all registered")
]=])
    add_test(NAME determinism_physics.registered
        COMMAND "${CMAKE_COMMAND}" "-DDE_EXE=$<TARGET_FILE:test_det_physics>"
                "-DDE_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}" "-DDE_EXPECTED=${filters}"
                -P "${script}")
    set_tests_properties(determinism_physics.registered PROPERTIES
        LABELS "determinism;physics" TIMEOUT 60)
endfunction()
cmake_language(DEFER CALL de__phys_det_registry_test)

# DENG-136 (S22): collision-only scenarios.
de_phys_det_add_scenarios(SOURCES scenarios_collision.c
    SCENARIOS kinematic_sweep hull_builder mass_props)

# DENG-135 (S22): the collision pipeline (manifolds, touch state, warm-start carry-over).
de_phys_det_add_scenarios(SOURCES scenarios_contacts.c SCENARIOS contact_pile)

# DENG-217 (S23): the soft-step solver (falling and tumbling bodies, bounces, a stack of 10).
de_phys_det_add_scenarios(SOURCES scenarios_solver.c SCENARIOS box_drop sphere_bounce stack_10)

# DENG-218 (S23): islands, sleeping and the graph-colored solver (a 10-row pyramid; sleep and every
# kind of wake-up).
de_phys_det_add_scenarios(SOURCES scenarios_islands.c SCENARIOS pyramid_10 sleep_wake)
