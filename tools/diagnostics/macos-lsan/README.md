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
