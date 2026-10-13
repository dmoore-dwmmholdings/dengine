/* vk_bindless.c — the Vulkan bindless descriptor set (DENG-283; SPEC §12.1; layout fixed by
 * DENG-289 in shaders/common/bindless.slang and the decision "Shader resource conventions: one
 * bindless table set and one push-constant block per backend").
 *
 * One descriptor set, set 0, shared by every pipeline through one pipeline layout:
 *   binding 0  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE   [DE__VK_BINDLESS_TEXTURES]          de_tex2d
 *   binding 1  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE   [DE__VK_BINDLESS_STORAGE_TEXTURES]  de_rwtex2d
 *   binding 2  VK_DESCRIPTOR_TYPE_SAMPLER         [DE__VK_BINDLESS_SAMPLERS]          de_sampler
 *   binding 3  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER  [DE__VK_BINDLESS_BUFFERS]           de_buffer
 * Every binding is PARTIALLY_BOUND | UPDATE_AFTER_BIND | UPDATE_UNUSED_WHILE_PENDING, in an
 * UPDATE_AFTER_BIND pool: new indices are written while frames that use other indices are in
 * flight, and several threads may write different indices at once (the front-end never writes
 * one index twice until it is freed). The descriptor index is the front-end's bindless index.
 *
 * Index 0 of every binding holds a zeroed dummy: a 1x1 RGBA8 image (both image bindings, layout
 * GENERAL), a 256-byte buffer and a default sampler. A shader that reads index 0
 * (DE_GPU_BINDLESS_INVALID) gets zeros instead of undefined behaviour.
 *
 * The pipeline layout adds one push-constant range of min(128, maxPushConstantsSize) bytes at
 * offset 0 for all stages (shaders/common/push_constants.slang).
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

static const VkDescriptorType k_types[DE__VK_BINDING_COUNT] = {
    [DE__VK_BINDING_SAMPLED_IMAGES] = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    [DE__VK_BINDING_STORAGE_IMAGES] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    [DE__VK_BINDING_SAMPLERS] = VK_DESCRIPTOR_TYPE_SAMPLER,
    [DE__VK_BINDING_STORAGE_BUFFERS] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
};

static u32 min3(u32 a, u32 b, u32 c) {
    u32 m = a < b ? a : b;
    return m < c ? m : c;
}

/* Table sizes: the constants, or less on a forced device with smaller limits (at least 1). */
static void table_sizes(de__vk_device *vk) {
    const VkPhysicalDeviceVulkan12Properties *p = &vk->phys.p12;
    u32 s[DE__VK_BINDING_COUNT];
    s[DE__VK_BINDING_SAMPLED_IMAGES] =
        min3(DE__VK_BINDLESS_TEXTURES, p->maxPerStageDescriptorUpdateAfterBindSampledImages,
             p->maxDescriptorSetUpdateAfterBindSampledImages);
    s[DE__VK_BINDING_STORAGE_IMAGES] =
        min3(DE__VK_BINDLESS_STORAGE_TEXTURES, p->maxPerStageDescriptorUpdateAfterBindStorageImages,
             p->maxDescriptorSetUpdateAfterBindStorageImages);
    s[DE__VK_BINDING_SAMPLERS] =
        min3(DE__VK_BINDLESS_SAMPLERS, p->maxPerStageDescriptorUpdateAfterBindSamplers,
             p->maxDescriptorSetUpdateAfterBindSamplers);
    s[DE__VK_BINDING_STORAGE_BUFFERS] =
        min3(DE__VK_BINDLESS_BUFFERS, p->maxPerStageDescriptorUpdateAfterBindStorageBuffers,
             p->maxDescriptorSetUpdateAfterBindStorageBuffers);
    for (u32 i = 0; i < DE__VK_BINDING_COUNT; ++i)
        vk->table_size[i] = s[i] ? s[i] : 1u;
}

