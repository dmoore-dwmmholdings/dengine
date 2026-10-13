/*
 * islands.c: the island hooks of de_physics_step (DENG-218; replaces the DENG-217 stub). See
 * islands.h for the model. This file builds the awake set (wake requests first), and after the
 * gather the islands of this step (union-find over the constraint records); sleep.c owns the
 * timers, putting islands to sleep and the wake API.
 *
 * Step order (step.c, solver.c):
 *   de__islands_update     wake requests (ended contacts, user changes), awake set
 *   de__solver_prepare     gather; when a constraint woke an island: collect + gather again;
 *                          de__islands_build; coloring; prepare
 *   ... substeps, finalize ...
 *   de__islands_finalize   de__sleep_finalize: timers, islands go to sleep
 *
 * Determinism (SPEC §5.7): bodies in ascending index, records in record order; the union-find
 * root is always the lowest slot, so the result does not depend on the union order.
 */
#include "physics/islands.h"

#include "physics/contact/manifold.h"
#include "physics/solver/solver_internal.h"

/* ================================ Layout =============================================== */

#define ISL_ALIGN 16u

static size_t isl_align(size_t x) {
    return (x + (ISL_ALIGN - 1u)) & ~(size_t)(ISL_ALIGN - 1u);
}

typedef struct isl_layout {
    size_t sleeping, sleep_island, timer, rest_position, rest_rotation, wake_mark, wake_list,
        parent, island_of_slot, islands, island_bodies, size;
} isl_layout;

static isl_layout isl_layout_of(u32 cap) {
    const size_t n = cap, slots = (size_t)cap + 1u;
    isl_layout l;
    size_t off = isl_align(sizeof(de__islands));
    l.sleeping = off;
    off = isl_align(off + n);
    l.sleep_island = off;
    off = isl_align(off + sizeof(u32) * n);
    l.timer = off;
    off = isl_align(off + sizeof(f32) * n);
    l.rest_position = off;
    off = isl_align(off + sizeof(de_dvec3) * n);
    l.rest_rotation = off;
    off = isl_align(off + sizeof(de_quat) * n);
    l.wake_mark = off;
    off = isl_align(off + n);
    l.wake_list = off;
    off = isl_align(off + sizeof(u32) * n);
    l.parent = off;
    off = isl_align(off + sizeof(u32) * slots);
    l.island_of_slot = off;
    off = isl_align(off + sizeof(u32) * slots);
    l.islands = off;
    off = isl_align(off + sizeof(de__island) * n);
    l.island_bodies = off;
    off = isl_align(off + sizeof(u32) * n);
    l.size = off;
    return l;
}

size_t de__islands_bytes(u32 capacity) {
    return isl_layout_of(capacity).size;
}

de__islands *de__islands_init(void *mem, u32 capacity) {
    const isl_layout l = isl_layout_of(capacity);
    u8 *m = (u8 *)mem;
    de__islands *is = (de__islands *)mem;
    memset(is, 0, sizeof *is);
    is->capacity = capacity;
    is->sleeping = m + l.sleeping;
    is->sleep_island = (u32 *)(void *)(m + l.sleep_island);
    is->timer = (f32 *)(void *)(m + l.timer);
    is->rest_position = (de_dvec3 *)(void *)(m + l.rest_position);
    is->rest_rotation = (de_quat *)(void *)(m + l.rest_rotation);
    is->wake_mark = m + l.wake_mark;
    is->wake_list = (u32 *)(void *)(m + l.wake_list);
    is->parent = (u32 *)(void *)(m + l.parent);
    is->island_of_slot = (u32 *)(void *)(m + l.island_of_slot);
    is->islands = (de__island *)(void *)(m + l.islands);
    is->island_bodies = (u32 *)(void *)(m + l.island_bodies);
    for (u32 i = 0; i < capacity; ++i)
        is->sleep_island[i] = DE__ISLAND_NONE;
    de__sleep_read_settings(is);
    return is;
}

/* ================================ Awake set ============================================ */

/* A sleeping body that the user changed since it fell asleep: a velocity (it had zero) or another
 * pose (de_body_set_transform). Bit compare, so only a real change wakes it. */
static bool user_changed(const de_physics_world *p, const de__islands *is, u32 i) {
    const de__body_pool *b = &p->bodies;
    const de_vec3 v = b->linear_velocity[i], w = b->angular_velocity[i];
    if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f || w.x != 0.0f || w.y != 0.0f || w.z != 0.0f)
        return true;
    return memcmp(&b->position[i], &is->rest_position[i], sizeof(de_dvec3)) != 0 ||
           memcmp(&b->rotation[i], &is->rest_rotation[i], sizeof(de_quat)) != 0;
}

/* The surviving side of a contact that ended because the other body was destroyed (or its index
 * reused): the end record still names that side's generation. */
static void wake_survivor(de_physics_world *p, de__islands *is, u32 body, u32 gen) {
    if (body < is->capacity && de__body_alive(p, body) && p->bodies.generation[body] == gen &&
        is->sleeping[body])
        de__islands_request_wake(is, body);
}

void de__islands_update(de_physics_world *p, de__solver *s) {
    de__islands *is = s->islands;
    const u32 sleeping_before = is->stats.sleeping_bodies;
    memset(&is->stats, 0, sizeof is->stats);
    is->stats.sleeping_bodies = sleeping_before;
    is->island_count = 0;
    if (sleeping_before != 0u) {
        const de__contact_end *ends = NULL;
        u32 n = 0;
        de__contacts_ended(p, &ends, &n);
        for (u32 k = 0; k < n; ++k) {
            if ((ends[k].flags & DE__CONTACT_END_REMOVED) == 0u)
                continue;
            wake_survivor(p, is, ends[k].key.a, ends[k].gen_a);
            wake_survivor(p, is, ends[k].key.b, ends[k].gen_b);
        }
        const de__body_pool *b = &p->bodies;
        for (u32 i = 0; i < b->high_water; ++i)
            if (is->sleeping[i] && !is->wake_mark[is->sleep_island[i]] && user_changed(p, is, i))
                de__islands_request_wake(is, i);
    }
    de__islands_collect_awake(p, s);
}

