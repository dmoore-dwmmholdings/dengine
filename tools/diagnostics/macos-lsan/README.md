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

## libFuzzer RSS-thread lifetime remedy (DENG-559)

The installed LLVM 23.1.3 runtime's empty input callback reproduces a 56-byte,
two-allocation LeakSanitizer exit report without engine code. Its RSS watchdog
detaches an infinite thread; disassembly and symbolized stacks identify libc++
thread state. Explicit linkage to the matching bundled libc++ still reports the
same leak, so library pairing alone does not fix this host's result.

`build_fuzzer_runtime.py` creates a private arm64 archive from the installed
runtime, replacing only `FuzzerDriver.cpp.o` rebuilt from upstream LLVM commit
`0d261d1ca552c95a8f007e061c787ac7132fbcbc` (23.1.3). It verifies downloaded Git
blob hashes, records archive/compiler/patch provenance and never changes the
installed compiler. `fuzzer-rss-lifetime.patch` retains the one-second RSS check
and original out-of-memory callback, but wakes and joins the watchdog during
normal process exit, before leak checking. It adds no suppressions or sanitizer
settings. This is a version-scoped downstream toolchain patch, not an
upstream-supported runtime release. It is qualified on the observed macOS arm64
host; it does not clear unrelated engine sanitizer reports.

```sh
python3 tools/diagnostics/macos-lsan/build_fuzzer_runtime.py --out /tmp/fuzzer-joined
```

The script emits `clang-resource/`, which keeps normal Clang runtime selection
while replacing only libFuzzer. Its other resources link to the exact installed
toolchain, not the moving Homebrew `opt` alias; that installation must remain
available. Regenerate the directory after changing toolchains. No global compiler
configuration is modified, and unsupported compiler versions are rejected.

The script automatically builds/runs the no-engine `fuzzer_control.c` through
this private resource directory with normal `-fsanitize=fuzzer,address` linking.
MODE=0 is empty; MODE=1 intentionally leaks 1 KiB; MODE=2 sleeps while the RSS
watchdog runs. All use `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`, no LSAN
suppressions, and `-runs=0`. Only the MODE=2 negative adds `-rss_limit_mb=1` to prove
the watchdog still rejects excess resident memory. On the observed host, the
patched empty control exits 0; the real-leak control exits 1 with LeakSanitizer;
the watchdog control exits 71 with the actual RSS out-of-memory diagnostic.
The original installed runtime's empty control still exits 1 with the 56-byte
report. `controls.json`, individual logs and `provenance.json` retain evidence.
No production fuzz target or default configuration is switched automatically.

For an instrumented engine build, retain the normal fuzz flags and explicitly
select the private resource directory for this build only:

```sh
cmake -S . -B build/fuzz-joined -G Ninja -DCMAKE_BUILD_TYPE=Dev \
    -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
    -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
    '-DCMAKE_C_FLAGS=-resource-dir=/tmp/fuzzer-joined/clang-resource -fsanitize=fuzzer-no-link' \
    '-DCMAKE_CXX_FLAGS=-resource-dir=/tmp/fuzzer-joined/clang-resource -fsanitize=fuzzer-no-link' \
    -DDE_SANITIZE=address,undefined -DDE_FUZZ_LIBFUZZER=ON
cmake --build build/fuzz-joined --target fuzz_sprite_clip
mkdir -p /tmp/sprite-fuzz-corpus
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 LSAN_OPTIONS= UBSAN_OPTIONS=halt_on_error=1 \
    build/fuzz-joined/tests/fuzz/anim/fuzz_sprite_clip \
    -max_total_time=60 -timeout=10 -malloc_limit_mb=256 \
    /tmp/sprite-fuzz-corpus tests/fuzz/anim/corpus/sprite_clip
```

Actual combined engine source `87a32f5f` completed 3,399,418 sprite importer/loader
inputs in 61 seconds and exited 0 with the patched runtime. The importer and fuzz
target both retain coverage instrumentation plus ASan/UBSan; default RSS checking,
leak detection, timeout and allocation limits remain active. Peak RSS was 869 MiB.
The exact same compiled engine objects, linked against the original runtime,
still fail with the original 56-byte/two-allocation report. This paired control
distinguishes runtime lifetime cleanup from parser or sanitizer weakening.
Logs: `/tmp/deng190-joined-fuzz60.log`, `/tmp/deng190-original-runtime-control.log`;
paired link commands: `/tmp/deng190-fuzz-runtime-pair-link.json`.

This qualifies that original fuzz workload with the documented private runtime
remedy; it does not claim that the unmodified installed runtime is clean, that
other fuzz targets have run, or that DENG-551's native watcher/Metal reports are
resolved. Review and merge of the remedy remain separate from its test evidence.

Upstream: https://github.com/llvm/llvm-project/blob/0d261d1ca552c95a8f007e061c787ac7132fbcbc/compiler-rt/lib/fuzzer/FuzzerDriver.cpp
