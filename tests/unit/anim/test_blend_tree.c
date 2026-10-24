#include "de_test.h"
#include "de_alloc_guard.h"
#include "anim/blend_tree.h"
#include <dengine/de_task.h>
#include <math.h>
#include <string.h>

static f32 node_weight(const de__anim_blend_weights *w, u32 node) {
    f32 value = 0;
    for (u32 i = 0; i < w->count; ++i)
        if (w->node[i] == node)
            value += w->weight[i];
    return value;
}
static void weight_invariants(const de__anim_blend_weights *w) {
    ASSERT_TRUE(w->count > 0 && w->count <= 3);
    f64 sum = 0;
    for (u32 i = 0; i < w->count; ++i) {
        EXPECT_TRUE(w->weight[i] > 0 && w->weight[i] <= 1);
        sum += w->weight[i];
    }
    EXPECT_TRUE(fabs(sum - 1) <= 1e-6);
}
TEST(anim_blend_tree, one_dimensional_walk_run_clamp_and_exact_points) {
    const de__anim_blend_point points[] = {{{4, 0}, 1}, {{0, 0}, 0}};
    de__anim_blend_space *s = NULL;
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 1, points, 2, &s), DE_OK);
    de__anim_blend_weights w;
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){2, 0}, &w));
    EXPECT_TRUE(node_weight(&w, 0) == .5f && node_weight(&w, 1) == .5f);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){-1, 0}, &w));
    EXPECT_TRUE(w.count == 1 && w.node[0] == 0 && w.weight[0] == 1);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){10, 0}, &w));
    EXPECT_TRUE(w.count == 1 && w.node[0] == 1 && w.weight[0] == 1);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){4, 0}, &w));
    EXPECT_TRUE(w.count == 1 && w.node[0] == 1 && w.weight[0] == 1);
    de__anim_blend_space_destroy(s);
}
TEST(anim_blend_tree, triangle_barycentrics_and_nearest_hull_edge_projection) {
    const de__anim_blend_point points[] = {{{0, 0}, 0}, {{4, 0}, 1}, {{0, 4}, 2}};
    de__anim_blend_space *s = NULL;
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, points, 3, &s), DE_OK);
    EXPECT_EQ(s->triangle_count, 1u);
    de__anim_blend_weights w;
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){1, 1}, &w));
    weight_invariants(&w);
    EXPECT_TRUE(node_weight(&w, 0) == .5f && node_weight(&w, 1) == .25f &&
                node_weight(&w, 2) == .25f);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){3, 3}, &w));
    EXPECT_TRUE(node_weight(&w, 0) == 0 && node_weight(&w, 1) == .5f && node_weight(&w, 2) == .5f);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){-1, 1}, &w));
    EXPECT_TRUE(node_weight(&w, 0) == .75f && node_weight(&w, 2) == .25f);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){-5, -5}, &w));
    EXPECT_TRUE(w.count == 1 && w.node[0] == 0 && w.weight[0] == 1);
    de__anim_blend_space_destroy(s);
}
TEST(anim_blend_tree, single_point_collinear_and_subdivided_hull_samples) {
    const de__anim_blend_point line[] = {{{0, 0}, 0}, {{0, 2}, 1}, {{0, 6}, 2}};
    de__anim_blend_space *s = NULL;
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, line, 1, &s), DE_OK);
    de__anim_blend_weights w;
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){100, -2}, &w));
    EXPECT_TRUE(w.count == 1 && w.node[0] == 0 && w.weight[0] == 1);
    de__anim_blend_space_destroy(s);
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, line, 3, &s), DE_OK);
    EXPECT_TRUE(s->collinear && s->triangle_count == 0);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){4, 3}, &w));
    EXPECT_TRUE(node_weight(&w, 0) == 0 && node_weight(&w, 1) == .75f &&
                node_weight(&w, 2) == .25f);
    de__anim_blend_space_destroy(s);
    const de__anim_blend_point square[] = {{{0, 0}, 0}, {{2, 0}, 1}, {{4, 0}, 2},
                                           {{4, 4}, 3}, {{0, 4}, 4}, {{2, 2}, 5}};
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, square, 6, &s), DE_OK);
    EXPECT_EQ(s->triangle_count, 5u);
    EXPECT_EQ(s->hull_count, 5u);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){1, -1}, &w));
    EXPECT_TRUE(node_weight(&w, 0) == .5f && node_weight(&w, 1) == .5f);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){2, 2}, &w));
    EXPECT_TRUE(w.count == 1 && w.node[0] == 5 && w.weight[0] == 1);
    de__anim_blend_space_destroy(s);
}
/* Independent circumcenter oracle, intentionally not the runtime incircle
 * determinant. With integer grid inputs its positive margin is well resolved. */
