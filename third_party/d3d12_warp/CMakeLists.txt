# third_party/d3d12_warp: Microsoft Direct3D WARP (NuGet Microsoft.Direct3D.WARP), a newer
# software rasterizer for the D3D12 tests (DENG-405). A prebuilt entry of third_party/vendor.toml
# (DENG-390): only the licence text (LICENSE, LICENSE.TXT) is committed; d3d10warp.dll of host
# windows-x64 is extracted into <build>/prebuilt/d3d12_warp/ at configure time by
# de_prebuilt_import (cmake/DePrebuilt.cmake). No binary is ever committed.
#
# TEST-ONLY. The licence allows use "for testing purposes only" and forbids sharing or
# distributing the DLL (third_party/README.md, policy rule 9). So:
#   - it is imported only when the tests and the D3D12 backend are built (DE_BUILD_TESTS,
#     DE_HAS_GPU_D3D12) and DE_D3D12_TEST_WARP is ON;
#   - only test executables call de_d3d12_warp_deploy(); src/host/, cmake/DeAddGame.cmake, tools
#     and packaging never do;
#   - CI never uploads it (the logs artifacts hold Testing/Temporary/ and configure logs only).
#
# How it is used: the D3D12 runtime loads the WARP adapter's driver, d3d10warp.dll, through the
# normal DLL search order, which starts at the executable's directory. A copy next to the test
# executable therefore replaces the WARP of Windows (System32) in that process. It changes only
# WARP: hardware adapters do not use the DLL.
#
# DE_D3D12_TEST_WARP (option, default ON): the test-only switch. OFF runs the tests on the WARP
# of the OS (on the windows-2022 runners: shader model 6.2, no enhanced barriers, no mesh shaders),
# for example to compare the two.
#
# de_d3d12_warp_deploy(<target>): copies d3d10warp.dll next to <target> after each build, and
# defines DE_D3D12_TEST_WARP_DLL=1 on it, so the target's tests know that the vendored WARP must
# be the one loaded. Defined in every build, as a no-op when there is nothing to deploy, so
# callers need no guard. Call it from the CMakeLists.txt that defines the target (CMP0040).

option(DE_D3D12_TEST_WARP
    "Copy the vendored WARP (third_party/d3d12_warp, test-only) next to the D3D12 test executables"
    ON)

function(de_d3d12_warp_deploy target)
    if(NOT DE_D3D12_WARP_DLL OR NOT TARGET ${target})
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DE_D3D12_WARP_DLL}"
                "$<TARGET_FILE_DIR:${target}>/"
        COMMENT "Copying the vendored WARP (test-only) next to ${target}"
        VERBATIM)
    target_compile_definitions(${target} PRIVATE DE_D3D12_TEST_WARP_DLL=1)
endfunction()

set(DE_D3D12_WARP_DLL "" CACHE INTERNAL "The vendored d3d10warp.dll (test-only)")
if(NOT DE_HAS_GPU_D3D12 OR NOT DE_BUILD_TESTS OR NOT DE_D3D12_TEST_WARP)
    return()
endif()

include(DePrebuilt)
de_prebuilt_import(d3d12_warp OPTIONAL)
if(DE_PREBUILT_D3D12_WARP_FOUND)
    set(DE_D3D12_WARP_DLL "${DE_PREBUILT_D3D12_WARP_DIR}/build/native/bin/x64/d3d10warp.dll"
        CACHE INTERNAL "The vendored d3d10warp.dll (test-only)")
else()
    # Not windows-x64: the tests use the WARP of the OS.
    message(STATUS "d3d12_warp: no vendored WARP for this target; the tests use the OS WARP")
endif()
