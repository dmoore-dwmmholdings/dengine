# src/gpu/d3d12: module `gpu_d3d12` (target de_gpu_d3d12), layer content.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME gpu_d3d12
    LAYER content
    SOURCES d3d12_stub.c
    DEPS gpu
    OPTIONAL_OPTION DE_GPU_D3D12
)
