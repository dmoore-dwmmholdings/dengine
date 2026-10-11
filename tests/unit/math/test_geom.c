/* test_geom.c: de_aabb, de_plane, de_ray and de_frustum (DENG-241). */
#include "de_test.h"

#include <dengine/de_math.h>

#include <float.h>
#include <math.h>

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 r = {x, y, z};
    return r;
}
static de_aabb box(de_vec3 c, f32 half) {
    de_aabb b = {v3(c.x - half, c.y - half, c.z - half), v3(c.x + half, c.y + half, c.z + half)};
    return b;
}
static de_ray ray(de_vec3 o, de_vec3 d) {
    de_ray r = {o, d};
    return r;
}

#define EXPECT_V3_NEAR(a, b, eps)                                                                  \
    do {                                                                                           \
        de_vec3 a_ = (a), b_ = (b);                                                                \
        EXPECT_FLOAT_EQ(a_.x, b_.x, eps);                                                          \
        EXPECT_FLOAT_EQ(a_.y, b_.y, eps);                                                          \
        EXPECT_FLOAT_EQ(a_.z, b_.z, eps);                                                          \
    } while (0)

static u32 g_rng = 0x6C078965u;
static f32 rnd(f32 lo, f32 hi) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return lo + (hi - lo) * (f32)(g_rng >> 8) * (1.0f / 16777216.0f);
}
static de_vec3 rnd_v3(f32 lo, f32 hi) {
    return v3(rnd(lo, hi), rnd(lo, hi), rnd(lo, hi));
}

/* ---- de_aabb ---- */

TEST(geom, aabb_build) {
    de_aabb e = de_aabb_empty();
    EXPECT_TRUE(de_aabb_is_empty(e));
    EXPECT_FALSE(de_aabb_contains(e, v3(0, 0, 0)));
    EXPECT_TRUE(de_aabb_is_empty(de_aabb_from_points(NULL, 3)));
    EXPECT_TRUE(de_aabb_is_empty(de_aabb_from_points(NULL, 0)));

    const de_vec3 pts[4] = {v3(1, -2, 3), v3(-1, 5, 0), v3(0, 0, 7), v3(2, 1, -4)};
    de_aabb b = de_aabb_from_points(pts, 4);
    EXPECT_FALSE(de_aabb_is_empty(b));
    EXPECT_V3_NEAR(b.min, v3(-1, -2, -4), 0);
    EXPECT_V3_NEAR(b.max, v3(2, 5, 7), 0);
    EXPECT_V3_NEAR(de_aabb_center(b), v3(0.5f, 1.5f, 1.5f), 0);
    EXPECT_V3_NEAR(de_aabb_extents(b), v3(1.5f, 3.5f, 5.5f), 0);

    de_aabb one = de_aabb_add_point(de_aabb_empty(), v3(3, 3, 3));
    EXPECT_V3_NEAR(one.min, v3(3, 3, 3), 0);
    EXPECT_V3_NEAR(one.max, v3(3, 3, 3), 0);
    EXPECT_FALSE(de_aabb_is_empty(one));

    de_aabb m = de_aabb_merge(b, one);
    EXPECT_V3_NEAR(m.min, v3(-1, -2, -4), 0);
    EXPECT_V3_NEAR(m.max, v3(3, 5, 7), 0);
    de_aabb me = de_aabb_merge(de_aabb_empty(), b); /* empty is the identity of merge */
    EXPECT_MEMEQ(&me, &b, sizeof b);
}

TEST(geom, aabb_contains_intersects) {
    de_aabb a = box(v3(0, 0, 0), 1);
    EXPECT_TRUE(de_aabb_contains(a, v3(0, 0, 0)));
    EXPECT_TRUE(de_aabb_contains(a, v3(1, -1, 1))); /* boundary */
    EXPECT_FALSE(de_aabb_contains(a, v3(1.01f, 0, 0)));
    EXPECT_FALSE(de_aabb_contains(a, v3(0, 0, -1.01f)));

    EXPECT_TRUE(de_aabb_intersects(a, box(v3(1.5f, 0, 0), 1)));
    EXPECT_TRUE(de_aabb_intersects(a, box(v3(2, 0, 0), 1))); /* touching */
    EXPECT_FALSE(de_aabb_intersects(a, box(v3(2.1f, 0, 0), 1)));
    EXPECT_FALSE(de_aabb_intersects(a, box(v3(0, 0, -3), 1)));
    EXPECT_TRUE(de_aabb_intersects(a, box(v3(0, 0, 0), 0.1f))); /* nested */
    EXPECT_FALSE(de_aabb_intersects(a, de_aabb_empty()));
    EXPECT_FALSE(de_aabb_intersects(de_aabb_empty(), de_aabb_empty()));
}

