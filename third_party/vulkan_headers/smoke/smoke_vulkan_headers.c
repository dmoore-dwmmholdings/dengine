/* Smoke test for de_tp_vulkan_headers: <vulkan/vulkan.h> compiles under the engine's flags with
 * VK_NO_PROTOTYPES, has the pinned header version (vendor.toml: vulkan-sdk-1.4.363.0) and the
 * Vulkan 1.3 types the RHI backend uses. Nothing here calls Vulkan: no loader is linked. */
#include <string.h>

#include <vulkan/vulkan.h>

#include "de_test.h"

#ifndef VK_NO_PROTOTYPES
#error "de_tp_vulkan_headers must define VK_NO_PROTOTYPES (the backend loads Vulkan at run time)"
#endif

TEST(de_tp_smoke_vulkan_headers, version_and_types) {
    EXPECT_EQ(VK_HEADER_VERSION, 363);
    EXPECT_EQ(VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE), 1u);
    EXPECT_GE(VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE), 4u);
    EXPECT_EQ(VK_API_VERSION_MINOR(VK_API_VERSION_1_3), 3u);

    /* Function pointer types exist even without prototypes. */
    PFN_vkGetInstanceProcAddr gipa = NULL;
    PFN_vkQueueSubmit2 submit2 = NULL;
    EXPECT_TRUE(gipa == NULL && submit2 == NULL);

    VkPhysicalDeviceVulkan13Features f13;
    memset(&f13, 0, sizeof f13);
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    EXPECT_EQ((int)f13.sType, (int)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    EXPECT_EQ(sizeof(VkDeviceAddress), (size_t)8);
    EXPECT_EQ((unsigned)VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 1u);
}
