# third_party/vulkan_headers: Khronos Vulkan-Headers (Apache-2.0 OR MIT; we take Apache-2.0),
# pinned in third_party/vendor.toml. DENG-283.
#
# de_tp_vulkan_headers (INTERFACE): <vulkan/vulkan.h> and the video codec headers it includes, as
# SYSTEM includes, with VK_NO_PROTOTYPES: nothing links a Vulkan loader. The Vulkan RHI backend
# (src/gpu/vulkan/vk_loader.c) opens libvulkan at run time and resolves every entry point through
# vkGetInstanceProcAddr / vkGetDeviceProcAddr, so the build needs no Vulkan SDK on any host.
add_library(de_tp_vulkan_headers INTERFACE)
target_include_directories(de_tp_vulkan_headers SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(de_tp_vulkan_headers INTERFACE VK_NO_PROTOTYPES)
set_target_properties(de_tp_vulkan_headers PROPERTIES FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: the headers compile in engine code and have
# the pinned version. CTest entries: de_tp_smoke_vulkan_headers.<test>.
de_add_test(NAME de_tp_smoke_vulkan_headers
    SOURCES smoke/smoke_vulkan_headers.c
    LIBS de_tp_vulkan_headers
    LABELS unit third_party)
