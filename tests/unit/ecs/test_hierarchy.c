/* test_hierarchy.c: the ChildOf / Children hierarchy: set_parent, detach, child order, cycle
 * rejection, recursive despawn, orphaning on despawn, deferred hierarchy commands, and an
 * invariant checker over 10 000 random operations (DENG-351; SPEC §7.1).
 *
 * HIER_RANDOM_TRACE is the test vector of hier.random_ops_invariants: an integer-only hash of
 * the hierarchy after a fixed PRNG-driven sequence of immediate and deferred operations. It must
 * hold on every machine; a change is a determinism bug, never a reason to update it. */
#include "ecs_test_util.h"

#include "ecs/hierarchy_internal.h"

#include <dengine/de_platform.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

/** Hash of hier.random_ops_invariants (see the file comment). */
#define HIER_RANDOM_TRACE 0x4EF3181868238C91ull

static de_builtin_component_ids builtins(ecs_fx *f) {
    const de_builtin_component_ids ids = de_builtin_components(f->w);
    ASSERT_TRUE(DE_HANDLE_VALID(ids.transform) && DE_HANDLE_VALID(ids.global_transform) &&
                DE_HANDLE_VALID(ids.child_of) && DE_HANDLE_VALID(ids.children));
    return ids;
}

static de_entity spawn(ecs_fx *f) {
    const de_entity e = de_entity_spawn(f->w);
    ASSERT_TRUE(e.bits != 0);
    return e;
}

static bool same(de_entity a, de_entity b) {
    return a.bits == b.bits;
}

/* The children of `p` in list order into `out` (at most `cap`); returns the count walked. */
static u32 children_of(ecs_fx *f, de_entity p, de_entity *out, u32 cap) {
    u32 n = 0;
    for (de_entity c = de_entity_children_first(f->w, p); c.bits != 0;
         c = de_entity_children_next(f->w, c)) {
        if (n < cap)
            out[n] = c;
        if (++n > 100000u)
            break; /* a loop in the list */
    }
    return n;
}

/* ---- Invariant checker ---------------------------------------------------------------------- */

/* Checks every hierarchy invariant (hierarchy.c) for the entities `es[0..n)` that are alive.
 * Returns false (with a failed EXPECT) on the first violation. */
static bool check_structure(ecs_fx *f, const de_builtin_component_ids *ids, const de_entity *es,
                            u32 n) {
    for (u32 i = 0; i < n; ++i) {
        const de_entity e = es[i];
        if (!de_entity_alive(f->w, e))
            continue;
        const DeChildOf *co = (const DeChildOf *)de_entity_get_const(f->w, e, ids->child_of);
        const DeChildren *ch = (const DeChildren *)de_entity_get_const(f->w, e, ids->children);
        if (co != NULL) {
            /* The parent is alive, has children and lists e exactly once. */
            if (!de_entity_alive(f->w, co->parent) ||
                !de_entity_has(f->w, co->parent, ids->children)) {
                EXPECT_TRUE(!"DeChildOf names a dead parent or one without DeChildren");
                return false;
            }
            u32 hits = 0, walked = 0;
            for (de_entity c = de_entity_children_first(f->w, co->parent);
                 c.bits != 0 && walked <= n; c = de_entity_children_next(f->w, c), ++walked)
                hits += same(c, e) ? 1u : 0u;
            if (hits != 1u) {
                EXPECT_EQ(hits, 1u);
                return false;
            }
        }
        if (ch != NULL) {
            if (ch->count == 0) {
                EXPECT_TRUE(!"DeChildren with count 0");
                return false;
            }
            de_entity prev = DE_ENTITY_NULL, c = ch->first;
            u32 k = 0;
            while (c.bits != 0 && k <= n) {
                const DeChildOf *cc =
                    (const DeChildOf *)de_entity_get_const(f->w, c, ids->child_of);
                if (cc == NULL || !same(cc->parent, e) || !same(cc->prev_sibling, prev)) {
                    EXPECT_TRUE(!"a child's DeChildOf does not match the list");
                    return false;
                }
                prev = c;
                c = cc->next_sibling;
                ++k;
            }
            if (k != ch->count || !same(prev, ch->last)) {
                EXPECT_EQ(k, ch->count);
                EXPECT_EQ(prev.bits, ch->last.bits);
                return false;
            }
        }
        /* No cycle: the ancestor walk ends within n steps. */
        de_entity a = e;
        u32 steps = 0;
        while (a.bits != 0 && steps <= n) {
            a = de_entity_parent(f->w, a);
            ++steps;
        }
        if (a.bits != 0) {
            EXPECT_TRUE(!"cycle in the ancestor chain");
            return false;
        }
    }
    return true;
}

