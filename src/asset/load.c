/*
 * load.c: load requests, per-slot load state, the backend seam and the completion queue
 * (SPEC §11.1, §11.3, §11.4; DENG-141). See load_internal.h and backend_internal.h for the rules;
 * status.c aggregates requests and implements status and wait.
 */

#include "asset/load_internal.h"

#include "asset/asset_internal.h"
#include "asset/backend_internal.h"
#include "asset/budget_internal.h" /* DENG-144 */
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_hashmap.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <string.h>

/* Released request indices are reused FIFO, and only once more than this many are free (or no
 * fresh index is left), so a stale load handle aliases a new request only very late. */
#define DE__REQUEST_REUSE_MIN 64u

/* ---- Load state ---------------------------------------------------------------------------- */

de__load_state *de__load_get(de_asset_system *s, bool create) {
    if (s == NULL)
        return NULL;
    if (s->load != NULL || !create)
        return s->load;
    de__load_state *L = DE_NEW(s->alloc, de__load_state, DE_MEM_ASSET);
    if (L == NULL)
        return NULL;
    memset(L, 0, sizeof *L);
    for (u32 i = 0; i < DE__SLOT_MAX_PAGES; ++i)
        de_atomic_init_ptr(&L->slot_pages[i], NULL);
    de_mutex_init(&L->lock);
    s->load = L;
    return L;
}

/* ---- Backend ------------------------------------------------------------------------------- */

void de__asset_set_backend(de_asset_system *s, const de__load_backend *backend) {
    de__load_state *L = de__load_get(s, true);
    if (L == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__asset_set_backend: out of memory");
        return;
    }
    if (backend != NULL) {
        DE_ASSERT(backend->submit != NULL && backend->cancel != NULL &&
                  backend->reprioritize != NULL);
        L->backend = *backend;
        L->has_backend = true;
    } else {
        memset(&L->backend, 0, sizeof L->backend);
        L->has_backend = false;
    }
}

const de__load_backend *de__asset_backend(const de_asset_system *s) {
    return s != NULL && s->load != NULL && s->load->has_backend ? &s->load->backend : NULL;
}

/* ---- Per-slot load state ------------------------------------------------------------------- */

static de__load_slot *load_slot_at(de__load_state *L, u32 index) {
    de__load_slot *page = (de__load_slot *)de_atomic_load_ptr(
        &L->slot_pages[index >> DE__SLOT_PAGE_SHIFT], DE_ATOMIC_ACQUIRE);
    return page != NULL ? &page[index & (DE__SLOT_PAGE_SLOTS - 1u)] : NULL;
}

de__load_slot *de__load_slot_find(de_asset_system *s, u32 slot) {
    de__load_state *L = s != NULL ? s->load : NULL;
    if (L == NULL || slot == 0)
        return NULL;
    de__load_slot *ls = load_slot_at(L, de__slot_handle_index(slot));
    return ls != NULL && ls->gen == de__slot_handle_generation(slot) ? ls : NULL;
}

static void load_slot_clear_msg(de_asset_system *s, de__load_slot *ls) {
    if (ls->error_msg != NULL)
        DE_FREE(s->alloc, ls->error_msg, ls->error_msg_size);
    ls->error_msg = NULL;
    ls->error_msg_size = 0;
}

static void load_slot_set_msg(de_asset_system *s, de__load_slot *ls, const char *msg, u32 len) {
    load_slot_clear_msg(s, ls);
    if (len >= DE__LOAD_MSG_MAX)
        len = DE__LOAD_MSG_MAX - 1u;
    char *m = (char *)DE_ALLOC(s->alloc, (size_t)len + 1u, 1, DE_MEM_ASSET);
    if (m == NULL)
        return; /* the request falls back to a generated message */
    memcpy(m, msg, len);
    m[len] = '\0';
    ls->error_msg = m;
    ls->error_msg_size = len + 1u;
}

/* The per-slot state of live slot `slot`, created (or reset for a new occupant of the index) on
 * first use. NULL when out of memory. Main thread. */
