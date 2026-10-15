/*
 * test_two_phase.c: two-phase occlusion culling (DENG-233). Executable test_render_occlusion;
 * CTest names render_occlusion.two_phase.<test>_<backend>.
 *
 * Each frame the test predicts phase 1 (what was visible after the last frame, in the frustum;
 * nothing after a cut), ray casts those boxes into the phase-1 depth (occlusion_fixture.h), runs
 * the frame and checks that the GPU's phase-1 list is exactly the prediction. The scenes are
 * built so the occlusion results are not marginal: a wall that covers the whole view, boxes well
 * behind it, boxes well in front of it.
 *
 * Camera: at the origin, looking along +Y, 320 x 180, 1 rad vertical field of view. At 20 m the
 * view spans about +-19.4 m across and +-10.9 m up; the wall at y = 20 spans +-25 m and +-15 m.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "de_test.h"
#include "occlusion_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Slots of the wall scene. */
#define WALL       0u
#define FRONT_0    1u /* 4 boxes in front of the wall, visible */
#define BEHIND_0   5u /* 6 boxes behind the wall */
#define WALL_SLOTS 11u

static void wall_scene(ocx *f) {
    ocx_set_box(f, WALL, (de_vec3){0.0f, 20.0f, 0.0f}, (de_vec3){25.0f, 0.5f, 15.0f});
    for (u32 i = 0; i < 4; ++i)
        ocx_set_box(f, FRONT_0 + i, (de_vec3){-6.0f + 4.0f * (f32)i, 10.0f, -2.0f},
                    (de_vec3){0.5f, 0.5f, 0.5f});
    for (u32 i = 0; i < 6; ++i)
        ocx_set_box(f, BEHIND_0 + i, (de_vec3){-10.0f + 4.0f * (f32)i, 40.0f, 1.0f},
                    (de_vec3){1.0f, 1.0f, 1.0f});
}

/* The phase-1 set the GPU should draw this frame: visible after the last frame (its readback),
 * in the frustum; nothing after a cut. */
static void predict_phase1(ocx *f, bool cut, bool *draw) {
    const de__vis_cull_view v = ocx_cpu_view(f);
    const u32 n = f->w.instances.count;
    for (u32 s = 0; s < n; ++s) {
        const bool prev = !cut && s < f->visibility_count && f->visibility[s] != 0;
        draw[s] = prev && ocx_in_frustum(f, &v, s);
    }
}

/* One frame: predict and ray cast phase 1, run, check the phase-1 list against the prediction. */
static void step(ocx *f, bool cut) {
    bool *draw = (bool *)calloc(OCX_MAX_INSTANCES, sizeof(bool));
    ASSERT_TRUE(draw != NULL);
    predict_phase1(f, cut, draw);
    ocx_raycast(f, draw);
    const bool ok = ocx_frame(f);
    EXPECT_TRUE(ok);
    u32 wrong = 0;
    for (u32 s = 0; s < f->w.instances.count; ++s)
        if (ocx_drawn(f, 0, s) != draw[s]) {
            if (wrong++ < 4)
                printf("  slot %u: phase 1 %s, predicted %s\n", s,
                       ocx_drawn(f, 0, s) ? "drew it" : "did not", draw[s] ? "drawn" : "not");
        }
    EXPECT_EQ(wrong, 0u);
    free(draw);
}

static bool drawn_this_frame(const ocx *f, u32 s) {
    return ocx_drawn(f, 0, s) || ocx_drawn(f, 1, s);
}

/* two_phase_wall_reject: boxes fully behind the wall are rejected in phase 2 after one frame of
 * history, and not drawn at all from then on; the wall and the boxes in front stay in phase 1. */
