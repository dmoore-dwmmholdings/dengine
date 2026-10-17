/* mtl_barriers.m — DEngine barriers on Metal (DENG-288, SPEC §12.1).
 *
 * Every Metal resource of the engine sits in an untracked heap (DENG-287), so Metal orders
 * nothing: without a barrier, two encoders of one command buffer may run at the same time, and
 * so may the dispatches of one compute encoder. A de_gpu barrier becomes:
 *   - between encoders: MTLFences. A barrier ends an "epoch" of encoders. Every encoder updates
 *     its own fence when it ends; every encoder opened after the barrier waits for all fences of
 *     the epoch before it. An epoch's encoders waited for the epoch before theirs, so one wait set
 *     covers everything earlier in the command buffer. A render encoder waits before the stages
 *     the barriers' `after` accesses use (the table below) and updates after the stages their
 *     `before` accesses use, and always after the fragment stage.
 *   - inside an open compute encoder (S10 dispatches): memoryBarrierWithScope: for buffers and
 *     textures; the encoder carries on into the next epoch. A render encoder never has one: the
 *     front-end refuses barriers inside a render pass, so the render encoder is ended with its
 *     fence instead (memoryBarrierWithScope:afterStages:beforeStages: has no use).
 *   - between command buffers of one queue: the queue fence. A command buffer's first epoch waits
 *     for it, and the command buffer ends with a small blit encoder (the tail) that waits for its
 *     last epoch and updates it. A command-buffer boundary is thus a full barrier on its queue;
 *     barriers recorded at the end of one list (RENDER_TARGET -> PRESENT, ownership releases)
 *     cover the next list too.
 *   - between queues and with the CPU: MTLSharedEvents at submit (mtl_sync.m). Queue ownership
 *     transfers need nothing more: Metal has no queue families.
 * Metal resolves a fence wait against the latest update committed before it, so the order of
 * commits (the front-end's per-queue submit lock) is the order of the chain.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

_Static_assert(DE__MTL_STAGE_VERTEX == (u32)MTLRenderStageVertex, "MTLRenderStageVertex");
_Static_assert(DE__MTL_STAGE_FRAGMENT == (u32)MTLRenderStageFragment, "MTLRenderStageFragment");
_Static_assert(DE__MTL_STAGE_OBJECT == (u32)MTLRenderStageObject, "MTLRenderStageObject");
_Static_assert(DE__MTL_STAGE_MESH == (u32)MTLRenderStageMesh, "MTLRenderStageMesh");

#define SHADER_STAGES DE__MTL_STAGES_ALL

/* de_gpu_access -> the render stages that touch the resource. Copies and presentation happen
 * outside render encoders (0). Indirect arguments are read when a draw starts, before its first
 * stage. Attachments are loaded, written and stored by the fragment stage on Apple GPUs. */
static const struct {
    u32 access, stages;
} k_access_stages[] = {
    {DE_ACCESS_SHADER_READ, SHADER_STAGES},
    {DE_ACCESS_SHADER_WRITE, SHADER_STAGES},
    {DE_ACCESS_RENDER_TARGET, DE__MTL_STAGE_FRAGMENT},
    {DE_ACCESS_DEPTH_READ, DE__MTL_STAGE_FRAGMENT},
    {DE_ACCESS_DEPTH_WRITE, DE__MTL_STAGE_FRAGMENT},
    {DE_ACCESS_INDIRECT, DE__MTL_STAGE_VERTEX | DE__MTL_STAGE_OBJECT | DE__MTL_STAGE_MESH},
    {DE_ACCESS_COPY_SRC, 0},
    {DE_ACCESS_COPY_DST, 0},
    {DE_ACCESS_ACCEL_READ, SHADER_STAGES},
    {DE_ACCESS_ACCEL_WRITE, 0},
    {DE_ACCESS_PRESENT, 0},
};

u32 de__mtl_access_stages(u32 access) {
    u32 stages = 0;
    for (u32 i = 0; i < sizeof k_access_stages / sizeof k_access_stages[0]; ++i)
        if (access & k_access_stages[i].access)
            stages |= k_access_stages[i].stages;
    return stages;
}

