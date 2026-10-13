# third_party/d3d12_agility: the D3D12 Agility SDK (SPEC §22, Microsoft redistributable), a
# prebuilt entry of third_party/vendor.toml (DENG-390, DENG-285). The headers (MIT, LICENSE) and
# the DLL licence (LICENSE.txt) are committed; D3D12Core.dll and d3d12SDKLayers.dll of host
# windows-x64 are extracted into <build>/prebuilt/d3d12_agility/ at configure time by
# de_prebuilt_import (cmake/DePrebuilt.cmake). No binary is ever committed.
#
# Only for builds with the D3D12 backend (DE_HAS_GPU_D3D12, Windows targets): other builds
# download nothing.
#
# de_tp_d3d12_agility (INTERFACE): the headers, as a SYSTEM include directory. Engine code that
# includes <d3d12.h> links it; its directory comes before the Windows SDK's, so the Agility
# d3d12.h wins over the older copy in the SDK (src/gpu/d3d12/d3d12_internal.h checks the version).
#
# de_d3d12_agility_deploy(<target>): copies the two DLLs into <target dir>/D3D12/ after each build
# of an executable that creates D3D12 devices. The executable must also define the Agility
# exports (src/gpu/d3d12/d3d12_agility_exports.h), which point the D3D12 loader at that
# directory. Call it from the CMakeLists.txt that defines the target (CMP0040). Defined in every
# build, as a no-op without the D3D12 backend, so callers need no guard.

function(de_d3d12_agility_deploy target)
    if(NOT DE_D3D12_AGILITY_BIN_DIR OR NOT TARGET ${target})
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${DE_D3D12_AGILITY_BIN_DIR}/D3D12Core.dll"
                "${DE_D3D12_AGILITY_BIN_DIR}/d3d12SDKLayers.dll"
                "$<TARGET_FILE_DIR:${target}>/D3D12/"
        COMMENT "Copying the D3D12 Agility SDK runtime next to ${target}"
        VERBATIM)
endfunction()

set(DE_D3D12_AGILITY_BIN_DIR "" CACHE INTERNAL "Directory of the Agility D3D12Core.dll")
if(NOT DE_HAS_GPU_D3D12)
    return()
endif()

add_library(de_tp_d3d12_agility INTERFACE)
target_include_directories(de_tp_d3d12_agility SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/build/native/include")
set_target_properties(de_tp_d3d12_agility PROPERTIES FOLDER "dengine/third_party")

include(DePrebuilt)
de_prebuilt_import(d3d12_agility OPTIONAL)
if(DE_PREBUILT_D3D12_AGILITY_FOUND)
    set(DE_D3D12_AGILITY_BIN_DIR "${DE_PREBUILT_D3D12_AGILITY_DIR}/build/native/bin/x64"
        CACHE INTERNAL "Directory of the Agility D3D12Core.dll")
else()
    # Not windows-x64 (for example a future Windows arm64 target): the executables run on the
    # D3D12 runtime of the OS, which may lack features the backend reports as missing.
    message(STATUS "d3d12_agility: no Agility runtime for this target; using the OS D3D12 runtime")
endif()
