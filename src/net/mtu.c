/* DENG-53 hook; DENG-54 replaces the fixed limit with MTU discovery/fragmentation. */
#include "net/net_internal.h"
u32 de__mtu_payload(const de__conn *conn) {
    (void)conn;
    return 1200;
}