/* The set layout for the current table sizes; `b` and `f` back the pointers in `li` and `fi`. */
static void layout_info(const de__vk_device *vk, VkDescriptorSetLayoutBinding b[],
                        VkDescriptorBindingFlags f[],
                        VkDescriptorSetLayoutBindingFlagsCreateInfo *fi,
                        VkDescriptorSetLayoutCreateInfo *li) {
    memset(b, 0, sizeof(VkDescriptorSetLayoutBinding) * DE__VK_BINDING_COUNT);
    for (u32 i = 0; i < DE__VK_BINDING_COUNT; ++i) {
        b[i].binding = i;
        b[i].descriptorType = k_types[i];
        b[i].descriptorCount = vk->table_size[i];
        b[i].stageFlags = VK_SHADER_STAGE_ALL;
        f[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
               VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
               VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    memset(fi, 0, sizeof *fi);
    fi->sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    fi->bindingCount = DE__VK_BINDING_COUNT;
    fi->pBindingFlags = f;
    memset(li, 0, sizeof *li);
    li->sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li->pNext = fi;
    li->flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li->bindingCount = DE__VK_BINDING_COUNT;
    li->pBindings = b;
}

static bool layout_supported(const de__vk_device *vk) {
    VkDescriptorSetLayoutBinding b[DE__VK_BINDING_COUNT];
    VkDescriptorBindingFlags f[DE__VK_BINDING_COUNT];
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi;
    VkDescriptorSetLayoutCreateInfo li;
    layout_info(vk, b, f, &fi, &li);
    VkDescriptorSetLayoutSupport sup;
    memset(&sup, 0, sizeof sup);
    sup.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
    vk->fn.vkGetDescriptorSetLayoutSupport(vk->device, &li, &sup);
    return sup.supported != VK_FALSE;
}

/*
 * Sets above maxPerSetDescriptors are valid only when vkGetDescriptorSetLayoutSupport accepts them
 * (Vulkan 1.1). A device that refuses the full bindless set is not Base-tier capable; a forced one
 * gets tables halved until the set is supported (caps report the smaller sizes).
 */
static de_result check_layout_support(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    if (layout_supported(vk))
        return DE_OK;
    u32 total = 0;
    for (u32 i = 0; i < DE__VK_BINDING_COUNT; ++i)
        total += vk->table_size[i];
    vk->base_ok = false;
    snprintf(vk->base_reason, sizeof vk->base_reason,
             "bindless descriptor set (%u descriptors) not supported; maxPerSetDescriptors is %u",
             total, vk->phys.p11.maxPerSetDescriptors);
    if (!vk->forced)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "vulkan: \"%s\" is not Base-tier capable: %s (set "
                            "de_gpu_device_desc.adapter to use it anyway)",
                            vk->phys.props.properties.deviceName, vk->base_reason);
    for (;;) {
        bool shrunk = false;
        for (u32 i = 0; i < DE__VK_BINDING_COUNT; ++i)
            if (vk->table_size[i] > 16u) {
                vk->table_size[i] /= 2u;
                shrunk = true;
            }
        if (layout_supported(vk))
            break;
        if (!shrunk)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                                "vulkan: \"%s\" supports no bindless descriptor set",
                                vk->phys.props.properties.deviceName);
    }
    DE_WARN(d->log, "vulkan: bindless tables reduced to %u/%u/%u/%u on the forced adapter (%s)",
            vk->table_size[0], vk->table_size[1], vk->table_size[2], vk->table_size[3],
            vk->base_reason);
    return DE_OK;
}

static de_result create_layouts(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    DE_TRY(check_layout_support(d));
    VkDescriptorSetLayoutBinding b[DE__VK_BINDING_COUNT];
    VkDescriptorBindingFlags f[DE__VK_BINDING_COUNT];
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi;
    VkDescriptorSetLayoutCreateInfo li;
    layout_info(vk, b, f, &fi, &li);
    VkDescriptorPoolSize sizes[DE__VK_BINDING_COUNT];
    for (u32 i = 0; i < DE__VK_BINDING_COUNT; ++i) {
        sizes[i].type = k_types[i];
        sizes[i].descriptorCount = vk->table_size[i];
    }
    VkResult vr = vk->fn.vkCreateDescriptorSetLayout(vk->device, &li, NULL, &vk->set_layout);
    if (vr != VK_SUCCESS) {
        vk->set_layout = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: bindless descriptor set layout (%u/%u/%u/%u)",
                           vk->table_size[0], vk->table_size[1], vk->table_size[2],
                           vk->table_size[3]);
    }

    VkDescriptorPoolCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = DE__VK_BINDING_COUNT;
    pi.pPoolSizes = sizes;
    vr = vk->fn.vkCreateDescriptorPool(vk->device, &pi, NULL, &vk->descriptor_pool);
    if (vr != VK_SUCCESS) {
        vk->descriptor_pool = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: bindless descriptor pool");
    }
    VkDescriptorSetAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = vk->descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &vk->set_layout;
    vr = vk->fn.vkAllocateDescriptorSets(vk->device, &ai, &vk->set);
    if (vr != VK_SUCCESS) {
        vk->set = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: bindless descriptor set");
    }

    const u32 limit = vk->phys.props.properties.limits.maxPushConstantsSize;
    VkPushConstantRange pc;
    pc.stageFlags = VK_SHADER_STAGE_ALL;
    pc.offset = 0;
    pc.size = limit < DE_GPU_MAX_PUSH_CONSTANT_BYTES ? limit : DE_GPU_MAX_PUSH_CONSTANT_BYTES;
    VkPipelineLayoutCreateInfo pl;
    memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &vk->set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    vr = vk->fn.vkCreatePipelineLayout(vk->device, &pl, NULL, &vk->pipeline_layout);
    if (vr != VK_SUCCESS) {
        vk->pipeline_layout = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: bindless pipeline layout");
    }
    return DE_OK;
}

