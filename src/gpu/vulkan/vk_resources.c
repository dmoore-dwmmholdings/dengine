/* vk_resources.c — Vulkan buffers, images and samplers (DENG-283; SPEC §12.1).
 *
 * Buffers: VkBuffer with the usage of the desc, plus TRANSFER_SRC | TRANSFER_DST always and
 * STORAGE_BUFFER | SHADER_DEVICE_ADDRESS when the buffer gets a bindless index (VERTEX, INDEX,
 * CONSTANT or STORAGE usage): every bindless buffer is a storage-buffer descriptor (binding 3) and
 * has a device address. Memory comes from the front-end sub-allocator (de__gpu_mem_alloc) in the
 * class picked by vk_memory.c, or from a transient region for placed buffers. UPLOAD and READBACK
 * buffers point `mapped` into their heap's persistent mapping.
 *
 * Textures: an optimal-tiling VkImage (cube textures CUBE_COMPATIBLE), starting in
 * VK_IMAGE_LAYOUT_UNDEFINED; a sampled view of the whole texture (2D, 2D_ARRAY, CUBE, CUBE_ARRAY or
 * 3D) for SAMPLED usage, and one storage view per mip (2D, 2D_ARRAY for arrays and cubes, or 3D)
 * for STORAGE usage. Combined depth-stencil formats are sampled through their depth aspect.
 * Render-target and depth attachment views are made by the command code (DENG-284).
 *
 * Samplers: filters and address modes from the desc, anisotropy when max_aniso > 1 and the device
 * has it (clamped to its limit), border colour transparent black, no LOD clamp; compare_reverse_z
 * makes a comparison sampler with VK_COMPARE_OP_GREATER_OR_EQUAL (reverse-Z depth).
 */
#include "gpu/vulkan/vk_internal.h"

#include <string.h>

/* ---------- Formats ---------- */

static const VkFormat k_formats[DE_FMT_COUNT_] = {
    [DE_FMT_UNKNOWN] = VK_FORMAT_UNDEFINED,
    [DE_FMT_R8_UNORM] = VK_FORMAT_R8_UNORM,
    [DE_FMT_RG8_UNORM] = VK_FORMAT_R8G8_UNORM,
    [DE_FMT_RGBA8_UNORM] = VK_FORMAT_R8G8B8A8_UNORM,
    [DE_FMT_RGBA8_SRGB] = VK_FORMAT_R8G8B8A8_SRGB,
    [DE_FMT_BGRA8_UNORM] = VK_FORMAT_B8G8R8A8_UNORM,
    [DE_FMT_BGRA8_SRGB] = VK_FORMAT_B8G8R8A8_SRGB,
    [DE_FMT_R16_FLOAT] = VK_FORMAT_R16_SFLOAT,
    [DE_FMT_RG16_FLOAT] = VK_FORMAT_R16G16_SFLOAT,
    [DE_FMT_RGBA16_FLOAT] = VK_FORMAT_R16G16B16A16_SFLOAT,
    [DE_FMT_R32_UINT] = VK_FORMAT_R32_UINT,
    [DE_FMT_RG32_UINT] = VK_FORMAT_R32G32_UINT,
    [DE_FMT_R32_FLOAT] = VK_FORMAT_R32_SFLOAT,
    [DE_FMT_RG32_FLOAT] = VK_FORMAT_R32G32_SFLOAT,
    [DE_FMT_RGB32_FLOAT] = VK_FORMAT_R32G32B32_SFLOAT,
    [DE_FMT_RGBA32_FLOAT] = VK_FORMAT_R32G32B32A32_SFLOAT,
    /* Same bit layout as DXGI R11G11B10_FLOAT / R10G10B10A2_UNORM: red in the low bits. */
    [DE_FMT_R11G11B10_FLOAT] = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
    [DE_FMT_RGB10A2_UNORM] = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
    [DE_FMT_D32_FLOAT] = VK_FORMAT_D32_SFLOAT,
    [DE_FMT_D32_FLOAT_S8] = VK_FORMAT_D32_SFLOAT_S8_UINT,
    [DE_FMT_BC1_SRGB] = VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
    [DE_FMT_BC3_SRGB] = VK_FORMAT_BC3_SRGB_BLOCK,
    [DE_FMT_BC4_UNORM] = VK_FORMAT_BC4_UNORM_BLOCK,
    [DE_FMT_BC5_UNORM] = VK_FORMAT_BC5_UNORM_BLOCK,
    [DE_FMT_BC6H_UFLOAT] = VK_FORMAT_BC6H_UFLOAT_BLOCK,
    [DE_FMT_BC7_UNORM] = VK_FORMAT_BC7_UNORM_BLOCK,
    [DE_FMT_BC7_SRGB] = VK_FORMAT_BC7_SRGB_BLOCK,
};

