/*
 * collide_chain2d.c: chains in the 2D collision pipeline (DENG-253): per-segment bounds (an
 * implicit hierarchy over the segments, built with the chain), their AABB and ray traversals, and
 * the narrowphase of one chain segment against a circle or a polygon core (one-sided, with ghost
 * vertices so bodies slide across joints without catching). See narrow2d.h and p2d_internal.h.
 */
#include "physics/2d/narrow2d.h"

/* ---- Segment bounds ------------------------------------------------------------------------- */

u32 de__chain2d_bounds_count(u32 segment_count) {
    u32 total = 0, n = segment_count;
    while (n > 1u) {
        total += n;
        n = (n + 1u) / 2u;
    }
    return total + 1u;
}

void de__chain2d_build_bounds(de__chain2d *c) {
    const u32 n0 = c->segment_count;
    de_aabb2d *b = c->bounds;
    for (u32 i = 0; i < n0; ++i) {
        const de__chain_segment *g = &c->segments[i];
        b[i].min = de__v2_min(g->p1, g->p2);
        b[i].max = de__v2_max(g->p1, g->p2);
    }
    u32 level = 0, offset = 0, n = n0;
    c->level_offset[0] = 0;
    while (n > 1u) {
        const u32 up = (n + 1u) / 2u;
        const u32 next = offset + n;
        for (u32 j = 0; j < up; ++j) {
            const u32 k = offset + 2u * j;
            b[next + j] = 2u * j + 1u < n ? de__aabb2d_union(&b[k], &b[k + 1u]) : b[k];
        }
        offset = next;
        n = up;
        c->level_offset[++level] = offset;
    }
    c->level_count = level + 1u;
    DE_ASSERT(c->level_count < DE__CHAIN_MAX_LEVELS);
    c->level_offset[c->level_count] = offset + 1u;
}

static u32 level_size(const de__chain2d *c, u32 level) {
    return c->level_offset[level + 1u] - c->level_offset[level];
}

/* Depth-first walk, child 2j before 2j + 1, so leaves come in ascending segment order. The stack
 * holds at most one pending sibling per level. */
typedef struct walk_item {
    u32 level, index;
} walk_item;

#define WALK_STACK (2u * DE__CHAIN_MAX_LEVELS + 2u)

bool de__chain2d_query(const de__chain2d *c, de_aabb2d box, de__chain2d_query_fn fn, void *user) {
    if (c->segment_count == 0 || c->bounds == NULL)
        return true;
    walk_item stack[WALK_STACK];
    u32 top = 0;
    stack[top].level = c->level_count - 1u;
    stack[top].index = 0;
    ++top;
    while (top > 0) {
        const walk_item it = stack[--top];
        const de_aabb2d *b = &c->bounds[c->level_offset[it.level] + it.index];
        if (!de__aabb2d_overlap(b, &box))
            continue;
        if (it.level == 0) {
            if (!fn(user, it.index))
                return false;
            continue;
        }
        const u32 child = 2u * it.index;
        if (child + 1u < level_size(c, it.level - 1u)) {
            stack[top].level = it.level - 1u;
            stack[top].index = child + 1u;
            ++top;
        }
        stack[top].level = it.level - 1u;
        stack[top].index = child;
        ++top;
        DE_ASSERT(top <= WALK_STACK);
    }
    return true;
}

f32 de__chain2d_raycast(const de__chain2d *c, const de__tree2d_ray *ray, de__chain2d_ray_fn fn,
                        void *user) {
    f32 max_fraction = ray->max_fraction;
    if (c->segment_count == 0 || c->bounds == NULL)
        return max_fraction;
    walk_item stack[WALK_STACK];
    u32 top = 0;
    stack[top].level = c->level_count - 1u;
    stack[top].index = 0;
    ++top;
    while (top > 0) {
        const walk_item it = stack[--top];
        const de_aabb2d *b = &c->bounds[c->level_offset[it.level] + it.index];
        f64 t_enter;
        if (!de__ray2d_aabb(ray, max_fraction, b, &t_enter))
            continue;
        if (it.level == 0) {
            const f32 r = fn(user, ray, it.index, max_fraction);
            if (!(r > 0.0f))
                return 0.0f;
            if (r < max_fraction)
                max_fraction = r;
            continue;
        }
        const u32 child = 2u * it.index;
        if (child + 1u < level_size(c, it.level - 1u)) {
            stack[top].level = it.level - 1u;
            stack[top].index = child + 1u;
            ++top;
        }
        stack[top].level = it.level - 1u;
        stack[top].index = child;
        ++top;
    }
    return max_fraction;
}

