# check_cooker_link.cmake: CTest cooker_link_check (DENG-209, SPEC §11.5: "Shipping: the cooker is
# not linked").
#
#   cmake -DDE_NM=<nm> -DDE_EXPECT_COOKER=<0|1> -DDE_FILES=<file>|<file>... -DDE_CONFIG=<config>
#         -P check_cooker_link.cmake
#
# Cooker symbols are those of tools/cook/lib/: de_cooker_*, de_cook_* and de_asset_import_*
# (public), de__cooker_* and de__import_ctx_* (internal). The asset module's own cook stage
# (de__cook_stage_*) exists in every build and does not count.
#
#   DE_EXPECT_COOKER=0 (Release, Shipping): no file may name a cooker symbol, defined or
#                      undefined.
#   DE_EXPECT_COOKER=1 (Debug, Dev): the positive control. Some file names de_cooker_cook, and each
#                      executable with a symbol table (not a Windows .exe or .dll, whose symbols
#                      are in the .pdb) defines de_cooker_cook and de_asset_import_source.
#
# Without an nm the test prints "cooker_link_check: SKIPPED".
if(NOT DE_NM)
    message("cooker_link_check: SKIPPED (no nm or llvm-nm found)")
    return()
endif()
string(REPLACE "|" ";" files "${DE_FILES}")
set(sym_re "(^|[^A-Za-z0-9_])_?(de_cooker_[A-Za-z0-9_]+|de_cook_[A-Za-z0-9_]+|de_asset_import_[A-Za-z0-9_]+|de__cooker_[A-Za-z0-9_]+|de__import_ctx_[A-Za-z0-9_]+)")
set(failures "")
set(mentions_cook FALSE)
foreach(f IN LISTS files)
    if(NOT EXISTS "${f}")
        string(APPEND failures "  ${f}: missing (build it first)\n")
        continue()
    endif()
    execute_process(COMMAND "${DE_NM}" "${f}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        string(APPEND failures "  ${f}: ${DE_NM} failed (${rc}): ${err}\n")
        continue()
    endif()
    string(REPLACE "\n" ";" lines "${out}")
    set(found "")
    set(defines_cook FALSE)
    set(defines_import FALSE)
    foreach(line IN LISTS lines)
        if(line MATCHES "${sym_re}")
            list(APPEND found "${line}")
            if(line MATCHES "_?de_cooker_cook$")
                set(mentions_cook TRUE)
                if(line MATCHES " [TtSD] _?de_cooker_cook$")
                    set(defines_cook TRUE)
                endif()
            endif()
            if(line MATCHES " [TtSD] _?de_asset_import_source$")
                set(defines_import TRUE)
            endif()
        endif()
    endforeach()
    list(LENGTH found n)
    get_filename_component(name "${f}" NAME)
    if(DE_EXPECT_COOKER)
        if(NOT f MATCHES "\\.(a|lib)$" AND NOT f MATCHES "\\.(exe|dll)$")
            if(NOT defines_cook OR NOT defines_import)
                string(APPEND failures
                    "  ${name}: an executable of a ${DE_CONFIG} build does not define the cooker "
                    "(de_cooker_cook, de_asset_import_source)\n")
            endif()
        endif()
        message(STATUS "${name}: ${n} cooker symbol(s), as expected in ${DE_CONFIG}")
    elseif(n GREATER 0)
        list(SUBLIST found 0 10 shown)
        list(JOIN shown "\n    " shown)
        string(APPEND failures
            "  ${name}: ${n} cooker symbol(s) in a ${DE_CONFIG} build, e.g.\n    ${shown}\n")
    else()
        message(STATUS "${name}: no cooker symbol (${DE_CONFIG})")
    endif()
endforeach()
if(DE_EXPECT_COOKER AND NOT mentions_cook)
    string(APPEND failures "  no file names de_cooker_cook in a ${DE_CONFIG} build\n")
endif()
if(failures)
    message(FATAL_ERROR "cooker_link_check failed:\n${failures}")
endif()
message(STATUS "cooker_link_check: OK (${DE_CONFIG})")
