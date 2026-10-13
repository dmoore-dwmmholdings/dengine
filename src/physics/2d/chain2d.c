/*
 * chain2d.c: chain shapes (DENG-252). A chain is a list of one-sided segments that share their
 * end points; each segment stores its neighbours' far points as ghost vertices so the 2D
 * narrowphase (DENG-253) can collide smoothly across the joints (no catching on internal
 * corners), as Box2D's chain segments do.
 *
 * Layout (one allocation, s->data): points[point_count], then segments[segment_count] at a
 * 16-byte boundary. Segment i runs from points[i] to points[(i + 1) % point_count].
 */
#include "physics/2d/p2d_internal.h"

#define DE__CHAIN_ALIGN 16u

static size_t align_up(size_t x) {
    return (x + (DE__CHAIN_ALIGN - 1u)) & ~(size_t)(DE__CHAIN_ALIGN - 1u);
}

/* 2 * a - b in f64, rounded once: the collinear extension of b -> a past a. */
static de_vec2 extend(de_vec2 a, de_vec2 b) {
    return de__v2((f32)(2.0 * (f64)a.x - (f64)b.x), (f32)(2.0 * (f64)a.y - (f64)b.y));
}

de_result de__chain2d_build(de_physics2d_world *p, const de_vec2 *points, u32 count, bool loop,
                            de__shape2d *s) {
    const u32 min_count = loop ? 3u : 2u;
    if (points == NULL || count < min_count || count > DE_SHAPE2D_MAX_CHAIN_POINTS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape2d_chain_create: needs %u..%u points (got %u)", min_count,
                       DE_SHAPE2D_MAX_CHAIN_POINTS, count);
    f64 big = 0.0;
    for (u32 i = 0; i < count; ++i) {
        if (!de__v2_finite(points[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape2d_chain_create: point %u is not finite", i);
        big = de__maxd(big, de__maxd(de__absd(points[i].x), de__absd(points[i].y)));
    }
    /* Shortest segment: a few f32 steps at the chain's coordinates, at least 1e-5 m. */
    const f64 tol = de__maxd(1e-5, 4e-7 * big);
    const u32 seg_count = loop ? count : count - 1u;
    for (u32 i = 0; i < seg_count; ++i) {
        const de__d2 d = de__d2_sub(de__d2_from(points[(i + 1u) % count]), de__d2_from(points[i]));
        if (de__d2_dot(d, d) <= tol * tol)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape2d_chain_create: segment %u is shorter than %g m", i,
                           (double)tol);
    }

    const size_t pts_size = align_up(sizeof(de_vec2) * (size_t)count);
    const size_t size = pts_size + sizeof(de__chain_segment) * (size_t)seg_count;
    u8 *block = (u8 *)DE_ALLOC(p->alloc, size, DE__CHAIN_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape2d_chain_create: %u points", count);
    memset(block, 0, size);
    s->data = block;
    s->data_size = size;

    de__chain2d *c = &s->u.chain;
    c->points = (de_vec2 *)block;
    c->segments = (de__chain_segment *)(block + pts_size);
    c->point_count = count;
    c->segment_count = seg_count;
    c->loop = loop;
    memcpy(c->points, points, sizeof(de_vec2) * (size_t)count);

    for (u32 i = 0; i < seg_count; ++i) {
        de__chain_segment *g = &c->segments[i];
        const u32 i2 = (i + 1u) % count;
        g->p1 = points[i];
        g->p2 = points[i2];
        if (loop || i > 0) {
            g->ghost1 = points[(i + count - 1u) % count];
        } else {
            g->ghost1 = extend(g->p1, g->p2);
            g->flags |= DE__CHAIN_GHOST1_OPEN;
        }
        if (loop || i2 + 1u < count) {
            g->ghost2 = points[(i2 + 1u) % count];
        } else {
            g->ghost2 = extend(g->p2, g->p1);
            g->flags |= DE__CHAIN_GHOST2_OPEN;
        }
        const de__d2 e = de__d2_sub(de__d2_from(g->p2), de__d2_from(g->p1));
        g->normal = de__v2_normalize_d(de__d2_make(e.y, -e.x));
    }

    /* Local box and bounding radius. */
    de__d2 lo = de__d2_from(points[0]), hi = lo;
    f64 r2 = 0.0;
    for (u32 i = 0; i < count; ++i) {
        const de__d2 q = de__d2_from(points[i]);
        lo = de__d2_make(de__mind(lo.x, q.x), de__mind(lo.y, q.y));
        hi = de__d2_make(de__maxd(hi.x, q.x), de__maxd(hi.y, q.y));
        r2 = de__maxd(r2, de__d2_dot(q, q));
    }
    s->local_aabb = de__aabb2d_from_d(lo, hi);
    s->bounding_radius = de__f32_round_up(de_dsqrt64(r2));
    s->radius = 0.0f;
    s->has_area = false;
    return DE_OK;
}
