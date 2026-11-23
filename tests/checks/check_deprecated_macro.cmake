# DE_DEPRECATED(msg) check (DENG-767). Compiles the fixture, which calls a deprecated function.
#   MODE=warns    the compiler must report a deprecation warning that carries the message, and
#                 -Werror=deprecated-declarations must fail the compile.
#   MODE=fallback the compiler is told it is not GCC or Clang (DE_TEST_FALLBACK in the fixture). The macro
#                 must expand to nothing: the compile succeeds with no deprecation warning.
# Inputs: -DDE_COMPILER -DDE_LANG (C|CXX) -DDE_INCLUDE_DIR -DDE_FIXTURE -DDE_MODE
if(NOT DE_COMPILER OR NOT DE_INCLUDE_DIR OR NOT DE_FIXTURE OR NOT DE_MODE)
    message(FATAL_ERROR "DE_COMPILER, DE_INCLUDE_DIR, DE_FIXTURE and DE_MODE are required")
endif()
set(_lang_flags -x c -std=c11)
if(DE_LANG STREQUAL "CXX")
    set(_lang_flags -x c++ -std=c++17)
endif()
set(_base "${DE_COMPILER}" ${_lang_flags} -fsyntax-only "-I${DE_INCLUDE_DIR}" "${DE_FIXTURE}")

if(DE_MODE STREQUAL "warns")
    execute_process(COMMAND ${_base} RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_VARIABLE _out)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "plain compile failed (${_rc}):\n${_err}")
    endif()
    if(NOT _err MATCHES "de_fixture_old is deprecated: use de_fixture_new")
        message(FATAL_ERROR "no deprecation warning with the message:\n${_err}")
    endif()
    execute_process(COMMAND ${_base} -Werror=deprecated-declarations RESULT_VARIABLE _rc
        ERROR_QUIET OUTPUT_QUIET)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "-Werror=deprecated-declarations did not fail the compile")
    endif()
elseif(DE_MODE STREQUAL "fallback")
    execute_process(COMMAND ${_base} -DDE_TEST_FALLBACK -Werror=deprecated-declarations
        RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_VARIABLE _out)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "fallback compile failed (${_rc}):\n${_err}")
    endif()
    if(_err MATCHES "deprecated")
        message(FATAL_ERROR "fallback still warns:\n${_err}")
    endif()
else()
    message(FATAL_ERROR "unknown DE_MODE ${DE_MODE}")
endif()
