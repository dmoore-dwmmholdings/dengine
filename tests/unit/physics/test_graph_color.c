/* test_graph_color.c: deterministic graph coloring of the solver's constraints (DENG-218, SPEC
 * §13.2, §5.7): every color holds each writable body at most once, static and kinematic sides use
 * no color, the coloring does not depend on the history of body creation, more than 24 colors
 * spill into the serial overflow set (and still solve every constraint), and a 1,000-box pile
 * fits in 24 colors. */
#include "island_test_util.h"

/* Independent check of the coloring of the last prepare (graph_color.h contract). */
static void expect_valid_coloring(const de__solver *s) {
    const de__coloring *g = s->coloring;
    EXPECT_TRUE(de__graph_color_validate(s));
    EXPECT_TRUE(g->color_count <= DE__GRAPH_MAX_COLORS);
    u32 seen = 0;
    for (u32 k = 0; k <= DE__GRAPH_OVERFLOW; ++k) {
        for (u32 a = g->start[k]; a < g->start[k + 1u]; ++a) {
            const de__constraint *ca = &s->constraints[g->order[a]];
            EXPECT_EQ(ca->color, k);
            ++seen;
            if (k == DE__GRAPH_OVERFLOW)
                continue;
            for (u32 b = a + 1u; b < g->start[k + 1u]; ++b) {
                const de__constraint *cb = &s->constraints[g->order[b]];
                const bool wa = (ca->flags & DE__CONSTRAINT_WRITE_A) != 0u;
                const bool wb = (ca->flags & DE__CONSTRAINT_WRITE_B) != 0u;
                const bool xa = (cb->flags & DE__CONSTRAINT_WRITE_A) != 0u;
                const bool xb = (cb->flags & DE__CONSTRAINT_WRITE_B) != 0u;
                EXPECT_FALSE(wa && xa && ca->solver_a == cb->solver_a);
                EXPECT_FALSE(wa && xb && ca->solver_a == cb->solver_b);
                EXPECT_FALSE(wb && xa && ca->solver_b == cb->solver_a);
                EXPECT_FALSE(wb && xb && ca->solver_b == cb->solver_b);
            }
        }
    }
    EXPECT_EQ(seen, s->constraint_count);
}

/* A pit of boxes and spheres dropped on each other: random contacts of every kind. */
static void random_pile(de_physics_world *p, u64 seed, u32 n) {
    de_rng r = de_rng_seed(seed, 0);
    const de_shape g = st_box(p, 6.0f, 6.0f, 0.5f);
    (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f, 0.0f);
    const de_shape cube = st_box(p, 0.4f, 0.4f, 0.4f);
    const de_shape ball = st_sphere(p, 0.35f);
    for (u32 i = 0; i < n; ++i) {
        const f32 x = de_rng_f32(&r);
        const f32 y = de_rng_f32(&r);
        const f32 kind = de_rng_f32(&r);
        const de_transform t = st_xform(-2.0 + 4.0 * (f64)x, -2.0 + 4.0 * (f64)y,
                                        0.5 + 0.9 * (f64)i * 0.25, st_quat_identity());
        (void)st_body(p, DE_BODY_DYNAMIC, kind < 0.5f ? cube : ball, t, 0.5f, 0.0f);
    }
}