static de__load_slot *load_slot_prepare(de_asset_system *s, de__load_state *L, u32 slot) {
    u32 index = de__slot_handle_index(slot);
    u32 gen = de__slot_handle_generation(slot);
    de__load_slot *ls = load_slot_at(L, index);
    if (ls == NULL) {
        size_t size = sizeof(de__load_slot) * DE__SLOT_PAGE_SLOTS;
        de__load_slot *page = (de__load_slot *)DE_ALLOC(s->alloc, size, 64, DE_MEM_ASSET);
        if (page == NULL)
            return NULL;
        memset(page, 0, size);
        for (u32 i = 0; i < DE__SLOT_PAGE_SLOTS; ++i) {
            de_atomic_init_u32(&page[i].started, 0);
            de_atomic_init_u64(&page[i].bytes_done, 0);
            de_atomic_init_u64(&page[i].bytes_total, 0);
        }
        /* Release: a backend thread that loads the page pointer sees initialised entries. */
        de_atomic_store_ptr(&L->slot_pages[index >> DE__SLOT_PAGE_SHIFT], page, DE_ATOMIC_RELEASE);
        ls = &page[index & (DE__SLOT_PAGE_SLOTS - 1u)];
    }
    if (ls->gen != gen) {
        /* A new occupant of this slot index: the old one was freed with no request left. */
        DE_ASSERT(ls->interest_total == 0 && !ls->in_flight);
        load_slot_clear_msg(s, ls);
        memset(ls->interest, 0, sizeof ls->interest);
        ls->interest_total = 0;
        ls->interest_events = 0; /* DENG-143 */
        ls->priority = DE__LOAD_PRIORITY_NORMAL;
        ls->in_flight = 0;
        de_atomic_store_u32(&ls->started, 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u64(&ls->bytes_done, 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u64(&ls->bytes_total, 0, DE_ATOMIC_RELAXED);
        ls->gen = gen;
    }
    return ls;
}

/* Marks a slot FAILED with `result` and `msg` (NULL: generated from the result when read). */
static void load_slot_fail(de_asset_system *s, de__slot *sl, de__load_slot *ls, de_result result,
                           const char *msg, u32 msg_len) {
    ls->in_flight = 0;
    sl->state = DE_LOAD_FAILED;
    sl->error = result != DE_OK ? result : DE_ERR_INTERNAL;
    if (msg != NULL && msg_len != 0)
        load_slot_set_msg(s, ls, msg, msg_len);
    else
        load_slot_clear_msg(s, ls);
    /* A finished slot counts all its known bytes as done. */
    u64 total = de_atomic_load_u64(&ls->bytes_total, DE_ATOMIC_RELAXED);
    u64 done = de_atomic_load_u64(&ls->bytes_done, DE_ATOMIC_RELAXED);
    u64 w = total > done ? total : done;
    de_atomic_store_u64(&ls->bytes_total, w, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&ls->bytes_done, w, DE_ATOMIC_RELAXED);
    /* DENG-143: per-asset DE_ASSET_EV_FAILED (only if a waiting request wants events). */
    if (ls->interest_events != 0) {
        de_asset_event ev = {DE_ASSET_EV_FAILED, {0}, sl->id, DE_LOAD_FAILED, sl->error};
        de__asset_emit(s, &ev);
    }
}

static void load_slot_ready(de_asset_system *s, u32 slot, de__slot *sl, de__load_slot *ls,
                            u64 bytes) {
    ls->in_flight = 0;
    sl->state = DE_LOAD_READY;
    sl->error = DE_OK;
    sl->bytes = bytes;
    sl->version++;
    load_slot_clear_msg(s, ls);
    u64 total = de_atomic_load_u64(&ls->bytes_total, DE_ATOMIC_RELAXED);
    u64 done = de_atomic_load_u64(&ls->bytes_done, DE_ATOMIC_RELAXED);
    u64 w = total > done ? total : done;
    if (w == 0)
        w = bytes; /* no progress was reported: weigh the asset by its resident size */
    de_atomic_store_u64(&ls->bytes_total, w, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&ls->bytes_done, w, DE_ATOMIC_RELAXED);
    de__budget_on_publish(s, slot, bytes); /* DENG-144: residency accounting, LRU */
    /* DENG-143: per-asset DE_ASSET_EV_LOADED (only if a waiting request wants events). */
    if (ls->interest_events != 0) {
        de_asset_event ev = {DE_ASSET_EV_LOADED, {0}, sl->id, DE_LOAD_READY, DE_OK};
        de__asset_emit(s, &ev);
    }
}

/* Starts loading `slot` with `priority`. Main thread. */
static void load_slot_submit(de_asset_system *s, de__load_state *L, u32 slot, de__slot *sl,
                             de__load_slot *ls, de__load_priority priority) {
    load_slot_clear_msg(s, ls);
    de_atomic_store_u32(&ls->started, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&ls->bytes_done, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&ls->bytes_total, 0, DE_ATOMIC_RELAXED);
    ls->priority = (u8)priority;
    ls->in_flight = 1;
    sl->state = DE_LOAD_PENDING;
    sl->error = DE_OK;
    if (L->has_backend) {
        L->backend.submit(L->backend.user, s, slot, priority);
        return;
    }
    static const char k_no_backend[] = "no asset load backend is installed";
    load_slot_fail(s, sl, ls, DE_ERR_UNSUPPORTED, k_no_backend, (u32)sizeof k_no_backend - 1u);
}

/* A new waiting request of `priority` on `slot`: submits or reprioritizes as needed. */
static void load_slot_want(de_asset_system *s, de__load_state *L, u32 slot, de__slot *sl,
                           de__load_slot *ls, de__load_priority priority) {
    /* A failure is retried only when no other waiting request still counts it as failed. */
    bool retry_failed = sl->state == DE_LOAD_FAILED && ls->interest_total == 0;
    ls->interest[priority]++;
    ls->interest_total++;
    switch (sl->state) {
    case DE_LOAD_INVALID:
    case DE_LOAD_CANCELLED:
        load_slot_submit(s, L, slot, sl, ls, priority);
        break;
    case DE_LOAD_FAILED:
        if (retry_failed)
            load_slot_submit(s, L, slot, sl, ls, priority);
        break;
    case DE_LOAD_PENDING:
    case DE_LOAD_LOADING:
        if (ls->in_flight && (u8)priority < ls->priority) {
            ls->priority = (u8)priority;
            if (L->has_backend)
                L->backend.reprioritize(L->backend.user, s, slot, priority);
        }
        break;
    default: /* READY counts at once */
        break;
    }
}

/* Drops the queued, not yet applied failures of `slot` (after a cancel): if the slot is submitted
 * again before the next update, an old failure must not end the new submission. Successes stay:
 * the data they published is loaded. */
static void load_purge_failures(de__load_state *L, u32 slot) {
    de_mutex_lock(&L->lock);
    de__load_queue *q = &L->queues[L->incoming];
    for (u32 i = 0; i < q->count; ++i)
        if (q->items[i].slot == slot && q->items[i].result != DE_OK)
            q->items[i].slot = 0;
    de_mutex_unlock(&L->lock);
}

void de__load_slot_unwant(de_asset_system *s, u32 slot, de__load_priority priority) {
    de__load_state *L = s != NULL ? s->load : NULL;
    de__load_slot *ls = de__load_slot_find(s, slot);
    de__slot *sl = de__slot_get(s, slot);
    if (L == NULL || ls == NULL || sl == NULL)
        return;
    DE_ASSERT(ls->interest[priority] != 0 && ls->interest_total != 0);
    if (ls->interest[priority] == 0 || ls->interest_total == 0)
        return;
    ls->interest[priority]--;
    ls->interest_total--;
    if (!ls->in_flight)
        return;
    if (ls->interest_total == 0) {
        ls->in_flight = 0;
        sl->state = DE_LOAD_CANCELLED;
        if (L->has_backend)
            L->backend.cancel(L->backend.user, s, slot);
        load_purge_failures(L, slot);
        return;
    }
    u8 best = DE__LOAD_PRIORITY_COUNT;
    for (u32 p = 0; p < DE__LOAD_PRIORITY_COUNT && best == DE__LOAD_PRIORITY_COUNT; ++p)
        if (ls->interest[p] != 0)
            best = (u8)p;
    if (best != ls->priority) {
        ls->priority = best;
        if (L->has_backend)
            L->backend.reprioritize(L->backend.user, s, slot, (de__load_priority)best);
    }
}

/* ---- Backend entry points (any thread) ----------------------------------------------------- */

/* Raises `*a` to `v`; returns by how much it grew (0 when `v` was not larger). */
static u64 atomic_max_u64(de_atomic_u64 *a, u64 v) {
    u64 cur = de_atomic_load_u64(a, DE_ATOMIC_RELAXED);
    while (v > cur) {
        if (de_atomic_cas_weak_u64(a, &cur, v, DE_ATOMIC_RELAXED, DE_ATOMIC_RELAXED))
            return v - cur;
    }
    return 0;
}

void de__slot_progress(de_asset_system *s, u32 slot, u64 bytes_done, u64 bytes_total) {
    if (de__slot_get(s, slot) == NULL)
        return;
    de__load_slot *ls = de__load_slot_find(s, slot);
    if (ls == NULL)
        return;
    if (bytes_total < bytes_done)
        bytes_total = bytes_done;
    (void)atomic_max_u64(&ls->bytes_total, bytes_total);
    /* DENG-144: the bytes read since the last report feed the I/O rate (de_asset_get_stats). */
    de__budget_add_io_bytes(s, atomic_max_u64(&ls->bytes_done, bytes_done));
    de_atomic_store_u32(&ls->started, 1, DE_ATOMIC_RELEASE);
}

static bool queue_reserve(const de_allocator *a, de__load_queue *q, u32 msg_len) {
    if (q->count == q->capacity) {
        u32 cap = q->capacity != 0 ? q->capacity * 2u : 64u;
        void *p = DE_REALLOC(a, q->items, sizeof(de__load_completion) * q->capacity,
                             sizeof(de__load_completion) * cap, DE_ALIGNOF(de__load_completion),
                             DE_MEM_ASSET);
        if (p == NULL)
            return false;
        q->items = (de__load_completion *)p;
        q->capacity = cap;
    }
    if (msg_len != 0 && q->text_capacity - q->text_len < msg_len) {
        u32 cap = q->text_capacity != 0 ? q->text_capacity : 1024u;
        while (cap - q->text_len < msg_len)
            cap *= 2u;
        void *p = DE_REALLOC(a, q->text, q->text_capacity, cap, 1, DE_MEM_ASSET);
        if (p == NULL)
            return false;
        q->text = (char *)p;
        q->text_capacity = cap;
    }
    return true;
}

void de__slot_complete(de_asset_system *s, u32 slot, de_result result, u64 bytes) {
    de__load_state *L = s != NULL ? s->load : NULL;
    if (L == NULL || slot == 0)
        return;
    const char *msg = NULL;
    u32 msg_len = 0;
    if (result != DE_OK) {
        const de_error *e = de_error_last();
        if (e != NULL && e->result == result) {
            msg = e->message;
            size_t n = strlen(msg);
            msg_len = (u32)(n < DE__LOAD_MSG_MAX - 1u ? n : DE__LOAD_MSG_MAX - 1u);
        }
    }
    de_mutex_lock(&L->lock);
    de__load_queue *q = &L->queues[L->incoming];
    if (!queue_reserve(s->alloc, q, msg_len) && !queue_reserve(s->alloc, q, 0)) {
        de_mutex_unlock(&L->lock);
        DE_ERROR(s->log, "asset slot 0x%08x: completion lost (out of memory)", slot);
        return;
    }
    de__load_completion *c = &q->items[q->count++];
    c->slot = slot;
    c->result = result;
    c->bytes = bytes;
    c->msg_offset = 0;
    c->msg_len = 0;
    if (msg_len != 0 && q->text_capacity - q->text_len >= msg_len) {
        memcpy(q->text + q->text_len, msg, msg_len);
        c->msg_offset = q->text_len;
        c->msg_len = msg_len;
        q->text_len += msg_len;
    }
    de_mutex_unlock(&L->lock);
}

/* Applies one queued completion. Main thread. */
static void load_apply(de_asset_system *s, const de__load_queue *q, const de__load_completion *c) {
    de__slot *sl = de__slot_get(s, c->slot);
    de__load_slot *ls = sl != NULL ? de__load_slot_find(s, c->slot) : NULL;
    if (ls == NULL)
        return; /* stale, or never requested */
    if (!ls->in_flight) {
        /* Cancelled, but the work raced to its end: a success is still a loaded asset. */
        if (c->result == DE_OK && sl->state != DE_LOAD_READY)
            load_slot_ready(s, c->slot, sl, ls, c->bytes);
        return;
    }
    if (c->result == DE_OK)
        load_slot_ready(s, c->slot, sl, ls, c->bytes);
    else
        load_slot_fail(s, sl, ls, c->result, c->msg_len != 0 ? q->text + c->msg_offset : NULL,
                       c->msg_len);
}

/* DENG-142: a completion applied at once (main thread, backend `update` hook). */
void de__slot_complete_now(de_asset_system *s, u32 slot, de_result result, u64 bytes,
                           const char *msg) {
    if (s == NULL || s->load == NULL || slot == 0)
        return;
    size_t n = msg != NULL ? strlen(msg) : 0u;
    if (n >= DE__LOAD_MSG_MAX)
        n = DE__LOAD_MSG_MAX - 1u;
    de__load_queue q;
    memset(&q, 0, sizeof q);
    q.text = (char *)(uintptr_t)msg; /* read only by load_apply */
    de__load_completion c = {slot, result, bytes, 0, (u32)n};
    load_apply(s, &q, &c);
}

/* ---- Requests ------------------------------------------------------------------------------ */

static de_load_handle request_handle(u32 index, u32 gen) {
    return (de_load_handle){de__slot_handle_make(index, gen)};
}

de__request *de__request_get(de_asset_system *s, de_load_handle h) {
    de__load_state *L = s != NULL ? s->load : NULL;
    if (L == NULL || h.id == 0)
        return NULL;
    u32 index = de__slot_handle_index(h.id);
    if (index >= L->request_count)
        return NULL;
    de__request *r = &L->requests[index];
    return r->live && r->gen == de__slot_handle_generation(h.id) ? r : NULL;
}

/* A free request index (FIFO reuse, see DE__REQUEST_REUSE_MIN), or DE__LOAD_MAX_REQUESTS when
 * none is left or memory ran out. */
static u32 request_take_index(de_asset_system *s, de__load_state *L) {
    bool fresh_left = L->request_count < DE__LOAD_MAX_REQUESTS;
    if (L->free_head != 0 && (L->free_count > DE__REQUEST_REUSE_MIN || !fresh_left)) {
        u32 index = L->free_head - 1u;
        de__request *r = &L->requests[index];
        L->free_head = r->free_next;
        if (L->free_head == 0)
            L->free_tail = 0;
        r->free_next = 0;
        L->free_count--;
        return index;
    }
    if (!fresh_left)
        return DE__LOAD_MAX_REQUESTS;
    if (L->request_count == L->request_capacity) {
        u32 cap = L->request_capacity != 0 ? L->request_capacity * 2u : 16u;
        if (cap > DE__LOAD_MAX_REQUESTS)
            cap = DE__LOAD_MAX_REQUESTS;
        void *p = DE_REALLOC(s->alloc, L->requests, sizeof(de__request) * L->request_capacity,
                             sizeof(de__request) * cap, DE_ALIGNOF(de__request), DE_MEM_ASSET);
        if (p == NULL)
            return DE__LOAD_MAX_REQUESTS;
        L->requests = (de__request *)p;
        memset(L->requests + L->request_capacity, 0,
               sizeof(de__request) * (cap - L->request_capacity));
        L->request_capacity = cap;
    }
    return L->request_count++;
}

static void request_put_free(de__load_state *L, u32 index) {
    de__request *r = &L->requests[index];
    r->free_next = 0;
    if (L->free_tail != 0)
        L->requests[L->free_tail - 1u].free_next = index + 1u;
    else
        L->free_head = index + 1u;
    L->free_tail = index + 1u;
    L->free_count++;
}

/* A new live request with room for `count` entries; *out_index set. NULL (error context set)
 * when out of memory or out of request indices. */
static de__request *request_new(de_asset_system *s, de__load_state *L, u32 count, u32 flags,
                                de__load_priority priority, u32 *out_index) {
    u32 index = request_take_index(s, L);
    if (index >= DE__LOAD_MAX_REQUESTS) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset load: no request slot left (%u live)",
                      L->live_requests);
        return NULL;
    }
    de__request *r = &L->requests[index];
    de__load_entry *entries = NULL;
    if (count != 0) {
        entries = (de__load_entry *)DE_ALLOC(s->alloc, sizeof(de__load_entry) * count,
                                             DE_ALIGNOF(de__load_entry), DE_MEM_ASSET);
        if (entries == NULL) {
            request_put_free(L, index);
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset load: out of memory");
            return NULL;
        }
    }
    u32 gen = r->gen >= DE__SLOT_GEN_MAX ? 1u : r->gen + 1u;
    memset(r, 0, sizeof *r);
    r->gen = gen;
    r->live = true;
    r->state = DE_LOAD_PENDING;
    r->flags = flags;
    r->priority = (u8)priority;
    r->entries = entries;
    r->count = 0;
    r->entry_capacity = count;
    r->first_failed = DE__LOAD_NONE;
    r->first_error = DE_OK;
    L->live_requests++;
    *out_index = index;
    return r;
}