static void wall_reject(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 64u);
    wall_scene(f);
    /* Frame 0: a new view, no history: phase 1 draws nothing, phase 2 everything (the HZB of an
     * empty depth hides nothing). */
    step(f, true);
    EXPECT_EQ(f->item_count[0], 0u);
    for (u32 s = 0; s < WALL_SLOTS; ++s)
        EXPECT_TRUE(ocx_drawn(f, 1, s));
    /* Frame 1: phase 1 draws all of them; phase 2 finds the boxes behind occluded. */
    step(f, false);
    EXPECT_EQ(f->item_count[1], 0u);
    for (u32 i = 0; i < 6; ++i)
        EXPECT_EQ(f->visibility[BEHIND_0 + i], 0u);
    EXPECT_EQ(f->visibility[WALL], 1u);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_EQ(f->visibility[FRONT_0 + i], 1u);
    /* Frames 2..4: the boxes behind are drawn in neither phase. */
    for (u32 k = 0; k < 3; ++k) {
        step(f, false);
        /* The stats read back while building this frame: the frame before. */
        const de__vis_stats st = de__vis_view_stats(f->vis, 0);
        EXPECT_TRUE(st.valid);
        EXPECT_EQ(st.frame, f->frames - 2u);
        EXPECT_EQ(st.instances_visible_phase1, k == 0 ? 11u : 5u);
        EXPECT_EQ(st.instances_visible_phase2, 0u);
        EXPECT_EQ(st.instances_visible, st.instances_visible_phase1);
        for (u32 i = 0; i < 6; ++i)
            EXPECT_FALSE(drawn_this_frame(f, BEHIND_0 + i));
        EXPECT_TRUE(ocx_drawn(f, 0, WALL));
        EXPECT_EQ(f->item_count[1], 0u);
        EXPECT_EQ(f->counters[1][DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], 0u);
        EXPECT_EQ(f->counters[0][DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], 5u);
    }
}

/* two_phase_reveal_no_pop: when the wall moves away, the boxes it hid are drawn in phase 2 of
 * the same frame (no frame without them); when it comes back they are drawn that frame (phase 1)
 * and rejected from the next. */
static void reveal_no_pop(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 64u);
    wall_scene(f);
    for (u32 k = 0; k < 3; ++k)
        step(f, k == 0);
    for (u32 i = 0; i < 6; ++i)
        ASSERT_FALSE(drawn_this_frame(f, BEHIND_0 + i));
    /* The wall moves far to the side: out of the frustum. */
    ocx_set_box(f, WALL, (de_vec3){200.0f, 20.0f, 0.0f}, (de_vec3){25.0f, 0.5f, 15.0f});
    step(f, false);
    for (u32 i = 0; i < 6; ++i) {
        EXPECT_TRUE(ocx_drawn(f, 1, BEHIND_0 + i)); /* revealed: phase 2, this frame */
        EXPECT_EQ(f->visibility[BEHIND_0 + i], 1u);
    }
    EXPECT_FALSE(drawn_this_frame(f, WALL));
    EXPECT_EQ(f->counters[1][DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], 6u);
    /* Next frame they are part of phase 1. */
    step(f, false);
    for (u32 i = 0; i < 6; ++i)
        EXPECT_TRUE(ocx_drawn(f, 0, BEHIND_0 + i));
    /* The wall comes back: drawn in phase 2 (it was not visible last frame); the boxes behind it
     * are still drawn this frame (phase 1), then rejected. */
    ocx_set_box(f, WALL, (de_vec3){0.0f, 20.0f, 0.0f}, (de_vec3){25.0f, 0.5f, 15.0f});
    step(f, false);
    EXPECT_TRUE(ocx_drawn(f, 1, WALL));
    for (u32 i = 0; i < 6; ++i)
        EXPECT_TRUE(ocx_drawn(f, 0, BEHIND_0 + i));
    step(f, false);
    step(f, false);
    for (u32 i = 0; i < 6; ++i)
        EXPECT_FALSE(drawn_this_frame(f, BEHIND_0 + i));
}

