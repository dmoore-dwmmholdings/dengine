/* socket_loopback.c: the in-process transport, DE_TRANSPORT_LOOPBACK (DENG-51).
 *
 * A loopback network is a list of sockets; each has a bounded FIFO of datagrams (a ring of
 * fixed-size slots, allocated at open). send_to copies the datagram into the receiver's ring
 * under the network's mutex; recv_from copies it out. A full ring drops the new datagram and
 * counts it, as a full OS receive buffer would (but visibly). Every net unit test after DENG-51
 * runs on this backend, and the in-process listen server can too.
 *
 * Sockets stay in the list in the order they were opened (removal shifts), and lookups walk the
 * list front to back, so behaviour does not depend on addresses or hashing. */
#include "net/net_internal.h"

#include <dengine/de_platform.h>

#include <string.h>

#define LB_DEFAULT_QUEUE        256u
#define LB_DEFAULT_MAX_DATAGRAM 2048u
#define LB_EPHEMERAL_FIRST      49152u
#define LB_EPHEMERAL_LAST       65535u

typedef struct lb_slot {
    u32 bytes;
    de_net_address from;
} lb_slot;

typedef struct lb_socket {
    de__socket sock; /* first: the allocation starts with the public part */
    de__loopback_net *net;
    size_t alloc_size;
    de_net_address bound;  /* as bound: may be unspecified */
    de_net_address sender; /* what receivers see as `from` */
    lb_slot *slots;        /* queue_capacity */
    u8 *data;              /* queue_capacity * max_datagram */
    u32 head;              /* oldest datagram */
    u32 count;
    u64 dropped; /* written by senders under the lock; copied to sock.stats in recv_from */
} lb_socket;

struct de__loopback_net {
    const de_allocator *alloc;
    de_mutex lock;
    u32 queue_capacity;
    u32 max_datagram;
    lb_socket **sockets;
    u32 socket_count;
    u32 socket_cap;
    u32 next_port;
};

/* ---------- Matching ---------- */

static bool same_host(const de_net_address *a, const de_net_address *b) {
    if (a->family != b->family || a->scope_id != b->scope_id)
        return false;
    return memcmp(a->addr, b->addr, a->family == DE_NET_FAMILY_IPV4 ? 4u : 16u) == 0;
}

/* Two bindings that would both receive some datagram. */
static bool overlaps(const de_net_address *a, const de_net_address *b) {
    if (a->port != b->port)
        return false;
    return de__net_address_is_unspecified(a) || de__net_address_is_unspecified(b) ||
           same_host(a, b);
}

/* The socket a datagram to `to` reaches, or NULL. Bindings never overlap, so at most one. */
static lb_socket *find_receiver(de__loopback_net *n, const de_net_address *to) {
    for (u32 i = 0; i < n->socket_count; ++i) {
        lb_socket *ls = n->sockets[i];
        if (ls->bound.port == to->port &&
            (de__net_address_is_unspecified(&ls->bound) || same_host(&ls->bound, to)))
            return ls;
    }
    return NULL;
}

static bool binding_free(const de__loopback_net *n, const de_net_address *a) {
    for (u32 i = 0; i < n->socket_count; ++i)
        if (overlaps(&n->sockets[i]->bound, a))
            return false;
    return true;
}

/* ---------- Socket functions ---------- */

