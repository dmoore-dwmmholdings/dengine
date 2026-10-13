/* d3d12_barriers.c — de_gpu barriers on D3D12: enhanced barriers, with legacy ResourceBarrier as
 * the fallback (DENG-286; SPEC §12.1).
 *
 * Two parts:
 *   - The translation (every platform): pure functions over plain-number copies of the D3D12
 *     enums (d3d12_internal.h), unit-tested on every build host. Each copy is checked against
 *     d3d12.h below when this file is compiled for Windows.
 *   - The recording (Windows): de__d3d12_cmd_barriers.
 *
 * Mode: a device records enhanced barriers (ID3D12GraphicsCommandList7::Barrier) when
 * caps.enhanced_barriers is true and the cvar gpu.d3d12.force_legacy_barriers is false (its
 * default comes from the environment variable DE_D3D12_FORCE_LEGACY_BARRIERS=1, so a test run can
 * force it); else legacy ResourceBarrier. The choice is made once per device, by its first
 * command list, and then fixed: the legacy state tracking below must not see a mix.
 *
 * Enhanced: one Barrier() call per de_gpu_cmd_barriers call, with one texture group and one buffer
 * group (in chunks of 64 when there are more). Textures carry layouts; buffers do not.
 *
 * Legacy: a ResourceBarrier transition names the state a subresource is in, and D3D12 changes
 * some states on its own (buffers, and textures used on the copy queue, decay to COMMON when an
 * ExecuteCommandLists completes), so the fallback tracks states; enhanced barriers need none of
 * this. Buffers keep the state of the current recording only (any other recording means COMMON,
 * because legacy submits run one ExecuteCommandLists per command list, d3d12_sync.c). Textures
 * keep a state per subresource across recordings, so the fallback assumes that command lists
 * using the same texture are recorded in the order they are submitted (the render graph records
 * in pass order); transfers are the exception, as their acquire half starts from COMMON. The
 * state before a transition is always the tracked one; the barrier's `before` only matters for
 * the enhanced path. A barrier that stays in UNORDERED_ACCESS is a UAV barrier.
 *
 * UPLOAD and READBACK buffers never get a barrier: their heaps fix their states (GENERIC_READ,
 * COPY_DEST), and GPU writes reach the CPU when the submission completes.
 *
 * Queue ownership: D3D12 has no ownership transfer. A barrier with src_queue != dst_queue is
 * recorded twice, as on Vulkan: on src_queue it releases the resource to the COMMON layout (or
 * state), which every queue type can use, and on dst_queue it acquires it from COMMON. A transfer
 * recorded on a third queue is an error (logged in Debug and Dev) and records nothing. The fence
 * wait between the two submissions orders them.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

/* ---------- Translation tables (portable) ---------- */

#define WRITE_BITS                                                                                 \
    (DE_ACCESS_RENDER_TARGET | DE_ACCESS_DEPTH_WRITE | DE_ACCESS_SHADER_WRITE |                    \
     DE_ACCESS_COPY_DST | DE_ACCESS_ACCEL_WRITE)
#define ALL_BITS ((DE_ACCESS_PRESENT << 1) - 1u)

/* One de_gpu_access bit. Layout 0 here is a real value (COMMON), so `has_layout` says whether
 * the bit names a texture layout at all (INDIRECT and the ACCEL bits are buffer-only). */
typedef struct bit_info {
    u32 bit;
    u32 sync;
    u32 tex_access, buf_access;
    u32 layout;
    bool has_layout;
    u32 tex_state, buf_state;
} bit_info;

