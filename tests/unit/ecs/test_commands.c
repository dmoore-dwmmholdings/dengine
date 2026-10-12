/* test_commands.c: command buffers, pending entity ids and deterministic apply (DENG-349). */
#include "ecs_test_util.h"

#include "ecs/commands_internal.h"

#include <dengine/de_platform.h>

/* ---- Extra components and events -------------------------------------------------------------
 */

/* Entity references in every place reflection allows: a field, a fixed array, a nested struct
 * and an array of nested structs. */
typedef struct TInner {
    de_entity e;
    u32 tag;
    u32 pad_;
} TInner;
typedef struct TLink {
    de_entity target;
    de_entity arr[3];
    TInner inner;
    TInner inners[2];
    u32 n;
    u32 pad_;
} TLink;

/* Counting hooks: every constructed value is destructed exactly once. */
typedef struct TCounted {
    u32 value;
} TCounted;
static int g_construct, g_destruct;
static void counted_construct(void *o) {
    ((TCounted *)o)->value = 0x5EEDu;
    ++g_construct;
}
static void counted_destruct(void *o) {
    (void)o;
    ++g_destruct;
}
static void counted_copy(void *dst, const void *src) {
    memcpy(dst, src, sizeof(TCounted));
}

/* Events: a hit with an entity reference, a hooked payload, and one larger than a page. */
typedef struct EHit {
    de_entity who;
    u32 seq;
    u32 src;
} EHit;
typedef struct EHooked {
    u32 value;
} EHooked;
static int g_ev_copy, g_ev_destruct;
static void ehooked_copy(void *dst, const void *src) {
    ((EHooked *)dst)->value = ((const EHooked *)src)->value;
    ++g_ev_copy;
}
static void ehooked_destruct(void *o) {
    ((EHooked *)o)->value = 0xDEADu;
    ++g_ev_destruct;
}
#define EBIG_BYTES 40000u
typedef struct EBig {
    u8 data[EBIG_BYTES];
} EBig;

typedef struct cmd_types {
    de_type_id inner, link, counted, hit, hooked, big;
} cmd_types;

static cmd_types g_types;

