# src/gpu/vulkan: module `gpu_vulkan` (target de_gpu_vulkan), layer content. SPEC §3.3, §12.1.
# The Vulkan 1.3 RHI backend (DENG-283: device, queues, caps, memory, resources, bindless; DENG-284
# appends command lists, barriers, sync and the swapchain; DENG-323 pipelines, draws and the
# pipeline cache). The Khronos headers are vendored
# (third_party/vulkan_headers, VK_NO_PROTOTYPES) and vk_loader.c opens libvulkan at run time, so
# the backend builds on every desktop host without a Vulkan SDK (cmake/DeOptions.cmake turns
# DE_GPU_VULKAN on by default on Linux, Windows and macOS).
de_add_module(
    NAME gpu_vulkan
    LAYER content
    SOURCES
        vk_internal.h
        vk_loader.c
        vk_device.c
        vk_caps.c
        vk_memory.c
        vk_resources.c
        vk_bindless.c
        vk_cmd.c
        vk_barriers.c
        vk_sync.c
        vk_swapchain.c
        vk_pipeline.c
        vk_pipeline_cache.c
        vk_draw.c
    DEPS gpu
    LIBS de_tp_vulkan_headers
    OPTIONAL_OPTION DE_GPU_VULKAN
)