/* ---- hier.set_parent_detach ------------------------------------------------------------------ */

TEST_F(hier, set_parent_detach, ecs_fx) {
    /* Before the first call the world has only the fixture's components. */
    EXPECT_EQ(de_entity_parent(f->w, spawn(f)).bits, 0u);
    EXPECT_EQ(de_component_find(f->w, "DeTransform").id, 0u);
    const de_builtin_component_ids ids = builtins(f);
    /* Lazy registration: the next free ids, in the documented order; idempotent. */
    EXPECT_EQ(ids.transform.id, 6u);
    EXPECT_EQ(ids.global_transform.id, 7u);
    EXPECT_EQ(ids.child_of.id, 8u);
    EXPECT_EQ(ids.children.id, 9u);
    EXPECT_EQ(de_builtin_components(f->w).children.id, 9u);
    EXPECT_EQ(de_component_find(f->w, "DeChildOf").id, ids.child_of.id);

    const de_entity p = spawn(f), c = spawn(f);
    ASSERT_EQ(de_entity_set_parent(f->w, c, p), DE_OK);
    EXPECT_EQ(de_entity_parent(f->w, c).bits, p.bits);
    EXPECT_EQ(de_entity_parent(f->w, p).bits, 0u);
    EXPECT_EQ(de_entity_children_first(f->w, p).bits, c.bits);
    EXPECT_EQ(de_entity_children_next(f->w, c).bits, 0u);
    EXPECT_EQ(de_entity_children_count(f->w, p), 1u);
    EXPECT_TRUE(de_entity_has(f->w, c, ids.child_of));
    EXPECT_TRUE(de_entity_has(f->w, p, ids.children));
    EXPECT_FALSE(de_entity_has(f->w, p, ids.child_of));
    const de_entity es[2] = {p, c};
    EXPECT_TRUE(check_structure(f, &ids, es, 2));

    /* Same parent again: nothing changes. */
    ASSERT_EQ(de_entity_set_parent(f->w, c, p), DE_OK);
    EXPECT_EQ(de_entity_children_count(f->w, p), 1u);

    /* Detach: the child loses DeChildOf, the parent DeChildren (its last child). */
    ASSERT_EQ(de_entity_set_parent(f->w, c, DE_ENTITY_NULL), DE_OK);
    EXPECT_EQ(de_entity_parent(f->w, c).bits, 0u);
    EXPECT_FALSE(de_entity_has(f->w, c, ids.child_of));
    EXPECT_FALSE(de_entity_has(f->w, p, ids.children));
    EXPECT_EQ(de_entity_children_first(f->w, p).bits, 0u);
    EXPECT_EQ(de_entity_children_count(f->w, p), 0u);
    /* Detaching a root is a no-op. */
    EXPECT_EQ(de_entity_set_parent(f->w, c, DE_ENTITY_NULL), DE_OK);

    /* Dead child or parent. */
    const de_entity dead = spawn(f);
    de_entity_despawn(f->w, dead);
    EXPECT_EQ(de_entity_set_parent(f->w, dead, p), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_entity_set_parent(f->w, c, dead), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_entity_set_parent(NULL, c, p), DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(de_entity_has(f->w, c, ids.child_of));
    EXPECT_FALSE(de_entity_has(f->w, p, ids.children));

    /* Other components survive the moves; a reparent goes from one list to the other. */
    TPos *pos = (TPos *)de_entity_add(f->w, c, f->pos);
    pos->v.x = 42.0f;
    const de_entity q = spawn(f);
    ASSERT_EQ(de_entity_set_parent(f->w, c, p), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, c, q), DE_OK);
    EXPECT_EQ(de_entity_parent(f->w, c).bits, q.bits);
    EXPECT_FALSE(de_entity_has(f->w, p, ids.children));
    EXPECT_EQ(de_entity_children_count(f->w, q), 1u);
    EXPECT_FLOAT_EQ(((const TPos *)de_entity_get_const(f->w, c, f->pos))->v.x, 42.0, 0.0);
    const de_entity es2[3] = {p, c, q};
    EXPECT_TRUE(check_structure(f, &ids, es2, 3));
}

