/* mtl_draw.m — pipeline binding, push constants, draws and dispatches on Metal (DENG-325,
 * SPEC §12.1).
 *
 * Metal state belongs to an encoder, and a command list opens many (mtl_cmd.m switches them
 * lazily, a barrier ends a render encoder, an indirect-count draw splits a render pass). So the
 * list keeps its draw state (mtl_draw_state: the bound pipeline, its depth state and cull mode,
 * 128 bytes of push constants, the viewport, the scissor, the index buffer) and applies what
 * changed to the open encoder before each draw or dispatch; a new encoder gets all of it.
 *
 *   - Pipelines: setRenderPipelineState / setComputePipelineState, the pipeline's depth-stencil
 *     state, its cull mode and MTLWindingCounterClockwise (front faces are counter-clockwise in
 *     clip space; mtl_pipeline.m).
 *   - Push constants: setVertexBytes / setFragmentBytes / setObjectBytes / setMeshBytes /
 *     setBytes at buffer index 1 for the stages whose shaders read them, `push_bytes` bytes (the
 *     shaders' binding size, so MSL's struct padding is covered).
 *   - Viewport: as given (mtl_cmd.m). Scissor: in pixels from the top-left of the render area,
 *     cut to it (de__mtl_scissor_rect): Metal refuses a rectangle outside the attachments.
 *     begin_render leaves both at the whole area (Metal's default for a new render encoder).
 *   - Draws: drawPrimitives / drawIndexedPrimitives (triangles); the index buffer's offset plus
 *     first_index * index size.
 *   - Dispatches: dispatchThreadgroups with the pipeline's threadgroup size (from the desc), and
 *     dispatchThreadgroupsWithIndirectBuffer (12-byte x, y, z records).
 *   - Mesh: drawMeshThreadgroups with the object and mesh threadgroup sizes. Indirect mesh
 *     dispatches issue `max` drawMeshThreadgroupsWithIndirectBuffer calls, 12 bytes apart, and do
 *     not read the count buffer: records past the count must hold zero threadgroups (the GPU
 *     culling that writes them, S13, clears the unused ones). A zero record draws nothing.
 *   - Indexed indirect draws: mtl_icb.m.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

static mtl_cmd *payload(de_gpu_cmd *c) {
    return (mtl_cmd *)de__gpu_cmd_payload(c);
}

static mtl_draw_state *state_of(de_gpu_cmd *c) {
    return &payload(c)->draw;
}

/* ---------- Lifetime (wrapping mtl_cmd.m) ---------- */

de_result de__mtl_draw_cmd_begin(de_gpu_cmd *c, const char *name) {
    mtl_draw_state *ds = state_of(c);
    /* The list's previous submission has completed: its ICB and range buffer are free again. */
    de__mtl_icb_release(ds, true);
    void *icb = ds->icb, *ranges = ds->ranges;
    const u32 icb_cap = ds->icb_cap, range_cap = ds->range_cap;
    memset(ds, 0, sizeof *ds);
    ds->icb = icb;
    ds->icb_cap = icb_cap;
    ds->ranges = ranges;
    ds->range_cap = range_cap;
    return de__mtl_cmd_begin(c, name);
}

void de__mtl_draw_cmd_fini(de_gpu_device *d, de_gpu_cmd *c) {
    de__mtl_icb_release(state_of(c), false);
    de__mtl_cmd_fini(d, c);
}

void de__mtl_draw_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                               const de_gpu_attachment *depth) {
    mtl_draw_state *ds = state_of(c);
    de__mtl_cmd_begin_render(c, colors, color_count, depth);
    /* Kept for a split (mtl_icb.m), and for the render area. */
    ds->color_count =
        color_count < DE_GPU_MAX_COLOR_ATTACHMENTS ? color_count : DE_GPU_MAX_COLOR_ATTACHMENTS;
    if (ds->color_count)
        memcpy(ds->colors, colors, sizeof(de_gpu_attachment) * ds->color_count);
    ds->has_depth = depth != NULL;
    if (depth)
        ds->depth = *depth;
    ds->area[0] = ds->area[1] = 0;
    const de_gpu_attachment *first = ds->color_count ? &colors[0] : depth;
    de__gpu_texture *t = first ? de__gpu_get_texture(c->device, first->texture) : NULL;
    if (t) {
        const u32 w = t->width >> first->mip, h = t->height >> first->mip;
        ds->area[0] = w ? w : 1u;
        ds->area[1] = h ? h : 1u;
    }
    ds->has_viewport = false;
    ds->has_scissor = false;
}

