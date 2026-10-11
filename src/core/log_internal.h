/* log_internal.h: shared internals of the logging module (src/core/log.c, log_sinks.c) and its
 * tests. Not a public header; include as "core/log_internal.h". SPEC §5.2, DENG-150. */
#ifndef DE_CORE_LOG_INTERNAL_H
#define DE_CORE_LOG_INTERNAL_H

#include <dengine/de_base.h>

#include <stddef.h>

/**
 * Called once after each drain batch, after the last record of the batch went to the sinks, with
 * the drain lock held. The file sink flushes its FILE here; tests use it to see batch borders.
 */
typedef void (*de__log_batch_end_fn)(void *user);

/**
 * de_log_add_sink with a batch-end callback. `batch_end` may be NULL. Returns false when the sink
 * table is full or the (fn, user) pair is already there.
 */
bool de__log_add_sink_ex(de_log_sink_fn fn, de__log_batch_end_fn batch_end, void *user);

/**
 * Best-effort flush for the panic path (src/core/panic.c). Never blocks for long: if another
 * thread holds the drain lock it waits at most about 100 ms, and it returns at once when the
 * calling thread is itself inside a drain (a sink that panicked).
 */
void de__log_flush_for_panic(void);

struct de_allocator;
/**
 * The allocator for log objects (thread buffers, file sinks): a copy of the de_log_desc
 * allocator while logging is initialized, the system heap otherwise. Keep the copy with the
 * object so it is freed by the allocator that made it, even across de_log_shutdown().
 */
void de__log_allocator(struct de_allocator *out);

/** Writes `rec` to stderr in the console format. Shared by the console sink and the
 * synchronous path used before de_log_init(). */
void de__log_write_console(const de_log_record *rec);

/** The file name part of a path (after the last '/' or '\\'). NULL gives "?". */
const char *de__log_basename(const char *path);

/** Counters for tests. Monotonic over the process lifetime. */
typedef struct de__log_counters {
    u64 formatted; /**< messages the CALLING THREAD formatted in de_log_ (both paths) */
    u64 dropped;   /**< messages dropped because a thread buffer was full */
    u64 batches;   /**< drain batches run */
    u64 rings;     /**< thread buffers allocated (not reused ones) */
    u64 recycled;  /**< thread buffers taken over from a thread that exited */
} de__log_counters;
de__log_counters de__log_get_counters(void);

#endif /* DE_CORE_LOG_INTERNAL_H */
