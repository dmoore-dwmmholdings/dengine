/* mtl_icb.m — indexed indirect draws, with and without a count buffer, on Metal (DENG-325,
 * SPEC §12.1).
 *
 * Records are de_gpu.h's 20-byte (index_count, instance_count, first_index, vertex_offset,
 * first_instance), which is MTLDrawIndexedPrimitivesIndirectArguments.
 *
 * Without a count buffer: `max_draws` drawIndexedPrimitives:...indirectBuffer: calls, 20 bytes
 * apart, with the bound index buffer.
 *
 * With a count buffer: Metal has no count-buffer draws, so the draws go through an indirect
 * command buffer (ICB) that the GPU encodes:
 *   1. A compute kernel (de_icb_encode, below) reads the count and the records. Thread i < min(
 *      count, max_draws) encodes draw i into ICB command `first + i` (draw_indexed_primitives with
 *      the index buffer's GPU address); thread 0 writes the execution range {first, n} into a
 *      range buffer.
 *   2. The render encoder runs executeCommandsInBuffer:indirectBuffer:indirectBufferOffset: with
 *      that range. The ICB inherits the pipeline state and the buffers (the bindless table at
 *      index 0, the push constants at index 1) from the encoder, so its draws are the bound
 *      pipeline's.
 * A compute kernel cannot run inside a render pass, so the pass is split around it: the open
 * render encoder stores every attachment (its open store actions, mtl_cmd.m) and ends, the kernel
 * runs on a compute encoder (which waits, like any new encoder, for the epoch before the last
 * barrier: the one that wrote the records), and a new render encoder loads the attachments and
 * waits for both before it runs the ICB. The draw state is applied to the new encoder
 * (mtl_draw.m). On Apple GPUs a split costs a store and a load of the attachments, so a pass
 * should keep its indirect-count draws few (GPU culling, S13, records one per pass).
 *
 * The kernel is Metal Shading Language compiled at run time (once per device, on first use):
 * Slang cannot write MSL's indirect command buffers (command_buffer, render_command), so it
 * cannot come from shaders/ through the importer. Each command list owns one ICB and one range
 * buffer, grown when a recording needs more and reused once the list's submission completed; a
 * draw takes `max_draws` commands of it. Both are private, hazard-tracked resources outside the
 * heaps, named to Metal with useResource:.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

/* Commands of one ICB: max_draws above it are cut (a warning). */
#define ICB_MAX_COMMANDS 16384u
#define ICB_MIN_COMMANDS 256u
#define RANGE_MIN        64u

static NSString *const k_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct de_icb_container { command_buffer cmds; };\n"
     "struct de_icb_params {\n"
     "    ulong index_address; /* GPU address of index 0 (the bound offset applied) */\n"
     "    uint index_u16;\n"
     "    uint max_draws;\n"
     "    uint first;          /* first ICB command of this draw */\n"
     "    uint pad;\n"
     "};\n"
     "struct de_icb_range { uint location; uint length; };\n"
     "kernel void de_icb_encode(constant de_icb_container &icb [[buffer(0)]],\n"
     "                          const device uint *args [[buffer(1)]],\n"
     "                          const device uint *count [[buffer(2)]],\n"
     "                          device de_icb_range *range [[buffer(3)]],\n"
     "                          constant de_icb_params &p [[buffer(4)]],\n"
     "                          uint i [[thread_position_in_grid]])\n"
     "{\n"
     "    const uint n = min(count[0], p.max_draws);\n"
     "    if (i == 0u) {\n"
     "        range->location = p.first;\n"
     "        range->length = n;\n"
     "    }\n"
     "    if (i >= n)\n"
     "        return;\n"
     "    const device uint *a = args + i * 5u;\n"
     "    render_command cmd(icb.cmds, p.first + i);\n"
     "    if (p.index_u16 != 0u) {\n"
     "        const device ushort *ib = reinterpret_cast<const device ushort *>(p.index_address);\n"
     "        cmd.draw_indexed_primitives(primitive_type::triangle, a[0], ib + a[2], a[1],\n"
     "                                    int(a[3]), a[4]);\n"
     "    } else {\n"
     "        const device uint *ib = reinterpret_cast<const device uint *>(p.index_address);\n"
     "        cmd.draw_indexed_primitives(primitive_type::triangle, a[0], ib + a[2], a[1],\n"
     "                                    int(a[3]), a[4]);\n"
     "    }\n"
     "}\n";

typedef struct icb_params {
    u64 index_address;
    u32 index_u16, max_draws, first, pad;
} icb_params;

static mtl_cmd *payload(de_gpu_cmd *c) {
    return (mtl_cmd *)de__gpu_cmd_payload(c);
}

static void release(void **p) {
    if (*p)
        (void)CFBridgingRelease(*p);
    *p = NULL;
}

