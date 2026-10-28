# Stdlib-only trace attribution checks; no GPU or private ANC checkout required.
if(NOT DE_BUILD_TESTS)
    return()
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME acquire_trace_report_selftest
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}" -p test_acquire_trace_report.py -v)
set_tests_properties(acquire_trace_report_selftest PROPERTIES LABELS check TIMEOUT 30)

add_test(NAME presentation_trace_report_selftest
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}" -p test_presentation_trace_report.py -v)
set_tests_properties(presentation_trace_report_selftest PROPERTIES LABELS check TIMEOUT 30)
set_tests_properties(acquire_trace_report_selftest presentation_trace_report_selftest
    PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
