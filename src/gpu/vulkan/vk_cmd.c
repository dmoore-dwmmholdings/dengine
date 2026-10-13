/* vk_cmd.c — Vulkan command lists: recording, dynamic rendering, viewports, copies (DENG-284;
 * SPEC §12.1).
 *
 * Pools: every front-end command-list object owns one VK_COMMAND_POOL_CREATE_TRANSIENT pool for
 * its queue's family and one primary command buffer from it. The front-end recycles a list (per
 * queue) only after the internal timeline value of its submission has completed, and a list is
 * recorded by one thread at a time, so the pool needs no lock: cmd_begin resets it and begins a
 * one-time-submit buffer; queue_submit (vk_sync.c) ends it. Pools hang off the command lists,
 * which hang off the device: no per-thread globals.
 *
 * Rendering: dynamic rendering (Vulkan 1.3). begin_render makes a 2D view per attachment (the
 * attachment's mip and layer) and keeps it on the list until the list is reused; the render area
 * is the first attachment's mip size, with a full viewport and scissor set. Color attachments are
 * in COLOR_ATTACHMENT_OPTIMAL and depth in the DEPTH_WRITE layout of vk_barriers.c: the caller's
 * barriers put them there. Clear: LOAD_OP_CLEAR with the attachment's values, else LOAD.
 *
 * Viewport: clip space is D3D-style on every backend (y up, Decision: Shader resource
 * conventions), so the Vulkan viewport has a negative height: origin at the bottom edge of the
 * de_gpu rectangle (x, y, w, h; y down from the top-left).
 *
 * Copies: vkCmdCopyBuffer; buffer <-> texture copies with rows `row_pitch` bytes apart, in texels
 * (or 4x4 blocks for BC formats): bufferRowLength = row_pitch / block bytes * block width. The
 * texture is in TRANSFER_DST_OPTIMAL (to) or TRANSFER_SRC_OPTIMAL (from).
 */
#include "gpu/vulkan/vk_internal.h"

#include <string.h>

/* ---------- Command-list lifetime ---------- */

static void drop_views(de__vk_device *vk, de__vk_cmd *vc) {
    for (u32 i = 0; i < vc->view_count; ++i)
        vk->fn.vkDestroyImageView(vk->device, vc->views[i], NULL);
    vc->view_count = 0;
}

de_result de__vk_cmd_init(de_gpu_device *d, de_gpu_cmd *c) {
    de__vk_device *vk = de__vk(d);
    de__vk_cmd *vc = de__vk_cmd_of(c);
    de__vk_barrier_ctx_init(d, c->queue, &vc->barrier_ctx);
    VkCommandPoolCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = vk->queues[c->queue].family;
    VkResult vr = vk->fn.vkCreateCommandPool(vk->device, &pi, NULL, &vc->pool);
    if (vr != VK_SUCCESS) {
        vc->pool = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkCreateCommandPool (queue family %u)",
                           pi.queueFamilyIndex);
    }
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = vc->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vr = vk->fn.vkAllocateCommandBuffers(vk->device, &ai, &vc->cb);
    if (vr != VK_SUCCESS) {
        vk->fn.vkDestroyCommandPool(vk->device, vc->pool, NULL);
        vc->pool = VK_NULL_HANDLE;
        vc->cb = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkAllocateCommandBuffers");
    }
    return DE_OK;
}

void de__vk_cmd_fini(de_gpu_device *d, de_gpu_cmd *c) {
    de__vk_device *vk = de__vk(d);
    de__vk_cmd *vc = de__vk_cmd_of(c);
    drop_views(vk, vc);
    if (vc->views)
        DE_FREE(d->alloc, vc->views, (size_t)vc->view_cap * sizeof(VkImageView));
    vc->views = NULL;
    vc->view_cap = 0;
    /* Destroying the pool frees its command buffer. */
    if (vc->pool)
        vk->fn.vkDestroyCommandPool(vk->device, vc->pool, NULL);
    vc->pool = VK_NULL_HANDLE;
    vc->cb = VK_NULL_HANDLE;
}

de_result de__vk_cmd_begin(de_gpu_cmd *c, const char *name) {
    de_gpu_device *d = c->device;
    de__vk_device *vk = de__vk(d);
    de__vk_cmd *vc = de__vk_cmd_of(c);
    /* The previous submission of this list has completed (front-end rule): its views and its
     * command memory can go. */
    drop_views(vk, vc);
    vc->set_bound[0] = vc->set_bound[1] = false; /* DENG-323: rebind the bindless set */
    VkResult vr = vk->fn.vkResetCommandPool(vk->device, vc->pool, 0);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "vulkan: vkResetCommandPool(\"%s\")", c->name);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = vk->fn.vkBeginCommandBuffer(vc->cb, &bi);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "vulkan: vkBeginCommandBuffer(\"%s\")", c->name);
    if (name && name[0] && vk->fn.vkSetDebugUtilsObjectNameEXT && vk->on.debug_utils) {
        VkDebugUtilsObjectNameInfoEXT ni;
        memset(&ni, 0, sizeof ni);
        ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        ni.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
        ni.objectHandle = (u64)(uintptr_t)vc->cb;
        ni.pObjectName = c->name;
        vk->fn.vkSetDebugUtilsObjectNameEXT(vk->device, &ni);
    }
    return DE_OK;
}