/* ---- hier.child_order_insertion -------------------------------------------------------------- */

TEST_F(hier, child_order_insertion, ecs_fx) {
    const de_builtin_component_ids ids = builtins(f);
    const de_entity p = spawn(f), other = spawn(f);
    de_entity k[5];
    for (u32 i = 0; i < 5; ++i) {
        k[i] = spawn(f);
        ASSERT_EQ(de_entity_set_parent(f->w, k[i], p), DE_OK);
    }
    de_entity got[8];
    ASSERT_EQ(children_of(f, p, got, 8), 5u);
    for (u32 i = 0; i < 5; ++i)
        EXPECT_EQ(got[i].bits, k[i].bits);

    /* Remove the middle, the first and the last: the rest keep their order. */
    ASSERT_EQ(de_entity_set_parent(f->w, k[2], other), DE_OK);
    ASSERT_EQ(children_of(f, p, got, 8), 4u);
    EXPECT_EQ(got[0].bits, k[0].bits);
    EXPECT_EQ(got[1].bits, k[1].bits);
    EXPECT_EQ(got[2].bits, k[3].bits);
    EXPECT_EQ(got[3].bits, k[4].bits);
    ASSERT_EQ(de_entity_set_parent(f->w, k[0], DE_ENTITY_NULL), DE_OK);
    de_entity_despawn(f->w, k[4]);
    ASSERT_EQ(children_of(f, p, got, 8), 2u);
    EXPECT_EQ(got[0].bits, k[1].bits);
    EXPECT_EQ(got[1].bits, k[3].bits);

    /* Coming back appends at the end (insertion order, not original order). */
    ASSERT_EQ(de_entity_set_parent(f->w, k[2], p), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, k[0], p), DE_OK);
    ASSERT_EQ(children_of(f, p, got, 8), 4u);
    EXPECT_EQ(got[0].bits, k[1].bits);
    EXPECT_EQ(got[1].bits, k[3].bits);
    EXPECT_EQ(got[2].bits, k[2].bits);
    EXPECT_EQ(got[3].bits, k[0].bits);
    const DeChildren *ch = (const DeChildren *)de_entity_get_const(f->w, p, ids.children);
    ASSERT_TRUE(ch != NULL);
    EXPECT_EQ(ch->first.bits, k[1].bits);
    EXPECT_EQ(ch->last.bits, k[0].bits);
    EXPECT_EQ(ch->count, 4u);
    const de_entity es[7] = {p, other, k[0], k[1], k[2], k[3], k[4]};
    EXPECT_TRUE(check_structure(f, &ids, es, 7));
}

/* ---- hier.cycle_rejected --------------------------------------------------------------------- */

static jmp_buf g_panic_jmp;
static int g_panic_calls;

static void longjmp_hook(const de_panic_info *info, void *user) {
    (void)info;
    (void)user;
    ++g_panic_calls;
    longjmp(g_panic_jmp, 1);
}

typedef struct cycle_case {
    de_world *w;
    de_entity child, parent;
    de_result result;
    bool returned;
} cycle_case;

static i32 set_parent_thread(void *p) {
    cycle_case *cc = (cycle_case *)p;
    if (setjmp(g_panic_jmp) == 0) {
        cc->result = de_entity_set_parent(cc->w, cc->child, cc->parent);
        cc->returned = true;
    }
    return 0;
}

/* Calls de_entity_set_parent on a fresh thread with a panic hook (a thread that left a panic by
 * longjmp still counts as panicking). Returns true if it panicked. */
static bool set_parent_expect(ecs_fx *f, de_entity child, de_entity parent, de_result *out) {
    const int before = g_panic_calls;
    cycle_case cc = {f->w, child, parent, DE_OK, false};
    de_panic_set_hook(longjmp_hook, NULL);
    de_thread *t = NULL;
    if (de_thread_create(set_parent_thread, &cc, "hier-cycle", DE_THREAD_NORMAL, 0, &t) != DE_OK) {
        de_panic_set_hook(NULL, NULL);
        EXPECT_TRUE(!"de_thread_create failed");
        return false;
    }
    (void)de_thread_join(t);
    de_panic_set_hook(NULL, NULL);
    *out = cc.returned ? cc.result : DE_OK;
    return g_panic_calls != before;
}

