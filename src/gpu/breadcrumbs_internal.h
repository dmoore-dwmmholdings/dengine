/* breadcrumbs_internal.h — GPU crash diagnostics in the de_gpu front-end (DENG-328; SPEC §18.5,
 * §12.1, §12.3). Internal to src/gpu/; gpu.c calls these, breadcrumbs.c implements them. The
 * backend side (cmd_breadcrumb, device_lost, device_lost_report) is in gpu_backend.h.
 *
 * Breadcrumbs. With de_gpu_device_desc.crash_breadcrumbs each queue has a ring buffer of
 * DE__GPU_BC_SLOTS u32 slots in READBACK memory. de_gpu_cmd_breadcrumb(c, id) gives the
 * breadcrumb the queue's next sequence number s (command lists reserve DE__GPU_BC_CHUNK numbers
 * at a time from an atomic counter, so recording takes no lock and allocates nothing), stores id
 * and the open marker path (DENG-326) in slot s % DE__GPU_BC_SLOTS of a CPU table, and records a
 * GPU write of s into that slot of the ring once all earlier work of the list has completed.
 * Each submit logs the chunks of its lists with the queue value, in submission order. A
 * breadcrumb has completed when its ring slot holds its own sequence number; the report walks the
 * log in order and names the last completed breadcrumb and those not reached after it.
 *
 * Device lost. The first front-end call that gets DE_ERR_DEVICE_LOST from the backend (or finds
 * the backend's device_lost poll true) builds the report once into a preallocated buffer, copies
 * it into a process-wide static buffer for the crash handler, sets the `gpu_fault` crash
 * annotation and calls the device-lost callback. From then on every call fails fast with
 * DE_ERR_DEVICE_LOST and recording calls do nothing, without calling the backend. Destroy calls
 * and de_gpu_device_destroy still release objects.
 *
 * Crash handler. The first device made registers one chained crash handler for the process
 * (de_crash_add_handler). When a crash report is written after a device was lost, it writes
 * `<stem>.gpu_report.txt` from the static copy and appends `gpu_report: <path>` to the report.
 * It is async-signal-safe: preallocated memory, DENG-251's open/write/close helpers.
 */
#ifndef DE_GPU_BREADCRUMBS_INTERNAL_H
#define DE_GPU_BREADCRUMBS_INTERNAL_H
#include <dengine/de_debug.h>

#include "gpu/gpu_backend.h"

/** Submit-log entries kept per queue (one per used chunk of each submitted list). */
#define DE__GPU_BC_LOG 512u
/** Bytes of marker path kept per breadcrumb (cut). */
#define DE__GPU_BC_PATH 40u
/** Not-reached breadcrumbs listed per queue in a report (the rest are counted). */
#define DE__GPU_BC_REPORT_PENDING 16u

/**
 * Sets up crash diagnostics for a new device, after its pools exist: the report buffer always;
 * the breadcrumb rings with desc.crash_breadcrumbs; the process's crash handler (once).
 * DE_ERR_OUT_OF_MEMORY or the ring buffer's create error.
 */
de_result de__gpu_crash_init(de_gpu_device *d);
/** Destroys the breadcrumb rings (start of de_gpu_device_destroy, before the leak check). */
void de__gpu_crash_release(de_gpu_device *d);
/** Frees what de__gpu_crash_init allocated (device free). */
void de__gpu_crash_free(de_gpu_device *d);

/** de_gpu_cmd_breadcrumb's work for a list outside a render pass. */
void de__gpu_breadcrumb_record(de_gpu_cmd *c, u32 id);
/** Logs the breadcrumbs of a submit's lists with its queue value. Under submit_lock[q]. */
void de__gpu_breadcrumb_submitted(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                                  u64 value);

/* ---------- Test hooks ---------- */

/**
 * Sequence number and id of the last breadcrumb of queue `q` whose GPU write landed, in
 * submission order (works on a device that is not lost). False when none has, or the device has
 * no breadcrumbs.
 */
bool de__gpu_breadcrumb_last(de_gpu_device *d, de_gpu_queue q, u32 *seq, u32 *id);
/** The ring buffer record of queue `q` (NULL without breadcrumbs). For tests that play the GPU. */
de__gpu_buffer *de__gpu_breadcrumb_ring(de_gpu_device *d, de_gpu_queue q);
/** Breadcrumbs dropped because a recording ran out of chunks, since device create. */
u32 de__gpu_breadcrumb_dropped(de_gpu_device *d);
/** The process-wide copy the crash handler writes ("" when no device was lost), and its length. */
const char *de__gpu_crash_report_copy(size_t *len);
/** The chained crash handler, for tests that call it with a context of their own. */
void de__gpu_crash_handler(const de_crash_context *ctx, void *user);

#endif