VkFormat de__vk_format(de_gpu_format f) {
    return (u32)f < DE_FMT_COUNT_ ? k_formats[f] : VK_FORMAT_UNDEFINED;
}

VkImageAspectFlags de__vk_format_aspect(de_gpu_format f) {
    if (f == DE_FMT_D32_FLOAT_S8)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (de__gpu_format_is_depth(f))
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

/* ---------- Create infos ---------- */

#define BINDLESS_BUFFER_USAGE                                                                      \
    (DE_USAGE_VERTEX | DE_USAGE_INDEX | DE_USAGE_CONSTANT | DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT)

void de__vk_buffer_info(const de__vk_device *vk, const de_gpu_buffer_desc *desc,
                        VkBufferCreateInfo *out) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (desc->usage & DE_USAGE_SHADER_BINDING_TABLE)
        u |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (desc->usage & DE_USAGE_VERTEX)
        u |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (desc->usage & DE_USAGE_INDEX)
        u |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (desc->usage & DE_USAGE_CONSTANT)
        u |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (desc->usage & DE_USAGE_INDIRECT)
        u |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (desc->usage & BINDLESS_BUFFER_USAGE) {
        u |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vk->phys.f12.bufferDeviceAddress)
            u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (vk->on.accel) {
        if (desc->usage & DE_USAGE_ACCEL_INPUT)
            u |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (desc->usage & DE_USAGE_ACCEL_STORAGE)
            u |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    } else if (desc->usage & (DE_USAGE_ACCEL_INPUT | DE_USAGE_ACCEL_STORAGE)) {
        /* Without ray tracing the buffer is ordinary storage (S16 decides what more it needs). */
        u |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    out->size = desc->size;
    out->usage = u;
    out->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
}

void de__vk_image_info(const de_gpu_texture_desc *desc, VkImageCreateInfo *out) {
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    out->imageType = desc->dim == DE_TEX_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    if (desc->dim == DE_TEX_CUBE)
        out->flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    out->format = de__vk_format(desc->format);
    out->extent.width = desc->width;
    out->extent.height = desc->height ? desc->height : 1u;
    const u32 layers =
        desc->depth_or_layers ? desc->depth_or_layers : (desc->dim == DE_TEX_CUBE ? 6u : 1u);
    out->extent.depth = desc->dim == DE_TEX_3D ? layers : 1u;
    out->arrayLayers = desc->dim == DE_TEX_3D ? 1u : layers;
    out->mipLevels = desc->mip_count ? desc->mip_count : 1u;
    const u32 samples = desc->sample_count ? desc->sample_count : 1u;
    out->samples = (VkSampleCountFlagBits)samples; /* 1, 2, 4, 8: the flag bit equals the count */
    out->tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageUsageFlags u = 0;
    if (desc->usage & DE_USAGE_SAMPLED)
        u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc->usage & DE_USAGE_STORAGE)
        u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (desc->usage & DE_USAGE_RENDER_TARGET)
        u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->usage & DE_USAGE_DEPTH)
        u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->usage & DE_USAGE_COPY_SRC)
        u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc->usage & DE_USAGE_COPY_DST)
        u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    out->usage = u;
    out->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    out->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

