# Standalone macOS LeakSanitizer controls

These DENG-551 diagnostics distinguish runtime allocation reports from engine
ownership. They deliberately call no engine APIs and do not link engine libraries.
They are investigative controls, not replacement acceptance tests. A clean result
does not prove the engine leak-free; a leak report does not prove every similar
engine allocation external. No engine test, threshold, suppression, or timeout is
changed by running them.

From the repository root, use the same Clang installation as the sanitizer engine
build (see `CMAKE_C_COMPILER` in its `CMakeCache.txt`):

```sh
clang -x objective-c -std=c11 -fobjc-arc -O2 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -fno-sanitize-recover=undefined -Wall -Wextra -Wpedantic -Werror \
  tools/diagnostics/macos-lsan/control.m \
  -framework Foundation -framework Metal -o /tmp/deng-macos-lsan-control

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=0 \
  /tmp/deng-macos-lsan-control metal

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/deng-macos-lsan-control dispatch
```

The Metal mode creates three queues, submits an empty blit command buffer to each,
waits for completion, checks its status, and releases all ARC references inside
autorelease pools. It returns 77 if no Metal device is available. Keep Metal layer
settings identical to the engine comparison.

The dispatch mode uses only Foundation/libdispatch APIs. It creates 256 chained
groups on a serial queue, waits for the last group and all 256 notifications, and
synchronously drains that queue before releasing its references. The executable
links Metal for the other
mode, but dispatch mode never creates a Metal device or submits GPU work.

Successful control work returns 0. Usage errors return 2 and control failures
return 1. LeakSanitizer may independently turn an otherwise successful run into a
nonzero process exit; retain the whole report, compiler version, macOS version,
architecture, environment, and the engine commit being compared. Do not infer
ownership from changing byte counts. Analysis and results live in the Trama DENG
space under `Testing: macOS sanitizer runtime controls`.

## FSEvents lifecycle isolation

`fsevents.c` is a separate C executable linking only CoreServices and libdispatch,
with no engine, Foundation application code or Metal API calls. Use the same
compiler and sanitizer settings as the engine comparison:

```sh
clang -std=c11 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -fno-sanitize-recover=undefined -Wall -Wextra -Wpedantic -Werror \
  tools/diagnostics/macos-lsan/fsevents.c \
  -framework CoreServices -o /tmp/deng-fsevents-lsan-control

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/deng-fsevents-lsan-control create 1
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/deng-fsevents-lsan-control schedule 1
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/deng-fsevents-lsan-control start 1
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/deng-fsevents-lsan-control start 32
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/deng-fsevents-lsan-control start 32 finalize
```

The modes isolate stream creation/release, dispatch scheduling/invalidation,
and successful start/flush/stop/invalidate/release. Each iteration owns and
releases a private serial queue and semaphore. Queue drains run before stop and
after stream release; no sleep or longer engine acceptance deadline is added.
The native event flags and latency match `src/platform/watch_fsevents.c`.
The control reports requested, created, scheduled and successfully started
stream counts. A failed start sets the control result to 1 and is never called
a successful lifecycle. `create` intentionally never schedules the stream and
therefore does not invalidate it; `schedule` and `start` invalidate while still
scheduled, following Apple's API contract:
[FSEventStreamSetDispatchQueue](https://developer.apple.com/documentation/coreservices/1444164-fseventstreamsetdispatchqueue).

The optional `finalize` argument also observes each private queue's actual
finalizer after releasing it. It waits for that completion signal for at most
two seconds and reports the observed count; a timeout fails the control and
stops further iterations. This is an additional diagnostic, not an engine
timeout change or an arbitrary sleep. The semaphore is retained independently
by its finalizer and caller, so a timeout does not leave a dangling stack
context. Queue finalization proves that queue reached disposal; it does not
prove all framework allocations have been freed. See Apple's
[queue finalizer contract](https://developer.apple.com/library/archive/documentation/General/Conceptual/ConcurrencyProgrammingGuide/OperationQueues/OperationQueues.html).

Run each comparison in a fresh process. Comparing one and 32 lifecycles can
distinguish repeated stream allocation growth from initialization reports, but
neither result clears the engine's sanitizer gate. These controls do not verify
native event delivery, hot-reload behavior, or all engine-owned lifetimes.
