/* Process-lifetime crash handoff. Normal operations are service-thread only; report may run
 * on any faulting thread. No service/connection object is dereferenced by the callback. */
#ifndef DE_HOST_EDITOR_CRASH_H
#define DE_HOST_EDITOR_CRASH_H
#include <dengine/de_debug.h>
#include "de_editor_transport.h"

de_result de__ed_crash_register(void);
bool de__ed_crash_arm(const de_ep_conn *conn, u64 next_seq);
/* False means another writer owns the handle: do not close it. Fatal ownership lasts until
 * process termination. Normal callers run only on the service thread. */
bool de__ed_crash_disarm(void);
bool de__ed_crash_frozen(void);
de_ep_io de__ed_crash_send(de_ep_conn *conn, u8 kind, const void *bytes, size_t n, u64 next_seq);
/* Also used by isolated fault-path tests; terminal, never followed by normal service work. */
void de__ed_crash_report(const de_crash_context *ctx, void *user);
#endif
