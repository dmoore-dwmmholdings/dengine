/* test_vk_device.c — the Vulkan backend's device, caps and Base-tier check (DENG-283).
 * CTest names: gpu_vulkan.vk.<test>.
 *
 * The device tests need a Vulkan loader and driver. Without one they SKIP with the reason from
 * de_gpu_device_create (no loader, no device, or the device lacks a Base-tier feature, which is
 * named). DE_VULKAN_LIBRARY selects the library: a loader, or the MoltenVK dylib on macOS. The
 * Base-tier check itself is pure and runs everywhere.
 */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

/* The whole error chain ("outer: inner: ..."), for skip reasons. */
static const char *errchain(char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (const de_error *e = de_error_last(); e && used + 1 < cap; e = e->cause) {
        int w = snprintf(buf + used, cap - used, "%s%s", used ? " <- " : "", e->message);
        if (w < 0)
            break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
    }
    return buf;
}

static char g_skip[1200];
static char g_err[1100];

/* A Vulkan device, or SKIP when this machine cannot make one (the reason is in the skip text).
 * Any other failure is a test failure. */
static de_gpu_device *open_device(de_gpu_device_desc desc) {
    desc.backend = DE_GPU_VULKAN;
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&desc, &d);
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(g_skip, sizeof g_skip, "no usable Vulkan device: %s",
                 errchain(g_err, sizeof g_err));
        SKIP(g_skip);
    }
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(d != NULL);
    return d;
}

static void print_caps(const de_gpu_caps *c) {
    printf("vulkan caps: \"%s\" vendor 0x%04x device 0x%04x\n", c->device_name, c->vendor_id,
           c->device_id);
    printf("  vram %llu MiB, shared %llu MiB, rt %d (ray query %d, pipelines %d), mesh %d "
           "(%u verts, %u prims), async compute %d\n",
           (unsigned long long)(c->dedicated_vram >> 20),
           (unsigned long long)(c->shared_memory >> 20), (int)c->raytracing, (int)c->ray_query,
           (int)c->rt_pipelines, (int)c->mesh_shaders, c->mesh_max_output_vertices,
           c->mesh_max_output_primitives, (int)c->async_compute);
    printf("  waves %u..%u, bindless textures %u storage %u buffers %u samplers %u, push %u B, "
           "timestamps %llu Hz, base tier %s%s%s\n",
           c->wave_size_min, c->wave_size_max, c->max_bindless_textures,
           c->max_bindless_storage_textures, c->max_bindless_buffers, c->max_bindless_samplers,
           c->max_push_constant_bytes, (unsigned long long)c->timestamp_frequency,
           c->base_tier_ok ? "ok" : "NOT ok", c->base_tier_ok ? "" : ": ", c->base_tier_reason);
}

TEST(vk, device_create_caps) {
    de_gpu_device *d = open_device((de_gpu_device_desc){0});
    const de_gpu_caps *c = de_gpu_get_caps(d);
    print_caps(c);
    EXPECT_EQ(c->backend, DE_GPU_VULKAN);
    EXPECT_GT(strlen(c->device_name), (size_t)0);
    EXPECT_TRUE(c->base_tier_ok); /* not forced: creation succeeds only for Base-tier devices */
    EXPECT_STREQ(c->base_tier_reason, "");
    EXPECT_EQ(c->max_bindless_textures, DE__VK_BINDLESS_TEXTURES);
    EXPECT_EQ(c->max_bindless_storage_textures, DE__VK_BINDLESS_STORAGE_TEXTURES);
    EXPECT_EQ(c->max_bindless_buffers, DE__VK_BINDLESS_BUFFERS);
    EXPECT_EQ(c->max_bindless_samplers, DE__VK_BINDLESS_SAMPLERS);
    EXPECT_EQ(c->max_bindless_accels, 0u);
    EXPECT_EQ(c->max_push_constant_bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    EXPECT_GT(c->timestamp_frequency, (u64)0);
    EXPECT_GE(c->wave_size_min, 1u);
    EXPECT_LE(c->wave_size_min, c->wave_size_max);
    EXPECT_GT(c->dedicated_vram + c->shared_memory, (u64)0);
    if (c->mesh_shaders) {
        EXPECT_GT(c->mesh_max_output_vertices, 0u);
        EXPECT_GT(c->mesh_max_output_primitives, 0u);
    } else {
        EXPECT_EQ(c->mesh_max_output_vertices, 0u);
    }
    if (c->raytracing == DE_RT_NONE) {
        EXPECT_FALSE(c->ray_query);
        EXPECT_FALSE(c->rt_pipelines);
    }

    /* Memory classes: three base classes, at least one image class. */
    EXPECT_GE(de__gpu_mem_class_count(d), 4u);

    /* Empty submits signal each queue's internal timeline; wait_idle sees them complete. */
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        de_gpu_submit s;
        memset(&s, 0, sizeof s);
        EXPECT_EQ(de_gpu_queue_submit(d, (de_gpu_queue)q, &s), DE_OK);
    }
    de_gpu_wait_idle(d);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        EXPECT_GE(d->vt->queue_completed(d, (de_gpu_queue)q), (u64)1);

    u32 warnings = 0;
    EXPECT_EQ(de__gpu_vk_validation_messages(d, &warnings), 0u);
    de_gpu_device_destroy(d);
}