void de__islands_request_wake(de__islands *is, u32 body) {
    if (body >= is->capacity || !is->sleeping[body])
        return;
    const u32 id = is->sleep_island[body];
    DE_ASSERT(id < is->capacity);
    if (is->wake_mark[id])
        return;
    is->wake_mark[id] = 1u;
    is->wake_list[is->wake_count++] = id;
}

void de__islands_collect_awake(de_physics_world *p, de__solver *s) {
    de__islands *is = s->islands;
    const de__body_pool *b = &p->bodies;
    if (is->wake_count != 0u) {
        u32 woken = 0;
        for (u32 i = 0; i < b->high_water; ++i) {
            if (!is->sleeping[i] || !is->wake_mark[is->sleep_island[i]])
                continue;
            is->sleeping[i] = 0u;
            is->sleep_island[i] = DE__ISLAND_NONE;
            is->timer[i] = 0.0f;
            ++woken;
        }
        for (u32 k = 0; k < is->wake_count; ++k)
            is->wake_mark[is->wake_list[k]] = 0u;
        is->stats.woken_islands += is->wake_count;
        is->wake_count = 0;
        DE_ASSERT(woken <= is->stats.sleeping_bodies);
        is->stats.sleeping_bodies -= woken;
    }
    for (u32 i = 0; i < b->high_water; ++i)
        if (b->alive[i] && b->type[i] != (u8)DE_BODY_STATIC && !is->sleeping[i])
            (void)de__solver_add_awake(s, i);
}

/* ================================ Islands of this step ================================= */

static u32 uf_find(u32 *parent, u32 x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]]; /* path halving: parents only move toward the root */
        x = parent[x];
    }
    return x;
}

/* The lower root wins, so the root is the lowest slot of the set whatever the order. */
static void uf_union(u32 *parent, u32 a, u32 b) {
    const u32 ra = uf_find(parent, a), rb = uf_find(parent, b);
    if (ra < rb)
        parent[rb] = ra;
    else if (rb < ra)
        parent[ra] = rb;
}

static bool kinematic_moving(const de__solver_body *sb) {
    if ((sb->flags & DE__SOLVER_BODY_KINEMATIC) == 0u)
        return false;
    return sb->v.x != 0.0f || sb->v.y != 0.0f || sb->v.z != 0.0f || sb->w.x != 0.0f ||
           sb->w.y != 0.0f || sb->w.z != 0.0f;
}

void de__islands_build(de__solver *s) {
    de__islands *is = s->islands;
    u32 *parent = is->parent;
    const u32 slots = s->body_count;
    const u32 both = DE__CONSTRAINT_WRITE_A | DE__CONSTRAINT_WRITE_B;
    for (u32 k = 0; k < slots; ++k)
        parent[k] = k;
    for (u32 r = 0; r < s->constraint_count; ++r) {
        const de__constraint *c = &s->constraints[r];
        if ((c->flags & both) == both)
            uf_union(parent, c->solver_a, c->solver_b);
    }

    /* Islands in ascending root slot = ascending lowest body index. */
    u32 count = 0;
    is->island_of_slot[0] = DE__ISLAND_NONE;
    for (u32 k = 1; k < slots; ++k) {
        if ((s->bodies[k].flags & DE__SOLVER_BODY_DYNAMIC) == 0u) {
            is->island_of_slot[k] = DE__ISLAND_NONE;
            continue;
        }
        const u32 root = uf_find(parent, k);
        if (root == k) {
            de__island *isl = &is->islands[count];
            memset(isl, 0, sizeof *isl);
            isl->id = s->awake[k - 1u];
            is->island_of_slot[k] = count++;
        } else {
            is->island_of_slot[k] = is->island_of_slot[root]; /* root < k: already numbered */
        }
        ++is->islands[is->island_of_slot[k]].body_count;
    }
    u32 first = 0, largest = 0;
    for (u32 i = 0; i < count; ++i) {
        is->islands[i].first = first;
        first += is->islands[i].body_count;
        largest = is->islands[i].body_count > largest ? is->islands[i].body_count : largest;
        is->islands[i].body_count = 0; /* refilled below */
    }
    for (u32 k = 1; k < slots; ++k) {
        const u32 isl = is->island_of_slot[k];
        if (isl == DE__ISLAND_NONE)
            continue;
        de__island *I = &is->islands[isl];
        is->island_bodies[I->first + I->body_count++] = s->awake[k - 1u];
    }

    /* Constraints per island; a moving kinematic body keeps the island it touches awake. */
    for (u32 r = 0; r < s->constraint_count; ++r) {
        const de__constraint *c = &s->constraints[r];
        const bool wa = (c->flags & DE__CONSTRAINT_WRITE_A) != 0u;
        const u32 mine = wa ? c->solver_a : c->solver_b;
        const u32 other = wa ? c->solver_b : c->solver_a;
        de__island *I = &is->islands[is->island_of_slot[mine]];
        ++I->constraint_count;
        if ((c->flags & both) != both && kinematic_moving(&s->bodies[other]))
            I->flags |= DE__ISLAND_KEEP_AWAKE;
    }
    is->island_count = count;
    is->stats.islands = count;
    is->stats.largest = largest;
}

/* ================================ Hooks (step.c) ======================================= */

void de__islands_finalize(de_physics_world *p, de__solver *s) {
    de__sleep_finalize(p, s);
}
