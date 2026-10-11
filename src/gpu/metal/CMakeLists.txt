# src/gpu/metal: module `gpu_metal` (target de_gpu_metal), layer content.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME gpu_metal
    LAYER content
    SOURCES metal_stub.c
    DEPS gpu
    OPTIONAL_OPTION DE_GPU_METAL
)
