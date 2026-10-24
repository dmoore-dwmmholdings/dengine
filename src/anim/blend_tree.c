#include "anim/blend_tree.h"
#include "anim/anim_math.h"
#include <string.h>

static bool point_less(de__anim_blend_point a, de__anim_blend_point b) {
    return a.position.x < b.position.x ||
           (a.position.x == b.position.x && a.position.y < b.position.y);
}
static f64 orient(const de__anim_blend_space *s, u32 a, u32 b, u32 c) {
    return de__anim_orient2d(s->points[a].position, s->points[b].position, s->points[c].position);
}
static void hull_build(de__anim_blend_space *s, bool keep_collinear) {
    u32 n = s->point_count, k = 0;
    for (u32 i = 0; i < n; ++i) {
        while (k >= 2) {
            f64 turn = orient(s, s->hull[k - 2], s->hull[k - 1], i);
            if (turn > 0 || (keep_collinear && turn == 0))
                break;
            --k;
        }
        s->hull[k++] = i;
    }
    u32 lower = k;
    for (u32 i = n - 1; i-- > 0;) {
        while (k > lower) {
            f64 turn = orient(s, s->hull[k - 2], s->hull[k - 1], i);
            if (turn > 0 || (keep_collinear && turn == 0))
                break;
            --k;
        }
        s->hull[k++] = i;
    }
    s->hull_count = n > 1 ? k - 1 : k;
    s->collinear = s->hull_count < 3;
}
typedef struct edge {
    u32 a, b;
} edge;
static bool triangle_contains(const de__anim_blend_space *s, de__anim_blend_triangle t, u32 p) {
    return orient(s, t.point[0], t.point[1], p) >= 0 && orient(s, t.point[1], t.point[2], p) >= 0 &&
           orient(s, t.point[2], t.point[0], p) >= 0;
}
static bool insert_point(de__anim_blend_space *s, u32 p) {
    u32 selected[2], count = 0;
    edge boundary[6];
    u32 edge_count = 0;
    for (u32 i = 0; i < s->triangle_count; ++i) {
        de__anim_blend_triangle t = s->triangles[i];
        if (!triangle_contains(s, t, p))
            continue;
        if (count == 2)
            return false;
        selected[count++] = i;
        for (u32 j = 0; j < 3; ++j) {
            edge e = {t.point[j], t.point[(j + 1) % 3]};
            u32 k = 0;
            while (k < edge_count && !(boundary[k].a == e.b && boundary[k].b == e.a))
                ++k;
            if (k < edge_count)
                boundary[k] = boundary[--edge_count];
            else
                boundary[edge_count++] = e;
        }
    }
    if (!count)
        return false;
    while (count) {
        u32 i = selected[--count];
        s->triangles[i] = s->triangles[--s->triangle_count];
    }
    for (u32 i = 0; i < edge_count; ++i) {
        edge e = boundary[i];
        if (orient(s, e.a, e.b, p) > 0)
            s->triangles[s->triangle_count++] = (de__anim_blend_triangle){{e.a, e.b, p}};
    }
    return true;
}
static bool diagonal_less(u32 a, u32 b, u32 c, u32 d) {
    u32 lo = a < b ? a : b, hi = a < b ? b : a;
    u32 old_lo = c < d ? c : d, old_hi = c < d ? d : c;
    return lo < old_lo || (lo == old_lo && hi < old_hi);
}
/* Lawson edge flips, stable scan order and a spatial-index tie break for exact
 * cocircular points. No synthetic supertriangle can discard a thin real hull. */
