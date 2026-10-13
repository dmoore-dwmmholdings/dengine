# tests/unit/gpu: unit tests of module `gpu` (de_gpu, SPEC §12.1). Later S09 tasks append their
# test sources or executables below.
#
# DENG-281: the front-end and the null backend. CTest names: gpu.<test> (suite gpu),
# gpu.handle_pool.<test>, gpu.gpu_validation.<test>. The tests include internal headers
# ("gpu/gpu_backend.h") for the null backend's manual fence control and the pool/bindless/deferred
# counters. Label `tsan`: gpu.threads_create_destroy_submit runs 8 threads on one device.
# The gpu module does not exist in DE_HEADLESS builds (cmake/DeOptions.cmake), so neither do these.
if(NOT DE_HAS_GPU)
    return()
endif()

de_add_test(NAME gpu
    SOURCES test_gpu_null.c test_handle_pool.c test_gpu_validation.c
    LIBS de_gpu
    LABELS unit tsan
    TIMEOUT 120)
if(TARGET test_gpu)
    target_include_directories(test_gpu PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-282: device memory heaps. The offset TLSF (gpu.gpu_tlsf.<test>) and the heap manager with
# transient regions on the null backend (gpu.gpu_heap.<test>; gpu_heap.threads runs 8 threads on
# one device). gpu.gpu_tlsf.perf asserts its 200 ms bound in Release and Shipping only.
if(TARGET test_gpu)
    target_sources(test_gpu PRIVATE test_gpu_tlsf.c test_gpu_heap.c)
endif()

# DENG-289: the shader pipeline. test_shader_cook.c cooks tests/unit/gpu/data/shaders/ (copied into
# a scratch directory of the build tree, mounted as shd:/) with the shader importer
# (de_cook_shader) through the cooker, checks SPIR-V (spirv-val from the Vulkan SDK when present),
# metallib (loaded on the Metal device, shader_metal_check.m), reflection, the push-constant limit,
# determinism, import invalidation, and the Dev on-demand cook and hot reload of the runtime type
# (de_gpu_shader). CTest names gpu_shader.shader.<test>. Needs the tools (the importer) and Slang;
# tests/ is added before tools/, so test for the option and the Slang target, not for the importer.
if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
    set(_de_shader_test_sources test_shader_cook.c)
    if(APPLE)
        list(APPEND _de_shader_test_sources shader_metal_check.m)
        set_source_files_properties(shader_metal_check.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()
    de_add_test(NAME gpu_shader
        SOURCES ${_de_shader_test_sources}
        LIBS de_gpu_shader de_cook_shader de_cooker de_asset de_task de_platform
        TIMEOUT 300)
    if(TARGET test_gpu_shader)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_gpu_shader_tmp")
        target_include_directories(test_gpu_shader PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_gpu_shader PRIVATE
            "DE_SHADER_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data/shaders\""
            "DE_SHADER_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_gpu_shader_tmp\"")
        if(APPLE)
            target_compile_definitions(test_gpu_shader PRIVATE DE_SHADER_TEST_METAL=1)
            target_link_libraries(test_gpu_shader PRIVATE "-framework Metal" "-framework Foundation")
        endif()
    endif()
endif()

# DENG-283: the Vulkan backend (src/gpu/vulkan/). test_vk_device.c (device, caps, adapters, the pure
# Base-tier check) and test_vk_resources.c (1000 buffers/textures/samplers, mapping, bindless
# descriptors, placed resources, threads) build executable test_gpu_vulkan; CTest names
# gpu_vulkan.vk.<test>. Without a Vulkan loader or driver the device tests SKIP with the reason;
# DE_VULKAN_LIBRARY points them at a loader or ICD (the MoltenVK dylib on macOS). With
# VK_LAYER_KHRONOS_validation installed they also require zero validation errors. DENG-284 appends
# its command, sync and swapchain tests to this executable.
if(DE_HAS_GPU_VULKAN AND TARGET de_gpu_vulkan)
    de_add_test(NAME gpu_vulkan
        SOURCES vulkan/test_vk_device.c vulkan/test_vk_resources.c
        LIBS de_gpu de_gpu_vulkan de_tp_vulkan_headers
        LABELS unit tsan
        TIMEOUT 300)
    if(TARGET test_gpu_vulkan)
        target_include_directories(test_gpu_vulkan PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endif()

# DENG-287: the Metal backend (tests/unit/gpu/metal/, executable test_gpu_metal). The directory
# returns early when de_gpu_metal is not built.
add_subdirectory(metal)

# DENG-285: the D3D12 backend, executable test_gpu_d3d12 (CTest names gpu_d3d12.d3d12.<test>).
# d3d12.caps_evaluator and d3d12.bindless_shared_numbering run on every host: without the D3D12
# backend the test compiles the pure caps evaluator (src/gpu/d3d12/d3d12_caps.c) itself. On
# Windows it also creates devices (every adapter, WARP on CI), with the Agility runtime copied
# next to it (the test exports D3D12SDKVersion / D3D12SDKPath).
set(_de_d3d12_test_sources d3d12/test_d3d12_device.c)
set(_de_d3d12_test_libs de_gpu)
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_d3d12_test_libs de_gpu_d3d12 de_tp_d3d12_agility)
else()
    list(APPEND _de_d3d12_test_sources "${DE_ROOT_DIR}/src/gpu/d3d12/d3d12_caps.c")
endif()
de_add_test(NAME gpu_d3d12
    SOURCES ${_de_d3d12_test_sources}
    LIBS ${_de_d3d12_test_libs}
    TIMEOUT 300)
if(TARGET test_gpu_d3d12)
    target_include_directories(test_gpu_d3d12 PRIVATE "${DE_ROOT_DIR}/src")
    de_d3d12_agility_deploy(test_gpu_d3d12)
endif()

# DENG-284: Vulkan command lists, barriers, fences and swapchains. test_vk_cmd.c (clear_readback,
# copy_bc7_mips, cross_queue_timeline on the best device or adapter 1 forced; the pure
# barrier_translation_table) and test_vk_swapchain.c (swapchain_present_resize on a hidden window;
# skips without a display) join test_gpu_vulkan. With VK_LAYER_KHRONOS_validation installed they run
# with synchronization validation and require zero validation errors.
if(TARGET test_gpu_vulkan)
    target_sources(test_gpu_vulkan PRIVATE vulkan/test_vk_cmd.c vulkan/test_vk_swapchain.c)
    target_link_libraries(test_gpu_vulkan PRIVATE de_platform)
    if(TARGET de_tp_sdl3) # the swapchain test resizes its window through SDL
        target_link_libraries(test_gpu_vulkan PRIVATE de_tp_sdl3)
        target_compile_definitions(test_gpu_vulkan PRIVATE DE_VK_TEST_SDL=1)
    endif()
endif()

# DENG-286: D3D12 command lists, barriers, fences and swapchains (CTest names gpu_d3d12.d3d12.<test>).
# test_d3d12_barrier_tables.c (barrier_translation_table, the pure translation) runs on every host:
# without the D3D12 backend the test compiles src/gpu/d3d12/d3d12_barriers.c itself. On Windows,
# test_d3d12_cmd.c (clear_readback, copy_bc7_mips, cross_queue_timeline, legacy_barriers_forced,
# swapchain_present_resize; DENG-284's names and values) runs on the best adapter (WARP on CI) with
# the debug layer.
if(TARGET test_gpu_d3d12)
    target_sources(test_gpu_d3d12 PRIVATE d3d12/test_d3d12_barrier_tables.c)
    if(DE_HAS_GPU_D3D12)
        target_sources(test_gpu_d3d12 PRIVATE d3d12/test_d3d12_cmd.c)
        target_link_libraries(test_gpu_d3d12 PRIVATE de_platform)
        if(TARGET de_tp_sdl3) # the swapchain test resizes its window through SDL
            target_link_libraries(test_gpu_d3d12 PRIVATE de_tp_sdl3)
            target_compile_definitions(test_gpu_d3d12 PRIVATE DE_D3D12_TEST_SDL=1)
        endif()
    else()
        target_sources(test_gpu_d3d12 PRIVATE "${DE_ROOT_DIR}/src/gpu/d3d12/d3d12_barriers.c")
    endif()
endif()
