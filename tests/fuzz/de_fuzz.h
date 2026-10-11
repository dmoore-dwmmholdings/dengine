/*
 * de_fuzz.h: the fuzz-target contract (SPEC §19.1: one libFuzzer target per parser).
 *
 * A target defines LLVMFuzzerTestOneInput() and nothing else: no main(). de_add_fuzz_target()
 * (cmake/DeTesting.cmake) links it either with libFuzzer (-fsanitize=fuzzer,address, when the
 * compiler supports it) or with de_fuzz_standalone.c, which replays corpus files. Either way the
 * target runs as a CTest entry with the label `fuzz`.
 *
 * Rules for a target:
 *   - Return 0. Report a bug by crashing (abort(), a failed assert, a sanitizer report).
 *   - Treat `data` as untrusted and read-only; do not keep pointers into it after returning.
 *   - Be deterministic: no clocks, no global state that survives between calls.
 *   - Free everything you allocate (LeakSanitizer runs under libFuzzer).
 *
 * Optional LLVMFuzzerInitialize() is not supported by the standalone runner; do one-time setup
 * lazily inside LLVMFuzzerTestOneInput instead.
 */
#ifndef DE_FUZZ_H
#define DE_FUZZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Called once per input. Must return 0. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* DE_FUZZ_H */
