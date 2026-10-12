/* socket_win32.c: non-blocking UDP sockets and the resolver on Windows (DENG-51).
 *
 * Winsock 2. WSAStartup is reference-counted by Windows itself: every open (and every resolve)
 * calls it once and the matching close calls WSACleanup, so no global is needed. SIO_UDP_CONNRESET
 * is switched off, or an ICMP port-unreachable for an earlier send makes the next recvfrom fail
 * with WSAECONNRESET. A datagram bigger than the caller's buffer (WSAEMSGSIZE) is dropped and
 * counted. No heap after open. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "net/net_internal.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <string.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef IP_DONTFRAGMENT
#define IP_DONTFRAGMENT 14
#endif
#ifndef IPV6_DONTFRAG
#define IPV6_DONTFRAG 14
#endif

typedef struct udp_state {
    de__socket sock; /* first: the allocation starts with the public part */
    SOCKET fd;
    int af; /* AF_INET or AF_INET6 */
    de__udp_info info;
    const de_allocator *alloc;
} udp_state;

static de_result wsa_start(void) {
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0)
        return DE_FAIL(DE_ERR_NETWORK, "net: WSAStartup failed: %d", rc);
    return DE_OK;
}

/* ---------- Address conversion ---------- */

static void put_port(u8 *dst, u16 port) {
    dst[0] = (u8)(port >> 8);
    dst[1] = (u8)(port & 0xFFu);
}

static u16 get_port(const u8 *src) {
    return (u16)((src[0] << 8) | src[1]);
}

/* For a socket of family `af`; IPv4 on an IPv6 socket becomes ::ffff:a.b.c.d. 0 = no fit. */
static int to_sockaddr(const de_net_address *a, int af, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof *ss);
    if (af == AF_INET) {
        if (a->family != DE_NET_FAMILY_IPV4)
            return 0;
        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)ss;
        sin->sin_family = AF_INET;
        put_port((u8 *)&sin->sin_port, a->port);
        memcpy(&sin->sin_addr, a->addr, 4);
        return (int)sizeof *sin;
    }
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)(void *)ss;
    sin6->sin6_family = AF_INET6;
    put_port((u8 *)&sin6->sin6_port, a->port);
    if (a->family == DE_NET_FAMILY_IPV4) {
        u8 *b = (u8 *)&sin6->sin6_addr;
        b[10] = 0xFF;
        b[11] = 0xFF;
        memcpy(b + 12, a->addr, 4);
    } else if (a->family == DE_NET_FAMILY_IPV6) {
        memcpy(&sin6->sin6_addr, a->addr, 16);
        sin6->sin6_scope_id = a->scope_id;
    } else {
        return 0;
    }
    return (int)sizeof *sin6;
}

static void from_sockaddr(const struct sockaddr_storage *ss, de_net_address *a) {
    memset(a, 0, sizeof *a);
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)ss;
        a->family = DE_NET_FAMILY_IPV4;
        a->port = get_port((const u8 *)&sin->sin_port);
        memcpy(a->addr, &sin->sin_addr, 4);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)(const void *)ss;
        a->family = DE_NET_FAMILY_IPV6;
        a->port = get_port((const u8 *)&sin6->sin6_port);
        memcpy(a->addr, &sin6->sin6_addr, 16);
        a->scope_id = (u32)sin6->sin6_scope_id;
    }
}

/* ---------- Send / receive / close ---------- */

