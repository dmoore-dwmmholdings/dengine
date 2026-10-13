/* vk_memory.c — Vulkan memory classes and device heaps for the front-end sub-allocator
 * (DENG-283 on DENG-282's gpu/memory/gpu_heap.h).
 *
 * Classes (heap-manager ids; backend_bits = memory type index | DE__VK_CLASS_LINEAR):
 *   0 GPU_ONLY buffers   the best DEVICE_LOCAL type a buffer may use (prefers memory the CPU
 *                        cannot see: real VRAM on a discrete GPU)
 *   1 UPLOAD buffers     HOST_VISIBLE | HOST_COHERENT, preferring uncached (write-combined)
 *                        system memory
 *   2 READBACK buffers   HOST_VISIBLE | HOST_COHERENT | HOST_CACHED, else HOST_VISIBLE | COHERENT
 *   3 GPU_ONLY images    the best DEVICE_LOCAL type an optimal-tiling image may use
 *   4+                   every other usable DEVICE_LOCAL type, once for buffers and once for
 *                        images, for resources whose memoryTypeBits exclude the primary type
 * Buffers and optimal-tiling images never share a class, so bufferImageGranularity never applies
 * inside a heap (DENG-282 decision). Host-visible heaps are mapped whole when created
 * (heap->mapped), so a buffer's CPU pointer is mapped + offset. Mapped classes are coherent:
 * de_gpu_buffer_map has no flush or invalidate call.
 *
 * Block size per class: an eighth of the Vulkan memory heap, between 16 MiB and 256 MiB (the
 * default); requests above a quarter of the block get their own heap.
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

#define DE__VK_CLASS_LINEAR 0x100u
#define DE__VK_TYPE_MASK    0xffu

static const VkMemoryPropertyFlags k_unusable =
    VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT |
    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;

/* The memoryTypeBits of a typical buffer and of a typical optimal image. */
static void probe_type_bits(de__vk_device *vk, u32 *buffer_bits, u32 *image_bits) {
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 65536;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkDeviceBufferMemoryRequirements breq;
    memset(&breq, 0, sizeof breq);
    breq.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS;
    breq.pCreateInfo = &bi;
    VkMemoryRequirements2 out;
    memset(&out, 0, sizeof out);
    out.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vk->fn.vkGetDeviceBufferMemoryRequirements(vk->device, &breq, &out);
    *buffer_bits = out.memoryRequirements.memoryTypeBits;

    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = ii.extent.height = 256;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDeviceImageMemoryRequirements ireq;
    memset(&ireq, 0, sizeof ireq);
    ireq.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS;
    ireq.pCreateInfo = &ii;
    memset(&out, 0, sizeof out);
    out.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vk->fn.vkGetDeviceImageMemoryRequirements(vk->device, &ireq, &out);
    *image_bits = out.memoryRequirements.memoryTypeBits;
}

typedef enum purpose { P_GPU, P_UPLOAD, P_READBACK } purpose;

