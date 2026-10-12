/* test_harness.c: self tests of the physics determinism harness (DENG-136).
 *
 *   harness.detects_one_ulp        a 1-ulp change of one body's position at tick 400 is reported
 *                                  at exactly that tick (no task system and 4 workers), and the
 *                                  field-level diff names that body and field
 *   harness.workers_field_level    workers 0 and 4: no field differs after the last tick
 *   harness.scenarios_collide      kinematic_sweep makes contacts on many ticks; hull_builder
 *                                  builds its hulls and rejects the planar clouds
 *   harness.begin_errors           incomplete scenarios and failing setups are refused cleanly
 */
#include "harness.h"

#include "physics/physics_internal.h"

#include <stdio.h>
#include <string.h>

/* scenarios_collision.c */
extern const de_phys_det_scenario de_phys_det_scenario_kinematic_sweep;
extern const de_phys_det_scenario de_phys_det_scenario_hull_builder;
u32 de_phys_det_kinematic_sweep_contact_ticks(const de_phys_det_ctx *c);
de_body de_phys_det_kinematic_sweep_mover(const de_phys_det_ctx *c, u32 k);
u32 de_phys_det_hull_builder_counts(const de_phys_det_ctx *c, u32 *rejected);

#define KS_TICKS      1000u
#define PERTURB_TICK  400u
#define PERTURB_MOVER 2u

static u64 g_base[KS_TICKS], g_run[KS_TICKS];

/* Test-only perturbation: one ulp on the x position of one mover, before the step of `tick`. */
typedef struct perturb {
    u64 tick;
    u32 mover;
} perturb;

static void inject_one_ulp(de_phys_det_ctx *c, u64 tick, void *user) {
    const perturb *pt = (const perturb *)user;
    if (tick != pt->tick)
        return;
    u32 i;
    if (!de__body_resolve(c->physics, de_phys_det_kinematic_sweep_mover(c, pt->mover), &i))
        return;
    f64 x = c->physics->bodies.position[i].x;
    u64 bits;
    memcpy(&bits, &x, sizeof bits);
    ++bits; /* the next representable double away from zero (x > 0 or x < 0, never 0 here) */
    memcpy(&x, &bits, sizeof x);
    c->physics->bodies.position[i].x = x;
}

static u32 mover_index(u32 k) {
    de_phys_det_session s;
    if (de_phys_det_begin(&s, &de_phys_det_scenario_kinematic_sweep, NULL) != DE_OK)
        return 0xFFFFFFFFu;
    const de_body b = de_phys_det_kinematic_sweep_mover(&s.ctx, k);
    u32 i = 0xFFFFFFFFu;
    (void)de__body_resolve(s.ctx.physics, b, &i);
    de_phys_det_end(&s);
    return i;
}

TEST(harness, detects_one_ulp) {
    const de_phys_det_scenario *sc = &de_phys_det_scenario_kinematic_sweep;
    ASSERT_EQ(sc->ticks, KS_TICKS);
    ASSERT_EQ(de_phys_det_run(sc, NULL, g_base, NULL), DE_OK);
    const u32 index = mover_index(PERTURB_MOVER);
    ASSERT_NE(index, 0xFFFFFFFFu);

    perturb pt = {PERTURB_TICK, PERTURB_MOVER};
    static const u32 workers[2] = {0u, 4u};
    for (u32 w = 0; w < 2u; ++w) {
        de_phys_det_options o;
        memset(&o, 0, sizeof o);
        o.worker_count = workers[w];
        o.inject = inject_one_ulp;
        o.inject_user = &pt;
        ASSERT_EQ(de_phys_det_run(sc, &o, g_run, NULL), DE_OK);
        fprintf(stderr, "(expected) 1 ulp on mover %u at tick %u, workers %u:\n", PERTURB_MOVER,
                PERTURB_TICK, workers[w]);
        EXPECT_EQ(de_det_report("baseline vs perturbed", g_base, g_run, KS_TICKS), PERTURB_TICK);
        /* The movers integrate their own state, so the difference carries into the next ticks
         * (until the rounding of a later position update absorbs the one ulp). */
        EXPECT_NE(g_base[PERTURB_TICK + 1u], g_run[PERTURB_TICK + 1u]);

        de_phys_det_field_diff d;
        de_phys_det_options base;
        memset(&base, 0, sizeof base);
        EXPECT_GE(de_phys_det_diff_at("(expected) baseline vs perturbed", sc, &base, &o,
                                      PERTURB_TICK, &d),
                  1u);
        EXPECT_EQ(d.body, index);
        EXPECT_STREQ(d.field, "position.x");
        /* The tick before is identical. */
        EXPECT_EQ(
            de_phys_det_diff_at("before the perturbation", sc, &base, &o, PERTURB_TICK - 1u, &d),
            0u);
    }
}

