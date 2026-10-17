/* vk_barriers.c — de_gpu_access to synchronization2 barriers (DENG-284; SPEC §12.1).
 *
 * The render graph issues every barrier (no implicit state tracking). One de_gpu_cmd_barriers
 * call becomes one vkCmdPipelineBarrier2 with an image barrier per texture and a buffer barrier
 * per buffer. The mapping is table-driven and used by nothing else (D3D12 and Metal have their
 * own tables):
 *
 *   access         stages                        image layout
 *   SHADER_READ    shaders (pre-raster, fragment, SHADER_READ_ONLY_OPTIMAL (bindless binding 0)
 *                  compute, ray tracing)
 *   SHADER_WRITE   shaders                       GENERAL (bindless binding 1)
 *   RENDER_TARGET  COLOR_ATTACHMENT_OUTPUT       COLOR_ATTACHMENT_OPTIMAL
 *   DEPTH_READ     early + late fragment tests   DEPTH_READ_ONLY_OPTIMAL
 *   DEPTH_WRITE    early + late fragment tests   DEPTH_ATTACHMENT_OPTIMAL
 *   INDIRECT       DRAW_INDIRECT                 (buffers)
 *   COPY_SRC/DST   ALL_TRANSFER                  TRANSFER_SRC/DST_OPTIMAL
 *   ACCEL_*        acceleration-structure build (and shaders, for reads)
 *   PRESENT        none                          PRESENT_SRC_KHR
 *
 * Depth layouts become the combined DEPTH_STENCIL_* layouts for formats with stencil, or when the
 * device lacks separateDepthStencilLayouts. Stages the recording queue's family does not support
 * are dropped with their accesses (a copy queue barrier only has transfer stages). A `before` of
 * NONE (contents discarded) or PRESENT on a texture waits for ALL_COMMANDS: the barrier then
 * chains after the swapchain acquire wait, whose stage is ALL_COMMANDS (vk_sync.c).
 */
#include "gpu/vulkan/vk_internal.h"

#include <string.h>

/* Stage set placeholders, expanded per device by stages_of(). */
enum {
    ST_SHADERS = 1u << 0, /* every shader stage that can read or write a resource */
    ST_FIXED = 1u << 1,   /* the row's own fixed stages only */
    ST_BUFFER = 1u << 2,  /* the row applies to buffers only */
};

typedef struct access_row {
    u32 bit;
    u32 stage_set;
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 image_access;
    VkAccessFlags2 buffer_access;
    VkImageLayout layout;
} access_row;

#define S2(x) VK_PIPELINE_STAGE_2_##x
#define A2(x) VK_ACCESS_2_##x
#define L(x)  VK_IMAGE_LAYOUT_##x