TEST(geom, aabb_transform) {
    /* 90 degrees about Z: x extent and y extent swap; then translate. */
    de_aabb b = {v3(-1, -2, -3), v3(1, 2, 3)};
    de_affine a =
        de_affine_from_trs(v3(10, 0, 0), de_quat_axis_angle(DE_AXIS_UP, DE_PI / 2), v3(1, 1, 1));
    de_aabb t = de_aabb_transform(b, &a);
    EXPECT_V3_NEAR(t.min, v3(8, -1, -3), 1e-5);
    EXPECT_V3_NEAR(t.max, v3(12, 1, 3), 1e-5);
    /* 45 degrees: the unit cube's x/y extent grows to sqrt(2). */
    de_affine r45 =
        de_affine_from_trs(v3(0, 0, 0), de_quat_axis_angle(DE_AXIS_UP, DE_PI / 4), v3(1, 1, 1));
    de_aabb c = de_aabb_transform(box(v3(0, 0, 0), 1), &r45);
    EXPECT_V3_NEAR(c.max, v3(sqrtf(2), sqrtf(2), 1), 1e-5);
    /* Random: the result holds every transformed corner, and is tight (touches on each side). */
    for (int i = 0; i < 200; ++i) {
        de_aabb src = de_aabb_from_points((de_vec3[2]){rnd_v3(-5, 5), rnd_v3(-5, 5)}, 2);
        de_affine m = de_affine_from_trs(
            rnd_v3(-10, 10),
            de_quat_axis_angle(de_v3_normalize(rnd_v3(0.1f, 1)), rnd(-DE_PI, DE_PI)),
            rnd_v3(0.5f, 2));
        de_aabb dst = de_aabb_transform(src, &m);
        de_aabb corners = de_aabb_empty();
        for (int k = 0; k < 8; ++k) {
            de_vec3 p = v3(k & 1 ? src.max.x : src.min.x, k & 2 ? src.max.y : src.min.y,
                           k & 4 ? src.max.z : src.min.z);
            corners = de_aabb_add_point(corners, de_affine_transform_point(&m, p));
        }
        EXPECT_V3_NEAR(dst.min, corners.min, 1e-3);
        EXPECT_V3_NEAR(dst.max, corners.max, 1e-3);
    }
    EXPECT_TRUE(de_aabb_is_empty(de_aabb_transform(de_aabb_empty(), &a)));
}

/* ---- de_plane ---- */

TEST(geom, plane) {
    de_plane p = de_plane_from_point_normal(v3(0, 0, 5), v3(0, 0, 2));
    EXPECT_V3_NEAR(p.n, v3(0, 0, 1), 0);
    EXPECT_FLOAT_EQ(p.d, -5, 0);
    EXPECT_FLOAT_EQ(de_plane_distance(p, v3(3, 4, 8)), 3, 1e-6);
    EXPECT_FLOAT_EQ(de_plane_distance(p, v3(3, 4, 2)), -3, 1e-6);
    de_plane raw = {v3(0, 3, 4), 10};
    de_plane n = de_plane_normalize(raw);
    EXPECT_V3_NEAR(n.n, v3(0, 0.6f, 0.8f), 1e-6);
    EXPECT_FLOAT_EQ(n.d, 2, 1e-6);
    de_plane zero = {v3(0, 0, 0), 1};
    de_plane z2 = de_plane_normalize(zero);
    EXPECT_MEMEQ(&z2, &zero, sizeof zero);
}

/* ---- de_ray ---- */

