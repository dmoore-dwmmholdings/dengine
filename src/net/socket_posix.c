/* socket_posix.c: non-blocking UDP sockets and the resolver on macOS and Linux (DENG-51).
 *
 * BSD sockets. Linux receives in batches with recvmmsg (one system call for up to
 * DE__NET_RECV_BATCH_MAX datagrams); macOS loops recvmsg. Both read MSG_TRUNC so a datagram
 * bigger than the caller's buffer is dropped instead of handed over cut. Every call retries on
 * EINTR. No heap after open: addresses and message headers live on the stack. */
#if defined(__linux__)
#define _GNU_SOURCE /* recvmmsg, SOCK_CLOEXEC, IP_MTU_DISCOVER */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE     /* IP_DONTFRAG */
#define __APPLE_USE_RFC_3542 /* IPV6_DONTFRAG is declared only in RFC 3542 mode */
#endif

#include "net/net_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

typedef struct udp_state {
    de__socket sock; /* first: the allocation starts with the public part */
    int fd;
    int af; /* AF_INET or AF_INET6 */
    de__udp_info info;
    const de_allocator *alloc;
} udp_state;

/* ---------- Address conversion ---------- */

static void put_port(u8 *dst, u16 port) {
    dst[0] = (u8)(port >> 8);
    dst[1] = (u8)(port & 0xFFu);
}

static u16 get_port(const u8 *src) {
    return (u16)((src[0] << 8) | src[1]);
}

/* For a socket of family `af`; IPv4 on an IPv6 socket becomes ::ffff:a.b.c.d. 0 = no fit. */
static socklen_t to_sockaddr(const de_net_address *a, int af, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof *ss);
    if (af == AF_INET) {
        if (a->family != DE_NET_FAMILY_IPV4)
            return 0;
        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)ss;
        sin->sin_family = AF_INET;
        put_port((u8 *)&sin->sin_port, a->port);
        memcpy(&sin->sin_addr, a->addr, 4);
        return (socklen_t)sizeof *sin;
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
    return (socklen_t)sizeof *sin6;
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
    socklen_t len = to_sockaddr(to, u->af, &ss);
    if (len == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "net send: address family %u does not fit the socket", (unsigned)to->family);
    if (bytes > DE__NET_MAX_UDP_PAYLOAD)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net send: %u bytes is more than UDP carries",
                       bytes);
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    for (;;) {
        ssize_t r =
            sendto(u->fd, data, bytes, flags, (const struct sockaddr *)(const void *)&ss, len);
        if (r >= 0) {
            *sent = (u32)r;
            return DE_OK;
        }
        int e = errno;
        if (e == EINTR)
            continue;
        /* ENOBUFS: the interface queue is full (BSD); like EAGAIN, try again later. */
        if (e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS)
            return DE_OK;
        if (e == EMSGSIZE)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "net send: %u bytes is more than the path MTU allows (Don't-Fragment)",
                           bytes);
        char addr[DE_NET_ADDRESS_STRING_MAX];
        de_net_address_to_string(to, addr, (u32)sizeof addr);
        return DE_FAIL(DE_ERR_NETWORK, "net send to %s: errno %d", addr, e);
    }
}

/* ICMP errors from earlier sends that some stacks report on the next receive. They are about a
 * peer, not this socket: skip them (Winsock gets the same from SIO_UDP_CONNRESET off). */
static bool is_peer_error(int e) {
    return e == ECONNREFUSED || e == ECONNRESET || e == EHOSTUNREACH || e == ENETUNREACH;
}