TEST_F(hier, cycle_rejected, ecs_fx) {
    const de_builtin_component_ids ids = builtins(f);
    /* a -> b -> c (c is a's grandchild). */
    const de_entity a = spawn(f), b = spawn(f), c = spawn(f);
    ASSERT_EQ(de_entity_set_parent(f->w, b, a), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, c, b), DE_OK);
    const de_entity es[3] = {a, b, c};

    const de_entity tries[3][2] = {{a, c}, {a, b}, {b, b}}; /* under a descendant, or itself */
    for (u32 i = 0; i < 3; ++i) {
        de_result r = DE_OK;
        const bool panicked = set_parent_expect(f, tries[i][0], tries[i][1], &r);
#if defined(DE_DEBUG) || defined(DE_DEV)
        EXPECT_TRUE(panicked); /* the Debug assert */
#else
        EXPECT_FALSE(panicked);
        EXPECT_EQ(r, DE_ERR_INVALID_ARGUMENT);
#endif
        /* Nothing changed. */
        EXPECT_EQ(de_entity_parent(f->w, a).bits, 0u);
        EXPECT_EQ(de_entity_parent(f->w, b).bits, a.bits);
        EXPECT_EQ(de_entity_parent(f->w, c).bits, b.bits);
        EXPECT_TRUE(check_structure(f, &ids, es, 3));
    }

    /* Deferred: dropped at apply, with a warning and no assert. */
    de_commands *cmd = de_world_commands(f->w);
    de_cmd_set_parent(cmd, a, c);
    de_cmd_set_parent(cmd, c, c);
    de_world_apply_commands(f->w);
    EXPECT_EQ(de_entity_parent(f->w, a).bits, 0u);
    EXPECT_EQ(de_entity_parent(f->w, c).bits, b.bits);
    EXPECT_TRUE(check_structure(f, &ids, es, 3));

    /* Moving a subtree under a node outside it is fine. */
    const de_entity d = spawn(f);
    ASSERT_EQ(de_entity_set_parent(f->w, b, d), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, a, c), DE_OK); /* a no longer is c's ancestor */
    const de_entity es2[4] = {a, b, c, d};
    EXPECT_TRUE(check_structure(f, &ids, es2, 4));
}

/* ---- hier.despawn_recursive ------------------------------------------------------------------ */

TEST_F(hier, despawn_recursive, ecs_fx) {
    const de_builtin_component_ids ids = builtins(f);
    /* root -> {m, s}; m -> {x, y}; x -> {z}. */
    const de_entity root = spawn(f), m = spawn(f), s = spawn(f), x = spawn(f), y = spawn(f),
                    z = spawn(f);
    ASSERT_EQ(de_entity_set_parent(f->w, m, root), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, s, root), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, x, m), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, y, m), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, z, x), DE_OK);

    de_entity_despawn_recursive(f->w, m);
    EXPECT_FALSE(de_entity_alive(f->w, m));
    EXPECT_FALSE(de_entity_alive(f->w, x));
    EXPECT_FALSE(de_entity_alive(f->w, y));
    EXPECT_FALSE(de_entity_alive(f->w, z));
    EXPECT_TRUE(de_entity_alive(f->w, root));
    EXPECT_TRUE(de_entity_alive(f->w, s));
    de_entity got[4];
    ASSERT_EQ(children_of(f, root, got, 4), 1u);
    EXPECT_EQ(got[0].bits, s.bits);
    const de_entity es[2] = {root, s};
    EXPECT_TRUE(check_structure(f, &ids, es, 2));

    /* Post-order, leftmost leaf first: z, x, y, then m. Freed indices are reused last in, first
     * out, so the next spawns get m, y, x, z's indices in that order. */
    EXPECT_EQ(de_entity_index(spawn(f)), de_entity_index(m));
    EXPECT_EQ(de_entity_index(spawn(f)), de_entity_index(y));
    EXPECT_EQ(de_entity_index(spawn(f)), de_entity_index(x));
    EXPECT_EQ(de_entity_index(spawn(f)), de_entity_index(z));

    /* A deep chain (no recursion in the implementation) and a pending target through commands. */
    de_entity chain[2000];
    chain[0] = spawn(f);
    for (u32 i = 1; i < 2000; ++i) {
        chain[i] = spawn(f);
        ASSERT_EQ(de_entity_set_parent(f->w, chain[i], chain[i - 1]), DE_OK);
    }
    de_cmd_despawn_recursive(de_world_commands(f->w), chain[0]);
    de_world_apply_commands(f->w);
    for (u32 i = 0; i < 2000; ++i)
        EXPECT_FALSE(de_entity_alive(f->w, chain[i]));

    /* A dead entity is ignored; a lone entity is a plain despawn. */
    de_entity_despawn_recursive(f->w, m);
    const de_entity lone = spawn(f);
    de_entity_despawn_recursive(f->w, lone);
    EXPECT_FALSE(de_entity_alive(f->w, lone));
}