TEST(geom, ray_aabb) {
    de_aabb b = box(v3(0, 10, 0), 1);
    f32 t = -1;
    EXPECT_TRUE(de_ray_aabb(ray(v3(0, 0, 0), DE_AXIS_FORWARD), b, &t));
    EXPECT_FLOAT_EQ(t, 9, 1e-6);
    EXPECT_TRUE(de_ray_aabb(ray(v3(0, 0, 0), v3(0, 2, 0)), b, &t)); /* t in units of dir */
    EXPECT_FLOAT_EQ(t, 4.5f, 1e-6);
    EXPECT_FALSE(de_ray_aabb(ray(v3(0, 0, 0), v3(0, -1, 0)), b, &t)); /* behind */
    EXPECT_FALSE(de_ray_aabb(ray(v3(0, 0, 0), DE_AXIS_UP), b, NULL));
    EXPECT_TRUE(de_ray_aabb(ray(v3(0, 10, 0), DE_AXIS_UP), b, &t)); /* inside */
    EXPECT_FLOAT_EQ(t, 0, 0);
    /* Zero dir components: inside or outside the slab. */
    EXPECT_TRUE(de_ray_aabb(ray(v3(0.5f, 0, -0.5f), DE_AXIS_FORWARD), b, NULL));
    EXPECT_FALSE(de_ray_aabb(ray(v3(1.5f, 0, 0), DE_AXIS_FORWARD), b, NULL));
    EXPECT_TRUE(de_ray_aabb(ray(v3(1, 0, 1), DE_AXIS_FORWARD), b, NULL)); /* on the edge */
    /* Diagonal. */
    EXPECT_TRUE(de_ray_aabb(ray(v3(-5, 5, 0), v3(1, 1, 0)), b, &t));
    EXPECT_FLOAT_EQ(t, 4, 1e-5);
    EXPECT_FALSE(de_ray_aabb(ray(v3(-5, 5, 0), v3(1, -1, 0)), b, NULL));
    EXPECT_FALSE(de_ray_aabb(ray(v3(0, 0, 0), DE_AXIS_FORWARD), de_aabb_empty(), NULL));
}

TEST(geom, ray_plane) {
    de_plane ground = de_plane_from_point_normal(v3(0, 0, 0), DE_AXIS_UP);
    f32 t = -1;
    EXPECT_TRUE(de_ray_plane(ray(v3(1, 2, 10), v3(0, 0, -2)), ground, &t));
    EXPECT_FLOAT_EQ(t, 5, 1e-6);
    EXPECT_TRUE(de_ray_plane(ray(v3(0, 0, -3), DE_AXIS_UP), ground, &t)); /* from behind */
    EXPECT_FLOAT_EQ(t, 3, 1e-6);
    EXPECT_FALSE(de_ray_plane(ray(v3(0, 0, 10), DE_AXIS_UP), ground, &t));    /* away */
    EXPECT_FALSE(de_ray_plane(ray(v3(0, 0, 10), DE_AXIS_RIGHT), ground, &t)); /* parallel */
}