TEST(physics_graph_color, valid_every_step_of_a_random_pile) {
    de_physics_world *p = st_world(256, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    random_pile(p, 0x218u, 120);
    u32 max_colors = 0, max_records = 0;
    for (u32 t = 0; t < 150; ++t) {
        de_physics_step(p, ST_DT);
        const de__solver *s = de__solver_get(p);
        if (t % 10u == 9u)
            expect_valid_coloring(s);
        EXPECT_TRUE(de__graph_color_validate(s));
        max_colors = s->coloring->color_count > max_colors ? s->coloring->color_count : max_colors;
        max_records = s->constraint_count > max_records ? s->constraint_count : max_records;
    }
    printf("  random pile: up to %u constraints in up to %u colors\n", max_records, max_colors);
    EXPECT_GT(max_colors, 1u);
    de_physics_destroy(p);
}

TEST(physics_graph_color, static_and_kinematic_sides_use_no_color) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape g = st_box(p, 20.0f, 20.0f, 0.5f);
    const de_shape plate = st_box(p, 20.0f, 1.0f, 0.25f);
    const de_shape c = st_box(p, 0.4f, 0.4f, 0.4f);
    (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f, 0.0f);
    (void)st_body(p, DE_BODY_KINEMATIC, plate, st_xform(0, 5, 0.25, st_quat_identity()), 0.5f,
                  0.0f);
    /* 10 boxes on the ground and 10 on the kinematic plate, none touching another box. */
    for (u32 i = 0; i < 10; ++i) {
        (void)st_body(p, DE_BODY_DYNAMIC, c,
                      st_xform(-9.0 + 2.0 * i, -3, 0.401, st_quat_identity()), 0.5f, 0.0f);
        (void)st_body(p, DE_BODY_DYNAMIC, c, st_xform(-9.0 + 2.0 * i, 5, 0.901, st_quat_identity()),
                      0.5f, 0.0f);
    }
    de_physics_step(p, ST_DT);
    const de__solver *s = de__solver_get(p);
    EXPECT_EQ(s->constraint_count, 20u);
    EXPECT_EQ(s->coloring->color_count, 1u); /* the shared ground and plate are only read */
    expect_valid_coloring(s);
    de_physics_destroy(p);
}

/* Bodies created in a different temporal order but at the same indices (LIFO index reuse) give
 * the same contacts and so the same colors, tick for tick. */
TEST(physics_graph_color, independent_of_creation_history) {
    enum { N = 40, TICKS = 90 };
    static u32 colors[2][TICKS][256];
    static u32 counts[2][TICKS];
    for (u32 run = 0; run < 2; ++run) {
        de_physics_world *p = st_world(64, NULL, NULL);
        ASSERT_TRUE(p != NULL);
        const de_shape g = st_box(p, 6.0f, 6.0f, 0.5f);
        const de_shape c = st_box(p, 0.4f, 0.4f, 0.4f);
        /* Spec of body k: a column of boxes over a 4x4 grid. */
        de_transform spec[N];
        for (u32 k = 0; k < N; ++k)
            spec[k] = st_xform(-1.5 + (f64)(k % 4u), -1.5 + (f64)((k / 4u) % 4u),
                               0.45 + 0.81 * (f64)(k / 16u), st_quat_identity());
        u32 order[N];
        for (u32 k = 0; k < N; ++k)
            order[k] = run == 0 ? k : (k * 17u + 5u) % N; /* 17 coprime with 40 */
        if (run == 1) {
            /* Fill indices 1..N with placeholders, free them so that the LIFO stack hands out
             * index order[j] + 1 to the j-th create. */
            de_body tmp[N + 1];
            tmp[0] =
                st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f, 0.0f);
            for (u32 k = 1; k <= N; ++k)
                tmp[k] = st_body(p, DE_BODY_STATIC, c, st_xform(50.0 + k, 0, 0, st_quat_identity()),
                                 0, 0);
            for (u32 j = N; j-- > 0;)
                de_body_destroy(p, tmp[order[j] + 1u]);
        } else {
            (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f,
                          0.0f);
        }
        for (u32 j = 0; j < N; ++j) {
            const de_body b = st_body(p, DE_BODY_DYNAMIC, c, spec[order[j]], 0.5f, 0.0f);
            EXPECT_EQ(it_index(b), order[j] + 1u);
        }
        for (u32 t = 0; t < TICKS; ++t) {
            de_physics_step(p, ST_DT);
            const de__solver *s = de__solver_get(p);
            counts[run][t] = s->constraint_count;
            for (u32 i = 0; i < s->constraint_count && i < 256u; ++i)
                colors[run][t][i] = s->constraints[i].color;
        }
        de_physics_destroy(p);
    }
    for (u32 t = 0; t < TICKS; ++t) {
        ASSERT_EQ(counts[0][t], counts[1][t]);
        EXPECT_EQ(memcmp(colors[0][t], colors[1][t],
                         sizeof(u32) * (counts[0][t] < 256u ? counts[0][t] : 256u)),
                  0);
    }
}

