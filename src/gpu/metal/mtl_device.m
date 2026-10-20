/* mtl_device.m — the Metal 3 backend's device, queues and vtable (DENG-287, SPEC §12.1).
 *
 * Device: `adapter` 0 is MTLCreateSystemDefaultDevice(), n is MTLCopyAllDevices()[n - 1]. Three
 * MTLCommandQueues serve DE_QUEUE_GRAPHICS, _COMPUTE and _COPY; Apple GPUs run them concurrently.
 *
 * Queue timelines: the front-end's internal value per queue is an MTLSharedEvent the queue
 * signals at the end of each submission (queue_submit / queue_completed / wait_idle). Command
 * lists, barriers, submission, user fences, the swapchain and residency are DENG-288's
 * (mtl_cmd.m, mtl_barriers.m, mtl_sync.m, mtl_swapchain.m, mtl_residency.m).
 *
 * API validation is the Metal debug layer, switched on by the environment (MTL_DEBUG_LAYER=1)
 * before the first device of the process; desc->debug_layer cannot turn it on later.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"
#include "gpu/metal/mtl_accel_internal.h"

#include <stdlib.h>
#include <string.h>

static const char *const k_queue_names[DE_QUEUE_COUNT_] = {"graphics", "compute", "copy"};

static void release(void **p) {
    if (*p)
        (void)CFBridgingRelease(*p);
    *p = NULL;
}

/* Releases everything device_create made; safe on a partly created device. */
static void device_release(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    @autoreleasepool {
        de__mtl_sync_drain(d);
        de__mtl_accel_shutdown(d);
        de__mtl_pipeline_shutdown(d); /* DENG-325 */
        de__mtl_archive_close(d);
        de__mtl_residency_shutdown(d);
        de__mtl_sync_shutdown(d);
        de__mtl_bindless_shutdown(d);
        de__mtl_errors_shutdown(d); /* DENG-328 */
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
            release(&m->timeline[q]);
            release(&m->queues[q]);
        }
        release(&m->device);
    }
    DE_ASSERT(m->heap_count == 0);
    if (m->heaps)
        DE_FREE(d->alloc, m->heaps, sizeof(mtl_heap *) * m->heap_cap);
    m->heaps = NULL;
    m->heap_count = m->heap_cap = 0;
    de_mutex_destroy(&m->heap_lock);
    de_mutex_destroy(&m->sampler_lock);
    de_mutex_destroy(&m->size_lock);
    de_mutex_destroy(&m->library_lock);
    de_mutex_destroy(&m->archive_lock);
}

static de_result mtl_device_create(de_gpu_device *d, const de_gpu_device_desc *desc,
                                   de_gpu_caps *caps) {
    mtl_device *m = de__mtl_dev(d);
    de_mutex_init(&m->heap_lock);
    de_mutex_init(&m->sampler_lock);
    de_mutex_init(&m->size_lock);
    de_mutex_init(&m->library_lock);
    de_mutex_init(&m->archive_lock);
    de_result r = DE_OK;
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        if (desc->adapter == 0) {
            dev = MTLCreateSystemDefaultDevice();
            if (dev == nil)
                r = DE_FAIL(DE_ERR_UNSUPPORTED, "metal: this machine has no Metal device");
        } else {
            NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
            if (desc->adapter <= all.count)
                dev = all[desc->adapter - 1];
            else
                r = DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                            "metal: adapter %u asked for, but this machine has %u Metal device%s",
                            desc->adapter, (u32)all.count, all.count == 1 ? "" : "s");
        }
        if (r == DE_OK) {
            m->device = (void *)CFBridgingRetain(dev);
            m->unified_memory = dev.hasUnifiedMemory;
            m->border_color = [dev supportsFamily:MTLGPUFamilyApple7];
            for (u32 q = 0; q < DE_QUEUE_COUNT_ && r == DE_OK; ++q) {
                id<MTLCommandQueue> queue = [dev newCommandQueue];
                id<MTLSharedEvent> ev = [dev newSharedEvent];
                if (queue == nil || ev == nil) {
                    r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "metal: %s queue", k_queue_names[q]);
                    break;
                }
                queue.label = [NSString stringWithFormat:@"de %s queue", k_queue_names[q]];
                ev.label = [NSString stringWithFormat:@"de %s timeline", k_queue_names[q]];
                ev.signaledValue = 0;
                m->queues[q] = (void *)CFBridgingRetain(queue);
                m->timeline[q] = (void *)CFBridgingRetain(ev);
            }
        }
        if (r == DE_OK) {
            de__mtl_caps_fill(m->device, caps);
            r = de__mtl_memory_init(d);
        }
        if (r == DE_OK)
            r = de__mtl_bindless_init(d);
        if (r == DE_OK)
            r = de__mtl_sync_init(d);
        if (r == DE_OK)
            r = de__mtl_residency_init(d);
        if (r == DE_OK)
            de__mtl_archive_open(d, desc); /* DENG-325: never fails */
        if (r == DE_OK)
            r = de__mtl_errors_init(d, desc); /* DENG-328 */
    }
    if (r != DE_OK) {
        device_release(d);
        return r;
    }
    if (!caps->base_tier_ok)
        DE_WARN(d->log, "metal: \"%s\" is below the Base tier: %s", caps->device_name,
                caps->base_tier_reason);
    if ((desc->debug_layer || desc->gpu_validation) && !getenv("MTL_DEBUG_LAYER"))
        DE_INFO(d->log, "metal: API validation is switched on by the environment: set "
                        "MTL_DEBUG_LAYER=1 (and MTL_SHADER_VALIDATION=1) before the process "
                        "starts");
    return DE_OK;
}