/* Frees a request's entries and index; its refs must already be dropped. */
static void request_free(de_asset_system *s, de__load_state *L, u32 index) {
    de__request *r = &L->requests[index];
    if (r->entries != NULL)
        DE_FREE(s->alloc, r->entries, sizeof(de__load_entry) * r->entry_capacity);
    r->entries = NULL;
    r->count = 0;
    r->entry_capacity = 0;
    r->live = false;
    L->live_requests--;
    request_put_free(L, index);
}

static de__load_priority priority_from_flags(u32 flags, de__load_priority fallback) {
    if (flags & DE_LOAD_PRIORITY_CRITICAL)
        return DE__LOAD_PRIORITY_CRITICAL;
    if (flags & DE_LOAD_PRIORITY_HIGH)
        return DE__LOAD_PRIORITY_HIGH;
    if (flags & DE_LOAD_PRIORITY_LOW)
        return DE__LOAD_PRIORITY_LOW;
    if (flags & DE_LOAD_PRIORITY_BACKGROUND)
        return DE__LOAD_PRIORITY_BACKGROUND;
    return fallback;
}

/* Load and manifest priorities share their values (bundle priorities are used as they are).
 * Each is compared with a literal: MSVC warns (C5287) on comparing two enum types, casts or not. */
DE_STATIC_ASSERT(DE__LOAD_PRIORITY_CRITICAL == 0 && DE__MANIFEST_PRIORITY_CRITICAL == 0,
                 "priority values");