/* ---------- Writes ---------- */

static void record(de__vk_device *vk, u32 binding, u32 index, u64 handle) {
    if (vk->shadow[binding] && index < vk->table_size[binding])
        vk->shadow[binding][index] = handle;
}

static void write_image(de__vk_device *vk, u32 binding, u32 index, VkImageView view,
                        VkImageLayout layout) {
    VkDescriptorImageInfo ii;
    memset(&ii, 0, sizeof ii);
    ii.imageView = view;
    ii.imageLayout = layout;
    VkWriteDescriptorSet w;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = vk->set;
    w.dstBinding = binding;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = k_types[binding];
    w.pImageInfo = &ii;
    vk->fn.vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
    record(vk, binding, index, (u64)(uintptr_t)view);
}

static void write_sampler(de__vk_device *vk, u32 index, VkSampler sampler) {
    VkDescriptorImageInfo ii;
    memset(&ii, 0, sizeof ii);
    ii.sampler = sampler;
    VkWriteDescriptorSet w;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = vk->set;
    w.dstBinding = DE__VK_BINDING_SAMPLERS;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    vk->fn.vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
    record(vk, DE__VK_BINDING_SAMPLERS, index, (u64)(uintptr_t)sampler);
}

static void write_buffer(de__vk_device *vk, u32 index, VkBuffer buffer, u64 size) {
    VkDescriptorBufferInfo bi;
    bi.buffer = buffer;
    bi.offset = 0;
    /* A range above maxStorageBufferRange is invalid; shaders see the first part. */
    bi.range = size < vk->max_storage_range ? size : vk->max_storage_range;
    VkWriteDescriptorSet w;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = vk->set;
    w.dstBinding = DE__VK_BINDING_STORAGE_BUFFERS;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vk->fn.vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
    record(vk, DE__VK_BINDING_STORAGE_BUFFERS, index, (u64)(uintptr_t)buffer);
}

void de__vk_bindless_write_buffer(de_gpu_device *d, u32 index, de__gpu_buffer *b) {
    de__vk_device *vk = de__vk(d);
    write_buffer(vk, index, DE__GPU_PAYLOAD(de__vk_buffer, b)->buffer, b->size);
}

void de__vk_bindless_write_texture(de_gpu_device *d, u32 index, de__gpu_texture *t, u32 mip,
                                   bool storage) {
    de__vk_device *vk = de__vk(d);
    const de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
    if (storage)
        write_image(vk, DE__VK_BINDING_STORAGE_IMAGES, index, vt->storage_views[mip],
                    VK_IMAGE_LAYOUT_GENERAL);
    else
        write_image(vk, DE__VK_BINDING_SAMPLED_IMAGES, index, vt->sampled_view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void de__vk_bindless_write_sampler(de_gpu_device *d, u32 index, de__gpu_sampler *s) {
    write_sampler(de__vk(d), index, DE__GPU_PAYLOAD(de__vk_sampler, s)->sampler);
}

/* ---------- Index 0: zeroed dummies ---------- */

static de_result alloc_bind(de_gpu_device *d, VkMemoryRequirements mr, bool linear, u32 slot) {
    de__vk_device *vk = de__vk(d);
    u32 type = UINT32_MAX;
    for (u32 c = 0; c < vk->class_count && type == UINT32_MAX; ++c)
        if (vk->classes[c].linear == linear && vk->classes[c].memory == DE_MEM_GPU_ONLY &&
            (mr.memoryTypeBits & (1u << vk->classes[c].type)))
            type = vk->classes[c].type;
    if (type == UINT32_MAX)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "vulkan: no memory type for the null %s",
                            linear ? "buffer" : "image");
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    VkResult vr = vk->fn.vkAllocateMemory(vk->device, &ai, NULL, &vk->null_memory[slot]);
    if (vr != VK_SUCCESS) {
        vk->null_memory[slot] = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: memory for the null descriptors");
    }
    vr = linear ? vk->fn.vkBindBufferMemory(vk->device, vk->null_buffer, vk->null_memory[slot], 0)
                : vk->fn.vkBindImageMemory(vk->device, vk->null_image, vk->null_memory[slot], 0);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "vulkan: binding the null %s", linear ? "buffer" : "image");
    return DE_OK;
}

