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