DE_STATIC_ASSERT(DE__LOAD_PRIORITY_HIGH == 1 && DE__MANIFEST_PRIORITY_HIGH == 1, "priority values");
DE_STATIC_ASSERT(DE__LOAD_PRIORITY_NORMAL == 2 && DE__MANIFEST_PRIORITY_NORMAL == 2,
                 "priority values");
DE_STATIC_ASSERT(DE__LOAD_PRIORITY_LOW == 3 && DE__MANIFEST_PRIORITY_LOW == 3, "priority values");
DE_STATIC_ASSERT(DE__LOAD_PRIORITY_BACKGROUND == 4 && DE__MANIFEST_PRIORITY_BACKGROUND == 4,
                 "priority values");

/* Adds the asset `id` to request `r` (not yet in it): takes a handle ref on its slot. */
static void request_add_entry(de_asset_system *s, de__request *r, de_asset_id id) {
    de__load_entry *e = &r->entries[r->count++];
    e->id = id;
    e->slot = 0;
    e->error = DE_OK;
    if (id.hash == 0) {
        e->error = DE_ERR_INVALID_ARGUMENT;
        return;
    }
    u32 h = de__asset_handle_of(s, id, NULL);
    if (h == 0) {
        const de_error *err = de_error_last();
        e->error = err != NULL && err->result != DE_OK ? err->result : DE_ERR_NOT_FOUND;
        return;
    }
    if (!de__slot_ref_handle(s, h)) {
        e->error = DE_ERR_INTERNAL;
        return;
    }
    e->slot = h;
}

