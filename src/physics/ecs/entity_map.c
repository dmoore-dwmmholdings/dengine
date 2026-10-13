/*
 * entity_map.c: the ECS state of a physics world (extension slot DE__PHYS_EXT_ECS), the sorted
 * entity map, the creation marks, the graveyard and the lookups (DENG-223). See phys_ecs.h.
 */
#include "physics/ecs/phys_ecs.h"

/* ================================ State ================================================ */

static void pecs_free(de__pecs *s, void *arr, u32 cap, size_t elem) {
    if (arr != NULL)
        DE_FREE(s->alloc, arr, (size_t)cap * elem);
}

static void pecs_destroy(de_physics_world *p, void *state) {
    de__pecs *s = (de__pecs *)state;
    (void)p;
    pecs_free(s, s->map, s->cap, sizeof *s->map);
    pecs_free(s, s->batch, s->batch_cap, sizeof *s->batch);
    pecs_free(s, s->scratch, s->scratch_cap, sizeof *s->scratch);
    pecs_free(s, s->marks, s->mark_words, sizeof *s->marks);
    pecs_free(s, s->marked, s->marked_cap, sizeof *s->marked);
    pecs_free(s, s->poses, s->pose_cap, sizeof *s->poses);
    pecs_free(s, s->targets, s->target_cap, sizeof *s->targets);
    pecs_free(s, s->graves, s->grave_cap, sizeof *s->graves);
    pecs_free(s, s->breaks, s->break_cap, sizeof *s->breaks);
    DE_FREE(s->alloc, s, sizeof *s);
}

/* A body is about to be freed (by physics.step or by anyone else): keep its owner for the END
 * events of the next step. */
static void pecs_body_removed(de_physics_world *p, void *state, u32 index) {
    de__pecs *s = (de__pecs *)state;
    const de_entity owner = p->bodies.owner[index];
    if (owner.bits == 0)
        return;
    /* At most one grave per body slot and step is needed in practice; the cap bounds a world that
     * destroys bodies without ever running physics.step. */
    if (s->grave_count >= p->max_bodies || !de__pecs_grow(s, (void **)&s->graves, &s->grave_cap,
                                                          s->grave_count + 1u, sizeof *s->graves)) {
        if (!s->grave_overflow_logged) {
            DE_WARN(de__physics_log(), "physics ECS: graveyard full; END events of destroyed "
                                       "bodies may name no entity");
            s->grave_overflow_logged = true;
        }
        de_error_clear();
        return;
    }
    de__pecs_grave *g = &s->graves[s->grave_count++];
    g->index = index;
    g->generation = p->bodies.generation[index];
    g->owner = owner;
}

static const de__phys_ext_vtbl k_pecs_vtbl = {pecs_destroy, NULL, pecs_body_removed};

de_result de__pecs_create(de_physics_world *p, de_world *w, de__pecs **out) {
    de__pecs *s = DE_NEW(p->alloc, de__pecs, DE_MEM_PHYSICS);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics_attach: ECS state");
    memset(s, 0, sizeof *s);
    s->alloc = p->alloc;
    s->p = p;
    s->w = w;
    de__phys_ext_set(p, DE__PHYS_EXT_ECS, s, &k_pecs_vtbl);
    *out = s;
    return DE_OK;
}

bool de__pecs_grow(de__pecs *s, void **arr, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap)
        return true;
    u32 n = *cap > 0 ? *cap : 16u;
    while (n < need)
        n = n > 0x7FFFFFFFu ? need : n * 2u;
    void *a = DE_ALLOC(s->alloc, (size_t)n * elem, 16, DE_MEM_PHYSICS);
    if (a == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "physics ECS: growing an array to %u entries", n);
        return false;
    }
    if (*arr != NULL) {
        memcpy(a, *arr, (size_t)*cap * elem);
        DE_FREE(s->alloc, *arr, (size_t)*cap * elem);
    }
    *arr = a;
    *cap = n;
    return true;
}

/* ================================ Map ================================================== */

