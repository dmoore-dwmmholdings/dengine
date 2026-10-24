/* DENG-606: opt-in diagnostic CoreGraphics outcome and native drawable timing audit.
 * clang -dynamiclib -framework ApplicationServices -framework QuartzCore -o mouse-audit.dylib
 * this.m DE_MOUSE_AUDIT=/absolute/audit.csv DYLD_INSERT_LIBRARIES=/absolute/mouse-audit.dylib app
 * ... Records actual CGAssociate return codes; last acknowledgement is not an OS getter or
 * physical-device proof. nextDrawable is timed independently of engine collection/fence waits.
 * DE_MOUSE_REFUSE_ENABLE=1 injects one enable refusal without calling the OS.
 */
#import <ApplicationServices/ApplicationServices.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static FILE *audit_file;
static atomic_int observed = -1;
static atomic_uint transitions;
static int refuse;
static uint64_t now_ns(void) {
    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    uint64_t n = mach_absolute_time();
    return n / t.denom * t.numer + n % t.denom * t.numer / t.denom;
}
static id (*original_next_drawable)(id, SEL);
static id timed_next_drawable(id layer, SEL selector) {
    uint64_t begin = now_ns();
    id drawable = original_next_drawable(layer, selector);
    uint64_t duration = now_ns() - begin;
    if (audit_file)
        fprintf(audit_file, "nextDrawable,%llu,%d,0,%d,0,%llu\n", (unsigned long long)begin,
                getpid(), drawable != nil, (unsigned long long)duration);
    return drawable;
}
__attribute__((constructor)) static void init(void) {
    const char *path = getenv("DE_MOUSE_AUDIT");
    if (path) {
        audit_file = fopen(path, "w");
        if (audit_file) {
            setvbuf(stdout, NULL, _IOLBF, 0);
            setvbuf(audit_file, NULL, _IOLBF, 0);
            fputs("kind,time_ns,pid,argument,result,injected,duration_ns\n", audit_file);
        }
    }
    refuse = getenv("DE_MOUSE_REFUSE_ENABLE") != NULL;
    if (audit_file) {
        Method method = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
        if (method)
            original_next_drawable =
                (void *)method_setImplementation(method, (IMP)timed_next_drawable);
    }
}
int de_mouse_audit_observed(void) {
    return atomic_load(&observed);
}
unsigned de_mouse_audit_transitions(void) {
    return atomic_load(&transitions);
}
static CGError audit_associate(boolean_t connected) {
    uint64_t begin = now_ns();
    int injected = !connected && refuse;
    if (injected)
        refuse = 0;
    CGError result = injected ? kCGErrorFailure : CGAssociateMouseAndMouseCursorPosition(connected);
    if (result == kCGErrorSuccess)
        atomic_store(&observed, !connected);
    atomic_fetch_add(&transitions, 1);
    if (audit_file)
        fprintf(audit_file, "associate,%llu,%d,%u,%d,%d,%llu\n", (unsigned long long)begin,
                getpid(), connected, result, injected, (unsigned long long)(now_ns() - begin));
    return result;
}
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement, *original;
} interpose[] = {
    {(const void *)audit_associate, (const void *)CGAssociateMouseAndMouseCursorPosition}};