TEST(geom, ray_triangle) {
    de_vec3 a = v3(0, 5, 0), b = v3(2, 5, 0), c = v3(0, 5, 2);
    f32 t = -1;
    de_vec2 uv = {-1, -1};
    EXPECT_TRUE(de_ray_triangle(ray(v3(0.5f, 0, 0.5f), DE_AXIS_FORWARD), a, b, c, &t, &uv));
    EXPECT_FLOAT_EQ(t, 5, 1e-6);
    EXPECT_FLOAT_EQ(uv.x, 0.25f, 1e-6);
    EXPECT_FLOAT_EQ(uv.y, 0.25f, 1e-6);
    /* Hit point = a + u (b - a) + v (c - a). */
    de_vec3 hit = de_v3_add(
        a, de_v3_add(de_v3_scale(de_v3_sub(b, a), uv.x), de_v3_scale(de_v3_sub(c, a), uv.y)));
    EXPECT_V3_NEAR(hit, v3(0.5f, 5, 0.5f), 1e-6);
    /* Both faces. */
    EXPECT_TRUE(de_ray_triangle(ray(v3(0.5f, 10, 0.5f), v3(0, -1, 0)), a, b, c, &t, NULL));
    EXPECT_FLOAT_EQ(t, 5, 1e-6);
    /* Misses: outside u, outside v, outside u + v, behind, parallel, degenerate. */
    EXPECT_FALSE(de_ray_triangle(ray(v3(-0.1f, 0, 0.5f), DE_AXIS_FORWARD), a, b, c, NULL, NULL));
    EXPECT_FALSE(de_ray_triangle(ray(v3(0.5f, 0, -0.1f), DE_AXIS_FORWARD), a, b, c, NULL, NULL));
    EXPECT_FALSE(de_ray_triangle(ray(v3(1.1f, 0, 1.1f), DE_AXIS_FORWARD), a, b, c, NULL, NULL));
    EXPECT_FALSE(de_ray_triangle(ray(v3(0.5f, 6, 0.5f), DE_AXIS_FORWARD), a, b, c, NULL, NULL));
    EXPECT_FALSE(de_ray_triangle(ray(v3(-1, 5, 0.5f), DE_AXIS_RIGHT), a, b, c, NULL, NULL));
    EXPECT_FALSE(
        de_ray_triangle(ray(v3(0.5f, 0, 0.5f), DE_AXIS_FORWARD), a, b, v3(4, 5, 0), NULL, NULL));
    /* Random rays through random interior points hit at the expected t and uv. */
    for (int i = 0; i < 500; ++i) {
        de_vec3 p0 = rnd_v3(-10, 10), p1 = rnd_v3(-10, 10), p2 = rnd_v3(-10, 10);
        f32 u = rnd(0.05f, 0.9f), v = rnd(0.05f, 0.95f - u);
        de_vec3 target = de_v3_add(
            p0, de_v3_add(de_v3_scale(de_v3_sub(p1, p0), u), de_v3_scale(de_v3_sub(p2, p0), v)));
        de_vec3 n = de_v3_cross(de_v3_sub(p1, p0), de_v3_sub(p2, p0));
        if (de_v3_length(n) < 1.0f)
            continue; /* skip slivers */
        de_vec3 origin = de_v3_add(target, de_v3_scale(de_v3_normalize(n), 7));
        de_vec3 dir = de_v3_normalize(de_v3_sub(target, origin));
        ASSERT_TRUE(de_ray_triangle(ray(origin, dir), p0, p1, p2, &t, &uv));
        EXPECT_FLOAT_EQ(t, 7, 1e-3);
        EXPECT_FLOAT_EQ(uv.x, u, 1e-3);
        EXPECT_FLOAT_EQ(uv.y, v, 1e-3);
    }
}

/* ---- de_frustum ---- */

typedef struct cam_fx {
    de_mat4 vp;
    de_frustum f;
} cam_fx;

/* Camera at the origin looking along world +Y, up +Z, 90 degree fov, aspect 1, near 0.1:
 * the visible region is y > 0.1, |x| < y, |z| < y. */
static void cam_fx_setup(cam_fx *fx) {
    de_mat4 view = de_m4_look_at(v3(0, 0, 0), v3(0, 1, 0), DE_AXIS_UP);
    de_mat4 proj = de_m4_perspective_rz(DE_PI / 2, 1.0f, 0.1f);
    fx->vp = de_m4_mul(&proj, &view);
    fx->f = de_frustum_from_m4(&fx->vp);
}
static void cam_fx_teardown(cam_fx *fx) {
    (void)fx;
}

/* The plane that culls box b, or -1 when no plane does. */
static int culling_plane(const de_frustum *f, de_aabb b) {
    for (int i = 0; i < 6; ++i) {
        const de_plane *p = &f->planes[i];
        de_vec3 pv = v3(p->n.x >= 0 ? b.max.x : b.min.x, p->n.y >= 0 ? b.max.y : b.min.y,
                        p->n.z >= 0 ? b.max.z : b.min.z);
        if (de_plane_distance(*p, pv) < 0)
            return i;
    }
    return -1;
}