void de__mtl_draw_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h) {
    mtl_draw_state *ds = state_of(c);
    ds->viewport[0] = x;
    ds->viewport[1] = y;
    ds->viewport[2] = w;
    ds->viewport[3] = h;
    ds->has_viewport = true;
    ds->dirty |= DE__MTL_DIRTY_VIEWPORT;
}

/* ---------- Applying the state ---------- */

void de__mtl_scissor_rect(i32 x, i32 y, u32 w, u32 h, u32 area_w, u32 area_h, u32 out[4]) {
    i64 x0 = x, y0 = y, x1 = (i64)x + (i64)w, y1 = (i64)y + (i64)h;
    x0 = x0 < 0 ? 0 : (x0 > (i64)area_w ? (i64)area_w : x0);
    y0 = y0 < 0 ? 0 : (y0 > (i64)area_h ? (i64)area_h : y0);
    x1 = x1 < 0 ? 0 : (x1 > (i64)area_w ? (i64)area_w : x1);
    y1 = y1 < 0 ? 0 : (y1 > (i64)area_h ? (i64)area_h : y1);
    if (x0 > x1)
        x0 = x1;
    if (y0 > y1)
        y0 = y1;
    out[0] = (u32)x0;
    out[1] = (u32)y0;
    out[2] = (u32)(x1 - x0);
    out[3] = (u32)(y1 - y0);
}

/* Marks the state as not applied when `encoders` has moved on (a new encoder). */
static u32 pending(mtl_cmd *mc, mtl_draw_state *ds) {
    if (ds->applied != mc->encoders) {
        ds->applied = mc->encoders;
        ds->dirty = DE__MTL_DIRTY_ALL;
    }
    const u32 dirty = ds->dirty;
    ds->dirty = 0;
    return dirty;
}

void *de__mtl_draw_render_encoder(de_gpu_cmd *c) {
    mtl_cmd *mc = payload(c);
    mtl_draw_state *ds = &mc->draw;
    if (!mc->encoder || mc->kind != DE__MTL_ENC_RENDER || mc->render_ended)
        return NULL;
    id<MTLRenderCommandEncoder> r = (__bridge id<MTLRenderCommandEncoder>)mc->encoder;
    const u32 dirty = pending(mc, ds);
    const bool raster = ds->pipeline && ds->kind != DE_PIPE_COMPUTE;
    if ((dirty & DE__MTL_DIRTY_PIPELINE) && raster) {
        [r setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)ds->pipeline];
        if (ds->depth_state)
            [r setDepthStencilState:(__bridge id<MTLDepthStencilState>)ds->depth_state];
        [r setCullMode:(MTLCullMode)ds->cull];
        [r setFrontFacingWinding:MTLWindingCounterClockwise];
    }
    if ((dirty & DE__MTL_DIRTY_VIEWPORT) && ds->has_viewport)
        de__mtl_cmd_viewport(c, ds->viewport[0], ds->viewport[1], ds->viewport[2], ds->viewport[3]);
    if ((dirty & DE__MTL_DIRTY_SCISSOR) && ds->has_scissor) {
        u32 s[4];
        de__mtl_scissor_rect(ds->scissor_xy[0], ds->scissor_xy[1], ds->scissor_wh[0],
                             ds->scissor_wh[1], ds->area[0], ds->area[1], s);
        [r setScissorRect:(MTLScissorRect){s[0], s[1], s[2], s[3]}];
    }
    if ((dirty & DE__MTL_DIRTY_PUSH) && raster && ds->push_bytes) {
        const u32 st = ds->push_stages;
        const NSUInteger n = ds->push_bytes;
        if (st & DE__MTL_PUSH_VERTEX)
            [r setVertexBytes:ds->push length:n atIndex:1];
        if (st & DE__MTL_PUSH_FRAGMENT)
            [r setFragmentBytes:ds->push length:n atIndex:1];
        if (@available(macOS 13.0, *)) {
            if (st & DE__MTL_PUSH_OBJECT)
                [r setObjectBytes:ds->push length:n atIndex:1];
            if (st & DE__MTL_PUSH_MESH)
                [r setMeshBytes:ds->push length:n atIndex:1];
        }
    }
    return mc->encoder;
}

