/* vmem_win32.c: virtual memory shim for Windows (see vmem.h). */
#if defined(_WIN32)

#include "core/vmem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/* GetSystemInfo reads process data without a system call; it is only asked when an arena is
 * created, so it is not cached (a cache would be a data race between first callers). */
size_t de__vmem_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? (size_t)si.dwPageSize : 4096u;
}

size_t de__vmem_reserve_granularity(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwAllocationGranularity ? (size_t)si.dwAllocationGranularity : 65536u;
}

void *de__vmem_reserve(size_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool de__vmem_commit(void *p, size_t size) {
    /* Committed pages are zero-filled on first touch and count against the commit charge. */
    return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void de__vmem_decommit(void *p, size_t size) {
#if defined(_MSC_VER) && !defined(__clang__)
/* C6250: MEM_DECOMMIT without MEM_RELEASE leaves the range reserved, which is the point. */
#pragma warning(suppress : 6250)
#endif
    VirtualFree(p, size, MEM_DECOMMIT);
}

void de__vmem_release(void *p, size_t size) {
    (void)size; /* MEM_RELEASE frees the whole reservation and requires size 0 */
    VirtualFree(p, 0, MEM_RELEASE);
}

#else
/* ISO C forbids an empty translation unit. */
typedef int de__vmem_win32_unused;
#endif