#if defined(__linux__)
static de_result udp_recv_from(de__socket *s, de__datagram *msgs, u32 count, u32 *received) {
    udp_state *u = (udp_state *)s->user;
    u32 n = 0;
    while (n < count) {
        u32 want = count - n < DE__NET_RECV_BATCH_MAX ? count - n : DE__NET_RECV_BATCH_MAX;
        struct mmsghdr mm[DE__NET_RECV_BATCH_MAX];
        struct iovec iov[DE__NET_RECV_BATCH_MAX];
        struct sockaddr_storage from[DE__NET_RECV_BATCH_MAX];
        memset(mm, 0, want * sizeof mm[0]);
        for (u32 i = 0; i < want; ++i) {
            iov[i].iov_base = msgs[n + i].data;
            iov[i].iov_len = msgs[n + i].capacity;
            mm[i].msg_hdr.msg_name = &from[i];
            mm[i].msg_hdr.msg_namelen = (socklen_t)sizeof from[i];
            mm[i].msg_hdr.msg_iov = &iov[i];
            mm[i].msg_hdr.msg_iovlen = 1;
        }
        int r = recvmmsg(u->fd, mm, want, MSG_DONTWAIT, NULL);
        if (r < 0) {
            int e = errno;
            if (e == EINTR || is_peer_error(e))
                continue;
            if (e == EAGAIN || e == EWOULDBLOCK)
                break;
            *received = n;
            return DE_FAIL(DE_ERR_NETWORK, "net recv: errno %d", e);
        }
        /* Keep the good datagrams contiguous: a truncated one's slot takes the next buffer. */
        u32 got = 0;
        for (u32 i = 0; i < (u32)r; ++i) {
            if ((mm[i].msg_hdr.msg_flags & MSG_TRUNC) != 0) {
                s->stats.truncated++;
                continue;
            }
            de__datagram *dst = &msgs[n + got];
            de__datagram *src = &msgs[n + i];
            if (dst != src) {
                void *d = dst->data;
                u32 c = dst->capacity;
                dst->data = src->data;
                dst->capacity = src->capacity;
                src->data = d;
                src->capacity = c;
            }
            dst->bytes = (u32)mm[i].msg_len;
            from_sockaddr(&from[i], &dst->from);
            de__net_address_unmap(&dst->from);
            ++got;
        }
        n += got;
        if ((u32)r < want)
            break; /* drained */
    }
    *received = n;
    return DE_OK;
}
#else
static de_result udp_recv_from(de__socket *s, de__datagram *msgs, u32 count, u32 *received) {
    udp_state *u = (udp_state *)s->user;
    u32 n = 0;
    while (n < count) {
        struct sockaddr_storage from;
        struct iovec iov;
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        iov.iov_base = msgs[n].data;
        iov.iov_len = msgs[n].capacity;
        mh.msg_name = &from;
        mh.msg_namelen = (socklen_t)sizeof from;
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        ssize_t r = recvmsg(u->fd, &mh, MSG_DONTWAIT);
        if (r < 0) {
            int e = errno;
            if (e == EINTR || is_peer_error(e))
                continue;
            if (e == EAGAIN || e == EWOULDBLOCK)
                break;
            *received = n;
            return DE_FAIL(DE_ERR_NETWORK, "net recv: errno %d", e);
        }
        if ((mh.msg_flags & MSG_TRUNC) != 0) {
            s->stats.truncated++;
            continue; /* the slot is reused for the next datagram */
        }
        msgs[n].bytes = (u32)r;
        from_sockaddr(&from, &msgs[n].from);
        de__net_address_unmap(&msgs[n].from);
        ++n;
    }
    *received = n;
    return DE_OK;
}
#endif

static void udp_close(de__socket *s) {
    udp_state *u = (udp_state *)s->user;
    while (close(u->fd) != 0 && errno == EINTR) {
    }
    DE_FREE(u->alloc, u, sizeof *u);
}

/* ---------- Open ---------- */

static de_result errno_result(int e) {
    switch (e) {
    case EAFNOSUPPORT:
    case EPROTONOSUPPORT:
        return DE_ERR_UNSUPPORTED;
    case EADDRINUSE:
        return DE_ERR_ALREADY_EXISTS;
    case EADDRNOTAVAIL:
        return DE_ERR_NOT_FOUND;
    case ENOMEM:
    case ENOBUFS:
        return DE_ERR_OUT_OF_MEMORY;
    default:
        return DE_ERR_NETWORK;
    }
}

/* Asks for `bytes`, halving while the OS refuses (macOS refuses more than kern.ipc.maxsockbuf;
 * Linux silently caps at net.core.[rw]mem_max). */
static void set_buffer(int fd, int opt, u32 bytes) {
    int v = bytes > (u32)INT_MAX ? INT_MAX : (int)bytes;
    while (v >= 4096) {
        if (setsockopt(fd, SOL_SOCKET, opt, &v, (socklen_t)sizeof v) == 0)
            return;
        v /= 2;
    }
}

static u32 get_buffer(int fd, int opt) {
    int v = 0;
    socklen_t len = (socklen_t)sizeof v;
    if (getsockopt(fd, SOL_SOCKET, opt, &v, &len) != 0 || v < 0)
        return 0;
    return (u32)v;
}

static bool set_int(int fd, int level, int opt, int value) {
    return setsockopt(fd, level, opt, &value, (socklen_t)sizeof value) == 0;
}

/* Don't-Fragment for the socket's own family; for a dual-stack socket also (best effort) for the
 * IPv4 traffic it carries. Returns whether the own-family option took. */
static bool set_dont_fragment(int fd, int af) {
    bool ok = false;
#if defined(__linux__)
    if (af == AF_INET6) {
        ok = set_int(fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, IPV6_PMTUDISC_DO);
        (void)set_int(fd, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DO);
    } else {
        ok = set_int(fd, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DO);
    }
#else
    if (af == AF_INET6) {
#if defined(IPV6_DONTFRAG)
        ok = set_int(fd, IPPROTO_IPV6, IPV6_DONTFRAG, 1);
#endif
#if defined(IP_DONTFRAG)
        (void)set_int(fd, IPPROTO_IP, IP_DONTFRAG, 1);
#endif
    } else {
#if defined(IP_DONTFRAG)
        ok = set_int(fd, IPPROTO_IP, IP_DONTFRAG, 1);
#endif
    }
#endif
    return ok;
}

