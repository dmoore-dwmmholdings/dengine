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
