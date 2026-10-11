/* fpenv_util.h: raw access to the FP control register, for tests that change the FP mode and
 * must put it back (DENG-243). Test-only. */
#ifndef DE_TEST_FPENV_UTIL_H
#define DE_TEST_FPENV_UTIL_H

#include <dengine/de_base.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define DE_TEST_FPENV_KNOWN 1
/* Rounding control (MXCSR bits 13-14): 01 = round down. */
#define DE_TEST_FPENV_ROUND_DOWN(v) (((v) & ~(u64)0x6000u) | 0x2000u)
static inline u64 de_test_fpenv_get(void) {
    return (u64)_mm_getcsr();
}
static inline void de_test_fpenv_set(u64 v) {
    _mm_setcsr((unsigned int)v);
}
#elif defined(__aarch64__) || defined(_M_ARM64)
#define DE_TEST_FPENV_KNOWN         1
/* RMode (FPCR bits 22-23): 10 = round toward minus infinity. */
#define DE_TEST_FPENV_ROUND_DOWN(v) (((v) & ~(3ull << 22)) | (2ull << 22))
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline u64 de_test_fpenv_get(void) {
    return (u64)_ReadStatusReg(0x5A20);
}
static inline void de_test_fpenv_set(u64 v) {
    _WriteStatusReg(0x5A20, (__int64)v);
}
#else
static inline u64 de_test_fpenv_get(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
}
static inline void de_test_fpenv_set(u64 v) {
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
}
#endif
#else
#define DE_TEST_FPENV_KNOWN         0
#define DE_TEST_FPENV_ROUND_DOWN(v) (v)
static inline u64 de_test_fpenv_get(void) {
    return 0;
}
static inline void de_test_fpenv_set(u64 v) {
    (void)v;
}
#endif

#endif