TEST_F(geom, frustum_planes, cam_fx) {
    const de_frustum *fr = &f->f;
    for (int i = 0; i < 5; ++i)
        EXPECT_FLOAT_EQ(de_v3_length(fr->planes[i].n), 1, 1e-6);
    /* Inward normals in world space. */
    EXPECT_V3_NEAR(fr->planes[DE_FRUSTUM_LEFT].n, v3(sqrtf(0.5f), sqrtf(0.5f), 0), 1e-6);
    EXPECT_V3_NEAR(fr->planes[DE_FRUSTUM_RIGHT].n, v3(-sqrtf(0.5f), sqrtf(0.5f), 0), 1e-6);
    EXPECT_V3_NEAR(fr->planes[DE_FRUSTUM_BOTTOM].n, v3(0, sqrtf(0.5f), sqrtf(0.5f)), 1e-6);
    EXPECT_V3_NEAR(fr->planes[DE_FRUSTUM_TOP].n, v3(0, sqrtf(0.5f), -sqrtf(0.5f)), 1e-6);
    EXPECT_V3_NEAR(fr->planes[DE_FRUSTUM_NEAR].n, v3(0, 1, 0), 1e-6);
    EXPECT_FLOAT_EQ(fr->planes[DE_FRUSTUM_NEAR].d, -0.1f, 1e-6);
    /* Infinite far: degenerate row stored as always-pass. */
    EXPECT_V3_NEAR(fr->planes[DE_FRUSTUM_FAR].n, v3(0, 0, 0), 0);
    EXPECT_FLOAT_EQ(fr->planes[DE_FRUSTUM_FAR].d, FLT_MAX, 0);
}

TEST_F(geom, frustum_cull_sides, cam_fx) {
    const de_frustum *fr = &f->f;
    /* Inside. */
    EXPECT_TRUE(de_frustum_test_aabb(fr, box(v3(0, 10, 0), 1)));
    EXPECT_EQ(culling_plane(fr, box(v3(0, 10, 0), 1)), -1);
    /* One box beyond each of the five real planes, culled by exactly that plane. */
    struct {
        de_vec3 c;
        f32 half;
        int plane;
    } out[] = {
        {{-20, 10, 0}, 1, DE_FRUSTUM_LEFT},      {{20, 10, 0}, 1, DE_FRUSTUM_RIGHT},
        {{0, 10, -20}, 1, DE_FRUSTUM_BOTTOM},    {{0, 10, 20}, 1, DE_FRUSTUM_TOP},
        {{0, 0.05f, 0}, 0.01f, DE_FRUSTUM_NEAR}, /* between the camera and the near plane */
    };
    for (size_t i = 0; i < sizeof out / sizeof out[0]; ++i) {
        de_aabb b = box(out[i].c, out[i].half);
        EXPECT_FALSE(de_frustum_test_aabb(fr, b));
        EXPECT_EQ(culling_plane(fr, b), out[i].plane);
    }
    /* Behind the camera. */
    EXPECT_FALSE(de_frustum_test_aabb(fr, box(v3(0, -5, 0), 1)));
    /* Straddling each real plane: kept. */
    EXPECT_TRUE(de_frustum_test_aabb(fr, box(v3(-10, 10, 0), 1)));
    EXPECT_TRUE(de_frustum_test_aabb(fr, box(v3(10, 10, 0), 1)));
    EXPECT_TRUE(de_frustum_test_aabb(fr, box(v3(0, 10, -10), 1)));
    EXPECT_TRUE(de_frustum_test_aabb(fr, box(v3(0, 10, 10), 1)));
    EXPECT_TRUE(de_frustum_test_aabb(fr, box(v3(0, 0.1f, 0), 0.05f)));
    /* Sixth side, the degenerate far plane: arbitrarily far boxes are never culled by it. */
    EXPECT_TRUE(de_frustum_test_aabb(fr, box(v3(0, 1e7f, 0), 1)));
    EXPECT_TRUE(de_frustum_test_aabb(fr, box(v3(0, 1e30f, 0), 1e28f)));
    EXPECT_EQ(culling_plane(fr, box(v3(0, 1e7f, 0), 1)), -1);
    /* ...but far boxes off to the side are still culled by the side planes. */
    EXPECT_FALSE(de_frustum_test_aabb(fr, box(v3(-3e7f, 1e7f, 0), 1)));
    EXPECT_FALSE(de_frustum_test_aabb(fr, de_aabb_empty()));
}

