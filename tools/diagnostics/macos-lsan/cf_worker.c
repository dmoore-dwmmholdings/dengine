/* DENG-686: CoreFoundation worker TLS control, no engine or FSEvents linkage.
 * Build/run instructions and interpretation limits: README.md in this directory.
 * A sanitizer process-exit failure is retained, never suppressed by this control. */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void create_number(void) {
    long long value = 123456789;
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberLongLongType, &value);
    if (!number) {
        fprintf(stderr, "CFNumberCreate failed\n");
        abort();
    }
    CFRelease(number);
}
static void work(void *arg) {
    create_number();
    dispatch_semaphore_signal((dispatch_semaphore_t)arg);
}
static void noop(void *arg) {
    (void)arg;
}
int main(int argc, char **argv) {
    if (argc != 2 ||
        (strcmp(argv[1], "none") && strcmp(argv[1], "main") && strcmp(argv[1], "worker"))) {
        fprintf(stderr, "usage: %s none|main|worker\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "none")) {
        fprintf(stderr, "No-work control completed; no CFNumber or dispatch API used\n");
        return 0;
    }
    if (!strcmp(argv[1], "main")) {
        create_number();
        fprintf(stderr, "CFNumber created/released on main thread; no dispatch API used\n");
        return 0;
    }

    dispatch_queue_t queue = dispatch_queue_create("deng686.cf-control", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    if (!queue || !done) {
        fprintf(stderr, "dispatch allocation failed\n");
        if (queue)
            dispatch_release(queue);
        if (done)
            dispatch_release(done);
        return 2;
    }
    dispatch_async_f(queue, done, work);
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2000000000LL))) {
        fprintf(stderr, "worker did not complete within 2 seconds; no completed-lifetime claim\n");
        return 2;
    }
    /* The semaphore allows main to wait without executing the worker inline. */
    dispatch_sync_f(queue, NULL, noop);
    dispatch_release(queue);
    dispatch_release(done);
    fprintf(stderr, "CFNumber created/released on dispatch worker; no FSEvents API used\n");
    return 0;
}