de_result de__vk_image_supported(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                 const VkImageCreateInfo *ci) {
    de__vk_device *vk = de__vk(d);
    const char *name = desc->name ? desc->name : "";
    if (desc->dim == DE_TEX_CUBE && ci->arrayLayers > 6 && (desc->usage & DE_USAGE_SAMPLED) &&
        !vk->on.cube_array)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_texture_create(\"%s\"): cube arrays need the imageCubeArray "
                            "feature, which this Vulkan device lacks",
                            name);
    VkImageFormatProperties fp;
    VkResult vr = vk->fn.vkGetPhysicalDeviceImageFormatProperties(
        vk->phys.handle, ci->format, ci->imageType, ci->tiling, ci->usage, ci->flags, &fp);
    if (vr != VK_SUCCESS)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_texture_create(\"%s\"): format %s with usage 0x%x is not "
                            "supported by this Vulkan device (%s)",
                            name, de__gpu_format_name(desc->format), desc->usage,
                            de__vk_result_name(vr));
    if (ci->extent.width > fp.maxExtent.width || ci->extent.height > fp.maxExtent.height ||
        ci->extent.depth > fp.maxExtent.depth || ci->mipLevels > fp.maxMipLevels ||
        ci->arrayLayers > fp.maxArrayLayers || !(fp.sampleCounts & (VkSampleCountFlags)ci->samples))
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_texture_create(\"%s\"): %ux%ux%u, %u mips, %u layers, %u "
                            "samples of %s exceeds this Vulkan device's limits (%ux%ux%u, %u mips, "
                            "%u layers, sample mask 0x%x)",
                            name, ci->extent.width, ci->extent.height, ci->extent.depth,
                            ci->mipLevels, ci->arrayLayers, (u32)ci->samples,
                            de__gpu_format_name(desc->format), fp.maxExtent.width,
                            fp.maxExtent.height, fp.maxExtent.depth, fp.maxMipLevels,
                            fp.maxArrayLayers, (u32)fp.sampleCounts);
    return DE_OK;
}

/* ---------- Buffers ---------- */

static de_result bind_buffer(de_gpu_device *d, de__gpu_buffer *b, de__vk_buffer *vb,
                             de__gpu_heap *heap, u64 offset) {
    de__vk_device *vk = de__vk(d);
    VkResult vr = vk->fn.vkBindBufferMemory(vk->device, vb->buffer,
                                            DE__GPU_PAYLOAD(de__vk_heap, heap)->memory, offset);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "vulkan: vkBindBufferMemory(\"%s\")", b->name);
    if (b->memory != DE_MEM_GPU_ONLY) {
        if (!heap->mapped)
            return de__gpu_fail(d, DE_ERR_INTERNAL,
                                "vulkan: buffer \"%s\" is %s but its heap is not mapped", b->name,
                                b->memory == DE_MEM_UPLOAD ? "UPLOAD" : "READBACK");
        b->mapped = (u8 *)heap->mapped + offset;
    }
    if ((b->usage & BINDLESS_BUFFER_USAGE) && vk->phys.f12.bufferDeviceAddress) {
        VkBufferDeviceAddressInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        ai.buffer = vb->buffer;
        vb->address = vk->fn.vkGetBufferDeviceAddress(vk->device, &ai);
    }
    return DE_OK;
}

static de_result create_vk_buffer(de_gpu_device *d, de__gpu_buffer *b,
                                  const de_gpu_buffer_desc *desc, de__vk_buffer *vb) {
    de__vk_device *vk = de__vk(d);
    VkBufferCreateInfo bi;
    de__vk_buffer_info(vk, desc, &bi);
    VkResult vr = vk->fn.vkCreateBuffer(vk->device, &bi, NULL, &vb->buffer);
    if (vr != VK_SUCCESS) {
        vb->buffer = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkCreateBuffer(\"%s\", %llu bytes)", b->name,
                           (unsigned long long)desc->size);
    }
    return DE_OK;
}

