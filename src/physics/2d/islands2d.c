/*
 * islands2d.c: the awake set, wake requests and the islands of each step of the 2D solver
 * (DENG-254; the 2D port of physics/islands.c, DENG-218 and DENG-222). sleep2d.c owns the sleep
 * timers, putting islands to sleep and the wake API.
 *
 * Islands. Rebuilt every step after the gather (de__islands2d_build): a union-find over the
 * solver slots of the awake bodies, visiting the constraint records in record order (contacts in
 * sorted manifold order, joints in stable id order). Only records whose two sides are writable
 * (awake dynamic bodies) link: static, kinematic and sleeping bodies and the world never join an
 * island. The lower root always wins, so the root is the lowest slot and the island id is the
 * lowest body index of the island, whatever the union order. Islands are numbered in ascending
 * id and list their bodies in ascending index. Each island's solve origin is the f64 position of
 * its lowest body (contact and joint anchors are formed relative to it).
 *
 * Waking. A sleeping body wakes with its whole island (same sleep island id) at the start of the
 * next step when:
 *   - a gathered constraint links it to an awake dynamic body or a moving kinematic body (a
 *     contact that begins, a joint): de__solver2d_add_constraint requests it and the solver
 *     gathers again;
 *   - the user changed it: a non-zero velocity (de_body2d_set_velocity) or a pose that differs bit
 *     for bit from the one it fell asleep with (de_body2d_set_transform);
 *   - de_body2d_wake, or de__islands2d_wake_body (joint create and destroy, DENG-256);
 *   - a body it touched is destroyed (contact end with DE__CONTACT2D_END_REMOVED), or a body of
 *     its island is destroyed (the solver slot's body_removed).
 *
 * Determinism (SPEC §5.7): bodies in ascending index, records in record order.
 */
#include "physics/2d/manifold2d.h"
#include "physics/2d/solver2d_internal.h"

/* ================================ Layout =============================================== */

#define ISL2_ALIGN 16u

static size_t isl2_align(size_t x) {
    return (x + (ISL2_ALIGN - 1u)) & ~(size_t)(ISL2_ALIGN - 1u);
}

typedef struct isl2_layout {
    size_t sleeping, sleep_island, timer, rest_position, rest_rotation, wake_mark, wake_list,
        parent, island_of_slot, islands, island_bodies, size;
} isl2_layout;

static isl2_layout layout_of(u32 cap) {
    const size_t n = cap, slots = (size_t)cap + 1u;
    isl2_layout l;
    size_t off = isl2_align(sizeof(de__islands2d));
    l.sleeping = off;
    off = isl2_align(off + n);
    l.sleep_island = off;
    off = isl2_align(off + sizeof(u32) * n);
    l.timer = off;
    off = isl2_align(off + sizeof(f32) * n);
    l.rest_position = off;
    off = isl2_align(off + sizeof(de__d2) * n);
    l.rest_rotation = off;
    off = isl2_align(off + sizeof(de_rot2d) * n);
    l.wake_mark = off;
    off = isl2_align(off + n);
    l.wake_list = off;
    off = isl2_align(off + sizeof(u32) * n);
    l.parent = off;
    off = isl2_align(off + sizeof(u32) * slots);
    l.island_of_slot = off;
    off = isl2_align(off + sizeof(u32) * slots);
    l.islands = off;
    off = isl2_align(off + sizeof(de__island2d) * n);
    l.island_bodies = off;
    off = isl2_align(off + sizeof(u32) * n);
    l.size = off;
    return l;
}

size_t de__islands2d_bytes(u32 capacity) {
    return layout_of(capacity).size;
}

de__islands2d *de__islands2d_init(void *mem, u32 capacity) {
    const isl2_layout l = layout_of(capacity);
    u8 *m = (u8 *)mem;
    de__islands2d *is = (de__islands2d *)mem;
    memset(is, 0, sizeof *is);
    is->capacity = capacity;
    is->sleeping = m + l.sleeping;
    is->sleep_island = (u32 *)(void *)(m + l.sleep_island);
    is->timer = (f32 *)(void *)(m + l.timer);
    is->rest_position = (de__d2 *)(void *)(m + l.rest_position);
    is->rest_rotation = (de_rot2d *)(void *)(m + l.rest_rotation);
    is->wake_mark = m + l.wake_mark;
    is->wake_list = (u32 *)(void *)(m + l.wake_list);
    is->parent = (u32 *)(void *)(m + l.parent);
    is->island_of_slot = (u32 *)(void *)(m + l.island_of_slot);
    is->islands = (de__island2d *)(void *)(m + l.islands);
    is->island_bodies = (u32 *)(void *)(m + l.island_bodies);
    for (u32 i = 0; i < capacity; ++i)
        is->sleep_island[i] = DE__ISLAND2D_NONE;
    de__sleep2d_read_settings(is);
    return is;
}

/* ================================ Awake set ============================================ */

/* A sleeping body that the user changed since it fell asleep: a velocity (it had zero) or another
 * pose. Bit compare, so only a real change wakes it. */
static bool user_changed(const de_physics2d_world *p, const de__islands2d *is, u32 i) {
    const de__body2d_pool *b = &p->bodies;
    const de_vec2 v = b->linear_velocity[i];
    if (v.x != 0.0f || v.y != 0.0f || b->angular_velocity[i] != 0.0f)
        return true;
    return memcmp(&b->position[i], &is->rest_position[i], sizeof(de__d2)) != 0 ||
           memcmp(&b->rotation[i], &is->rest_rotation[i], sizeof(de_rot2d)) != 0;
}