/* ---- Narrowphase ---------------------------------------------------------------------------- */

static de_vec2 v2_neg(de_vec2 a) {
    return de__v2(-a.x, -a.y);
}
static de_vec2 v2_mad(de_vec2 a, f32 s, de_vec2 b) {
    return de__v2(a.x + s * b.x, a.y + s * b.y);
}

/* (SHAPE 0, SHAPE 0) */
#define K_CURVED_ID ((2u << 14) | ((2u << 14) << 16))

void de__n2_chain_circle(const de__chain_segment *seg, de_vec2 shift, de_vec2 c, f32 rb,
                         f32 speculative, de__n2_manifold *m) {
    const de_vec2 p1 = de__v2_sub(seg->p1, shift);
    const de_vec2 p2 = de__v2_sub(seg->p2, shift);
    const de_vec2 e = de__v2_sub(p2, p1);
    /* One-sided: the right side of p1 -> p2. */
    if (de__v2_dot(de__v2_right_perp(e), de__v2_sub(c, p1)) < 0.0f)
        return;
    const f32 u = de__v2_dot(e, de__v2_sub(p2, c));
    const f32 v = de__v2_dot(e, de__v2_sub(c, p1));
    de_vec2 q;
    if (v <= 0.0f) {
        /* p1's region: ours unless the center is still beside the previous segment. */
        if ((seg->flags & DE__CHAIN_GHOST1_OPEN) == 0) {
            const de_vec2 prev = de__v2_sub(seg->p1, seg->ghost1);
            if (de__v2_dot(prev, de__v2_sub(c, p1)) < 0.0f)
                return;
        }
        q = p1;
    } else if (u <= 0.0f) {
        /* p2's region belongs to the next segment (its p1), unless the chain ends here. */
        if ((seg->flags & DE__CHAIN_GHOST2_OPEN) == 0)
            return;
        q = p2;
    } else {
        const f32 ee = de__v2_dot(e, e);
        q = de__v2((u * p1.x + v * p2.x) / ee, (u * p1.y + v * p2.y) / ee);
    }
    de_vec2 n;
    const f32 dist = de__n2_normalize(de__v2_sub(c, q), &n);
    if (dist == 0.0f && de__n2_normalize(de__v2_right_perp(e), &n) == 0.0f)
        return;
    const f32 sep = dist - rb;
    if (!(sep <= speculative))
        return;
    m->normal = n;
    de__n2_add(m, q, v2_mad(c, -rb, n), sep, K_CURVED_ID, speculative);
}

/* Box2D v3 b2ChainSegmentParams and b2ClassifyNormal. */
typedef struct smooth_params {
    de_vec2 edge1;
    de_vec2 normal0, normal2;
    bool convex1, convex2;
} smooth_params;

enum { NORMAL_SKIP, NORMAL_ADMIT, NORMAL_SNAP };

static u32 classify_normal(const smooth_params *sp, de_vec2 normal) {
    const f32 sin_tol = 0.01f;
    if (de__v2_dot(normal, sp->edge1) <= 0.0f) {
        /* Toward the segment's tail. */
        if (!sp->convex1)
            return NORMAL_SNAP;
        return de__v2_cross(normal, sp->normal0) > sin_tol ? NORMAL_SKIP : NORMAL_ADMIT;
    }
    /* Toward the head. */
    if (!sp->convex2)
        return NORMAL_SNAP;
    return de__v2_cross(sp->normal2, normal) > sin_tol ? NORMAL_SKIP : NORMAL_ADMIT;
}