static de_result udp_open_at(const de_net_address *bind_addr, const de__udp_desc *d,
                             const de_allocator *a, de__socket **out) {
    if (bind_addr->family != DE_NET_FAMILY_IPV4 && bind_addr->family != DE_NET_FAMILY_IPV6)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net udp open: bind address has no family");
    int af = bind_addr->family == DE_NET_FAMILY_IPV4 ? AF_INET : AF_INET6;

    udp_state *u = DE_NEW(a, udp_state, DE_MEM_NET);
    if (u == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "net udp open: no memory for the socket");
    memset(u, 0, sizeof *u);
    u->alloc = a;
    u->af = af;

#if defined(SOCK_CLOEXEC)
    u->fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
#else
    u->fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (u->fd < 0) {
        int e = errno;
        DE_FREE(a, u, sizeof *u);
        return DE_FAIL(errno_result(e), "net udp open: socket(%s) failed: errno %d",
                       af == AF_INET ? "IPv4" : "IPv6", e);
    }
#if !defined(SOCK_CLOEXEC)
    (void)fcntl(u->fd, F_SETFD, FD_CLOEXEC);
#endif

    char addr[DE_NET_ADDRESS_STRING_MAX];
    de_net_address_to_string(bind_addr, addr, (u32)sizeof addr);
    de_result r = DE_OK;
    int fl = fcntl(u->fd, F_GETFL, 0);
    if (fl < 0 || fcntl(u->fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        r = DE_FAIL(DE_ERR_NETWORK, "net udp open %s: cannot make it non-blocking: errno %d", addr,
                    errno);
        goto fail;
    }
    if (af == AF_INET6) {
        if (!set_int(u->fd, IPPROTO_IPV6, IPV6_V6ONLY, d->v6_only ? 1 : 0)) {
            r = DE_FAIL(DE_ERR_NETWORK, "net udp open %s: IPV6_V6ONLY failed: errno %d", addr,
                        errno);
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
        u->info.dont_fragment = set_dont_fragment(u->fd, af);
        if (!u->info.dont_fragment)
            DE_LOG(DE_LOG_WARN, de__net_log(),
                   "net udp open %s: cannot set Don't-Fragment; MTU discovery will not work", addr);
    }

    struct sockaddr_storage ss;
    socklen_t len = to_sockaddr(bind_addr, af, &ss);
    if (bind(u->fd, (const struct sockaddr *)(const void *)&ss, len) != 0) {
        int e = errno;
        r = DE_FAIL(errno_result(e), "net udp open: bind %s failed: errno %d", addr, e);
        goto fail;
    }
    struct sockaddr_storage local;
    socklen_t local_len = (socklen_t)sizeof local;
    if (getsockname(u->fd, (struct sockaddr *)(void *)&local, &local_len) != 0) {
        r = DE_FAIL(DE_ERR_NETWORK, "net udp open %s: getsockname failed: errno %d", addr, errno);
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
    while (close(u->fd) != 0 && errno == EINTR) {
    }
    DE_FREE(a, u, sizeof *u);
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
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family == DE_NET_FAMILY_IPV4   ? AF_INET
                      : family == DE_NET_FAMILY_IPV6 ? AF_INET6
                                                     : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    /* No AI_ADDRCONFIG: glibc ignores loopback addresses for it, so an offline host would not
     * resolve "localhost". */
    struct addrinfo *res = NULL;
    int rc;
    do {
        rc = getaddrinfo(host, NULL, &hints, &res);
    } while (rc == EAI_SYSTEM && errno == EINTR);
    if (rc != 0) {
        de_result r = DE_ERR_NETWORK;
        if (rc == EAI_NONAME
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
            || rc == EAI_NODATA
#endif
#if defined(EAI_ADDRFAMILY)
            || rc == EAI_ADDRFAMILY
#endif
        )
            r = DE_ERR_NOT_FOUND;
        else if (rc == EAI_MEMORY)
            r = DE_ERR_OUT_OF_MEMORY;
        return DE_FAIL(r, "de_net_resolve '%s': %s", host, gai_strerror(rc));
    }
    u32 n = 0;
    for (const struct addrinfo *ai = res; ai != NULL && n < max; ai = ai->ai_next) {
        if ((ai->ai_family != AF_INET && ai->ai_family != AF_INET6) || ai->ai_addr == NULL ||
            (size_t)ai->ai_addrlen > sizeof(struct sockaddr_storage))
            continue;
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        memcpy(&ss, ai->ai_addr, (size_t)ai->ai_addrlen);
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
    *count = n;
    if (n == 0)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_net_resolve '%s': no address of the requested family",
                       host);
    return DE_OK;
}
