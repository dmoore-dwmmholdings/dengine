/* Supervised macOS acceptance helper. No NSApplication, window, activation or focus calls.
 * Observe before spawning the exact child; PID-only sampling is deliberately insufficient. */
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static volatile sig_atomic_t stop_requested;
static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static bool window_counts(pid_t pid, NSUInteger *all, NSUInteger *onscreen) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
    if (!list)
        return false;
    *all = *onscreen = 0;
    for (NSDictionary *window in (__bridge NSArray *)list) {
        if ([window[(__bridge NSString *)kCGWindowOwnerPID] intValue] != pid)
            continue;
        ++*all;
        if ([window[(__bridge NSString *)kCGWindowIsOnscreen] boolValue])
            ++*onscreen;
    }
    CFRelease(list);
    return true;
}
static void pump(NSTimeInterval duration) {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:duration];
    do {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:end];
    } while ([end timeIntervalSinceNow] > 0);
}
int main(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[1], "--report") || strcmp(argv[3], "--")) {
        fprintf(stderr, "usage: de-offscreen-observe --report <json> -- <fixture> [arguments]\n");
        return 2;
    }
    @autoreleasepool {
        NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
        NSNotificationCenter *center = workspace.notificationCenter;
        pid_t original_foreground = workspace.frontmostApplication.processIdentifier;
        __block pid_t child = 0;
        __block NSUInteger space_changes = 0, activations = 0, child_activations = 0;
        id spaces = [center addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification
                                        object:nil
                                         queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification *n) {
                                      (void)n;
                                      ++space_changes;
                                    }];
        id foreground = [center addObserverForName:NSWorkspaceDidActivateApplicationNotification
                                            object:nil
                                             queue:[NSOperationQueue mainQueue]
                                        usingBlock:^(NSNotification *n) {
                                          ++activations;
                                          NSRunningApplication *app =
                                              n.userInfo[NSWorkspaceApplicationKey];
                                          if (app.processIdentifier == child)
                                              ++child_activations;
                                        }];
        (void)signal(SIGTERM, on_signal);
        (void)signal(SIGINT, on_signal);
        NSTimeInterval begin = NSProcessInfo.processInfo.systemUptime;
        pid_t launched = 0;
        int spawn_error = posix_spawnp(&launched, argv[4], NULL, NULL, &argv[4], environ);
        child = launched;
        if (spawn_error) {
            fprintf(stderr, "offscreen observer: spawn failed: %d\n", spawn_error);
            [center removeObserver:spaces];
            [center removeObserver:foreground];
            return 2;
        }
        NSUInteger polls = 0, census_errors = 0, max_windows = 0, max_visible = 0;
        NSUInteger child_frontmost = 0, changed_frontmost = 0;
        NSTimeInterval max_poll_gap = 0, previous = begin, stop_at = 0;
        bool timed_out = false, signalled = false;
        int status = 0;
        for (;;) {
            NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
            if (now - previous > max_poll_gap)
                max_poll_gap = now - previous;
            previous = now;
            ++polls;
            pid_t active = workspace.frontmostApplication.processIdentifier;
            if (active == child)
                ++child_frontmost;
            if (active != original_foreground)
                ++changed_frontmost;
            NSUInteger all = 0, visible = 0;
            if (!window_counts(child, &all, &visible))
                ++census_errors;
            if (all > max_windows)
                max_windows = all;
            if (visible > max_visible)
                max_visible = visible;
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result < 0 && errno == EINTR)
                continue; /* Still own the child: process the pending stop on the next poll. */
            if (result == child)
                break;
            if (result < 0) {
                status = -1;
                break;
            }
            if (now - begin > 600)
                timed_out = true;
            if ((timed_out || stop_requested) && !signalled) {
                /* Only this unreaped, owned child can have this PID. */
                (void)kill(child, SIGTERM);
                signalled = true;
                stop_at = now;
            } else if (signalled && now - stop_at > 2) {
                (void)kill(child, SIGKILL);
            }
            pump(0.01);
        }
        /* Drain notifications posted just before process exit. */
        pump(0.2);
        pid_t final_foreground = workspace.frontmostApplication.processIdentifier;
        [center removeObserver:spaces];
        [center removeObserver:foreground];
        int exit_code = status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        bool observed_quiet = original_foreground > 0 && original_foreground == final_foreground &&
                              !space_changes && !activations && !child_activations &&
                              !child_frontmost && !changed_frontmost && !max_windows &&
                              !max_visible && !census_errors;
        bool ok = observed_quiet && exit_code == 0 && !timed_out && !signalled;
        NSDictionary *report = @{
            @"ok" : @(ok),
            @"child_pid" : @(child),
            @"child_exit" : @(exit_code),
            @"original_foreground_pid" : @(original_foreground),
            @"final_foreground_pid" : @(final_foreground),
            @"space_changes" : @(space_changes),
            @"activation_notifications" : @(activations),
            @"child_activations" : @(child_activations),
            @"child_foreground_samples" : @(child_frontmost),
            @"changed_foreground_samples" : @(changed_frontmost),
            @"max_child_windows" : @(max_windows),
            @"max_visible_child_windows" : @(max_visible),
            @"window_census_errors" : @(census_errors),
            @"polls" : @(polls),
            @"maximum_poll_gap_ms" : @(max_poll_gap * 1000),
            @"timed_out" : @(timed_out),
            @"signalled" : @(signalled),
            @"seconds" : @(NSProcessInfo.processInfo.systemUptime - begin),
            @"limits" :
                @"Window inventory is sampled. Full-screen context and no perceived "
                 "interruption require the supervising user; no PID-only or universal proof."
        };
        NSError *error = nil;
        NSData *data = [NSJSONSerialization dataWithJSONObject:report
                                                       options:NSJSONWritingPrettyPrinted
                                                         error:&error];
        bool saved = data && [data writeToFile:[NSString stringWithUTF8String:argv[2]]
                                       options:NSDataWritingAtomic
                                         error:&error];
        if (!saved)
            fprintf(stderr, "offscreen observer: report write failed\n");
        return saved && ok ? 0 : 1;
    }
}