static de_result lb_send_to(de__socket *s, const de_net_address *to, const void *data, u32 bytes,
                            u32 *sent) {
    lb_socket *self = (lb_socket *)s->user;
    de__loopback_net *n = self->net;
    *sent = 0;
    if (to->family != DE_NET_FAMILY_IPV4 && to->family != DE_NET_FAMILY_IPV6)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net loopback send: address has no family");
    if (bytes > n->max_datagram)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "net loopback send: %u bytes is more than max_datagram_bytes (%u)", bytes,
                       n->max_datagram);
    de_mutex_lock(&n->lock);
    lb_socket *dst = find_receiver(n, to);
    if (dst != NULL) {
        if (dst->count == n->queue_capacity) {
            dst->dropped++;
        } else {
            u32 idx = (dst->head + dst->count) % n->queue_capacity;
            dst->slots[idx].bytes = bytes;
            dst->slots[idx].from = self->sender;
            if (bytes > 0)
                memcpy(dst->data + (size_t)idx * n->max_datagram, data, bytes);
            dst->count++;
        }
    }
    de_mutex_unlock(&n->lock);
    /* Handed over, as UDP would report it, even when nobody has the address or the queue was
     * full: the datagram is lost on the way, not refused. */
    *sent = bytes;
    return DE_OK;
}

static de_result lb_recv_from(de__socket *s, de__datagram *msgs, u32 count, u32 *received) {
    lb_socket *self = (lb_socket *)s->user;
    de__loopback_net *n = self->net;
    u32 got = 0;
    de_mutex_lock(&n->lock);
    while (got < count && self->count > 0) {
        const lb_slot *slot = &self->slots[self->head];
        const u8 *src = self->data + (size_t)self->head * n->max_datagram;
        self->head = (self->head + 1u) % n->queue_capacity;
        self->count--;
        de__datagram *m = &msgs[got];
        if (slot->bytes > m->capacity) {
            s->stats.truncated++;
            continue; /* the caller's slot is reused for the next datagram */
        }
        if (slot->bytes > 0)
            memcpy(m->data, src, slot->bytes);
        m->bytes = slot->bytes;
        m->from = slot->from;
        ++got;
    }
    s->stats.dropped = self->dropped;
    de_mutex_unlock(&n->lock);
    *received = got;
    return DE_OK;
}

static void lb_close(de__socket *s) {
    lb_socket *self = (lb_socket *)s->user;
    de__loopback_net *n = self->net;
    de_mutex_lock(&n->lock);
    for (u32 i = 0; i < n->socket_count; ++i) {
        if (n->sockets[i] == self) {
            memmove(n->sockets + i, n->sockets + i + 1,
                    (size_t)(n->socket_count - i - 1u) * sizeof n->sockets[0]);
            n->socket_count--;
            break;
        }
    }
    de_mutex_unlock(&n->lock);
    DE_FREE(n->alloc, self, self->alloc_size);
}

/* ---------- Network ---------- */

de_result de__loopback_net_create(const de__loopback_desc *desc, const de_allocator *a,
                                  de__loopback_net **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net loopback create: out is NULL");
    *out = NULL;
    if (a == NULL)
        a = de_heap_allocator();
    u32 queue = desc != NULL && desc->queue_capacity != 0 ? desc->queue_capacity : LB_DEFAULT_QUEUE;
    u32 max_dg = desc != NULL && desc->max_datagram_bytes != 0 ? desc->max_datagram_bytes
                                                               : LB_DEFAULT_MAX_DATAGRAM;
    if (max_dg > DE__NET_MAX_UDP_PAYLOAD)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "net loopback create: max_datagram_bytes %u is more than UDP carries",
                       max_dg);
    if ((u64)queue * max_dg > (u64)1 << 31)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "net loopback create: queue_capacity * max_datagram_bytes over 2 GiB");
    de__loopback_net *n = DE_NEW(a, de__loopback_net, DE_MEM_NET);
    if (n == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "net loopback create: no memory");
    memset(n, 0, sizeof *n);
    n->alloc = a;
    n->queue_capacity = queue;
    n->max_datagram = max_dg;
    n->next_port = LB_EPHEMERAL_FIRST;
    de_mutex_init(&n->lock);
    *out = n;
    return DE_OK;
}

void de__loopback_net_destroy(de__loopback_net *n) {
    if (n == NULL)
        return;
    DE_ASSERT(n->socket_count == 0); /* close every socket first */
    if (n->sockets != NULL)
        DE_FREE(n->alloc, n->sockets, (size_t)n->socket_cap * sizeof n->sockets[0]);
    de_mutex_destroy(&n->lock);
    DE_FREE(n->alloc, n, sizeof *n);
}

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