static de_result udp_send_to(de__socket *s, const de_net_address *to, const void *data, u32 bytes,
                             u32 *sent) {
    udp_state *u = (udp_state *)s->user;
    *sent = 0;
    struct sockaddr_storage ss;
    int len = to_sockaddr(to, u->af, &ss);
    if (len == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "net send: address family %u does not fit the socket", (unsigned)to->family);
    if (bytes > DE__NET_MAX_UDP_PAYLOAD)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net send: %u bytes is more than UDP carries",
                       bytes);
    int r = sendto(u->fd, (const char *)data, (int)bytes, 0,
                   (const struct sockaddr *)(const void *)&ss, len);
    if (r != SOCKET_ERROR) {
        *sent = (u32)r;
        return DE_OK;
    }
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK || e == WSAENOBUFS)
        return DE_OK;
    if (e == WSAEMSGSIZE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "net send: %u bytes is more than the path MTU allows (Don't-Fragment)",
                       bytes);
    char addr[DE_NET_ADDRESS_STRING_MAX];
    de_net_address_to_string(to, addr, (u32)sizeof addr);
    return DE_FAIL(DE_ERR_NETWORK, "net send to %s: WSA error %d", addr, e);
}

static de_result udp_recv_from(de__socket *s, de__datagram *msgs, u32 count, u32 *received) {
    udp_state *u = (udp_state *)s->user;
    u32 n = 0;
    while (n < count) {
        struct sockaddr_storage from;
        int from_len = (int)sizeof from;
        u32 cap = msgs[n].capacity > (u32)0x7FFFFFFF ? (u32)0x7FFFFFFF : msgs[n].capacity;
        int r = recvfrom(u->fd, (char *)msgs[n].data, (int)cap, 0, (struct sockaddr *)(void *)&from,
                         &from_len);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK)
                break;
            if (e == WSAEMSGSIZE) {
                s->stats.truncated++;
                continue; /* the slot is reused for the next datagram */
            }
            /* ICMP errors about a peer (should not happen with SIO_UDP_CONNRESET off). */
            if (e == WSAECONNRESET || e == WSAENETRESET || e == WSAEHOSTUNREACH ||
                e == WSAENETUNREACH || e == WSAEINTR)
                continue;
            *received = n;
            return DE_FAIL(DE_ERR_NETWORK, "net recv: WSA error %d", e);
        }
        msgs[n].bytes = (u32)r;
        from_sockaddr(&from, &msgs[n].from);
        de__net_address_unmap(&msgs[n].from);
        ++n;
    }
    *received = n;
    return DE_OK;
}

static void udp_close(de__socket *s) {
    udp_state *u = (udp_state *)s->user;
    closesocket(u->fd);
    WSACleanup();
    DE_FREE(u->alloc, u, sizeof *u);
}

/* ---------- Open ---------- */

static de_result wsa_result(int e) {
    switch (e) {
    case WSAEAFNOSUPPORT:
    case WSAEPROTONOSUPPORT:
    case WSAEPFNOSUPPORT:
        return DE_ERR_UNSUPPORTED;
    case WSAEADDRINUSE:
    case WSAEACCES: /* Windows reports an exclusive-use port as access denied */
        return DE_ERR_ALREADY_EXISTS;
    case WSAEADDRNOTAVAIL:
        return DE_ERR_NOT_FOUND;
    case WSAENOBUFS:
        return DE_ERR_OUT_OF_MEMORY;
    default:
        return DE_ERR_NETWORK;
    }
}

static bool set_int(SOCKET fd, int level, int opt, int value) {
    return setsockopt(fd, level, opt, (const char *)&value, (int)sizeof value) == 0;
}

static void set_buffer(SOCKET fd, int opt, u32 bytes) {
    int v = bytes > (u32)0x7FFFFFFF ? 0x7FFFFFFF : (int)bytes;
    while (v >= 4096) {
        if (set_int(fd, SOL_SOCKET, opt, v))
            return;
        v /= 2;
    }
}

static u32 get_buffer(SOCKET fd, int opt) {
    int v = 0;
    int len = (int)sizeof v;
    if (getsockopt(fd, SOL_SOCKET, opt, (char *)&v, &len) != 0 || v < 0)
        return 0;
    return (u32)v;
}

