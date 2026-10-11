# Checks the runner's command line on test_selftest.
# Usage: cmake -DEXE=<test_selftest> -P runner_cli.cmake

function(run_exe out_text)
    execute_process(COMMAND "${EXE}" ${ARGN}
        OUTPUT_VARIABLE text ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "'${EXE} ${ARGN}' exited with ${rc}\n${text}${err}")
    endif()
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

# --list: registration order is source order, the same on every run.
run_exe(list --list)
string(REGEX MATCHALL "[^\n]+" names "${list}")
list(LENGTH names n)
if(n LESS 10)
    message(FATAL_ERROR "--list printed ${n} tests:\n${list}")
endif()
list(GET names 0 first)
if(NOT first STREQUAL "selftest.asserts_pass")
    message(FATAL_ERROR "--list does not start with the first test in the file:\n${list}")
endif()

# --shuffle --seed=1 twice: identical order, and a permutation of --list.
run_exe(shuffled_a --list --shuffle --seed=1)
run_exe(shuffled_b --list --shuffle --seed=1)
if(NOT shuffled_a STREQUAL shuffled_b)
    message(FATAL_ERROR "--shuffle --seed=1 gave two orders:\n${shuffled_a}\n---\n${shuffled_b}")
endif()
string(REGEX MATCHALL "[^\n]+" shuffled "${shuffled_a}")
set(sorted_list ${names})
set(sorted_shuffled ${shuffled})
list(SORT sorted_list)
list(SORT sorted_shuffled)
if(NOT sorted_list STREQUAL sorted_shuffled)
    message(FATAL_ERROR "--shuffle changed the set of tests:\n${shuffled_a}")
endif()
if(shuffled STREQUAL names)
    message(FATAL_ERROR "--shuffle --seed=1 did not change the order")
endif()
# The shuffled run itself passes too.
run_exe(out --shuffle --seed=1)

# --filter: globs, ':' alternatives and '-' exclusions.
run_exe(out --list "--filter=selftest.float*:selftest.fixture_*-*_registered")
if(NOT out STREQUAL "selftest.float_eps\nselftest.fixture_order\n")
    message(FATAL_ERROR "--filter selected the wrong tests:\n${out}")
endif()

# --repeat: the selection runs N times.
run_exe(out --repeat=3 --filter=selftest.asserts_pass)
string(REGEX MATCHALL "\\[ OK \\] selftest\\.asserts_pass" oks "${out}")
list(LENGTH oks n_ok)
if(NOT n_ok EQUAL 3)
    message(FATAL_ERROR "--repeat=3 ran the test ${n_ok} times:\n${out}")
endif()

# A filter that matches nothing is an error, so a stale CTest entry cannot pass silently.
execute_process(COMMAND "${EXE}" --filter=no.such_test RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(rc EQUAL 0)
    message(FATAL_ERROR "--filter matching nothing exited 0")
endif()