static bool triangulate(de__anim_blend_space *s) {
    for (u32 i = 1; i + 1 < s->hull_count; ++i)
        s->triangles[s->triangle_count++] =
            (de__anim_blend_triangle){{s->hull[0], s->hull[i], s->hull[i + 1]}};
    for (u32 p = 0; p < s->point_count; ++p) {
        bool on_hull = false;
        for (u32 i = 0; i < s->hull_count; ++i)
            on_hull |= s->hull[i] == p;
        if (!on_hull && !insert_point(s, p))
            return false;
    }
    u64 remaining = (u64)s->point_count * s->point_count * 8 + 1;
    for (;;) {
        bool flipped = false;
        for (u32 i = 0; i < s->triangle_count && !flipped; ++i) {
            for (u32 j = i + 1; j < s->triangle_count && !flipped; ++j) {
                for (u32 x = 0; x < 3 && !flipped; ++x) {
                    u32 a = s->triangles[i].point[x], b = s->triangles[i].point[(x + 1) % 3],
                        c = s->triangles[i].point[(x + 2) % 3];
                    for (u32 y = 0; y < 3; ++y) {
                        if (s->triangles[j].point[y] != b ||
                            s->triangles[j].point[(y + 1) % 3] != a)
                            continue;
                        u32 d = s->triangles[j].point[(y + 2) % 3];
                        if (!(orient(s, c, d, b) > 0 && orient(s, d, c, a) > 0))
                            continue;
                        f64 circle =
                            de__anim_incircle(s->points[a].position, s->points[b].position,
                                              s->points[c].position, s->points[d].position);
                        if (circle > 0 || (circle == 0 && diagonal_less(c, d, a, b))) {
                            if (!remaining--)
                                return false;
                            s->triangles[i] = (de__anim_blend_triangle){{c, d, b}};
                            s->triangles[j] = (de__anim_blend_triangle){{d, c, a}};
                            flipped = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!flipped)
            return true;
    }
}
de_result de__anim_blend_space_create(const de_allocator *a, u32 dimensions,
                                      const de__anim_blend_point *points, u32 count,
                                      de__anim_blend_space **out) {
    if (!out)
        return DE_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!a || !points || !count || count > 65535 || (dimensions != 1 && dimensions != 2))
        return DE_ERR_INVALID_ARGUMENT;
    for (u32 i = 0; i < count; ++i)
        if (!de__anim_finite(points[i].position.x) ||
            (dimensions == 2 && !de__anim_finite(points[i].position.y)))
            return DE_ERR_INVALID_ARGUMENT;
    size_t bytes = sizeof(de__anim_blend_space) + (size_t)count * sizeof *points +
                   (size_t)count * 2 * sizeof(de__anim_blend_triangle) +
                   (size_t)count * 2 * sizeof(u32);
    de__anim_blend_space *s = DE_ALLOC(a, bytes, 16, DE_MEM_GENERAL);
    if (!s)
        return DE_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof *s);
    s->allocator = *a;
    s->bytes = bytes;
    s->dimensions = dimensions;
    s->point_count = count;
    s->points = (de__anim_blend_point *)(s + 1);
    s->triangles = (de__anim_blend_triangle *)(s->points + count);
    s->hull = (u32 *)(s->triangles + (size_t)count * 2);
    for (u32 i = 0; i < count; ++i) {
        de__anim_blend_point p = points[i];
        if (dimensions == 1)
            p.position.y = 0;
        u32 j = i;
        while (j && point_less(p, s->points[j - 1])) {
            s->points[j] = s->points[j - 1];
            --j;
        }
        s->points[j] = p;
    }
    for (u32 i = 1; i < count; ++i) {
        if (s->points[i].position.x == s->points[i - 1].position.x &&
            s->points[i].position.y == s->points[i - 1].position.y) {
            de__anim_blend_space_destroy(s);
            return DE_ERR_INVALID_ARGUMENT;
        }
    }
    if (dimensions == 2) {
        hull_build(s, false);
        if (!s->collinear && !triangulate(s)) {
            de__anim_blend_space_destroy(s);
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "blend space cannot be triangulated");
        }
        /* Hull-edge samples inserted into the triangulation also split the
         * projection boundary; outside weights must not skip those samples. */
        if (!s->collinear)
            hull_build(s, true);
    }
    *out = s;
    return DE_OK;
}
void de__anim_blend_space_destroy(de__anim_blend_space *s) {
    if (s) {
        de_allocator a = s->allocator;
        DE_FREE(&a, s, s->bytes);
    }
}
static void segment_weights(const de__anim_blend_space *s, u32 a, u32 b, f64 t,
                            de__anim_blend_weights *out) {
    *out = (de__anim_blend_weights){0};
    if ((f32)(1 - t) > 0) {
        out->node[out->count] = s->points[a].node;
        out->weight[out->count++] = (f32)(1 - t);
    }
    if ((f32)t > 0) {
        out->node[out->count] = s->points[b].node;
        out->weight[out->count++] = (f32)t;
    }
}
bool de__anim_blend_space_weights(const de__anim_blend_space *s, de_vec2 query,
                                  de__anim_blend_weights *out) {
    if (!s || !out || !de__anim_finite(query.x) ||
        (s->dimensions == 2 && !de__anim_finite(query.y)))
        return false;
    if (s->point_count == 1) {
        *out = (de__anim_blend_weights){.count = 1, .node = {s->points[0].node}, .weight = {1}};
        return true;
    }
    if (s->dimensions == 1) {
        u32 hi = 0;
        while (hi < s->point_count && s->points[hi].position.x < query.x)
            ++hi;
        if (!hi || hi == s->point_count) {
            u32 p = hi ? hi - 1 : 0;
            *out = (de__anim_blend_weights){.count = 1, .node = {s->points[p].node}, .weight = {1}};
        } else {
            f64 distance;
            query.y = 0;
            f64 t = de__anim_segment_projection(s->points[hi - 1].position, s->points[hi].position,
                                                query, &distance);
            segment_weights(s, hi - 1, hi, t, out);
        }
        return true;
    }
    for (u32 i = 0; i < s->triangle_count; ++i) {
        de__anim_blend_triangle t = s->triangles[i];
        de_vec2 a = s->points[t.point[0]].position, b = s->points[t.point[1]].position,
                c = s->points[t.point[2]].position;
        f64 w[3] = {de__anim_orient2d(b, c, query), de__anim_orient2d(c, a, query),
                    de__anim_orient2d(a, b, query)};
        if (w[0] >= 0 && w[1] >= 0 && w[2] >= 0) {
            f64 total = w[0] + w[1] + w[2];
            *out = (de__anim_blend_weights){0};
            for (u32 j = 0; j < 3; ++j)
                if ((f32)(w[j] / total) > 0) {
                    out->node[out->count] = s->points[t.point[j]].node;
                    out->weight[out->count++] = (f32)(w[j] / total);
                }
            return true;
        }
    }
    f64 closest = 0, best_t = 0;
    u32 best_a = 0, best_b = 1;
    u32 edges = s->collinear ? s->point_count - 1 : s->hull_count;
    for (u32 i = 0; i < edges; ++i) {
        u32 a = s->collinear ? i : s->hull[i];
        u32 b = s->collinear ? i + 1 : s->hull[(i + 1) % s->hull_count];
        f64 distance;
        f64 t = de__anim_segment_projection(s->points[a].position, s->points[b].position, query,
                                            &distance);
        if (!i || distance < closest) {
            closest = distance;
            best_t = t;
            best_a = a;
            best_b = b;
        }
    }
    segment_weights(s, best_a, best_b, best_t, out);
    return true;
}
