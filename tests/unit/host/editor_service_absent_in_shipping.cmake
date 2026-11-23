# editor_service_absent_in_shipping.cmake: CTest script (DENG-555, DIDE-R135 AC1). Checks that a
# Shipping build of the engine library has no editor-service or editor-IPC symbol (de__ed_*,
# de__editor_*, de_ep_*), defined or referenced: the service is Dev-only.
#
#   cmake -DDE_SOURCE_DIR=... -DDE_BUILD_DIR=... -DDE_IS_SHIPPING=0|1 -DDE_NM=<nm> \
#         -DDE_GENERATOR=... [-DDE_MAKE_PROGRAM=...] [-DDE_TOOLCHAIN_FILE=...] \
#         [-DDE_C_COMPILER=... -DDE_CXX_COMPILER=...] -DDE_HEADLESS=ON|OFF \
#         [-DDE_PARENT_LIB=<dengine of this build>] [-DDE_EXPECT_EDITOR=0|1] -P <this file>
#
# The same Shipping build as prof_no_tracy_in_shipping.cmake (DENG-94), in <build>/prof_shipping/
# with the same configure arguments, so the two checks share one build (both hold the
# `prof_shipping` resource lock). As a control, when this build has the editor service
# (DE_EXPECT_EDITOR) the same scan must find de__editor_start in this build's `dengine`, so a
# scan that sees nothing cannot pass. Without an nm tool the test is reported as skipped.

set(_name editor_service_absent_in_shipping)
if(NOT DE_NM)
    message("${_name}: SKIPPED (no nm or llvm-nm found)")
    return()
endif()

function(de__nm lib out_var)
    execute_process(COMMAND "${DE_NM}" "${lib}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${_name}: '${DE_NM} ${lib}' failed (${rc}):\n${err}")
    endif()
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

function(de__find_lib dir out_var)
    file(GLOB_RECURSE libs "${dir}/libdengine.a" "${dir}/dengine.lib")
    list(FILTER libs EXCLUDE REGEX "/CMakeFiles/")
    list(LENGTH libs n)
    if(NOT n EQUAL 1)
        message(FATAL_ERROR "${_name}: expected one engine library under ${dir}, found ${n}: ${libs}")
    endif()
    set(${out_var} "${libs}" PARENT_SCOPE)
endfunction()

function(de__run what)
    execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${_name}: ${what} failed (${rc}):\n${out}\n${err}")
    endif()
endfunction()

# A symbol name (after nm's type letter) that starts with one of the service prefixes; Mach-O
# adds a leading underscore.
set(_editor_re " _?(de__ed_|de__editor_|de_ep_)[A-Za-z0-9_]*")

if(DE_IS_SHIPPING)
    de__run("building dengine" "${CMAKE_COMMAND}" --build "${DE_BUILD_DIR}" --target dengine)
    de__find_lib("${DE_BUILD_DIR}/src" lib)
else()
    if(DE_EXPECT_EDITOR AND DE_PARENT_LIB AND EXISTS "${DE_PARENT_LIB}")
        de__nm("${DE_PARENT_LIB}" syms)
        if(NOT syms MATCHES " _?de__editor_start")
            message(FATAL_ERROR "${_name}: control failed: no de__editor_start in "
                                "${DE_PARENT_LIB}, which has the editor service; the scan is broken")
        endif()
        message(STATUS "control: ${DE_PARENT_LIB} has the editor service, as it should")
    endif()

    # Keep these arguments identical to prof_no_tracy_in_shipping.cmake: one shared build.
    set(ship "${DE_BUILD_DIR}/prof_shipping")
    set(args -S "${DE_SOURCE_DIR}" -B "${ship}" -G "${DE_GENERATOR}"
        -DCMAKE_BUILD_TYPE=Shipping
        -DDE_BUILD_TESTS=OFF -DDE_BUILD_TOOLS=OFF -DDE_BUILD_EXAMPLES=OFF
        "-DDE_HEADLESS=${DE_HEADLESS}"
        -DCMAKE_OPTIMIZE_DEPENDENCIES=ON)
    foreach(v MAKE_PROGRAM TOOLCHAIN_FILE C_COMPILER CXX_COMPILER)
        if(DE_${v})
            list(APPEND args "-DCMAKE_${v}=${DE_${v}}")
        endif()
    endforeach()
    de__run("configuring the Shipping build" "${CMAKE_COMMAND}" ${args})
    de__run("building the Shipping dengine" "${CMAKE_COMMAND}" --build "${ship}" --target dengine)
    de__find_lib("${ship}/src" lib)
endif()

de__nm("${lib}" syms)
string(REGEX MATCHALL "[^\n]*${_editor_re}[^\n]*" hits "${syms}")
if(hits)
    list(JOIN hits "\n  " hits)
    message(FATAL_ERROR "${_name}: editor service symbols in the Shipping ${lib}:\n  ${hits}")
endif()
message(STATUS "${_name}: ${lib}: no editor service symbol")
