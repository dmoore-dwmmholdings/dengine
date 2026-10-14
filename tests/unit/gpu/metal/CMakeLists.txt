# tests/unit/gpu/metal: the Metal backend (DENG-287), executable test_gpu_metal, CTest names
# gpu_metal.<test>, labels unit;gpu;metal. Runs on the host GPU with Metal API validation on
# (test_mtl_device.c sets MTL_DEBUG_LAYER=1 before the first device). The tests are C and include
# src/gpu/metal/mtl_internal.h; mtl_test_helpers.m is the Objective-C side (independent Metal
# queries, a compute kernel that reads the bindless table). Tests skip without a Metal device, and
# the GPU read skips below the Base tier.
if(NOT TARGET de_gpu_metal)
    return()
endif()

de_add_test(NAME gpu_metal
    SOURCES test_mtl_device.c test_mtl_resources.c mtl_test_helpers.m
    LIBS de_gpu de_gpu_metal "-framework Metal" "-framework Foundation"
    LABELS unit gpu
    TIMEOUT 300)
if(TARGET test_gpu_metal)
    set_source_files_properties(mtl_test_helpers.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(test_gpu_metal PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_gpu_metal PRIVATE "DE_TEST_ROOT_DIR=\"${DE_ROOT_DIR}\"")
endif()

# DENG-288: command lists, barriers, fences, residency (test_mtl_cmd.c: CTest gpu_metal.mtl.*, the
# same names and expected values as DENG-284's Vulkan tests where they overlap) and the
# CAMetalLayer swapchain (test_mtl_swapchain.c: a hidden window; skips without a display).
# mtl_cmd_helpers.m is their Objective-C side. test_mtl_cmd.c also turns Metal shader validation
# on (MTL_SHADER_VALIDATION=1) in processes that run an mtl.* test.
if(TARGET test_gpu_metal)
    target_sources(test_gpu_metal PRIVATE test_mtl_cmd.c test_mtl_swapchain.c mtl_cmd_helpers.m)
    set_source_files_properties(mtl_cmd_helpers.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(test_gpu_metal PRIVATE de_platform "-framework QuartzCore")
    if(TARGET de_tp_sdl3) # the swapchain test resizes its window through SDL
        target_link_libraries(test_gpu_metal PRIVATE de_tp_sdl3)
        target_compile_definitions(test_gpu_metal PRIVATE DE_MTL_TEST_SDL=1)
    endif()
endif()