/* ---- hier.despawn_parent_orphans ------------------------------------------------------------- */

TEST_F(hier, despawn_parent_orphans, ecs_fx) {
    const de_builtin_component_ids ids = builtins(f);
    const de_entity g = spawn(f), p = spawn(f), a = spawn(f), b = spawn(f), q = spawn(f),
                    c = spawn(f);
    ASSERT_EQ(de_entity_set_parent(f->w, p, g), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, q, g), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, a, p), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, b, p), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, c, q), DE_OK);
    ASSERT_TRUE(de_entity_add(f->w, a, ids.transform) != NULL);

    /* Immediate despawn of p: a and b become roots, g lists only q. */
    de_entity_despawn(f->w, p);
    EXPECT_TRUE(de_entity_alive(f->w, a));
    EXPECT_TRUE(de_entity_alive(f->w, b));
    EXPECT_EQ(de_entity_parent(f->w, a).bits, 0u);
    EXPECT_EQ(de_entity_parent(f->w, b).bits, 0u);
    EXPECT_FALSE(de_entity_has(f->w, a, ids.child_of));
    EXPECT_FALSE(de_entity_has(f->w, b, ids.child_of));
    EXPECT_TRUE(de_entity_has(f->w, a, ids.transform)); /* other components stay */
    de_entity got[4];
    ASSERT_EQ(children_of(f, g, got, 4), 1u);
    EXPECT_EQ(got[0].bits, q.bits);

    /* Deferred despawn of q: the same through the command path. */
    de_cmd_despawn(de_world_commands(f->w), q);
    de_world_apply_commands(f->w);
    EXPECT_FALSE(de_entity_alive(f->w, q));
    EXPECT_EQ(de_entity_parent(f->w, c).bits, 0u);
    EXPECT_FALSE(de_entity_has(f->w, g, ids.children));
    const de_entity es[6] = {g, p, a, b, q, c};
    EXPECT_TRUE(check_structure(f, &ids, es, 6));
}

/* ---- hier.cmd_set_parent --------------------------------------------------------------------- */

typedef struct seen_cmds {
    u32 set_parent, despawn_recursive;
    de_entity last_parent;
} seen_cmds;

static bool visit_cmd(const de_command *cmd, void *user) {
    seen_cmds *s = (seen_cmds *)user;
    if (cmd->op == DE_CMD_SET_PARENT) {
        ++s->set_parent;
        if (cmd->payload_size == sizeof(de_entity))
            memcpy(&s->last_parent, cmd->payload, sizeof(de_entity));
    } else if (cmd->op == DE_CMD_DESPAWN_RECURSIVE) {
        ++s->despawn_recursive;
    }
    return true;
}

