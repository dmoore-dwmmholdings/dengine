/* DENG-53 hooks; DENG-55 supplies congestion control without changing channel routing. */
#include "net/net_internal.h"
u32 de__cc_send_budget(const de__conn *conn) {
    (void)conn;
    return UINT32_MAX;
}
void de__cc_on_ack(de__conn *conn, u32 bytes, u64 rtt_ns) {
    (void)conn;
    (void)bytes;
    (void)rtt_ns;
}
void de__cc_on_loss(de__conn *conn, u32 bytes) {
    (void)conn;
    (void)bytes;
}