static void assert_delaunay(const de__anim_blend_space *s) {
    for (u32 i = 0; i < s->triangle_count; ++i) {
        const de__anim_blend_triangle *t = &s->triangles[i];
        de_vec2 a = s->points[t->point[0]].position;
        de_vec2 b = s->points[t->point[1]].position;
        de_vec2 c = s->points[t->point[2]].position;
        long double bx = (long double)b.x - a.x, by = (long double)b.y - a.y;
        long double cx = (long double)c.x - a.x, cy = (long double)c.y - a.y;
        long double d = 2 * (bx * cy - by * cx);
        ASSERT_TRUE(d > 0);
        long double ub = bx * bx + by * by, uc = cx * cx + cy * cy;
        long double x = (cy * ub - by * uc) / d, y = (bx * uc - cx * ub) / d;
        long double radius = x * x + y * y;
        for (u32 p = 0; p < s->point_count; ++p) {
            long double px = (long double)s->points[p].position.x - a.x - x;
            long double py = (long double)s->points[p].position.y - a.y - y;
            EXPECT_TRUE(px * px + py * py >= radius - 1e-8L);
        }
    }
}
TEST(anim_blend_tree, delaunay_grid_interior_vertices_cocircular_ties_and_input_permutations) {
    de__anim_blend_point points[25], reversed[25];
    for (u32 i = 0; i < 25; ++i)
        points[i] = (de__anim_blend_point){{(f32)(i % 5), (f32)(i / 5)}, i};
    for (u32 i = 0; i < 25; ++i)
        reversed[i] = points[24 - i];
    de__anim_blend_space *a = NULL, *b = NULL;
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, points, 25, &a), DE_OK);
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, reversed, 25, &b), DE_OK);
    EXPECT_EQ(a->triangle_count, 32u);
    EXPECT_EQ(a->triangle_count, b->triangle_count);
    EXPECT_MEMEQ(a->triangles, b->triangles, a->triangle_count * sizeof *a->triangles);
    assert_delaunay(a);
    for (u32 i = 0; i < 25; ++i) {
        de__anim_blend_weights w;
        ASSERT_TRUE(de__anim_blend_space_weights(a, points[i].position, &w));
        EXPECT_TRUE(w.count == 1 && w.node[0] == i && w.weight[0] == 1);
    }
    for (u32 i = 0; i < 1000; ++i) {
        de_vec2 query = {(f32)(i % 31) / 7 - 1, (f32)((i * 7) % 37) / 9 - 1};
        de__anim_blend_weights x, y;
        ASSERT_TRUE(de__anim_blend_space_weights(a, query, &x));
        ASSERT_TRUE(de__anim_blend_space_weights(b, query, &y));
        weight_invariants(&x);
        EXPECT_MEMEQ(&x, &y, sizeof x);
        /* A linear field must interpolate to the query inside the square and
         * its independently clamped nearest point outside. */
        f64 px = 0, py = 0;
        for (u32 j = 0; j < x.count; ++j) {
            px += (f64)x.weight[j] * points[x.node[j]].position.x;
            py += (f64)x.weight[j] * points[x.node[j]].position.y;
        }
        EXPECT_TRUE(fabs(px - fmin(4, fmax(0, query.x))) < 1e-6);
        EXPECT_TRUE(fabs(py - fmin(4, fmax(0, query.y))) < 1e-6);
    }
    de__anim_blend_space_destroy(a);
    de__anim_blend_space_destroy(b);
}
TEST(anim_blend_tree, thin_and_extreme_scale_triangles_retain_real_hull) {
    const f32 scales[] = {1e-30f, 1, 1e30f};
    for (u32 i = 0; i < 3; ++i) {
        f32 scale = scales[i];
        de__anim_blend_point points[] = {
            {{0, 0}, 0}, {{4 * scale, 0}, 1}, {{2 * scale, 1e-6f * scale}, 2}};
        de__anim_blend_space *s = NULL;
        ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, points, 3, &s), DE_OK);
        EXPECT_EQ(s->triangle_count, 1u);
        de__anim_blend_weights w;
        ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){2 * scale, .5e-6f * scale}, &w));
        weight_invariants(&w);
        EXPECT_TRUE(fabsf(node_weight(&w, 0) - .25f) < 1e-6f);
        EXPECT_TRUE(fabsf(node_weight(&w, 1) - .25f) < 1e-6f);
        EXPECT_TRUE(fabsf(node_weight(&w, 2) - .5f) < 1e-6f);
        de__anim_blend_space_destroy(s);
    }
}
TEST(anim_blend_tree, non_delaunay_fan_flips_and_varied_point_sets_have_empty_circumcircles) {
    const de__anim_blend_point quad[] = {{{0, 0}, 0}, {{4, 0}, 1}, {{4, 4}, 2}, {{0, 1}, 3}};
    de__anim_blend_space *s = NULL;
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, quad, 4, &s), DE_OK);
    EXPECT_EQ(s->triangle_count, 2u);
    /* The initial hull fan uses 0--2. Its circle contains vertex3, so the
     * final Delaunay diagonal must be 1--3. */
    for (u32 i = 0; i < s->triangle_count; ++i) {
        bool has_one = false, has_three = false;
        for (u32 j = 0; j < 3; ++j) {
            u32 node = s->points[s->triangles[i].point[j]].node;
            has_one |= node == 1;
            has_three |= node == 3;
        }
        EXPECT_TRUE(has_one && has_three);
    }
    assert_delaunay(s);
    de__anim_blend_space_destroy(s);
    for (u32 seed = 0; seed < 12; ++seed) {
        de__anim_blend_point points[32];
        for (u32 i = 0; i < 32; ++i) {
            points[i] =
                (de__anim_blend_point){{(f32)((i * 37 + seed * 19) % 131),
                                        (f32)((i * i * 17 + seed * i * 7 + seed * 29) % 127)},
                                       i};
        }
        ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, points, 32, &s), DE_OK);
        EXPECT_EQ(s->triangle_count, 2 * s->point_count - s->hull_count - 2);
        assert_delaunay(s);
        for (u32 i = 0; i < 32; ++i) {
            de__anim_blend_weights w;
            ASSERT_TRUE(de__anim_blend_space_weights(s, points[i].position, &w));
            EXPECT_TRUE(w.count == 1 && w.node[0] == i && w.weight[0] == 1);
        }
        de__anim_blend_space_destroy(s);
    }
}
TEST(anim_blend_tree, tiny_interior_point_is_retained_alongside_large_coordinates) {
    const de__anim_blend_point points[] = {
        {{0, 0}, 0}, {{1e30f, 0}, 1}, {{0, 1e30f}, 2}, {{1e-30f, 1e-30f}, 3}};
    de__anim_blend_space *s = NULL;
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, points, 4, &s), DE_OK);
    EXPECT_EQ(s->triangle_count, 3u);
    de__anim_blend_weights w;
    ASSERT_TRUE(de__anim_blend_space_weights(s, points[3].position, &w));
    EXPECT_TRUE(w.count == 1 && w.node[0] == 3 && w.weight[0] == 1);
    de__anim_blend_space_destroy(s);
}
typedef struct space_queries {
    const de__anim_blend_space *space;
    de__anim_blend_weights *output;
} space_queries;
static void evaluate_queries(u32 begin, u32 end, void *user) {
    space_queries *q = user;
    for (u32 i = begin; i < end; ++i) {
        de_vec2 p = {(f32)(i % 67) / 13 - 1, (f32)((i * 11) % 71) / 17 - 1};
        if (!de__anim_blend_space_weights(q->space, p, &q->output[i]))
            q->output[i].count = 0;
    }
}
TEST(anim_blend_tree, immutable_space_has_identical_serial_and_parallel_weights) {
    de_fp_env_init_thread();
    const de__anim_blend_point points[] = {{{0, 0}, 0}, {{4, 0}, 1}, {{4, 4}, 2},
                                           {{0, 4}, 3}, {{1, 2}, 4}, {{3, 1}, 5}};
    de__anim_blend_space *s = NULL;
    ASSERT_EQ(de__anim_blend_space_create(de_heap_allocator(), 2, points, 6, &s), DE_OK);
    de_task_system *tasks = NULL;
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.allocator = de_heap_allocator(),
                                                           .worker_count = 3,
                                                           .background_worker_count = 1},
                                    &tasks),
              DE_OK);
    de__anim_blend_weights serial[1024], parallel[1024];
    space_queries q = {s, serial};
    evaluate_queries(0, 1024, &q);
    q.output = parallel;
    de_task_parallel_for(tasks, 1024, 32, evaluate_queries, &q);
    for (u32 i = 0; i < 1024; ++i) {
        weight_invariants(&serial[i]);
        EXPECT_MEMEQ(&serial[i], &parallel[i], sizeof serial[i]);
    }
    de_task_system_destroy(tasks);
    de__anim_blend_space_destroy(s);
}
TEST(anim_blend_tree, rejects_bad_points_cleans_up_and_queries_allocate_nothing) {
    de_tracking_allocator tracking = {0};
    de_allocator a = de_tracking_allocator_init(&tracking, de_heap_allocator(), DE_MEM_GENERAL);
    de__anim_blend_point points[] = {{{0, 0}, 0}, {{2, 0}, 1}, {{0, 2}, 2}};
    de__anim_blend_space *s = NULL;
    EXPECT_EQ(de__anim_blend_space_create(de_allocator_null(), 2, points, 3, &s),
              DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(s == NULL);
    points[1].position = (de_vec2){-0.f, 0};
    EXPECT_EQ(de__anim_blend_space_create(&a, 2, points, 3, &s), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
    points[1].position = (de_vec2){0, 1};
    EXPECT_EQ(de__anim_blend_space_create(&a, 1, points, 3, &s), DE_ERR_INVALID_ARGUMENT);
    points[1].position.x = NAN;
    EXPECT_EQ(de__anim_blend_space_create(&a, 2, points, 3, &s), DE_ERR_INVALID_ARGUMENT);
    points[1].position = (de_vec2){2, 0};
    ASSERT_EQ(de__anim_blend_space_create(&a, 2, points, 3, &s), DE_OK);
    de__anim_blend_weights w = {.count = 17}, before = w;
    EXPECT_FALSE(de__anim_blend_space_weights(s, (de_vec2){INFINITY, 0}, &w));
    EXPECT_MEMEQ(&w, &before, sizeof w);
    ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){1, 0}, &w));
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 1000; ++i)
        ASSERT_TRUE(de__anim_blend_space_weights(s, (de_vec2){.25f, .5f}, &w));
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    de__anim_blend_space_destroy(s);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
}
