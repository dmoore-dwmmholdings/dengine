/* DENG-551: standalone FSEvents ownership control, no engine linkage. */
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void callback(ConstFSEventStreamRef stream, void *context, size_t count, void *paths,
                     const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
    (void)stream;
    (void)context;
    (void)count;
    (void)paths;
    (void)flags;
    (void)ids;
}
static void noop(void *context) {
    (void)context;
}
static void finalized(void *context) {
    dispatch_semaphore_t done = context;
    dispatch_semaphore_signal(done);
    dispatch_release(done); /* The finalizer owns one reference independently. */
}
int main(int argc, char **argv) {
    unsigned stage;
    if (argc != 3 && argc != 4)
        goto usage;
    int observe_finalizer = argc == 4 && !strcmp(argv[3], "finalize");
    if (argc == 4 && !observe_finalizer)
        goto usage;
    if (!strcmp(argv[1], "create"))
        stage = 0;
    else if (!strcmp(argv[1], "schedule"))
        stage = 1;
    else if (!strcmp(argv[1], "start"))
        stage = 2;
    else
        goto usage;
    errno = 0;
    char *end;
    unsigned long iterations = strtoul(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || iterations < 1 || iterations > 128)
        goto usage;
    char directory[] = "/private/tmp/deng-fsevents-control-XXXXXX";
    if (!mkdtemp(directory)) {
        perror("mkdtemp");
        return 1;
    }
    unsigned created = 0, scheduled = 0, started = 0, finalized_count = 0;
    int result = 0;
    for (unsigned long i = 0; i < iterations; ++i) {
        CFStringRef path = CFStringCreateWithCString(NULL, directory, kCFStringEncodingUTF8);
        CFArrayRef paths =
            path ? CFArrayCreate(NULL, (const void **)&path, 1, &kCFTypeArrayCallBacks) : NULL;
        FSEventStreamRef stream =
            paths ? FSEventStreamCreate(
                        NULL, callback, NULL, paths, kFSEventStreamEventIdSinceNow, .02,
                        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                            kFSEventStreamCreateFlagUseCFTypes |
                            kFSEventStreamCreateFlagUseExtendedData)
                  : NULL;
        if (paths)
            CFRelease(paths);
        if (path)
            CFRelease(path);
        if (!stream) {
            fprintf(stderr, "FSEventStreamCreate failed at iteration %lu\n", i);
            result = 1;
            break;
        }
        ++created;
        dispatch_queue_t queue = NULL;
        dispatch_semaphore_t semaphore = NULL;
        if (stage >= 1) {
            queue = dispatch_queue_create("deng551.fsevents.control", DISPATCH_QUEUE_SERIAL);
            semaphore = dispatch_semaphore_create(0);
            if (observe_finalizer) {
                dispatch_retain(semaphore);
                dispatch_set_context(queue, semaphore);
                dispatch_set_finalizer_f(queue, finalized);
            }
            FSEventStreamSetDispatchQueue(stream, queue);
            ++scheduled;
        }
        if (stage == 2) {
            if (FSEventStreamStart(stream)) {
                ++started;
                FSEventStreamFlushSync(stream);
                dispatch_sync_f(queue, NULL, noop);
                FSEventStreamStop(stream);
            } else {
                fprintf(stderr, "FSEventStreamStart failed at iteration %lu\n", i);
                result = 1;
            }
        }
        /* Invalidate while still scheduled, as the API requires. A successful
         * start is stopped first; failed starts still require invalidation. */
        if (stage >= 1)
            FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        if (stage >= 1) {
            dispatch_sync_f(queue, NULL, noop);
            dispatch_release(queue);
            if (observe_finalizer) {
                if (!dispatch_semaphore_wait(semaphore,
                                             dispatch_time(DISPATCH_TIME_NOW, 2000000000)))
                    ++finalized_count;
                else {
                    fprintf(stderr, "Queue finalizer not observed within 2s at iteration %lu\n", i);
                    result = 1;
                }
            }
            dispatch_release(semaphore);
            if (observe_finalizer && result)
                break;
        }
    }
    if (rmdir(directory)) {
        perror("rmdir");
        result = 1;
    }
    fprintf(stderr,
            "FSEvents control: mode=%s requested=%lu created=%u scheduled=%u started=%u; "
            "finalized=%u observe_finalizer=%d; no engine linked; control result=%d\n",
            argv[1], iterations, created, scheduled, started, finalized_count, observe_finalizer,
            result);
    return result;
usage:
    fprintf(stderr, "usage: %s create|schedule|start ITERATIONS(1..128) [finalize]\n", argv[0]);
    return 2;
}
