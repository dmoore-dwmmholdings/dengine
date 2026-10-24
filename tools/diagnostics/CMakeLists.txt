# Stdlib-only trace attribution checks; no GPU or private ANC checkout required.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME acquire_trace_report_selftest
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}" -p test_acquire_trace_report.py -v)
set_tests_properties(acquire_trace_report_selftest PROPERTIES LABELS check TIMEOUT 30)
