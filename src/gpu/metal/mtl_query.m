/* mtl_query.m — timestamp queries on Metal (DENG-326; SPEC §12.1, §12.3).
 *
 * A de_gpu_query_heap is an MTLCounterSampleBuffer of the common timestamp counter set (shared
 * storage, at most DE__MTL_MAX_QUERIES samples) plus a shared staging buffer for its resolved
 * values. Counter values count nanoseconds on Apple GPUs (caps.timestamp_frequency is 1 GHz,
 * DENG-288), in the clock of sampleTimestamps:gpuTimestamp:.
 *
 * Timestamps. Apple GPUs sample counters only at stage boundaries (encoder start and end), not
 * between commands. de_gpu_cmd_timestamp therefore ends the open encoder and records a blit
 * encoder of its own with startOfEncoderSampleIndex = the query and one tiny fill (an encoder
 * needs a command to run and so to be sampled). Barriers on both sides (mtl_barriers.m's fence
 * epochs, with no resource) make it wait for every earlier encoder of the list and every later
 * encoder wait for it, so the time is when the earlier work finished and later work does not
 * overlap it.
 *
 * Resolve. GPU-side resolveCounters: writes zeros on Apple GPUs, so the samples are resolved on
 * the CPU (resolveCounterRange:) while the list waits: the command buffer signals the list's
 * resolve event, a listener block resolves into the staging buffer and signals the event again,
 * the command buffer waits for that value (encodeWaitForEvent:value:, which holds back every later
 * command of the command buffer), and a blit copies the staged values into the destination. So
 * the values are in place before anything after the resolve runs, including the submit's fence
 * signal. The event and listener live on the command list (made on its first resolve). The
 * command buffers do not retain the heap: the front-end's deferred destruction keeps it alive
 * until the lists that use it complete, and the listener block retains what it reads.
 *
 * Hand-off (DENG-419). Metal runs the listener block on a GCD thread that IOSurface's event
 * callback starts; nothing on that path orders it after the recording thread for the thread
 * sanitizer (nor, formally, for the C11 memory model): the block's captured values, written by
 * the block copy inside notifyListener:, and the heap's staging buffer, written by earlier blocks
 * of other lists on other listener threads, would be read and written without a visible
 * happens-before. Each heap therefore has one counter, `handoff`, changed only by acq_rel
 * read-modify-writes: the recording thread bumps it after notifyListener: (publishing the block),
 * a block acquires it before it reads anything captured, and bumps it again after it wrote the
 * staging buffer (publishing its values to the heap's next resolve). Because every change is a
 * read-modify-write, each one continues the release sequence of all earlier ones, so a block
 * sees everything any thread did before its own resolve was recorded. Real overlap (two lists
 * resolving the same queries at the same time, which the API does not allow) is still reported.
 *
 * Destroy (DENG-424). The front-end destroys a heap only after the GPU completed the lists that
 * used it (deferred destruction), and a resolve's command buffer cannot complete before its
 * block ran (it waits for the event value the block sets last), so a block never outlives its
 * heap: a probe that marked destroyed counters never saw a block after its heap's destroy. Destroy
 * still does an acq_rel read-modify-write on `handoff` first, so the heap's teardown, and the
 * front-end's reuse of its slot for the next heap, happen after the last block in the C11 sense
 * too, not only in time.
 *
 * Thread sanitizer (DENG-424). TSan checks an atomic read-modify-write's own memory access before
 * it applies the operation's acquire, so the block's first RMW raced in its view with the plain
 * writes that made the counter: the heap's init and the front-end's zeroing of a reused slot, made
 * after the list's listener already existed. The hand-off through Metal (event signal, IOSurface
 * notification, a GCD thread) is real but invisible to TSan, so it is annotated: the recording
 * thread calls __tsan_release on the counter after notifyListener:, and the block calls
 * __tsan_acquire before it touches anything. These are no-ops outside TSan builds.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

/* Bytes after the resolved values that timestamp encoders fill. */
#define SCRATCH_BYTES 16u

/* TSan annotations for the hand-off through Metal's event path (see "Thread sanitizer"). */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DE__MTL_TSAN 1
#endif
#endif
#if defined(DE__MTL_TSAN)
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
#define DE__MTL_TSAN_ACQUIRE(p) __tsan_acquire((void *)(p))
#define DE__MTL_TSAN_RELEASE(p) __tsan_release((void *)(p))
#else
#define DE__MTL_TSAN_ACQUIRE(p) ((void)(p))
#define DE__MTL_TSAN_RELEASE(p) ((void)(p))
#endif

static mtl_query_heap *payload(de__gpu_query_heap *h) {
    return DE__GPU_PAYLOAD(mtl_query_heap, h);
}

static mtl_cmd *cmd_payload(de_gpu_cmd *c) {
    return (mtl_cmd *)de__gpu_cmd_payload(c);
}

