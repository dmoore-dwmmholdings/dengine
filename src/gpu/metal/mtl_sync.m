/* mtl_sync.m — submission, queue fences and timeline fences on Metal (DENG-288, SPEC §12.1).
 *
 * de_gpu_fence = MTLSharedEvent: its signaledValue is the timeline value, the GPU signals and
 * waits on it (encodeSignalEvent:value:, encodeWaitForEvent:value:), the CPU reads it and waits
 * with waitUntilSignaledValue:timeoutMS: (macOS 14, the deployment minimum).
 *
 * queue_submit commits, in this order and under the front-end's per-queue submit lock:
 *   1. when the submit waits on fences (or has no lists): a head command buffer with one
 *      encodeWaitForEvent:value: per wait and a blit encoder that waits for and updates the
 *      queue fence, so the lists committed after it start after the waits;
 *   2. each list's command buffer, ended by its tail encoder (mtl_barriers.m);
 *   3. in the last command buffer: the signal of the user fence (on the GPU, so other queues
 *      can wait for it).
 * The queue's internal timeline value (deferred destruction, command-list recycling) is set from
 * the CPU once every command buffer of the submit, and of every submit before it on the queue,
 * has completed: a dispatch group per submit, chained to the previous one. Not with
 * encodeSignalEvent: in the command buffer: the GPU reaches that signal before Metal counts the
 * command buffer as complete, and the command buffers do not retain their resources, so a
 * deferred destroy ran while Metal still held the command buffer (the debug layer's
 * "destroyed while still required to be alive by the command buffer", on CI's paravirtual GPU).
 * The waits live in their own command buffer because a list's encoders are recorded before its
 * waits are known, and a wait cannot go in front of recorded encoders.
 *
 * A command buffer that fails on the GPU (a fault, the GPU watchdog) may skip its signals, and the
 * ones after it on the queue may wait for its fence forever. The completion handler of every
 * command buffer therefore logs an error with the command buffer's label and sets the submit's
 * user fence from the CPU, so fence waits end (the front-end contract for a lost device); the
 * timeline value completes through the dispatch group either way. Full error reporting
 * (encoder-level status, device-lost state) is DENG-328.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

static const char *const k_queue_names[DE_QUEUE_COUNT_] = {"graphics", "compute", "copy"};

/* ---------- Queue fences ---------- */

de_result de__mtl_sync_init(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
        m->render_stages = DE__MTL_STAGE_VERTEX | DE__MTL_STAGE_FRAGMENT;
        if ([dev supportsFamily:MTLGPUFamilyMetal3])
            m->render_stages |= DE__MTL_STAGE_OBJECT | DE__MTL_STAGE_MESH;
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
            id<MTLFence> f = [dev newFence];
            /* Retained references: it holds the fence until it completes. */
            id<MTLCommandBuffer> cb = [(__bridge id<MTLCommandQueue>)m->queues[q] commandBuffer];
            id<MTLBlitCommandEncoder> enc = [cb blitCommandEncoder];
            if (f == nil || enc == nil)
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "metal: %s queue fence", k_queue_names[q]);
            f.label = [NSString stringWithFormat:@"de %s queue fence", k_queue_names[q]];
            /* One update before any wait: a wait with no update before it never ends. */
            [enc updateFence:f];
            [enc endEncoding];
            [cb commit];
            /* Waited for: these command buffers are not on the timeline, so nothing else would
             * wait for them, and a device destroyed right after creation then released its queues
             * with them in flight, which hung CI's paravirtual GPU (and with it the next test
             * processes' work). */
            [cb waitUntilCompleted];
            m->queue_fence[q] = (void *)CFBridgingRetain(f);
        }
    }
    return DE_OK;
}

void de__mtl_sync_drain(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    @autoreleasepool {
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
            if (!m->queues[q])
                continue;
            id<MTLCommandBuffer> cb = [(__bridge id<MTLCommandQueue>)m->queues[q] commandBuffer];
            [cb commit];
            [cb waitUntilCompleted];
        }
    }
}

void de__mtl_sync_shutdown(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    de__mtl_sync_release_groups(d);

    @autoreleasepool {
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
            if (m->queue_fence[q])
                (void)CFBridgingRelease(m->queue_fence[q]);
            m->queue_fence[q] = NULL;
        }
    }
}

/* ---------- Fences ---------- */

static id<MTLSharedEvent> event_of(de__gpu_fence *f) {
    return (__bridge id<MTLSharedEvent>)DE__GPU_PAYLOAD(mtl_fence, f)->event;
}

de_result de__mtl_fence_create(de_gpu_device *d, de__gpu_fence *f, u64 initial) {
    @autoreleasepool {
        id<MTLSharedEvent> ev = [(__bridge id<MTLDevice>)de__mtl_dev(d)->device newSharedEvent];
        if (ev == nil)
            return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "metal: MTLSharedEvent for a fence");
        ev.label = @"de fence";
        ev.signaledValue = initial;
        DE__GPU_PAYLOAD(mtl_fence, f)->event = (void *)CFBridgingRetain(ev);
    }
    return DE_OK;
}

void de__mtl_fence_destroy(de_gpu_device *d, de__gpu_fence *f) {
    (void)d;
    mtl_fence *mf = DE__GPU_PAYLOAD(mtl_fence, f);
    if (mf->event) {
        @autoreleasepool {
            (void)CFBridgingRelease(mf->event);
        }
    }
    mf->event = NULL;
}

u64 de__mtl_fence_value(de_gpu_device *d, de__gpu_fence *f) {
    (void)d;
    return event_of(f).signaledValue;
}

