# prof_no_tracy_in_shipping.cmake: CTest script (DENG-94, SPEC §18.1). Checks that a Shipping
# build of the engine library has no Tracy symbol, defined or referenced.
#
#   cmake -DDE_SOURCE_DIR=... -DDE_BUILD_DIR=... -DDE_IS_SHIPPING=0|1 -DDE_NM=<nm> \
#         -DDE_GENERATOR=... [-DDE_MAKE_PROGRAM=...] [-DDE_TOOLCHAIN_FILE=...] \
#         [-DDE_C_COMPILER=... -DDE_CXX_COMPILER=...] -DDE_HEADLESS=ON|OFF \
#         [-DDE_PARENT_LIB=<dengine of this build>] [-DDE_EXPECT_TRACY=0|1] -P <this file>
#
# In a Shipping build it builds and checks this build's `dengine`. Otherwise it configures a
# Shipping build of the same source with the same generator and compilers in
# <build>/prof_shipping/ (no tests, tools or examples), builds only the `dengine` target, and
# checks that. As a control, when this build links Tracy (DE_EXPECT_TRACY) and its `dengine`
# exists, the same nm scan must find Tracy symbols in it, so a scan that sees nothing cannot pass.
# Without an nm tool the test is reported as skipped.

if(NOT DE_NM)
    message("prof_no_tracy_in_shipping: SKIPPED (no nm or llvm-nm found)")
    return()
endif()

# Lists the symbols of `lib` in `out_var`.
function(de__nm lib out_var)
    execute_process(COMMAND "${DE_NM}" "${lib}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "prof_no_tracy_in_shipping: '${DE_NM} ${lib}' failed (${rc}):\n${err}")
    endif()
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# The engine library in `dir` (static: libdengine.a or dengine.lib).
function(de__find_lib dir out_var)
    file(GLOB_RECURSE libs "${dir}/libdengine.a" "${dir}/dengine.lib")
    list(FILTER libs EXCLUDE REGEX "/CMakeFiles/")
    list(LENGTH libs n)
    if(NOT n EQUAL 1)
        message(FATAL_ERROR "prof_no_tracy_in_shipping: expected one engine library under ${dir}, "
                            "found ${n}: ${libs}")
    endif()
    set(${out_var} "${libs}" PARENT_SCOPE)
endfunction()

function(de__run what)
    execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "prof_no_tracy_in_shipping: ${what} failed (${rc}):\n${out}\n${err}")
    endif()
endfunction()

set(_tracy_re "[Tt]racy")

if(DE_IS_SHIPPING)
    de__run("building dengine" "${CMAKE_COMMAND}" --build "${DE_BUILD_DIR}" --target dengine)
    de__find_lib("${DE_BUILD_DIR}/src" lib)
else()
    # Control: this build's library does show Tracy symbols to the same scan.
    if(DE_EXPECT_TRACY AND DE_PARENT_LIB AND EXISTS "${DE_PARENT_LIB}")
        de__nm("${DE_PARENT_LIB}" syms)
        if(NOT syms MATCHES "___tracy")
            message(FATAL_ERROR "prof_no_tracy_in_shipping: control failed: no ___tracy symbol "
                                "in ${DE_PARENT_LIB}, which links Tracy; the scan is broken")
        endif()
        message(STATUS "control: ${DE_PARENT_LIB} has Tracy symbols, as it should")
    endif()

    set(ship "${DE_BUILD_DIR}/prof_shipping")
    set(args -S "${DE_SOURCE_DIR}" -B "${ship}" -G "${DE_GENERATOR}"
        -DCMAKE_BUILD_TYPE=Shipping
        -DDE_BUILD_TESTS=OFF -DDE_BUILD_TOOLS=OFF -DDE_BUILD_EXAMPLES=OFF
        "-DDE_HEADLESS=${DE_HEADLESS}"
        # A static library needs none of its link dependencies built (SDL3 is not compiled).
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
string(REGEX MATCHALL "[^\n]*${_tracy_re}[^\n]*" hits "${syms}")
if(hits)
    list(JOIN hits "\n  " hits)
    message(FATAL_ERROR "prof_no_tracy_in_shipping: Tracy symbols in the Shipping ${lib}:\n  ${hits}")
endif()
message(STATUS "prof_no_tracy_in_shipping: ${lib}: no Tracy symbol")