/* Position of the first entry whose entity index is >= `index`. */
static u32 lower_bound(const de__pecs *s, u32 index) {
    u32 lo = 0, hi = s->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (de_entity_index(s->map[mid].entity) < index)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

de__pecs_entry *de__pecs_find(de__pecs *s, de_entity e) {
    if (s == NULL || e.bits == 0)
        return NULL;
    const u32 i = lower_bound(s, de_entity_index(e));
    if (i < s->count && s->map[i].entity.bits == e.bits)
        return &s->map[i];
    return NULL;
}

bool de__pecs_mark(de__pecs *s, de_entity e) {
    const u32 index = de_entity_index(e);
    const u32 word = index / 64u;
    if (word >= s->mark_words) {
        const u32 old = s->mark_words;
        if (!de__pecs_grow(s, (void **)&s->marks, &s->mark_words, word + 1u, sizeof *s->marks)) {
            de_error_clear();
            return false;
        }
        memset(s->marks + old, 0, (size_t)(s->mark_words - old) * sizeof *s->marks);
    }
    const u64 bit = 1ull << (index % 64u);
    if (s->marks[word] & bit)
        return false;
    if (!de__pecs_grow(s, (void **)&s->marked, &s->marked_cap, s->marked_count + 1u,
                       sizeof *s->marked)) {
        de_error_clear();
        return false;
    }
    s->marks[word] |= bit;
    s->marked[s->marked_count++] = index;
    return true;
}

bool de__pecs_marked(const de__pecs *s, de_entity e) {
    const u32 index = de_entity_index(e);
    const u32 word = index / 64u;
    return word < s->mark_words && (s->marks[word] & (1ull << (index % 64u))) != 0;
}

void de__pecs_clear_marks(de__pecs *s) {
    for (u32 i = 0; i < s->marked_count; ++i)
        s->marks[s->marked[i] / 64u] &= ~(1ull << (s->marked[i] % 64u));
    s->marked_count = 0;
}

bool de__pecs_batch_add(de__pecs *s, const de__pecs_entry *en) {
    if (!de__pecs_grow(s, (void **)&s->batch, &s->batch_cap, s->batch_count + 1u, sizeof *s->batch))
        return false;
    s->batch[s->batch_count++] = *en;
    return true;
}

void de__pecs_batch_merge(de__pecs *s) {
    const u32 nb = s->batch_count;
    if (nb == 0)
        return;
    /* Insertion sort by entity index: batches are small, and the keys are unique (one entry per
     * live entity index), so the order does not depend on the sort. */
    for (u32 i = 1; i < nb; ++i) {
        const de__pecs_entry x = s->batch[i];
        u32 j = i;
        while (j > 0 && de_entity_index(s->batch[j - 1u].entity) > de_entity_index(x.entity)) {
            s->batch[j] = s->batch[j - 1u];
            --j;
        }
        s->batch[j] = x;
    }
    const u32 total = s->count + nb;
    /* Room for the merged map and the merge buffer. A failure here would lose bodies from the
     * map, so the batch entries' bodies are destroyed instead (they are re-created next tick). */
    if (!de__pecs_grow(s, (void **)&s->map, &s->cap, total, sizeof *s->map) ||
        !de__pecs_grow(s, (void **)&s->scratch, &s->scratch_cap, total, sizeof *s->scratch)) {
        DE_WARN(de__physics_log(),
                "physics ECS: out of memory adding %u bodies to the entity "
                "map; retried next tick",
                nb);
        de_error_clear();
        for (u32 i = 0; i < nb; ++i)
            de__pecs_destroy_body(s, &s->batch[i]);
        s->batch_count = 0;
        s->rescan = true;
        return;
    }
    u32 i = 0, j = 0, k = 0;
    while (i < s->count || j < nb) {
        if (j == nb || (i < s->count &&
                        de_entity_index(s->map[i].entity) < de_entity_index(s->batch[j].entity)))
            s->scratch[k++] = s->map[i++];
        else
            s->scratch[k++] = s->batch[j++];
    }
    memcpy(s->map, s->scratch, (size_t)total * sizeof *s->map);
    s->count = total;
    s->batch_count = 0;
}

/* ================================ Owners =============================================== */

de_entity de__pecs_owner(const de__pecs *s, u32 index, u32 generation) {
    const de_physics_world *p = s->p;
    if (de__body_alive(p, index) && p->bodies.generation[index] == generation)
        return p->bodies.owner[index];
    for (u32 i = s->grave_count; i-- > 0;)
        if (s->graves[i].index == index && s->graves[i].generation == generation)
            return s->graves[i].owner;
    return DE_ENTITY_NULL;
}

void de__pecs_graves_clear(de__pecs *s) {
    s->grave_count = 0;
}

bool de__pecs_poses_ensure(de__pecs *s) {
    const de__body_pool *b = &s->p->bodies;
    const u32 need = b->high_water;
    const u32 old = s->pose_cap;
    if (need <= old)
        return true;
    if (!de__pecs_grow(s, (void **)&s->poses, &s->pose_cap, need, sizeof *s->poses))
        return false;
    /* Fresh slots hold the poses of now (never interpolate from garbage). */
    for (u32 i = old; i < s->pose_cap; ++i) {
        de__pecs_pose q;
        memset(&q, 0, sizeof q);
        q.rotation.w = 1.0f;
        if (i < b->high_water && b->alive[i]) {
            q.position = b->position[i];
            q.rotation = b->rotation[i];
        }
        s->poses[i].prev = q;
        s->poses[i].curr = q;
    }
    return true;
}

/* ================================ Lookups ============================================== */

bool de__phys_ecs_entity_body(const de_physics_world *p, de_entity e, u32 *index) {
    de__pecs *s = de__pecs_get(p);
    if (s == NULL)
        return false;
    const de__pecs_entry *en = de__pecs_find(s, e);
    return en != NULL && de__body_resolve(p, en->body, index);
}

de_body de_physics_entity_body(const de_physics_world *p, de_entity e) {
    u32 index;
    if (!de__phys_ecs_entity_body(p, e, &index))
        return (de_body){0};
    return de__body_handle(p, index);
}

de_event_type de_physics_contact_event_type(const de_physics_world *p) {
    const de__pecs *s = de__pecs_get(p);
    return s != NULL ? s->ev_contact : (de_event_type){0};
}

de_event_type de_physics_joint_break_event_type(const de_physics_world *p) {
    const de__pecs *s = de__pecs_get(p);
    return s != NULL ? s->ev_break : (de_event_type){0};
}