static void register_cmd_types(ecs_fx *f) {
    static const de_field_info k_inner[] = {
        DE_FIELD(TInner, e, DE_FK_ENTITY, DE_FIELD_DEFAULT),
        DE_FIELD(TInner, tag, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_link[] = {
        DE_FIELD(TLink, target, DE_FK_ENTITY, DE_FIELD_DEFAULT),
        DE_FIELD_EX(TLink, arr, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "entity", 3}),
        DE_FIELD_EX(TLink, inner, DE_FK_STRUCT, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TInner", 0}),
        DE_FIELD_EX(TLink, inners, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TInner", 2}),
        DE_FIELD(TLink, n, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_counted[] = {
        DE_FIELD(TCounted, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    static const de_field_info k_hit[] = {
        DE_FIELD(EHit, who, DE_FK_ENTITY, DE_FIELD_DEFAULT),
        DE_FIELD(EHit, seq, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(EHit, src, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_hooked[] = {DE_FIELD(EHooked, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    static const de_field_info k_big[] = {
        DE_FIELD_EX(EBig, data, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, 0, EBIG_BYTES})};
    if (DE_HANDLE_VALID(de_type_find(f->types, "TInner"))) {
        return; /* registered by an earlier test of this process */
    }
    g_types.inner = de_type_register(f->types, &DE_TYPE_DESC(TInner, 1, k_inner));
    g_types.link = de_type_register(f->types, &DE_TYPE_DESC(TLink, 1, k_link));
    de_type_desc d = DE_TYPE_DESC(TCounted, 1, k_counted);
    d.is_pod = false;
    d.hooks.construct = counted_construct;
    d.hooks.destruct = counted_destruct;
    d.hooks.copy = counted_copy;
    g_types.counted = de_type_register(f->types, &d);
    g_types.hit = de_type_register(f->types, &DE_TYPE_DESC(EHit, 1, k_hit));
    de_type_desc h = DE_TYPE_DESC(EHooked, 1, k_hooked);
    h.is_pod = false;
    h.hooks.copy = ehooked_copy;
    h.hooks.destruct = ehooked_destruct;
    g_types.hooked = de_type_register(f->types, &h);
    g_types.big = de_type_register(f->types, &DE_TYPE_DESC(EBig, 1, k_big));
    ASSERT_TRUE(DE_HANDLE_VALID(g_types.inner) && DE_HANDLE_VALID(g_types.link) &&
                DE_HANDLE_VALID(g_types.counted) && DE_HANDLE_VALID(g_types.hit) &&
                DE_HANDLE_VALID(g_types.hooked) && DE_HANDLE_VALID(g_types.big));
}

/* The fixture's world plus TLink (id 6), TCounted (id 7) and the three event types. */
typedef struct cmd_world {
    de_component link, counted;
    de_event_type hit, hooked, big;
} cmd_world;

static cmd_world cmd_setup(ecs_fx *f) {
    register_cmd_types(f);
    cmd_world cw;
    cw.link = de_component_register(f->w, g_types.link, DE_COMP_SNAPSHOT);
    cw.counted = de_component_register(f->w, g_types.counted, 0);
    cw.hit = de_event_register(f->w, g_types.hit);
    cw.hooked = de_event_register(f->w, g_types.hooked);
    cw.big = de_event_register(f->w, g_types.big);
    EXPECT_EQ(cw.link.id, 6u);
    EXPECT_EQ(cw.counted.id, 7u);
    EXPECT_TRUE(DE_HANDLE_VALID(cw.hit) && DE_HANDLE_VALID(cw.hooked) && DE_HANDLE_VALID(cw.big));
    g_construct = g_destruct = g_ev_copy = g_ev_destruct = 0;
    return cw;
}

/* ---- Structural dump --------------------------------------------------------------------------
 */

static u64 fnv1a(u64 h, u64 v) {
    for (u32 i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8u)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}
static u64 fnv1a_bytes(u64 h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}
#define FNV_SEED 0xCBF29CE484222325ull

/* A checksum of everything structural: archetypes (creation order, sets), every row (entity id
 * and component bytes, in archetype, chunk, row order), every entity record and the free list. */
static u64 world_dump(de_world *w) {
    u64 h = FNV_SEED;
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        const ecs_archetype *a = ecs_archetype_at(w, ai);
        h = fnv1a(h, ((u64)ai << 32) | a->count);
        for (u32 j = 0; j < a->component_count; ++j)
            h = fnv1a(h, a->components[j]);
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            for (u32 r = 0; r < ch->count; ++r) {
                h = fnv1a(h, ecs_chunk_entities(ch)[r].bits);
                for (u32 col = 0; col < a->column_count; ++col)
                    h = fnv1a_bytes(h, ecs_row_column(a, c, r, col), a->columns[col].size);
            }
        }
    }
    for (u32 i = 0; i < w->records.count; ++i) {
        const ecs_record *rec = (const ecs_record *)de_array_at(&w->records, i);
        h = fnv1a(h, ((u64)rec->archetype << 32) | rec->generation);
        h = fnv1a(h, ((u64)rec->chunk << 32) | rec->row);
    }
    for (u32 i = 0; i < w->free_indices.count; ++i)
        h = fnv1a(h, DE_ARRAY_AT(&w->free_indices, u32, i));
    return h;
}

/* Every alive entity id, in archetype, chunk and row order, folded into a checksum. */
static u64 entity_ids_hash(de_world *w, u32 *out_count) {
    u64 h = FNV_SEED;
    u32 n = 0;
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        const ecs_archetype *a = ecs_archetype_at(w, ai);
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            for (u32 r = 0; r < ch->count; ++r, ++n)
                h = fnv1a(h, ecs_chunk_entities(ch)[r].bits);
        }
    }
    if (out_count)
        *out_count = n;
    return h;
}

static u32 alive_count(de_world *w) {
    u32 n = 0;
    (void)entity_ids_hash(w, &n);
    return n;
}

/* ---- cmd.equivalent_to_immediate --------------------------------------------------------------
 */

/* A scripted program over entity slots. Ops on slot s: spawn into s, add/remove a component,
 * despawn. Run immediately on one world and as commands on another. */
typedef enum prog_op {
    P_SPAWN,
    P_ADD_POS,
    P_ADD_VEL,
    P_ADD_ENEMY,
    P_ADD_HEALTH,
    P_REMOVE,
    P_DESPAWN
} prog_op;
typedef struct prog_step {
    prog_op op;
    u32 slot;
    u32 arg;
} prog_step;

#define PROG_SLOTS 64u
#define PROG_STEPS 3000u

static void make_program(prog_step *steps) {
    u32 rng = 0x9E3779B9u;
    bool live[PROG_SLOTS] = {false};
    /* Slots 0..15 hold entities spawned before the program; spawned slots are only despawned if
     * they existed before the program, so no pending id is despawned (that cancels; see
     * cmd.despawn_pending_cancels). */
    bool pre[PROG_SLOTS] = {false};
    for (u32 s = 0; s < 16u; ++s)
        live[s] = pre[s] = true;
    for (u32 k = 0; k < PROG_STEPS; ++k) {
        rng = rng * 1664525u + 1013904223u;
        const u32 slot = (rng >> 8) % PROG_SLOTS;
        prog_step st = {P_SPAWN, slot, rng >> 16};
        if (!live[slot]) {
            st.op = P_SPAWN;
            live[slot] = true;
            pre[slot] = false;
        } else {
            const u32 r = (rng >> 24) % 12u;
            if (r == 0 && pre[slot]) {
                st.op = P_DESPAWN;
                live[slot] = false;
            } else if (r < 4) {
                st.op = P_ADD_POS;
            } else if (r < 6) {
                st.op = P_ADD_VEL;
            } else if (r < 7) {
                st.op = P_ADD_ENEMY;
            } else if (r < 9) {
                st.op = P_ADD_HEALTH;
            } else {
                st.op = P_REMOVE;
            }
        }
        steps[k] = st;
    }
}

static de_component prog_remove_comp(ecs_fx *f, u32 arg) {
    const de_component c[] = {f->pos, f->vel, f->enemy, f->health};
    return c[arg % 4u];
}

static void run_immediate(ecs_fx *f, const prog_step *steps, de_entity *slots) {
    for (u32 k = 0; k < PROG_STEPS; ++k) {
        const prog_step *s = &steps[k];
        de_entity *e = &slots[s->slot];
        switch (s->op) {
        case P_SPAWN:
            *e = de_entity_spawn(f->w);
            break;
        case P_ADD_POS:
            ((TPos *)de_entity_add(f->w, *e, f->pos))->v = (de_vec3){(f32)s->arg, 1.0f, 2.0f};
            break;
        case P_ADD_VEL:
            ((TVel *)de_entity_add(f->w, *e, f->vel))->v = (de_vec3){3.0f, (f32)s->arg, 4.0f};
            break;
        case P_ADD_ENEMY:
            ASSERT_TRUE(de_entity_add(f->w, *e, f->enemy) != NULL);
            break;
        case P_ADD_HEALTH:
            *(THealth *)de_entity_add(f->w, *e, f->health) = (THealth){(f32)s->arg, 100.0f};
            break;
        case P_REMOVE:
            de_entity_remove(f->w, *e, prog_remove_comp(f, s->arg));
            break;
        case P_DESPAWN:
            de_entity_despawn(f->w, *e);
            break;
        }
    }
}

typedef struct deferred_payload {
    void *ptr;
    u32 step;
} deferred_payload;

/* Records the program as commands: steps [0, t1) into the main buffer, [t1, t2) into (9, 0) and
 * the rest into (9, 1), so the apply order (main, then by key) is the program order. (9, 1) is
 * acquired first: the key decides the order, not the acquire. Payloads are written only after
 * everything is recorded, through the pointers de_cmd_add returned, which must survive later
 * records and page changes. */
static void run_deferred(ecs_fx *f, const prog_step *steps, de_entity *slots) {
    static deferred_payload payloads[PROG_STEPS];
    const u32 t1 = PROG_STEPS / 3u, t2 = 2u * PROG_STEPS / 3u;
    u32 np = 0;
    de_commands *b2 = ecs_cmd_buffer_acquire(f->w, 9, 1);
    de_commands *b1 = ecs_cmd_buffer_acquire(f->w, 9, 0);
    de_commands *bm = de_world_commands(f->w);
    ASSERT_TRUE(bm != NULL && b1 != NULL && b2 != NULL);
    EXPECT_TRUE(ecs_cmd_buffer_acquire(f->w, 9, 0) == b1); /* same key, same buffer */
    EXPECT_TRUE(b1 != b2 && b1 != bm);
    for (u32 k = 0; k < PROG_STEPS; ++k) {
        de_commands *c = k < t1 ? bm : k < t2 ? b1 : b2;
        const prog_step *s = &steps[k];
        de_entity *e = &slots[s->slot];
        switch (s->op) {
        case P_SPAWN:
            *e = de_cmd_spawn(c);
            ASSERT_TRUE(de_entity_is_pending(*e));
            EXPECT_FALSE(de_entity_alive(f->w, *e));
            break;
        case P_ADD_POS:
        case P_ADD_VEL:
        case P_ADD_HEALTH: {
            const de_component comp = s->op == P_ADD_POS   ? f->pos
                                      : s->op == P_ADD_VEL ? f->vel
                                                           : f->health;
            payloads[np++] = (deferred_payload){de_cmd_add(c, *e, comp), k};
            break;
        }
        case P_ADD_ENEMY:
            ASSERT_TRUE(de_cmd_add(c, *e, f->enemy) != NULL);
            break;
        case P_REMOVE:
            de_cmd_remove(c, *e, prog_remove_comp(f, s->arg));
            break;
        case P_DESPAWN:
            de_cmd_despawn(c, *e);
            break;
        }
    }
    for (u32 i = 0; i < np; ++i) {
        const prog_step *s = &steps[payloads[i].step];
        ASSERT_TRUE(payloads[i].ptr != NULL);
        if (s->op == P_ADD_POS)
            ((TPos *)payloads[i].ptr)->v = (de_vec3){(f32)s->arg, 1.0f, 2.0f};
        else if (s->op == P_ADD_VEL)
            ((TVel *)payloads[i].ptr)->v = (de_vec3){3.0f, (f32)s->arg, 4.0f};
        else
            *(THealth *)payloads[i].ptr = (THealth){(f32)s->arg, 100.0f};
    }
    EXPECT_GT(np, 1000u); /* enough payloads to span several pages */
    EXPECT_EQ(de_commands_count(bm) + de_commands_count(b1) + de_commands_count(b2), PROG_STEPS);
    de_world_apply_commands(f->w);
    EXPECT_EQ(de_commands_count(bm) + de_commands_count(b1) + de_commands_count(b2), 0u);
}

TEST_F(cmd, equivalent_to_immediate, ecs_fx) {
    static prog_step steps[PROG_STEPS];
    make_program(steps);
    de_entity slots_a[PROG_SLOTS], slots_b[PROG_SLOTS];

    for (u32 s = 0; s < PROG_SLOTS; ++s)
        slots_a[s] = s < 16u ? de_entity_spawn(f->w) : DE_ENTITY_NULL;
    run_immediate(f, steps, slots_a);
    const u64 immediate = world_dump(f->w);

    ecs_fx_destroy_world(f);
    ecs_fx_create_world(f, NULL);
    for (u32 s = 0; s < PROG_SLOTS; ++s)
        slots_b[s] = s < 16u ? de_entity_spawn(f->w) : DE_ENTITY_NULL;
    run_deferred(f, steps, slots_b);
    EXPECT_EQ(world_dump(f->w), immediate);
    /* Every slot's pending id resolves to the entity the immediate run got. */
    for (u32 s = 0; s < PROG_SLOTS; ++s)
        EXPECT_EQ(de_commands_resolve(f->w, slots_b[s]).bits, slots_a[s].bits);
}

/* ---- cmd.apply_order_deterministic ------------------------------------------------------------
 */

#define DET_RANGES  64u
#define DET_PRE     256u
#define DET_ROUNDS  3u
#define DET_THREADS 8u

/* Golden values of cmd.apply_order_deterministic (integer-only checksums; see world_dump). */
#define DET_ALIVE_GOLDEN  1034u
#define DET_IDS_GOLDEN    0x993C3A4F328B002Full
#define DET_DUMP_GOLDEN   0xD3C79F60257B4116ull
#define DET_EVENTS_GOLDEN 0xBC89AEECD250158Bull

typedef struct det_ctx {
    ecs_fx *f;
    const cmd_world *cw;
    const de_entity *pre;
    u32 round;
} det_ctx;

static u32 det_hash3(u32 a, u32 b, u32 c) {
    u32 h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

/* What system `order` records for query range `range` in round `round`. Depends only on these
 * three numbers and on `pre`, never on the thread. */
static void det_record(const det_ctx *d, u32 order, u32 range) {
    ecs_fx *f = d->f;
    de_commands *c = ecs_cmd_buffer_acquire(f->w, order, range);
    if (c == NULL)
        return; /* the checksum then differs */
    const u32 rng = det_hash3(d->round, order, range);
    const u32 spawns = 1u + rng % 4u;
    de_entity prev = DE_ENTITY_NULL;
    for (u32 s = 0; s < spawns; ++s) {
        const de_entity p = de_cmd_spawn(c);
        TPos *pos = (TPos *)de_cmd_add(c, p, f->pos);
        if (pos != NULL)
            pos->v = (de_vec3){(f32)range, (f32)s, (f32)d->round};
        if ((rng >> (4u + s)) & 1u) {
            TVel *v = (TVel *)de_cmd_add(c, p, f->vel);
            if (v != NULL)
                v->v = (de_vec3){(f32)order, 0.0f, 1.0f};
        }
        if (range % 3u == 0)
            (void)de_cmd_add(c, p, f->enemy);
        if (prev.bits != 0) {
            TLink *l = (TLink *)de_cmd_add(c, p, d->cw->link);
            if (l != NULL) {
                l->target = prev;
                l->arr[0] = d->pre[(range * 4u + s) % DET_PRE];
                l->n = s;
            }
        }
        prev = p;
    }
    /* An existing entity: several (order, range, round) triples name the same one, so some of
     * these hit entities that an earlier command despawned and are dropped. */
    const de_entity own = d->pre[(range * 4u + (order - 1u) * 2u + d->round) % DET_PRE];
    switch ((rng >> 12) % 4u) {
    case 0:
        de_cmd_despawn(c, own);
        break;
    case 1: {
        THealth *h = (THealth *)de_cmd_add(c, own, f->health);
        if (h != NULL)
            *h = (THealth){(f32)range, (f32)order};
        break;
    }
    case 2:
        de_cmd_remove(c, own, f->pos);
        break;
    default:
        (void)de_cmd_add(c, own, f->rot);
        break;
    }
    if ((rng >> 20) % 8u == 0)
        de_cmd_despawn(c, d->pre[(range * 7u) % DET_PRE]);
    const EHit hit = {prev, range, order};
    de_cmd_send_event(c, d->cw->hit, &hit);
    if (range % 5u == 0)
        de_cmd_despawn(c, prev); /* cancels the last spawn; the event's `who` becomes null */
}

static void det_range(u32 begin, u32 end, void *user) {
    const det_ctx *d = (const det_ctx *)user;
    for (u32 r = begin; r < end; ++r) {
        det_record(d, 1, r);
        det_record(d, 2, r);
    }
}

typedef struct det_thread_arg {
    const det_ctx *d;
    u32 t;
} det_thread_arg;

/* Thread t records the ranges r with r % DET_THREADS == t, system 2 before system 1. */
static i32 det_thread(void *user) {
    const det_thread_arg *a = (const det_thread_arg *)user;
    for (u32 r = a->t; r < DET_RANGES; r += DET_THREADS) {
        det_record(a->d, 2, r);
        det_record(a->d, 1, r);
    }
    return 0;
}

typedef enum det_mode { DET_SEQUENTIAL, DET_REVERSE, DET_TASKS, DET_THREADS_RAW } det_mode;

typedef struct det_result {
    u64 dump;
    u64 ids;
    u64 events;
    u32 alive;
} det_result;

/* One full run in a fresh world: DET_PRE entities, then DET_ROUNDS rounds of recording (in the
 * given mode) and applying. */
static det_result det_run(ecs_fx *f, det_mode mode, de_task_system *ts) {
    det_result res = {0, 0, FNV_SEED, 0};
    ecs_fx_destroy_world(f);
    ecs_fx_create_world(f, NULL);
    const cmd_world cw = cmd_setup(f);
    static de_entity pre[DET_PRE];
    for (u32 i = 0; i < DET_PRE; ++i) {
        const de_component set[] = {f->pos};
        if (de_entity_spawn_with(f->w, set, 1, &pre[i]) != DE_OK)
            return res;
    }
    de_event_reader reader = de_event_reader_create(f->w, cw.hit);
    det_ctx d = {f, &cw, pre, 0};
    for (u32 round = 0; round < DET_ROUNDS; ++round) {
        d.round = round;
        switch (mode) {
        case DET_SEQUENTIAL:
            det_range(0, DET_RANGES, &d);
            break;
        case DET_REVERSE:
            for (u32 r = DET_RANGES; r-- > 0;) {
                det_record(&d, 2, r);
                det_record(&d, 1, r);
            }
            break;
        case DET_TASKS:
            de_task_parallel_for(ts, DET_RANGES, 1, det_range, &d);
            break;
        case DET_THREADS_RAW: {
            de_thread *th[DET_THREADS];
            det_thread_arg args[DET_THREADS];
            for (u32 t = 0; t < DET_THREADS; ++t) {
                args[t] = (det_thread_arg){&d, t};
                if (de_thread_create(det_thread, &args[t], "cmd-rec", DE_THREAD_NORMAL, 0,
                                     &th[t]) != DE_OK)
                    th[t] = NULL;
            }
            for (u32 t = 0; t < DET_THREADS; ++t) {
                EXPECT_TRUE(th[t] != NULL);
                if (th[t] != NULL)
                    (void)de_thread_join(th[t]);
            }
            break;
        }
        }
        de_world_apply_commands(f->w);
        const EHit *e;
        while ((e = (const EHit *)de_event_next(f->w, &reader)) != NULL) {
            res.events = fnv1a(res.events, e->who.bits);
            res.events = fnv1a(res.events, ((u64)e->seq << 32) | e->src);
        }
        de_world_update_events(f->w);
    }
    res.dump = world_dump(f->w);
    res.ids = entity_ids_hash(f->w, &res.alive);
    return res;
}

static void expect_same(const det_result *a, const det_result *b) {
    EXPECT_EQ(a->dump, b->dump);
    EXPECT_EQ(a->ids, b->ids);
    EXPECT_EQ(a->events, b->events);
    EXPECT_EQ(a->alive, b->alive);
}

TEST_F(cmd, apply_order_deterministic, ecs_fx) {
    const det_result base = det_run(f, DET_SEQUENTIAL, NULL);
    EXPECT_GT(base.alive, DET_PRE / 2u);

    /* Acquire and record order do not matter, only the keys. */
    det_result r = det_run(f, DET_REVERSE, NULL);
    expect_same(&base, &r);

    /* 8 OS threads recording at once, 20 runs. */
    for (u32 run = 0; run < 20u; ++run) {
        r = det_run(f, DET_THREADS_RAW, NULL);
        expect_same(&base, &r);
    }

    /* The task system with 1 and 8 workers, 20 runs each. */
    de_fp_env_init_thread();
    const u32 workers[] = {1u, 8u};
    for (u32 wi = 0; wi < 2u; ++wi) {
        de_task_system *ts = NULL;
        de_task_system_desc td;
        memset(&td, 0, sizeof td);
        td.worker_count = workers[wi];
        td.background_worker_count = 1;
        ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
        for (u32 run = 0; run < 20u; ++run) {
            r = det_run(f, DET_TASKS, ts);
            expect_same(&base, &r);
        }
        de_task_system_destroy(ts);
    }

    /* Test vector: integer-only checksums, the same on every platform and worker count. */
    EXPECT_EQ(base.alive, DET_ALIVE_GOLDEN);
    EXPECT_EQ(base.ids, DET_IDS_GOLDEN);
    EXPECT_EQ(base.dump, DET_DUMP_GOLDEN);
    EXPECT_EQ(base.events, DET_EVENTS_GOLDEN);
}

/* ---- cmd.pending_ids_resolve ------------------------------------------------------------------
 */

TEST_F(cmd, pending_ids_resolve, ecs_fx) {
    de_commands *m = de_world_commands(f->w);
    de_commands *b1 = ecs_cmd_buffer_acquire(f->w, 1, 0);
    de_commands *b0 = ecs_cmd_buffer_acquire(f->w, 0, 0);
    de_commands *b2 = ecs_cmd_buffer_acquire(f->w, 2, 0);
    ASSERT_TRUE(m && b0 && b1 && b2);
    EXPECT_TRUE(de_world_commands(f->w) == m);

    const de_entity p0 = de_cmd_spawn(m);
    const de_entity p1 = de_cmd_spawn(b1);
    const de_entity p2 = de_cmd_spawn(b1);
    for (u32 i = 0; i < 3; ++i) {
        const de_entity p = i == 0 ? p0 : i == 1 ? p1 : p2;
        EXPECT_TRUE(de_entity_is_pending(p));
        EXPECT_EQ(de_entity_generation(p), 0u);
        EXPECT_FALSE(de_entity_alive(f->w, p));
        EXPECT_EQ(de_commands_resolve(f->w, p).bits, 0u); /* not applied yet */
    }
    EXPECT_NE(p0.bits, p1.bits);
    EXPECT_NE(p1.bits, p2.bits);
    EXPECT_FALSE(de_entity_is_pending(DE_ENTITY_NULL));

    /* (0, 0) applies before (1, 0): p1 is not spawned yet there, so the add is dropped. (2, 0)
     * applies after: its add lands on p1's entity. */
    (void)de_cmd_add(b0, p1, f->pos);
    ((TVel *)de_cmd_add(b2, p1, f->vel))->v = (de_vec3){1.0f, 2.0f, 3.0f};
    de_cmd_remove(b2, p2, f->pos); /* absent: a no-op */

    de_world_apply_commands(f->w);
    const de_entity r0 = de_commands_resolve(f->w, p0);
    const de_entity r1 = de_commands_resolve(f->w, p1);
    const de_entity r2 = de_commands_resolve(f->w, p2);
    /* Real ids in apply order: main first, then (1, 0). */
    EXPECT_EQ(r0.bits, 0x0000000100000000ull);
    EXPECT_EQ(r1.bits, 0x0000000100000001ull);
    EXPECT_EQ(r2.bits, 0x0000000100000002ull);
    EXPECT_TRUE(de_entity_alive(f->w, r0) && de_entity_alive(f->w, r1) &&
                de_entity_alive(f->w, r2));
    EXPECT_FALSE(de_entity_has(f->w, r1, f->pos));
    ASSERT_TRUE(de_entity_has(f->w, r1, f->vel));
    EXPECT_FLOAT_EQ(((const TVel *)de_entity_get_const(f->w, r1, f->vel))->v.z, 3.0f, 0.0f);
    /* A real id resolves to itself. */
    EXPECT_EQ(de_commands_resolve(f->w, r1).bits, r1.bits);
    EXPECT_EQ(de_commands_resolve(f->w, DE_ENTITY_NULL).bits, 0u);

    /* The next apply starts a new resolve generation: old pending ids no longer resolve, and a
     * reused slot's new pending ids resolve to the new entities. */
    de_commands *again = ecs_cmd_buffer_acquire(f->w, 1, 0);
    const de_entity q = de_cmd_spawn(again);
    EXPECT_EQ(de_commands_resolve(f->w, p1).bits, r1.bits); /* until the next apply */
    de_world_apply_commands(f->w);
    EXPECT_EQ(de_commands_resolve(f->w, p0).bits, 0u);
    EXPECT_EQ(de_commands_resolve(f->w, q).bits, 0x0000000100000003ull);

    /* At most DE_CMD_MAX_SPAWNS spawns per buffer and apply. */
    de_commands *big = ecs_cmd_buffer_acquire(f->w, 7, 7);
    ASSERT_TRUE(big != NULL);
    for (u32 i = 0; i < DE_CMD_MAX_SPAWNS; ++i)
        if (de_cmd_spawn(big).bits == 0)
            FAIL("spawn below the limit failed");
    EXPECT_EQ(de_cmd_spawn(big).bits, 0u);
    EXPECT_EQ(de_commands_count(big), DE_CMD_MAX_SPAWNS);
    ecs_cmd_buffer_discard(big);
    EXPECT_EQ(de_commands_count(big), 0u);
    de_world_apply_commands(f->w);
    EXPECT_EQ(alive_count(f->w), 4u);

    /* NULL arguments are refused. */
    EXPECT_EQ(de_cmd_spawn(NULL).bits, 0u);
    EXPECT_TRUE(de_cmd_add(NULL, r0, f->pos) == NULL);
    EXPECT_TRUE(de_cmd_add(m, r0, (de_component){99}) == NULL);
    de_cmd_remove(m, r0, (de_component){99});
    de_cmd_despawn(NULL, r0);
    EXPECT_EQ(de_commands_count(m), 0u);
    EXPECT_TRUE(de_world_commands(NULL) == NULL);
    de_world_apply_commands(NULL);
}

/* ---- cmd.pending_ref_in_payload_remapped ------------------------------------------------------
 */

TEST_F(cmd, pending_ref_in_payload_remapped, ecs_fx) {
    const cmd_world cw = cmd_setup(f);
    const de_entity real = de_entity_spawn(f->w);
    de_commands *m = de_world_commands(f->w);
    de_commands *b1 = ecs_cmd_buffer_acquire(f->w, 1, 0);
    ASSERT_TRUE(m && b1);

    /* Recorded first, applied second. */
    const de_entity later = de_cmd_spawn(b1);
    const de_entity gone = de_cmd_spawn(b1);
    de_cmd_despawn(b1, gone);

    const de_entity a = de_cmd_spawn(m);
    const de_entity b = de_cmd_spawn(m);
    TLink *l = (TLink *)de_cmd_add(m, b, cw.link);
    ASSERT_TRUE(l != NULL);
    l->target = a;
    l->arr[0] = a;
    l->arr[1] = real;
    l->arr[2] = DE_ENTITY_NULL;
    l->inner.e = a;
    l->inner.tag = 11;
    l->inners[0].e = b;     /* itself: spawned before this add applies */
    l->inners[1].e = later; /* spawned in a later buffer: not yet at this point -> null */
    l->n = 7;

    TLink *l2 = (TLink *)de_cmd_add(b1, later, cw.link);
    ASSERT_TRUE(l2 != NULL);
    l2->target = a;     /* from an earlier buffer of the same apply */
    l2->inner.e = gone; /* cancelled -> null */
    l2->arr[1] = later; /* itself */

    de_world_apply_commands(f->w);
    const de_entity ra = de_commands_resolve(f->w, a);
    const de_entity rb = de_commands_resolve(f->w, b);
    const de_entity rl = de_commands_resolve(f->w, later);
    ASSERT_TRUE(de_entity_alive(f->w, ra) && de_entity_alive(f->w, rb) &&
                de_entity_alive(f->w, rl));
    EXPECT_EQ(de_commands_resolve(f->w, gone).bits, 0u);

    const TLink *x = (const TLink *)de_entity_get_const(f->w, rb, cw.link);
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ(x->target.bits, ra.bits);
    EXPECT_EQ(x->arr[0].bits, ra.bits);
    EXPECT_EQ(x->arr[1].bits, real.bits);
    EXPECT_EQ(x->arr[2].bits, 0u);
    EXPECT_EQ(x->inner.e.bits, ra.bits);
    EXPECT_EQ(x->inner.tag, 11u);
    EXPECT_EQ(x->inners[0].e.bits, rb.bits);
    EXPECT_EQ(x->inners[1].e.bits, 0u);
    EXPECT_EQ(x->n, 7u);

    const TLink *y = (const TLink *)de_entity_get_const(f->w, rl, cw.link);
    ASSERT_TRUE(y != NULL);
    EXPECT_EQ(y->target.bits, ra.bits);
    EXPECT_EQ(y->inner.e.bits, 0u);
    EXPECT_EQ(y->arr[1].bits, rl.bits);
}

/* ---- cmd.send_event_applied -------------------------------------------------------------------
 */

TEST_F(cmd, send_event_applied, ecs_fx) {
    const cmd_world cw = cmd_setup(f);
    const de_entity real = de_entity_spawn(f->w);
    de_event_reader r = de_event_reader_create(f->w, cw.hit);
    de_event_reader rh = de_event_reader_create(f->w, cw.hooked);
    de_event_reader rb = de_event_reader_create(f->w, cw.big);

    de_commands *b2 = ecs_cmd_buffer_acquire(f->w, 2, 0);
    de_commands *b1 = ecs_cmd_buffer_acquire(f->w, 1, 0);
    de_commands *m = de_world_commands(f->w);
    ASSERT_TRUE(m && b1 && b2);

    de_cmd_send_event(b2, cw.hit, &(EHit){real, 20, 2});
    de_cmd_send_event(b2, cw.hit, &(EHit){real, 21, 2});
    const de_entity p = de_cmd_spawn(m);
    de_cmd_send_event(m, cw.hit, &(EHit){p, 0, 0});
    de_cmd_send_event(m, cw.hit, &(EHit){DE_ENTITY_NULL, 1, 0});
    de_cmd_send_event(b1, cw.hit, &(EHit){p, 10, 1});
    de_cmd_send_event(b1, cw.hooked, &(EHooked){5});
    EXPECT_EQ(g_ev_copy, 1);
    static EBig big;
    for (u32 i = 0; i < EBIG_BYTES; ++i)
        big.data[i] = (u8)(i * 7u);
    de_cmd_send_event(b1, cw.big, &big); /* larger than a command page */
    /* Refused: nothing recorded. */
    const u32 before = de_commands_count(b1);
    de_cmd_send_event(b1, (de_event_type){99}, &big);
    de_cmd_send_event(b1, cw.hit, NULL);
    de_cmd_send_event(NULL, cw.hit, &big);
    EXPECT_EQ(de_commands_count(b1), before);

    EXPECT_TRUE(de_event_next(f->w, &r) == NULL); /* nothing is sent before the apply */
    de_world_apply_commands(f->w);
    const de_entity rp = de_commands_resolve(f->w, p);
    ASSERT_TRUE(de_entity_alive(f->w, rp));

    /* Apply order: main, (1, 0), (2, 0); record order inside each. */
    const u32 want_seq[] = {0, 1, 10, 20, 21};
    const u64 want_who[] = {rp.bits, 0, rp.bits, real.bits, real.bits};
    for (u32 i = 0; i < 5; ++i) {
        const EHit *e = (const EHit *)de_event_next(f->w, &r);
        ASSERT_TRUE(e != NULL);
        EXPECT_EQ(e->seq, want_seq[i]);
        EXPECT_EQ(e->who.bits, want_who[i]);
    }
    EXPECT_TRUE(de_event_next(f->w, &r) == NULL);

    const EHooked *h = (const EHooked *)de_event_next(f->w, &rh);
    ASSERT_TRUE(h != NULL);
    EXPECT_EQ(h->value, 5u);
    EXPECT_EQ(g_ev_copy, 2);     /* into the buffer, then into the event queue */
    EXPECT_EQ(g_ev_destruct, 1); /* the buffer's copy */
    const EBig *eb = (const EBig *)de_event_next(f->w, &rb);
    ASSERT_TRUE(eb != NULL);
    EXPECT_EQ(memcmp(eb->data, big.data, EBIG_BYTES), 0);

    de_world_update_events(f->w);
    de_world_update_events(f->w);
    EXPECT_EQ(g_ev_destruct, 2); /* the event itself, when dropped */

    /* An unapplied hooked event is destructed when the world is destroyed. */
    de_cmd_send_event(m, cw.hooked, &(EHooked){6});
    ecs_fx_destroy_world(f);
    EXPECT_EQ(g_ev_copy, 3);
    EXPECT_EQ(g_ev_destruct, 3);
}

/* ---- cmd.dead_entity_dropped ------------------------------------------------------------------
 */

TEST_F(cmd, dead_entity_dropped, ecs_fx) {
    const cmd_world cw = cmd_setup(f);
    const de_component set[] = {f->pos};
    de_entity e = DE_ENTITY_NULL, keep = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, set, 1, &e), DE_OK);
    ASSERT_EQ(de_entity_spawn_with(f->w, set, 1, &keep), DE_OK);
    const de_entity stale = de_entity_spawn(f->w);
    de_entity_despawn(f->w, stale);

    de_commands *m = de_world_commands(f->w);
    de_commands *b1 = ecs_cmd_buffer_acquire(f->w, 1, 0);
    ASSERT_TRUE(m && b1);
    de_cmd_despawn(m, e);
    (void)de_cmd_add(b1, e, f->vel);
    TCounted *c = (TCounted *)de_cmd_add(b1, e, cw.counted);
    ASSERT_TRUE(c != NULL);
    EXPECT_EQ(c->value, 0x5EEDu); /* constructed by the hook */
    de_cmd_remove(b1, e, f->pos);
    de_cmd_despawn(b1, e);
    (void)de_cmd_add(b1, stale, f->vel);
    de_cmd_despawn(b1, stale);
    de_cmd_despawn(b1, DE_ENTITY_NULL);
    /* And one that applies: the payload replaces the constructed value. */
    TCounted *k = (TCounted *)de_cmd_add(b1, keep, cw.counted);
    ASSERT_TRUE(k != NULL);
    k->value = 42;

    de_world_apply_commands(f->w);
    EXPECT_FALSE(de_entity_alive(f->w, e));
    EXPECT_FALSE(de_entity_alive(f->w, stale));
    EXPECT_EQ(alive_count(f->w), 1u);
    const TCounted *kv = (const TCounted *)de_entity_get_const(f->w, keep, cw.counted);
    ASSERT_TRUE(kv != NULL);
    EXPECT_EQ(kv->value, 42u);
    /* The dropped payload was destructed; the applied one's column was constructed and
     * destructed before the payload moved in. */
    EXPECT_EQ(g_construct, 3);
    EXPECT_EQ(g_destruct, 2);

    /* Only `e`'s despawn and keep's add did anything: the same as doing them immediately. */
    const u64 deferred = world_dump(f->w);
    ecs_fx_destroy_world(f);
    EXPECT_EQ(g_construct, g_destruct); /* every value destructed exactly once */
    ecs_fx_create_world(f, NULL);
    const cmd_world cw2 = cmd_setup(f);
    ASSERT_EQ(de_entity_spawn_with(f->w, set, 1, &e), DE_OK);
    ASSERT_EQ(de_entity_spawn_with(f->w, set, 1, &keep), DE_OK);
    de_entity_despawn(f->w, de_entity_spawn(f->w));
    de_entity_despawn(f->w, e);
    ((TCounted *)de_entity_add(f->w, keep, cw2.counted))->value = 42;
    EXPECT_EQ(world_dump(f->w), deferred);
}

/* ---- cmd.despawn_pending_cancels --------------------------------------------------------------
 */

TEST_F(cmd, despawn_pending_cancels, ecs_fx) {
    const cmd_world cw = cmd_setup(f);
    de_entity pre[3];
    for (u32 i = 0; i < 3; ++i)
        pre[i] = de_entity_spawn(f->w);
    de_entity_despawn(f->w, pre[1]);
    const u64 before = world_dump(f->w);
    const u32 archetypes = ecs_archetype_count(f->w);
    de_event_reader r = de_event_reader_create(f->w, cw.hit);

    de_commands *m = de_world_commands(f->w);
    de_commands *b1 = ecs_cmd_buffer_acquire(f->w, 1, 0);
    de_commands *b2 = ecs_cmd_buffer_acquire(f->w, 2, 0);
    de_commands *b3 = ecs_cmd_buffer_acquire(f->w, 3, 0);
    ASSERT_TRUE(m && b1 && b2 && b3);

    /* Same buffer: spawn, adds, despawn. */
    const de_entity p = de_cmd_spawn(m);
    ((TPos *)de_cmd_add(m, p, f->pos))->v = (de_vec3){1.0f, 1.0f, 1.0f};
    (void)de_cmd_add(m, p, cw.counted);
    de_cmd_send_event(m, cw.hit, &(EHit){p, 1, 0});
    de_cmd_despawn(m, p);
    /* Despawned by a later buffer. */
    const de_entity q = de_cmd_spawn(b1);
    (void)de_cmd_add(b1, q, f->vel);
    de_cmd_despawn(b2, q);
    /* Despawned by an earlier buffer (the main one): still cancelled. */
    const de_entity s = de_cmd_spawn(b3);
    (void)de_cmd_add(b3, s, f->health);
    de_cmd_despawn(m, s);

    de_world_apply_commands(f->w);
    EXPECT_EQ(world_dump(f->w), before); /* no index used, no archetype, no generation bump */
    EXPECT_EQ(ecs_archetype_count(f->w), archetypes);
    EXPECT_EQ(de_commands_resolve(f->w, p).bits, 0u);
    EXPECT_EQ(de_commands_resolve(f->w, q).bits, 0u);
    EXPECT_EQ(de_commands_resolve(f->w, s).bits, 0u);
    EXPECT_EQ(g_construct, 1); /* the cancelled add's payload ... */
    EXPECT_EQ(g_destruct, 1);  /* ... is destructed */
    const EHit *e = (const EHit *)de_event_next(f->w, &r);
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(e->who.bits, 0u); /* the reference to the cancelled entity is null */

    /* The next real spawn takes the index the immediate API would. */
    const de_entity n = de_cmd_spawn(m);
    de_world_apply_commands(f->w);
    EXPECT_EQ(de_commands_resolve(f->w, n).bits, ecs_entity_make(1, 2).bits);
}

/* ---- cmd.no_alloc_steady_state ----------------------------------------------------------------
 */

#define NOALLOC_BUFFERS 4u
#define NOALLOC_SPAWNS  128u

TEST_F(cmd, no_alloc_steady_state, ecs_fx) {
    const cmd_world cw = cmd_setup(f);
    de_event_reader r = de_event_reader_create(f->w, cw.hit);
    static de_entity pending[NOALLOC_BUFFERS][NOALLOC_SPAWNS];
    static de_entity live[NOALLOC_BUFFERS][NOALLOC_SPAWNS];
    memset(live, 0, sizeof live);
    de_tracking_stats start = {0};
    for (u32 cycle = 0; cycle < 204u; ++cycle) {
        if (cycle == 4u)
            start = de_tracking_allocator_stats(&f->track);
        de_commands *m = de_world_commands(f->w);
        for (u32 b = 0; b < NOALLOC_BUFFERS; ++b)
            for (u32 i = 0; i < NOALLOC_SPAWNS; ++i)
                if (live[b][i].bits != 0)
                    de_cmd_despawn(m, live[b][i]);
        for (u32 b = 0; b < NOALLOC_BUFFERS; ++b) {
            de_commands *c = ecs_cmd_buffer_acquire(f->w, 1, b);
            ASSERT_TRUE(c != NULL);
            for (u32 i = 0; i < NOALLOC_SPAWNS; ++i) {
                const de_entity p = de_cmd_spawn(c);
                pending[b][i] = p;
                ((TPos *)de_cmd_add(c, p, f->pos))->v = (de_vec3){(f32)i, 0.0f, 0.0f};
                (void)de_cmd_add(c, p, f->vel);
                ((TLink *)de_cmd_add(c, p, cw.link))->target = pending[b][0];
                if (i % 16u == 0)
                    de_cmd_send_event(c, cw.hit, &(EHit){p, i, b});
            }
            de_cmd_remove(c, pending[b][1], f->vel);
        }
        de_world_apply_commands(f->w);
        for (u32 b = 0; b < NOALLOC_BUFFERS; ++b)
            for (u32 i = 0; i < NOALLOC_SPAWNS; ++i)
                live[b][i] = de_commands_resolve(f->w, pending[b][i]);
        while (de_event_next(f->w, &r) != NULL) {
        }
        de_world_update_events(f->w);
    }
    const de_tracking_stats end = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(end.alloc_count, start.alloc_count);
    EXPECT_EQ(end.realloc_count, start.realloc_count);
    EXPECT_EQ(end.free_count, start.free_count);
    EXPECT_EQ(alive_count(f->w), NOALLOC_BUFFERS * NOALLOC_SPAWNS);
    const TLink *l = (const TLink *)de_entity_get_const(f->w, live[2][5], cw.link);
    ASSERT_TRUE(l != NULL);
    EXPECT_EQ(l->target.bits, live[2][0].bits);
}

/* ---- cmd.iterate_for_undo ---------------------------------------------------------------------
 */

typedef struct seen_cmds {
    de_command cmds[16];
    u32 count;
    u32 stop_after;
} seen_cmds;

static bool collect_cmd(const de_command *cmd, void *user) {
    seen_cmds *s = (seen_cmds *)user;
    if (s->count < 16u)
        s->cmds[s->count] = *cmd;
    s->count += 1u;
    return s->stop_after == 0 || s->count < s->stop_after;
}

TEST_F(cmd, iterate_for_undo, ecs_fx) {
    const cmd_world cw = cmd_setup(f);
    const de_component set[] = {f->vel};
    de_entity real = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, set, 1, &real), DE_OK);
    const de_entity other = de_entity_spawn(f->w);

    de_commands *m = de_world_commands(f->w);
    const de_entity p = de_cmd_spawn(m);
    TPos *pp = (TPos *)de_cmd_add(m, p, f->pos);
    pp->v = (de_vec3){4.0f, 5.0f, 6.0f};
    EXPECT_TRUE(de_cmd_add(m, p, f->enemy) != NULL); /* tag: placeholder, no payload */
    de_cmd_remove(m, real, f->vel);
    de_cmd_despawn(m, other);
    de_cmd_send_event(m, cw.hit, &(EHit){p, 9, 3});
    EXPECT_EQ(de_commands_count(m), 6u);

    seen_cmds s;
    memset(&s, 0, sizeof s);
    de_commands_iterate(m, collect_cmd, &s);
    ASSERT_EQ(s.count, 6u);
    EXPECT_EQ(s.cmds[0].op, DE_CMD_SPAWN);
    EXPECT_EQ(s.cmds[0].entity.bits, p.bits);
    EXPECT_TRUE(s.cmds[0].payload == NULL);
    EXPECT_EQ(s.cmds[1].op, DE_CMD_ADD);
    EXPECT_EQ(s.cmds[1].entity.bits, p.bits);
    EXPECT_EQ(s.cmds[1].component.id, f->pos.id);
    EXPECT_EQ(s.cmds[1].payload_size, (u32)sizeof(TPos));
    EXPECT_TRUE(s.cmds[1].payload == pp);
    EXPECT_FLOAT_EQ(((const TPos *)s.cmds[1].payload)->v.y, 5.0f, 0.0f);
    EXPECT_EQ(s.cmds[2].op, DE_CMD_ADD);
    EXPECT_EQ(s.cmds[2].component.id, f->enemy.id);
    EXPECT_EQ(s.cmds[2].payload_size, 0u);
    EXPECT_EQ(s.cmds[3].op, DE_CMD_REMOVE);
    EXPECT_EQ(s.cmds[3].entity.bits, real.bits);
    EXPECT_EQ(s.cmds[3].component.id, f->vel.id);
    EXPECT_EQ(s.cmds[4].op, DE_CMD_DESPAWN);
    EXPECT_EQ(s.cmds[4].entity.bits, other.bits);
    EXPECT_EQ(s.cmds[5].op, DE_CMD_SEND_EVENT);
    EXPECT_EQ(s.cmds[5].event.id, cw.hit.id);
    EXPECT_EQ(s.cmds[5].entity.bits, 0u);
    EXPECT_EQ(s.cmds[5].payload_size, (u32)sizeof(EHit));
    EXPECT_EQ(((const EHit *)s.cmds[5].payload)->seq, 9u);

    /* Stops when the callback returns false; iterating does not change the buffer. */
    memset(&s, 0, sizeof s);
    s.stop_after = 2;
    de_commands_iterate(m, collect_cmd, &s);
    EXPECT_EQ(s.count, 2u);
    de_commands_iterate(NULL, collect_cmd, &s);
    de_commands_iterate(m, NULL, NULL);
    EXPECT_EQ(de_commands_count(m), 6u);
    EXPECT_EQ(de_commands_count(NULL), 0u);

    de_world_apply_commands(f->w);
    memset(&s, 0, sizeof s);
    de_commands_iterate(m, collect_cmd, &s);
    EXPECT_EQ(s.count, 0u);
    EXPECT_FALSE(de_entity_alive(f->w, other));
    EXPECT_FALSE(de_entity_has(f->w, real, f->vel));
    const de_entity rp = de_commands_resolve(f->w, p);
    EXPECT_TRUE(de_entity_has(f->w, rp, f->pos) && de_entity_has(f->w, rp, f->enemy));
}