void de__mtl_icb_release(mtl_draw_state *ds, bool keep) {
    @autoreleasepool {
        release(&ds->retired);
        if (!keep) {
            release(&ds->icb);
            release(&ds->ranges);
            ds->icb_cap = ds->range_cap = 0;
        }
    }
    ds->icb_used = ds->range_used = 0;
}

/* The encoding kernel, compiled on first use. NULL (logged) when it does not compile. */
static id<MTLComputePipelineState> kernel_of(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    de_mutex_lock(&m->library_lock);
    id<MTLComputePipelineState> k = (__bridge id<MTLComputePipelineState>)m->icb_kernel;
    if (k == nil) {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
        MTLCompileOptions *o = [MTLCompileOptions new];
        o.languageVersion = MTLLanguageVersion3_0;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:k_source options:o error:&err];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"de_icb_encode"] : nil;
        if (fn != nil)
            k = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (k != nil)
            m->icb_kernel = (void *)CFBridgingRetain(k);
        else
            DE_ERROR(d->log, "metal: the indirect-count kernel does not compile: %s",
                     err ? err.localizedDescription.UTF8String : "no reason given");
    }
    de_mutex_unlock(&m->library_lock);
    return k;
}

static void retire(mtl_draw_state *ds, void **obj) {
    if (!*obj)
        return;
    if (!ds->retired)
        ds->retired = (void *)CFBridgingRetain([NSMutableArray new]);
    [(__bridge NSMutableArray *)ds->retired addObject:(id)CFBridgingRelease(*obj)];
    *obj = NULL;
}

/* Room for `commands` more ICB commands and one more range in this recording. */
static bool reserve(de_gpu_cmd *c, mtl_draw_state *ds, u32 commands) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(c->device)->device;
    if (ds->icb_used + commands > ds->icb_cap) {
        u32 cap = ds->icb_cap * 2u;
        if (cap < ds->icb_used + commands)
            cap = ds->icb_used + commands;
        if (cap < ICB_MIN_COMMANDS)
            cap = ICB_MIN_COMMANDS;
        if (cap > ICB_MAX_COMMANDS) /* commands <= ICB_MAX_COMMANDS */
            cap = ICB_MAX_COMMANDS;
        /* A new ICB starts at command 0: the old one stays alive for the draws already in it. */
        MTLIndirectCommandBufferDescriptor *id_ = [MTLIndirectCommandBufferDescriptor new];
        id_.commandTypes = MTLIndirectCommandTypeDrawIndexed;
        id_.inheritPipelineState = YES;
        id_.inheritBuffers = YES;
        id_.maxVertexBufferBindCount = 0;
        id_.maxFragmentBufferBindCount = 0;
        id<MTLIndirectCommandBuffer> icb =
            [dev newIndirectCommandBufferWithDescriptor:id_
                                        maxCommandCount:cap
                                                options:MTLResourceStorageModePrivate];
        if (icb == nil)
            return false;
        icb.label = @"de indirect-count draws";
        retire(ds, &ds->icb);
        ds->icb = (void *)CFBridgingRetain(icb);
        ds->icb_cap = cap;
        ds->icb_used = 0;
    }
    if (ds->range_used + 1u > ds->range_cap) {
        const u32 cap = ds->range_cap ? ds->range_cap * 2u : RANGE_MIN;
        id<MTLBuffer> b = [dev newBufferWithLength:(NSUInteger)cap * 8u
                                           options:MTLResourceStorageModePrivate];
        if (b == nil)
            return false;
        b.label = @"de indirect-count ranges";
        retire(ds, &ds->ranges);
        ds->ranges = (void *)CFBridgingRetain(b);
        ds->range_cap = cap;
        ds->range_used = 0;
    }
    return true;
}

static id<MTLBuffer> buffer_of(de__gpu_buffer *b) {
    return (__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer, b)->buffer;
}

static void plain_draws(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset, u32 max_draws) {
    mtl_draw_state *ds = &payload(c)->draw;
    id<MTLRenderCommandEncoder> r =
        (__bridge id<MTLRenderCommandEncoder>)de__mtl_draw_render_encoder(c);
    if (r == nil || !ds->index_buffer || ds->kind != DE_PIPE_GRAPHICS)
        return;
    id<MTLBuffer> ib = (__bridge id<MTLBuffer>)ds->index_buffer;
    const MTLIndexType it = ds->index_u16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    id<MTLBuffer> a = buffer_of(args);
    for (u32 i = 0; i < max_draws; ++i)
        [r drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                       indexType:it
                     indexBuffer:ib
               indexBufferOffset:(NSUInteger)ds->index_offset
                  indirectBuffer:a
            indirectBufferOffset:(NSUInteger)(offset + (u64)i * 20u)];
}