static const bit_info k_bits[] = {
    {DE_ACCESS_SHADER_READ, DE__D3D12_SYNC_ALL_SHADING, DE__D3D12_ACCESS_SHADER_RESOURCE,
     DE__D3D12_ACCESS_VERTEX_BUFFER | DE__D3D12_ACCESS_CONSTANT_BUFFER |
         DE__D3D12_ACCESS_INDEX_BUFFER | DE__D3D12_ACCESS_SHADER_RESOURCE,
     DE__D3D12_LAYOUT_SHADER_RESOURCE, true,
     DE__D3D12_STATE_NON_PIXEL_SHADER_RESOURCE | DE__D3D12_STATE_PIXEL_SHADER_RESOURCE,
     DE__D3D12_STATE_VERTEX_AND_CONSTANT_BUFFER | DE__D3D12_STATE_INDEX_BUFFER |
         DE__D3D12_STATE_NON_PIXEL_SHADER_RESOURCE | DE__D3D12_STATE_PIXEL_SHADER_RESOURCE},
    {DE_ACCESS_SHADER_WRITE, DE__D3D12_SYNC_ALL_SHADING, DE__D3D12_ACCESS_UNORDERED_ACCESS,
     DE__D3D12_ACCESS_UNORDERED_ACCESS, DE__D3D12_LAYOUT_UNORDERED_ACCESS, true,
     DE__D3D12_STATE_UNORDERED_ACCESS, DE__D3D12_STATE_UNORDERED_ACCESS},
    {DE_ACCESS_RENDER_TARGET, DE__D3D12_SYNC_RENDER_TARGET, DE__D3D12_ACCESS_RENDER_TARGET,
     DE__D3D12_ACCESS_RENDER_TARGET, DE__D3D12_LAYOUT_RENDER_TARGET, true,
     DE__D3D12_STATE_RENDER_TARGET, DE__D3D12_STATE_RENDER_TARGET},
    {DE_ACCESS_DEPTH_READ, DE__D3D12_SYNC_DEPTH_STENCIL, DE__D3D12_ACCESS_DEPTH_STENCIL_READ,
     DE__D3D12_ACCESS_DEPTH_STENCIL_READ, DE__D3D12_LAYOUT_DEPTH_STENCIL_READ, true,
     DE__D3D12_STATE_DEPTH_READ, DE__D3D12_STATE_DEPTH_READ},
    {DE_ACCESS_DEPTH_WRITE, DE__D3D12_SYNC_DEPTH_STENCIL, DE__D3D12_ACCESS_DEPTH_STENCIL_WRITE,
     DE__D3D12_ACCESS_DEPTH_STENCIL_WRITE, DE__D3D12_LAYOUT_DEPTH_STENCIL_WRITE, true,
     DE__D3D12_STATE_DEPTH_WRITE, DE__D3D12_STATE_DEPTH_WRITE},
    {DE_ACCESS_INDIRECT, DE__D3D12_SYNC_EXECUTE_INDIRECT, DE__D3D12_ACCESS_INDIRECT_ARGUMENT,
     DE__D3D12_ACCESS_INDIRECT_ARGUMENT, DE__D3D12_LAYOUT_GENERIC_READ, false,
     DE__D3D12_STATE_INDIRECT_ARGUMENT, DE__D3D12_STATE_INDIRECT_ARGUMENT},
    {DE_ACCESS_COPY_SRC, DE__D3D12_SYNC_COPY, DE__D3D12_ACCESS_COPY_SOURCE,
     DE__D3D12_ACCESS_COPY_SOURCE, DE__D3D12_LAYOUT_COPY_SOURCE, true, DE__D3D12_STATE_COPY_SOURCE,
     DE__D3D12_STATE_COPY_SOURCE},
    {DE_ACCESS_COPY_DST, DE__D3D12_SYNC_COPY, DE__D3D12_ACCESS_COPY_DEST,
     DE__D3D12_ACCESS_COPY_DEST, DE__D3D12_LAYOUT_COPY_DEST, true, DE__D3D12_STATE_COPY_DEST,
     DE__D3D12_STATE_COPY_DEST},
    {DE_ACCESS_ACCEL_READ, DE__D3D12_SYNC_ALL_SHADING | DE__D3D12_SYNC_RAYTRACING,
     DE__D3D12_ACCESS_RTAS_READ, DE__D3D12_ACCESS_RTAS_READ, DE__D3D12_LAYOUT_GENERIC_READ, false,
     DE__D3D12_STATE_RTAS, DE__D3D12_STATE_RTAS},
    {DE_ACCESS_ACCEL_WRITE, DE__D3D12_SYNC_BUILD_RTAS, DE__D3D12_ACCESS_RTAS_WRITE,
     DE__D3D12_ACCESS_RTAS_WRITE, DE__D3D12_LAYOUT_GENERIC_READ, false, DE__D3D12_STATE_RTAS,
     DE__D3D12_STATE_RTAS},
};
#define K_BITS_COUNT (sizeof k_bits / sizeof k_bits[0])

/* What each queue type can sync on and access (D3D12 enhanced-barrier queue rules). */
#define COPY_SYNC   (DE__D3D12_SYNC_ALL | DE__D3D12_SYNC_COPY)
#define COPY_ACCESS (DE__D3D12_ACCESS_COPY_SOURCE | DE__D3D12_ACCESS_COPY_DEST)
#define COMPUTE_SYNC_DROPPED                                                                       \
    (DE__D3D12_SYNC_DRAW | DE__D3D12_SYNC_INDEX_INPUT | DE__D3D12_SYNC_VERTEX_SHADING |            \
     DE__D3D12_SYNC_PIXEL_SHADING | DE__D3D12_SYNC_DEPTH_STENCIL | DE__D3D12_SYNC_RENDER_TARGET)
#define COMPUTE_ACCESS_DROPPED                                                                     \
    (DE__D3D12_ACCESS_VERTEX_BUFFER | DE__D3D12_ACCESS_INDEX_BUFFER |                              \
     DE__D3D12_ACCESS_RENDER_TARGET | DE__D3D12_ACCESS_DEPTH_STENCIL_WRITE |                       \
     DE__D3D12_ACCESS_DEPTH_STENCIL_READ)
#define COPY_STATES (DE__D3D12_STATE_COPY_SOURCE | DE__D3D12_STATE_COPY_DEST)
#define COMPUTE_STATES_DROPPED                                                                     \
    (DE__D3D12_STATE_RENDER_TARGET | DE__D3D12_STATE_DEPTH_WRITE | DE__D3D12_STATE_DEPTH_READ |    \
     DE__D3D12_STATE_PIXEL_SHADER_RESOURCE | DE__D3D12_STATE_INDEX_BUFFER)

u32 de__d3d12_access_normalize(u32 access) {
    access &= ALL_BITS;
    if ((access & DE_ACCESS_PRESENT) && access != DE_ACCESS_PRESENT)
        access &= ~(u32)DE_ACCESS_PRESENT;
    if (access & WRITE_BITS) {
        static const u32 k_order[] = {DE_ACCESS_RENDER_TARGET, DE_ACCESS_DEPTH_WRITE,
                                      DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_DST,
                                      DE_ACCESS_ACCEL_WRITE};
        for (u32 i = 0; i < sizeof k_order / sizeof k_order[0]; ++i)
            if (access & k_order[i])
                return k_order[i];
    }
    return access;
}

