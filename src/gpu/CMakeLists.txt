# src/gpu: module `gpu` (target de_gpu), layer content. SPEC §12.1, DENG-281.
# The front-end (handle pools, validation, bindless indices, deferred destruction, the backend
# vtable) and the null backend. Other tasks append their sources to the SOURCES list, one per
# line (DENG-282 device heaps, DENG-289 shader/, DENG-290 tier.c); no new subdirectories.
de_add_module(
    NAME gpu
    LAYER content
    SOURCES
        gpu_backend.h
        gpu.c
        handle_pool.h
        handle_pool.c
        validation.c
        bindless_alloc.c
        deferred_destroy.c
        null/null_backend.c
        memory/gpu_tlsf.h
        memory/gpu_tlsf.c
        memory/gpu_heap.h
        memory/gpu_heap.c
        tier.c
        query.c
        breadcrumbs_internal.h
        breadcrumbs.c
    DEPS core platform
)

# Backends (d3d12/, vulkan/, metal/) and any later child directory.
de_add_subdirectories()

# gpu.c's backend table names each compiled backend's vtable (DE_HAS_GPU_<NAME>), so de_gpu links
# the backend libraries back. The backends DEPS on gpu, so this is a cycle of static libraries,
# which CMake repeats on the link line where the linker needs it.
foreach(_de_gpu_backend IN ITEMS d3d12 metal vulkan)
    if(TARGET de_gpu_${_de_gpu_backend})
        target_link_libraries(de_gpu PUBLIC de_gpu_${_de_gpu_backend})
    endif()
endforeach()

# DENG-289: src/gpu/shader/ (the `shader` asset type) is its own module, target de_gpu_shader,
# added by de_add_subdirectories() above: it depends on de_asset, which de_gpu must not.