static u32 next_index(u32 i, u32 count) {
    return i + 1u < count ? i + 1u : 0u;
}
static u32 prev_index(u32 i, u32 count) {
    return i > 0u ? i - 1u : count - 1u;
}

/* True when p is inside (or on) the core polygon b (3+ vertices). */
static bool core_contains(const de__n2_poly *b, de_vec2 p) {
    if (b->count < 3u)
        return false;
    for (u32 i = 0; i < b->count; ++i)
        if (de__v2_dot(b->n[i], de__v2_sub(p, b->v[i])) > 0.0f)
            return false;
    return true;
}

/* Clipping with the reference on B's edge (ref_b) or on the segment: maps the clip points to A/B
 * anchors. Lower point first. */
static void emit_clip(const de__n2_clip *cl, bool ref_is_b, de_vec2 normal_ab, u32 id_lower,
                      u32 id_upper, f32 speculative, de__n2_manifold *m) {
    m->normal = normal_ab;
    const u32 ids[2] = {id_lower, id_upper};
    for (u32 i = 0; i < 2; ++i) {
        const de_vec2 pa = ref_is_b ? cl->inc[i] : cl->ref[i];
        const de_vec2 pb = ref_is_b ? cl->ref[i] : cl->inc[i];
        de__n2_add(m, pa, pb, cl->separation[i], ids[i], speculative);
    }
    if (m->count == 0)
        m->normal = de__v2(0.0f, 0.0f);
}

static u32 vid(u32 a, u32 b) {
    return de__f2_id(de__f2(DE__F2_VERTEX, a), de__f2(DE__F2_VERTEX, b));
}