TEST(physics_graph_color, overflow_set_is_solved_serially) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    static it_joints j;
    memset(&j, 0, sizeof j);
    it_joints_register(p, &j);
    const de_shape c = st_sphere(p, 0.2f);
    for (u32 i = 0; i < 32; ++i) {
        de_body_desc d;
        memset(&d, 0, sizeof d);
        d.type = DE_BODY_DYNAMIC;
        d.shape = c;
        d.transform = st_xform(2.0 * (f64)i, 0, 0, st_quat_identity());
        (void)de_body_create(p, &d);
    }
    /* A hub: body 0 linked to 30 others needs 30 colors; links between the others fit. */
    for (u32 i = 1; i <= 30; ++i)
        (void)it_joints_add(&j, 0, i);
    (void)it_joints_add(&j, 1, 2);
    (void)it_joints_add(&j, 31, 3);
    de_physics_step(p, ST_DT);
    const de__solver *s = de__solver_get(p);
    const de__coloring *g = s->coloring;
    EXPECT_EQ(g->color_count, DE__GRAPH_MAX_COLORS);
    EXPECT_EQ(g->stats.overflow, 6u); /* hub links 25..30 */
    for (u32 k = 24; k < 30; ++k)
        EXPECT_EQ(s->constraints[k].color, DE__GRAPH_OVERFLOW);
    EXPECT_EQ(s->constraints[30].color, 2u); /* 1-2: colors 0 and 1 hold bodies 1 and 2 */
    EXPECT_EQ(s->constraints[31].color, 0u); /* 31-3: body 3 is in color 2, 31 nowhere */
    expect_valid_coloring(s);
    /* Every link, colored or not, was solved once per solve and relax pass. */
    for (u32 k = 0; k < j.count; ++k)
        EXPECT_EQ(j.solves[k], 2u * p->substeps);
    de_physics_destroy(p);
}

TEST(physics_graph_color, pile_of_1000_boxes_fits_24_colors) {
    de_physics_world *p = st_world(1100, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    it_capacity(p, 16384, 16384);
    const de_shape g = st_box(p, 30.0f, 30.0f, 0.5f);
    const de_shape c = st_box(p, 0.5f, 0.5f, 0.5f);
    (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f, 0.0f);
    /* A 10 x 10 x 10 block of unit boxes, 1 mm apart, every other layer shifted by half a box so
     * each box rests on four below it. */
    for (u32 z = 0; z < 10; ++z)
        for (u32 y = 0; y < 10; ++y)
            for (u32 x = 0; x < 10; ++x) {
                const f64 off = (z & 1u) ? 0.5 : 0.0;
                (void)st_body(
                    p, DE_BODY_DYNAMIC, c,
                    st_xform(1.001 * x + off, 1.001 * y + off, 0.5 + 1.001 * z, st_quat_identity()),
                    0.5f, 0.0f);
            }
    u32 max_colors = 0, overflow = 0;
    for (u32 t = 0; t < 2; ++t) {
        de_physics_step(p, ST_DT);
        const de__solver *s = de__solver_get(p);
        max_colors = s->coloring->color_count > max_colors ? s->coloring->color_count : max_colors;
        overflow += s->coloring->stats.overflow;
        EXPECT_TRUE(de__graph_color_validate(s));
        if (t == 1)
            printf("  1,000-box pile: %u constraints, %u colors, %u in overflow, largest color "
                   "%u\n",
                   s->constraint_count, s->coloring->color_count, s->coloring->stats.overflow,
                   s->coloring->stats.largest);
    }
    EXPECT_TRUE(max_colors <= DE__GRAPH_MAX_COLORS);
    EXPECT_EQ(overflow, 0u);
    de_physics_destroy(p);
}
