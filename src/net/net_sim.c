#include "net/net_sim.h"

#if defined(DE_DEV) || defined(DE_DEBUG)
#include <dengine/de_debug.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <math.h>
#include <string.h>

#define SIM_MEMORY_MAX   (64u << 20)
#define SIM_QUEUE_MAX    65536u
#define SIM_DEFAULT_CAP  1024u
#define SIM_SLOT_DEFAULT 2048u
#define SIM_NS_MS        1000000ull

typedef struct delayed {
    u64 due, order;
    de_net_address address;
    u32 bytes, next;
} delayed;
typedef struct delay_queue {
    unsigned char *slots;
    u32 *heap;
    u32 count, free_head;
} delay_queue;
typedef struct simulator {
    de__socket socket;
    de__socket *inner;
    const de_allocator *allocator;
    de__net_sim_desc desc;
    de_net_sim settings;
    de_rng rng[2];
    u64 seed, order;
    size_t stride, allocation;
    delay_queue queue[2];
    unsigned char *scratch;
    de__net_sim_stats counters;
    de_result deferred_error;
    de_cvar latency, jitter, loss, duplicate, seed_cv;
} simulator;

static de_result sim_send(de__socket *, const de_net_address *, const void *, u32, u32 *);
static de_result sim_recv(de__socket *, de__datagram *, u32, u32 *);

static simulator *as_sim(const de__socket *s) {
    return s && s->send_to == sim_send ? (simulator *)s : NULL;
}
static delayed *slot(const simulator *s, const delay_queue *q, u32 index) {
    return (delayed *)(q->slots + (size_t)index * s->stride);
}
static void seed_rng(simulator *s, u64 seed) {
    s->seed = seed;
    s->rng[0] = de_rng_seed(seed, 56);
    s->rng[1] = de_rng_seed(seed, 57);
}
static de_cvar number(const char *name, bool integer, f64 def, f64 min, f64 max) {
    const de_cvar_desc d = {.name = name,
                            .help = "Dev network condition simulator",
                            .type = integer ? DE_CVAR_INT : DE_CVAR_FLOAT,
                            .flags = DE_CVAR_DEV_ONLY,
                            .default_number = def,
                            .min = min,
                            .max = max};
    return de_cvar_register(&d);
}
static u64 now(const simulator *s) {
    return s->desc.now_ns ? s->desc.now_ns(s->desc.clock_user) : de_time_now_ns();
}
static void refresh(simulator *s) {
    if (!s->desc.use_cvars)
        return;
    s->settings = (de_net_sim){(u32)de_cvar_int(s->latency), (u32)de_cvar_int(s->jitter),
                               (f32)de_cvar_float(s->loss), (f32)de_cvar_float(s->duplicate)};
    const u64 seed = (u64)de_cvar_int(s->seed_cv);
    if (s->seed != seed)
        seed_rng(s, seed);
}
static bool bypass(const simulator *s) {
    return s->settings.latency_ms == 0 && s->settings.jitter_ms == 0 && s->settings.loss == 0 &&
           s->settings.duplicate == 0;
}
static bool before(const simulator *s, const delay_queue *q, u32 a, u32 b) {
    const delayed *x = slot(s, q, a), *y = slot(s, q, b);
    return x->due < y->due || (x->due == y->due && x->order < y->order);
}
static bool push(simulator *s, u32 direction, const de_net_address *address, const void *bytes,
                 u32 n, u64 due) {
    delay_queue *q = &s->queue[direction];
    if (q->count == s->desc.queue_capacity) {
        s->counters.queue_dropped++;
        s->socket.stats.dropped++;
        return false;
    }
    const u32 id = q->free_head;
    delayed *d = slot(s, q, id);
    q->free_head = d->next;
    d->due = due;
    d->order = s->order++;
    d->address = *address;
    d->bytes = n;
    if (n)
        memcpy(d + 1, bytes, n);
    u32 at = q->count++;
    while (at) {
        const u32 parent = (at - 1) / 2;
        if (!before(s, q, id, q->heap[parent]))
            break;
        q->heap[at] = q->heap[parent];
        at = parent;
    }
    q->heap[at] = id;
    return true;
}
static void pop(simulator *s, delay_queue *q) {
    const u32 released = q->heap[0], last = q->heap[--q->count];
    u32 at = 0;
    while (at * 2 + 1 < q->count) {
        u32 child = at * 2 + 1;
        if (child + 1 < q->count && before(s, q, q->heap[child + 1], q->heap[child]))
            child++;
        if (!before(s, q, q->heap[child], last))
            break;
        q->heap[at] = q->heap[child];
        at = child;
    }
    if (q->count)
        q->heap[at] = last;
    delayed *d = slot(s, q, released);
    d->next = q->free_head;
    q->free_head = released;
}
static u64 release_time(simulator *s, u32 direction, u64 time) {
    const i64 jitter = (i64)s->settings.jitter_ms * (i64)SIM_NS_MS;
    const i64 offset =
        jitter ? (i64)(de_rng_f64(&s->rng[direction]) * (f64)(2 * jitter + 1)) - jitter : 0;
    const i64 delay = (i64)s->settings.latency_ms * (i64)SIM_NS_MS + offset;
    const u64 duration = delay > 0 ? (u64)delay : 0;
    return duration > UINT64_MAX - time ? UINT64_MAX : time + duration;
}
static void schedule(simulator *s, u32 direction, const de_net_address *address, const void *bytes,
                     u32 n, u64 time) {
    de_rng *rng = &s->rng[direction];
    if (s->settings.loss > 0 && de_rng_f64(rng) < s->settings.loss) {
        s->counters.loss_dropped++;
        s->socket.stats.dropped++;
        return;
    }
    (void)push(s, direction, address, bytes, n, release_time(s, direction, time));
    if (s->settings.duplicate > 0 && de_rng_f64(rng) < s->settings.duplicate &&
        push(s, direction, address, bytes, n, release_time(s, direction, time)))
        s->counters.duplicates++;
}