static de_result create_dummies(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = ii.extent.height = ii.extent.depth = 1;
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult vr = vk->fn.vkCreateImage(vk->device, &ii, NULL, &vk->null_image);
    if (vr != VK_SUCCESS) {
        vk->null_image = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: null image");
    }
    VkMemoryRequirements mr;
    vk->fn.vkGetImageMemoryRequirements(vk->device, vk->null_image, &mr);
    DE_TRY(alloc_bind(d, mr, false, 0));
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = vk->null_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    vr = vk->fn.vkCreateImageView(vk->device, &vi, NULL, &vk->null_sampled_view);
    if (vr == VK_SUCCESS)
        vr = vk->fn.vkCreateImageView(vk->device, &vi, NULL, &vk->null_storage_view);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "vulkan: null image views");

    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 256;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vr = vk->fn.vkCreateBuffer(vk->device, &bi, NULL, &vk->null_buffer);
    if (vr != VK_SUCCESS) {
        vk->null_buffer = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: null buffer");
    }
    vk->fn.vkGetBufferMemoryRequirements(vk->device, vk->null_buffer, &mr);
    DE_TRY(alloc_bind(d, mr, true, 1));

    VkSamplerCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.maxLod = VK_LOD_CLAMP_NONE;
    vr = vk->fn.vkCreateSampler(vk->device, &si, NULL, &vk->null_sampler);
    if (vr != VK_SUCCESS) {
        vk->null_sampler = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: null sampler");
    }
    return DE_OK;
}

/* Clears the dummy image and buffer to zero and leaves the image in GENERAL: one command buffer
 * on the graphics queue at device creation, waited for with vkQueueWaitIdle. */
static de_result clear_dummies(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    const de__vk_queue *q = &vk->queues[DE_QUEUE_GRAPHICS];
    VkCommandPoolCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = q->family;
    VkCommandPool pool;
    VkResult vr = vk->fn.vkCreateCommandPool(vk->device, &pi, NULL, &pool);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "vulkan: command pool for the null descriptors");

    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vr = vk->fn.vkAllocateCommandBuffers(vk->device, &ai, &cb);
    if (vr == VK_SUCCESS) {
        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vr = vk->fn.vkBeginCommandBuffer(cb, &bi);
    }
    if (vr == VK_SUCCESS) {
        VkImageMemoryBarrier2 ib;
        memset(&ib, 0, sizeof ib);
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        ib.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        ib.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = vk->null_image;
        ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ib.subresourceRange.levelCount = 1;
        ib.subresourceRange.layerCount = 1;
        VkDependencyInfo dep;
        memset(&dep, 0, sizeof dep);
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &ib;
        vk->fn.vkCmdPipelineBarrier2(cb, &dep);

        VkClearColorValue zero;
        memset(&zero, 0, sizeof zero);
        vk->fn.vkCmdClearColorImage(cb, vk->null_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero,
                                    1, &ib.subresourceRange);
        vk->fn.vkCmdFillBuffer(cb, vk->null_buffer, 0, VK_WHOLE_SIZE, 0u);

        /* Make the clears visible to every later shader access; the image stays in GENERAL. */
        ib.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ib.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkMemoryBarrier2 mb;
        memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vk->fn.vkCmdPipelineBarrier2(cb, &dep);
        vr = vk->fn.vkEndCommandBuffer(cb);
    }
    if (vr == VK_SUCCESS) {
        VkCommandBufferSubmitInfo cbi;
        memset(&cbi, 0, sizeof cbi);
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbi.commandBuffer = cb;
        VkSubmitInfo2 si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        vr = vk->fn.vkQueueSubmit2(q->queue, 1, &si, VK_NULL_HANDLE);
        if (vr == VK_SUCCESS)
            vr = vk->fn.vkQueueWaitIdle(q->queue);
    }
    vk->fn.vkDestroyCommandPool(vk->device, pool, NULL);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "vulkan: clearing the null descriptors");
    return DE_OK;
}

/* ---------- Init / shutdown ---------- */

