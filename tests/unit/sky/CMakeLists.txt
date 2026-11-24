# tests/unit/sky: the physical sky (src/world/sky, DENG-206).
#
# test_sky (every build; CTest names sky.<suite>.<test>): the celestial model against PyEphem
# references (test_celestial_sun_reference, test_celestial_moon_phase) and the CPU atmosphere.
de_add_test(NAME sky
    SOURCES test_celestial.c
    LIBS de_sky de_core)
if(TARGET test_sky)
    target_include_directories(test_sky PRIVATE "${DE_ROOT_DIR}/src")
endif()

# test_sky_gpu (render builds; CTest names sky_gpu.<suite>.<test>): de_sky_attach driving the
# sun light (no device), and the transmittance LUT against the CPU reference on each GPU backend
# compiled in (test_atmosphere_transmittance_cpu_vs_gpu_<backend>; SKIP without the device).
if(NOT DE_HAS_RENDER OR NOT DE_HAS_GPU)
    return()
endif()
set(_de_sky_libs de_sky de_render de_asset de_gpu de_ecs de_reflect de_task de_platform
    de_test_support)
if(TARGET de_gpu_vulkan)
    list(APPEND _de_sky_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(TARGET de_gpu_metal)
    list(APPEND _de_sky_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_sky_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME sky_gpu
    SOURCES test_sky_system.c test_atmosphere_cpu.c test_sky_ibl.c
    LIBS ${_de_sky_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_sky_gpu)
    target_include_directories(test_sky_gpu PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        de_d3d12_agility_deploy(test_sky_gpu)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_sky_gpu)
        endif()
    endif()
endif()

# test_sky_fog (render builds; CTest names sky_fog.fog.<test>): the froxel fog's CPU side and pass
# list on a null-backend device (DENG-207).
de_add_test(NAME sky_fog
    SOURCES test_fog_volumes.c
    LIBS ${_de_sky_libs})
if(TARGET test_sky_fog)
    target_include_directories(test_sky_fog PRIVATE "${DE_ROOT_DIR}/src")
endif()

# test_sky_clouds (render builds; CTest names sky_clouds.clouds.<test>): the volumetric clouds'
# settings, pass list on a null-backend device and large-coordinate weather offsets (DENG-208).
de_add_test(NAME sky_clouds
    SOURCES test_clouds.c
    LIBS ${_de_sky_libs})
if(TARGET test_sky_clouds)
    target_include_directories(test_sky_clouds PRIVATE "${DE_ROOT_DIR}/src")
endif()
