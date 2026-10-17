/* mtl_cmd.m — command lists on Metal (DENG-288, SPEC §12.1).
 *
 * A de_gpu_cmd is one MTLCommandBuffer made with commandBufferWithUnretainedReferences: Metal does
 * not retain the resources it uses, the front-end's deferred destruction keeps them alive until
 * the submission completes (DENG-281). Commands go into encoders that are switched lazily: a copy
 * opens a blit encoder (or keeps the open one), begin_render opens a render encoder, S10's
 * dispatches a compute encoder (de__mtl_cmd_encoder); opening one kind ends the other. A render
 * encoder stays open after end_render until the next command, barrier or submit, so a barrier can
 * still put its fence update in it (mtl_barriers.m).
 *
 * Copies are blit commands: copy_buffer, copy_buffer_to_texture and copy_texture_to_buffer
 * (DENG-284's readback call, Decision: de_gpu reads textures back with
 * de_gpu_cmd_copy_texture_to_buffer).
 *
 * Render passes: one MTLRenderPassDescriptor from the attachments. `clear` loads with the clear
 * value, otherwise the old contents are loaded; `store` stores, otherwise they are discarded (that
 * store action is set when the encoder ends, so an indirect-count draw can split the pass,
 * DENG-325). A D32_FLOAT_S8 depth attachment also sets the stencil attachment (cleared to 0).
 * Copies use the
 * portable layout of de_gpu.h (row pitch a multiple of 256, offsets of 512); for BC formats a row
 * is a row of 4x4 blocks and the copy size is the mip's size in texels.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

static mtl_cmd *payload(de_gpu_cmd *c) {
    return (mtl_cmd *)de__gpu_cmd_payload(c);
}

/* ---------- Lifetime ---------- */

de_result de__mtl_cmd_init(de_gpu_device *d, de_gpu_cmd *c) {
    (void)d;
    @autoreleasepool {
        payload(c)->fences = (void *)CFBridgingRetain([NSMutableArray new]);
    }
    return DE_OK;
}

/* DENG-325: begin_render leaves the store action of an attachment without `store` open
 * (MTLStoreActionUnknown), so a render pass that is split for an indirect-count draw (mtl_icb.m)
 * can still store it; the encoder's end decides (DontCare unless the pass was split). */
static void resolve_store(mtl_cmd *mc, id<MTLRenderCommandEncoder> r, bool store_all) {
    const u32 mask = mc->deferred_store;
    mc->deferred_store = 0;
    if (!mask || r == nil)
        return;
    const MTLStoreAction act = store_all ? MTLStoreActionStore : MTLStoreActionDontCare;
    for (u32 i = 0; i < DE_GPU_MAX_COLOR_ATTACHMENTS; ++i)
        if (mask & (1u << i))
            [r setColorStoreAction:act atIndex:i];
    if (mask & DE__MTL_STORE_DEPTH)
        [r setDepthStoreAction:act];
    if (mask & DE__MTL_STORE_STENCIL)
        [r setStencilStoreAction:act];
}

void de__mtl_render_resolve_store(de_gpu_cmd *c, void *render_encoder, bool store_all) {
    resolve_store(payload(c), (__bridge id<MTLRenderCommandEncoder>)render_encoder, store_all);
}

static void release_recording(mtl_cmd *mc) {
    @autoreleasepool {
        if (mc->encoder) {
            /* A list that was never submitted (device destroy): end the encoder, or Metal
             * asserts when the command buffer goes away with an encoder open. */
            id<MTLCommandEncoder> enc = (id<MTLCommandEncoder>)CFBridgingRelease(mc->encoder);
            if (mc->kind == DE__MTL_ENC_RENDER)
                resolve_store(mc, (id<MTLRenderCommandEncoder>)enc, false);
            [enc endEncoding];
        }
        if (mc->accel_keep) {
            [(__bridge NSMutableArray *)mc->accel_keep removeAllObjects];
        }
        if (mc->cb)
            (void)CFBridgingRelease(mc->cb);
    }
    mc->encoder = NULL;
    mc->cb = NULL;
    mc->kind = DE__MTL_ENC_NONE;
}

void de__mtl_cmd_fini(de_gpu_device *d, de_gpu_cmd *c) {
    (void)d;
    mtl_cmd *mc = payload(c);
    release_recording(mc);
    de__mtl_query_cmd_fini(c); /* DENG-326: the resolve event and listener */
    if (mc->fences) {
        @autoreleasepool {
            (void)CFBridgingRelease(mc->fences);
        }
    }
    mc->fences = NULL;
    if (mc->accel_keep) (void)CFBridgingRelease(mc->accel_keep);
    mc->accel_keep = NULL;
}