static void mtl_device_destroy(de_gpu_device *d) {
    de__mtl_archive_save(d); /* DENG-325 */
    device_release(d);
}

/* ---------- Queue timelines ---------- */

static u64 mtl_queue_completed(de_gpu_device *d, de_gpu_queue q) {
    return ((__bridge id<MTLSharedEvent>)de__mtl_dev(d)->timeline[q]).signaledValue;
}

static void mtl_wait_idle(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        const u64 target = de_atomic_load_u64(&d->submitted[q], DE_ATOMIC_ACQUIRE);
        if (mtl_queue_completed(d, (de_gpu_queue)q) >= target)
            continue;
        @autoreleasepool {
            /* A queue runs its command buffers in commit order: once an empty one committed now
             * is done, so is every signal before it. */
            id<MTLCommandBuffer> cb =
                [(__bridge id<MTLCommandQueue>)m->queues[q] commandBufferWithUnretainedReferences];
            [cb commit];
            [cb waitUntilCompleted];
        }
        /* The event update can trail the command buffer's completion; poll briefly. */
        for (u32 i = 0; i < 100000 && mtl_queue_completed(d, (de_gpu_queue)q) < target; ++i)
            de_sleep_ns(10000);
        if (mtl_queue_completed(d, (de_gpu_queue)q) < target)
            DE_WARN(d->log, "metal: wait_idle: the %s queue is at %llu, not %llu", k_queue_names[q],
                    (unsigned long long)mtl_queue_completed(d, (de_gpu_queue)q),
                    (unsigned long long)target);
    }
}

/* ---------- The vtable ---------- */

/* ---------- Adapter list (DENG-410) ---------- */

/* MTLCopyAllDevices() in its order, as device_create numbers `adapter` n. The values are the ones
 * de__mtl_caps_fill reports for the same device. */
static de_result mtl_enumerate_adapters(de_gpu_adapter_info *out, u32 cap, u32 *count) {
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        const u32 total = (u32)all.count;
        for (u32 i = 0; i < total && i < cap; ++i) {
            id<MTLDevice> dev = all[i];
            de_gpu_caps c;
            de__mtl_caps_fill((__bridge void *)dev, &c);
            de_gpu_adapter_info *o = &out[i];
            memcpy(o->name, c.device_name, sizeof o->name);
            o->name[sizeof o->name - 1] = '\0';
            o->vendor_id = c.vendor_id;
            o->device_id = c.device_id;
            o->dedicated_vram = c.dedicated_vram;
            o->shared_memory = c.shared_memory;
            /* Apple silicon has unified memory; an Intel Mac's integrated GPU is the low-power
             * one; anything else (built-in or eGPU) has its own memory. */
            o->type = dev.hasUnifiedMemory || dev.lowPower ? DE_GPU_ADAPTER_INTEGRATED
                                                           : DE_GPU_ADAPTER_DISCRETE;
        }
        *count = total;
    }
    return DE_OK;
}