/*
 * Creates a request for `ids` (duplicates dropped, first occurrence kept) and starts it: refs,
 * interest, submissions, then the first aggregate (READY slots count at once). Returns an invalid
 * handle only when out of memory (error context set).
 */
static de_load_handle request_create(de_asset_system *s, const de_asset_id *ids, u32 count,
                                     u32 flags, de__load_priority priority) {
    de__load_state *L = de__load_get(s, true);
    if (L == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset load: out of memory");
        return (de_load_handle){0};
    }
    u32 index = 0;
    de__request *r = request_new(s, L, count, flags, priority, &index);
    if (r == NULL)
        return (de_load_handle){0};

    /* Duplicates: a set of the IDs seen (probed only, never iterated). */
    de_hashmap seen;
    bool use_set = count > 1;
    if (use_set) {
        de_hashmap_desc hd = {.allocator = s->alloc, .key_size = sizeof(u64), .tag = DE_MEM_ASSET};
        if (de_hashmap_init(&seen, &hd) != DE_OK || de_hashmap_reserve(&seen, count) != DE_OK) {
            de_hashmap_free(&seen);
            request_free(s, L, index);
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset load: out of memory");
            return (de_load_handle){0};
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if (use_set) {
            void *unused = NULL;
            bool inserted = false;
            u64 key = ids[i].hash;
            /* Reserved above: get_or_put cannot fail here. */
            if (de_hashmap_get_or_put(&seen, key, &key, &unused, &inserted) == DE_OK && !inserted)
                continue;
        }
        request_add_entry(s, r, ids[i]);
    }
    if (use_set)
        de_hashmap_free(&seen);

    /* Interest and submissions. A request is created on the main thread between updates, so
     * no completion is applied while this runs (completions are queued). */
    for (u32 i = 0; i < r->count; ++i) {
        de__load_entry *e = &r->entries[i];
        if (e->slot == 0)
            continue;
        de__slot *sl = de__slot_get(s, e->slot);
        de__load_slot *ls = sl != NULL ? load_slot_prepare(s, L, e->slot) : NULL;
        if (ls == NULL) {
            (void)de__slot_unref_handle(s, e->slot);
            e->slot = 0;
            e->error = DE_ERR_OUT_OF_MEMORY;
            continue;
        }
        /* DENG-143: counted before the want, which can fail the slot at once (no backend). */
        if ((flags & DE_LOAD_NO_EVENTS) == 0)
            ls->interest_events++;
        load_slot_want(s, L, e->slot, sl, ls, priority);
    }
    de__request_refresh(s, r);
    return request_handle(index, r->gen);
}

/* A request that failed as a whole (unknown bundle): FAILED at once, no assets. */
static de_load_handle request_create_failed(de_asset_system *s, u32 flags, de_result code,
                                            const char *msg) {
    de__load_state *L = de__load_get(s, true);
    if (L == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset load: out of memory");
        return (de_load_handle){0};
    }
    u32 index = 0;
    de__request *r = request_new(s, L, 0, flags, DE__LOAD_PRIORITY_NORMAL, &index);
    if (r == NULL)
        return (de_load_handle){0};
    r->first_error = code;
    snprintf(r->first_error_msg, sizeof r->first_error_msg, "%s", msg);
    de__request_finish(s, r, DE_LOAD_FAILED);
    return request_handle(index, r->gen);
}

/* ---- Public load API ----------------------------------------------------------------------- */

de_load_handle de_asset_load(de_asset_system *s, de_asset_id id, u32 flags) {
    if (s == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_load: NULL system");
        return (de_load_handle){0};
    }
    return request_create(s, &id, 1, flags, priority_from_flags(flags, DE__LOAD_PRIORITY_NORMAL));
}

de_load_handle de_asset_load_many(de_asset_system *s, const de_asset_id *ids, u32 count,
                                  u32 flags) {
    if (s == NULL || (ids == NULL && count != 0)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_load_many: NULL system or ids");
        return (de_load_handle){0};
    }
    return request_create(s, ids, count, flags,
                          priority_from_flags(flags, DE__LOAD_PRIORITY_NORMAL));
}

de_load_handle de_asset_load_bundle(de_asset_system *s, const char *bundle, u32 flags) {
    if (s == NULL || bundle == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_load_bundle: NULL system or bundle");
        return (de_load_handle){0};
    }
    const de__resolved *res = s->resolved;
    const de__resolved_bundle *rb = res != NULL ? de__resolved_find_bundle(res, bundle) : NULL;
    if (rb == NULL) {
        char msg[DE__LOAD_MSG_MAX];
        snprintf(msg, sizeof msg, "bundle \"%.200s\" is not in the asset manifest", bundle);
        return request_create_failed(s, flags, DE_ERR_NOT_FOUND, msg);
    }
    de__load_priority priority =
        priority_from_flags(flags, (de__load_priority)rb->bundle->priority);

    /* The dependency bundles' members first (closure order: dependencies first), then the
     * bundle's own; request_create drops the duplicates. */
    u64 total = rb->member_count;
    for (u32 d = 0; d < rb->depends_count; ++d)
        total += res->bundles[rb->depends[d]].member_count;
    if (total > DE__SLOT_MAX) {
        char msg[DE__LOAD_MSG_MAX];
        snprintf(msg, sizeof msg, "bundle \"%.200s\" has more assets than slots", bundle);
        return request_create_failed(s, flags, DE_ERR_OUT_OF_MEMORY, msg);
    }
    de_asset_id *ids = NULL;
    if (total != 0) {
        ids = (de_asset_id *)DE_ALLOC(s->alloc, sizeof(de_asset_id) * (size_t)total,
                                      DE_ALIGNOF(de_asset_id), DE_MEM_ASSET);
        if (ids == NULL) {
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_asset_load_bundle: out of memory");
            return (de_load_handle){0};
        }
    }
    u32 n = 0;
    for (u32 d = 0; d < rb->depends_count; ++d) {
        const de__resolved_bundle *dep = &res->bundles[rb->depends[d]];
        if (dep->member_count == 0)
            continue; /* DENG-143: `ids` is NULL when every bundle is empty (UBSan) */
        memcpy(ids + n, dep->members, sizeof(de_asset_id) * dep->member_count);
        n += dep->member_count;
    }
    if (rb->member_count != 0)
        memcpy(ids + n, rb->members, sizeof(de_asset_id) * rb->member_count);
    n += rb->member_count;
    de_load_handle h = request_create(s, ids, n, flags, priority);
    if (DE_HANDLE_VALID(h))
        de__budget_pin_bundle(s, res, rb); /* DENG-144: `resident = true` bundles are pinned */
    if (ids != NULL)
        DE_FREE(s->alloc, ids, sizeof(de_asset_id) * (size_t)total);
    return h;
}

void de_asset_cancel(de_asset_system *s, de_load_handle h) {
    de__request *r = de__request_get(s, h);
    if (r == NULL || r->state >= DE_LOAD_READY)
        return; /* stale, or already final */
    de__request_finish(s, r, DE_LOAD_CANCELLED);
}

void de_asset_release(de_asset_system *s, de_load_handle h) {
    de__request *r = de__request_get(s, h);
    if (r == NULL)
        return;
    if (r->state < DE_LOAD_READY) {
        r->releasing = true; /* DENG-143: the handle is stale at once: no REQUEST_DONE */
        de__request_finish(s, r, DE_LOAD_CANCELLED);
    }
    for (u32 i = 0; i < r->count; ++i)
        if (r->entries[i].slot != 0)
            (void)de__slot_unref_handle(s, r->entries[i].slot);
    request_free(s, s->load, de__slot_handle_index(h.id));
}

/* ---- Update and shutdown ------------------------------------------------------------------- */

void de__load_update(de_asset_system *s) {
    de__load_state *L = s != NULL ? s->load : NULL;
    if (L == NULL || L->applying)
        return;
    L->applying = true;

    de_mutex_lock(&L->lock);
    de__load_queue *q = &L->queues[L->incoming];
    L->incoming ^= 1u;
    de_mutex_unlock(&L->lock);
    for (u32 i = 0; i < q->count; ++i)
        load_apply(s, q, &q->items[i]);
    q->count = 0;
    q->text_len = 0;

    /* DENG-142: the backend's own main-thread step (the pipeline publishes finished jobs here),
     * before the requests are refreshed, so they see its changes in this update. */
    if (L->has_backend && L->backend.update != NULL)
        L->backend.update(L->backend.user, s);

    /* Index order: refreshing one request never changes another's slots' states, except by
     * cancelling slots nobody else waits on. */
    for (u32 i = 0; i < L->request_count; ++i) {
        de__request *r = &L->requests[i];
        if (r->live && r->state < DE_LOAD_READY)
            de__request_refresh(s, r);
    }
    L->applying = false;
}

void de__load_shutdown(de_asset_system *s) {
    de__load_state *L = s != NULL ? s->load : NULL;
    if (L == NULL)
        return;
    for (u32 i = 0; i < L->request_count; ++i)
        if (L->requests[i].live)
            de_asset_release(s, request_handle(i, L->requests[i].gen));
    if (L->requests != NULL)
        DE_FREE(s->alloc, L->requests, sizeof(de__request) * L->request_capacity);
    for (u32 p = 0; p < DE__SLOT_MAX_PAGES; ++p) {
        de__load_slot *page =
            (de__load_slot *)de_atomic_load_ptr(&L->slot_pages[p], DE_ATOMIC_ACQUIRE);
        if (page == NULL)
            continue;
        for (u32 i = 0; i < DE__SLOT_PAGE_SLOTS; ++i)
            load_slot_clear_msg(s, &page[i]);
        DE_FREE(s->alloc, page, sizeof(de__load_slot) * DE__SLOT_PAGE_SLOTS);
    }
    for (u32 k = 0; k < 2; ++k) {
        de__load_queue *q = &L->queues[k];
        if (q->items != NULL)
            DE_FREE(s->alloc, q->items, sizeof(de__load_completion) * q->capacity);
        if (q->text != NULL)
            DE_FREE(s->alloc, q->text, q->text_capacity);
    }
    de_mutex_destroy(&L->lock);
    DE_FREE(s->alloc, L, sizeof *L);
    s->load = NULL;
}