de_result de__mtl_cmd_begin(de_gpu_cmd *c, const char *name) {
    mtl_cmd *mc = payload(c);
    mtl_device *m = de__mtl_dev(c->device);
    release_recording(mc);
    @autoreleasepool {
        /* DENG-328: with encoder execution status when crash diagnostics are on. */
        void *made = de__mtl_new_command_buffer(c->device, c->queue);
        if (made == NULL)
            return de__gpu_fail(c->device, DE_ERR_DEVICE_LOST,
                                "de_gpu_cmd_begin(\"%s\"): metal: no command buffer",
                                name ? name : "");
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)made;
        if (name && name[0])
            cb.label = [NSString stringWithUTF8String:name];
        mc->cb = made;
    }
    mc->render_ended = false;
    mc->fence_used = 0;
    mc->enc_fence = 0;
    mc->epoch_count = 0;
    mc->wait_count = 0;
    mc->wait_queue = true;
    mc->wait_stages = m->render_stages;
    mc->update_stages = m->render_stages;
    mc->encoders = 0;
    return DE_OK;
}

/* ---------- Encoders ---------- */

void *de__mtl_cmd_encoder(de_gpu_cmd *c, de__mtl_encoder_kind kind) {
    mtl_cmd *mc = payload(c);
    if (kind != DE__MTL_ENC_BLIT && kind != DE__MTL_ENC_COMPUTE && kind != DE__MTL_ENC_ACCEL)
        return NULL;
    if (mc->encoder && mc->kind == (u32)kind)
        return mc->encoder;
    de__mtl_encoder_end(c);
    if (!mc->cb)
        return NULL;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)mc->cb;
        id<MTLCommandEncoder> enc =
            kind == DE__MTL_ENC_ACCEL ? (id<MTLCommandEncoder>)[cb accelerationStructureCommandEncoder]
            : kind == DE__MTL_ENC_BLIT
                ? (id<MTLCommandEncoder>)[cb blitCommandEncoder]
                /* Concurrent: dispatches are ordered by barriers only, as everywhere else. */
                : (id<MTLCommandEncoder>)[cb
                      computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
        if (enc == nil) {
            de__gpu_fail(c->device, DE_ERR_DEVICE_LOST, "metal: \"%s\": no %s encoder", c->name,
                         kind == DE__MTL_ENC_ACCEL ? "acceleration" : kind == DE__MTL_ENC_BLIT ? "blit" : "compute");
            return NULL;
        }
        if (c->name[0])
            enc.label = [NSString stringWithUTF8String:c->name];
        de__mtl_encoder_started(c, (__bridge void *)enc, kind);
    }
    return mc->encoder;
}

static id<MTLBlitCommandEncoder> blit(de_gpu_cmd *c) {
    return (__bridge id<MTLBlitCommandEncoder>)de__mtl_cmd_encoder(c, DE__MTL_ENC_BLIT);
}

/* ---------- Render passes ---------- */

static id<MTLTexture> texture_of(de_gpu_device *d, de_gpu_texture h) {
    de__gpu_texture *t = de__gpu_get_texture(d, h);
    return t ? (__bridge id<MTLTexture>)DE__GPU_PAYLOAD(mtl_texture, t)->texture : nil;
}

static void set_attachment(MTLRenderPassAttachmentDescriptor *a, id<MTLTexture> tex,
                           const de_gpu_attachment *src) {
    a.texture = tex;
    a.level = src->mip;
    if (tex.textureType == MTLTextureType3D)
        a.depthPlane = src->layer;
    else
        a.slice = src->layer;
    a.loadAction = src->clear ? MTLLoadActionClear : MTLLoadActionLoad;
    /* Without `store`: decided when the encoder ends (resolve_store, DENG-325). */
    a.storeAction = src->store ? MTLStoreActionStore : MTLStoreActionUnknown;
}

void de__mtl_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                              const de_gpu_attachment *depth) {
    mtl_cmd *mc = payload(c);
    de__mtl_encoder_end(c);
    if (!mc->cb)
        return;
    u32 deferred = 0;
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        for (u32 i = 0; i < color_count; ++i) {
            id<MTLTexture> tex = texture_of(c->device, colors[i].texture);
            if (tex == nil)
                continue; /* stale handle: reported by the front-end in Debug/Dev */
            MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[i];
            set_attachment(a, tex, &colors[i]);
            if (!colors[i].store)
                deferred |= 1u << i;
            const f32 *cc = colors[i].clear_color;
            a.clearColor = MTLClearColorMake(cc[0], cc[1], cc[2], cc[3]);
        }
        if (depth) {
            id<MTLTexture> tex = texture_of(c->device, depth->texture);
            if (tex != nil) {
                set_attachment(rp.depthAttachment, tex, depth);
                rp.depthAttachment.clearDepth = depth->clear_depth;
                if (!depth->store)
                    deferred |= DE__MTL_STORE_DEPTH;
                if (tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
                    set_attachment(rp.stencilAttachment, tex, depth);
                    rp.stencilAttachment.clearStencil = 0;
                    if (!depth->store)
                        deferred |= DE__MTL_STORE_STENCIL;
                }
            }
        }
        id<MTLRenderCommandEncoder> enc =
            [(__bridge id<MTLCommandBuffer>)mc->cb renderCommandEncoderWithDescriptor:rp];
        if (enc == nil) {
            de__gpu_fail(c->device, DE_ERR_DEVICE_LOST, "metal: \"%s\": no render encoder",
                         c->name);
            return;
        }
        if (c->name[0])
            enc.label = [NSString stringWithUTF8String:c->name];
        de__mtl_encoder_started(c, (__bridge void *)enc, DE__MTL_ENC_RENDER);
        mc->deferred_store = deferred;
    }
}