void de__mtl_cmd_draw_indexed_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                       de__gpu_buffer *count, u64 count_offset, u32 max_draws) {
    mtl_cmd *mc = payload(c);
    mtl_draw_state *ds = &mc->draw;
    @autoreleasepool {
        if (!count) {
            plain_draws(c, args, offset, max_draws);
            return;
        }
        if (max_draws == 0 || !mc->encoder || mc->kind != DE__MTL_ENC_RENDER || mc->render_ended ||
            !ds->index_buffer || ds->kind != DE_PIPE_GRAPHICS)
            return;
        if (max_draws > ICB_MAX_COMMANDS) {
            DE_WARN(c->device->log,
                    "metal: \"%s\": draw_indexed_indirect with max_draws %u; the indirect "
                    "command buffer takes %u, the rest are not drawn",
                    c->name, max_draws, ICB_MAX_COMMANDS);
            max_draws = ICB_MAX_COMMANDS;
        }
        id<MTLComputePipelineState> kernel = kernel_of(c->device);
        if (kernel == nil)
            return;
        if (!reserve(c, ds, max_draws)) {
            de__gpu_fail(c->device, DE_ERR_OUT_OF_MEMORY,
                         "metal: \"%s\": no indirect command buffer for %u draws", c->name,
                         max_draws);
            return;
        }
        id<MTLIndirectCommandBuffer> icb = (__bridge id<MTLIndirectCommandBuffer>)ds->icb;
        id<MTLBuffer> ranges = (__bridge id<MTLBuffer>)ds->ranges;
        const u32 first = ds->icb_used;
        const NSUInteger range_off = (NSUInteger)ds->range_used * 8u;
        ds->icb_used += max_draws;
        ds->range_used += 1u;

        /* Split: store everything, end the render encoder (it updates its fence). */
        const u32 part_fence = mc->enc_fence < mc->fence_used ? mc->enc_fence : UINT32_MAX;
        de__mtl_render_resolve_store(c, mc->encoder, true);
        de__mtl_encoder_end(c);

        /* Encode the ICB. */
        id<MTLComputeCommandEncoder> e =
            (__bridge id<MTLComputeCommandEncoder>)de__mtl_cmd_encoder(c, DE__MTL_ENC_COMPUTE);
        if (e == nil)
            return;
        const u32 kernel_fence = mc->enc_fence < mc->fence_used ? mc->enc_fence : UINT32_MAX;
        [e setComputePipelineState:kernel];
        const MTLResourceID rid = icb.gpuResourceID;
        [e setBytes:&rid length:sizeof rid atIndex:0];
        [e setBuffer:buffer_of(args) offset:(NSUInteger)offset atIndex:1];
        [e setBuffer:buffer_of(count) offset:(NSUInteger)count_offset atIndex:2];
        [e setBuffer:ranges offset:range_off atIndex:3];
        const icb_params prm = {.index_address = ds->index_address,
                                .index_u16 = ds->index_u16 ? 1u : 0u,
                                .max_draws = max_draws,
                                .first = first};
        [e setBytes:&prm length:sizeof prm atIndex:4];
        [e useResource:icb usage:MTLResourceUsageWrite];
        const NSUInteger width = max_draws < 64u ? max_draws : 64u;
        [e dispatchThreads:MTLSizeMake(max_draws, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        de__mtl_encoder_end(c);

        /* Resume the pass: load what the first part stored. */
        de_gpu_attachment colors[DE_GPU_MAX_COLOR_ATTACHMENTS], depth;
        memcpy(colors, ds->colors, sizeof(de_gpu_attachment) * ds->color_count);
        for (u32 i = 0; i < ds->color_count; ++i)
            colors[i].clear = false;
        depth = ds->depth;
        depth.clear = false;
        de__mtl_cmd_begin_render(c, colors, ds->color_count, ds->has_depth ? &depth : NULL);
        if (!mc->encoder || mc->kind != DE__MTL_ENC_RENDER)
            return;
        id<MTLRenderCommandEncoder> r = (__bridge id<MTLRenderCommandEncoder>)mc->encoder;
        const u32 all = de__mtl_dev(c->device)->render_stages;
        if (part_fence != UINT32_MAX)
            [r waitForFence:(__bridge id<MTLFence>)de__mtl_cmd_fence(c, part_fence)
                beforeStages:(MTLRenderStages)all];
        if (kernel_fence != UINT32_MAX)
            [r waitForFence:(__bridge id<MTLFence>)de__mtl_cmd_fence(c, kernel_fence)
                beforeStages:(MTLRenderStages)all];
        ++ds->splits;

        r = (__bridge id<MTLRenderCommandEncoder>)de__mtl_draw_render_encoder(c);
        if (r == nil)
            return;
        [r useResource:icb
                 usage:MTLResourceUsageRead
                stages:MTLRenderStageVertex | MTLRenderStageFragment];
        [r executeCommandsInBuffer:icb indirectBuffer:ranges indirectBufferOffset:range_off];
    }
}