void de__mtl_barrier_stages(const de_gpu_barrier *b, u32 count, u32 all, u32 *before, u32 *after) {
    u32 bs = 0, as = 0;
    for (u32 i = 0; i < count; ++i) {
        bs |= de__mtl_access_stages(b[i].before);
        as |= de__mtl_access_stages(b[i].after);
    }
    *before = bs & all;
    *after = as & all;
}

/* ---------- Fences of a command list ---------- */

void *de__mtl_cmd_fence(de_gpu_cmd *c, u32 index) {
    mtl_cmd *mc = (mtl_cmd *)de__gpu_cmd_payload(c);
    NSMutableArray *pool = (__bridge NSMutableArray *)mc->fences;
    while (pool.count <= index) {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(c->device)->device;
        id<MTLFence> f = [dev newFence];
        if (f == nil)
            return NULL;
        f.label = [NSString stringWithFormat:@"de cmd fence %u", (u32)pool.count];
        [pool addObject:f];
    }
    return (__bridge void *)pool[index];
}

static id<MTLFence> fence_at(de_gpu_cmd *c, u32 index) {
    return (__bridge id<MTLFence>)de__mtl_cmd_fence(c, index);
}

/* Makes `encoder` wait for `fence` (render encoders: before `stages`). */
static void wait_fence(id<MTLCommandEncoder> encoder, de__mtl_encoder_kind kind, id<MTLFence> fence,
                       u32 stages) {
    if (fence == nil)
        return;
    switch (kind) {
    case DE__MTL_ENC_RENDER:
        [(id<MTLRenderCommandEncoder>)encoder waitForFence:fence
                                              beforeStages:(MTLRenderStages)stages];
        break;
    case DE__MTL_ENC_COMPUTE:
        [(id<MTLComputeCommandEncoder>)encoder waitForFence:fence];
        break;
    case DE__MTL_ENC_ACCEL:
        [(id<MTLAccelerationStructureCommandEncoder>)encoder waitForFence:fence];
        break;
    case DE__MTL_ENC_BLIT:
        [(id<MTLBlitCommandEncoder>)encoder waitForFence:fence];
        break;
    default:
        break;
    }
}

/* ---------- Encoders ---------- */

void de__mtl_encoder_started(de_gpu_cmd *c, void *encoder, de__mtl_encoder_kind kind) {
    mtl_cmd *mc = (mtl_cmd *)de__gpu_cmd_payload(c);
    mtl_device *m = de__mtl_dev(c->device);
    id<MTLCommandEncoder> enc = (__bridge id<MTLCommandEncoder>)encoder;
    const u32 all = m->render_stages;
    /* Before the guarded stages, or every stage when the barriers named none of them. */
    const u32 stages = (mc->wait_stages & all) ? (mc->wait_stages & all) : all;

    if (mc->wait_queue)
        wait_fence(enc, kind, (__bridge id<MTLFence>)m->queue_fence[c->queue], all);
    for (u32 i = 0; i < mc->wait_count; ++i)
        wait_fence(enc, kind, fence_at(c, mc->wait[i]), stages);
    /* A full epoch: this encoder also waits for all of it, so its fence stands for them. */
    if (mc->epoch_count == DE__MTL_EPOCH_MAX) {
        for (u32 i = 0; i < mc->epoch_count; ++i)
            wait_fence(enc, kind, fence_at(c, mc->epoch[i]), all);
        mc->epoch_count = 0;
    }
    const u32 f = mc->fence_used;
    if (de__mtl_cmd_fence(c, f) != NULL) {
        ++mc->fence_used;
        mc->epoch[mc->epoch_count++] = f;
    } else {
        DE_ERROR(c->device->log, "metal: \"%s\": no MTLFence for encoder %u; it is not ordered",
                 c->name, mc->encoders);
    }
    mc->enc_fence = f;
    mc->update_stages = all;
    if (kind == DE__MTL_ENC_RENDER || kind == DE__MTL_ENC_COMPUTE)
        de__mtl_residency_encoder(c->device, encoder, kind);
    mc->encoder = (void *)CFBridgingRetain(enc);
    mc->kind = (u32)kind;
    mc->render_ended = false;
    ++mc->encoders;
}

