/*
 * slots.c: the asset slot table, handles with generations, refcounts and the eviction-candidate
 * list (SPEC §11.1, §11.8; DENG-140). See slots_internal.h for the rules.
 */

#include "asset/slots_internal.h"

#include "asset/asset_internal.h"
#include "asset/hot_reload_internal.h" /* DENG-214: de__slot_swap, de__slot_for_each_live */

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "core/de_hashmap.h"
#include "platform/de_atomic.h"

#include <string.h>

/*
 * Free slots are reused in FIFO order, and only once more than DE__SLOT_REUSE_MIN slots are free
 * (or no fresh index is left). So one index comes back at most every DE__SLOT_REUSE_MIN frees,
 * and a stale handle aliases a new occupant only after 4095 * DE__SLOT_REUSE_MIN frees instead of
 * 4095 frees of one asset loaded and evicted in a loop.
 */
#define DE__SLOT_REUSE_MIN 64u

typedef struct de__slot_table {
    de_pool *page_pool;                      /* pages of DE__SLOT_PAGE_SLOTS slots */
    de_atomic_ptr pages[DE__SLOT_MAX_PAGES]; /* de__slot[DE__SLOT_PAGE_SLOTS]; NULL = not yet */
    u32 slot_count;                          /* indices handed out so far (high-water mark) */
    u32 live_count;
    u32 free_count;
    u32 free_head, free_tail;   /* index + 1; FIFO through de__slot.free_next */
    de_hashmap by_id;           /* u64 id -> u32 index; lookup only */
    de_atomic_u32 evict_head;   /* index + 1 of the newest candidate; 0 = empty */
    de_atomic_u32 stale_logged; /* 1 after the first stale lookup was logged */
} de__slot_table;

/* ---- Table ------------------------------------------------------------------------------- */

static de__slot_table *slots_table(de_asset_system *s, bool create) {
    if (s->slots != NULL || !create)
        return s->slots;
    de__slot_table *t = DE_NEW(s->alloc, de__slot_table, DE_MEM_ASSET);
    if (t == NULL)
        return NULL;
    memset(t, 0, sizeof *t);
    for (u32 i = 0; i < DE__SLOT_MAX_PAGES; ++i)
        de_atomic_init_ptr(&t->pages[i], NULL);
    de_atomic_init_u32(&t->evict_head, 0);
    de_atomic_init_u32(&t->stale_logged, 0);

    de_pool_desc pd = {.block_size = sizeof(de__slot) * DE__SLOT_PAGE_SLOTS,
                       .block_align = DE_ALIGNOF(de__slot) > 64 ? DE_ALIGNOF(de__slot) : 64,
                       .blocks_per_page = 1,
                       .backing = s->alloc,
                       .tag = DE_MEM_ASSET};
    de_hashmap_desc hd = {.allocator = s->alloc,
                          .key_size = sizeof(u64),
                          .value_size = sizeof(u32),
                          .tag = DE_MEM_ASSET};
    if (de_pool_create(&pd, &t->page_pool) != DE_OK || de_hashmap_init(&t->by_id, &hd) != DE_OK) {
        de_pool_destroy(t->page_pool);
        DE_FREE(s->alloc, t, sizeof *t);
        return NULL;
    }
    s->slots = t;
    return t;
}

static de__slot *slots_at(de__slot_table *t, u32 index) {
    de__slot *page =
        (de__slot *)de_atomic_load_ptr(&t->pages[index >> DE__SLOT_PAGE_SHIFT], DE_ATOMIC_ACQUIRE);
    return page != NULL ? &page[index & (DE__SLOT_PAGE_SLOTS - 1u)] : NULL;
}

/* Allocates the page that holds `index`. Its slots are free with generation 0. */
static bool slots_add_page(de__slot_table *t, u32 index) {
    de__slot *page = (de__slot *)de_pool_alloc(t->page_pool);
    if (page == NULL)
        return false;
    memset(page, 0, sizeof(de__slot) * DE__SLOT_PAGE_SLOTS);
    for (u32 i = 0; i < DE__SLOT_PAGE_SLOTS; ++i) {
        de__slot *sl = &page[i];
        de_atomic_init_u64(&sl->refs, 0);
        de_atomic_init_ptr(&sl->object, NULL);
        de_atomic_init_u32(&sl->live_gen, 0);
        de_atomic_init_u32(&sl->queued, 0);
        de_atomic_init_u32(&sl->evict_next, 0);
    }
    /* Release: a thread that loads the page pointer sees the initialised slots. */
    de_atomic_store_ptr(&t->pages[index >> DE__SLOT_PAGE_SHIFT], page, DE_ATOMIC_RELEASE);
    return true;
}