de_result de__net_sim_pump(de__socket *socket) {
    simulator *s = as_sim(socket);
    if (!s)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net simulator: not a wrapped socket");
    if (s->deferred_error != DE_OK) {
        const de_result error = s->deferred_error;
        s->deferred_error = DE_OK;
        return error;
    }
    refresh(s);
    const u64 time = now(s);
    delay_queue *q = &s->queue[0];
    while (q->count) {
        delayed *d = slot(s, q, q->heap[0]);
        if (d->due > time)
            break;
        u32 sent = 0;
        const de_result r = de__socket_send_to(s->inner, &d->address, d + 1, d->bytes, &sent);
        if (r == DE_OK && sent == 0 && d->bytes != 0)
            break; /* preserve the earliest packet for the next tick; never busy-wait */
        pop(s, q);
        if (r != DE_OK) {
            s->counters.errors++;
            return r;
        }
        s->counters.delivered_out++;
    }
    return DE_OK;
}
static de_result sim_send(de__socket *socket, const de_net_address *to, const void *bytes, u32 n,
                          u32 *sent) {
    simulator *s = (simulator *)socket;
    de_result r = de__net_sim_pump(socket);
    if (r != DE_OK)
        return r;
    if (bypass(s)) {
        r = de__socket_send_to(s->inner, to, bytes, n, sent);
        if (r != DE_OK)
            s->counters.errors++;
        else if (*sent == n)
            s->counters.delivered_out++;
        return r;
    }
    if (n > s->desc.slot_bytes)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net simulator: datagram exceeds retained slot");
    schedule(s, 0, to, bytes, n, now(s));
    *sent = n; /* simulated loss is accepted UDP loss, not a send that should be retried */
    /* Zero-delay loss/duplication must not add a frame of artificial latency. Report any
     * downstream failure on the next pump rather than invite retry of an accepted packet. */
    s->deferred_error = de__net_sim_pump(socket);
    return DE_OK;
}
static void receive_ready(simulator *s, de__datagram *messages, u32 count, u32 *received,
                          u64 time) {
    delay_queue *q = &s->queue[1];
    while (q->count && *received < count) {
        delayed *d = slot(s, q, q->heap[0]);
        if (d->due > time)
            break;
        de__datagram *m = &messages[*received];
        if (d->bytes > m->capacity || (d->bytes && !m->data))
            s->socket.stats.truncated++;
        else {
            m->from = d->address;
            m->bytes = d->bytes;
            if (d->bytes)
                memcpy(m->data, d + 1, d->bytes);
            (*received)++;
            s->counters.delivered_in++;
        }
        pop(s, q);
    }
}
static de_result receive_inner(simulator *s, de__datagram *messages, u32 count, u32 *received) {
    const u64 truncated = s->inner->stats.truncated;
    const de_result r = de__socket_recv_from(s->inner, messages, count, received);
    s->socket.stats.truncated += s->inner->stats.truncated - truncated;
    if (r != DE_OK)
        s->counters.errors++;
    return r;
}
static de_result sim_recv(de__socket *socket, de__datagram *messages, u32 count, u32 *received) {
    simulator *s = (simulator *)socket;
    *received = 0;
    de_result r = de__net_sim_pump(socket);
    if (r != DE_OK)
        return r;
    if (!s->desc.incoming) {
        r = receive_inner(s, messages, count, received);
        s->counters.delivered_in += *received;
        return r;
    }
    const u64 time = now(s);
    receive_ready(s, messages, count, received, time);
    if (*received == count)
        return DE_OK;
    if (bypass(s)) {
        u32 got = 0;
        r = receive_inner(s, messages + *received, count - *received, &got);
        *received += got;
        s->counters.delivered_in += got;
        return r;
    }
    /* Bound work even when another thread continually sends to the underlying socket. */
    for (u32 i = 0; i < s->desc.queue_capacity; ++i) {
        de__datagram m = {.data = s->scratch, .capacity = s->desc.slot_bytes};
        u32 got = 0;
        r = receive_inner(s, &m, 1, &got);
        if (got)
            schedule(s, 1, &m.from, m.data, m.bytes, time);
        if (r != DE_OK || got == 0)
            break;
    }
    receive_ready(s, messages, count, received, time);
    return r;
}
static void sim_close(de__socket *socket) {
    simulator *s = (simulator *)socket;
    const de_allocator *a = s->allocator;
    const size_t size = s->allocation;
    de__socket_close(s->inner);
    DE_FREE(a, s, size);
}