void de__mtl_encoder_end(de_gpu_cmd *c) {
    mtl_cmd *mc = (mtl_cmd *)de__gpu_cmd_payload(c);
    if (!mc->encoder)
        return;
    @autoreleasepool {
        id<MTLCommandEncoder> enc = (id<MTLCommandEncoder>)CFBridgingRelease(mc->encoder);
        mc->encoder = NULL;
        id<MTLFence> f = mc->enc_fence < mc->fence_used ? fence_at(c, mc->enc_fence) : nil;
        if (f != nil) {
            switch ((de__mtl_encoder_kind)mc->kind) {
            case DE__MTL_ENC_RENDER:
                [(id<MTLRenderCommandEncoder>)enc
                    updateFence:f
                    afterStages:(MTLRenderStages)(mc->update_stages | DE__MTL_STAGE_FRAGMENT)];
                break;
            case DE__MTL_ENC_COMPUTE:
                [(id<MTLComputeCommandEncoder>)enc updateFence:f];
                break;
            case DE__MTL_ENC_ACCEL:
                [(id<MTLAccelerationStructureCommandEncoder>)enc updateFence:f];
                break;
            case DE__MTL_ENC_BLIT:
                [(id<MTLBlitCommandEncoder>)enc updateFence:f];
                break;
            default:
                break;
            }
        }
        if (mc->kind == DE__MTL_ENC_RENDER) /* DENG-325: store actions left open */
            de__mtl_render_resolve_store(c, (__bridge void *)enc, false);
        [enc endEncoding];
    }
    mc->kind = DE__MTL_ENC_NONE;
    mc->render_ended = false;
}

/* ---------- Barriers ---------- */

/* The epoch so far becomes what new encoders wait for. */
static void close_epoch(mtl_cmd *mc, u32 after) {
    if (mc->epoch_count > 0) {
        memcpy(mc->wait, mc->epoch, sizeof(u32) * mc->epoch_count);
        mc->wait_count = mc->epoch_count;
        mc->wait_stages = after;
        mc->wait_queue = false; /* the epoch waited for it */
        mc->epoch_count = 0;
    } else {
        /* Barrier after barrier: the same encoders are waited for, by more stages. */
        mc->wait_stages |= after;
    }
}

void de__mtl_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count) {
    mtl_cmd *mc = (mtl_cmd *)de__gpu_cmd_payload(c);
    const u32 all = de__mtl_dev(c->device)->render_stages;
    u32 before = 0, after = 0;
    de__mtl_barrier_stages(b, count, all, &before, &after);
    if (mc->encoder && mc->kind == DE__MTL_ENC_COMPUTE) {
        /* Order the dispatches inside the encoder; it carries on into the next epoch, and
         * encoders opened later wait for it as a whole. */
        [(__bridge id<MTLComputeCommandEncoder>)mc->encoder
            memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
        close_epoch(mc, after);
        if (mc->enc_fence < mc->fence_used)
            mc->epoch[mc->epoch_count++] = mc->enc_fence;
        return;
    }
    if (mc->encoder) {
        if (mc->kind == DE__MTL_ENC_RENDER)
            mc->update_stages = before | DE__MTL_STAGE_FRAGMENT;
        de__mtl_encoder_end(c); /* blit, or a render pass that has ended */
    }
    close_epoch(mc, after);
}

/* ---------- End of a recording ---------- */

void de__mtl_cmd_finish(de_gpu_cmd *c) {
    mtl_cmd *mc = (mtl_cmd *)de__gpu_cmd_payload(c);
    mtl_device *m = de__mtl_dev(c->device);
    de__mtl_encoder_end(c);
    if (!mc->cb)
        return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)mc->cb;
        id<MTLFence> qf = (__bridge id<MTLFence>)m->queue_fence[c->queue];
        id<MTLBlitCommandEncoder> tail = [cb blitCommandEncoder];
        if (tail == nil) {
            DE_ERROR(c->device->log,
                     "metal: \"%s\": no tail encoder; the queue fence is not "
                     "updated",
                     c->name);
            return;
        }
        tail.label = @"de queue fence";
        if (mc->wait_queue)
            [tail waitForFence:qf];
        for (u32 i = 0; i < mc->wait_count; ++i)
            wait_fence(tail, DE__MTL_ENC_BLIT, fence_at(c, mc->wait[i]), 0);
        for (u32 i = 0; i < mc->epoch_count; ++i)
            wait_fence(tail, DE__MTL_ENC_BLIT, fence_at(c, mc->epoch[i]), 0);
        [tail updateFence:qf];
        [tail endEncoding];
    }
}