static id<MTLRenderCommandEncoder> render_encoder(de_gpu_cmd *c) {
    return (__bridge id<MTLRenderCommandEncoder>)de__mtl_draw_render_encoder(c);
}

static id<MTLComputeCommandEncoder> compute_encoder(de_gpu_cmd *c) {
    mtl_cmd *mc = payload(c);
    mtl_draw_state *ds = &mc->draw;
    id<MTLComputeCommandEncoder> e =
        (__bridge id<MTLComputeCommandEncoder>)de__mtl_cmd_encoder(c, DE__MTL_ENC_COMPUTE);
    if (e == nil || !ds->pipeline || ds->kind != DE_PIPE_COMPUTE)
        return nil;
    const u32 dirty = pending(mc, ds);
    if (dirty & DE__MTL_DIRTY_PIPELINE)
        [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)ds->pipeline];
    if ((dirty & DE__MTL_DIRTY_PUSH) && ds->push_bytes && (ds->push_stages & DE__MTL_PUSH_COMPUTE))
        [e setBytes:ds->push length:ds->push_bytes atIndex:1];
    return e;
}

/* ---------- Commands ---------- */

void de__mtl_cmd_set_pipeline(de_gpu_cmd *c, de__gpu_pipeline *p) {
    mtl_draw_state *ds = state_of(c);
    const mtl_pipeline *mp = DE__GPU_PAYLOAD(mtl_pipeline, p);
    ds->pipeline = mp->state;
    ds->depth_state = mp->depth_state;
    ds->kind = (u32)p->kind;
    ds->cull = mp->cull;
    ds->push_stages = mp->push_stages;
    ds->push_bytes = mp->push_bytes;
    memcpy(ds->threadgroup, p->threadgroup_size, sizeof ds->threadgroup);
    memcpy(ds->object_threadgroup, p->object_threadgroup_size, sizeof ds->object_threadgroup);
    /* Push constants stay set across pipelines (as on Vulkan); the new one's stages get them. */
    ds->dirty |= DE__MTL_DIRTY_PIPELINE | DE__MTL_DIRTY_PUSH;
}

void de__mtl_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes) {
    mtl_draw_state *ds = state_of(c);
    if (bytes > sizeof ds->push)
        bytes = sizeof ds->push;
    memcpy(ds->push, data, bytes);
    ds->dirty |= DE__MTL_DIRTY_PUSH;
}

void de__mtl_cmd_scissor(de_gpu_cmd *c, i32 x, i32 y, u32 w, u32 h) {
    mtl_draw_state *ds = state_of(c);
    ds->scissor_xy[0] = x;
    ds->scissor_xy[1] = y;
    ds->scissor_wh[0] = w;
    ds->scissor_wh[1] = h;
    ds->has_scissor = true;
    ds->dirty |= DE__MTL_DIRTY_SCISSOR;
}

void de__mtl_cmd_set_index_buffer(de_gpu_cmd *c, de__gpu_buffer *b, u64 offset, bool index_u16) {
    mtl_draw_state *ds = state_of(c);
    const mtl_buffer *mb = DE__GPU_PAYLOAD(mtl_buffer, b);
    ds->index_buffer = mb->buffer;
    ds->index_offset = offset;
    ds->index_address = mb->gpu_address + offset;
    ds->index_u16 = index_u16;
}

