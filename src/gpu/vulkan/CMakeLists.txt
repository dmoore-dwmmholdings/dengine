# src/gpu/vulkan: module `gpu_vulkan` (target de_gpu_vulkan), layer content.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME gpu_vulkan
    LAYER content
    SOURCES vulkan_stub.c
    DEPS gpu
    OPTIONAL_OPTION DE_GPU_VULKAN
)
