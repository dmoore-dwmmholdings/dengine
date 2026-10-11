# check_deterministic_asm.cmake: SPEC §5.7 rule 1 on the compiled deterministic code (DENG-364).
#
# The deterministic objects must contain no fused multiply-add and no approximate reciprocal or
# reciprocal square root: those round differently from a separate multiply and add (or from a
# real divide and sqrt), and only some targets have them. -ffp-contract=off (/fp:precise on MSVC
# cl, /clang:-ffp-contract=off on clang-cl) is what keeps them out; this check verifies the
# result. Forbidden mnemonics:
#   arm64:  fmadd fmsub fnmadd fnmsub, fmla fmls (vector), the SVE fmad fmsb fnmad fnmsb fnmla
#           fnmls, and frecpe frecps frecpx frsqrte frsqrts.
#   x86-64: vfmadd* vfmsub* vfnmadd* vfnmsub* (FMA3/FMA4), rcpps rcpss rsqrtps rsqrtss and their
#           AVX / AVX-512 forms (vrcp*, vrsqrt*).
#
# Usage (tests/checks/CMakeLists.txt registers it as deterministic_asm_check):
#   cmake -DDE_OBJ_LIST=<deterministic_objects_<config>.txt> -DDE_OBJDUMP=<objdump>
#         [-DDE_BUILD_DIR=<build dir> -DDE_CONFIG=<config>] -P <this file>
# DE_OBJ_LIST: a first line "build <target>..." (built first when DE_BUILD_DIR is set, so the
# check also works right after a configure), then lines
# "<module|source> <target> <source or *> <object>|<object>..."
if(NOT DE_OBJDUMP OR DE_OBJDUMP MATCHES "-NOTFOUND$")
    message("deterministic_asm_check: SKIPPED: no llvm-objdump or objdump found at configure time")
    return()
endif()
if(NOT EXISTS "${DE_OBJ_LIST}")
    message(FATAL_ERROR "deterministic_asm_check: ${DE_OBJ_LIST} is missing (configure again)")
endif()

file(STRINGS "${DE_OBJ_LIST}" entries)
list(POP_FRONT entries build_line)
if(NOT build_line MATCHES "^build ?(.*)$")
    message(FATAL_ERROR "deterministic_asm_check: ${DE_OBJ_LIST} does not start with 'build'")
endif()
separate_arguments(build_targets UNIX_COMMAND "${CMAKE_MATCH_1}")
if(DE_BUILD_DIR AND build_targets)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${DE_BUILD_DIR}" --config "${DE_CONFIG}"
                            --target ${build_targets}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "deterministic_asm_check: building ${build_targets} failed:\n${out}")
    endif()
endif()
set(forbidden "^(v?fn?m(add|sub)|fml[as]|fn?m(ad|sb)$|fnml[as]|fr(ecp|sqrt)[a-z]*|v?rcp|v?rsqrt)")

set(errors "")
set(nobjs 0)
set(ninsns_total 0)
foreach(entry IN LISTS entries)
    if(NOT entry MATCHES "^(module|source) ([^ ]+) ([^ ]+) ?(.*)$")
        string(APPEND errors "malformed line in ${DE_OBJ_LIST}: ${entry}\n")
        continue()
    endif()
    set(kind "${CMAKE_MATCH_1}")
    set(target "${CMAKE_MATCH_2}")
    set(what "${CMAKE_MATCH_3}")
    string(REPLACE "|" ";" objs "${CMAKE_MATCH_4}")
    if(NOT objs)
        string(APPEND errors "${target} ${what}: no object found for this ${kind}\n")
        continue()
    endif()
    if(kind STREQUAL "source")
        list(LENGTH objs n)
        if(NOT n EQUAL 1)
            string(APPEND errors "${target} ${what}: expected one object, found ${objs}\n")
        endif()
    endif()
    foreach(obj IN LISTS objs)
        if(NOT EXISTS "${obj}")
            string(APPEND errors "${obj}: missing (build the project before running the check)\n")
            continue()
        endif()
        execute_process(COMMAND "${DE_OBJDUMP}" -d --no-show-raw-insn "${obj}"
            OUTPUT_VARIABLE asm ERROR_VARIABLE err RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            string(APPEND errors "${obj}: ${DE_OBJDUMP} failed (${rc}): ${err}\n")
            continue()
        endif()
        math(EXPR nobjs "${nobjs} + 1")
        # Instruction lines are "<hex address>: <mnemonic> <operands>" (llvm-objdump and GNU
        # objdump with --no-show-raw-insn). Apple's llvm-objdump writes arrangement suffixes on
        # the mnemonic (fmla.4s), which the patterns allow.
        string(REGEX MATCHALL "\n[ \t]*[0-9a-fA-F]+:[ \t]+[A-Za-z][A-Za-z0-9.]*" insns "${asm}")
        list(LENGTH insns ninsns)
        math(EXPR ninsns_total "${ninsns_total} + ${ninsns}")
        set(bad "")
        foreach(line IN LISTS insns)
            string(REGEX REPLACE "^\n[ \t]*([0-9a-fA-F]+):[ \t]+" "" mnem "${line}")
            string(TOLOWER "${mnem}" mnem)
            if(mnem MATCHES "${forbidden}")
                string(REGEX REPLACE "^\n[ \t]*" "" at "${line}")
                list(APPEND bad "${at}")
            endif()
        endforeach()
        if(bad)
            list(LENGTH bad nbad)
            list(SUBLIST bad 0 10 shown)
            string(REPLACE ";" "\n    " shown "${shown}")
            string(APPEND errors "${obj} (${target} ${what}): ${nbad} forbidden instruction(s):\n"
                                 "    ${shown}\n")
        endif()
    endforeach()
endforeach()

if(nobjs EQUAL 0 AND NOT errors)
    string(APPEND errors "no deterministic object was checked (empty ${DE_OBJ_LIST})\n")
endif()
if(errors)
    message(FATAL_ERROR "deterministic_asm_check: FMA or approximate reciprocals in deterministic "
                        "code (SPEC §5.7 rule 1):\n${errors}")
endif()
message(STATUS "deterministic_asm_check: ${nobjs} objects, ${ninsns_total} instructions clean "
               "(${DE_OBJDUMP})")