/* ---------- Rendering ---------- */

static u32 mip_dim(u32 v, u32 mip) {
    v >>= mip;
    return v ? v : 1u;
}

/* A 2D view of one mip and layer of `t`, kept on the list until it is reused. */
static VkImageView attachment_view(de_gpu_cmd *c, de__gpu_texture *t, u32 mip, u32 layer) {
    de_gpu_device *d = c->device;
    de__vk_device *vk = de__vk(d);
    de__vk_cmd *vc = de__vk_cmd_of(c);
    if (t->dim == DE_TEX_3D) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                     "de_gpu_cmd_begin_render(\"%s\"): \"%s\" is a 3D texture; Vulkan renders to "
                     "3D textures only through 2D-array-compatible images",
                     c->name, t->name);
        return VK_NULL_HANDLE;
    }
    if (vc->view_count == vc->view_cap) {
        const u32 cap = vc->view_cap ? vc->view_cap * 2u : 8u;
        VkImageView *v = (VkImageView *)DE_ALLOC(d->alloc, (size_t)cap * sizeof(VkImageView),
                                                 DE_ALIGNOF(VkImageView), DE_MEM_RENDER);
        if (!v) {
            DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_cmd_begin_render: attachment views");
            return VK_NULL_HANDLE;
        }
        if (vc->views) {
            memcpy(v, vc->views, (size_t)vc->view_count * sizeof(VkImageView));
            DE_FREE(d->alloc, vc->views, (size_t)vc->view_cap * sizeof(VkImageView));
        }
        vc->views = v;
        vc->view_cap = cap;
    }
    const de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = vt->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = vt->format;
    vi.subresourceRange.aspectMask = vt->aspect;
    vi.subresourceRange.baseMipLevel = mip;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = layer;
    vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    VkResult vr = vk->fn.vkCreateImageView(vk->device, &vi, NULL, &view);
    if (vr != VK_SUCCESS) {
        de__vk_fail(d, vr, "de_gpu_cmd_begin_render(\"%s\"): attachment view of \"%s\"", c->name,
                    t->name);
        return VK_NULL_HANDLE;
    }
    vc->views[vc->view_count++] = view;
    return view;
}

static void set_viewport(de__vk_device *vk, de__vk_cmd *vc, f32 x, f32 y, f32 w, f32 h) {
    /* Negative height: y up in clip space, as on D3D12 and Metal. */
    VkViewport vp;
    vp.x = x;
    vp.y = y + h;
    vp.width = w;
    vp.height = -h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vk->fn.vkCmdSetViewport(vc->cb, 0, 1, &vp);
}