static void release_buffer(de_gpu_device *d, de__gpu_buffer *b) {
    de__vk_device *vk = de__vk(d);
    de__vk_buffer *vb = DE__GPU_PAYLOAD(de__vk_buffer, b);
    if (vb->buffer)
        vk->fn.vkDestroyBuffer(vk->device, vb->buffer, NULL);
    vb->buffer = VK_NULL_HANDLE;
    if (vb->alloc.heap)
        de__gpu_mem_free(d, &vb->alloc);
    vb->address = 0;
    b->mapped = NULL;
}

de_result de__vk_buffer_create(de_gpu_device *d, de__gpu_buffer *b,
                               const de_gpu_buffer_desc *desc) {
    de__vk_device *vk = de__vk(d);
    de__vk_buffer *vb = DE__GPU_PAYLOAD(de__vk_buffer, b);
    DE_TRY(create_vk_buffer(d, b, desc, vb));
    VkMemoryRequirements mr;
    vk->fn.vkGetBufferMemoryRequirements(vk->device, vb->buffer, &mr);
    const u32 mask = de__vk_class_mask(vk, true, desc->memory, mr.memoryTypeBits);
    const u32 cls = de__vk_pick_class(vk, mask, desc->memory);
    de_result r = DE_OK;
    if (cls == DE__GPU_MEM_CLASS_MAX)
        r = de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                         "vulkan: no memory class can hold buffer \"%s\" (memoryTypeBits 0x%x)",
                         b->name, mr.memoryTypeBits);
    if (r == DE_OK)
        r = de__gpu_mem_alloc(d, cls, mr.size, mr.alignment, DE_MEM_RENDER, &vb->alloc);
    if (r == DE_OK)
        r = bind_buffer(d, b, vb, vb->alloc.heap, vb->alloc.offset);
    if (r != DE_OK)
        release_buffer(d, b);
    return r;
}

de_result de__vk_buffer_create_placed(de_gpu_device *d, de__gpu_buffer *b,
                                      const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                      u64 offset) {
    de__vk_buffer *vb = DE__GPU_PAYLOAD(de__vk_buffer, b);
    DE_TRY(create_vk_buffer(d, b, desc, vb));
    de_result r = bind_buffer(d, b, vb, heap, offset);
    if (r != DE_OK)
        release_buffer(d, b);
    return r;
}

void de__vk_buffer_destroy(de_gpu_device *d, de__gpu_buffer *b) {
    de__vk_bindless_clear_buffer(d, b);
    release_buffer(d, b);
}

/* ---------- Textures ---------- */

static de_result make_view(de_gpu_device *d, const de__gpu_texture *t, const de__vk_texture *vt,
                           VkImageViewType type, VkImageAspectFlags aspect, u32 mip, u32 mips,
                           u32 layers, VkImageView *out) {
    de__vk_device *vk = de__vk(d);
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = vt->image;
    vi.viewType = type;
    vi.format = vt->format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.baseMipLevel = mip;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = layers;
    VkResult vr = vk->fn.vkCreateImageView(vk->device, &vi, NULL, out);
    if (vr != VK_SUCCESS) {
        *out = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkCreateImageView(\"%s\", mip %u)", t->name, mip);
    }
    return DE_OK;
}