void de__n2_chain_polygon(const de__chain_segment *seg, de_vec2 shift, const de__n2_poly *b,
                          f32 speculative, de__n2_manifold *m) {
    const de_vec2 p1 = de__v2_sub(seg->p1, shift);
    const de_vec2 p2 = de__v2_sub(seg->p2, shift);
    const u32 count = b->count;
    const f32 rb = b->radius;

    smooth_params sp;
    if (de__n2_normalize(de__v2_sub(p2, p1), &sp.edge1) == 0.0f)
        return;
    const de_vec2 normal1 = de__v2_right_perp(sp.edge1);
    if (seg->flags & DE__CHAIN_GHOST1_OPEN) {
        /* A free corner: every normal between normal1 and the outward tail direction. */
        sp.normal0 = v2_neg(sp.edge1);
        sp.convex1 = true;
    } else {
        de_vec2 edge0;
        (void)de__n2_normalize(de__v2_sub(seg->p1, seg->ghost1), &edge0);
        sp.normal0 = de__v2_right_perp(edge0);
        sp.convex1 = de__v2_cross(edge0, sp.edge1) >= 0.01f;
    }
    if (seg->flags & DE__CHAIN_GHOST2_OPEN) {
        sp.normal2 = sp.edge1;
        sp.convex2 = true;
    } else {
        de_vec2 edge2;
        (void)de__n2_normalize(de__v2_sub(seg->ghost2, seg->p2), &edge2);
        sp.normal2 = de__v2_right_perp(edge2);
        sp.convex2 = de__v2_cross(sp.edge1, edge2) >= 0.01f;
    }

    /* One-sided: skip B when its interior point is behind the segment and its convex
     * neighbours. */
    const de_vec2 cb = b->centroid;
    const bool behind1 = de__v2_dot(normal1, de__v2_sub(cb, p1)) < 0.0f;
    const bool behind0 = !sp.convex1 || de__v2_dot(sp.normal0, de__v2_sub(cb, p1)) < 0.0f;
    const bool behind2 = !sp.convex2 || de__v2_dot(sp.normal2, de__v2_sub(cb, p2)) < 0.0f;
    if (behind1 && behind0 && behind2)
        return;

    /* Closest features of the segment and B's core: the closest edge of B (ties: lowest). */
    u32 close_edge = 0;
    de__n2_segdist close;
    close.d2 = FLT_MAX;
    close.f1 = close.f2 = 0.0f;
    close.c1 = close.c2 = p1;
    for (u32 j = 0; j < count; ++j) {
        const de__n2_segdist r =
            de__n2_segment_distance(p1, p2, b->v[j], b->v[next_index(j, count)]);
        if (r.d2 < close.d2) {
            close = r;
            close_edge = j;
        }
    }
    if (count == 2u && de__v2_dot(b->n[close_edge], de__v2_sub(close.c1, close.c2)) < 0.0f) {
        /* A two-vertex core has the same segment twice: take the side that faces us. */
        close_edge = 1u - close_edge;
        close = de__n2_segment_distance(p1, p2, b->v[close_edge], b->v[1u - close_edge]);
    }
    const f32 distance = core_contains(b, p1) ? 0.0f : de_dsqrt(close.d2);
    if (distance > rb + speculative)
        return;

    const de_vec2 n0 = sp.convex1 ? sp.normal0 : normal1;
    const de_vec2 n2 = sp.convex2 ? sp.normal2 : normal1;
    u32 incident_index = UINT32_MAX;  /* B's deepest vertex for the segment normal */
    u32 incident_normal = UINT32_MAX; /* or B's incident edge */
    de__n2_clip cl;

    if (!behind1 && distance > 0.1f * DE__P2D_LINEAR_SLOP) {
        const u32 seg_v = close.f1 == 0.0f ? 0u : (close.f1 == 1.0f ? 1u : UINT32_MAX);
        const u32 poly_v = close.f2 == 0.0f   ? close_edge
                           : close.f2 == 1.0f ? next_index(close_edge, count)
                                              : UINT32_MAX;
        if (seg_v != UINT32_MAX && poly_v != UINT32_MAX) {
            /* Vertex - vertex. */
            de_vec2 normal;
            (void)de__n2_normalize(de__v2_sub(close.c2, close.c1), &normal);
            const u32 type = classify_normal(&sp, normal);
            if (type == NORMAL_SKIP)
                return;
            if (type == NORMAL_ADMIT) {
                m->normal = normal;
                de__n2_add(m, close.c1, v2_mad(close.c2, -rb, normal), distance - rb,
                           vid(seg_v, poly_v), speculative);
                return;
            }
            incident_index = poly_v; /* snap to the segment normal */
        } else if (seg_v != UINT32_MAX) {
            /* A segment vertex against B's edge. */
            const u32 ib = close_edge;
            const de_vec2 normal_b = b->n[ib];
            const u32 type = classify_normal(&sp, v2_neg(normal_b));
            if (type == NORMAL_SKIP)
                return;
            if (type == NORMAL_ADMIT) {
                const u32 ib2 = next_index(ib, count);
                const de_vec2 b1 = b->v[ib], b2 = b->v[ib2];
                const f32 dot1 = de__v2_dot(normal_b, de__v2_sub(p1, b1));
                const f32 dot2 = de__v2_dot(normal_b, de__v2_sub(p2, b1));
                const de_vec2 nn = dot1 < dot2 ? n0 : n2;
                if (de__v2_dot(nn, normal_b) < de__v2_dot(normal1, normal_b))
                    return; /* the neighbour is the incident segment */
                if (de__n2_clip_segments(b1, b2, p1, p2, normal_b, rb, 0.0f, &cl) == 0)
                    return;
                emit_clip(&cl, true, v2_neg(normal_b), vid(1u, ib), vid(0u, ib2), speculative, m);
                return;
            }
            incident_normal = ib;
        } else {
            /* The segment's edge against B's vertex (or parallel edges). */
            if (poly_v != UINT32_MAX) {
                incident_index = poly_v;
            } else {
                const u32 j2 = next_index(close_edge, count);
                const f32 d1 = de__v2_dot(normal1, de__v2_sub(b->v[close_edge], p1));
                const f32 d2 = de__v2_dot(normal1, de__v2_sub(b->v[j2], p1));
                incident_index = d1 < d2 ? close_edge : j2;
            }
        }
    } else {
        /* Overlapping (or behind the segment line): SAT. */
        f32 edge_separation = FLT_MAX;
        for (u32 i = 0; i < count; ++i) {
            const f32 s = de__v2_dot(normal1, de__v2_sub(b->v[i], p1));
            if (s < edge_separation) {
                edge_separation = s;
                incident_index = i;
            }
        }
        /* A convex neighbour that separates more owns the contact. */
        if (sp.convex1) {
            f32 s0 = FLT_MAX;
            for (u32 i = 0; i < count; ++i)
                s0 = de__minf(s0, de__v2_dot(sp.normal0, de__v2_sub(b->v[i], p1)));
            if (s0 > edge_separation) {
                edge_separation = s0;
                incident_index = UINT32_MAX;
            }
        }
        if (sp.convex2) {
            f32 s2 = FLT_MAX;
            for (u32 i = 0; i < count; ++i)
                s2 = de__minf(s2, de__v2_dot(sp.normal2, de__v2_sub(b->v[i], p2)));
            if (s2 > edge_separation) {
                edge_separation = s2;
                incident_index = UINT32_MAX;
            }
        }
        /* B's admissible normals. */
        f32 polygon_separation = -FLT_MAX;
        u32 reference = UINT32_MAX;
        for (u32 i = 0; i < count; ++i) {
            const de_vec2 n = b->n[i];
            if (classify_normal(&sp, v2_neg(n)) != NORMAL_ADMIT)
                continue;
            const de_vec2 p = b->v[i];
            const f32 s =
                de__minf(de__v2_dot(n, de__v2_sub(p2, p)), de__v2_dot(n, de__v2_sub(p1, p)));
            if (s > polygon_separation) {
                polygon_separation = s;
                reference = i;
            }
        }
        if (de__maxf(edge_separation, polygon_separation) > rb + speculative)
            return;
        if (reference != UINT32_MAX && polygon_separation > edge_separation) {
            const u32 ia2 = next_index(reference, count);
            const de_vec2 a1 = b->v[reference], a2 = b->v[ia2];
            const de_vec2 n = b->n[reference];
            const f32 dot1 = de__v2_dot(n, de__v2_sub(p1, a1));
            const f32 dot2 = de__v2_dot(n, de__v2_sub(p2, a1));
            const de_vec2 nn = dot1 < dot2 ? n0 : n2;
            if (de__v2_dot(nn, n) < de__v2_dot(normal1, n))
                return; /* the neighbour is the incident segment */
            if (de__n2_clip_segments(a1, a2, p1, p2, n, rb, 0.0f, &cl) == 0)
                return;
            emit_clip(&cl, true, v2_neg(n), vid(1u, reference), vid(0u, ia2), speculative, m);
            return;
        }
        if (incident_index == UINT32_MAX)
            return; /* a neighbouring segment is the separating axis */
    }

    /* The segment normal: B's incident edge next to its deepest vertex, the one most
     * anti-parallel to normal1. */
    u32 ib1, ib2;
    if (incident_normal != UINT32_MAX) {
        ib1 = incident_normal;
        ib2 = next_index(ib1, count);
    } else {
        const u32 i2 = incident_index;
        const u32 i1 = prev_index(i2, count);
        const f32 d1 = de__v2_dot(normal1, b->n[i1]);
        const f32 d2 = de__v2_dot(normal1, b->n[i2]);
        if (d1 < d2) {
            ib1 = i1;
            ib2 = i2;
        } else {
            ib1 = i2;
            ib2 = next_index(i2, count);
        }
    }
    if (de__n2_clip_segments(p1, p2, b->v[ib1], b->v[ib2], normal1, 0.0f, rb, &cl) == 0)
        return;
    emit_clip(&cl, false, normal1, vid(0u, ib2), vid(1u, ib1), speculative, m);
}
