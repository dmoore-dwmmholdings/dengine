#include "net/fragment.h"
#include "net/crypto.h"
#include <string.h>

#define PAGE_WORDS     (DE__FRAGMENT_MAX_POOL / DE__FRAGMENT_SLICE / 64u)
#define FRAGMENT_WORDS (DE__FRAGMENT_MAX_MESSAGE / DE__FRAGMENT_SLICE / 64u)
typedef struct assembly {
    u64 first, bitmap[FRAGMENT_WORDS];
    u32 id, total, page;
    u16 count, received;
    u8 channel;
    bool used;
} assembly;
typedef struct retired {
    u64 at;
    u32 id;
    u8 channel;
    bool used;
} retired;
struct de__fragment_pool {
    de__fragment_desc desc;
    de__fragment_stats stats;
    u64 pages[PAGE_WORDS];
    assembly messages[DE__FRAGMENT_SLOTS];
    retired tombstones[DE__FRAGMENT_SLOTS];
    u32 retired_next;
    u8 data[];
};
static u16 read16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}
static void put16(u8 *p, u16 n) {
    p[0] = (u8)n;
    p[1] = (u8)(n >> 8);
}
static u32 page_count(u32 bytes) {
    return (bytes + DE__FRAGMENT_SLICE - 1u) / DE__FRAGMENT_SLICE;
}
de_result de__fragment_create(const de__fragment_desc *desc, de__fragment_pool **out) {
    if (!out)
        return DE_ERR_INVALID_ARGUMENT;
    *out = NULL;
    de__fragment_desc d = desc ? *desc : (de__fragment_desc){0};
    if (!d.allocator)
        d.allocator = de_heap_allocator();
    if (!d.max_message_bytes)
        d.max_message_bytes = DE__FRAGMENT_MAX_MESSAGE;
    if (!d.pool_bytes)
        d.pool_bytes = DE__FRAGMENT_DEFAULT_POOL;
    if (!d.timeout_ns)
        d.timeout_ns = DE__FRAGMENT_TIMEOUT_NS;
    if (d.max_message_bytes > DE__FRAGMENT_MAX_MESSAGE || d.pool_bytes < DE__FRAGMENT_SLICE ||
        d.pool_bytes > DE__FRAGMENT_MAX_POOL)
        return DE_ERR_INVALID_ARGUMENT;
    d.pool_bytes -= d.pool_bytes % DE__FRAGMENT_SLICE;
    const size_t allocation = sizeof(de__fragment_pool) + d.pool_bytes;
    de__fragment_pool *p = DE_ALLOC(d.allocator, allocation, 16, DE_MEM_NET);
    if (!p)
        return DE_ERR_OUT_OF_MEMORY;
    memset(p, 0, allocation);
    p->desc = d;
    p->stats.pool_bytes = d.pool_bytes;
    p->stats.allocated_bytes = allocation;
    *out = p;
    return DE_OK;
}
void de__fragment_destroy(de__fragment_pool *p) {
    if (p)
        DE_FREE(p->desc.allocator, p, p->stats.allocated_bytes);
}
void de__fragment_reset(de__fragment_pool *p) {
    if (!p)
        return;
    const de__fragment_desc d = p->desc;
    const size_t allocation = p->stats.allocated_bytes;
    memset(p, 0, allocation);
    p->desc = d;
    p->stats.pool_bytes = d.pool_bytes;
    p->stats.allocated_bytes = allocation;
}
bool de__fragment_read(const void *body, u32 bytes, u32 max_message, de__fragment_view *out) {
    if (!body || !out || bytes < DE__FRAGMENT_HEADER || !max_message ||
        max_message > DE__FRAGMENT_MAX_MESSAGE)
        return false;
    const u8 *p = body;
    const u32 total = de__net_get_u32(p + 8);
    const u16 index = read16(p + 4), count = read16(p + 6);
    if (total <= DE__FRAGMENT_SLICE || total > max_message || count != page_count(total) ||
        index >= count)
        return false;
    const u32 offset = (u32)index * DE__FRAGMENT_SLICE;
    const u32 size = total - offset < DE__FRAGMENT_SLICE ? total - offset : DE__FRAGMENT_SLICE;
    if (bytes - DE__FRAGMENT_HEADER != size)
        return false;
    *out =
        (de__fragment_view){de__net_get_u32(p), total, size, index, count, p + DE__FRAGMENT_HEADER};
    return true;
}
u32 de__fragment_write(void *body, u32 capacity, u32 id, u16 index, const void *message,
                       u32 bytes) {
    if (!body || !message || bytes <= DE__FRAGMENT_SLICE || bytes > DE__FRAGMENT_MAX_MESSAGE ||
        index >= page_count(bytes))
        return 0;
    const u32 offset = (u32)index * DE__FRAGMENT_SLICE;
    const u32 n = bytes - offset < DE__FRAGMENT_SLICE ? bytes - offset : DE__FRAGMENT_SLICE;
    if (capacity < DE__FRAGMENT_HEADER + n)
        return 0;
    u8 *p = body;
    de__net_put_u32(p, id);
    put16(p + 4, index);
    put16(p + 6, (u16)page_count(bytes));
    de__net_put_u32(p + 8, bytes);
    memcpy(p + DE__FRAGMENT_HEADER, (const u8 *)message + offset, n);
    return DE__FRAGMENT_HEADER + n;
}
static bool elapsed(u64 now, u64 start, u64 duration) {
    return now >= start && now - start >= duration;
}
static void retire(de__fragment_pool *p, de_channel ch, u32 id, u64 now) {
    p->tombstones[p->retired_next++ % DE__FRAGMENT_SLOTS] = (retired){now, id, (u8)ch, true};
}
static void release(de__fragment_pool *p, assembly *a) {
    const u32 count = page_count(a->total);
    for (u32 i = a->page; i < a->page + count; ++i)
        p->pages[i / 64u] &= ~((u64)1 << (i % 64u));
    p->stats.used_bytes -= count * DE__FRAGMENT_SLICE;
    p->stats.active--;
    a->used = false;
}
static void drop(de__fragment_pool *p, assembly *a, u64 now) {
    retire(p, (de_channel)a->channel, a->id, now);
    release(p, a);
    p->stats.dropped++;
}
void de__fragment_expire(de__fragment_pool *p, u64 now) {
    if (!p)
        return;
    for (u32 i = 0; i < DE__FRAGMENT_SLOTS; ++i) {
        assembly *a = &p->messages[i];
        if (a->used && a->channel == DE_CHANNEL_UNRELIABLE &&
            elapsed(now, a->first, p->desc.timeout_ns))
            drop(p, a, now);
    }
}
static assembly *find(de__fragment_pool *p, de_channel ch, u32 id) {
    for (u32 i = 0; i < DE__FRAGMENT_SLOTS; ++i)
        if (p->messages[i].used && p->messages[i].id == id && p->messages[i].channel == (u8)ch)
            return &p->messages[i];
    return NULL;
}
void de__fragment_reject(de__fragment_pool *p, de_channel ch, u32 id, u64 now) {
    if (!p || (u32)ch > DE_CHANNEL_RELIABLE_UNORDERED)
        return;
    assembly *a = find(p, ch, id);
    if (a)
        drop(p, a, now);
    else
        retire(p, ch, id, now);
}
static u32 free_pages(const de__fragment_pool *p, u32 count) {
    u32 run = 0;
    for (u32 i = 0; i < p->desc.pool_bytes / DE__FRAGMENT_SLICE; ++i) {
        run = p->pages[i / 64u] & ((u64)1 << (i % 64u)) ? 0 : run + 1;
        if (run == count)
            return i + 1 - count;
    }
    return UINT32_MAX;
}
static assembly *reserve(de__fragment_pool *p, de_channel ch, const de__fragment_view *v, u64 now) {
    const u32 pages = page_count(v->total);
    if (pages > p->desc.pool_bytes / DE__FRAGMENT_SLICE)
        return NULL;
    for (;;) {
        assembly *empty = NULL, *oldest = NULL;
        for (u32 i = 0; i < DE__FRAGMENT_SLOTS; ++i) {
            assembly *a = &p->messages[i];
            if (!a->used && !empty)
                empty = a;
            if (a->used && a->channel == DE_CHANNEL_UNRELIABLE &&
                (!oldest || a->first < oldest->first))
                oldest = a;
        }
        const u32 page = free_pages(p, pages);
        if (empty && page != UINT32_MAX) {
            memset(empty, 0, sizeof *empty);
            empty->used = true;
            empty->id = v->id;
            empty->channel = (u8)ch;
            empty->total = v->total;
            empty->count = v->count;
            empty->page = page;
            empty->first = now;
            for (u32 i = page; i < page + pages; ++i)
                p->pages[i / 64u] |= (u64)1 << (i % 64u);
            p->stats.used_bytes += pages * DE__FRAGMENT_SLICE;
            if (p->stats.peak_bytes < p->stats.used_bytes)
                p->stats.peak_bytes = p->stats.used_bytes;
            p->stats.active++;
            return empty;
        }
        if (!oldest)
            return NULL;
        drop(p, oldest, now);
    }
}
de_result de__fragment_push(de__fragment_pool *p, de_channel ch, const void *body, u32 bytes,
                            u64 now, de__fragment_message_fn fn, void *user) {
    if (!p || (u32)ch > DE_CHANNEL_RELIABLE_UNORDERED || !fn)
        return DE_ERR_INVALID_ARGUMENT;
    de__fragment_expire(p, now);
    de__fragment_view v;
    if (!de__fragment_read(body, bytes, p->desc.max_message_bytes, &v)) {
        p->stats.malformed++;
        if (body && bytes >= 4) {
            const u32 id = de__net_get_u32(body);
            de__fragment_reject(p, ch, id, now);
        }
        return DE_ERR_PARSE;
    }
    for (u32 i = 0; i < DE__FRAGMENT_SLOTS; ++i) {
        const retired *t = &p->tombstones[i];
        if (t->used && t->id == v.id && t->channel == (u8)ch &&
            !elapsed(now, t->at, p->desc.timeout_ns))
            return DE_OK;
    }
    assembly *a = find(p, ch, v.id);
    if (a && (a->total != v.total || a->count != v.count)) {
        drop(p, a, now);
        p->stats.malformed++;
        return DE_ERR_PARSE;
    }
    if (!a)
        a = reserve(p, ch, &v, now);
    if (!a) {
        retire(p, ch, v.id, now);
        p->stats.dropped++;
        return ch == DE_CHANNEL_UNRELIABLE ? DE_ERR_BUSY : DE_ERR_OUT_OF_MEMORY;
    }
    u8 *dest = p->data + (size_t)a->page * DE__FRAGMENT_SLICE + (u32)v.index * DE__FRAGMENT_SLICE;
    const u64 bit = (u64)1 << (v.index % 64u);
    if (a->bitmap[v.index / 64u] & bit) {
        if (memcmp(dest, v.data, v.bytes) == 0)
            return DE_OK;
        drop(p, a, now);
        p->stats.malformed++;
        return DE_ERR_PARSE;
    }
    memcpy(dest, v.data, v.bytes);
    a->bitmap[v.index / 64u] |= bit;
    if (++a->received == a->count) {
        fn(user, ch, p->data + (size_t)a->page * DE__FRAGMENT_SLICE, a->total);
        retire(p, ch, a->id, now);
        release(p, a);
        p->stats.completed++;
    }
    return DE_OK;
}
de__fragment_stats de__fragment_get_stats(const de__fragment_pool *p) {
    return p ? p->stats : (de__fragment_stats){0};
}