/* The surviving side of a contact that ended because the other body was destroyed (or its index
 * reused): the end record still names that side's generation. */
static void wake_survivor(de_physics2d_world *p, de__islands2d *is, u32 body, u32 gen) {
    if (body < is->capacity && de__body2d_alive(p, body) && p->bodies.generation[body] == gen &&
        is->sleeping[body])
        de__islands2d_request_wake(is, body);
}

void de__islands2d_update(de_physics2d_world *p, de__solver2d *s) {
    de__islands2d *is = s->islands;
    const u32 sleeping_before = is->stats.sleeping_bodies;
    memset(&is->stats, 0, sizeof is->stats);
    is->stats.sleeping_bodies = sleeping_before;
    is->island_count = 0;
    if (sleeping_before != 0u) {
        const de__contact_end2d *ends = NULL;
        u32 n = 0;
        de__contacts2d_ended(p, &ends, &n);
        for (u32 k = 0; k < n; ++k) {
            if ((ends[k].flags & DE__CONTACT2D_END_REMOVED) == 0u)
                continue;
            wake_survivor(p, is, ends[k].key.a, ends[k].gen_a);
            wake_survivor(p, is, ends[k].key.b, ends[k].gen_b);
        }
        const de__body2d_pool *b = &p->bodies;
        for (u32 i = 0; i < b->high_water; ++i)
            if (is->sleeping[i] && !is->wake_mark[is->sleep_island[i]] && user_changed(p, is, i))
                de__islands2d_request_wake(is, i);
    }
    de__islands2d_collect_awake(p, s);
}

void de__islands2d_request_wake(de__islands2d *is, u32 body) {
    if (body >= is->capacity || !is->sleeping[body])
        return;
    const u32 id = is->sleep_island[body];
    DE_ASSERT(id < is->capacity);
    if (is->wake_mark[id])
        return;
    is->wake_mark[id] = 1u;
    is->wake_list[is->wake_count++] = id;
}

void de__islands2d_collect_awake(de_physics2d_world *p, de__solver2d *s) {
    de__islands2d *is = s->islands;
    const de__body2d_pool *b = &p->bodies;
    if (is->wake_count != 0u) {
        u32 woken = 0;
        for (u32 i = 0; i < b->high_water; ++i) {
            if (!is->sleeping[i] || !is->wake_mark[is->sleep_island[i]])
                continue;
            is->sleeping[i] = 0u;
            is->sleep_island[i] = DE__ISLAND2D_NONE;
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
            (void)de__solver2d_add_awake(s, i);
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

static bool kinematic_moving(const de__solver_body2d *sb) {
    if ((sb->flags & DE__SOLVER2D_BODY_KINEMATIC) == 0u)
        return false;
    return sb->v.x != 0.0f || sb->v.y != 0.0f || sb->w != 0.0f;
}

void de__islands2d_build(de__solver2d *s) {
    de__islands2d *is = s->islands;
    u32 *parent = is->parent;
    const u32 slots = s->body_count;
    const u32 both = DE__CONSTRAINT2D_WRITE_A | DE__CONSTRAINT2D_WRITE_B;
    for (u32 k = 0; k < slots; ++k)
        parent[k] = k;
    for (u32 r = 0; r < s->constraint_count; ++r) {
        const de__constraint2d *c = &s->constraints[r];
        if ((c->flags & both) == both)
            uf_union(parent, c->solver_a, c->solver_b);
    }

    /* Islands in ascending root slot = ascending lowest body index. */
    u32 count = 0;
    is->island_of_slot[0] = DE__ISLAND2D_NONE;
    for (u32 k = 1; k < slots; ++k) {
        if ((s->bodies[k].flags & DE__SOLVER2D_BODY_DYNAMIC) == 0u) {
            is->island_of_slot[k] = DE__ISLAND2D_NONE;
            continue;
        }
        const u32 root = uf_find(parent, k);
        if (root == k) {
            de__island2d *isl = &is->islands[count];
            memset(isl, 0, sizeof *isl);
            isl->id = s->awake[k - 1u];
            isl->origin = s->p->bodies.position[isl->id];
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
        if (isl == DE__ISLAND2D_NONE)
            continue;
        de__island2d *I = &is->islands[isl];
        is->island_bodies[I->first + I->body_count++] = s->awake[k - 1u];
    }

    /* Constraints per island; a moving kinematic body keeps the island it touches awake. */
    for (u32 r = 0; r < s->constraint_count; ++r) {
        const de__constraint2d *c = &s->constraints[r];
        const bool wa = (c->flags & DE__CONSTRAINT2D_WRITE_A) != 0u;
        const u32 mine = wa ? c->solver_a : c->solver_b;
        const u32 other = wa ? c->solver_b : c->solver_a;
        de__island2d *I = &is->islands[is->island_of_slot[mine]];
        ++I->constraint_count;
        if ((c->flags & both) != both && kinematic_moving(&s->bodies[other]))
            I->flags |= DE__ISLAND2D_KEEP_AWAKE;
    }
    is->island_count = count;
    is->stats.islands = count;
    is->stats.largest = largest;
}
