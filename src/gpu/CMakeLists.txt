# src/gpu: module `gpu` (target de_gpu), layer content.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME gpu
    LAYER content
    SOURCES gpu_stub.c
    DEPS core platform
)

# Backends (d3d12/, vulkan/, metal/) and any later child directory.
de_add_subdirectories()
