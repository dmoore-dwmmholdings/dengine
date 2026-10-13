/*
 * p2d_collide_test_util.h: helpers of the 2D collision tests (DENG-253): shape and body makers,
 * field-by-field hashing (XXH3 over little-endian encodings, as the checksum does) and the
 * checked-in golden vector files (name + 64-bit hex per line, regenerated with DE_REGEN_GOLDEN=1).
 * Everything is static: each test translation unit gets its own copy.
 */
#ifndef DE_TEST_P2D_COLLIDE_TEST_UTIL_H
#define DE_TEST_P2D_COLLIDE_TEST_UTIL_H

#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */

#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/2d/broadphase2d.h"
#include "physics/2d/manifold2d.h"
#include "physics/2d/narrow2d.h"
#include "physics/2d/p2d_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exact float comparisons (EXPECT_EQ takes integers and pointers only). */
#define TU_EXPECT_FEQ(a, b) EXPECT_TRUE((a) == (b))
#define TU_EXPECT_FNE(a, b) EXPECT_TRUE((a) != (b))
#define TU_ASSERT_FEQ(a, b) ASSERT_TRUE((a) == (b))

/* ---- Shapes and bodies -------------------------------------------------------------------- */

static inline de_shape2d_desc tu_desc(de_shape2d_kind kind) {
    de_shape2d_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    return d;
}

static inline de_shape tu_circle(de_physics2d_world *p, f32 r, de_vec2 center) {
    de_shape2d_desc d = tu_desc(DE_SHAPE2D_CIRCLE);
    d.radius = r;
    d.center = center;
    return de_shape2d_create(p, &d);
}

static inline de_shape tu_capsule(de_physics2d_world *p, de_vec2 p1, de_vec2 p2, f32 r) {
    de_shape2d_desc d = tu_desc(DE_SHAPE2D_CAPSULE);
    d.p1 = p1;
    d.p2 = p2;
    d.radius = r;
    return de_shape2d_create(p, &d);
}

static inline de_shape tu_box(de_physics2d_world *p, f32 hx, f32 hy, f32 radius) {
    de_shape2d_desc d = tu_desc(DE_SHAPE2D_BOX);
    d.half_extents = (de_vec2){hx, hy};
    d.radius = radius;
    return de_shape2d_create(p, &d);
}

static inline de_shape tu_polygon(de_physics2d_world *p, const de_vec2 *pts, u32 n, f32 radius) {
    de_shape2d_desc d = tu_desc(DE_SHAPE2D_POLYGON);
    d.points = pts;
    d.point_count = n;
    d.radius = radius;
    return de_shape2d_create(p, &d);
}

static inline de_shape tu_segment(de_physics2d_world *p, de_vec2 p1, de_vec2 p2) {
    de_shape2d_desc d = tu_desc(DE_SHAPE2D_SEGMENT);
    d.p1 = p1;
    d.p2 = p2;
    return de_shape2d_create(p, &d);
}

static inline de_transform2d tu_xf(f64 x, f64 y, f32 angle) {
    de_transform2d t;
    t.x = x;
    t.y = y;
    t.rotation = de_rot2d_from_angle(angle);
    return t;
}

static inline de_body tu_body(de_physics2d_world *p, de_body_type type, de_shape s,
                              de_transform2d xf, u32 flags) {
    de_body2d_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = xf;
    d.flags = flags;
    return de_body2d_create(p, &d);
}

static inline de__xf2d tu_pose(f64 x, f64 y, f32 angle) {
    de__xf2d x2;
    x2.p = de__d2_make(x, y);
    x2.q = de_rot2d_from_angle(angle);
    return x2;
}

static inline u32 tu_index(de_body b) {
    return de__handle_index(b.id);
}

/* ---- Hashing ------------------------------------------------------------------------------ */

typedef struct tu_hash {
    u64 h;
} tu_hash;

static inline void tu_h_u32(tu_hash *s, u32 v) {
    u8 b[4];
    for (u32 i = 0; i < 4; ++i)
        b[i] = (u8)(v >> (8u * i));
    s->h = de_hash64(b, 4, s->h);
}
static inline void tu_h_f32(tu_hash *s, f32 v) {
    u32 u;
    memcpy(&u, &v, sizeof u);
    tu_h_u32(s, u);
}
static inline void tu_h_v2(tu_hash *s, de_vec2 v) {
    tu_h_f32(s, v.x);
    tu_h_f32(s, v.y);
}
static inline void tu_h_set(tu_hash *s, const de__contact_set2d *c) {
    tu_h_u32(s, c->point_count);
    if (c->point_count == 0)
        return;
    tu_h_v2(s, c->normal);
    for (u32 i = 0; i < c->point_count; ++i) {
        tu_h_v2(s, c->points[i].local_a);
        tu_h_v2(s, c->points[i].local_b);
        tu_h_f32(s, c->points[i].separation);
        tu_h_u32(s, c->points[i].feature_id);
    }
}

/* ---- Golden vectors ----------------------------------------------------------------------- */

#define TU_GOLDEN_MAX 128

typedef struct tu_golden {
    char name[32];
    u64 value;
} tu_golden;

/*
 * With DE_REGEN_GOLDEN=1: writes `n` entries to `path` (after the `header` comment lines) and
 * returns true (the caller skips). Otherwise compares them with the file line by line and returns
 * false.
 */
static inline bool tu_golden_check(const char *path, const char *header, const tu_golden *got,
                                   u32 n) {
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen != NULL && strcmp(regen, "1") == 0) {
        FILE *out = fopen(path, "wb");
        ASSERT_TRUE(out != NULL);
        fputs(header, out);
        for (u32 i = 0; i < n; ++i)
            fprintf(out, "%s %016llx\n", got[i].name, (unsigned long long)got[i].value);
        fclose(out);
        return true;
    }
    FILE *in = fopen(path, "rb");
    ASSERT_TRUE(in != NULL);
    char line[256];
    u32 lines = 0, bad = 0;
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char name[64];
        unsigned long long v = 0;
        ASSERT_EQ(sscanf(line, "%63s %llx", name, &v), 2);
        ASSERT_LT(lines, n);
        EXPECT_STREQ(name, got[lines].name);
        if ((u64)v != got[lines].value) {
            printf("  %s: expected %016llx got %016llx\n", name, v,
                   (unsigned long long)got[lines].value);
            ++bad;
        }
        ++lines;
    }
    fclose(in);
    EXPECT_EQ(lines, n);
    EXPECT_EQ(bad, 0u);
    return false;
}

/* ---- Angles ------------------------------------------------------------------------------- */

/* Angle between unit vectors a and b in degrees (test-side math). */
static inline double tu_angle_deg(de_vec2 a, de_vec2 b) {
    double d = (double)a.x * b.x + (double)a.y * b.y;
    const double la = sqrt((double)a.x * a.x + (double)a.y * a.y);
    const double lb = sqrt((double)b.x * b.x + (double)b.y * b.y);
    if (la == 0.0 || lb == 0.0)
        return 180.0;
    d /= la * lb;
    if (d > 1.0)
        d = 1.0;
    if (d < -1.0)
        d = -1.0;
    return acos(d) * 57.29577951308232;
}

#endif /* DE_TEST_P2D_COLLIDE_TEST_UTIL_H */