/* two_phase_camera_cut: r.occlusion_reset (one-shot), a camera jump and a sharp turn each drop
 * the history for one frame: phase 1 draws nothing and phase 2 tests everything. */
static void camera_cut(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 64u);
    wall_scene(f);
    for (u32 k = 0; k < 3; ++k)
        step(f, k == 0);
    ASSERT_GT(f->item_count[0], 0u);
    /* r.occlusion_reset. */
    ASSERT_TRUE(ocx_cvar("r.occlusion_reset", "1"));
    step(f, true);
    EXPECT_EQ(f->item_count[0], 0u);
    EXPECT_EQ(f->cv.visibility_count, 0u);
    EXPECT_EQ(f->counters[0][DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], 0u);
    for (u32 s = 0; s < WALL_SLOTS; ++s) /* everything in the frustum, the hidden boxes too */
        EXPECT_TRUE(ocx_drawn(f, 1, s));
    EXPECT_FALSE(de_cvar_bool(de_cvar_find("r.occlusion_reset"))); /* it cleared itself */
    step(f, false);
    EXPECT_GT(f->item_count[0], 0u); /* history again */
    /* A jump of 12 m (more than r.occlusion_cut_m = 10). */
    f->eye.x += 12.0f;
    f->target.x += 12.0f;
    step(f, true);
    EXPECT_EQ(f->item_count[0], 0u);
    step(f, false);
    EXPECT_GT(f->item_count[0], 0u);
    /* A small move is not a cut. */
    f->eye.x += 1.0f;
    f->target.x += 1.0f;
    step(f, false);
    EXPECT_GT(f->item_count[0], 0u);
    /* A turn of 60 degrees. */
    f->target = (de_vec3){f->eye.x + 0.866f, f->eye.y + 0.5f, f->eye.z};
    step(f, true);
    EXPECT_EQ(f->item_count[0], 0u);
}

/* two_phase_new_instances: instances added this frame (also past the visibility buffer's and
 * the instance table's capacity) are tested in phase 2: the ones in front are drawn, the ones
 * behind the wall are not; the history of the others survives the growth. */
static void new_instances(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 16u);
    wall_scene(f);
    for (u32 k = 0; k < 3; ++k)
        step(f, k == 0);
    /* 1600 new boxes: half at y = 8 (in front of everything: nothing hides them), half at
     * y = 45 (behind the wall). A 40 x 20 grid, x in [-6, 6), z in [-3, 3): inside the view. */
    const u32 first = WALL_SLOTS, added = 1600u;
    for (u32 i = 0; i < added; ++i) {
        const f32 x = -6.0f + 12.0f * (f32)(i % 40u) / 40.0f;
        const f32 z = -3.0f + 6.0f * (f32)((i / 40u) % 20u) / 20.0f;
        const bool front = i < added / 2u;
        ocx_set_box(f, first + i, (de_vec3){x, front ? 8.0f : 45.0f, z},
                    (de_vec3){0.05f, 0.05f, 0.05f});
    }
    step(f, false);
    u32 front_drawn = 0, behind_drawn = 0, in_phase1 = 0;
    for (u32 i = 0; i < added; ++i) {
        in_phase1 += ocx_drawn(f, 0, first + i);
        if (i < added / 2u)
            front_drawn += ocx_drawn(f, 1, first + i);
        else
            behind_drawn += drawn_this_frame(f, first + i);
    }
    EXPECT_EQ(in_phase1, 0u);
    EXPECT_EQ(front_drawn, added / 2u);
    EXPECT_EQ(behind_drawn, 0u);
    /* The old history survived the growth: the wall and the boxes in front are phase 1. */
    EXPECT_TRUE(ocx_drawn(f, 0, WALL));
    for (u32 i = 0; i < 4; ++i)
        EXPECT_TRUE(ocx_drawn(f, 0, FRONT_0 + i));
    for (u32 i = 0; i < 6; ++i)
        EXPECT_FALSE(drawn_this_frame(f, BEHIND_0 + i));
    /* Next frame the new front boxes are phase 1. */
    step(f, false);
    u32 now_phase1 = 0;
    for (u32 i = 0; i < added / 2u; ++i)
        now_phase1 += ocx_drawn(f, 0, first + i);
    EXPECT_EQ(now_phase1, added / 2u);
}

