/* fpenv.c: per-thread floating-point environment (DENG-243, SPEC §5.7 rule 4).
 *
 * The deterministic modes are set on every thread that runs simulation code: flush denormal
 * results to zero, treat denormal inputs as zero, round to nearest even, all exceptions masked.
 * The S04 task scheduler calls de_fp_env_init_thread() on each worker. It reads the control
 * register and writes it only when it differs, so calling it again is cheap.
 *
 *   x86-64:  MXCSR FTZ (bit 15) and DAZ (bit 6) set, RC (bits 13-14) = 00, the six exception
 *            masks (bits 7-12) set. x87 is never used for f32/f64 on x86-64.
 *   arm64:   FPCR FZ (bit 24) set, RMode (bits 22-23) = 00, the trap enables (IOE, DZE, OFE,
 *            UFE, IXE: bits 8-12, IDE: bit 15) clear, and AH (bit 1) and FIZ (bit 0) clear so
 *            FEAT_AFP cores use the standard flush behaviour. FZ on arm64 flushes denormal
 *            inputs and outputs of f32 and f64 operations, like FTZ + DAZ.
 *
 * Other targets (none in v1, DENG-2 Q3): init does nothing and check returns false.
 *
 * The de_d* functions do not rely on these modes (dmath_reduce.h); plain f32 arithmetic in
 * simulation code does.
 */
#include <dengine/de_math.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define DE__FPENV_X86 1
/* FTZ | DAZ | exception masks; RC must be 00. */
#define DE__MXCSR_SET  (0x8000u | 0x0040u | 0x1F80u)
#define DE__MXCSR_MASK (DE__MXCSR_SET | 0x6000u)
#elif defined(__aarch64__) || defined(_M_ARM64)
#define DE__FPENV_ARM64 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
/* ARM64_SYSREG(3, 3, 4, 4, 0): FPCR. */
#define DE__FPCR_REG 0x5A20
#endif
#define DE__FPCR_FZ   (1ull << 24)
#define DE__FPCR_MASK (DE__FPCR_FZ | (3ull << 22) | (0x1Full << 8) | (1ull << 15) | 0x3ull)
#endif

#if defined(DE__FPENV_ARM64)
static inline u64 de__fpcr_read(void) {
#if defined(_MSC_VER) && !defined(__clang__)
    return (u64)_ReadStatusReg(DE__FPCR_REG);
#else
    u64 v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
#endif
}
static inline void de__fpcr_write(u64 v) {
#if defined(_MSC_VER) && !defined(__clang__)
    _WriteStatusReg(DE__FPCR_REG, (__int64)v);
#else
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
#endif
}
#endif

void de_fp_env_init_thread(void) {
#if defined(DE__FPENV_X86)
    unsigned int cur = _mm_getcsr();
    unsigned int want = (cur & ~DE__MXCSR_MASK) | DE__MXCSR_SET;
    if (cur != want) {
        _mm_setcsr(want);
    }
#elif defined(DE__FPENV_ARM64)
    u64 cur = de__fpcr_read();
    u64 want = (cur & ~DE__FPCR_MASK) | DE__FPCR_FZ;
    if (cur != want) {
        de__fpcr_write(want);
    }
#endif
}

bool de_fp_env_check(void) {
#if defined(DE__FPENV_X86)
    return (_mm_getcsr() & DE__MXCSR_MASK) == DE__MXCSR_SET;
#elif defined(DE__FPENV_ARM64)
    return (de__fpcr_read() & DE__FPCR_MASK) == DE__FPCR_FZ;
#else
    return false;
#endif
}