de_result de__loopback_open(de__loopback_net *n, const de_net_address *bind_addr,
                            de__socket **out) {
    if (out == NULL || n == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net loopback open: NULL argument");
    *out = NULL;
    de_net_address want =
        bind_addr != NULL ? *bind_addr : de_net_address_loopback(DE_NET_FAMILY_IPV4, 0);
    if (want.family != DE_NET_FAMILY_IPV4 && want.family != DE_NET_FAMILY_IPV6)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net loopback open: bind address has no family");

    size_t off_slots = align_up(sizeof(lb_socket), DE_ALIGNOF(lb_slot));
    size_t off_data = align_up(off_slots + (size_t)n->queue_capacity * sizeof(lb_slot), 16);
    size_t size = off_data + (size_t)n->queue_capacity * n->max_datagram;
    u8 *mem = (u8 *)DE_ALLOC(n->alloc, size, 16, DE_MEM_NET);
    if (mem == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "net loopback open: no memory for %zu bytes", size);
    lb_socket *ls = (lb_socket *)(void *)mem;
    memset(ls, 0, sizeof *ls);
    ls->net = n;
    ls->alloc_size = size;
    ls->slots = (lb_slot *)(void *)(mem + off_slots);
    ls->data = mem + off_data;

    de_result r = DE_OK;
    de_mutex_lock(&n->lock);
    if (want.port == 0) {
        bool found = false;
        for (u32 tries = 0; tries <= LB_EPHEMERAL_LAST - LB_EPHEMERAL_FIRST && !found; ++tries) {
            want.port = (u16)n->next_port;
            n->next_port =
                n->next_port == LB_EPHEMERAL_LAST ? LB_EPHEMERAL_FIRST : n->next_port + 1u;
            found = binding_free(n, &want);
        }
        if (!found)
            r = DE_FAIL(DE_ERR_BUSY, "net loopback open: no free ephemeral port");
    } else if (!binding_free(n, &want)) {
        char text[DE_NET_ADDRESS_STRING_MAX];
        de_net_address_to_string(&want, text, (u32)sizeof text);
        r = DE_FAIL(DE_ERR_ALREADY_EXISTS, "net loopback open: %s is in use", text);
    }
    if (r == DE_OK && n->socket_count == n->socket_cap) {
        u32 cap = n->socket_cap != 0 ? n->socket_cap * 2u : 8u;
        lb_socket **grown = (lb_socket **)DE_ALLOC(n->alloc, (size_t)cap * sizeof n->sockets[0],
                                                   DE_ALIGNOF(lb_socket *), DE_MEM_NET);
        if (grown == NULL) {
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "net loopback open: no memory for the socket list");
        } else {
            if (n->sockets != NULL) {
                memcpy(grown, n->sockets, (size_t)n->socket_count * sizeof n->sockets[0]);
                DE_FREE(n->alloc, n->sockets, (size_t)n->socket_cap * sizeof n->sockets[0]);
            }
            n->sockets = grown;
            n->socket_cap = cap;
        }
    }
    if (r == DE_OK) {
        /* Complete before it is visible to senders. */
        ls->bound = want;
        ls->sender = want;
        if (de__net_address_is_unspecified(&want))
            ls->sender = de_net_address_loopback((de_net_family)want.family, want.port);
        ls->sock.send_to = lb_send_to;
        ls->sock.recv_from = lb_recv_from;
        ls->sock.close = lb_close;
        ls->sock.user = ls;
        ls->sock.local = want;
        n->sockets[n->socket_count++] = ls;
    }
    de_mutex_unlock(&n->lock);
    if (r != DE_OK) {
        DE_FREE(n->alloc, mem, size);
        return r;
    }
    *out = &ls->sock;
    return DE_OK;
}
