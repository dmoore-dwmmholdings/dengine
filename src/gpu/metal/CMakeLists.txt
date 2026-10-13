# src/gpu/metal: module `gpu/metal` (target de_gpu_metal), layer content. SPEC §12.1, §3.3.
# The Metal 3 RHI backend (DENG-287: device, queues, caps, heaps, resources, bindless; DENG-288:
# command buffers, sync, swapchain, residency). Objective-C with ARC stays in the .m files;
# mtl_internal.h is plain C. de_gpu's backend table names de__gpu_metal_vtable (DE_HAS_GPU_METAL).
if(NOT APPLE)
    return()
endif()

set(_de_mtl_objc_sources
    mtl_device.m
    mtl_caps.m
    mtl_memory.m
    mtl_resources.m
    mtl_bindless.m
)
de_add_module(
    NAME gpu_metal
    LAYER content
    SOURCES
        mtl_internal.h
        ${_de_mtl_objc_sources}
    DEPS gpu
    LIBS "-framework Metal" "-framework QuartzCore" "-framework Foundation"
    OPTIONAL_OPTION DE_GPU_METAL
)
set_source_files_properties(${_de_mtl_objc_sources} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