/* An index for a new slot: a free one (FIFO) if enough are free or no fresh one is left, else a
 * fresh one. DE__SLOT_MAX when every index is live or a new page cannot be allocated. */
static u32 slots_take_index(de__slot_table *t) {
    bool fresh_left = t->slot_count < DE__SLOT_MAX;
    if (t->free_head != 0 && (t->free_count > DE__SLOT_REUSE_MIN || !fresh_left)) {
        u32 index = t->free_head - 1u;
        de__slot *sl = slots_at(t, index);
        t->free_head = sl->free_next;
        if (t->free_head == 0)
            t->free_tail = 0;
        sl->free_next = 0;
        t->free_count--;
        return index;
    }
    if (!fresh_left)
        return DE__SLOT_MAX;
    u32 index = t->slot_count;
    if ((index & (DE__SLOT_PAGE_SLOTS - 1u)) == 0 && !slots_add_page(t, index))
        return DE__SLOT_MAX; /* no memory for the page */
    t->slot_count++;
    return index;
}

static void slots_put_free(de__slot_table *t, u32 index, de__slot *sl) {
    sl->free_next = 0;
    if (t->free_tail != 0)
        slots_at(t, t->free_tail - 1u)->free_next = index + 1u;
    else
        t->free_head = index + 1u;
    t->free_tail = index + 1u;
    t->free_count++;
}

/* ---- Lookup ------------------------------------------------------------------------------ */

de__slot *de__slot_get(de_asset_system *s, u32 handle_id) {
    if (s == NULL || handle_id == 0)
        return NULL;
    de__slot_table *t = s->slots;
    u32 index = de__slot_handle_index(handle_id);
    u32 gen = de__slot_handle_generation(handle_id);
    de__slot *sl = t != NULL ? slots_at(t, index) : NULL;
    if (sl != NULL && gen != 0 && de_atomic_load_u32(&sl->live_gen, DE_ATOMIC_ACQUIRE) == gen)
        return sl;
#if defined(DE_DEBUG) || defined(DE_DEV)
    if (t != NULL && de_atomic_exchange_u32(&t->stale_logged, 1, DE_ATOMIC_RELAXED) == 0)
        DE_WARN(s->log,
                "stale asset handle 0x%08x (slot %u, generation %u); further stale handles are "
                "not logged",
                handle_id, index, gen);
#endif
    return NULL;
}

u32 de__slot_find(de_asset_system *s, de_asset_id id) {
    de__slot_table *t = s != NULL ? s->slots : NULL;
    if (t == NULL || id.hash == 0)
        return 0;
    const u32 *index = DE_HASHMAP_GET(&t->by_id, u32, id.hash, &id.hash);
    if (index == NULL)
        return 0;
    const de__slot *sl = slots_at(t, *index);
    return de__slot_handle_make(*index, de_atomic_load_u32(&sl->live_gen, DE_ATOMIC_RELAXED));
}

u32 de__slot_live_count(const de_asset_system *s) {
    return s != NULL && s->slots != NULL ? s->slots->live_count : 0u;
}

/* ---- Create and free --------------------------------------------------------------------- */

