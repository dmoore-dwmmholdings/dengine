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

# DENG-290: tier evaluation (src/gpu/tier.c). test_gpu_tier.c runs de_gpu_evaluate_tier on
# synthetic caps (GTX 1060, RTX 3070, M1, M3, Steam Deck, missing SM 6.6, ...) and on the null
# device; CTest names gpu.gpu_tier.<test>. The de-caps JSON tests are in tools/caps/.
if(TARGET test_gpu)
    target_sources(test_gpu PRIVATE test_gpu_tier.c)
endif()

# DENG-323: Vulkan pipelines, draws, dispatches and the pipeline cache. test_vk_pipeline.c joins
# test_gpu_vulkan (CTest names gpu_vulkan.vk.<test>) with the shared S10 pipeline-test helper
# data/pipelines/pipeline_shaders.c, which DENG-324 (D3D12) and DENG-325 (Metal) reuse: it cooks
# data/pipelines/*.slang through the shader importer (DE_PIPE_TEST_COOK) when the build has it
# (DE_BUILD_TOOLS and Slang, as gpu_shader above); otherwise the GPU pipeline tests skip. The cache
# tests write below <build>/tests/unit/gpu/test_gpu_vulkan_pipecache/ (mounted as pipecache:/).
if(TARGET test_gpu_vulkan)
    target_sources(test_gpu_vulkan PRIVATE vulkan/test_vk_pipeline.c data/pipelines/pipeline_shaders.c)
    target_include_directories(test_gpu_vulkan PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/data/pipelines")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_gpu_vulkan_pipecache")
    target_compile_definitions(test_gpu_vulkan PRIVATE
        "DE_PIPE_TEST_CACHE_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_gpu_vulkan_pipecache\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_gpu_vulkan PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_gpu_vulkan PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_gpu_vulkan PRIVATE DE_PIPE_TEST_COOK=1
            "DE_PIPE_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data/pipelines\"")
    endif()
endif()

# DENG-324: D3D12 pipelines, draws, dispatches and the pipeline library. test_d3d12_pipeline.c joins
# test_gpu_d3d12 (CTest names gpu_d3d12.d3d12.<test>) with DENG-323's shared helper
# data/pipelines/pipeline_shaders.c, which cooks the shared shaders to DXIL when the build has the
# shader importer and the host has DXC (Windows CI). The pure tests (DXIL reader, library file,
# keys, scissor) run on every host: without the D3D12 backend the test compiles the portable parts
# of d3d12_pipeline.c and d3d12_pipeline_library.c itself. The library tests write below
# <build>/tests/unit/gpu/test_gpu_d3d12_pipecache/ (mounted as d3d12cache:/).
if(TARGET test_gpu_d3d12)
    target_sources(test_gpu_d3d12 PRIVATE d3d12/test_d3d12_pipeline.c data/pipelines/pipeline_shaders.c)
    if(NOT DE_HAS_GPU_D3D12)
        target_sources(test_gpu_d3d12 PRIVATE
            "${DE_ROOT_DIR}/src/gpu/d3d12/d3d12_pipeline.c"
            "${DE_ROOT_DIR}/src/gpu/d3d12/d3d12_pipeline_library.c")
    endif()
    target_include_directories(test_gpu_d3d12 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/data/pipelines")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_gpu_d3d12_pipecache")
    target_compile_definitions(test_gpu_d3d12 PRIVATE
        "DE_D3D12_TEST_CACHE_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_gpu_d3d12_pipecache\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_gpu_d3d12 PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_gpu_d3d12 PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_gpu_d3d12 PRIVATE DE_PIPE_TEST_COOK=1
            "DE_PIPE_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data/pipelines\"")
    endif()
endif()