static u32 popcount32(u32 v) {
    u32 n = 0;
    for (; v; v &= v - 1u)
        ++n;
    return n;
}

de__d3d12_access_info de__d3d12_access_info_of(de_gpu_queue q, u32 access, bool texture,
                                               bool before) {
    de__d3d12_access_info r;
    r.layout = DE__D3D12_LAYOUT_UNDEFINED;
    access = de__d3d12_access_normalize(access);
    if (access == DE_ACCESS_NONE || access == DE_ACCESS_PRESENT) {
        /* Before: unknown or presented earlier, so wait for everything (no access to flush);
         * NONE also discards the contents (UNDEFINED). After: nothing waits. */
        r.sync = before ? DE__D3D12_SYNC_ALL : DE__D3D12_SYNC_NONE;
        r.access = DE__D3D12_ACCESS_NO_ACCESS;
        if (texture && access == DE_ACCESS_PRESENT)
            r.layout = DE__D3D12_LAYOUT_COMMON;
        if (texture && q == DE_QUEUE_COPY)
            r.layout = DE__D3D12_LAYOUT_COMMON;
        return r;
    }
    u32 sync = 0, acc = 0, layout = DE__D3D12_LAYOUT_COMMON;
    bool layout_set = false;
    for (u32 i = 0; i < K_BITS_COUNT; ++i) {
        const bit_info *bi = &k_bits[i];
        if (!(access & bi->bit))
            continue;
        sync |= bi->sync;
        acc |= texture ? bi->tex_access : bi->buf_access;
        if (texture && bi->has_layout && !layout_set) {
            layout = bi->layout;
            layout_set = true;
        }
    }
    if (!texture) {
        /* Index buffers are read by the input assembler, before any shader stage. */
        if (access & DE_ACCESS_SHADER_READ)
            sync |= DE__D3D12_SYNC_INDEX_INPUT;
    } else if (popcount32(access) > 1) {
        /* Only reads are left (normalize): one layout every one of them can use. */
        layout = (access & DE_ACCESS_DEPTH_READ) && q == DE_QUEUE_GRAPHICS
                     ? DE__D3D12_LAYOUT_DIRECT_QUEUE_GENERIC_READ
                     : DE__D3D12_LAYOUT_GENERIC_READ;
    }
    if (q == DE_QUEUE_COPY) {
        sync &= COPY_SYNC;
        acc &= COPY_ACCESS;
        layout = DE__D3D12_LAYOUT_COMMON; /* copy queues use textures only in COMMON */
    } else if (q == DE_QUEUE_COMPUTE) {
        if (sync & DE__D3D12_SYNC_ALL_SHADING)
            sync = (sync & ~DE__D3D12_SYNC_ALL_SHADING) | DE__D3D12_SYNC_COMPUTE_SHADING;
        sync &= ~(u32)COMPUTE_SYNC_DROPPED;
        acc &= ~(u32)COMPUTE_ACCESS_DROPPED;
    }
    if (sync == 0) {
        sync = DE__D3D12_SYNC_NONE;
        acc = DE__D3D12_ACCESS_NO_ACCESS;
    } else if (acc == 0) {
        acc = DE__D3D12_ACCESS_NO_ACCESS;
    }
    r.sync = sync;
    r.access = acc;
    r.layout = texture ? layout : DE__D3D12_LAYOUT_UNDEFINED;
    return r;
}

de__d3d12_transfer de__d3d12_transfer_of(de_gpu_queue cmd_queue, const de_gpu_barrier *b) {
    if (b->src_queue == b->dst_queue)
        return DE__D3D12_TRANSFER_NONE;
    if (cmd_queue == b->src_queue)
        return DE__D3D12_TRANSFER_RELEASE;
    if (cmd_queue == b->dst_queue)
        return DE__D3D12_TRANSFER_ACQUIRE;
    return DE__D3D12_TRANSFER_FOREIGN;
}

void de__d3d12_barrier_range(const de_gpu_barrier *b, const de__d3d12_texture_shape *tex,
                             u32 *mip_first, u32 *mip_count, u32 *layer_first, u32 *layer_count) {
    u32 mf = b->mip_first < tex->mips ? b->mip_first : tex->mips - 1u;
    u32 mc = b->mip_count ? b->mip_count : tex->mips - mf;
    if (mc > tex->mips - mf)
        mc = tex->mips - mf;
    /* A 3D texture's slices are one subresource: its "layers" are always the whole range. */
    u32 lf = tex->layers > 1 && b->layer_first < tex->layers ? b->layer_first : 0u;
    u32 lc = tex->layers > 1 && b->layer_count ? b->layer_count : tex->layers - lf;
    if (lc > tex->layers - lf)
        lc = tex->layers - lf;
    *mip_first = mf;
    *mip_count = mc;
    *layer_first = lf;
    *layer_count = lc;
}