de_result de__net_sim_set(de__socket *socket, const de_net_sim *settings) {
    simulator *s = as_sim(socket);
    if (!s || !settings || settings->latency_ms > 5000 || settings->jitter_ms > 5000 ||
        !isfinite(settings->loss) || settings->loss < 0 || settings->loss > 1 ||
        !isfinite(settings->duplicate) || settings->duplicate < 0 || settings->duplicate > 1)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net simulator: delays 0..5000 ms, rates 0..1");
    s->settings = *settings;
    s->desc.use_cvars = false;
    return DE_OK;
}
de__net_sim_stats de__net_sim_get_stats(const de__socket *socket) {
    const simulator *s = as_sim(socket);
    if (!s)
        return (de__net_sim_stats){0};
    de__net_sim_stats result = s->counters;
    result.pending_out = s->queue[0].count;
    result.pending_in = s->queue[1].count;
    result.allocated_bytes = s->allocation;
    return result;
}
de_result de__net_sim_wrap(de__socket *inner, const de_allocator *a, u32 cap, de__socket **out) {
    const de__net_sim_desc d = {.queue_capacity = cap, .use_cvars = true};
    return de__net_sim_wrap_ex(inner, &d, a, out);
}
de_result de__net_sim_wrap_ex(de__socket *inner, const de__net_sim_desc *desc,
                              const de_allocator *a, de__socket **out) {
    if (out)
        *out = NULL;
    if (!inner || !out || !inner->send_to || !inner->recv_from || !inner->close)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net simulator: invalid socket/output");
    de__net_sim_desc d = desc ? *desc : (de__net_sim_desc){0};
    if (!d.queue_capacity) {
        const de_cvar capacity =
            number("net.sim.queue_capacity", true, SIM_DEFAULT_CAP, 1, SIM_QUEUE_MAX);
        d.queue_capacity = (u32)de_cvar_int(capacity);
    }
    if (!d.slot_bytes)
        d.slot_bytes = SIM_SLOT_DEFAULT;
    if (!d.queue_capacity || d.queue_capacity > SIM_QUEUE_MAX ||
        d.slot_bytes > DE__NET_MAX_UDP_PAYLOAD)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net simulator: invalid storage limits");
    const size_t stride = (sizeof(delayed) + d.slot_bytes + 15u) & ~(size_t)15u;
    const u32 directions = d.incoming ? 2 : 1;
    const size_t heap_bytes = ((size_t)d.queue_capacity * sizeof(u32) + 15u) & ~(size_t)15u;
    const u64 total = sizeof(simulator) + (u64)d.slot_bytes + 15u +
                      directions * ((u64)d.queue_capacity * stride + heap_bytes);
    if (total > SIM_MEMORY_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net simulator: storage exceeds 64 MiB");
    const size_t bytes = (size_t)total;
    if (!a)
        a = de_heap_allocator();
    simulator *s = DE_ALLOC(a, bytes, 16, DE_MEM_NET);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "net simulator storage");
    memset(s, 0, sizeof *s);
    s->allocator = a;
    s->allocation = bytes;
    s->desc = d;
    s->stride = stride;
    s->socket = (de__socket){
        .send_to = sim_send, .recv_from = sim_recv, .close = sim_close, .local = inner->local};
    unsigned char *mem = (unsigned char *)(s + 1);
    mem = (unsigned char *)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    for (u32 dir = 0; dir < directions; ++dir) {
        delay_queue *q = &s->queue[dir];
        q->slots = mem;
        mem += stride * d.queue_capacity;
        q->heap = (u32 *)mem;
        mem += heap_bytes;
        for (u32 i = 0; i < d.queue_capacity; ++i)
            slot(s, q, i)->next = i + 1;
    }
    s->scratch = mem;
    seed_rng(s, d.seed);
    if (d.use_cvars) {
        s->latency = number("net.sim.latency_ms", true, 0, 0, 5000);
        s->jitter = number("net.sim.jitter_ms", true, 0, 0, 5000);
        s->loss = number("net.sim.loss", false, 0, 0, 1);
        s->duplicate = number("net.sim.duplicate", false, 0, 0, 1);
        s->seed_cv = number("net.sim.seed", true, 1, 0, 4294967295.0);
        if (!DE_HANDLE_VALID(s->latency) || !DE_HANDLE_VALID(s->jitter) ||
            !DE_HANDLE_VALID(s->loss) || !DE_HANDLE_VALID(s->duplicate) ||
            !DE_HANDLE_VALID(s->seed_cv)) {
            DE_FREE(a, s, bytes);
            return DE_FAIL(DE_ERR_INTERNAL, "net simulator: cvar registration failed");
        }
        refresh(s);
    }
    s->inner = inner;
    *out = &s->socket;
    return DE_OK;
}
#else
/* Sessions wire this in Dev/Debug (S32). Non-development callers always get a real no-op:
 * no socket decorator, allocation, clock, random stream or retained settings exists here. */
void de_net_set_simulation(de_net_session *s, const de_net_sim *settings) {
    (void)s;
    (void)settings;
}
#endif