/* Higher is better; 0 = not usable for the purpose. */
static u64 type_score(const VkPhysicalDeviceMemoryProperties *m, u32 t, purpose p) {
    const VkMemoryPropertyFlags f = m->memoryTypes[t].propertyFlags;
    const u64 heap_mib = m->memoryHeaps[m->memoryTypes[t].heapIndex].size >> 20;
    if (f & k_unusable)
        return 0;
    const bool local = (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    const bool visible = (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const bool coherent = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    const bool cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    u64 s = 1 + (heap_mib > 0xffffffu ? 0xffffffu : heap_mib); /* tie-break: the larger heap */
    switch (p) {
    case P_GPU:
        if (!local)
            return 0;
        return s + (visible ? 0 : (u64)1 << 40);
    case P_UPLOAD:
        if (!visible || !coherent)
            return 0;
        return s + (cached ? 0 : (u64)1 << 40) + (local ? 0 : (u64)1 << 39);
    case P_READBACK:
        if (!visible || !coherent)
            return 0;
        return s + (cached ? (u64)1 << 40 : 0);
    }
    return 0;
}

static u32 best_type(const VkPhysicalDeviceMemoryProperties *m, u32 bits, purpose p) {
    u32 best = UINT32_MAX;
    u64 best_score = 0;
    for (u32 t = 0; t < m->memoryTypeCount; ++t) {
        if (!(bits & (1u << t)))
            continue;
        const u64 s = type_score(m, t, p);
        if (s > best_score) {
            best_score = s;
            best = t;
        }
    }
    return best;
}

static u64 block_size_for(const VkPhysicalDeviceMemoryProperties *m, u32 t) {
    u64 heap = m->memoryHeaps[m->memoryTypes[t].heapIndex].size;
    u64 b = heap / 8u;
    const u64 lo = (u64)16 << 20, hi = DE__GPU_HEAP_DEFAULT_BLOCK_SIZE;
    b = b < lo ? lo : (b > hi ? hi : b);
    return b & ~(((u64)1 << 20) - 1u);
}

static de_result add_class(de_gpu_device *d, int base_id, u32 type, bool linear,
                           de_gpu_memory memory) {
    de__vk_device *vk = de__vk(d);
    const VkPhysicalDeviceMemoryProperties *m = &vk->phys.mem;
    static const char *const k_mem[] = {"gpu_only", "upload", "readback"};
    char name[32];
    snprintf(name, sizeof name, "vk.%s.%s.t%u", k_mem[memory], linear ? "buf" : "img", type);
    de__gpu_mem_class_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.memory = memory;
    cd.name = name;
    cd.block_size = block_size_for(m, type);
    cd.dedicated_threshold = cd.block_size / 4u;
    cd.backend_bits = type | (linear ? DE__VK_CLASS_LINEAR : 0u);
    u32 id;
    if (base_id >= 0) {
        id = (u32)base_id;
        DE_TRY(de__gpu_mem_configure_class(d, id, &cd));
    } else {
        if (vk->class_count >= DE__GPU_MEM_CLASS_MAX)
            return DE_OK; /* out of class ids: the resource falls back to the primary class */
        DE_TRY(de__gpu_mem_add_class(d, &cd, &id));
    }
    if (id >= DE__GPU_MEM_CLASS_MAX)
        return de__gpu_fail(d, DE_ERR_INTERNAL, "vulkan: memory class id %u out of range", id);
    vk->classes[id].type = type;
    vk->classes[id].linear = linear;
    vk->classes[id].memory = memory;
    vk->classes[id].flags = m->memoryTypes[type].propertyFlags;
    if (id + 1u > vk->class_count)
        vk->class_count = id + 1u;
    return DE_OK;
}

de_result de__vk_memory_init(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    const VkPhysicalDeviceMemoryProperties *m = &vk->phys.mem;
    u32 buffer_bits, image_bits;
    probe_type_bits(vk, &buffer_bits, &image_bits);

    const u32 gpu_buf = best_type(m, buffer_bits, P_GPU);
    const u32 upload = best_type(m, buffer_bits, P_UPLOAD);
    const u32 readback = best_type(m, buffer_bits, P_READBACK);
    const u32 gpu_img = best_type(m, image_bits, P_GPU);
    if (gpu_buf == UINT32_MAX || upload == UINT32_MAX || readback == UINT32_MAX ||
        gpu_img == UINT32_MAX)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "vulkan: \"%s\" lacks a usable memory type for %s",
                            vk->phys.props.properties.deviceName,
                            gpu_buf == UINT32_MAX    ? "device-local buffers"
                            : upload == UINT32_MAX   ? "upload buffers (host-visible, coherent)"
                            : readback == UINT32_MAX ? "readback buffers (host-visible, coherent)"
                                                     : "device-local images");
    vk->class_count = 0;
    DE_TRY(add_class(d, DE__GPU_MEM_CLASS_GPU_ONLY, gpu_buf, true, DE_MEM_GPU_ONLY));
    DE_TRY(add_class(d, DE__GPU_MEM_CLASS_UPLOAD, upload, true, DE_MEM_UPLOAD));
    DE_TRY(add_class(d, DE__GPU_MEM_CLASS_READBACK, readback, true, DE_MEM_READBACK));
    DE_TRY(add_class(d, -1, gpu_img, false, DE_MEM_GPU_ONLY));
    /* Every other usable device-local type, for resources that cannot use the primary one. */
    for (u32 t = 0; t < m->memoryTypeCount; ++t) {
        if (type_score(m, t, P_GPU) == 0)
            continue;
        if (t != gpu_buf)
            DE_TRY(add_class(d, -1, t, true, DE_MEM_GPU_ONLY));
        if (t != gpu_img)
            DE_TRY(add_class(d, -1, t, false, DE_MEM_GPU_ONLY));
    }
    return DE_OK;
}

u32 de__vk_class_mask(const de__vk_device *vk, bool linear, de_gpu_memory memory, u32 type_bits) {
    u32 mask = 0;
    for (u32 c = 0; c < vk->class_count; ++c) {
        const de__vk_mem_class *k = &vk->classes[c];
        if (k->linear == linear && k->memory == memory && (type_bits & (1u << k->type)))
            mask |= 1u << c;
    }
    return mask;
}

u32 de__vk_pick_class(const de__vk_device *vk, u32 mask, de_gpu_memory memory) {
    for (u32 c = 0; c < vk->class_count; ++c)
        if ((mask & (1u << c)) && vk->classes[c].memory == memory)
            return c;
    return DE__GPU_MEM_CLASS_MAX;
}

/* ---------- Heaps ---------- */

de_result de__vk_heap_create(de_gpu_device *d, de__gpu_heap *h, const de__gpu_mem_class_desc *cls) {
    de__vk_device *vk = de__vk(d);
    de__vk_heap *vh = DE__GPU_PAYLOAD(de__vk_heap, h);
    const u32 type = cls->backend_bits & DE__VK_TYPE_MASK;
    const bool linear = (cls->backend_bits & DE__VK_CLASS_LINEAR) != 0;

    VkMemoryAllocateFlagsInfo flags;
    memset(&flags, 0, sizeof flags);
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    /* Buffers in a linear heap may ask for their device address (shaders, ray tracing). */
    ai.pNext = linear && vk->phys.f12.bufferDeviceAddress ? &flags : NULL;
    ai.allocationSize = h->size;
    ai.memoryTypeIndex = type;
    VkResult vr = vk->fn.vkAllocateMemory(vk->device, &ai, NULL, &vh->memory);
    if (vr != VK_SUCCESS) {
        vh->memory = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkAllocateMemory(%llu bytes, type %u, class \"%s\")",
                           (unsigned long long)h->size, type, cls->name ? cls->name : "?");
    }
    if (vk->phys.mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void *p = NULL;
        vr = vk->fn.vkMapMemory(vk->device, vh->memory, 0, VK_WHOLE_SIZE, 0, &p);
        if (vr != VK_SUCCESS) {
            vk->fn.vkFreeMemory(vk->device, vh->memory, NULL);
            vh->memory = VK_NULL_HANDLE;
            return de__vk_fail(d, vr, "vulkan: vkMapMemory(%llu bytes, type %u)",
                               (unsigned long long)h->size, type);
        }
        h->mapped = p;
    }
    return DE_OK;
}

void de__vk_heap_destroy(de_gpu_device *d, de__gpu_heap *h) {
    de__vk_device *vk = de__vk(d);
    de__vk_heap *vh = DE__GPU_PAYLOAD(de__vk_heap, h);
    if (vh->memory)
        vk->fn.vkFreeMemory(vk->device, vh->memory, NULL); /* also unmaps */
    vh->memory = VK_NULL_HANDLE;
    h->mapped = NULL;
}

/* ---------- Requirements ---------- */

de_result de__vk_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                     de__gpu_mem_req *out) {
    de__vk_device *vk = de__vk(d);
    VkBufferCreateInfo bi;
    de__vk_buffer_info(vk, desc, &bi);
    VkDeviceBufferMemoryRequirements req;
    memset(&req, 0, sizeof req);
    req.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS;
    req.pCreateInfo = &bi;
    VkMemoryRequirements2 mr;
    memset(&mr, 0, sizeof mr);
    mr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vk->fn.vkGetDeviceBufferMemoryRequirements(vk->device, &req, &mr);
    out->size = mr.memoryRequirements.size;
    out->align = mr.memoryRequirements.alignment ? mr.memoryRequirements.alignment : 1u;
    out->class_mask =
        de__vk_class_mask(vk, true, desc->memory, mr.memoryRequirements.memoryTypeBits);
    if (!out->class_mask)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "vulkan: no memory class can hold buffer \"%s\" (memoryTypeBits 0x%x)",
                            desc->name ? desc->name : "", mr.memoryRequirements.memoryTypeBits);
    return DE_OK;
}

de_result de__vk_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                      de__gpu_mem_req *out) {
    de__vk_device *vk = de__vk(d);
    VkImageCreateInfo ii;
    de__vk_image_info(desc, &ii);
    DE_TRY(de__vk_image_supported(d, desc, &ii));
    VkDeviceImageMemoryRequirements req;
    memset(&req, 0, sizeof req);
    req.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS;
    req.pCreateInfo = &ii;
    VkMemoryRequirements2 mr;
    memset(&mr, 0, sizeof mr);
    mr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vk->fn.vkGetDeviceImageMemoryRequirements(vk->device, &req, &mr);
    out->size = mr.memoryRequirements.size;
    out->align = mr.memoryRequirements.alignment ? mr.memoryRequirements.alignment : 1u;
    out->class_mask =
        de__vk_class_mask(vk, false, DE_MEM_GPU_ONLY, mr.memoryRequirements.memoryTypeBits);
    if (!out->class_mask)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "vulkan: no memory class can hold texture \"%s\" (memoryTypeBits 0x%x)",
                            desc->name ? desc->name : "", mr.memoryRequirements.memoryTypeBits);
    return DE_OK;
}