TEST_F(hier, cmd_set_parent, ecs_fx) {
    /* Without built-ins nothing is recorded. */
    de_commands *cmd = de_world_commands(f->w);
    const de_entity r0 = spawn(f), r1 = spawn(f);
    de_cmd_set_parent(cmd, r1, r0);
    EXPECT_EQ(de_commands_count(cmd), 0u);

    const de_builtin_component_ids ids = builtins(f);
    /* Pending child under a real parent, a pending parent, a real reparent and a detach. */
    const de_entity pc = de_cmd_spawn(cmd);
    const de_entity pp = de_cmd_spawn(cmd);
    de_cmd_set_parent(cmd, pc, r0);
    de_cmd_set_parent(cmd, r1, pp);
    de_cmd_set_parent(cmd, pp, r0);
    de_cmd_despawn_recursive(cmd, DE_ENTITY_NULL); /* dropped at apply (not alive) */
    seen_cmds seen;
    memset(&seen, 0, sizeof seen);
    de_commands_iterate(cmd, visit_cmd, &seen);
    EXPECT_EQ(seen.set_parent, 3u);
    EXPECT_EQ(seen.despawn_recursive, 1u);
    EXPECT_EQ(seen.last_parent.bits, r0.bits);
    de_world_apply_commands(f->w);

    const de_entity c = de_commands_resolve(f->w, pc);
    const de_entity p = de_commands_resolve(f->w, pp);
    ASSERT_TRUE(de_entity_alive(f->w, c) && de_entity_alive(f->w, p));
    EXPECT_EQ(de_entity_parent(f->w, c).bits, r0.bits);
    EXPECT_EQ(de_entity_parent(f->w, r1).bits, p.bits);
    EXPECT_EQ(de_entity_parent(f->w, p).bits, r0.bits);
    de_entity got[4];
    ASSERT_EQ(children_of(f, r0, got, 4), 2u);
    EXPECT_EQ(got[0].bits, c.bits);
    EXPECT_EQ(got[1].bits, p.bits);

    /* A cancelled pending parent drops the command; a detach; a dead child is dropped. */
    const de_entity gone = de_cmd_spawn(cmd);
    de_cmd_set_parent(cmd, c, gone);
    de_cmd_despawn(cmd, gone);
    de_cmd_set_parent(cmd, r1, DE_ENTITY_NULL);
    de_world_apply_commands(f->w);
    EXPECT_EQ(de_entity_parent(f->w, c).bits, r0.bits);
    EXPECT_EQ(de_entity_parent(f->w, r1).bits, 0u);
    EXPECT_FALSE(de_entity_has(f->w, p, ids.children));
    const de_entity es[4] = {r0, r1, c, p};
    EXPECT_TRUE(check_structure(f, &ids, es, 4));
}

/* ---- hier.random_ops_invariants -------------------------------------------------------------- */

#define RM_SLOTS     16384u
#define RM_MAX_ALIVE 160u
#define RM_OPS       10000u
#define RM_KIDS_CAP  (RM_MAX_ALIVE + 8u)

/* The model: every entity ever spawned has a slot; `par` is a slot or -1; the children of a
 * parent are its live children ordered by `stamp` (the time they were attached). `live` lists the
 * alive slots in spawn order, so picks and scans cost O(alive). */
typedef struct rmodel {
    de_entity e[RM_SLOTS];
    bool alive[RM_SLOTS];
    i32 par[RM_SLOTS];
    u64 stamp[RM_SLOTS];
    i32 live[RM_SLOTS];
    i32 doomed[RM_SLOTS];    /* rm_despawn_recursive scratch */
    de_entity tmp[RM_SLOTS]; /* check_structure input */
    u32 n, nlive;
    u64 next_stamp;
    u64 rng;
} rmodel;

static u32 rm_rand(rmodel *m) {
    /* xorshift64*: integer-only, the same on every machine. */
    m->rng ^= m->rng >> 12;
    m->rng ^= m->rng << 25;
    m->rng ^= m->rng >> 27;
    return (u32)((m->rng * 0x2545F4914F6CDD1Dull) >> 32);
}

/* A random alive slot, or -1. */
static i32 rm_pick(rmodel *m) {
    return m->nlive ? m->live[rm_rand(m) % m->nlive] : -1;
}

static void rm_add(rmodel *m, de_entity e) {
    m->e[m->n] = e;
    m->alive[m->n] = true;
    m->par[m->n] = -1;
    m->stamp[m->n] = 0;
    m->live[m->nlive++] = (i32)m->n;
    ++m->n;
}

/* Marks slot `s` dead (keeps `live` in spawn order). */
static void rm_kill(rmodel *m, i32 s) {
    m->alive[s] = false;
    m->par[s] = -1;
    for (u32 i = 0; i < m->nlive; ++i)
        if (m->live[i] == s) {
            memmove(&m->live[i], &m->live[i + 1], (size_t)(m->nlive - i - 1u) * sizeof(i32));
            --m->nlive;
            return;
        }
}

/* true if `anc` is `s` or one of its ancestors in the model. */
static bool rm_is_self_or_ancestor(const rmodel *m, i32 anc, i32 s) {
    while (s >= 0) {
        if (s == anc)
            return true;
        s = m->par[s];
    }
    return false;
}

/* The model of set_parent at the time it applies; false when it is rejected or dropped. */
static bool rm_set_parent(rmodel *m, i32 c, i32 p) {
    if (c < 0 || !m->alive[c])
        return false;
    if (p < 0) {
        m->par[c] = -1;
        return true;
    }
    if (!m->alive[p] || rm_is_self_or_ancestor(m, c, p))
        return false;
    if (m->par[c] != p) {
        m->par[c] = p;
        m->stamp[c] = m->next_stamp++;
    }
    return true;
}