/* ---------- Heaps ---------- */

de_result de__mtl_query_heap_create(de_gpu_device *d, de__gpu_query_heap *h,
                                    const de_gpu_query_heap_desc *desc) {
    (void)desc;
    static const char F[] = "de_gpu_query_heap_create";
    mtl_query_heap *qh = payload(h);
    if (h->count > DE__MTL_MAX_QUERIES)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "%s(\"%s\"): metal: %u timestamps; a counter sample buffer holds at "
                            "most %u",
                            F, h->name, h->count, DE__MTL_MAX_QUERIES);
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        id<MTLCounterSet> set = nil;
        for (id<MTLCounterSet> s in dev.counterSets)
            if ([s.name isEqualToString:MTLCommonCounterSetTimestamp])
                set = s;
        if (set == nil || ![dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                                "%s(\"%s\"): metal: \"%s\" cannot sample GPU timestamps at stage "
                                "boundaries",
                                F, h->name, d->caps.device_name);
        MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
        sd.counterSet = set;
        sd.storageMode = MTLStorageModeShared; /* resolveCounterRange: reads it on the CPU */
        sd.sampleCount = h->count;
        if (h->name[0])
            sd.label = [NSString stringWithUTF8String:h->name];
        NSError *err = nil;
        id<MTLCounterSampleBuffer> samples = [dev newCounterSampleBufferWithDescriptor:sd
                                                                                 error:&err];
        if (samples == nil)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                                "%s(\"%s\"): metal: newCounterSampleBufferWithDescriptor (%u): %s",
                                F, h->name, h->count,
                                err ? err.localizedDescription.UTF8String : "no reason given");
        id<MTLBuffer> staging =
            [dev newBufferWithLength:(NSUInteger)h->count * sizeof(u64) + SCRATCH_BYTES
                             options:MTLResourceStorageModeShared |
                                     MTLResourceHazardTrackingModeUntracked];
        if (staging == nil)
            return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY,
                                "%s(\"%s\"): metal: staging buffer (%u timestamps)", F, h->name,
                                h->count);
        memset(staging.contents, 0xFF, staging.length); /* "never written" until resolved */
        qh->samples = (void *)CFBridgingRetain(samples);
        qh->staging = (void *)CFBridgingRetain(staging);
        de_atomic_init_u64(&qh->handoff, 0);
    }
    return DE_OK;
}

void de__mtl_query_heap_destroy(de_gpu_device *d, de__gpu_query_heap *h) {
    (void)d;
    mtl_query_heap *qh = payload(h);
    /* After every finished block of this heap (see "Destroy"). */
    (void)de_atomic_fetch_add_u64(&qh->handoff, 1u, DE_ATOMIC_ACQ_REL);
    @autoreleasepool {
        if (qh->samples)
            (void)CFBridgingRelease(qh->samples);
        if (qh->staging)
            (void)CFBridgingRelease(qh->staging);
    }
    qh->samples = NULL;
    qh->staging = NULL;
}

/* ---------- Timestamps ---------- */

void de__mtl_cmd_timestamp(de_gpu_cmd *c, de__gpu_query_heap *h, u32 index) {
    mtl_cmd *mc = cmd_payload(c);
    mtl_query_heap *qh = payload(h);
    /* Everything recorded so far is waited for by the timestamp's encoder... */
    de__mtl_encoder_end(c);
    de__mtl_cmd_barriers(c, NULL, 0);
    if (!mc->cb)
        return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)mc->cb;
        id<MTLBuffer> staging = (__bridge id<MTLBuffer>)qh->staging;
        MTLBlitPassDescriptor *bp = [MTLBlitPassDescriptor blitPassDescriptor];
        bp.sampleBufferAttachments[0].sampleBuffer =
            (__bridge id<MTLCounterSampleBuffer>)qh->samples;
        bp.sampleBufferAttachments[0].startOfEncoderSampleIndex = index;
        bp.sampleBufferAttachments[0].endOfEncoderSampleIndex = MTLCounterDontSample;
        id<MTLBlitCommandEncoder> enc = [cb blitCommandEncoderWithDescriptor:bp];
        if (enc == nil) {
            de__gpu_fail(c->device, DE_ERR_DEVICE_LOST, "metal: \"%s\": no timestamp encoder",
                         c->name);
            return;
        }
        enc.label = @"de timestamp";
        de__mtl_encoder_started(c, (__bridge void *)enc, DE__MTL_ENC_BLIT);
        [enc fillBuffer:staging
                  range:NSMakeRange((NSUInteger)h->count * sizeof(u64), SCRATCH_BYTES)
                  value:0];
    }
    de__mtl_encoder_end(c);
    /* ...and waits for it. */
    de__mtl_cmd_barriers(c, NULL, 0);
}

/* ---------- Resolve ---------- */

