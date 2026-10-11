/*
 * vmem.h: virtual memory shim (internal to core; include as "core/vmem.h").
 *
 * Reserve address space, commit and decommit pages inside it, release it. POSIX uses
 * mmap(PROT_NONE) + mprotect (vmem_posix.c); Windows uses VirtualAlloc/VirtualFree
 * (vmem_win32.c). Both files are always in the source list and compile to nothing on the other
 * platform.
 *
 * Addresses and sizes passed to commit/decommit/release must be multiples of
 * de__vmem_page_size() and lie inside one reservation. Freshly committed memory reads as zero,
 * also after a decommit + commit of the same range.
 */
#ifndef DE_CORE_VMEM_H
#define DE_CORE_VMEM_H

#include <dengine/de_base.h>

/** OS page size: the commit granularity. A power of two. */
size_t de__vmem_page_size(void);

/** Granularity of reservation addresses and sizes (64 KiB on Windows, the page size elsewhere). */
size_t de__vmem_reserve_granularity(void);

/** Reserves `size` bytes of inaccessible address space. NULL on failure. No error context. */
void *de__vmem_reserve(size_t size);

/** Makes [p, p + size) readable and writable. false on failure. No error context. */
bool de__vmem_commit(void *p, size_t size);

/** Gives the pages of [p, p + size) back to the OS and makes them inaccessible again. */
void de__vmem_decommit(void *p, size_t size);

/** Releases a whole reservation made by de__vmem_reserve(size). */
void de__vmem_release(void *p, size_t size);

#endif /* DE_CORE_VMEM_H */