static void rm_despawn(rmodel *m, i32 s) {
    for (u32 i = 0; i < m->nlive; ++i)
        if (m->par[m->live[i]] == s)
            m->par[m->live[i]] = -1;
    rm_kill(m, s);
}

static void rm_despawn_recursive(rmodel *m, i32 s) {
    u32 nd = 0;
    for (u32 i = 0; i < m->nlive; ++i)
        if (rm_is_self_or_ancestor(m, s, m->live[i]))
            m->doomed[nd++] = m->live[i];
    for (u32 i = 0; i < nd; ++i)
        rm_kill(m, m->doomed[i]);
}

/* Compares the world with the model: liveness of every slot, parents and every child list's
 * order. */
static bool rm_check_model(ecs_fx *f, const rmodel *m) {
    for (u32 s = 0; s < m->n; ++s)
        if (de_entity_alive(f->w, m->e[s]) != m->alive[s]) {
            EXPECT_TRUE(!"liveness differs from the model");
            return false;
        }
    for (u32 li = 0; li < m->nlive; ++li) {
        const i32 s = m->live[li];
        const de_entity want = m->par[s] < 0 ? DE_ENTITY_NULL : m->e[m->par[s]];
        if (!same(de_entity_parent(f->w, m->e[s]), want)) {
            EXPECT_TRUE(!"parent differs from the model");
            return false;
        }
        /* Expected children: by stamp (insertion sort of the model's kids). */
        i32 kids[RM_KIDS_CAP];
        u32 nk = 0;
        for (u32 lj = 0; lj < m->nlive && nk < RM_KIDS_CAP; ++lj) {
            const i32 i = m->live[lj];
            if (m->par[i] != s)
                continue;
            u32 j = nk++;
            while (j > 0 && m->stamp[kids[j - 1]] > m->stamp[i]) {
                kids[j] = kids[j - 1];
                --j;
            }
            kids[j] = i;
        }
        de_entity got[RM_KIDS_CAP];
        const u32 ng = children_of(f, m->e[s], got, RM_KIDS_CAP);
        if (ng != nk || de_entity_children_count(f->w, m->e[s]) != nk) {
            EXPECT_EQ(ng, nk);
            return false;
        }
        for (u32 i = 0; i < nk; ++i)
            if (!same(got[i], m->e[kids[i]])) {
                EXPECT_TRUE(!"child order differs from the model");
                return false;
            }
    }
    return true;
}

