/* DENG-551: standalone runtime controls, deliberately without engine linkage. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static int metal_control(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
            return 77;
        for (unsigned i = 0; i < 3; ++i) {
            @autoreleasepool {
                id<MTLCommandQueue> queue = [device newCommandQueue];
                if (!queue)
                    return 1;
                queue.label = @"DENG-551 standalone control";
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLBlitCommandEncoder> encoder = [cb blitCommandEncoder];
                if (!encoder)
                    return 1;
                [encoder endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.status != MTLCommandBufferStatusCompleted)
                    return 1;
            }
        }
    }
    fprintf(stderr, "standalone Metal control completed; no engine linked\n");
    return 0;
}

static int dispatch_control(void) {
    @autoreleasepool {
        dispatch_queue_t queue = dispatch_queue_create("deng551-control", DISPATCH_QUEUE_SERIAL);
        dispatch_semaphore_t completed = dispatch_semaphore_create(0);
        dispatch_group_t prev = nil;
        for (unsigned i = 0; i < 256; ++i) {
            dispatch_group_t group = dispatch_group_create();
            if (prev) {
                dispatch_group_enter(group);
                dispatch_group_notify(prev, queue, ^{
                  dispatch_group_leave(group);
                });
            }
            dispatch_group_enter(group);
            dispatch_async(queue, ^{
              dispatch_group_leave(group);
            });
            dispatch_group_notify(group, queue, ^{
              dispatch_semaphore_signal(completed);
            });
            prev = group;
        }
        dispatch_group_wait(prev, DISPATCH_TIME_FOREVER);
        /* Group completion can precede notification execution. Observe every
         * notification, then ensure the last callback has actually returned. */
        for (unsigned i = 0; i < 256; ++i)
            dispatch_semaphore_wait(completed, DISPATCH_TIME_FOREVER);
        dispatch_sync(queue, ^{
                      });
    }
    fprintf(stderr, "standalone dispatch chain completed; no engine calls\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "metal") == 0)
        return metal_control();
    if (argc == 2 && strcmp(argv[1], "dispatch") == 0)
        return dispatch_control();
    fprintf(stderr, "usage: %s metal|dispatch\n", argv[0]);
    return 2;
}
