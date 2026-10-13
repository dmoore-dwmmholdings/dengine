# tests/unit/input: unit tests of module `input` (de_input, SPEC §15). Shared file (append-only
# within S21): later S21 tasks add their own de_add_test() blocks below, one per task.
#
# One executable per suite, so the CTest names are input_state.<test>, ... The tests include the
# internal header "input/input_internal.h" (src/ is an include root) and the shared injection
# helpers input_test_util.h, so they need no device and no SDL.

# DENG-83: input state from platform events.
if(TARGET de_input)
    de_add_test(NAME input_state SOURCES test_input_state.c LIBS de_input de_test_support)
    foreach(_de_input_test test_input_state)
        if(TARGET ${_de_input_test})
            target_include_directories(${_de_input_test} PRIVATE "${DE_ROOT_DIR}/src")
        endif()
    endforeach()
endif()

# DENG-84: action maps (bindings, composites, modifiers, interactions, tick view, TOML files).
# DE_INPUT_TEST_DATA_DIR holds the action-map files of data/, which test_action_toml mounts as
# game:/.
if(TARGET de_input)
    de_add_test(NAME action_map SOURCES test_action_map.c LIBS de_input de_test_support)
    de_add_test(NAME action_interactions SOURCES test_action_interactions.c
                LIBS de_input de_test_support)
    de_add_test(NAME action_toml SOURCES test_action_toml.c LIBS de_input de_test_support)
    foreach(_de_input_test test_action_map test_action_interactions test_action_toml)
        if(TARGET ${_de_input_test})
            target_include_directories(${_de_input_test} PRIVATE "${DE_ROOT_DIR}/src")
            target_compile_definitions(${_de_input_test} PRIVATE
                DE_INPUT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
        endif()
    endforeach()
endif()

# DENG-87: haptics (rumble, trigger rumble, adaptive triggers) and gyro (calibration, aiming).
# They use the mock device sink of haptics_internal.h, so no pad or SDL is needed.
if(TARGET de_input)
    de_add_test(NAME haptics SOURCES test_haptics.c LIBS de_input de_test_support)
    de_add_test(NAME gyro SOURCES test_gyro.c LIBS de_input de_test_support)
    foreach(_de_input_test test_haptics test_gyro)
        if(TARGET ${_de_input_test})
            target_include_directories(${_de_input_test} PRIVATE "${DE_ROOT_DIR}/src")
        endif()
    endforeach()
endif()

# DENG-85: context stacks (order, blocking, passthrough, transitions, named sets, [contexts] in
# files). data/contexts.toml is mounted as game:/.
if(TARGET de_input)
    de_add_test(NAME context_stack SOURCES test_context_stack.c LIBS de_input de_test_support)
    if(TARGET test_context_stack)
        target_include_directories(test_context_stack PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_definitions(test_context_stack PRIVATE
            DE_INPUT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    endif()
endif()

# DENG-86: runtime rebinding (listen mode, conflicts, policies) and the user bindings file.
# test_bindings_io mounts its own directories under DE_INPUT_TEST_TMP_DIR as user:/.
if(TARGET de_input)
    de_add_test(NAME rebind SOURCES test_rebind.c LIBS de_input de_test_support)
    de_add_test(NAME bindings_io SOURCES test_bindings_io.c LIBS de_input de_test_support)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_bindings_io_tmp")
    foreach(_de_input_test test_rebind test_bindings_io)
        if(TARGET ${_de_input_test})
            target_include_directories(${_de_input_test} PRIVATE "${DE_ROOT_DIR}/src")
            target_compile_definitions(${_de_input_test} PRIVATE
                DE_INPUT_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_bindings_io_tmp")
        endif()
    endforeach()
endif()