de_result de__vk_bindless_init(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    table_sizes(vk);
    vk->max_storage_range = vk->phys.props.properties.limits.maxStorageBufferRange;
    DE_TRY(create_layouts(d));
#if DE__GPU_VALIDATE_COMMANDS
    for (u32 i = 0; i < DE__VK_BINDING_COUNT; ++i) {
        const size_t bytes = (size_t)vk->table_size[i] * sizeof(u64);
        vk->shadow[i] = (u64 *)DE_ALLOC(d->alloc, bytes, 8, DE_MEM_RENDER);
        if (!vk->shadow[i])
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "vulkan: bindless shadow table (%zu bytes)",
                                bytes);
        memset(vk->shadow[i], 0, bytes);
    }
#endif
    DE_TRY(create_dummies(d));
    DE_TRY(clear_dummies(d));
    write_image(vk, DE__VK_BINDING_SAMPLED_IMAGES, 0, vk->null_sampled_view,
                VK_IMAGE_LAYOUT_GENERAL);
    write_image(vk, DE__VK_BINDING_STORAGE_IMAGES, 0, vk->null_storage_view,
                VK_IMAGE_LAYOUT_GENERAL);
    write_sampler(vk, 0, vk->null_sampler);
    write_buffer(vk, 0, vk->null_buffer, 256);
    return DE_OK;
}

void de__vk_bindless_shutdown(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    /* Every handle below is non-null only if its create call (and entry point) succeeded. */
    if (vk->null_sampled_view)
        vk->fn.vkDestroyImageView(vk->device, vk->null_sampled_view, NULL);
    if (vk->null_storage_view)
        vk->fn.vkDestroyImageView(vk->device, vk->null_storage_view, NULL);
    if (vk->null_image)
        vk->fn.vkDestroyImage(vk->device, vk->null_image, NULL);
    if (vk->null_buffer)
        vk->fn.vkDestroyBuffer(vk->device, vk->null_buffer, NULL);
    if (vk->null_sampler)
        vk->fn.vkDestroySampler(vk->device, vk->null_sampler, NULL);
    for (u32 i = 0; i < 2; ++i)
        if (vk->null_memory[i])
            vk->fn.vkFreeMemory(vk->device, vk->null_memory[i], NULL);
    if (vk->pipeline_layout)
        vk->fn.vkDestroyPipelineLayout(vk->device, vk->pipeline_layout, NULL);
    if (vk->descriptor_pool)
        vk->fn.vkDestroyDescriptorPool(vk->device, vk->descriptor_pool, NULL);
    if (vk->set_layout)
        vk->fn.vkDestroyDescriptorSetLayout(vk->device, vk->set_layout, NULL);
    vk->null_sampled_view = vk->null_storage_view = VK_NULL_HANDLE;
    vk->null_image = VK_NULL_HANDLE;
    vk->null_buffer = VK_NULL_HANDLE;
    vk->null_sampler = VK_NULL_HANDLE;
    vk->null_memory[0] = vk->null_memory[1] = VK_NULL_HANDLE;
    vk->pipeline_layout = VK_NULL_HANDLE;
    vk->descriptor_pool = VK_NULL_HANDLE;
    vk->set_layout = VK_NULL_HANDLE;
    vk->set = VK_NULL_HANDLE;
    for (u32 i = 0; i < DE__VK_BINDING_COUNT; ++i) {
        if (vk->shadow[i])
            DE_FREE(d->alloc, vk->shadow[i], (size_t)vk->table_size[i] * sizeof(u64));
        vk->shadow[i] = NULL;
    }
}

/* ---------- Test hook ---------- */

bool de__gpu_vk_bindless_written(de_gpu_device *d, de__gpu_bindless_space space, u32 index,
                                 u64 *handle) {
    if (handle)
        *handle = 0;
    if (!d || d->vt != &de__gpu_vulkan_vtable)
        return false;
    de__vk_device *vk = de__vk(d);
    u32 binding;
    switch (space) {
    case DE__GPU_BINDLESS_SAMPLED_TEXTURE:
        binding = DE__VK_BINDING_SAMPLED_IMAGES;
        break;
    case DE__GPU_BINDLESS_STORAGE_TEXTURE:
        binding = DE__VK_BINDING_STORAGE_IMAGES;
        break;
    case DE__GPU_BINDLESS_SAMPLER:
        binding = DE__VK_BINDING_SAMPLERS;
        break;
    case DE__GPU_BINDLESS_BUFFER:
        binding = DE__VK_BINDING_STORAGE_BUFFERS;
        break;
    default:
        return false;
    }
    if (!vk->shadow[binding] || index >= vk->table_size[binding])
        return false;
    if (handle)
        *handle = vk->shadow[binding][index];
    return true;
}