const de__gpu_backend_vtable de__gpu_metal_vtable = {
    .name = "metal",
    .device_payload_size = sizeof(mtl_device),
    .cmd_payload_size = sizeof(mtl_cmd),
    .payload_size =
        {
            [DE__GPU_BUFFER] = sizeof(mtl_buffer),
            [DE__GPU_TEXTURE] = sizeof(mtl_texture),
            [DE__GPU_SAMPLER] = sizeof(mtl_sampler),
            [DE__GPU_SWAPCHAIN] = sizeof(mtl_swapchain),
            [DE__GPU_FENCE] = sizeof(mtl_fence),
            [DE__GPU_PIPELINE] = sizeof(mtl_pipeline),
            [DE__GPU_ACCEL] = sizeof(mtl_accel),
            [DE__GPU_QUERY_HEAP] = sizeof(mtl_query_heap), /* DENG-326 */
        },
    .device_create = mtl_device_create,
    .device_destroy = mtl_device_destroy,
    .wait_idle = mtl_wait_idle,
    .buffer_create = de__mtl_buffer_create,
    .buffer_destroy = de__mtl_buffer_destroy,
    .texture_create = de__mtl_texture_create,
    .texture_destroy = de__mtl_texture_destroy,
    .texture_retire = de__mtl_texture_retire,
    .sampler_create = de__mtl_sampler_create,
    .sampler_destroy = de__mtl_sampler_destroy,
    .bindless_write_buffer = de__mtl_bindless_write_buffer,
    .bindless_write_texture = de__mtl_bindless_write_texture,
    .bindless_write_sampler = de__mtl_bindless_write_sampler,
    .pipeline_create = de__mtl_pipeline_create,
    .pipeline_destroy = de__mtl_pipeline_destroy,
    .cmd_init = de__mtl_cmd_init,
    .cmd_fini = de__mtl_draw_cmd_fini,
    .cmd_begin = de__mtl_draw_cmd_begin,
    .cmd_barriers = de__mtl_cmd_barriers,
    .cmd_begin_render = de__mtl_draw_begin_render,
    .cmd_end_render = de__mtl_cmd_end_render,
    .cmd_set_pipeline = de__mtl_cmd_set_pipeline,
    .cmd_push_constants = de__mtl_cmd_push_constants,
    .cmd_viewport = de__mtl_draw_viewport,
    .cmd_draw_indexed_indirect = de__mtl_cmd_draw_indexed_indirect,
    .cmd_dispatch_mesh_indirect = de__mtl_cmd_dispatch_mesh_indirect,
    .cmd_dispatch_rays = de__mtl_cmd_dispatch_rays,
    .cmd_dispatch = de__mtl_cmd_dispatch,
    .cmd_dispatch_indirect = de__mtl_cmd_dispatch_indirect,
    .cmd_copy_buffer = de__mtl_cmd_copy_buffer,
    .cmd_copy_buffer_to_texture = de__mtl_cmd_copy_buffer_to_texture,
    .queue_submit = de__mtl_queue_submit,
    .queue_completed = mtl_queue_completed,
    .fence_create = de__mtl_fence_create,
    .fence_destroy = de__mtl_fence_destroy,
    .fence_value = de__mtl_fence_value,
    .fence_wait = de__mtl_fence_wait,
    .swapchain_create = de__mtl_swapchain_create,
    .swapchain_destroy = de__mtl_swapchain_destroy,
    .swapchain_acquire = de__mtl_swapchain_acquire,
    .swapchain_present = de__mtl_swapchain_present,
    .swapchain_resize = de__mtl_swapchain_resize,
    .swapchain_colorspace = de__mtl_swapchain_colorspace,
    .heap_payload_size = sizeof(mtl_heap),
    .heap_create = de__mtl_heap_create,
    .heap_destroy = de__mtl_heap_destroy,
    .buffer_requirements = de__mtl_buffer_requirements,
    .texture_requirements = de__mtl_texture_requirements,
    .buffer_create_placed = de__mtl_buffer_create_placed,
    .texture_create_placed = de__mtl_texture_create_placed,
    .cmd_copy_texture_to_buffer = de__mtl_cmd_copy_texture_to_buffer,
    /* DENG-325 */
    .cmd_draw = de__mtl_cmd_draw,
    .cmd_draw_indexed = de__mtl_cmd_draw_indexed,
    .cmd_set_index_buffer = de__mtl_cmd_set_index_buffer,
    .cmd_scissor = de__mtl_cmd_scissor,
    .cmd_dispatch_mesh = de__mtl_cmd_dispatch_mesh,
    /* DENG-326: timestamps, debug groups, labels (mtl_query.m, mtl_debug.m) */
    .query_heap_create = de__mtl_query_heap_create,
    .query_heap_destroy = de__mtl_query_heap_destroy,
    .cmd_timestamp = de__mtl_cmd_timestamp,
    .cmd_resolve_queries = de__mtl_cmd_resolve_queries,
    .cmd_marker_push = de__mtl_cmd_marker_push,
    .cmd_marker_pop = de__mtl_cmd_marker_pop,
    .object_name = de__mtl_object_name,
    .queue_timestamp_now = de__mtl_queue_timestamp_now,
    /* DENG-328: command-buffer errors, breadcrumbs (mtl_errors.m) */
    .cmd_breadcrumb = de__mtl_cmd_breadcrumb,
    .device_lost = de__mtl_device_lost,
    .device_lost_report = de__mtl_device_lost_report,
    .enumerate_adapters = mtl_enumerate_adapters,
    .accel_query_sizes = de__mtl_accel_sizes,
    .accel_create = de__mtl_accel_create,
    .accel_destroy = de__mtl_accel_destroy,
    .accel_address = de__mtl_accel_address,
    .bindless_write_accel = de__mtl_bindless_write_accel,
    .cmd_build_blas = de__mtl_accel_blas,
    .cmd_build_tlas = de__mtl_accel_tlas,
    .cmd_accel_emit = de__mtl_accel_emit,
    .accel_compacted_size = de__mtl_accel_read,
    .cmd_accel_compact = de__mtl_accel_compact, /* DENG-410 */
};

/* ---------- Accessors ---------- */

void *de__mtl_device_object(de_gpu_device *d) {
    return d && d->vt == &de__gpu_metal_vtable ? de__mtl_dev(d)->device : NULL;
}

void *de__mtl_queue_object(de_gpu_device *d, de_gpu_queue q) {
    return de__mtl_device_object(d) && (u32)q < DE_QUEUE_COUNT_ ? de__mtl_dev(d)->queues[q] : NULL;
}