de_result de__slot_create(de_asset_system *s, de_asset_id id, u64 type_hash, u32 *out_handle) {
    if (out_handle != NULL)
        *out_handle = 0;
    if (s == NULL || out_handle == NULL || id.hash == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__slot_create: NULL argument or asset ID 0");
    de__slot_table *t = slots_table(s, true);
    if (t == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset slots: out of memory");

    u32 *mapped = NULL;
    bool inserted = false;
    if (de_hashmap_get_or_put(&t->by_id, id.hash, &id.hash, (void **)&mapped, &inserted) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset slots: out of memory");
    if (!inserted) {
        const de__slot *sl = slots_at(t, *mapped);
        *out_handle =
            de__slot_handle_make(*mapped, de_atomic_load_u32(&sl->live_gen, DE_ATOMIC_RELAXED));
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "asset %016llx already has a slot",
                       (unsigned long long)id.hash);
    }

    u32 index = slots_take_index(t);
    if (index >= DE__SLOT_MAX) {
        bool full = t->slot_count >= DE__SLOT_MAX && t->free_head == 0;
        de_hashmap_remove(&t->by_id, id.hash, &id.hash, NULL);
        return full ? DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                              "asset slots: all %u slots are in use (asset %016llx)", DE__SLOT_MAX,
                              (unsigned long long)id.hash)
                    : DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset slots: out of memory");
    }
    *mapped = index;

    de__slot *sl = slots_at(t, index);
    sl->id = id;
    sl->type_hash = type_hash;
    sl->type = de__asset_type_find_hash(s, type_hash);
    sl->bytes = 0;
    sl->last_used_frame = 0;
    sl->deps = NULL;
    sl->dep_count = 0;
    sl->state = DE_LOAD_INVALID;
    sl->version = 0;
    sl->error = DE_OK;
    de_atomic_store_u64(&sl->refs, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_ptr(&sl->object, NULL, DE_ATOMIC_RELAXED);
    /* `queued` is left alone: a freed slot may still sit on the candidate list, and only the
     * drain takes it off (a second push would corrupt the list). */
    sl->gen = sl->gen >= DE__SLOT_GEN_MAX ? 1u : sl->gen + 1u;
    /* Release: a thread that validates the handle sees the fields above. */
    de_atomic_store_u32(&sl->live_gen, sl->gen, DE_ATOMIC_RELEASE);
    t->live_count++;
    *out_handle = de__slot_handle_make(index, sl->gen);
    return DE_OK;
}

/* Unloads the object of a live slot through its type, if both exist. */
static void slots_unload(de_asset_system *s, de__slot *sl) {
    void *obj = de_atomic_exchange_ptr(&sl->object, NULL, DE_ATOMIC_ACQ_REL);
    if (obj == NULL)
        return;
    const de_asset_type_desc *td = de__asset_type_get(s, de__slot_type(s, sl));
    if (td != NULL && td->unload != NULL)
        td->unload(obj, td->user);
}

static void slots_free_deps(de_asset_system *s, de__slot *sl) {
    if (sl->deps != NULL)
        DE_FREE(s->alloc, sl->deps, sizeof(de_asset_id) * sl->dep_count);
    sl->deps = NULL;
    sl->dep_count = 0;
}

de_result de__slot_free(de_asset_system *s, u32 handle_id) {
    de__slot *sl = de__slot_get(s, handle_id);
    if (sl == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__slot_free: stale or invalid handle 0x%08x",
                       handle_id);
    if (de_atomic_load_u64(&sl->refs, DE_ATOMIC_ACQUIRE) != 0)
        return DE_FAIL(DE_ERR_BUSY, "de__slot_free: asset %016llx is still referenced",
                       (unsigned long long)sl->id.hash);
    de__slot_table *t = s->slots;
    u32 index = de__slot_handle_index(handle_id);

    slots_unload(s, sl);
    slots_free_deps(s, sl);
    de_hashmap_remove(&t->by_id, sl->id.hash, &sl->id.hash, NULL);
    sl->id = (de_asset_id){0};
    sl->type = (de_asset_type){0};
    sl->type_hash = 0;
    sl->state = DE_LOAD_INVALID;
    /* Every handle to this occupant is stale from here on. */
    de_atomic_store_u32(&sl->live_gen, 0, DE_ATOMIC_RELEASE);
    t->live_count--;
    slots_put_free(t, index, sl);
    return DE_OK;
}

/* ---- Refcounts --------------------------------------------------------------------------- */

/* Puts a slot on the eviction-candidate list unless it already is. Lock-free; any thread. Many
 * threads push, one thread (the drain) takes the whole list at once, so there is no ABA case. */
static void slots_enqueue_candidate(de__slot_table *t, de__slot *sl, u32 index) {
    u32 expected = 0;
    if (!de_atomic_cas_u32(&sl->queued, &expected, 1, DE_ATOMIC_ACQ_REL, DE_ATOMIC_RELAXED))
        return;
    u32 head = de_atomic_load_u32(&t->evict_head, DE_ATOMIC_RELAXED);
    do {
        de_atomic_store_u32(&sl->evict_next, head, DE_ATOMIC_RELAXED);
    } while (!de_atomic_cas_weak_u32(&t->evict_head, &head, index + 1u, DE_ATOMIC_RELEASE,
                                     DE_ATOMIC_RELAXED));
}

static bool slots_ref(de_asset_system *s, u32 handle_id, u64 unit) {
    de__slot *sl = de__slot_get(s, handle_id);
    if (sl == NULL)
        return false;
    u64 old = de_atomic_fetch_add_u64(&sl->refs, unit, DE_ATOMIC_RELAXED);
    DE_ASSERT((u32)(unit == DE__SLOT_REF_HANDLE ? old : old >> 32) != 0xFFFFFFFFu);
    (void)old;
    return true;
}

static bool slots_unref(de_asset_system *s, u32 handle_id, u64 unit) {
    de__slot *sl = de__slot_get(s, handle_id);
    if (sl == NULL)
        return false;
    /* A CAS loop rather than fetch_sub, so an unbalanced unref cannot borrow from the other
     * count. */
    u64 old = de_atomic_load_u64(&sl->refs, DE_ATOMIC_RELAXED);
    for (;;) {
        u32 part = unit == DE__SLOT_REF_HANDLE ? (u32)old : (u32)(old >> 32);
        DE_ASSERT(part != 0); /* an unref without a matching ref */
        if (part == 0)
            return false;
        if (de_atomic_cas_weak_u64(&sl->refs, &old, old - unit, DE_ATOMIC_ACQ_REL,
                                   DE_ATOMIC_RELAXED))
            break;
    }
    if (old != unit)
        return false;
    slots_enqueue_candidate(s->slots, sl, de__slot_handle_index(handle_id));
    return true;
}

bool de__slot_ref_handle(de_asset_system *s, u32 handle_id) {
    return slots_ref(s, handle_id, DE__SLOT_REF_HANDLE);
}
bool de__slot_unref_handle(de_asset_system *s, u32 handle_id) {
    return slots_unref(s, handle_id, DE__SLOT_REF_HANDLE);
}
bool de__slot_ref_asset(de_asset_system *s, u32 handle_id) {
    return slots_ref(s, handle_id, DE__SLOT_REF_ASSET);
}
bool de__slot_unref_asset(de_asset_system *s, u32 handle_id) {
    return slots_unref(s, handle_id, DE__SLOT_REF_ASSET);
}

u32 de__slot_eviction_drain(de_asset_system *s, de__slot_evict_fn fn, void *user) {
    de__slot_table *t = s != NULL ? s->slots : NULL;
    if (t == NULL)
        return 0;
    u32 node = de_atomic_exchange_u32(&t->evict_head, 0, DE_ATOMIC_ACQUIRE);
    u32 calls = 0;
    while (node != 0) {
        u32 index = node - 1u;
        de__slot *sl = slots_at(t, index);
        node = de_atomic_load_u32(&sl->evict_next, DE_ATOMIC_RELAXED);
        /* Read `next` before clearing `queued`: once it is 0, another thread may push the slot
         * again and overwrite evict_next. */
        de_atomic_store_u32(&sl->queued, 0, DE_ATOMIC_RELEASE);
        u32 gen = de_atomic_load_u32(&sl->live_gen, DE_ATOMIC_RELAXED);
        if (gen != 0 && fn != NULL) {
            fn(s, de__slot_handle_make(index, gen), user);
            calls++;
        }
    }
    return calls;
}

/* ---- Shutdown ---------------------------------------------------------------------------- */

void de__slots_shutdown(de_asset_system *s) {
    de__slot_table *t = s != NULL ? s->slots : NULL;
    if (t == NULL)
        return;
    for (u32 i = 0; i < t->slot_count; ++i) {
        de__slot *sl = slots_at(t, i);
        if (de_atomic_load_u32(&sl->live_gen, DE_ATOMIC_RELAXED) == 0)
            continue;
        slots_unload(s, sl);
        slots_free_deps(s, sl);
    }
    de_hashmap_free(&t->by_id);
    de_pool_destroy(t->page_pool);
    DE_FREE(s->alloc, t, sizeof *t);
    s->slots = NULL;
}

/* ---- DENG-214: hot reload (version swap, live-slot walk) ---------------------------------- */

void de__slot_swap(de_asset_system *s, u32 handle_id, void *object, u64 bytes, de_asset_id *deps,
                   u32 dep_count, de__slot_swapped *old) {
    de__slot *sl = de__slot_get(s, handle_id);
    memset(old, 0, sizeof *old);
    if (sl == NULL)
        return;
    old->object = de_atomic_exchange_ptr(&sl->object, object, DE_ATOMIC_ACQ_REL);
    old->bytes = sl->bytes;
    old->deps = sl->deps;
    old->dep_count = sl->dep_count;
    sl->bytes = bytes;
    sl->deps = deps;
    sl->dep_count = dep_count;
    sl->error = DE_OK;
    sl->version++; /* generation unchanged: every handle to the slot stays valid */
}

void de__slot_for_each_live(de_asset_system *s,
                            void (*fn)(de_asset_system *s, u32 handle_id, void *user), void *user) {
    de__slot_table *t = s != NULL ? s->slots : NULL;
    if (t == NULL)
        return;
    for (u32 i = 0; i < t->slot_count; ++i) {
        de__slot *sl = slots_at(t, i);
        u32 gen = de_atomic_load_u32(&sl->live_gen, DE_ATOMIC_RELAXED);
        if (gen != 0)
            fn(s, de__slot_handle_make(i, gen), user);
    }
}
