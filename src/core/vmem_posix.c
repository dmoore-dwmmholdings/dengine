/* vmem_posix.c: virtual memory shim for macOS and Linux (see vmem.h). */
#if !defined(_WIN32)

/* MAP_ANON and MAP_NORESERVE are not POSIX: ask for the platform extensions. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "core/vmem.h"

#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
/* Reserved ranges are PROT_NONE and never touched, so they need no swap backing (Linux strict
 * overcommit would otherwise count them). macOS has no such accounting. */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

size_t de__vmem_page_size(void) {
    /* Cheap (no system call on macOS or glibc) and only asked when an arena is created. */
    long v = sysconf(_SC_PAGESIZE);
    return v > 0 ? (size_t)v : 4096u;
}

size_t de__vmem_reserve_granularity(void) {
    return de__vmem_page_size();
}

void *de__vmem_reserve(size_t size) {
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

bool de__vmem_commit(void *p, size_t size) {
    /* Pages of a fresh anonymous mapping are zero-fill-on-demand: RSS grows only when touched. */
    return mprotect(p, size, PROT_READ | PROT_WRITE) == 0;
}

void de__vmem_decommit(void *p, size_t size) {
    /* Mapping a fresh PROT_NONE range over the old one frees its pages at once on both macOS and
     * Linux (madvise(MADV_DONTNEED) does not on macOS), and a later commit reads zeros. */
    void *r =
        mmap(p, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    (void)r;
}

void de__vmem_release(void *p, size_t size) {
    munmap(p, size);
}

#else
/* ISO C forbids an empty translation unit. */
typedef int de__vmem_posix_unused;
#endif