de_result de__vk_texture_create_views(de_gpu_device *d, de__gpu_texture *t) {
    de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
    const u32 layers = t->dim == DE_TEX_3D ? 1u : t->depth_or_layers;
    if (t->usage & DE_USAGE_SAMPLED) {
        VkImageViewType type;
        switch (t->dim) {
        case DE_TEX_3D:
            type = VK_IMAGE_VIEW_TYPE_3D;
            break;
        case DE_TEX_CUBE:
            type = layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
            break;
        case DE_TEX_2D_ARRAY:
            type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            break;
        default:
            type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            break;
        }
        /* Depth-stencil formats are sampled through the depth aspect. */
        const VkImageAspectFlags aspect =
            (vt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : vt->aspect;
        DE_TRY(make_view(d, t, vt, type, aspect, 0, t->mip_count, layers, &vt->sampled_view));
    }
    if (t->usage & DE_USAGE_STORAGE) {
        VkImageViewType type = t->dim == DE_TEX_3D                    ? VK_IMAGE_VIEW_TYPE_3D
                               : (t->dim == DE_TEX_2D && layers == 1) ? VK_IMAGE_VIEW_TYPE_2D
                                                                      : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        for (u32 m = 0; m < t->mip_count; ++m)
            DE_TRY(make_view(d, t, vt, type, vt->aspect, m, 1, layers, &vt->storage_views[m]));
    }
    return DE_OK;
}

static void release_texture(de_gpu_device *d, de__gpu_texture *t) {
    de__vk_device *vk = de__vk(d);
    de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
    if (vt->sampled_view)
        vk->fn.vkDestroyImageView(vk->device, vt->sampled_view, NULL);
    vt->sampled_view = VK_NULL_HANDLE;
    for (u32 m = 0; m < DE_GPU_MAX_MIPS; ++m) {
        if (vt->storage_views[m])
            vk->fn.vkDestroyImageView(vk->device, vt->storage_views[m], NULL);
        vt->storage_views[m] = VK_NULL_HANDLE;
    }
    /* A wrapped (swapchain) image belongs to its swapchain. */
    if (vt->image && !t->external)
        vk->fn.vkDestroyImage(vk->device, vt->image, NULL);
    vt->image = VK_NULL_HANDLE;
    if (vt->alloc.heap)
        de__gpu_mem_free(d, &vt->alloc);
}

static de_result create_vk_image(de_gpu_device *d, de__gpu_texture *t,
                                 const de_gpu_texture_desc *desc, de__vk_texture *vt) {
    de__vk_device *vk = de__vk(d);
    VkImageCreateInfo ii;
    de__vk_image_info(desc, &ii);
    DE_TRY(de__vk_image_supported(d, desc, &ii));
    vt->format = ii.format;
    vt->aspect = de__vk_format_aspect(desc->format);
    VkResult vr = vk->fn.vkCreateImage(vk->device, &ii, NULL, &vt->image);
    if (vr != VK_SUCCESS) {
        vt->image = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkCreateImage(\"%s\", %s %ux%u)", t->name,
                           de__gpu_format_name(desc->format), desc->width, ii.extent.height);
    }
    return DE_OK;
}

static de_result bind_image(de_gpu_device *d, de__gpu_texture *t, de__vk_texture *vt,
                            de__gpu_heap *heap, u64 offset) {
    de__vk_device *vk = de__vk(d);
    VkResult vr = vk->fn.vkBindImageMemory(vk->device, vt->image,
                                           DE__GPU_PAYLOAD(de__vk_heap, heap)->memory, offset);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "vulkan: vkBindImageMemory(\"%s\")", t->name);
    return de__vk_texture_create_views(d, t);
}

de_result de__vk_texture_create(de_gpu_device *d, de__gpu_texture *t,
                                const de_gpu_texture_desc *desc) {
    de__vk_device *vk = de__vk(d);
    de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
    DE_TRY(create_vk_image(d, t, desc, vt));
    VkMemoryRequirements mr;
    vk->fn.vkGetImageMemoryRequirements(vk->device, vt->image, &mr);
    const u32 mask = de__vk_class_mask(vk, false, DE_MEM_GPU_ONLY, mr.memoryTypeBits);
    const u32 cls = de__vk_pick_class(vk, mask, DE_MEM_GPU_ONLY);
    de_result r = DE_OK;
    if (cls == DE__GPU_MEM_CLASS_MAX)
        r = de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                         "vulkan: no memory class can hold texture \"%s\" (memoryTypeBits 0x%x)",
                         t->name, mr.memoryTypeBits);
    if (r == DE_OK)
        r = de__gpu_mem_alloc(d, cls, mr.size, mr.alignment, DE_MEM_RENDER, &vt->alloc);
    if (r == DE_OK)
        r = bind_image(d, t, vt, vt->alloc.heap, vt->alloc.offset);
    if (r != DE_OK)
        release_texture(d, t);
    return r;
}

