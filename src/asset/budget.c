/*
 * budget.c: per-category budgets, residency accounting, LRU eviction of unreferenced assets,
 * resident bundles and de_asset_flush_unused (SPEC §11.8; DENG-144). See budget_internal.h for
 * the rules; stats.c reports the totals.
 */

#include "asset/budget_internal.h"

#include "asset/asset_internal.h"
#include "asset/load_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <string.h>

/* ---- Lifetime -------------------------------------------------------------------------------- */

de_result de__budget_init(de_asset_system *s) {
    de__budget *b = DE_NEW(s->alloc, de__budget, DE_MEM_ASSET);
    if (b == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset budgets: out of memory");
    memset(b, 0, sizeof *b);
    de_atomic_init_u64(&b->io_bytes, 0);
    b->budgets = s->desc.budgets;
    b->frame = 1; /* 0 is the last_used_frame of a slot nobody used yet */
    s->budget = b;
    return DE_OK;
}

void de__budget_shutdown(de_asset_system *s) {
    de__budget *b = s != NULL ? s->budget : NULL;
    if (b == NULL)
        return;
    for (u32 p = 0; p < DE__SLOT_MAX_PAGES; ++p)
        if (b->pages[p] != NULL)
            DE_FREE(s->alloc, b->pages[p], sizeof(de__budget_slot) * DE__SLOT_PAGE_SLOTS);
    DE_FREE(s->alloc, b, sizeof *b);
    s->budget = NULL;
}

/* ---- Records --------------------------------------------------------------------------------- */

static de__budget_slot *rec_at(de__budget *b, u32 index) {
    de__budget_slot *page = b->pages[index >> DE__SLOT_PAGE_SHIFT];
    return page != NULL ? &page[index & (DE__SLOT_PAGE_SLOTS - 1u)] : NULL;
}

static u32 rec_slot(u32 index, const de__budget_slot *rec) {
    return de__slot_handle_make(index, rec->gen);
}

/* Takes a record off whichever list it is on. O(1). */
static void rec_unlink(de__budget *b, u32 index, de__budget_slot *rec) {
    if (rec->list == DE__BUDGET_LIST_NONE)
        return;
    u32 *head = rec->list == DE__BUDGET_LIST_LRU ? &b->lru_head[rec->category] : &b->pending_head;
    if (rec->prev != 0)
        rec_at(b, rec->prev - 1u)->next = rec->next;
    else
        *head = rec->next;
    if (rec->next != 0)
        rec_at(b, rec->next - 1u)->prev = rec->prev;
    else if (rec->list == DE__BUDGET_LIST_LRU)
        b->lru_tail[rec->category] = rec->prev;
    (void)index;
    rec->prev = 0;
    rec->next = 0;
    rec->list = DE__BUDGET_LIST_NONE;
}

/* Takes a record out of the lists and the totals: its slot occupant is gone. */
static void rec_forget(de__budget *b, u32 index, de__budget_slot *rec) {
    rec_unlink(b, index, rec);
    if (rec->accounted) {
        b->bytes[rec->category] -= rec->bytes;
        b->assets--;
    }
    rec->accounted = 0;
    rec->bytes = 0;
    rec->resident = 0;
}

/* The record of live slot `slot`. With `create`, the page is allocated and a record of an older
 * occupant of the index is reset; without it, NULL unless a record for this occupant exists.
 * NULL when out of memory. */
static de__budget_slot *rec_get(de_asset_system *s, de__budget *b, u32 slot, bool create) {
    u32 index = de__slot_handle_index(slot);
    u32 gen = de__slot_handle_generation(slot);
    de__budget_slot *rec = rec_at(b, index);
    if (rec == NULL) {
        if (!create)
            return NULL;
        size_t size = sizeof(de__budget_slot) * DE__SLOT_PAGE_SLOTS;
        de__budget_slot *page =
            (de__budget_slot *)DE_ALLOC(s->alloc, size, DE_ALIGNOF(de__budget_slot), DE_MEM_ASSET);
        if (page == NULL)
            return NULL;
        memset(page, 0, size);
        b->pages[index >> DE__SLOT_PAGE_SHIFT] = page;
        rec = &page[index & (DE__SLOT_PAGE_SLOTS - 1u)];
    }
    if (rec->gen != gen) {
        if (!create)
            return NULL;
        if (rec->gen != 0)
            rec_forget(b, index, rec); /* the slot was freed and its index reused */
        rec->gen = gen;
        rec->category = DE_ASSET_CAT_CPU;
    }
    if (index + 1u > b->index_end)
        b->index_end = index + 1u;
    return rec;
}

static de_asset_category slot_category(de_asset_system *s, const de__slot *sl) {
    const de_asset_type_desc *td = de__asset_type_get(s, de__slot_type(s, sl));
    u32 c = td != NULL ? (u32)td->category : (u32)DE_ASSET_CAT_CPU;
    return c < (u32)DE_ASSET_CAT_COUNT ? (de_asset_category)c : DE_ASSET_CAT_CPU;
}

static u64 budget_of(const de__budget *b, u32 category) {
    switch (category) {
    case DE_ASSET_CAT_GPU_TEXTURE:
        return b->budgets.gpu_texture_bytes;
    case DE_ASSET_CAT_GPU_MESH:
        return b->budgets.gpu_mesh_bytes;
    case DE_ASSET_CAT_AUDIO:
        return b->budgets.audio_bytes;
    default:
        return b->budgets.cpu_bytes;
    }
}

static const char *category_name(u32 category) {
    static const char *const names[DE_ASSET_CAT_COUNT] = {"cpu", "gpu texture", "gpu mesh",
                                                          "audio"};
    return category < (u32)DE_ASSET_CAT_COUNT ? names[category] : "?";
}

/* The live slot of a record, or NULL (the occupant is gone). */
static de__slot *rec_live_slot(de_asset_system *s, u32 index, const de__budget_slot *rec) {
    if (rec->gen == 0)
        return NULL;
    de__slot *sl = de__slot_get(s, rec_slot(index, rec));
    return sl;
}

/* A candidate: READY, counted, not pinned, unreferenced. */
static bool rec_evictable(const de__budget_slot *rec, const de__slot *sl) {
    return sl != NULL && rec->accounted && !rec->resident && sl->state == DE_LOAD_READY &&
           de__slot_ref_total(sl) == 0;
}

/* Puts a record into the pending batch with the current frame as its LRU key. */
static void rec_make_pending(de__budget *b, u32 index, de__budget_slot *rec, de__slot *sl) {
    sl->last_used_frame = b->frame;
    if (rec->list == DE__BUDGET_LIST_PENDING)
        return;
    rec_unlink(b, index, rec);
    rec->next = b->pending_head;
    rec->prev = 0;
    if (b->pending_head != 0)
        rec_at(b, b->pending_head - 1u)->prev = index + 1u;
    b->pending_head = index + 1u;
    rec->list = DE__BUDGET_LIST_PENDING;
}

/* ---- Ordering -------------------------------------------------------------------------------- */

/* LRU order: last_used_frame, then asset ID. A record whose occupant is gone sorts first (it is
 * dropped when eviction reaches it). */
static bool key_less(de_asset_system *s, de__budget *b, u32 a, u32 c) {
    const de__slot *sa = rec_live_slot(s, a, rec_at(b, a));
    const de__slot *sc = rec_live_slot(s, c, rec_at(b, c));
    u64 fa = sa != NULL ? sa->last_used_frame : 0u, fc = sc != NULL ? sc->last_used_frame : 0u;
    if (fa != fc)
        return fa < fc;
    return (sa != NULL ? sa->id.hash : 0u) < (sc != NULL ? sc->id.hash : 0u);
}

/* Merge sort of a chain linked through `next` (index + 1); returns the new head. Every record of
 * the chain has a live slot. No allocation; recursion depth log2(n). */
static u32 chain_sort(de_asset_system *s, de__budget *b, u32 head) {
    if (head == 0 || rec_at(b, head - 1u)->next == 0)
        return head;
    /* Split in halves (slow/fast walk). */
    u32 slow = head, fast = rec_at(b, head - 1u)->next;
    while (fast != 0) {
        fast = rec_at(b, fast - 1u)->next;
        if (fast != 0) {
            slow = rec_at(b, slow - 1u)->next;
            fast = rec_at(b, fast - 1u)->next;
        }
    }
    u32 second = rec_at(b, slow - 1u)->next;
    rec_at(b, slow - 1u)->next = 0;
    u32 l = chain_sort(s, b, head);
    u32 r = chain_sort(s, b, second);
    u32 out = 0, tail = 0;
    while (l != 0 || r != 0) {
        u32 take;
        if (r == 0 || (l != 0 && key_less(s, b, l - 1u, r - 1u))) {
            take = l;
            l = rec_at(b, l - 1u)->next;
        } else {
            take = r;
            r = rec_at(b, r - 1u)->next;
        }
        if (tail != 0)
            rec_at(b, tail - 1u)->next = take;
        else
            out = take;
        tail = take;
    }
    rec_at(b, tail - 1u)->next = 0;
    return out;
}

/* Inserts record `index` into its category's list after `after` (index + 1; 0 = at the head). */
static void lru_insert_after(de__budget *b, u32 after, u32 index, de__budget_slot *rec) {
    u32 c = rec->category;
    u32 next = after != 0 ? rec_at(b, after - 1u)->next : b->lru_head[c];
    rec->prev = after;
    rec->next = next;
    if (after != 0)
        rec_at(b, after - 1u)->next = index + 1u;
    else
        b->lru_head[c] = index + 1u;
    if (next != 0)
        rec_at(b, next - 1u)->prev = index + 1u;
    else
        b->lru_tail[c] = index + 1u;
    rec->list = DE__BUDGET_LIST_LRU;
}

/* Drain callback: a slot's refcount crossed zero. */
static void budget_on_candidate(de_asset_system *s, u32 slot, void *user) {
    de__budget *b = (de__budget *)user;
    de__slot *sl = de__slot_get(s, slot);
    if (sl == NULL || de__slot_ref_total(sl) != 0)
        return; /* referenced again since: not a candidate */
    de__budget_slot *rec = rec_get(s, b, slot, false);
    u32 index = de__slot_handle_index(slot);
    if (rec != NULL && rec_evictable(rec, sl)) {
        rec_make_pending(b, index, rec, sl);
        return;
    }
    /* Not a candidate (not READY, pinned): off the lists before its LRU key changes. */
    if (rec != NULL)
        rec_unlink(b, index, rec);
    sl->last_used_frame = b->frame;
}

/* Takes the drained candidates and merges the pending batch into the LRU lists. */
static void budget_commit(de_asset_system *s, de__budget *b) {
    (void)de__slot_eviction_drain(s, budget_on_candidate, b);
    if (b->pending_head == 0)
        return;

    /* Detach the batch; keep only the records that are still candidates. */
    u32 node = b->pending_head;
    b->pending_head = 0;
    u32 chain = 0;
    while (node != 0) {
        u32 index = node - 1u;
        de__budget_slot *rec = rec_at(b, index);
        node = rec->next;
        rec->prev = 0;
        rec->next = 0;
        rec->list = DE__BUDGET_LIST_NONE;
        if (!rec_evictable(rec, rec_live_slot(s, index, rec)))
            continue;
        rec->next = chain;
        chain = index + 1u;
    }
    chain = chain_sort(s, b, chain);

    /* Merge (ascending keys) into each category's list: the cursor of a category starts at the
     * last entry not after the first new key and only moves forward. */
    u32 cursor[DE_ASSET_CAT_COUNT] = {0};
    bool have_cursor[DE_ASSET_CAT_COUNT] = {false};
    while (chain != 0) {
        u32 index = chain - 1u;
        de__budget_slot *rec = rec_at(b, index);
        chain = rec->next;
        u32 c = rec->category;
        u32 pos;
        if (!have_cursor[c]) {
            pos = b->lru_tail[c];
            while (pos != 0 && key_less(s, b, index, pos - 1u))
                pos = rec_at(b, pos - 1u)->prev;
            have_cursor[c] = true;
        } else {
            pos = cursor[c];
            u32 nx = pos != 0 ? rec_at(b, pos - 1u)->next : b->lru_head[c];
            while (nx != 0 && key_less(s, b, nx - 1u, index)) {
                pos = nx;
                nx = rec_at(b, nx - 1u)->next;
            }
        }
        lru_insert_after(b, pos, index, rec);
        cursor[c] = index + 1u;
    }
}

/* ---- Eviction -------------------------------------------------------------------------------- */

static void budget_evict(de_asset_system *s, de__budget *b, u32 index, de__budget_slot *rec,
                         de__slot *sl) {
    u32 slot = rec_slot(index, rec);
    rec_unlink(b, index, rec);

    /* The object goes to its type's unload, then the refs it held on its dependencies go (so
     * they become candidates themselves), as the pipeline does on a publish again. */
    void *obj = de_atomic_exchange_ptr(&sl->object, NULL, DE_ATOMIC_ACQ_REL);
    if (obj != NULL) {
        const de_asset_type_desc *td = de__asset_type_get(s, de__slot_type(s, sl));
        if (td != NULL && td->unload != NULL)
            td->unload(obj, td->user);
    }
    for (u32 i = 0; i < sl->dep_count; ++i) {
        u32 h = de__slot_find(s, sl->deps[i]);
        if (h != 0)
            (void)de__slot_unref_asset(s, h);
    }
    if (sl->deps != NULL)
        DE_FREE(s->alloc, sl->deps, sizeof(de_asset_id) * sl->dep_count);
    sl->deps = NULL;
    sl->dep_count = 0;

    b->bytes[rec->category] -= rec->bytes;
    b->assets--;
    rec->accounted = 0;
    rec->bytes = 0;

    /* Unloaded: the slot (and every typed handle to it) stays; the next request loads it. */
    sl->bytes = 0;
    sl->state = DE_LOAD_INVALID;
    sl->error = DE_OK;
    de__load_slot *ls = de__load_slot_find(s, slot);
    if (ls != NULL) {
        de_atomic_store_u64(&ls->bytes_done, 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u64(&ls->bytes_total, 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&ls->started, 0, DE_ATOMIC_RELAXED);
    }
    b->evictions_total++;

    de_asset_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = DE_ASSET_EV_EVICTED;
    ev.id = sl->id;
    ev.state = DE_LOAD_INVALID;
    ev.error = DE_OK;
    de__asset_emit(s, &ev);
}

/* Evicts the head of category `c` if it is a candidate. Drops a head that is not (referenced
 * again, no longer READY, or gone). Returns 1 for an eviction, 0 otherwise; *empty when the list
 * was empty. */
static u32 budget_evict_head(de_asset_system *s, de__budget *b, u32 c, bool *empty) {
    u32 node = b->lru_head[c];
    *empty = node == 0;
    if (node == 0)
        return 0;
    u32 index = node - 1u;
    de__budget_slot *rec = rec_at(b, index);
    de__slot *sl = rec_live_slot(s, index, rec);
    if (sl == NULL) {
        rec_forget(b, index, rec);
        rec->gen = 0;
        return 0;
    }
    if (!rec_evictable(rec, sl)) {
        rec_unlink(b, index, rec); /* queued again when its refcount crosses zero again */
        return 0;
    }
    budget_evict(s, b, index, rec, sl);
    return 1;
}

static void budget_warn_over(de_asset_system *s, de__budget *b, u32 c, u64 now_ns) {
    if (b->warned[c] && now_ns - b->warned_ns[c] < DE__BUDGET_WINDOW_NS)
        return;
    b->warned[c] = true;
    b->warned_ns[c] = now_ns;
    b->over_budget_warns++;
    u64 limit = budget_of(b, c);
    DE_WARN(s->log,
            "asset budget \"%s\": %llu bytes resident, %llu over the budget of %llu, and no "
            "unreferenced asset is left to evict",
            category_name(c), (unsigned long long)b->bytes[c],
            (unsigned long long)(b->bytes[c] - limit), (unsigned long long)limit);
}

/* The I/O rate: bytes since the oldest sample of the last second (or since the newest older
 * one), per second. */
static void budget_sample_io(de__budget *b, u64 now_ns) {
    u64 total = de_atomic_load_u64(&b->io_bytes, DE_ATOMIC_RELAXED);
    const de__budget_io_sample *ref = NULL;
    for (u32 k = 0; k < b->io_sample_count; ++k) {
        u32 i = (b->io_sample_next + DE__BUDGET_IO_SAMPLES - 1u - k) % DE__BUDGET_IO_SAMPLES;
        const de__budget_io_sample *smp = &b->io_samples[i];
        u64 age = now_ns > smp->t_ns ? now_ns - smp->t_ns : 0u;
        if (age <= DE__BUDGET_WINDOW_NS) {
            ref = smp;
            continue;
        }
        if (ref == NULL)
            ref = smp;
        break;
    }
    if (ref == NULL) {
        b->io_rate = 0;
    } else if (now_ns > ref->t_ns) {
        f64 delta = (f64)(total - ref->bytes);
        b->io_rate = (u64)(delta * 1e9 / (f64)(now_ns - ref->t_ns));
    }
    const de__budget_io_sample *newest =
        b->io_sample_count != 0 ? &b->io_samples[(b->io_sample_next + DE__BUDGET_IO_SAMPLES - 1u) %
                                                 DE__BUDGET_IO_SAMPLES]
                                : NULL;
    if (newest == NULL || now_ns < newest->t_ns ||
        now_ns - newest->t_ns >= DE__BUDGET_WINDOW_NS / 32u) {
        b->io_samples[b->io_sample_next] = (de__budget_io_sample){now_ns, total};
        b->io_sample_next = (b->io_sample_next + 1u) % DE__BUDGET_IO_SAMPLES;
        if (b->io_sample_count < DE__BUDGET_IO_SAMPLES)
            b->io_sample_count++;
    }
}

void de__budget_update(de_asset_system *s, u64 now_ns) {
    de__budget *b = s != NULL ? s->budget : NULL;
    if (b == NULL)
        return;
    budget_commit(s, b);

    /* Categories in enum order; each one's least recently used candidates first. */
    u32 evicted = 0;
    for (u32 c = 0; c < (u32)DE_ASSET_CAT_COUNT; ++c) {
        u64 limit = budget_of(b, c);
        if (limit == 0)
            continue;
        while (b->bytes[c] > limit && evicted < DE__BUDGET_EVICT_CAP) {
            bool empty = false;
            evicted += budget_evict_head(s, b, c, &empty);
            if (empty) {
                budget_warn_over(s, b, c, now_ns);
                break;
            }
        }
    }
    budget_sample_io(b, now_ns);
    b->frame++;
}

/* ---- Hooks ----------------------------------------------------------------------------------- */

void de__budget_on_publish(de_asset_system *s, u32 slot, u64 bytes) {
    de__budget *b = s != NULL ? s->budget : NULL;
    de__slot *sl = b != NULL ? de__slot_get(s, slot) : NULL;
    if (sl == NULL)
        return;
    de__budget_slot *rec = rec_get(s, b, slot, true);
    if (rec == NULL) {
        DE_ERROR(s->log, "asset %016llx: out of memory for its budget record; it is not counted",
                 (unsigned long long)sl->id.hash);
        return;
    }
    u32 index = de__slot_handle_index(slot);
    rec_unlink(b, index, rec); /* a publish again may change the category */
    if (rec->accounted) {
        b->bytes[rec->category] -= rec->bytes;
    } else {
        b->assets++;
        rec->accounted = 1;
    }
    rec->category = (u8)slot_category(s, sl);
    rec->bytes = bytes;
    b->bytes[rec->category] += bytes;
    /* Normally a request holds the slot; one that finished after its last request went away
     * (a cancel that raced the load) is a candidate at once. */
    if (rec_evictable(rec, sl))
        rec_make_pending(b, index, rec, sl);
}

void de__budget_touch(de_asset_system *s, u32 slot) {
    de__budget *b = s != NULL ? s->budget : NULL;
    de__slot *sl = b != NULL ? de__slot_get(s, slot) : NULL;
    if (sl == NULL)
        return;
    de__budget_slot *rec = rec_get(s, b, slot, false);
    /* A listed slot moves to the pending batch before its LRU key changes. */
    if (rec != NULL && rec->list == DE__BUDGET_LIST_LRU)
        rec_make_pending(b, de__slot_handle_index(slot), rec, sl);
    sl->last_used_frame = b->frame;
}

static void budget_pin_members(de_asset_system *s, de__budget *b, const de__resolved_bundle *rb) {
    if (!rb->bundle->resident)
        return;
    for (u32 i = 0; i < rb->member_count; ++i) {
        u32 h = de__slot_find(s, rb->members[i]);
        if (h == 0)
            continue;
        de__budget_slot *rec = rec_get(s, b, h, true);
        if (rec == NULL) {
            DE_ERROR(s->log, "bundle \"%s\": out of memory pinning asset %016llx", rb->bundle->name,
                     (unsigned long long)rb->members[i].hash);
            continue;
        }
        rec->resident = 1;
        rec_unlink(b, de__slot_handle_index(h), rec);
    }
}

void de__budget_pin_bundle(de_asset_system *s, const de__resolved *res,
                           const de__resolved_bundle *rb) {
    de__budget *b = s != NULL ? s->budget : NULL;
    if (b == NULL || res == NULL || rb == NULL)
        return;
    budget_pin_members(s, b, rb);
    for (u32 d = 0; d < rb->depends_count; ++d)
        budget_pin_members(s, b, &res->bundles[rb->depends[d]]);
}

/* DENG-212: the members of resident pack bundles (mount.c gives them as IDs). */
void de__budget_pin_ids(de_asset_system *s, const de_asset_id *ids, u32 count) {
    de__budget *b = s != NULL ? s->budget : NULL;
    if (b == NULL)
        return;
    for (u32 i = 0; i < count; ++i) {
        u32 h = de__slot_find(s, ids[i]);
        if (h == 0)
            continue;
        de__budget_slot *rec = rec_get(s, b, h, true);
        if (rec == NULL) {
            DE_ERROR(s->log, "pack bundle: out of memory pinning asset %016llx",
                     (unsigned long long)ids[i].hash);
            continue;
        }
        rec->resident = 1;
        rec_unlink(b, de__slot_handle_index(h), rec);
    }
}

void de__budget_add_io_bytes(de_asset_system *s, u64 bytes) {
    de__budget *b = s != NULL ? s->budget : NULL;
    if (b != NULL && bytes != 0)
        (void)de_atomic_fetch_add_u64(&b->io_bytes, bytes, DE_ATOMIC_RELAXED);
}

bool de__budget_is_resident(de_asset_system *s, u32 slot) {
    de__budget *b = s != NULL ? s->budget : NULL;
    const de__budget_slot *rec = b != NULL ? rec_get(s, b, slot, false) : NULL;
    return rec != NULL && rec->resident;
}

/* ---- Public API ------------------------------------------------------------------------------ */

void de_asset_set_budgets(de_asset_system *s, const de_asset_budgets *budgets) {
    if (s == NULL || s->budget == NULL)
        return;
    if (budgets != NULL)
        s->budget->budgets = *budgets;
    else
        memset(&s->budget->budgets, 0, sizeof s->budget->budgets);
}

void de_asset_flush_unused(de_asset_system *s) {
    de__budget *b = s != NULL ? s->budget : NULL;
    if (b == NULL)
        return;
    /* Until an eviction leaves nothing new behind: evicting an asset drops its refs on its
     * dependencies, which then become candidates too. */
    for (;;) {
        budget_commit(s, b);
        u32 evicted = 0;
        for (u32 c = 0; c < (u32)DE_ASSET_CAT_COUNT; ++c) {
            bool empty = false;
            while (!empty)
                evicted += budget_evict_head(s, b, c, &empty);
        }
        /* Candidates that never reached a list (the slots' candidate list is a hint too). */
        for (u32 index = 0; index < b->index_end; ++index) {
            de__budget_slot *rec = rec_at(b, index);
            if (rec == NULL || rec->gen == 0 || !rec->accounted ||
                rec->list != DE__BUDGET_LIST_NONE)
                continue;
            de__slot *sl = rec_live_slot(s, index, rec);
            if (sl == NULL) {
                rec_forget(b, index, rec);
                rec->gen = 0;
                continue;
            }
            if (rec_evictable(rec, sl)) {
                budget_evict(s, b, index, rec, sl);
                evicted++;
            }
        }
        if (evicted == 0)
            break;
    }
}

void de__budget_resize_mesh(de_asset_system *s, de_asset_id id, const void *object, u64 bytes) {
    if (!s || !s->budget || !id.hash)
        return;
    u32 handle = de__slot_find(s, id);
    de__slot *sl = de__slot_get(s, handle);
    if (!sl || sl->state != DE_LOAD_READY || de__slot_object(sl) != object)
        return;
    de__budget_slot *rec = rec_get(s, s->budget, handle, false);
    if (!rec || !rec->accounted || rec->category != DE_ASSET_CAT_GPU_MESH)
        return;
    s->budget->bytes[DE_ASSET_CAT_GPU_MESH] -= rec->bytes;
    rec->bytes = bytes;
    sl->bytes = bytes;
    s->budget->bytes[DE_ASSET_CAT_GPU_MESH] += bytes;
}