static de_result udp_open_at(const de_net_address *bind_addr, const de__udp_desc *d,
                             const de_allocator *a, de__socket **out) {
    if (bind_addr->family != DE_NET_FAMILY_IPV4 && bind_addr->family != DE_NET_FAMILY_IPV6)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net udp open: bind address has no family");
    int af = bind_addr->family == DE_NET_FAMILY_IPV4 ? AF_INET : AF_INET6;

    de_result r = wsa_start();
    if (r != DE_OK)
        return r;
    udp_state *u = DE_NEW(a, udp_state, DE_MEM_NET);
    if (u == NULL) {
        WSACleanup();
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "net udp open: no memory for the socket");
    }
    memset(u, 0, sizeof *u);
    u->alloc = a;
    u->af = af;
    u->fd = WSASocketW(af, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (u->fd == INVALID_SOCKET) {
        int e = WSAGetLastError();
        DE_FREE(a, u, sizeof *u);
        WSACleanup();
        return DE_FAIL(wsa_result(e), "net udp open: socket(%s) failed: WSA error %d",
                       af == AF_INET ? "IPv4" : "IPv6", e);
    }

    char addr[DE_NET_ADDRESS_STRING_MAX];
    de_net_address_to_string(bind_addr, addr, (u32)sizeof addr);
    u_long nonblocking = 1;
    if (ioctlsocket(u->fd, FIONBIO, &nonblocking) != 0) {
        r = DE_FAIL(DE_ERR_NETWORK, "net udp open %s: cannot make it non-blocking: WSA error %d",
                    addr, WSAGetLastError());
        goto fail;
    }
    BOOL connreset = FALSE;
    DWORD ret_bytes = 0;
    if (WSAIoctl(u->fd, SIO_UDP_CONNRESET, &connreset, (DWORD)sizeof connreset, NULL, 0, &ret_bytes,
                 NULL, NULL) != 0) {
        r = DE_FAIL(DE_ERR_NETWORK, "net udp open %s: SIO_UDP_CONNRESET failed: WSA error %d", addr,
                    WSAGetLastError());
        goto fail;
    }
    if (af == AF_INET6) {
        /* Windows defaults to IPV6_V6ONLY = 1; dual stack must be asked for. */
        if (!set_int(u->fd, IPPROTO_IPV6, IPV6_V6ONLY, d->v6_only ? 1 : 0)) {
            r = DE_FAIL(DE_ERR_NETWORK, "net udp open %s: IPV6_V6ONLY failed: WSA error %d", addr,
                        WSAGetLastError());
            goto fail;
        }
        u->info.dual_stack = !d->v6_only;
    }
    u32 buf = de__net_socket_buffer_bytes(d->buffer_bytes);
    set_buffer(u->fd, SO_RCVBUF, buf);
    set_buffer(u->fd, SO_SNDBUF, buf);
    u->info.recv_buffer_bytes = get_buffer(u->fd, SO_RCVBUF);
    u->info.send_buffer_bytes = get_buffer(u->fd, SO_SNDBUF);
    if (!d->allow_fragments) {
        if (af == AF_INET6) {
            u->info.dont_fragment = set_int(u->fd, IPPROTO_IPV6, IPV6_DONTFRAG, 1);
            (void)set_int(u->fd, IPPROTO_IP, IP_DONTFRAGMENT, 1); /* mapped IPv4, best effort */
        } else {
            u->info.dont_fragment = set_int(u->fd, IPPROTO_IP, IP_DONTFRAGMENT, 1);
        }
        if (!u->info.dont_fragment)
            DE_LOG(DE_LOG_WARN, de__net_log(),
                   "net udp open %s: cannot set Don't-Fragment; MTU discovery will not work", addr);
    }

    struct sockaddr_storage ss;
    int len = to_sockaddr(bind_addr, af, &ss);
    if (bind(u->fd, (const struct sockaddr *)(const void *)&ss, len) != 0) {
        int e = WSAGetLastError();
        r = DE_FAIL(wsa_result(e), "net udp open: bind %s failed: WSA error %d", addr, e);
        goto fail;
    }
    struct sockaddr_storage local;
    int local_len = (int)sizeof local;
    if (getsockname(u->fd, (struct sockaddr *)(void *)&local, &local_len) != 0) {
        r = DE_FAIL(DE_ERR_NETWORK, "net udp open %s: getsockname failed: WSA error %d", addr,
                    WSAGetLastError());
        goto fail;
    }

    u->sock.send_to = udp_send_to;
    u->sock.recv_from = udp_recv_from;
    u->sock.close = udp_close;
    u->sock.user = u;
    from_sockaddr(&local, &u->sock.local);
    *out = &u->sock;
    return DE_OK;

fail:
    closesocket(u->fd);
    DE_FREE(a, u, sizeof *u);
    WSACleanup();
    return r;
}

