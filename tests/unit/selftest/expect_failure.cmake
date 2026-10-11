# Runs test_selftest_fail (built with DE_TEST_SELFTEST_FAIL) and checks how failures and skips
# are reported. Usage: cmake -DEXE=<test_selftest_fail> -P expect_failure.cmake

function(run_exe out_rc out_text)
    execute_process(COMMAND "${EXE}" ${ARGN}
        OUTPUT_VARIABLE text ERROR_VARIABLE text RESULT_VARIABLE rc)
    set(${out_rc} "${rc}" PARENT_SCOPE)
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

function(fail what text)
    message(FATAL_ERROR "${what}\n--- output ---\n${text}")
endfunction()

# 1. The deliberate failure: [FAIL] <file>:<line>: <message>, then a non-zero exit (not 77).
run_exe(rc out --filter=selftest.deliberate_failure)
if(rc EQUAL 0 OR rc EQUAL 77)
    fail("deliberate_failure exited with ${rc}; expected a failure exit code" "${out}")
endif()
if(NOT out MATCHES "\\[FAIL\\] [^\n]*test_selftest\\.c:[0-9]+: ASSERT_EQ\\(2 \\+ 2, 5\\)[^\n]*got 4 vs 5")
    fail("deliberate_failure did not report [FAIL] file:line with the values" "${out}")
endif()
if(NOT out MATCHES "\\[ FAILED \\] selftest\\.deliberate_failure")
    fail("deliberate_failure has no [ FAILED ] line" "${out}")
endif()

# 2. A skip-only run exits 77, which CTest maps to "skipped".
run_exe(rc out --filter=selftest.deliberate_skip)
if(NOT rc EQUAL 77)
    fail("deliberate_skip exited with ${rc}; expected 77" "${out}")
endif()
if(NOT out MATCHES "\\[SKIP\\] [^\n]*test_selftest\\.c:[0-9]+: deliberate skip")
    fail("deliberate_skip did not report [SKIP] file:line" "${out}")
endif()

# 3. A failure does not stop the run: the other tests still run and pass, the exit is 1.
run_exe(rc out)
if(NOT rc EQUAL 1)
    fail("full run exited with ${rc}; expected 1" "${out}")
endif()
if(NOT out MATCHES "\\[ OK \\] selftest\\.registry_sorted")
    fail("full run stopped at the failure" "${out}")
endif()
if(NOT out MATCHES "[0-9]+ run \\([^)]*\\): [0-9]+ passed, 1 failed, 1 skipped")
    fail("full run summary is wrong" "${out}")
endif()