/* The list's resolve event and listener, made on first use. */
static bool resolve_event(de_gpu_cmd *c) {
    mtl_cmd *mc = cmd_payload(c);
    if (mc->resolve_event)
        return true;
    id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(c->device)->device;
    id<MTLSharedEvent> ev = [dev newSharedEvent];
    MTLSharedEventListener *listener = [[MTLSharedEventListener alloc] init];
    if (ev == nil || listener == nil)
        return false;
    ev.label = @"de timestamp resolve";
    mc->resolve_event = (void *)CFBridgingRetain(ev);
    mc->resolve_listener = (void *)CFBridgingRetain(listener);
    mc->resolve_value = 0;
    return true;
}

void de__mtl_cmd_resolve_queries(de_gpu_cmd *c, de__gpu_query_heap *h, u32 first, u32 count,
                                 de__gpu_buffer *dst, u64 dst_off) {
    mtl_cmd *mc = cmd_payload(c);
    mtl_query_heap *qh = payload(h);
    de__mtl_encoder_end(c);
    de__mtl_cmd_barriers(c, NULL, 0);
    if (!mc->cb)
        return;
    @autoreleasepool {
        if (!resolve_event(c)) {
            de__gpu_fail(c->device, DE_ERR_OUT_OF_MEMORY,
                         "de_gpu_cmd_resolve_queries(\"%s\"): metal: no resolve event", c->name);
            return;
        }
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)mc->cb;
        id<MTLSharedEvent> ev = (__bridge id<MTLSharedEvent>)mc->resolve_event;
        MTLSharedEventListener *listener = (__bridge MTLSharedEventListener *)mc->resolve_listener;
        id<MTLCounterSampleBuffer> samples = (__bridge id<MTLCounterSampleBuffer>)qh->samples;
        id<MTLBuffer> staging = (__bridge id<MTLBuffer>)qh->staging;
        const u64 ready = mc->resolve_value + 1u, done = mc->resolve_value + 2u;
        mc->resolve_value = done;

        de_atomic_u64 *handoff = &qh->handoff; /* the heap outlives the block (file comment) */
        [cb encodeSignalEvent:ev value:ready];
        [ev notifyListener:listener
                   atValue:ready
                     block:^(id<MTLSharedEvent> e, uint64_t value) {
                       (void)value;
                       /* Before any captured value: see "Hand-off" (DENG-419) and "Thread
                        * sanitizer" (DENG-424). */
                       DE__MTL_TSAN_ACQUIRE(handoff);
                       (void)de_atomic_fetch_add_u64(handoff, 1u, DE_ATOMIC_ACQ_REL);
                       NSData *data = [samples resolveCounterRange:NSMakeRange(first, count)];
                       u8 *out = (u8 *)staging.contents + (size_t)first * sizeof(u64);
                       const size_t want = (size_t)count * sizeof(u64);
                       const size_t got = data ? data.length : 0;
                       if (got)
                           memcpy(out, data.bytes, got < want ? got : want);
                       if (got < want) /* not written: "never written" */
                           memset(out + got, 0xFF, want - got);
                       /* Publishes the staging writes to the heap's next resolve. */
                       (void)de_atomic_fetch_add_u64(handoff, 1u, DE_ATOMIC_ACQ_REL);
                       e.signaledValue = done;
                     }];
        /* Publishes the block (copied with its captures inside notifyListener:) to the thread
         * that will run it. */
        (void)de_atomic_fetch_add_u64(&qh->handoff, 1u, DE_ATOMIC_ACQ_REL);
        DE__MTL_TSAN_RELEASE(&qh->handoff); /* the same, for TSan (DENG-424) */
        [cb encodeWaitForEvent:ev value:done];

        id<MTLBlitCommandEncoder> blit =
            (__bridge id<MTLBlitCommandEncoder>)de__mtl_cmd_encoder(c, DE__MTL_ENC_BLIT);
        if (blit == nil)
            return;
        [blit copyFromBuffer:staging
                 sourceOffset:(NSUInteger)first * sizeof(u64)
                     toBuffer:(__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer, dst)->buffer
            destinationOffset:(NSUInteger)dst_off
                         size:(NSUInteger)count * sizeof(u64)];
    }
}

void de__mtl_query_cmd_fini(de_gpu_cmd *c) {
    mtl_cmd *mc = cmd_payload(c);
    @autoreleasepool {
        if (mc->resolve_listener)
            (void)CFBridgingRelease(mc->resolve_listener);
        if (mc->resolve_event)
            (void)CFBridgingRelease(mc->resolve_event);
    }
    mc->resolve_listener = NULL;
    mc->resolve_event = NULL;
}

/* ---------- Calibration ---------- */

bool de__mtl_queue_timestamp_now(de_gpu_device *d, de_gpu_queue q, u64 *ticks) {
    (void)q;
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        MTLTimestamp cpu = 0, gpu = 0;
        [dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
        *ticks = gpu;
        return gpu != 0;
    }
}
