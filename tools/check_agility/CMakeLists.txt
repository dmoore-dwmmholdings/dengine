# tools/check_agility: the D3D12 Agility exports of every deployed executable (DENG-423).
#
#   agility_exports_check     check_agility.py on <build>/de_d3d12_agility_deployed_<config>.txt,
#                             the executables de_d3d12_agility_deploy() deployed: each must export
#                             D3D12SDKVersion and D3D12SDKPath, or it silently runs on the OS
#                             D3D12 runtime (DENG-420). Windows builds with the Agility runtime;
#                             run it after building everything (CI does).
#   agility_exports_selftest  the checker on synthetic PE files with and without the exports, on
#                             every host.
#
# Plain Python (stdlib only, 3.10+), ctest label `check`.
if(NOT DE_BUILD_TESTS)
    return()
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_test(NAME agility_exports_selftest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_selftest.py")
set_tests_properties(agility_exports_selftest PROPERTIES LABELS check TIMEOUT 60)

if(DE_D3D12_AGILITY_BIN_DIR)
    add_test(NAME agility_exports_check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_agility.py"
                --manifest "${CMAKE_BINARY_DIR}/de_d3d12_agility_deployed_$<CONFIG>.txt"
                --expect-some)
    set_tests_properties(agility_exports_check PROPERTIES LABELS check TIMEOUT 60)
endif()