TEST(vk, adapter_selection) {
    de_gpu_device *d = open_device((de_gpu_device_desc){0});
    char best[128];
    snprintf(best, sizeof best, "%s", de_gpu_get_caps(d)->device_name);
    de_gpu_device_destroy(d);

    /* An adapter past the end is DE_ERR_NOT_FOUND and says how many there are. */
    de_gpu_device *bad = NULL;
    EXPECT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_VULKAN, .adapter = 1000},
                                   &bad),
              DE_ERR_NOT_FOUND);
    EXPECT_TRUE(bad == NULL);
    EXPECT_TRUE(strstr(errchain(g_err, sizeof g_err), "adapter 1000") != NULL);

    /* Forcing adapter 1 always creates a device (it may be another than the best one); its caps
     * say whether it is Base-tier capable and why not. */
    de_gpu_device *forced = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_VULKAN, .adapter = 1},
                                   &forced),
              DE_OK);
    const de_gpu_caps *c = de_gpu_get_caps(forced);
    printf("adapter 1: \"%s\" (best: \"%s\")\n", c->device_name, best);
    EXPECT_EQ(c->base_tier_ok, c->base_tier_reason[0] == '\0');
    de_gpu_device_destroy(forced);
}

/* A device description that passes every Base-tier requirement. */
static void passing_device(de__vk_physical *p) {
    memset(p, 0, sizeof *p);
    p->api_version = VK_API_VERSION_1_3;
#define DE__T_SET(s, field) p->s.field = VK_TRUE;
    DE_VK_BASE_TIER_FEATURES(DE__T_SET)
#undef DE__T_SET
    VkPhysicalDeviceVulkan12Properties *l = &p->p12;
    l->maxPerStageDescriptorUpdateAfterBindSampledImages = 1u << 20;
    l->maxDescriptorSetUpdateAfterBindSampledImages = 1u << 20;
    l->maxPerStageDescriptorUpdateAfterBindStorageImages = 1u << 20;
    l->maxDescriptorSetUpdateAfterBindStorageImages = 1u << 20;
    l->maxPerStageDescriptorUpdateAfterBindSamplers = 1u << 20;
    l->maxDescriptorSetUpdateAfterBindSamplers = 1u << 20;
    l->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 1u << 20;
    l->maxDescriptorSetUpdateAfterBindStorageBuffers = 1u << 20;
    l->maxPerStageUpdateAfterBindResources = 1u << 22;
    p->props.properties.limits.maxPushConstantsSize = 256;
}

TEST(vk, required_features_reason) {
    de__vk_physical *p = (de__vk_physical *)de_heap_allocator()->alloc(
        de_heap_allocator()->user, sizeof *p, 16, DE_MEM_GENERAL);
    ASSERT_TRUE(p != NULL);
    char reason[128];

    passing_device(p);
    EXPECT_TRUE(de__vk_base_tier_check(p, reason, sizeof reason));
    EXPECT_STREQ(reason, "");

    /* Each required feature, missing on its own, fails the tier and is named in the reason. */
    u32 checked = 0;
#define DE__T_EACH(s, field)                                                                       \
    passing_device(p);                                                                             \
    p->s.field = VK_FALSE;                                                                         \
    EXPECT_FALSE(de__vk_base_tier_check(p, reason, sizeof reason));                                \
    EXPECT_TRUE(strstr(reason, #field) != NULL);                                                   \
    EXPECT_TRUE(strstr(reason, "missing Vulkan feature") != NULL);                                 \
    ++checked;
    DE_VK_BASE_TIER_FEATURES(DE__T_EACH)
#undef DE__T_EACH
    EXPECT_GE(checked, 15u);

    /* Vulkan 1.2 fails first, with both versions in the reason. */
    passing_device(p);
    p->api_version = VK_MAKE_API_VERSION(0, 1, 2, 198);
    EXPECT_FALSE(de__vk_base_tier_check(p, reason, sizeof reason));
    EXPECT_STREQ(reason, "needs Vulkan 1.3; the device has 1.2");

    /* Update-after-bind limits that cannot hold the bindless tables. */
    passing_device(p);
    p->p12.maxPerStageDescriptorUpdateAfterBindStorageImages = 128;
    EXPECT_FALSE(de__vk_base_tier_check(p, reason, sizeof reason));
    EXPECT_TRUE(strstr(reason, "maxPerStageDescriptorUpdateAfterBindStorageImages is 128") != NULL);

    /* The reason always fits de_gpu_caps.base_tier_reason. */
    passing_device(p);
    p->f12.descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
    EXPECT_FALSE(de__vk_base_tier_check(p, reason, sizeof reason));
    EXPECT_LT(strlen(reason), sizeof(((de_gpu_caps *)0)->base_tier_reason));

    de_heap_allocator()->free(de_heap_allocator()->user, p, sizeof *p);
}

TEST(vk, unsupported_until_deng284) {
    de_gpu_device *d = open_device((de_gpu_device_desc){0});
    /* Command lists and fences arrive with DENG-284; until then they fail cleanly. */
    EXPECT_TRUE(de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "x") == NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_fence_create(d, 0)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_accel_create(d, &(de_gpu_accel_desc){0})));
    de_gpu_device_destroy(d);
}