de_result de__vk_texture_create_placed(de_gpu_device *d, de__gpu_texture *t,
                                       const de_gpu_texture_desc *desc, de__gpu_heap *heap,
                                       u64 offset) {
    de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
    DE_TRY(create_vk_image(d, t, desc, vt));
    de_result r = bind_image(d, t, vt, heap, offset);
    if (r != DE_OK)
        release_texture(d, t);
    return r;
}

void de__vk_texture_destroy(de_gpu_device *d, de__gpu_texture *t) {
    de__vk_bindless_clear_texture(d, t); /* its indices: the dummies again (vk_bindless.c) */
    release_texture(d, t);
}

/* ---------- Samplers ---------- */

static VkSamplerAddressMode address_mode(de_gpu_address a) {
    switch (a) {
    case DE_ADDR_CLAMP:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case DE_ADDR_MIRROR:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case DE_ADDR_BORDER:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case DE_ADDR_WRAP:
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

de_result de__vk_sampler_create(de_gpu_device *d, de__gpu_sampler *s,
                                const de_gpu_sampler_desc *desc) {
    de__vk_device *vk = de__vk(d);
    de__vk_sampler *vs = DE__GPU_PAYLOAD(de__vk_sampler, s);
    VkSamplerCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = desc->mag == DE_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = desc->min == DE_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = desc->mip == DE_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                  : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = address_mode(desc->u);
    si.addressModeV = address_mode(desc->v);
    si.addressModeW = address_mode(desc->w);
    if (desc->max_aniso > 1.0f && vk->on.sampler_anisotropy) {
        const float limit = vk->phys.props.properties.limits.maxSamplerAnisotropy;
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = desc->max_aniso < limit ? desc->max_aniso : limit;
    }
    if (desc->compare_reverse_z) {
        si.compareEnable = VK_TRUE;
        si.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
    si.minLod = 0.0f;
    si.maxLod = VK_LOD_CLAMP_NONE;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkResult vr = vk->fn.vkCreateSampler(vk->device, &si, NULL, &vs->sampler);
    if (vr != VK_SUCCESS) {
        vs->sampler = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkCreateSampler");
    }
    return DE_OK;
}

void de__vk_sampler_destroy(de_gpu_device *d, de__gpu_sampler *s) {
    de__vk_device *vk = de__vk(d);
    de__vk_sampler *vs = DE__GPU_PAYLOAD(de__vk_sampler, s);
    if (vs->sampler)
        vk->fn.vkDestroySampler(vk->device, vs->sampler, NULL);
    vs->sampler = VK_NULL_HANDLE;
}

/* ---------- Test hooks ---------- */

static bool is_vk(de_gpu_device *d) {
    return d && d->vt == &de__gpu_vulkan_vtable;
}

u64 de__gpu_vk_buffer_handle(de_gpu_device *d, de_gpu_buffer h) {
    de__gpu_buffer *b = is_vk(d) ? de__gpu_get_buffer(d, h) : NULL;
    return b ? (u64)(uintptr_t)DE__GPU_PAYLOAD(de__vk_buffer, b)->buffer : 0u;
}

u64 de__gpu_vk_texture_view(de_gpu_device *d, de_gpu_texture h, u32 mip, bool storage) {
    de__gpu_texture *t = is_vk(d) ? de__gpu_get_texture(d, h) : NULL;
    if (!t || mip >= DE_GPU_MAX_MIPS)
        return 0;
    const de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
    return (u64)(uintptr_t)(storage ? vt->storage_views[mip] : vt->sampled_view);
}

u64 de__gpu_vk_sampler_handle(de_gpu_device *d, de_gpu_sampler h) {
    de__gpu_sampler *s = is_vk(d) ? de__gpu_get_sampler(d, h) : NULL;
    return s ? (u64)(uintptr_t)DE__GPU_PAYLOAD(de__vk_sampler, s)->sampler : 0u;
}