de_result de__udp_open(const de_net_address *bind_addr, const de__udp_desc *desc,
                       const de_allocator *a, de__socket **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net udp open: out is NULL");
    *out = NULL;
    de__udp_desc d;
    memset(&d, 0, sizeof d);
    if (desc != NULL)
        d = *desc;
    if (a == NULL)
        a = de_heap_allocator();
    if (bind_addr != NULL)
        return udp_open_at(bind_addr, &d, a, out);
    de_net_address any6 = de_net_address_any(DE_NET_FAMILY_IPV6, 0);
    de_result r = udp_open_at(&any6, &d, a, out);
    if (r != DE_ERR_UNSUPPORTED)
        return r;
    de_net_address any4 = de_net_address_any(DE_NET_FAMILY_IPV4, 0);
    return udp_open_at(&any4, &d, a, out);
}

de__udp_info de__udp_get_info(const de__socket *s) {
    de__udp_info info;
    memset(&info, 0, sizeof info);
    DE_ASSERT(s != NULL && s->close == udp_close);
    if (s == NULL || s->close != udp_close)
        return info;
    return ((const udp_state *)s->user)->info;
}

/* ---------- Resolver ---------- */

de_result de__net_os_resolve(const char *host, de_net_family family, u16 port, de_net_address *out,
                             u32 max, u32 *count) {
    de_result r = wsa_start();
    if (r != DE_OK)
        return r;
    ADDRINFOA hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family == DE_NET_FAMILY_IPV4   ? AF_INET
                      : family == DE_NET_FAMILY_IPV6 ? AF_INET6
                                                     : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    PADDRINFOA res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        r = DE_ERR_NETWORK;
        if (rc == WSAHOST_NOT_FOUND || rc == WSANO_DATA)
            r = DE_ERR_NOT_FOUND;
        else if (rc == WSA_NOT_ENOUGH_MEMORY)
            r = DE_ERR_OUT_OF_MEMORY;
        WSACleanup();
        return DE_FAIL(r, "de_net_resolve '%s': WSA error %d", host, rc);
    }
    u32 n = 0;
    for (const ADDRINFOA *ai = res; ai != NULL && n < max; ai = ai->ai_next) {
        if ((ai->ai_family != AF_INET && ai->ai_family != AF_INET6) || ai->ai_addr == NULL ||
            ai->ai_addrlen > sizeof(struct sockaddr_storage))
            continue;
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        memcpy(&ss, ai->ai_addr, ai->ai_addrlen);
        de_net_address a;
        from_sockaddr(&ss, &a);
        de__net_address_unmap(&a);
        a.port = port;
        if (family != DE_NET_FAMILY_NONE && a.family != (u16)family)
            continue;
        bool dup = false;
        for (u32 i = 0; i < n && !dup; ++i)
            dup = de_net_address_equal(&out[i], &a);
        if (!dup)
            out[n++] = a;
    }
    freeaddrinfo(res);
    WSACleanup();
    *count = n;
    if (n == 0)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_net_resolve '%s': no address of the requested family",
                       host);
    return DE_OK;
}