TEST(harness, workers_field_level) {
    const de_phys_det_scenario *sc = &de_phys_det_scenario_kinematic_sweep;
    de_phys_det_options a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    b.worker_count = 4;
    de_phys_det_field_diff d;
    EXPECT_EQ(de_phys_det_diff_at("workers 0 vs 4", sc, &a, &b, sc->ticks - 1u, &d), 0u);
    EXPECT_EQ(d.differing, 0u);
}

TEST(harness, scenarios_collide) {
    de_phys_det_session s;
    ASSERT_EQ(de_phys_det_begin(&s, &de_phys_det_scenario_kinematic_sweep, NULL), DE_OK);
    for (u32 t = 0; t < KS_TICKS; ++t)
        ASSERT_EQ(de_phys_det_step(&s, &g_run[t], NULL), DE_OK);
    const u32 contact_ticks = de_phys_det_kinematic_sweep_contact_ticks(&s.ctx);
    de_phys_det_end(&s);
    fprintf(stderr, "kinematic_sweep: contacts on %u of %u ticks\n", contact_ticks, KS_TICKS);
    EXPECT_GE(contact_ticks, KS_TICKS / 4u);
    EXPECT_LT(contact_ticks, KS_TICKS); /* and free flight in between */

    const de_phys_det_scenario *hb = &de_phys_det_scenario_hull_builder;
    ASSERT_EQ(de_phys_det_begin(&s, hb, NULL), DE_OK);
    for (u32 t = 0; t < hb->ticks; ++t)
        ASSERT_EQ(de_phys_det_step(&s, &g_run[t], NULL), DE_OK);
    u32 rejected = 0;
    const u32 built = de_phys_det_hull_builder_counts(&s.ctx, &rejected);
    de_phys_det_end(&s);
    EXPECT_EQ(rejected, hb->ticks / 10u); /* every tenth cloud is planar */
    EXPECT_EQ(built + rejected, hb->ticks);
}

static de_result failing_setup(de_phys_det_ctx *c) {
    (void)c;
    return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "(expected) setup refuses");
}

TEST(harness, begin_errors) {
    de_phys_det_session s;
    EXPECT_EQ(de_phys_det_begin(&s, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    de_phys_det_scenario bad = de_phys_det_scenario_hull_builder;
    bad.ticks = 0;
    EXPECT_EQ(de_phys_det_begin(&s, &bad, NULL), DE_ERR_INVALID_ARGUMENT);
    bad = de_phys_det_scenario_hull_builder;
    bad.setup = NULL;
    EXPECT_EQ(de_phys_det_begin(&s, &bad, NULL), DE_ERR_INVALID_ARGUMENT);
    bad = de_phys_det_scenario_hull_builder;
    bad.setup = failing_setup;
    EXPECT_EQ(de_phys_det_begin(&s, &bad, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(s.ctx.physics == NULL); /* torn down and zeroed */
    de_phys_det_end(&s);                /* a zeroed session is ignored */
    EXPECT_EQ(de_phys_det_step(&s, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
}