void de__vk_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                             const de_gpu_attachment *depth) {
    de_gpu_device *d = c->device;
    de__vk_device *vk = de__vk(d);
    de__vk_cmd *vc = de__vk_cmd_of(c);
    VkRenderingAttachmentInfo ca[DE_GPU_MAX_COLOR_ATTACHMENTS];
    VkRenderingAttachmentInfo da;
    VkExtent2D extent = {0, 0};
    bool ok = true;
    if (color_count > DE_GPU_MAX_COLOR_ATTACHMENTS)
        color_count = DE_GPU_MAX_COLOR_ATTACHMENTS;
    for (u32 i = 0; i < color_count && ok; ++i) {
        de__gpu_texture *t = de__gpu_get_texture(d, colors[i].texture);
        VkImageView view = t ? attachment_view(c, t, colors[i].mip, colors[i].layer) : 0;
        ok = view != VK_NULL_HANDLE;
        if (!ok)
            break;
        if (extent.width == 0) {
            extent.width = mip_dim(t->width, colors[i].mip);
            extent.height = mip_dim(t->height, colors[i].mip);
        }
        memset(&ca[i], 0, sizeof ca[i]);
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = colors[i].clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp =
            colors[i].store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        for (u32 k = 0; k < 4; ++k)
            ca[i].clearValue.color.float32[k] = colors[i].clear_color[k];
    }
    bool stencil = false;
    if (ok && depth) {
        de__gpu_texture *t = de__gpu_get_texture(d, depth->texture);
        VkImageView view = t ? attachment_view(c, t, depth->mip, depth->layer) : 0;
        ok = view != VK_NULL_HANDLE;
        if (ok) {
            stencil =
                (DE__GPU_PAYLOAD(de__vk_texture, t)->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            if (extent.width == 0) {
                extent.width = mip_dim(t->width, depth->mip);
                extent.height = mip_dim(t->height, depth->mip);
            }
            memset(&da, 0, sizeof da);
            da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            da.imageView = view;
            da.imageLayout =
                de__vk_depth_write_layout(vc->barrier_ctx.separate_depth_layouts, stencil);
            da.loadOp = depth->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            da.storeOp =
                depth->store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            da.clearValue.depthStencil.depth = depth->clear_depth;
            da.clearValue.depthStencil.stencil = 0;
        }
    }
    /* Record an empty pass on failure (the error is set): end_render stays balanced. */
    VkRenderingInfo ri;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = extent;
    ri.layerCount = 1;
    if (ok) {
        ri.colorAttachmentCount = color_count;
        ri.pColorAttachments = color_count ? ca : NULL;
        ri.pDepthAttachment = depth ? &da : NULL;
        ri.pStencilAttachment = depth && stencil ? &da : NULL;
    }
    vc->render_extent = extent;
    vk->fn.vkCmdBeginRendering(vc->cb, &ri);
    if (extent.width) {
        set_viewport(vk, vc, 0.0f, 0.0f, (f32)extent.width, (f32)extent.height);
        VkRect2D sc;
        sc.offset.x = 0;
        sc.offset.y = 0;
        sc.extent = extent;
        vk->fn.vkCmdSetScissor(vc->cb, 0, 1, &sc);
    }
}

void de__vk_cmd_end_render(de_gpu_cmd *c) {
    de__vk_device *vk = de__vk(c->device);
    vk->fn.vkCmdEndRendering(de__vk_cmd_of(c)->cb);
}

void de__vk_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h) {
    set_viewport(de__vk(c->device), de__vk_cmd_of(c), x, y, w, h);
}

/* ---------- Copies ---------- */

void de__vk_cmd_copy_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off, de__gpu_buffer *src,
                            u64 src_off, u64 bytes) {
    de__vk_device *vk = de__vk(c->device);
    VkBufferCopy r;
    r.srcOffset = src_off;
    r.dstOffset = dst_off;
    r.size = bytes;
    vk->fn.vkCmdCopyBuffer(de__vk_cmd_of(c)->cb, DE__GPU_PAYLOAD(de__vk_buffer, src)->buffer,
                           DE__GPU_PAYLOAD(de__vk_buffer, dst)->buffer, 1, &r);
}

void de__vk_buffer_image_copy(const de__gpu_texture *t, VkImageAspectFlags aspect, u32 mip,
                              u32 layer, u64 offset, u32 row_pitch, VkBufferImageCopy *out) {
    const bool bc = de__gpu_format_is_compressed(t->format);
    const u32 block = de__gpu_format_block_bytes(t->format);
    memset(out, 0, sizeof *out);
    out->bufferOffset = offset;
    /* In texels: a BC row of blocks is 4 texel rows. */
    out->bufferRowLength = block ? row_pitch / block * (bc ? 4u : 1u) : 0u;
    out->bufferImageHeight = 0; /* tightly packed rows of the mip */
    /* Copies touch one aspect: the depth of a depth-stencil texture. */
    out->imageSubresource.aspectMask =
        (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : aspect;
    out->imageSubresource.mipLevel = mip;
    out->imageSubresource.baseArrayLayer = t->dim == DE_TEX_3D ? 0u : layer;
    out->imageSubresource.layerCount = 1;
    out->imageExtent.width = mip_dim(t->width, mip);
    out->imageExtent.height = mip_dim(t->height, mip);
    out->imageExtent.depth = t->dim == DE_TEX_3D ? mip_dim(t->depth_or_layers, mip) : 1u;
}

void de__vk_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de__gpu_texture *dst, u32 mip, u32 layer,
                                       de__gpu_buffer *src, u64 src_off, u32 row_pitch) {
    de__vk_device *vk = de__vk(c->device);
    const de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, dst);
    VkBufferImageCopy r;
    de__vk_buffer_image_copy(dst, vt->aspect, mip, layer, src_off, row_pitch, &r);
    vk->fn.vkCmdCopyBufferToImage(de__vk_cmd_of(c)->cb, DE__GPU_PAYLOAD(de__vk_buffer, src)->buffer,
                                  vt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

void de__vk_cmd_copy_texture_to_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off,
                                       u32 row_pitch, de__gpu_texture *src, u32 mip, u32 layer) {
    de__vk_device *vk = de__vk(c->device);
    const de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, src);
    VkBufferImageCopy r;
    de__vk_buffer_image_copy(src, vt->aspect, mip, layer, dst_off, row_pitch, &r);
    vk->fn.vkCmdCopyImageToBuffer(de__vk_cmd_of(c)->cb, vt->image,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  DE__GPU_PAYLOAD(de__vk_buffer, dst)->buffer, 1, &r);
}
