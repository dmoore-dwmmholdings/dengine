/* socket.c: the portable part of the transport (DENG-51): de__socket wrappers with stats, the
 * `net.socket_buffer_bytes` cvar, the `net` log category and de_net_resolve. The OS parts are
 * socket_posix.c / socket_win32.c; the in-process backend is socket_loopback.c. */
#include "net/net_internal.h"

#include <dengine/de_debug.h>

#include <string.h>

de_log_category de__net_log(void) {
    /* Registration returns the existing category after the first call (it takes a lock, so this
     * is for open/error paths, not per datagram). */
    return de_log_register_category("net", DE_LOG_INFO);
}

u32 de__net_socket_buffer_bytes(u32 desc_bytes) {
    if (desc_bytes != 0)
        return desc_bytes;
    /* Registering again returns the same handle, so no handle is cached in a global. */
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = "net.socket_buffer_bytes";
    d.help = "UDP socket SO_RCVBUF/SO_SNDBUF request in bytes (the OS may grant less)";
    d.type = DE_CVAR_INT;
    d.default_number = (f64)DE__NET_SOCKET_BUFFER_DEFAULT;
    d.min = 64.0 * 1024.0;
    d.max = 256.0 * 1024.0 * 1024.0;
    de_cvar cv = de_cvar_register(&d);
    if (!DE_HANDLE_VALID(cv))
        return DE__NET_SOCKET_BUFFER_DEFAULT;
    i64 v = de_cvar_int(cv);
    return v > 0 ? (u32)v : DE__NET_SOCKET_BUFFER_DEFAULT;
}

de_result de__socket_send_to(de__socket *s, const de_net_address *to, const void *data, u32 bytes,
                             u32 *sent) {
    u32 n = 0;
    if (sent != NULL)
        *sent = 0;
    if (s == NULL || to == NULL || (data == NULL && bytes > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net send: NULL argument");
    de_result r = s->send_to(s, to, data, bytes, &n);
    if (r == DE_OK) {
        if (n > 0 || bytes == 0) {
            s->stats.datagrams_sent++;
            s->stats.bytes_sent += n;
        } else {
            s->stats.send_would_block++;
        }
    }
    if (sent != NULL)
        *sent = n;
    return r;
}

de_result de__socket_recv_from(de__socket *s, de__datagram *msgs, u32 count, u32 *received) {
    if (received == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net recv: received is NULL");
    *received = 0;
    if (s == NULL || (msgs == NULL && count > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net recv: NULL argument");
    if (count == 0)
        return DE_OK;
    u32 n = 0;
    de_result r = s->recv_from(s, msgs, count, &n);
    s->stats.datagrams_received += n;
    for (u32 i = 0; i < n; ++i)
        s->stats.bytes_received += msgs[i].bytes;
    *received = n;
    return r;
}

void de__socket_close(de__socket *s) {
    if (s != NULL)
        s->close(s);
}

de_result de_net_resolve(const char *text, de_net_family family, de_net_address *out, u32 max,
                         u32 *count) {
    if (count != NULL)
        *count = 0;
    if (text == NULL || out == NULL || count == NULL || max == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_resolve: NULL argument or max 0");
    if (family != DE_NET_FAMILY_NONE && family != DE_NET_FAMILY_IPV4 &&
        family != DE_NET_FAMILY_IPV6)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_resolve: bad family %d", (int)family);

    /* A numeric address needs no resolver. The parse error of a host name is expected here, so
     * the error context is set again below either way. */
    de_net_address a;
    if (de_net_address_parse(text, &a) == DE_OK) {
        if (family != DE_NET_FAMILY_NONE && a.family != (u16)family)
            return DE_FAIL(DE_ERR_NOT_FOUND, "de_net_resolve: '%s' is not of the requested family",
                           text);
        out[0] = a;
        *count = 1;
        return DE_OK;
    }

    char host[256];
    u16 port = 0;
    de_result r = de__net_split_host_port(text, host, (u32)sizeof host, &port);
    if (r != DE_OK)
        return r;
    return de__net_os_resolve(host, family, port, out, max, count);
}