TEST_F(geom, frustum_matches_clip_test, cam_fx) {
    /* Points: the frustum agrees with the clip-space test (-w <= x, y <= w, 0 <= z <= w). */
    int inside = 0;
    for (int i = 0; i < 5000; ++i) {
        de_vec3 p = rnd_v3(-30, 30);
        de_vec4 h = {0};
        h.x = p.x;
        h.y = p.y;
        h.z = p.z;
        h.w = 1;
        de_vec4 c = de_m4_mul_v4(&f->vp, h);
        bool clip_in = c.w > 0 && fabsf(c.x) <= c.w && fabsf(c.y) <= c.w && c.z >= 0 && c.z <= c.w;
        /* Skip points within 1e-3 of a plane, where rounding can go either way. */
        f32 margin = FLT_MAX;
        for (int k = 0; k < 5; ++k)
            margin = fminf(margin, fabsf(de_plane_distance(f->f.planes[k], p)));
        if (margin < 1e-3f)
            continue;
        EXPECT_EQ(de_frustum_test_sphere(&f->f, p, 0), clip_in);
        EXPECT_EQ(de_frustum_test_aabb(&f->f, box(p, 0)), clip_in);
        inside += clip_in;
    }
    EXPECT_GT(inside, 100);
}

TEST_F(geom, frustum_sphere, cam_fx) {
    const de_frustum *fr = &f->f;
    EXPECT_TRUE(de_frustum_test_sphere(fr, v3(0, 10, 0), 1));
    EXPECT_FALSE(de_frustum_test_sphere(fr, v3(-20, 10, 0), 1));
    EXPECT_FALSE(de_frustum_test_sphere(fr, v3(0, 10, 20), 1));
    EXPECT_FALSE(de_frustum_test_sphere(fr, v3(0, -5, 0), 1));
    /* The sphere at x = -11, y = 10 is ~0.707 outside the left plane: radius decides. */
    EXPECT_FALSE(de_frustum_test_sphere(fr, v3(-11, 10, 0), 0.6f));
    EXPECT_TRUE(de_frustum_test_sphere(fr, v3(-11, 10, 0), 0.8f));
    EXPECT_TRUE(de_frustum_test_sphere(fr, v3(0, 1e20f, 0), 1));
}

TEST(geom, frustum_rotated_camera_and_ortho) {
    /* A camera somewhere else, looking down -X: the frustum moves with it. */
    de_mat4 view = de_m4_look_at(v3(100, 50, 2), v3(0, 50, 2), DE_AXIS_UP);
    de_mat4 proj = de_m4_perspective_rz(DE_PI / 3, 2.0f, 0.5f);
    de_mat4 vp = de_m4_mul(&proj, &view);
    de_frustum fr = de_frustum_from_m4(&vp);
    EXPECT_TRUE(de_frustum_test_aabb(&fr, box(v3(50, 50, 2), 1)));
    EXPECT_FALSE(de_frustum_test_aabb(&fr, box(v3(150, 50, 2), 1)));  /* behind */
    EXPECT_FALSE(de_frustum_test_aabb(&fr, box(v3(50, 50, 60), 1)));  /* above */
    EXPECT_TRUE(de_frustum_test_aabb(&fr, box(v3(-1e6f, 50, 2), 1))); /* far ahead */

    /* Reverse-Z ortho has a real far plane. */
    de_mat4 ortho = de_m4_ortho_rz(-10, 10, -10, 10, 1, 100);
    de_mat4 ovp = de_m4_mul(&ortho, &view);
    de_frustum of = de_frustum_from_m4(&ovp);
    EXPECT_FLOAT_EQ(de_v3_length(of.planes[DE_FRUSTUM_FAR].n), 1, 1e-6);
    EXPECT_TRUE(de_frustum_test_aabb(&of, box(v3(50, 50, 2), 1)));
    EXPECT_EQ(culling_plane(&of, box(v3(-50, 50, 2), 1)), DE_FRUSTUM_FAR); /* 150 m away */
    EXPECT_EQ(culling_plane(&of, box(v3(99.8f, 50, 2), 0.1f)), DE_FRUSTUM_NEAR);
    EXPECT_EQ(culling_plane(&of, box(v3(50, 70, 2), 1)), DE_FRUSTUM_RIGHT); /* +Y is right here */
}
