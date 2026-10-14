# src/gpu/d3d12: module `gpu_d3d12` (target de_gpu_d3d12), layer content. The D3D12 RHI backend
# (SPEC §12.1, §3.3), Windows only (DE_GPU_D3D12, on by default on Windows). DENG-285: device,
# queues, caps, heaps, resources, bindless; DENG-286 adds command lists, sync and the swapchain.
# DENG-324 adds the root signature, pipelines, the pipeline library and draws.
#
# Built against the Agility SDK headers (de_tp_d3d12_agility, third_party/d3d12_agility/), which
# come before the Windows SDK's on the include path; d3d12.lib and dxgi.lib are from the Windows
# SDK (xwin or Visual Studio). The IIDs are defined in d3d12_device.c, so dxguid.lib is not
# needed. Executables that create D3D12 devices call de_d3d12_agility_deploy(<target>)
# (src/host/, cmake/DeAddGame.cmake, tests/): it copies the runtime and adds the exports of
# d3d12_agility_exports.h unless a source of the target includes it (DENG-423).
de_add_module(
    NAME gpu_d3d12
    LAYER content
    SOURCES
        d3d12_internal.h
        d3d12_agility_exports.h
        d3d12_caps.c
        d3d12_device.c
        d3d12_memory.c
        d3d12_resources.c
        d3d12_bindless.c
        d3d12_barriers.c
        d3d12_cmd.c
        d3d12_sync.c
        d3d12_swapchain.c
        d3d12_root_signature.c
        d3d12_pipeline.c
        d3d12_pipeline_library.c
        d3d12_draw.c
        d3d12_query.c
        d3d12_debug.c
        d3d12_dred.c
    DEPS gpu
    LIBS de_tp_d3d12_agility d3d12 dxgi
    OPTIONAL_OPTION DE_GPU_D3D12
)