bool de__d3d12_translate_enhanced(de_gpu_queue cmd_queue, const de_gpu_barrier *b,
                                  const de__d3d12_texture_shape *tex, de__d3d12_enhanced *out) {
    memset(out, 0, sizeof *out);
    const bool texture = tex != NULL;
    const de__d3d12_transfer xfer = de__d3d12_transfer_of(cmd_queue, b);
    if (xfer == DE__D3D12_TRANSFER_FOREIGN)
        return false;
    const u32 before = de__d3d12_access_normalize(b->before);
    const u32 after = de__d3d12_access_normalize(b->after);
    if (xfer == DE__D3D12_TRANSFER_NONE && before == DE_ACCESS_NONE && after == DE_ACCESS_NONE)
        return false;

    de__d3d12_access_info src = de__d3d12_access_info_of(cmd_queue, before, texture, true);
    de__d3d12_access_info dst = de__d3d12_access_info_of(cmd_queue, after, texture, false);
    if (texture && after == DE_ACCESS_NONE)
        dst.layout = src.layout; /* no later use: the layout stays */
    if (xfer == DE__D3D12_TRANSFER_RELEASE) {
        dst.sync = DE__D3D12_SYNC_NONE;
        dst.access = DE__D3D12_ACCESS_NO_ACCESS;
        dst.layout = texture ? DE__D3D12_LAYOUT_COMMON : DE__D3D12_LAYOUT_UNDEFINED;
    } else if (xfer == DE__D3D12_TRANSFER_ACQUIRE) {
        src.sync = DE__D3D12_SYNC_NONE;
        src.access = DE__D3D12_ACCESS_NO_ACCESS;
        src.layout = texture ? DE__D3D12_LAYOUT_COMMON : DE__D3D12_LAYOUT_UNDEFINED;
    }
    out->sync_before = src.sync;
    out->access_before = src.access;
    out->sync_after = dst.sync;
    out->access_after = dst.access;
    out->layout_before = src.layout;
    out->layout_after = dst.layout;
    if (texture) {
        de__d3d12_barrier_range(b, tex, &out->mip_first, &out->mip_count, &out->layer_first,
                                &out->layer_count);
        out->plane_count = tex->planes ? tex->planes : 1u;
        /* First use of a render target or depth buffer: its contents (and compression metadata)
         * are initialized by the barrier. A copy queue never sees UNDEFINED. */
        out->discard = tex->rt_ds && src.layout == DE__D3D12_LAYOUT_UNDEFINED &&
                       (dst.layout == DE__D3D12_LAYOUT_RENDER_TARGET ||
                        dst.layout == DE__D3D12_LAYOUT_DEPTH_STENCIL_WRITE);
    }
    return true;
}

u32 de__d3d12_legacy_state_of(de_gpu_queue q, u32 access, bool texture) {
    access = de__d3d12_access_normalize(access);
    u32 state = DE__D3D12_STATE_COMMON;
    for (u32 i = 0; i < K_BITS_COUNT; ++i)
        if (access & k_bits[i].bit)
            state |= texture ? k_bits[i].tex_state : k_bits[i].buf_state;
    if (q == DE_QUEUE_COPY)
        state &= COPY_STATES;
    else if (q == DE_QUEUE_COMPUTE)
        state &= ~(u32)COMPUTE_STATES_DROPPED;
    return state;
}

de__d3d12_legacy_op de__d3d12_translate_legacy(de_gpu_queue cmd_queue, const de_gpu_barrier *b,
                                               bool texture, u32 current, u32 *before, u32 *after) {
    const de__d3d12_transfer xfer = de__d3d12_transfer_of(cmd_queue, b);
    *before = current;
    *after = current;
    if (xfer == DE__D3D12_TRANSFER_FOREIGN)
        return DE__D3D12_LEGACY_NOTHING;
    if (xfer == DE__D3D12_TRANSFER_ACQUIRE)
        *before = DE__D3D12_STATE_COMMON; /* released (or decayed) to COMMON by the other queue */
    const u32 next = de__d3d12_access_normalize(b->after);
    if (xfer == DE__D3D12_TRANSFER_RELEASE)
        *after = DE__D3D12_STATE_COMMON;
    else if (next != DE_ACCESS_NONE)
        *after = de__d3d12_legacy_state_of(cmd_queue, next, texture);
    else
        *after = *before;
    if (*before != *after)
        return DE__D3D12_LEGACY_TRANSITION;
    return *after == DE__D3D12_STATE_UNORDERED_ACCESS ? DE__D3D12_LEGACY_UAV
                                                      : DE__D3D12_LEGACY_NOTHING;
}

u32 de__d3d12_subresource(u32 mip, u32 layer, u32 plane, u32 mips, u32 layers) {
    return mip + layer * mips + plane * mips * layers;
}

/* ---------- Recording (Windows) ---------- */
#if defined(_WIN32)

#include <dengine/de_debug.h>

#include <stdlib.h>