void de__mtl_cmd_end_render(de_gpu_cmd *c) {
    mtl_cmd *mc = payload(c);
    if (mc->encoder && mc->kind == DE__MTL_ENC_RENDER)
        mc->render_ended = true; /* ended by the next command, barrier or submit */
}

void de__mtl_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h) {
    mtl_cmd *mc = payload(c);
    if (!mc->encoder || mc->kind != DE__MTL_ENC_RENDER || mc->render_ended)
        return;
    /* Metal's clip space already has +Y up and its framebuffer origin top-left: the viewport is
     * used as given (Vulkan flips it with a negative height to match). */
    const MTLViewport vp = {x, y, w, h, 0.0, 1.0};
    [(__bridge id<MTLRenderCommandEncoder>)mc->encoder setViewport:vp];
}

/* ---------- Copies ---------- */

de__mtl_copy_region de__mtl_copy_region_of(const de__gpu_texture *t, u32 mip, u32 layer,
                                           u32 row_pitch) {
    de__mtl_copy_region r;
    memset(&r, 0, sizeof r);
    u32 w = t->width >> mip, h = t->height >> mip;
    r.width = w ? w : 1u;
    r.height = h ? h : 1u;
    r.depth = 1u;
    r.slice = layer;
    if (t->dim == DE_TEX_3D) {
        const u32 dz = t->depth_or_layers >> mip;
        r.depth = dz ? dz : 1u;
        r.slice = 0;
    }
    const u32 rows = de__gpu_format_is_compressed(t->format) ? (r.height + 3u) / 4u : r.height;
    r.bytes_per_row = row_pitch;
    r.bytes_per_image = row_pitch * rows;
    return r;
}

static MTLBlitOption blit_options(const de__gpu_texture *t) {
    return t->format == DE_FMT_D32_FLOAT_S8 ? MTLBlitOptionDepthFromDepthStencil
                                            : MTLBlitOptionNone;
}

static id<MTLBuffer> buffer_object(de__gpu_buffer *b) {
    return (__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer, b)->buffer;
}

static id<MTLTexture> texture_object(de__gpu_texture *t) {
    return (__bridge id<MTLTexture>)DE__GPU_PAYLOAD(mtl_texture, t)->texture;
}

void de__mtl_cmd_copy_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off, de__gpu_buffer *src,
                             u64 src_off, u64 bytes) {
    @autoreleasepool {
        id<MTLBlitCommandEncoder> b = blit(c);
        [b copyFromBuffer:buffer_object(src)
                 sourceOffset:(NSUInteger)src_off
                     toBuffer:buffer_object(dst)
            destinationOffset:(NSUInteger)dst_off
                         size:(NSUInteger)bytes];
    }
}

void de__mtl_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de__gpu_texture *dst, u32 mip, u32 layer,
                                        de__gpu_buffer *src, u64 src_off, u32 row_pitch) {
    const de__mtl_copy_region r = de__mtl_copy_region_of(dst, mip, layer, row_pitch);
    @autoreleasepool {
        id<MTLBlitCommandEncoder> b = blit(c);
        [b copyFromBuffer:buffer_object(src)
                   sourceOffset:(NSUInteger)src_off
              sourceBytesPerRow:r.bytes_per_row
            sourceBytesPerImage:r.bytes_per_image
                     sourceSize:MTLSizeMake(r.width, r.height, r.depth)
                      toTexture:texture_object(dst)
               destinationSlice:r.slice
               destinationLevel:mip
              destinationOrigin:MTLOriginMake(0, 0, 0)
                        options:blit_options(dst)];
    }
}

void de__mtl_cmd_copy_texture_to_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off,
                                        u32 row_pitch, de__gpu_texture *src, u32 mip, u32 layer) {
    const de__mtl_copy_region r = de__mtl_copy_region_of(src, mip, layer, row_pitch);
    @autoreleasepool {
        id<MTLBlitCommandEncoder> b = blit(c);
        [b copyFromTexture:texture_object(src)
                         sourceSlice:r.slice
                         sourceLevel:mip
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(r.width, r.height, r.depth)
                            toBuffer:buffer_object(dst)
                   destinationOffset:(NSUInteger)dst_off
              destinationBytesPerRow:r.bytes_per_row
            destinationBytesPerImage:r.bytes_per_image
                             options:blit_options(src)];
    }
}