de_result de__mtl_fence_wait(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout_ns) {
    (void)d;
    id<MTLSharedEvent> ev = event_of(f);
    if (ev.signaledValue >= value)
        return DE_OK;
    if (timeout_ns > 0) {
        /* Whole milliseconds, rounded up; UINT64_MAX ns stays "practically forever". */
        const u64 ms = timeout_ns / 1000000u + (timeout_ns % 1000000u ? 1u : 0u);
        if ([ev waitUntilSignaledValue:value timeoutMS:ms])
            return DE_OK;
    }
    if (ev.signaledValue >= value)
        return DE_OK;
    /* An expected outcome (polling): the error context only, no log line. */
    return DE_FAIL(DE_ERR_TIMEOUT, "de_gpu_fence_wait: fence is at %llu, not %llu",
                   (unsigned long long)ev.signaledValue, (unsigned long long)value);
}

/* ---------- Submission ---------- */

/* Leaves the submit's group when `cb` completes; on an error, logs it and completes the user
 * fence (see the file comment). The block holds the events and the group, not the device: it may
 * run after the device is gone. */
static void watch(id<MTLCommandBuffer> cb, dispatch_group_t group, de_log_category log,
                  de_gpu_queue q, id<MTLSharedEvent> fence, u64 fence_value) {
    const char *queue_name = k_queue_names[q];
    dispatch_group_enter(group);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      if (done.status == MTLCommandBufferStatusError) {
          DE_ERROR(log, "metal: command buffer \"%s\" on the %s queue failed: %s",
                   done.label ? done.label.UTF8String : "", queue_name,
                   done.error ? done.error.localizedDescription.UTF8String : "no error given");
          if (fence != nil && fence.signaledValue < fence_value)
              fence.signaledValue = fence_value;
      }
      dispatch_group_leave(group);
    }];
}

/* The published internal value must complete even when the submit fails (gpu_backend.h). It is
 * set once the submits before it have completed (their groups), never ahead of them. */
static de_result lost(de_gpu_device *d, de_gpu_queue q, dispatch_group_t group, u64 queue_value,
                      const char *what) {
    mtl_device *m = de__mtl_dev(d);
    id<MTLSharedEvent> timeline = (__bridge id<MTLSharedEvent>)m->timeline[q];
    dispatch_group_notify(group, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
      if (timeline.signaledValue < queue_value)
          timeline.signaledValue = queue_value;
    });
    return DE_FAIL(DE_ERR_DEVICE_LOST, "metal: %s on the %s queue", what, k_queue_names[q]);
}

void de__mtl_sync_release_groups(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    @autoreleasepool {
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
            if (m->last_group[q])
                (void)CFBridgingRelease(m->last_group[q]);
            m->last_group[q] = NULL;
        }
    }
}

de_result de__mtl_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                               u64 queue_value) {
    mtl_device *m = de__mtl_dev(d);
    de__mtl_residency_sync(d);
    @autoreleasepool {
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queues[q];
        id<MTLSharedEvent> timeline = (__bridge id<MTLSharedEvent>)m->timeline[q];
        id<MTLFence> qf = (__bridge id<MTLFence>)m->queue_fence[q];
        id<MTLCommandBuffer> last = nil;
        id<MTLSharedEvent> user = nil;
        if (s->signal.id) {
            de__gpu_fence *f = de__gpu_get_fence(d, s->signal);
            if (f)
                user = event_of(f);
        }

        /* This submit's group: the previous submit's group, then each of its command buffers.
         * Under the front-end's per-queue submit lock, so the chain follows commit order. */
        dispatch_group_t group = dispatch_group_create();
        dispatch_queue_t dq = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
        if (m->last_group[q]) {
            dispatch_group_t prev = (dispatch_group_t)CFBridgingRelease(m->last_group[q]);
            dispatch_group_enter(group);
            dispatch_group_notify(prev, dq, ^{
              dispatch_group_leave(group);
            });
        }
        m->last_group[q] = (void *)CFBridgingRetain(group);

        if (s->wait_count > 0 || s->cmd_count == 0) {
            id<MTLCommandBuffer> head = [queue commandBufferWithUnretainedReferences];
            if (head == nil)
                return lost(d, q, group, queue_value, "no command buffer for the waits");
            head.label = @"de submit waits";
            for (u32 i = 0; i < s->wait_count; ++i) {
                de__gpu_fence *f = de__gpu_get_fence(d, s->waits[i].fence);
                if (f)
                    [head encodeWaitForEvent:event_of(f) value:s->waits[i].value];
            }
            id<MTLBlitCommandEncoder> enc = [head blitCommandEncoder];
            [enc waitForFence:qf];
            [enc updateFence:qf];
            [enc endEncoding];
            watch(head, group, d->log, q, user, s->signal_value);
            last = head;
        }
        for (u32 i = 0; i < s->cmd_count; ++i) {
            de_gpu_cmd *c = s->cmds[i];
            mtl_cmd *mc = (mtl_cmd *)de__gpu_cmd_payload(c);
            de__mtl_cmd_finish(c);
            if (!mc->cb)
                continue;
            [last commit];
            last = (id<MTLCommandBuffer>)CFBridgingRelease(mc->cb);
            mc->cb = NULL;
            watch(last, group, d->log, q, user, s->signal_value);
        }
        if (last == nil)
            return lost(d, q, group, queue_value, "no command buffer to signal");
        if (user != nil)
            [last encodeSignalEvent:user value:s->signal_value];
        dispatch_group_notify(group, dq, ^{
          if (timeline.signaledValue < queue_value)
              timeline.signaledValue = queue_value;
        });
        [last commit];
    }
    return DE_OK;
}
