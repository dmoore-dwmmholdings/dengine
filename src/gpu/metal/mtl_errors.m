/* mtl_errors.m — GPU crash diagnostics on Metal (DENG-328; SPEC §18.5).
 *
 * With de_gpu_device_desc.crash_breadcrumbs:
 *   - Command lists record into command buffers made with errorOptions =
 *     MTLCommandBufferErrorOptionEncoderExecutionStatus (de__mtl_new_command_buffer), so a failed
 *     command buffer reports, for each encoder, its label, the debug groups open in it
 *     (de_gpu_cmd_marker_push) and how far it got (completed, affected, pending, faulted).
 *   - The completion handler of every committed command buffer (mtl_sync.m's watch) hands a
 *     failed one to de__mtl_errors_record: its MTLCommandBufferError code, description, the
 *     underlying IOKit error (kIOGPUCommandBufferCallbackError*), the encoder infos and the
 *     shader logs go into the device's fault log as text, and the device counts as lost from
 *     then on (vtable device_lost), so the front-end builds its report and fails later calls.
 *     DENG-288's handling stays: the handler still completes the submit's user fence and the
 *     timeline values complete through the dispatch group, so nothing waits forever.
 *   - Breadcrumbs: the value goes into a shared per-queue staging buffer at the slot's offset,
 *     and a blit encoder of its own copies it into the ring, after a barrier (fence epochs with
 *     no resource), so it lands once every earlier encoder of the list has finished.
 * Without crash_breadcrumbs a failed command buffer is only logged (DENG-288), as before.
 *
 * The fault log is an Objective-C object, retained by the device and by each completion block:
 * a block may run after the device is gone. Its text buffer is preallocated; the handler formats
 * into it under an unfair lock.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <os/lock.h>

#include "gpu/metal/mtl_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** Bytes of fault text kept per device; command buffers recorded in full. */
#define FAULT_TEXT 8192u
#define FAULT_CBS  4u
#define FAULT_LOGS 4u

@interface DEMtlFaultLog : NSObject {
  @public
    os_unfair_lock lock;
    de_atomic_u32 lost;
    u32 failures; /* failed command buffers seen */
    size_t len;
    char text[FAULT_TEXT];
}
@end

@implementation DEMtlFaultLog
@end

/* ---------- Names ---------- */

static const char *cb_error_name(NSInteger code) {
    switch (code) {
    case MTLCommandBufferErrorNone:
        return "None";
    case MTLCommandBufferErrorInternal:
        return "Internal";
    case MTLCommandBufferErrorTimeout:
        return "Timeout";
    case MTLCommandBufferErrorPageFault:
        return "PageFault";
    case MTLCommandBufferErrorAccessRevoked:
        return "AccessRevoked";
    case MTLCommandBufferErrorNotPermitted:
        return "NotPermitted";
    case MTLCommandBufferErrorOutOfMemory:
        return "OutOfMemory";
    case MTLCommandBufferErrorInvalidResource:
        return "InvalidResource";
    case MTLCommandBufferErrorMemoryless:
        return "Memoryless";
    case MTLCommandBufferErrorDeviceRemoved:
        return "DeviceRemoved";
    case MTLCommandBufferErrorStackOverflow:
        return "StackOverflow";
    default:
        return "unknown";
    }
}

static const char *encoder_state_name(MTLCommandEncoderErrorState s) {
    switch (s) {
    case MTLCommandEncoderErrorStateCompleted:
        return "completed";
    case MTLCommandEncoderErrorStateAffected:
        return "affected";
    case MTLCommandEncoderErrorStatePending:
        return "pending";
    case MTLCommandEncoderErrorStateFaulted:
        return "FAULTED";
    case MTLCommandEncoderErrorStateUnknown:
    default:
        return "unknown";
    }
}

static const char *str(NSString *s) {
    return s ? s.UTF8String : "";
}