# DENG-405: the vendored WARP (third_party/d3d12_warp, Microsoft.Direct3D.WARP, test-only). On
# Windows x64 with DE_D3D12_TEST_WARP=ON, d3d10warp.dll is copied next to test_gpu_d3d12 so that
# its WARP device is the vendored one: Base tier (shader model 6.6), enhanced barriers and mesh
# shaders, so the DENG-324 pipeline tests and DENG-286's enhanced-barrier path run on CI.
# d3d12.vendored_warp (test_d3d12_warp.c) checks that it is the WARP loaded and that it has those
# features. Only this test executable gets the DLL.
if(TARGET test_gpu_d3d12 AND DE_D3D12_WARP_DLL)
    de_d3d12_warp_deploy(test_gpu_d3d12)
    target_sources(test_gpu_d3d12 PRIVATE d3d12/test_d3d12_warp.c)
endif()

# DENG-326: timestamps, readback, debug markers and object names on every backend
# (test_gpu_queries.c, executable test_gpu_queries, CTest names gpu_queries.queries.<test>). The
# front-end tests run on the null backend everywhere; the backend tests create devices of each
# backend compiled into the build (Metal on macOS, Vulkan where a loader is found, D3D12 on Windows,
# where CI runs them on the vendored WARP) and use DENG-323's shared compute shader, cooked through
# the shader importer when the build has it (else they skip). markers_nested_metal turns on
# MTL_CAPTURE_ENABLED and writes a .gputrace under <build>/tests/unit/gpu/test_gpu_queries_tmp/.
set(_de_queries_libs de_gpu de_platform)
if(TARGET de_gpu_vulkan)
    list(APPEND _de_queries_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(TARGET de_gpu_metal)
    list(APPEND _de_queries_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_queries_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME gpu_queries
    SOURCES test_gpu_queries.c data/pipelines/pipeline_shaders.c
    LIBS ${_de_queries_libs}
    LABELS unit gpu
    TIMEOUT 300)
if(TARGET test_gpu_queries)
    target_include_directories(test_gpu_queries PRIVATE "${DE_ROOT_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/pipelines")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_gpu_queries_tmp")
    target_compile_definitions(test_gpu_queries PRIVATE
        "DE_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_gpu_queries_tmp\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_gpu_queries PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_gpu_queries PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_gpu_queries PRIVATE DE_PIPE_TEST_COOK=1
            "DE_PIPE_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data/pipelines\"")
    endif()
    if(DE_HAS_GPU_D3D12)
        de_d3d12_agility_deploy(test_gpu_queries)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_gpu_queries)
        endif()
    endif()
endif()

# DENG-328: GPU crash diagnostics (test_breadcrumbs.c, executable test_breadcrumbs, CTest names
# gpu_breadcrumbs.breadcrumbs.<test>): breadcrumbs, device-lost reports and the crash bundle on a
# wrapped null backend (every host; the fork test on POSIX), breadcrumb ring writes on each GPU
# backend compiled in (DENG-323's compute shader, cooked when the build has the importer), and the
# Metal command-buffer error report (a simulated failure through de__mtl_errors_simulate).
set(_de_breadcrumbs_libs de_gpu de_platform)
if(TARGET de_gpu_vulkan)
    list(APPEND _de_breadcrumbs_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(TARGET de_gpu_metal)
    list(APPEND _de_breadcrumbs_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_breadcrumbs_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME gpu_breadcrumbs
    SOURCES test_breadcrumbs.c data/pipelines/pipeline_shaders.c
    LIBS ${_de_breadcrumbs_libs}
    LABELS unit gpu
    TIMEOUT 300)
if(TARGET test_gpu_breadcrumbs)
    target_include_directories(test_gpu_breadcrumbs PRIVATE "${DE_ROOT_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/pipelines")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_gpu_breadcrumbs PRIVATE de_gpu_shader de_cook_shader de_cooker
            de_asset)
        target_include_directories(test_gpu_breadcrumbs PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_gpu_breadcrumbs PRIVATE DE_PIPE_TEST_COOK=1
            "DE_PIPE_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data/pipelines\"")
    endif()
    if(DE_HAS_GPU_D3D12)
        de_d3d12_agility_deploy(test_gpu_breadcrumbs)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_gpu_breadcrumbs)
        endif()
    endif()
endif()