void de__mtl_cmd_draw(de_gpu_cmd *c, u32 vertex_count, u32 instance_count, u32 first_vertex,
                      u32 first_instance) {
    @autoreleasepool {
        id<MTLRenderCommandEncoder> r = render_encoder(c);
        if (r == nil || state_of(c)->kind != DE_PIPE_GRAPHICS)
            return;
        [r drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:first_vertex
              vertexCount:vertex_count
            instanceCount:instance_count
             baseInstance:first_instance];
    }
}

void de__mtl_cmd_draw_indexed(de_gpu_cmd *c, u32 index_count, u32 instance_count, u32 first_index,
                              i32 vertex_offset, u32 first_instance) {
    mtl_draw_state *ds = state_of(c);
    @autoreleasepool {
        id<MTLRenderCommandEncoder> r = render_encoder(c);
        if (r == nil || ds->kind != DE_PIPE_GRAPHICS || !ds->index_buffer)
            return;
        const u64 size = ds->index_u16 ? 2u : 4u;
        [r drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:index_count
                       indexType:ds->index_u16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                     indexBuffer:(__bridge id<MTLBuffer>)ds->index_buffer
               indexBufferOffset:(NSUInteger)(ds->index_offset + (u64)first_index * size)
                   instanceCount:instance_count
                      baseVertex:vertex_offset
                    baseInstance:first_instance];
    }
}

static MTLSize size3(const u32 s[3]) {
    return MTLSizeMake(s[0] ? s[0] : 1u, s[1] ? s[1] : 1u, s[2] ? s[2] : 1u);
}

void de__mtl_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    @autoreleasepool {
        id<MTLComputeCommandEncoder> e = compute_encoder(c);
        if (e == nil)
            return;
        [e dispatchThreadgroups:MTLSizeMake(x, y, z)
            threadsPerThreadgroup:size3(state_of(c)->threadgroup)];
    }
}

void de__mtl_cmd_dispatch_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset) {
    @autoreleasepool {
        id<MTLComputeCommandEncoder> e = compute_encoder(c);
        if (e == nil)
            return;
        [e dispatchThreadgroupsWithIndirectBuffer:(__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(
                                                      mtl_buffer, args)
                                                      ->buffer
                             indirectBufferOffset:(NSUInteger)offset
                            threadsPerThreadgroup:size3(state_of(c)->threadgroup)];
    }
}

void de__mtl_cmd_dispatch_mesh(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    mtl_draw_state *ds = state_of(c);
    @autoreleasepool {
        id<MTLRenderCommandEncoder> r = render_encoder(c);
        if (r == nil || ds->kind != DE_PIPE_MESH)
            return;
        if (@available(macOS 13.0, *)) {
            [r drawMeshThreadgroups:MTLSizeMake(x, y, z)
                threadsPerObjectThreadgroup:size3(ds->object_threadgroup)
                  threadsPerMeshThreadgroup:size3(ds->threadgroup)];
        }
    }
}

void de__mtl_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                        de__gpu_buffer *count, u64 count_offset, u32 max) {
    (void)count; /* records past the count hold zero threadgroups (see the file comment) */
    (void)count_offset;
    mtl_draw_state *ds = state_of(c);
    @autoreleasepool {
        id<MTLRenderCommandEncoder> r = render_encoder(c);
        if (r == nil || ds->kind != DE_PIPE_MESH)
            return;
        id<MTLBuffer> buf = (__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer, args)->buffer;
        if (@available(macOS 13.0, *)) {
            for (u32 i = 0; i < max; ++i)
                [r drawMeshThreadgroupsWithIndirectBuffer:buf
                                     indirectBufferOffset:(NSUInteger)(offset + (u64)i * 12u)
                              threadsPerObjectThreadgroup:size3(ds->object_threadgroup)
                                threadsPerMeshThreadgroup:size3(ds->threadgroup)];
        }
    }
}