static void fput(DEMtlFaultLog *f, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void fput(DEMtlFaultLog *f, const char *fmt, ...) {
    if (f->len + 1u >= FAULT_TEXT)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(f->text + f->len, FAULT_TEXT - f->len, fmt, ap);
    va_end(ap);
    if (w > 0)
        f->len += (size_t)w < FAULT_TEXT - f->len ? (size_t)w : FAULT_TEXT - f->len - 1u;
}

/* ---------- Recording a failure (completion handler thread) ---------- */

/* Formats one failed command buffer: its label, its error (code, text, the underlying IOKit
 * error, each encoder's status) and its shader logs (may be nil). */
static void describe(DEMtlFaultLog *f, NSString *label, NSError *err, id<MTLLogContainer> logs,
                     const char *queue) {
    fput(f, "command buffer \"%s\" on the %s queue failed: %s %s (code %ld)\n", str(label), queue,
         err ? str(err.domain) : "no error", err ? cb_error_name(err.code) : "",
         err ? (long)err.code : 0L);
    if (err) {
        fput(f, "  %s\n", str(err.localizedDescription));
        NSError *under = err.userInfo[NSUnderlyingErrorKey];
        if ([under isKindOfClass:[NSError class]])
            fput(f, "  underlying: %s code %ld: %s\n", str(under.domain), (long)under.code,
                 str(under.localizedDescription));
        NSArray *infos = err.userInfo[MTLCommandBufferEncoderInfoErrorKey];
        if ([infos isKindOfClass:[NSArray class]]) {
            for (id<MTLCommandBufferEncoderInfo> info in infos) {
                fput(f, "  encoder \"%s\": %s", str(info.label),
                     encoder_state_name(info.errorState));
                NSArray<NSString *> *groups = info.debugSignposts;
                if (groups.count) {
                    fput(f, "; debug groups ");
                    for (NSUInteger i = 0; i < groups.count; ++i)
                        fput(f, "%s%s", i ? "/" : "", str(groups[i]));
                }
                fput(f, "\n");
            }
        } else {
            fput(f, "  no encoder status (the command buffer was made without "
                    "MTLCommandBufferErrorOptionEncoderExecutionStatus)\n");
        }
    }
    u32 n = 0;
    for (id<MTLFunctionLog> log in logs) {
        if (n++ >= FAULT_LOGS)
            break;
        id<MTLFunctionLogDebugLocation> at = log.debugLocation;
        fput(f, "  shader log: encoder \"%s\", function \"%s\"", str(log.encoderLabel),
             log.function ? str(log.function.name) : "");
        if (at)
            fput(f, " at %s:%lu:%lu", str(at.URL.lastPathComponent), (unsigned long)at.line,
                 (unsigned long)at.column);
        fput(f, ": %s\n", str(log.description));
    }
}

void de__mtl_errors_record(void *fault, void *command_buffer, const char *queue) {
    DEMtlFaultLog *f = (__bridge DEMtlFaultLog *)fault;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
    if (f == nil || cb == nil)
        return;
    os_unfair_lock_lock(&f->lock);
    if (f->failures < FAULT_CBS)
        describe(f, cb.label, cb.error, cb.logs, queue);
    f->failures++;
    os_unfair_lock_unlock(&f->lock);
    /* After the text: the front-end reads it once it sees the device lost. */
    de_atomic_store_u32(&f->lost, 1u, DE_ATOMIC_RELEASE);
}

/* ---------- Test hook: a simulated failure ---------- */

/* An encoder status as Metal reports it in MTLCommandBufferEncoderInfoErrorKey. */
@interface DEMtlTestEncoderInfo : NSObject <MTLCommandBufferEncoderInfo>
@property(readonly, nonatomic) NSString *label;
@property(readonly, nonatomic) NSArray<NSString *> *debugSignposts;
@property(readonly, nonatomic) MTLCommandEncoderErrorState errorState;
- (instancetype)initWithLabel:(NSString *)label
                       groups:(NSArray<NSString *> *)groups
                        state:(MTLCommandEncoderErrorState)state;
@end

@implementation DEMtlTestEncoderInfo
- (instancetype)initWithLabel:(NSString *)label
                       groups:(NSArray<NSString *> *)groups
                        state:(MTLCommandEncoderErrorState)state {
    if ((self = [super init])) {
        _label = label;
        _debugSignposts = groups;
        _errorState = state;
    }
    return self;
}
@end

bool de__mtl_errors_simulate(de_gpu_device *d, const char *list, const char *group) {
    mtl_device *m = de__mtl_dev(d);
    if (!m->fault_log)
        return false;
    @autoreleasepool {
        NSString *g = [NSString stringWithUTF8String:group ? group : ""];
        NSArray *infos = @[
            [[DEMtlTestEncoderInfo alloc] initWithLabel:@"de breadcrumb"
                                                 groups:@[ g ]
                                                  state:MTLCommandEncoderErrorStateCompleted],
            [[DEMtlTestEncoderInfo alloc] initWithLabel:@"de_test compute"
                                                 groups:@[ g ]
                                                  state:MTLCommandEncoderErrorStateFaulted],
            [[DEMtlTestEncoderInfo alloc] initWithLabel:@"de breadcrumb"
                                                 groups:@[ g ]
                                                  state:MTLCommandEncoderErrorStatePending],
        ];
        NSError *under =
            [NSError errorWithDomain:@"IOGPUCommandQueueErrorDomain"
                                code:3
                            userInfo:@{
                                NSLocalizedDescriptionKey :
                                    @"Caused GPU Address Fault Error (simulated by the test)"
                            }];
        NSError *err = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                           code:MTLCommandBufferErrorPageFault
                                       userInfo:@{
                                           NSLocalizedDescriptionKey : @"Discarded (victim of GPU "
                                                                       @"error/recovery)",
                                           NSUnderlyingErrorKey : under,
                                           MTLCommandBufferEncoderInfoErrorKey : infos
                                       }];
        DEMtlFaultLog *f = (__bridge DEMtlFaultLog *)m->fault_log;
        os_unfair_lock_lock(&f->lock);
        describe(f, [NSString stringWithUTF8String:list ? list : ""], err, nil, "compute");
        f->failures++;
        os_unfair_lock_unlock(&f->lock);
        de_atomic_store_u32(&f->lost, 1u, DE_ATOMIC_RELEASE);
    }
    return true;
}