VkImageLayout de__vk_depth_write_layout(bool separate_depth_layouts, bool stencil) {
    return separate_depth_layouts && !stencil ? L(DEPTH_ATTACHMENT_OPTIMAL)
                                              : L(DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

static VkImageLayout depth_read_layout(const de__vk_barrier_ctx *ctx, bool stencil) {
    return ctx->separate_depth_layouts && !stencil ? L(DEPTH_READ_ONLY_OPTIMAL)
                                                   : L(DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}

static VkPipelineStageFlags2 stages_of(const de__vk_barrier_ctx *ctx, const access_row *r) {
    VkPipelineStageFlags2 s = r->stages;
    if (r->stage_set & ST_SHADERS) {
        s |= S2(PRE_RASTERIZATION_SHADERS_BIT) | S2(FRAGMENT_SHADER_BIT) | S2(COMPUTE_SHADER_BIT);
        if (ctx->rt_pipeline)
            s |= S2(RAY_TRACING_SHADER_BIT_KHR);
    }
    return s;
}

de__vk_access_info de__vk_access_info_of(const de__vk_barrier_ctx *ctx, u32 access, bool image,
                                         bool depth, bool stencil, bool before) {
    de__vk_access_info out = {0, 0, L(UNDEFINED)};
    if (access == DE_ACCESS_NONE || access == DE_ACCESS_PRESENT) {
        /* Nothing to wait for or make visible. A texture's prior use is unknown (discarded) or a
         * presentation: wait for every earlier command, which includes the acquire semaphore
         * wait, so the layout transition cannot start before the image is free. */
        if (before && image)
            out.stages = S2(ALL_COMMANDS_BIT);
        if (image)
            out.layout = access == DE_ACCESS_PRESENT ? L(PRESENT_SRC_KHR) : L(UNDEFINED);
        return out;
    }
    /* An automatic array: the C Vulkan headers declare the VK_*_2_* flag values as static const
     * variables, which MSVC and GCC do not accept in a static initializer. */
    const access_row k_rows[] = {
        {DE_ACCESS_SHADER_READ, ST_SHADERS, 0,
         A2(SHADER_SAMPLED_READ_BIT) | A2(SHADER_STORAGE_READ_BIT),
         A2(SHADER_STORAGE_READ_BIT) | A2(UNIFORM_READ_BIT), L(SHADER_READ_ONLY_OPTIMAL)},
        /* Buffers read by shaders may also be vertex or index input. */
        {DE_ACCESS_SHADER_READ, ST_FIXED | ST_BUFFER,
         S2(VERTEX_ATTRIBUTE_INPUT_BIT) | S2(INDEX_INPUT_BIT), 0,
         A2(VERTEX_ATTRIBUTE_READ_BIT) | A2(INDEX_READ_BIT), L(SHADER_READ_ONLY_OPTIMAL)},
        {DE_ACCESS_SHADER_WRITE, ST_SHADERS, 0,
         A2(SHADER_STORAGE_READ_BIT) | A2(SHADER_STORAGE_WRITE_BIT),
         A2(SHADER_STORAGE_READ_BIT) | A2(SHADER_STORAGE_WRITE_BIT), L(GENERAL)},
        {DE_ACCESS_RENDER_TARGET, ST_FIXED, S2(COLOR_ATTACHMENT_OUTPUT_BIT),
         A2(COLOR_ATTACHMENT_READ_BIT) | A2(COLOR_ATTACHMENT_WRITE_BIT), 0,
         L(COLOR_ATTACHMENT_OPTIMAL)},
        {DE_ACCESS_DEPTH_READ, ST_FIXED, S2(EARLY_FRAGMENT_TESTS_BIT) | S2(LATE_FRAGMENT_TESTS_BIT),
         A2(DEPTH_STENCIL_ATTACHMENT_READ_BIT), 0, L(DEPTH_READ_ONLY_OPTIMAL)},
        {DE_ACCESS_DEPTH_WRITE, ST_FIXED,
         S2(EARLY_FRAGMENT_TESTS_BIT) | S2(LATE_FRAGMENT_TESTS_BIT),
         A2(DEPTH_STENCIL_ATTACHMENT_READ_BIT) | A2(DEPTH_STENCIL_ATTACHMENT_WRITE_BIT), 0,
         L(DEPTH_ATTACHMENT_OPTIMAL)},
        {DE_ACCESS_INDIRECT, ST_FIXED, S2(DRAW_INDIRECT_BIT), 0, A2(INDIRECT_COMMAND_READ_BIT),
         L(GENERAL)},
        {DE_ACCESS_COPY_SRC, ST_FIXED, S2(ALL_TRANSFER_BIT), A2(TRANSFER_READ_BIT),
         A2(TRANSFER_READ_BIT), L(TRANSFER_SRC_OPTIMAL)},
        {DE_ACCESS_COPY_DST, ST_FIXED, S2(ALL_TRANSFER_BIT), A2(TRANSFER_WRITE_BIT),
         A2(TRANSFER_WRITE_BIT), L(TRANSFER_DST_OPTIMAL)},
        {DE_ACCESS_ACCEL_READ, ST_SHADERS, S2(ACCELERATION_STRUCTURE_BUILD_BIT_KHR), 0,
         A2(ACCELERATION_STRUCTURE_READ_BIT_KHR), L(GENERAL)},
        {DE_ACCESS_ACCEL_WRITE, ST_FIXED, S2(ACCELERATION_STRUCTURE_BUILD_BIT_KHR), 0,
         A2(ACCELERATION_STRUCTURE_WRITE_BIT_KHR), L(GENERAL)},
        {DE_ACCESS_PRESENT, ST_FIXED, 0, 0, 0, L(PRESENT_SRC_KHR)},
    };
    VkImageLayout layout = L(UNDEFINED);
    bool mixed = false;
    for (u32 i = 0; i < sizeof k_rows / sizeof k_rows[0]; ++i) {
        const access_row *r = &k_rows[i];
        if (!(access & r->bit))
            continue;
        if ((image && (r->stage_set & ST_BUFFER)) ||
            ((r->bit & (DE_ACCESS_ACCEL_READ | DE_ACCESS_ACCEL_WRITE)) && !ctx->accel))
            continue; /* vertex/index input is for buffers; no AS stages without ray tracing */
        /* Stages this queue cannot run are dropped with their accesses. */
        const VkPipelineStageFlags2 st = stages_of(ctx, r) & ctx->queue_stages;
        if (st != 0) {
            out.stages |= st;
            out.access |= image ? r->image_access : r->buffer_access;
        }
        if (image) {
            VkImageLayout l = r->layout;
            if (r->bit == DE_ACCESS_DEPTH_WRITE)
                l = de__vk_depth_write_layout(ctx->separate_depth_layouts, stencil);
            else if (r->bit == DE_ACCESS_DEPTH_READ)
                l = depth_read_layout(ctx, stencil);
            if (layout == L(UNDEFINED))
                layout = l;
            else if (layout != l)
                mixed = true;
        }
    }
    if (image) {
        if (!mixed) {
            out.layout = layout;
        } else if ((access & ~(u32)(DE_ACCESS_SHADER_READ | DE_ACCESS_DEPTH_READ)) == 0 && depth) {
            out.layout = depth_read_layout(ctx, stencil); /* sampled while bound read-only */
        } else {
            out.layout = L(GENERAL);
        }
    }
    return out;
}

void de__vk_translate_barrier(const de__vk_barrier_ctx *ctx, const de_gpu_barrier *b,
                              const de__vk_barrier_texture *tex, VkBuffer buf_handle,
                              VkImageMemoryBarrier2 *img, VkBufferMemoryBarrier2 *buf) {
    const bool image = tex != NULL;
    const bool depth = image && tex->depth, stencil = image && tex->stencil;
    de__vk_access_info src = de__vk_access_info_of(ctx, b->before, image, depth, stencil, true);
    de__vk_access_info dst = de__vk_access_info_of(ctx, b->after, image, depth, stencil, false);
    if (image && b->after == DE_ACCESS_NONE)
        dst.layout = src.layout; /* no later use: keep the layout, transition nothing */

    /* Queue-family ownership transfer: the recording queue performs its half. */
    u32 src_family = VK_QUEUE_FAMILY_IGNORED, dst_family = VK_QUEUE_FAMILY_IGNORED;
    const u32 fs = ctx->family[(u32)b->src_queue < DE_QUEUE_COUNT_ ? b->src_queue : 0];
    const u32 fd = ctx->family[(u32)b->dst_queue < DE_QUEUE_COUNT_ ? b->dst_queue : 0];
    if (b->src_queue != b->dst_queue && fs != fd) {
        src_family = fs;
        dst_family = fd;
        if (ctx->cmd_family == fs) { /* release: the destination half is the acquire's */
            dst.stages = 0;
            dst.access = 0;
        } else if (ctx->cmd_family == fd) { /* acquire: the source half was the release's */
            src.stages = 0;
            src.access = 0;
        }
    } else if (image && b->src_queue != b->dst_queue && ctx->cmd_queue == b->dst_queue) {
        /* DENG-105: both queues in one family, so no ownership transfer: the barrier recorded on
         * src_queue already changed the layout, and the image is in the new one. The dst_queue
         * half keeps it (a memory dependency only); a second transition would start from a
         * layout the image is no longer in. */
        src.layout = dst.layout;
    }

    if (image) {
        memset(img, 0, sizeof *img);
        img->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        img->srcStageMask = src.stages;
        img->srcAccessMask = src.access;
        img->dstStageMask = dst.stages;
        img->dstAccessMask = dst.access;
        img->oldLayout = src.layout;
        img->newLayout = dst.layout;
        img->srcQueueFamilyIndex = src_family;
        img->dstQueueFamilyIndex = dst_family;
        img->image = tex->image;
        img->subresourceRange.aspectMask = tex->aspect;
        const u32 mip_first = b->mip_first < tex->mip_count ? b->mip_first : 0u;
        img->subresourceRange.baseMipLevel = mip_first;
        img->subresourceRange.levelCount = b->mip_count ? b->mip_count : tex->mip_count - mip_first;
        const u32 layer_first =
            tex->layers > 1 && b->layer_first < tex->layers ? b->layer_first : 0u;
        img->subresourceRange.baseArrayLayer = layer_first;
        img->subresourceRange.layerCount =
            tex->layers > 1 && b->layer_count ? b->layer_count : tex->layers - layer_first;
    } else {
        memset(buf, 0, sizeof *buf);
        buf->sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        buf->srcStageMask = src.stages;
        buf->srcAccessMask = src.access;
        buf->dstStageMask = dst.stages;
        buf->dstAccessMask = dst.access;
        buf->srcQueueFamilyIndex = src_family;
        buf->dstQueueFamilyIndex = dst_family;
        buf->buffer = buf_handle;
        buf->offset = 0;
        buf->size = VK_WHOLE_SIZE;
    }
}

/* ---------- Device context ---------- */

void de__vk_barrier_ctx_init(de_gpu_device *d, de_gpu_queue q, de__vk_barrier_ctx *ctx) {
    de__vk_device *vk = de__vk(d);
    memset(ctx, 0, sizeof *ctx);
    VkQueueFamilyProperties props[32];
    u32 n = 32;
    vk->fn.vkGetPhysicalDeviceQueueFamilyProperties(vk->phys.handle, &n, props);
    const u32 fam = vk->queues[q].family;
    const VkQueueFlags flags = fam < n ? props[fam].queueFlags : 0;
    VkPipelineStageFlags2 st = S2(ALL_TRANSFER_BIT) | S2(ALL_COMMANDS_BIT) | S2(HOST_BIT);
    if (flags & VK_QUEUE_GRAPHICS_BIT)
        st = ~(VkPipelineStageFlags2)0;
    else if (flags & VK_QUEUE_COMPUTE_BIT)
        st |= S2(COMPUTE_SHADER_BIT) | S2(DRAW_INDIRECT_BIT) |
              S2(ACCELERATION_STRUCTURE_BUILD_BIT_KHR) | S2(RAY_TRACING_SHADER_BIT_KHR);
    ctx->queue_stages = st;
    ctx->separate_depth_layouts = vk->phys.f12.separateDepthStencilLayouts != VK_FALSE;
    ctx->accel = vk->on.accel;
    ctx->rt_pipeline = vk->on.rt_pipeline;
    ctx->mesh = vk->on.mesh_shader;
    ctx->cmd_family = fam;
    ctx->cmd_queue = q;
    for (u32 i = 0; i < DE_QUEUE_COUNT_; ++i)
        ctx->family[i] = vk->queues[i].family;
    /* Ray tracing and acceleration-structure stages only exist with their extensions. */
    if (!ctx->rt_pipeline)
        ctx->queue_stages &= ~S2(RAY_TRACING_SHADER_BIT_KHR);
    if (!ctx->accel)
        ctx->queue_stages &= ~S2(ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    if (!ctx->mesh)
        ctx->queue_stages &= ~(S2(TASK_SHADER_BIT_EXT) | S2(MESH_SHADER_BIT_EXT));
}

/* ---------- Recording ---------- */

#define BATCH 32u

void de__vk_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count) {
    de_gpu_device *d = c->device;
    de__vk_device *vk = de__vk(d);
    de__vk_cmd *vc = de__vk_cmd_of(c);
    VkImageMemoryBarrier2 imgs[BATCH];
    VkBufferMemoryBarrier2 bufs[BATCH];
    /* One vkCmdPipelineBarrier2 per call; calls with more than BATCH barriers of a kind (rare)
     * are split, which only orders them more strictly. */
    u32 i = 0;
    while (i < count) {
        u32 ni = 0, nb = 0;
        for (; i < count && ni < BATCH && nb < BATCH; ++i) {
            const de_gpu_barrier *x = &b[i];
            if (x->texture.id) {
                de__gpu_texture *t = de__gpu_get_texture(d, x->texture);
                if (!t)
                    continue;
                const de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
                de__vk_barrier_texture bt;
                bt.image = vt->image;
                bt.aspect = vt->aspect;
                bt.depth = (vt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
                bt.stencil = (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
                bt.mip_count = t->mip_count;
                bt.layers = t->dim == DE_TEX_3D ? 1u : t->depth_or_layers;
                de__vk_translate_barrier(&vc->barrier_ctx, x, &bt, VK_NULL_HANDLE, &imgs[ni++],
                                         NULL);
            } else if (x->buffer.id) {
                de__gpu_buffer *bb = de__gpu_get_buffer(d, x->buffer);
                if (!bb)
                    continue;
                de__vk_translate_barrier(&vc->barrier_ctx, x, NULL,
                                         DE__GPU_PAYLOAD(de__vk_buffer, bb)->buffer, NULL,
                                         &bufs[nb++]);
            }
            else if (x->accel.id) {
                de__gpu_accel *a = de__gpu_get_accel(d, x->accel);
                if (!a)
                    continue;
                de__vk_accel *va = DE__GPU_PAYLOAD(de__vk_accel, a);
                /* The backing buffer owns the AS memory, including queue-family ownership. */
                de__vk_translate_barrier(&vc->barrier_ctx, x, NULL,
                    DE__GPU_PAYLOAD(de__vk_buffer, va->storage)->buffer, NULL, &bufs[nb++]);
            }
        }
        if (ni == 0 && nb == 0)
            continue;
        VkDependencyInfo di;
        memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = ni;
        di.pImageMemoryBarriers = imgs;
        di.bufferMemoryBarrierCount = nb;
        di.pBufferMemoryBarriers = bufs;
        vk->fn.vkCmdPipelineBarrier2(vc->cb, &di);
    }
}
