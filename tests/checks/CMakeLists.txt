# tests/checks: build-output checks (DENG-364).
#
#   deterministic_asm_check  ctest (label `check`): disassembles every object of the deterministic
#                            code and fails on an FMA or approximate-reciprocal instruction
#                            (SPEC §5.7 rule 1). The objects are those of every DETERMINISTIC
#                            module and every file passed to de_mark_deterministic_sources()
#                            (cmake/DeModule.cmake records both). Uses llvm-objdump or objdump;
#                            without one the test is reported as skipped. It first builds the
#                            targets that own those objects (CI's check job only configures), so
#                            it runs serially: no other test may drive the build at that time.
#
# The object list is written at generate time, after every module is defined (a deferred call at
# the end of the top-level directory), because tools/ and examples/ are added after tests/.

find_program(DE_OBJDUMP NAMES llvm-objdump objdump
    DOC "Disassembler for deterministic_asm_check (llvm-objdump or objdump)")

set(_de_det_list "${CMAKE_CURRENT_BINARY_DIR}/deterministic_objects_$<CONFIG>.txt")

function(de__write_deterministic_objects out)
    get_property(targets GLOBAL PROPERTY DE_DETERMINISTIC_TARGETS)
    get_property(sources GLOBAL PROPERTY DE_DETERMINISTIC_SOURCES)
    # One line per entry: "<kind> <target> <what> <object>|<object>...". The check fails on an
    # entry with no object, so a source the regex cannot find is an error, not a silent pass.
    # The first line, "build <target>...", lists the targets the check builds first.
    set(lines "")
    set(build_targets "")
    foreach(t IN LISTS targets)
        if(TARGET ${t})
            list(APPEND build_targets ${t})
            string(APPEND lines "module ${t} * $<JOIN:$<TARGET_OBJECTS:${t}>,|>\n")
        endif()
    endforeach()
    foreach(entry IN LISTS sources)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 t)
        list(GET parts 1 rel)
        if(NOT TARGET ${t})
            continue() # module skipped (DE_WITH_<X>=OFF, headless): nothing was built
        endif()
        list(APPEND build_targets ${t})
        # Ninja and Makefiles name the object <target>.dir/<relative source path>.o (.obj);
        # TARGET_OBJECTS may spell it <target>.dir/./<path>.
        string(REGEX REPLACE "([][.+*?^$()|{}])" "\\\\\\1" t_re "${t}.dir")
        string(REGEX REPLACE "([][.+*?^$()|{}])" "\\\\\\1" rel_re "${rel}")
        set(re "/${t_re}/(\\./)?${rel_re}\\.(o|obj)$")
        string(APPEND lines
            "source ${t} ${rel} $<JOIN:$<FILTER:$<TARGET_OBJECTS:${t}>,INCLUDE,${re}>,|>\n")
    endforeach()
    list(REMOVE_DUPLICATES build_targets)
    list(JOIN build_targets " " build_targets)
    file(GENERATE OUTPUT "${out}" CONTENT "build ${build_targets}\n${lines}")
endfunction()
# A deferred call expands its arguments when it runs (in the top-level scope), so the path is
# substituted now with EVAL CODE.
cmake_language(EVAL CODE "
    cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
        CALL de__write_deterministic_objects [[${_de_det_list}]])")

add_test(NAME deterministic_asm_check
    COMMAND "${CMAKE_COMMAND}" "-DDE_OBJ_LIST=${_de_det_list}" "-DDE_OBJDUMP=${DE_OBJDUMP}"
            "-DDE_BUILD_DIR=${CMAKE_BINARY_DIR}" "-DDE_CONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_deterministic_asm.cmake")
set_tests_properties(deterministic_asm_check PROPERTIES
    LABELS check
    RUN_SERIAL TRUE
    SKIP_REGULAR_EXPRESSION "deterministic_asm_check: SKIPPED"
    TIMEOUT 120)