/* ---------- Device ---------- */

de_result de__mtl_errors_init(de_gpu_device *d, const de_gpu_device_desc *desc) {
    mtl_device *m = de__mtl_dev(d);
    if (!desc->crash_breadcrumbs)
        return DE_OK;
    @autoreleasepool {
        DEMtlFaultLog *f = [DEMtlFaultLog new];
        if (f == nil)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "metal: fault log");
        f->lock = OS_UNFAIR_LOCK_INIT;
        de_atomic_init_u32(&f->lost, 0);
        f->text[0] = '\0';
        m->fault_log = (void *)CFBridgingRetain(f);
        id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
            id<MTLBuffer> v = [dev newBufferWithLength:DE__GPU_BC_SLOTS * 4u
                                               options:MTLResourceStorageModeShared |
                                                       MTLResourceHazardTrackingModeUntracked];
            if (v == nil)
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "metal: breadcrumb staging buffer");
            v.label = @"de breadcrumb values";
            memset(v.contents, 0, v.length);
            m->bc_values[q] = (void *)CFBridgingRetain(v);
        }
    }
    m->crash_diag = true;
    return DE_OK;
}

void de__mtl_errors_shutdown(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    @autoreleasepool {
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
            if (m->bc_values[q])
                (void)CFBridgingRelease(m->bc_values[q]);
            m->bc_values[q] = NULL;
        }
        if (m->fault_log)
            (void)CFBridgingRelease(m->fault_log);
        m->fault_log = NULL;
    }
    m->crash_diag = false;
}

void *de__mtl_new_command_buffer(de_gpu_device *d, de_gpu_queue q) {
    mtl_device *m = de__mtl_dev(d);
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queues[q];
    id<MTLCommandBuffer> cb = nil;
    if (m->crash_diag) {
        MTLCommandBufferDescriptor *cd = [MTLCommandBufferDescriptor new];
        cd.retainedReferences = NO;
        cd.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
        cb = [queue commandBufferWithDescriptor:cd];
    } else {
        cb = [queue commandBufferWithUnretainedReferences];
    }
    return cb ? (void *)CFBridgingRetain(cb) : NULL;
}

bool de__mtl_device_lost(de_gpu_device *d) {
    DEMtlFaultLog *f = (__bridge DEMtlFaultLog *)de__mtl_dev(d)->fault_log;
    return f != nil && de_atomic_load_u32(&f->lost, DE_ATOMIC_ACQUIRE) != 0;
}

size_t de__mtl_device_lost_report(de_gpu_device *d, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    DEMtlFaultLog *f = (__bridge DEMtlFaultLog *)de__mtl_dev(d)->fault_log;
    int w;
    if (f == nil) {
        w = snprintf(
            out, cap,
            "command-buffer error capture is off (de_gpu_device_desc.crash_breadcrumbs)\n");
    } else {
        os_unfair_lock_lock(&f->lock);
        if (f->failures == 0)
            w = snprintf(out, cap, "no failed command buffer seen\n");
        else
            w = snprintf(out, cap, "failed command buffers: %u%s\n%s", f->failures,
                         f->failures > FAULT_CBS ? " (the first ones are listed)" : "", f->text);
        os_unfair_lock_unlock(&f->lock);
    }
    if (w < 0)
        return 0;
    return (size_t)w < cap ? (size_t)w : cap - 1u;
}

/* ---------- Breadcrumbs ---------- */

void de__mtl_cmd_breadcrumb(de_gpu_cmd *c, de__gpu_buffer *ring, u64 offset, u32 value) {
    mtl_device *m = de__mtl_dev(c->device);
    mtl_cmd *mc = (mtl_cmd *)de__gpu_cmd_payload(c);
    if (!m->bc_values[c->queue] || offset + 4u > (u64)DE__GPU_BC_SLOTS * 4u)
        return;
    @autoreleasepool {
        id<MTLBuffer> values = (__bridge id<MTLBuffer>)m->bc_values[c->queue];
        ((u32 *)values.contents)[offset / 4u] = value;
        /* Everything recorded so far finishes before the copy (fence epochs, no resource). */
        de__mtl_encoder_end(c);
        de__mtl_cmd_barriers(c, NULL, 0);
        if (!mc->cb)
            return;
        id<MTLBlitCommandEncoder> enc =
            (__bridge id<MTLBlitCommandEncoder>)de__mtl_cmd_encoder(c, DE__MTL_ENC_BLIT);
        if (enc == nil)
            return;
        enc.label = @"de breadcrumb";
        [enc copyFromBuffer:values
                 sourceOffset:(NSUInteger)offset
                     toBuffer:(__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer, ring)->buffer
            destinationOffset:(NSUInteger)offset
                         size:4];
        de__mtl_encoder_end(c);
    }
}