/* Occlusion off: phase 1 draws every frustum-visible box (also the hidden ones), phase 2 none. */
static void occlusion_off(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 64u);
    wall_scene(f);
    ASSERT_TRUE(ocx_cvar("r.occlusion", "0"));
    for (u32 k = 0; k < 3; ++k) {
        bool *draw = (bool *)calloc(OCX_MAX_INSTANCES, sizeof(bool));
        ASSERT_TRUE(draw != NULL);
        const de__vis_cull_view v = ocx_cpu_view(f);
        for (u32 s = 0; s < f->w.instances.count; ++s)
            draw[s] = ocx_in_frustum(f, &v, s);
        ocx_raycast(f, draw);
        free(draw);
        ASSERT_TRUE(ocx_frame(f));
        for (u32 s = 0; s < WALL_SLOTS; ++s)
            EXPECT_TRUE(ocx_drawn(f, 0, s));
        EXPECT_EQ(f->item_count[1], 0u);
    }
}

/* Split views keep their own history: view A looks at the wall (the boxes behind it are hidden
 * for A), view B stands behind them looking back (they are in front of B). Frames alternate
 * between the views; each one's phase 1 follows its own history. */
static void split_views(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 64u);
    wall_scene(f);
    const de_vec3 eye[2] = {{0.0f, 0.0f, 0.0f}, {0.0f, 70.0f, 1.0f}};
    const de_vec3 target[2] = {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    u32 *hist[2] = {NULL, NULL};
    u32 hist_count[2] = {0, 0};
    for (u32 k = 0; k < 8; ++k) {
        const u32 v = k % 2u;
        f->view_id = 1u + v;
        f->view_slot = v;
        f->eye = eye[v];
        f->target = target[v];
        /* The prediction reads this view's history, not the other view's last frame. */
        free(f->visibility);
        f->visibility = hist[v];
        f->visibility_count = hist_count[v];
        hist[v] = NULL;
        step(f, k < 2u);
        hist[v] = f->visibility;
        hist_count[v] = f->visibility_count;
        f->visibility = NULL;
        f->visibility_count = 0;
        if (k >= 4u) {
            for (u32 i = 0; i < 6; ++i) {
                if (v == 0u)
                    EXPECT_FALSE(drawn_this_frame(f, BEHIND_0 + i));
                else
                    EXPECT_TRUE(ocx_drawn(f, 0, BEHIND_0 + i)); /* B's history: visible */
            }
        }
    }
    free(hist[0]);
    free(hist[1]);
}

#define TWO_PHASE_TESTS(sfx, backend)                                                              \
    TEST_F(two_phase, wall_reject_##sfx, ocx) {                                                    \
        wall_reject(f, backend);                                                                   \
    }                                                                                              \
    TEST_F(two_phase, reveal_no_pop_##sfx, ocx) {                                                  \
        reveal_no_pop(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(two_phase, camera_cut_##sfx, ocx) {                                                     \
        camera_cut(f, backend);                                                                    \
    }                                                                                              \
    TEST_F(two_phase, new_instances_##sfx, ocx) {                                                  \
        new_instances(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(two_phase, occlusion_off_##sfx, ocx) {                                                  \
        occlusion_off(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(two_phase, split_views_##sfx, ocx) {                                                    \
        split_views(f, backend);                                                                   \
    }

TWO_PHASE_TESTS(metal, DE_GPU_METAL)
TWO_PHASE_TESTS(vulkan, DE_GPU_VULKAN)
TWO_PHASE_TESTS(d3d12, DE_GPU_D3D12)