static u64 fnv(u64 h, u64 v) {
    for (u32 i = 0; i < 8; ++i) {
        h ^= (v >> (8u * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

TEST_F(hier, random_ops_invariants, ecs_fx) {
    const de_builtin_component_ids ids = builtins(f);
    rmodel *m = (rmodel *)calloc(1, sizeof *m);
    ASSERT_TRUE(m != NULL);
    m->rng = 0x9E3779B97F4A7C15ull;
    u32 counts[6] = {0};
    u64 alive_sum = 0;
    de_commands *cmd = de_world_commands(f->w);

    for (u32 op = 0; op < RM_OPS; ++op) {
        const u32 r = rm_rand(m) % 100u;
        if (r < 35u || m->nlive < 4u) {
            if (m->nlive >= RM_MAX_ALIVE || m->n >= RM_SLOTS - 8u) {
                ++counts[5];
                continue;
            }
            de_entity e;
            if (rm_rand(m) & 1u) {
                ASSERT_EQ(de_entity_spawn_with(f->w, &ids.transform, 1, &e), DE_OK);
            } else {
                e = spawn(f);
            }
            rm_add(m, e);
            ++counts[0];
        } else if (r < 70u) {
            const i32 c = rm_pick(m);
            const i32 p = (rm_rand(m) % 10u) == 0 ? -1 : rm_pick(m);
            if (p >= 0 && rm_is_self_or_ancestor(m, c, p)) {
                ++counts[2]; /* a cycle: the immediate call would assert; tested deferred */
                de_cmd_set_parent(cmd, m->e[c], m->e[p]);
                de_world_apply_commands(f->w);
            } else {
                ASSERT_EQ(de_entity_set_parent(f->w, m->e[c], p < 0 ? DE_ENTITY_NULL : m->e[p]),
                          DE_OK);
                ASSERT_TRUE(rm_set_parent(m, c, p));
                ++counts[1];
            }
        } else if (r < 74u) {
            const i32 s = rm_pick(m);
            de_entity_despawn(f->w, m->e[s]);
            rm_despawn(m, s);
            ++counts[3];
        } else if (r < 76u) {
            const i32 s = rm_pick(m);
            de_entity_despawn_recursive(f->w, m->e[s]);
            rm_despawn_recursive(m, s);
            ++counts[3];
        } else {
            /* A batch of 1..4 deferred commands, modelled in apply (= record) order. */
            typedef struct rec {
                u32 kind;
                i32 a, b;
                de_entity pending;
            } rec;
            rec batch[4];
            const u32 nb = 1u + rm_rand(m) % 4u;
            for (u32 i = 0; i < nb; ++i) {
                rec *x = &batch[i];
                x->kind = rm_rand(m) % 8u;
                if (x->kind >= 6u && m->nlive >= RM_MAX_ALIVE)
                    x->kind = 0; /* full: a set_parent instead of a spawn */
                x->a = rm_pick(m);
                x->b = (rm_rand(m) % 8u) == 0 ? -1 : rm_pick(m);
                x->pending = DE_ENTITY_NULL;
                switch (x->kind) {
                case 0: /* set_parent (a cycle at apply is dropped) */
                case 1:
                case 2:
                case 3:
                    de_cmd_set_parent(cmd, m->e[x->a], x->b < 0 ? DE_ENTITY_NULL : m->e[x->b]);
                    break;
                case 4:
                    de_cmd_despawn(cmd, m->e[x->a]);
                    break;
                case 5:
                    de_cmd_despawn_recursive(cmd, m->e[x->a]);
                    break;
                default: /* spawn a child of b */
                    x->pending = de_cmd_spawn(cmd);
                    if (x->b >= 0)
                        de_cmd_set_parent(cmd, x->pending, m->e[x->b]);
                    break;
                }
            }
            de_world_apply_commands(f->w);
            for (u32 i = 0; i < nb; ++i) {
                const rec *x = &batch[i];
                switch (x->kind) {
                case 0:
                case 1:
                case 2:
                case 3:
                    (void)rm_set_parent(m, x->a, x->b);
                    break;
                case 4:
                    if (m->alive[x->a])
                        rm_despawn(m, x->a);
                    break;
                case 5:
                    if (m->alive[x->a])
                        rm_despawn_recursive(m, x->a);
                    break;
                default: {
                    /* The real id (a later command of the batch may have despawned it). */
                    const de_entity e = de_commands_resolve(f->w, x->pending);
                    ASSERT_TRUE(e.bits != 0);
                    rm_add(m, e);
                    if (x->b >= 0)
                        (void)rm_set_parent(m, (i32)(m->n - 1u), x->b);
                    break;
                }
                }
            }
            ++counts[4];
        }
        alive_sum += m->nlive;

        /* Structure after every operation; the full model every 100. */
        for (u32 i = 0; i < m->nlive; ++i)
            m->tmp[i] = m->e[m->live[i]];
        if (!check_structure(f, &ids, m->tmp, m->nlive)) {
            printf("hier.random_ops_invariants: structure broken after op %u\n", op);
            break;
        }
        if (op % 100u == 99u && !rm_check_model(f, m)) {
            printf("hier.random_ops_invariants: model differs after op %u\n", op);
            break;
        }
    }
    EXPECT_TRUE(rm_check_model(f, m));

    u64 h = 0xCBF29CE484222325ull;
    for (u32 s = 0; s < m->n; ++s) {
        h = fnv(h, m->alive[s] ? 1u : 0u);
        if (!m->alive[s])
            continue;
        h = fnv(h, m->e[s].bits);
        h = fnv(h, de_entity_parent(f->w, m->e[s]).bits);
        for (de_entity c = de_entity_children_first(f->w, m->e[s]); c.bits != 0;
             c = de_entity_children_next(f->w, c))
            h = fnv(h, c.bits);
    }
    printf("[hier] random ops: %u spawns, %u set_parent, %u cycles rejected, %u despawns "
           "(plain and recursive), %u command batches, %u spawns skipped (full); mean %.1f alive, "
           "%u slots; trace 0x%016llX\n",
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
           (double)alive_sum / RM_OPS, m->n, (unsigned long long)h);
    EXPECT_GT(counts[2], 0u);
    EXPECT_EQ(h, HIER_RANDOM_TRACE);
    free(m);
}
