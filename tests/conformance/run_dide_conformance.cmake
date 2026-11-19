# Runs the DIDE editor-IPC conformance suite (dide crate dide-conformance) against a game and
# fails on any failed check that is not a known, tracked gap below (DENG-783).
#   -DRUNNER=<dide-conformance> -DGAME=<game executable> -DLOG=<output file>
# The runner starts the game itself with --editor-service.
foreach(_v RUNNER GAME LOG)
  if(NOT ${_v})
    message(FATAL_ERROR "run_dide_conformance: ${_v} is not set")
  endif()
endforeach()

execute_process(COMMAND "${RUNNER}" -- "${GAME}"
                OUTPUT_FILE "${LOG}" ERROR_FILE "${LOG}.err" TIMEOUT 600 RESULT_VARIABLE _rc)
file(STRINGS "${LOG}" _lines REGEX "^(ok|FAIL|skip) +|editor IPC [0-9.]+ .*passed")

# Checks that fail for reasons outside the engine's protocol behaviour, each with its issue.
set(_known
  # The suite's catalog sweep has no requests for the replay.* methods (DIDE-357).
  "catalog: every method answers per the IDL: the sweep has no request for: replay"
  # The suite expects a first world.delta with the whole world; the IDL does not (DIDE-358).
  "deltas: a subscription sends the world"
  # IPC is serviced once per frame; the spec's service thread is DENG-784.
  "performance: a small read round trip under 2 ms")

set(_summary "")
set(_bad "")
foreach(_l IN LISTS _lines)
  if(_l MATCHES "^editor IPC")
    set(_summary "${_l}")
  elseif(_l MATCHES "^FAIL +(.*)$")
    set(_check "${CMAKE_MATCH_1}")
    set(_ok FALSE)
    foreach(_k IN LISTS _known)
      string(FIND "${_check}" "${_k}" _at)
      if(_at EQUAL 0)
        set(_ok TRUE)
      endif()
    endforeach()
    if(NOT _ok)
      list(APPEND _bad "${_check}")
    endif()
  endif()
endforeach()

if(_summary STREQUAL "")
  message(FATAL_ERROR "dide-conformance gave no summary (exit ${_rc}); see ${LOG}")
endif()
message(STATUS "${_summary}")
if(_bad)
  list(JOIN _bad "\n  " _list)
  message(FATAL_ERROR "dide-conformance: unexpected failures:\n  ${_list}\nSee ${LOG}")
endif()
