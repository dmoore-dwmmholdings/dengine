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

# DENG-325: pipelines, draws, dispatches, indirect-count draws (ICBs) and the binary archive
# (test_mtl_pipeline.c: CTest gpu_metal.mtl.*, the names and expected values of DENG-323's Vulkan
# tests). It uses DENG-323's shared helper ../data/pipelines/pipeline_shaders.c, which cooks the
# shared shaders to metallib through the shader importer when the build has it (DE_BUILD_TOOLS and
# Slang); otherwise the GPU pipeline tests skip. The archive tests write below
# <build>/tests/unit/gpu/metal/test_gpu_metal_pipecache/ (mounted as mtlcache:/).
if(TARGET test_gpu_metal)
    target_sources(test_gpu_metal PRIVATE test_mtl_pipeline.c ../data/pipelines/pipeline_shaders.c)
    target_include_directories(test_gpu_metal PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../data/pipelines")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_gpu_metal_pipecache")
    target_compile_definitions(test_gpu_metal PRIVATE
        "DE_MTL_TEST_CACHE_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_gpu_metal_pipecache\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_gpu_metal PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_gpu_metal PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_gpu_metal PRIVATE DE_PIPE_TEST_COOK=1
            "DE_PIPE_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../data/pipelines\"")
    endif()
endif()