/* The plain-number copies above are the d3d12.h values. */
#define SAME(a, b) _Static_assert((u32)(a) == (u32)(b), #a " differs from d3d12.h")
SAME(DE__D3D12_SYNC_NONE, D3D12_BARRIER_SYNC_NONE);
SAME(DE__D3D12_SYNC_ALL, D3D12_BARRIER_SYNC_ALL);
SAME(DE__D3D12_SYNC_DRAW, D3D12_BARRIER_SYNC_DRAW);
SAME(DE__D3D12_SYNC_INDEX_INPUT, D3D12_BARRIER_SYNC_INDEX_INPUT);
SAME(DE__D3D12_SYNC_VERTEX_SHADING, D3D12_BARRIER_SYNC_VERTEX_SHADING);
SAME(DE__D3D12_SYNC_PIXEL_SHADING, D3D12_BARRIER_SYNC_PIXEL_SHADING);
SAME(DE__D3D12_SYNC_DEPTH_STENCIL, D3D12_BARRIER_SYNC_DEPTH_STENCIL);
SAME(DE__D3D12_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET);
SAME(DE__D3D12_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING);
SAME(DE__D3D12_SYNC_RAYTRACING, D3D12_BARRIER_SYNC_RAYTRACING);
SAME(DE__D3D12_SYNC_COPY, D3D12_BARRIER_SYNC_COPY);
SAME(DE__D3D12_SYNC_EXECUTE_INDIRECT, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);
SAME(DE__D3D12_SYNC_ALL_SHADING, D3D12_BARRIER_SYNC_ALL_SHADING);
SAME(DE__D3D12_SYNC_NON_PIXEL_SHADING, D3D12_BARRIER_SYNC_NON_PIXEL_SHADING);
SAME(DE__D3D12_SYNC_BUILD_RTAS, D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE);
SAME(DE__D3D12_ACCESS_VERTEX_BUFFER, D3D12_BARRIER_ACCESS_VERTEX_BUFFER);
SAME(DE__D3D12_ACCESS_CONSTANT_BUFFER, D3D12_BARRIER_ACCESS_CONSTANT_BUFFER);
SAME(DE__D3D12_ACCESS_INDEX_BUFFER, D3D12_BARRIER_ACCESS_INDEX_BUFFER);
SAME(DE__D3D12_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET);
SAME(DE__D3D12_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
SAME(DE__D3D12_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
SAME(DE__D3D12_ACCESS_DEPTH_STENCIL_READ, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
SAME(DE__D3D12_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
SAME(DE__D3D12_ACCESS_INDIRECT_ARGUMENT, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
SAME(DE__D3D12_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_DEST);
SAME(DE__D3D12_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_COPY_SOURCE);
SAME(DE__D3D12_ACCESS_RTAS_READ, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
SAME(DE__D3D12_ACCESS_RTAS_WRITE, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE);
SAME(DE__D3D12_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_NO_ACCESS);
SAME(DE__D3D12_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_UNDEFINED);
SAME(DE__D3D12_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_COMMON);
SAME(DE__D3D12_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_PRESENT);
SAME(DE__D3D12_LAYOUT_GENERIC_READ, D3D12_BARRIER_LAYOUT_GENERIC_READ);
SAME(DE__D3D12_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
SAME(DE__D3D12_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
SAME(DE__D3D12_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
SAME(DE__D3D12_LAYOUT_DEPTH_STENCIL_READ, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ);
SAME(DE__D3D12_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
SAME(DE__D3D12_LAYOUT_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE);
SAME(DE__D3D12_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST);
SAME(DE__D3D12_LAYOUT_DIRECT_QUEUE_GENERIC_READ, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ);
SAME(DE__D3D12_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
SAME(DE__D3D12_STATE_COMMON, D3D12_RESOURCE_STATE_PRESENT);
SAME(DE__D3D12_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
SAME(DE__D3D12_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_INDEX_BUFFER);
SAME(DE__D3D12_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
SAME(DE__D3D12_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
SAME(DE__D3D12_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
SAME(DE__D3D12_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_READ);
SAME(DE__D3D12_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
SAME(DE__D3D12_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
SAME(DE__D3D12_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
SAME(DE__D3D12_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
SAME(DE__D3D12_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
SAME(DE__D3D12_STATE_RTAS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
#undef SAME

/* ---------- Mode ---------- */

static bool env_force_legacy(void) {
    char buf[8];
    size_t n = 0;
    if (getenv_s(&n, buf, sizeof buf, "DE_D3D12_FORCE_LEGACY_BARRIERS") != 0 || n == 0)
        return false;
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y';
}

bool de__d3d12_use_enhanced_barriers(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    u32 mode = de_atomic_load_u32(&dd->barrier_mode, DE_ATOMIC_ACQUIRE);
    if (mode != DE__D3D12_BARRIERS_UNSET)
        return mode == DE__D3D12_BARRIERS_ENHANCED;
    de_cvar_desc cd = {.name = "gpu.d3d12.force_legacy_barriers",
                       .help = "D3D12: record legacy ResourceBarrier transitions even where "
                               "enhanced barriers exist (default from the environment variable "
                               "DE_D3D12_FORCE_LEGACY_BARRIERS); read by a device's first command "
                               "list",
                       .type = DE_CVAR_BOOL,
                       .default_number = env_force_legacy() ? 1.0 : 0.0};
    const bool force = de_cvar_bool(de_cvar_register(&cd));
    const bool enhanced = d->caps.enhanced_barriers && !force;
    mode = enhanced ? DE__D3D12_BARRIERS_ENHANCED : DE__D3D12_BARRIERS_LEGACY;
    u32 expected = DE__D3D12_BARRIERS_UNSET;
    if (de_atomic_cas_u32(&dd->barrier_mode, &expected, mode, DE_ATOMIC_ACQ_REL,
                          DE_ATOMIC_ACQUIRE)) {
        DE_INFO(d->log, "d3d12: %s barriers%s", enhanced ? "enhanced" : "legacy",
                force ? " (gpu.d3d12.force_legacy_barriers)"
                      : (enhanced ? "" : " (the device has no enhanced barriers)"));
        return enhanced;
    }
    return expected == DE__D3D12_BARRIERS_ENHANCED; /* another thread chose first */
}

de__d3d12_texture_shape de__d3d12_texture_shape_of(const de__gpu_texture *t) {
    de__d3d12_texture_shape s;
    s.mips = t->mip_count ? t->mip_count : 1u;
    s.layers = t->dim == DE_TEX_3D ? 1u : (t->depth_or_layers ? t->depth_or_layers : 1u);
    s.planes = t->format == DE_FMT_D32_FLOAT_S8 ? 2u : 1u;
    s.rt_ds = (t->usage & (DE_USAGE_RENDER_TARGET | DE_USAGE_DEPTH)) != 0;
    return s;
}

/* Barriers recorded per Barrier() / ResourceBarrier() call; more are flushed in chunks. */
#define CHUNK 64u

/* ---------- Enhanced ---------- */

typedef struct enhanced_batch {
    D3D12_TEXTURE_BARRIER tex[CHUNK];
    D3D12_BUFFER_BARRIER buf[CHUNK];
    u32 ntex, nbuf;
} enhanced_batch;

static void flush_enhanced(d3d12_cmd *dc, enhanced_batch *e) {
    D3D12_BARRIER_GROUP g[2];
    u32 n = 0;
    if (e->ntex) {
        g[n].Type = D3D12_BARRIER_TYPE_TEXTURE;
        g[n].NumBarriers = e->ntex;
        g[n].pTextureBarriers = e->tex;
        ++n;
    }
    if (e->nbuf) {
        g[n].Type = D3D12_BARRIER_TYPE_BUFFER;
        g[n].NumBarriers = e->nbuf;
        g[n].pBufferBarriers = e->buf;
        ++n;
    }
    if (n)
        ID3D12GraphicsCommandList7_Barrier(dc->list7, n, g);
    e->ntex = e->nbuf = 0;
}

/* ---------- Legacy state tracking ---------- */

static u32 *legacy_states(de_gpu_device *d, de__gpu_texture *t, const de__d3d12_texture_shape *s) {
    d3d12_texture *dt = DE__GPU_PAYLOAD(d3d12_texture, t);
    const u32 n = s->mips * s->layers;
    if (n <= DE_GPU_MAX_MIPS)
        return dt->legacy_inline;
    if (!dt->legacy_sub) {
        /* Once per array texture, on its first legacy barrier; zero = COMMON, the initial state
         * (d3d12_resources.c creates textures in COMMON). */
        dt->legacy_sub =
            (u32 *)DE_ALLOC(d->alloc, (size_t)n * sizeof(u32), DE_ALIGNOF(u32), DE_MEM_RENDER);
        if (dt->legacy_sub)
            memset(dt->legacy_sub, 0, (size_t)n * sizeof(u32));
    }
    return dt->legacy_sub;
}

void de__d3d12_texture_legacy_release(de_gpu_device *d, de__gpu_texture *t) {
    d3d12_texture *dt = DE__GPU_PAYLOAD(d3d12_texture, t);
    if (!dt->legacy_sub)
        return;
    const de__d3d12_texture_shape s = de__d3d12_texture_shape_of(t);
    DE_FREE(d->alloc, dt->legacy_sub, (size_t)s.mips * s.layers * sizeof(u32));
    dt->legacy_sub = NULL;
}

typedef struct legacy_batch {
    D3D12_RESOURCE_BARRIER rb[CHUNK];
    u32 n;
} legacy_batch;

static void flush_legacy(d3d12_cmd *dc, legacy_batch *l) {
    if (l->n)
        ID3D12GraphicsCommandList_ResourceBarrier(dc->list, l->n, l->rb);
    l->n = 0;
}

static void push_transition(d3d12_cmd *dc, legacy_batch *l, ID3D12Resource *res, UINT sub,
                            u32 before, u32 after) {
    if (l->n == CHUNK)
        flush_legacy(dc, l);
    D3D12_RESOURCE_BARRIER *r = &l->rb[l->n++];
    memset(r, 0, sizeof *r);
    r->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    r->Transition.pResource = res;
    r->Transition.Subresource = sub;
    r->Transition.StateBefore = (D3D12_RESOURCE_STATES)before;
    r->Transition.StateAfter = (D3D12_RESOURCE_STATES)after;
}

static void push_uav(d3d12_cmd *dc, legacy_batch *l, ID3D12Resource *res) {
    if (l->n == CHUNK)
        flush_legacy(dc, l);
    D3D12_RESOURCE_BARRIER *r = &l->rb[l->n++];
    memset(r, 0, sizeof *r);
    r->Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    r->UAV.pResource = res;
}

/* Legacy DiscardResource after a first-use transition of a render target or depth buffer: the
 * transition has put the subresources in RENDER_TARGET / DEPTH_WRITE, which the call needs. */
typedef struct legacy_discard {
    ID3D12Resource *res;
    u32 mip_first, mip_count, layer_first, layer_count;
    bool whole;
} legacy_discard;

static void legacy_texture(de_gpu_cmd *c, d3d12_cmd *dc, legacy_batch *l, const de_gpu_barrier *b,
                           de__gpu_texture *t, legacy_discard *discard) {
    de_gpu_device *d = c->device;
    d3d12_texture *dt = DE__GPU_PAYLOAD(d3d12_texture, t);
    const de__d3d12_texture_shape s = de__d3d12_texture_shape_of(t);
    u32 *states = legacy_states(d, t, &s);
    if (!states) {
        de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY,
                     "de_gpu_cmd_barriers(\"%s\"): no memory for the legacy states of \"%s\"; "
                     "barrier skipped",
                     c->name, t->name);
        return;
    }
    const u32 total = s.mips * s.layers;
    /* Used on the copy queue by another recording: decayed to COMMON since. */
    if (dt->legacy_copy_serial && dt->legacy_copy_serial != dc->serial) {
        memset(states, 0, (size_t)total * sizeof(u32));
        dt->legacy_copy_serial = 0;
    }
    if (c->queue == DE_QUEUE_COPY)
        dt->legacy_copy_serial = dc->serial;

    u32 mf, mc, lf, lc;
    de__d3d12_barrier_range(b, &s, &mf, &mc, &lf, &lc);
    const bool whole = mc == s.mips && lc == s.layers;
    /* One transition for the whole resource when every subresource is in the same state. */
    bool uniform = whole;
    for (u32 i = 1; uniform && i < total; ++i)
        uniform = states[i] == states[0];
    bool discarded = false;
    if (uniform) {
        u32 before, after;
        de__d3d12_legacy_op op =
            de__d3d12_translate_legacy(c->queue, b, true, states[0], &before, &after);
        if (op == DE__D3D12_LEGACY_TRANSITION)
            push_transition(dc, l, dt->res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after);
        else if (op == DE__D3D12_LEGACY_UAV)
            push_uav(dc, l, dt->res);
        for (u32 i = 0; i < total; ++i)
            states[i] = after;
        discarded = op == DE__D3D12_LEGACY_TRANSITION;
    } else {
        bool uav = false;
        for (u32 layer = lf; layer < lf + lc; ++layer) {
            for (u32 mip = mf; mip < mf + mc; ++mip) {
                u32 *st = &states[mip + layer * s.mips];
                u32 before, after;
                de__d3d12_legacy_op op =
                    de__d3d12_translate_legacy(c->queue, b, true, *st, &before, &after);
                if (op == DE__D3D12_LEGACY_TRANSITION) {
                    for (u32 p = 0; p < s.planes; ++p)
                        push_transition(dc, l, dt->res,
                                        de__d3d12_subresource(mip, layer, p, s.mips, s.layers),
                                        before, after);
                    discarded = true;
                } else if (op == DE__D3D12_LEGACY_UAV) {
                    uav = true;
                }
                *st = after;
            }
        }
        if (uav)
            push_uav(dc, l, dt->res);
    }
    /* NONE before = contents discarded: initialize a render target or depth buffer. */
    const u32 next = de__d3d12_legacy_state_of(c->queue, b->after, true);
    if (discarded && s.rt_ds && de__d3d12_access_normalize(b->before) == DE_ACCESS_NONE &&
        de__d3d12_transfer_of(c->queue, b) == DE__D3D12_TRANSFER_NONE &&
        (next == DE__D3D12_STATE_RENDER_TARGET || next == DE__D3D12_STATE_DEPTH_WRITE)) {
        discard->res = dt->res;
        discard->mip_first = mf;
        discard->mip_count = mc;
        discard->layer_first = lf;
        discard->layer_count = lc;
        discard->whole = whole;
    }
}

static void legacy_buffer(de_gpu_cmd *c, d3d12_cmd *dc, legacy_batch *l, const de_gpu_barrier *b,
                          de__gpu_buffer *buf) {
    d3d12_buffer *db = DE__GPU_PAYLOAD(d3d12_buffer, buf);
    /* Buffers decay to COMMON when an ExecuteCommandLists completes, and legacy submits run one
     * per command list: a state from another recording is COMMON now. */
    const u32 current = db->legacy_serial == dc->serial ? db->legacy_state : DE__D3D12_STATE_COMMON;
    u32 before, after;
    de__d3d12_legacy_op op =
        de__d3d12_translate_legacy(c->queue, b, false, current, &before, &after);
    if (op == DE__D3D12_LEGACY_TRANSITION)
        push_transition(dc, l, db->res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after);
    else if (op == DE__D3D12_LEGACY_UAV)
        push_uav(dc, l, db->res);
    db->legacy_state = after;
    db->legacy_serial = dc->serial;
}

static void legacy_discards(d3d12_cmd *dc, const legacy_discard *ds, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        const legacy_discard *x = &ds[i];
        if (x->whole) {
            ID3D12GraphicsCommandList_DiscardResource(dc->list, x->res, NULL);
            continue;
        }
        /* Mip runs of each layer are contiguous subresources. */
        D3D12_RESOURCE_DESC rd;
        ID3D12Resource_GetDesc(x->res, &rd);
        const u32 mips = rd.MipLevels ? rd.MipLevels : 1u;
        const u32 layers =
            rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : rd.DepthOrArraySize;
        for (u32 layer = x->layer_first; layer < x->layer_first + x->layer_count; ++layer) {
            D3D12_DISCARD_REGION r;
            memset(&r, 0, sizeof r);
            r.FirstSubresource = de__d3d12_subresource(x->mip_first, layer, 0, mips, layers);
            r.NumSubresources = x->mip_count;
            ID3D12GraphicsCommandList_DiscardResource(dc->list, x->res, &r);
        }
    }
}

/* ---------- de_gpu_cmd_barriers ---------- */

static bool host_visible(const de__gpu_buffer *b) {
    return b->memory == DE_MEM_UPLOAD || b->memory == DE_MEM_READBACK;
}

/* Reports a transfer recorded on a third queue (Debug and Dev) and says whether to skip. */
static bool foreign(de_gpu_cmd *c, const de_gpu_barrier *b, u32 i) {
    if (de__d3d12_transfer_of(c->queue, b) != DE__D3D12_TRANSFER_FOREIGN)
        return false;
#if DE__GPU_VALIDATE_COMMANDS
    de__gpu_fail(c->device, DE_ERR_INVALID_ARGUMENT,
                 "de_gpu_cmd_barriers(\"%s\"): barrier %u transfers from queue %d to queue %d but "
                 "is recorded on queue %d; record it on both of them",
                 c->name, i, (int)b->src_queue, (int)b->dst_queue, (int)c->queue);
#else
    (void)i;
#endif
    return true;
}

static void record_enhanced(de_gpu_cmd *c, d3d12_cmd *dc, const de_gpu_barrier *b, u32 count) {
    de_gpu_device *d = c->device;
    enhanced_batch e;
    e.ntex = e.nbuf = 0;
    for (u32 i = 0; i < count; ++i) {
        const de_gpu_barrier *x = &b[i];
        if (foreign(c, x, i))
            continue;
        de__d3d12_enhanced t;
        if (x->texture.id) {
            de__gpu_texture *rec = de__gpu_get_texture(d, x->texture);
            if (!rec)
                continue;
            const de__d3d12_texture_shape s = de__d3d12_texture_shape_of(rec);
            if (!de__d3d12_translate_enhanced(c->queue, x, &s, &t))
                continue;
            if (e.ntex == CHUNK)
                flush_enhanced(dc, &e);
            D3D12_TEXTURE_BARRIER *tb = &e.tex[e.ntex++];
            memset(tb, 0, sizeof *tb);
            tb->SyncBefore = (D3D12_BARRIER_SYNC)t.sync_before;
            tb->SyncAfter = (D3D12_BARRIER_SYNC)t.sync_after;
            tb->AccessBefore = (D3D12_BARRIER_ACCESS)t.access_before;
            tb->AccessAfter = (D3D12_BARRIER_ACCESS)t.access_after;
            tb->LayoutBefore = (D3D12_BARRIER_LAYOUT)t.layout_before;
            tb->LayoutAfter = (D3D12_BARRIER_LAYOUT)t.layout_after;
            tb->pResource = DE__GPU_PAYLOAD(d3d12_texture, rec)->res;
            tb->Subresources.IndexOrFirstMipLevel = t.mip_first;
            tb->Subresources.NumMipLevels = t.mip_count;
            tb->Subresources.FirstArraySlice = t.layer_first;
            tb->Subresources.NumArraySlices = t.layer_count;
            tb->Subresources.FirstPlane = 0;
            tb->Subresources.NumPlanes = t.plane_count;
            tb->Flags =
                t.discard ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE;
        } else if (x->buffer.id) {
            de__gpu_buffer *rec = de__gpu_get_buffer(d, x->buffer);
            if (!rec || host_visible(rec))
                continue;
            if (!de__d3d12_translate_enhanced(c->queue, x, NULL, &t))
                continue;
            if (e.nbuf == CHUNK)
                flush_enhanced(dc, &e);
            D3D12_BUFFER_BARRIER *bb = &e.buf[e.nbuf++];
            memset(bb, 0, sizeof *bb);
            bb->SyncBefore = (D3D12_BARRIER_SYNC)t.sync_before;
            bb->SyncAfter = (D3D12_BARRIER_SYNC)t.sync_after;
            bb->AccessBefore = (D3D12_BARRIER_ACCESS)t.access_before;
            bb->AccessAfter = (D3D12_BARRIER_ACCESS)t.access_after;
            bb->pResource = DE__GPU_PAYLOAD(d3d12_buffer, rec)->res;
            bb->Offset = 0;
            bb->Size = UINT64_MAX; /* the whole buffer */
        }
        /* Acceleration structures: none exist before S16 (accel_create is refused). */
    }
    flush_enhanced(dc, &e);
}

static void record_legacy(de_gpu_cmd *c, d3d12_cmd *dc, const de_gpu_barrier *b, u32 count) {
    de_gpu_device *d = c->device;
    legacy_batch l;
    l.n = 0;
    legacy_discard discards[8];
    u32 ndiscard = 0;
    for (u32 i = 0; i < count; ++i) {
        const de_gpu_barrier *x = &b[i];
        if (foreign(c, x, i))
            continue;
        if (x->texture.id) {
            de__gpu_texture *rec = de__gpu_get_texture(d, x->texture);
            if (!rec)
                continue;
            legacy_discard dis;
            dis.res = NULL;
            legacy_texture(c, dc, &l, x, rec, &dis);
            if (dis.res) {
                if (ndiscard == sizeof discards / sizeof discards[0]) {
                    flush_legacy(dc, &l);
                    legacy_discards(dc, discards, ndiscard);
                    ndiscard = 0;
                }
                discards[ndiscard++] = dis;
            }
        } else if (x->buffer.id) {
            de__gpu_buffer *rec = de__gpu_get_buffer(d, x->buffer);
            if (rec && !host_visible(rec))
                legacy_buffer(c, dc, &l, x, rec);
        }
    }
    flush_legacy(dc, &l);
    legacy_discards(dc, discards, ndiscard);
}

void de__d3d12_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (!dc->open)
        return;
    if (dc->enhanced)
        record_enhanced(c, dc, b, count);
    else
        record_legacy(c, dc, b, count);
}

bool de__d3d12_enhanced_barriers_active(de_gpu_device *d) {
    if (!d || d->vt != &de__gpu_d3d12_vtable)
        return false;
    return de_atomic_load_u32(&de__d3d12(d)->barrier_mode, DE_ATOMIC_ACQUIRE) ==
           DE__D3D12_BARRIERS_ENHANCED;
}

#endif /* _WIN32 */
