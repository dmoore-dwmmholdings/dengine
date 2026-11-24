/* nav2d_mesh.c: 2D polygon navmesh (DENG-79). SPEC §21.2, §5.7.
 *
 * Build: each outline is made counter-clockwise and each hole clockwise; holes are bridged into
 * their outline (the nearest mutually visible vertex pair, rightmost hole vertex first), and the
 * resulting weakly simple polygon is ear-clipped. Vertices are then merged by exact position
 * (so outlines that share vertices join) and triangles are linked across shared edges.
 * The build also finds the corners (reflex boundary vertices) and the visibility graph between
 * them. Query: a straight line when the goal is in sight; otherwise A* over the corners, from
 * the corners the start sees to the goal. The result is the exact Euclidean shortest path (a
 * midpoint A* over triangles plus the funnel can pick the wrong side of a hole).
 * A uniform grid over the boundary edges and triangles (DENG-860) keeps locate and line of sight
 * local; it returns exactly what the linear scans return.
 * All geometry is f64 add/mul/compare plus de_dsqrt64; there is no libm. Every loop runs in
 * index order and ties break on the lower index, so results are the same on every machine. */
#include "gameplay/nav/nav2d_internal.h"

#include <string.h>

#define NONE UINT32_MAX

typedef struct dv2 {
    f64 x, y;
} dv2;

static dv2 dv(de_vec2 v) {
    dv2 r;
    r.x = (f64)v.x;
    r.y = (f64)v.y;
    return r;
}

static f64 area2(dv2 a, dv2 b, dv2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool same(dv2 a, dv2 b) {
    return a.x == b.x && a.y == b.y;
}

static f64 dist(dv2 a, dv2 b) {
    const f64 dx = b.x - a.x, dy = b.y - a.y;
    return de_dsqrt64(dx * dx + dy * dy);
}

static de_result build_visibility(de_nav2d_mesh *m);

/* ---------- build scratch: a growable u32 array ---------- */

typedef struct u32buf {
    const de_allocator *a;
    u32 *data;
    u32 count, cap;
} u32buf;

static bool buf_reserve(u32buf *b, u32 n) {
    if (n <= b->cap)
        return true;
    u32 cap = b->cap ? b->cap : 64u;
    while (cap < n)
        cap *= 2u;
    u32 *d = (u32 *)DE_ALLOC(b->a, sizeof(u32) * (size_t)cap, 4, DE_MEM_SCRATCH);
    if (d == NULL)
        return false;
    if (b->count)
        memcpy(d, b->data, sizeof(u32) * (size_t)b->count);
    if (b->data)
        DE_FREE(b->a, b->data, sizeof(u32) * (size_t)b->cap);
    b->data = d;
    b->cap = cap;
    return true;
}

static bool buf_push(u32buf *b, u32 v) {
    if (!buf_reserve(b, b->count + 1u))
        return false;
    b->data[b->count++] = v;
    return true;
}

static void buf_free(u32buf *b) {
    if (b->data)
        DE_FREE(b->a, b->data, sizeof(u32) * (size_t)b->cap);
    b->data = NULL;
    b->count = b->cap = 0;
}

typedef struct builder {
    const de_allocator *a;
    dv2 *pts; /* raw input points, all polygons */
    u32 pt_count;
    u32 *poly_first; /* per polygon (outlines then holes): first raw point, count */
    u32 *poly_count;
    u32 poly_n;
    u32 outline_n;
} builder;

static dv2 P(const builder *b, u32 i) {
    return b->pts[i];
}

static f64 poly_area2(const builder *b, u32 poly) {
    const u32 f = b->poly_first[poly], n = b->poly_count[poly];
    f64 s = 0;
    for (u32 i = 0; i < n; ++i) {
        const dv2 p = P(b, f + i), q = P(b, f + (i + 1u) % n);
        s += p.x * q.y - q.x * p.y;
    }
    return s;
}

static void poly_reverse(builder *b, u32 poly) {
    const u32 f = b->poly_first[poly], n = b->poly_count[poly];
    for (u32 i = 0; i < n / 2u; ++i) {
        const dv2 t = b->pts[f + i];
        b->pts[f + i] = b->pts[f + n - 1u - i];
        b->pts[f + n - 1u - i] = t;
    }
}

static bool point_in_poly(const builder *b, u32 poly, dv2 p) {
    const u32 f = b->poly_first[poly], n = b->poly_count[poly];
    bool in = false;
    for (u32 i = 0, j = n - 1u; i < n; j = i++) {
        const dv2 pi = P(b, f + i), pj = P(b, f + j);
        if ((pi.y > p.y) != (pj.y > p.y)) {
            const f64 t = (p.y - pi.y) / (pj.y - pi.y);
            if (p.x < pi.x + t * (pj.x - pi.x))
                in = !in;
        }
    }
    return in;
}

/* O'Rourke's InCone: is the direction from vertex `v` (neighbours `prev`, `next`, region on
 * the left of prev -> v -> next) toward `t` inside the region's wedge at `v`? */
static bool in_cone(dv2 prev, dv2 v, dv2 next, dv2 t) {
    if (area2(v, next, prev) >= 0) /* convex */
        return area2(v, t, prev) > 0 && area2(t, v, next) > 0;
    return !(area2(v, t, next) >= 0 && area2(t, v, prev) >= 0);
}

/* Does segment p-q cross edge a-b, or pass through a or b, away from the endpoints p, q? */
static bool blocks(dv2 p, dv2 q, dv2 a, dv2 b) {
    const bool pa = same(p, a) || same(q, a), pb = same(p, b) || same(q, b);
    if (pa && pb)
        return false;
    const f64 d1 = area2(p, q, a), d2 = area2(p, q, b);
    const f64 d3 = area2(a, b, p), d4 = area2(a, b, q);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;
    /* An edge endpoint on the open segment p-q. */
    for (u32 k = 0; k < 2; ++k) {
        const dv2 e = k ? b : a;
        if ((k ? pb : pa) || area2(p, q, e) != 0)
            continue;
        const f64 t = (e.x - p.x) * (q.x - p.x) + (e.y - p.y) * (q.y - p.y);
        const f64 l = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
        if (t > 0 && t < l)
            return true;
    }
    return false;
}

static bool seg_clear_ring(const builder *b, dv2 p, dv2 q, const u32 *ring, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (blocks(p, q, P(b, ring[i]), P(b, ring[(i + 1u) % n])))
            return false;
    return true;
}

static bool seg_clear_poly(const builder *b, dv2 p, dv2 q, u32 poly) {
    const u32 f = b->poly_first[poly], n = b->poly_count[poly];
    for (u32 i = 0; i < n; ++i)
        if (blocks(p, q, P(b, f + i), P(b, f + (i + 1u) % n)))
            return false;
    return true;
}

/* Merges hole `h` into `ring` (raw point ids). `pending` lists holes of this outline not yet
 * merged (they must not be crossed by the bridge). */
static de_result bridge_hole(const builder *b, u32buf *ring, u32 h, const u32 *pending,
                             u32 pending_n) {
    const u32 hf = b->poly_first[h], hn = b->poly_count[h];
    u32 mk = 0;
    for (u32 i = 1; i < hn; ++i) {
        const dv2 c = P(b, hf + i), m = P(b, hf + mk);
        if (c.x > m.x || (c.x == m.x && c.y > m.y))
            mk = i;
    }
    const dv2 M = P(b, hf + mk);
    const dv2 Mp = P(b, hf + (mk + hn - 1u) % hn), Mn = P(b, hf + (mk + 1u) % hn);
    const u32 n = ring->count;
    u32 best = NONE;
    f64 best_d = 0;
    for (u32 j = 0; j < n; ++j) {
        const dv2 V = P(b, ring->data[j]);
        const f64 d = (V.x - M.x) * (V.x - M.x) + (V.y - M.y) * (V.y - M.y);
        if (best != NONE && d >= best_d)
            continue;
        const dv2 Vp = P(b, ring->data[(j + n - 1u) % n]), Vn = P(b, ring->data[(j + 1u) % n]);
        if (!in_cone(Vp, V, Vn, M) || !in_cone(Mp, M, Mn, V))
            continue;
        if (!seg_clear_ring(b, M, V, ring->data, n) || !seg_clear_poly(b, M, V, h))
            continue;
        bool clear = true;
        for (u32 k = 0; k < pending_n && clear; ++k)
            clear = seg_clear_poly(b, M, V, pending[k]);
        if (!clear)
            continue;
        best = j;
        best_d = d;
    }
    if (best == NONE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_mesh_build: hole %u cannot be bridged", h);
    if (!buf_reserve(ring, n + hn + 2u))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: scratch");
    /* ring[0..best], M .. around the hole .. M, ring[best], ring[best+1..] */
    const u32 tail = n - best - 1u;
    memmove(ring->data + best + 1u + hn + 2u, ring->data + best + 1u, sizeof(u32) * (size_t)tail);
    u32 w = best + 1u;
    for (u32 i = 0; i <= hn; ++i)
        ring->data[w++] = hf + (mk + i) % hn;
    ring->data[w++] = ring->data[best];
    ring->count = n + hn + 2u;
    return DE_OK;
}

/* Ear clipping of a weakly simple CCW ring; triangles (raw ids) are appended to `tris`. */
static de_result ear_clip(const builder *b, u32buf *ring, u32buf *tris) {
    u32 n = ring->count;
    u32buf links = {b->a, NULL, 0, 0};
    if (!buf_reserve(&links, 2u * n))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: scratch");
    u32 *prev = links.data, *next = links.data + n;
    for (u32 i = 0; i < n; ++i) {
        prev[i] = (i + n - 1u) % n;
        next[i] = (i + 1u) % n;
    }
    u32 cur = 0, left = n, stall = 0;
    de_result res = DE_OK;
    while (left > 3u) {
        const u32 ia = prev[cur], ic = next[cur];
        const dv2 A = P(b, ring->data[ia]), B = P(b, ring->data[cur]), C = P(b, ring->data[ic]);
        const f64 ar = area2(A, B, C);
        bool ear = ar > 0;
        if (ear) {
            for (u32 v = next[ic]; v != ia && ear; v = next[v]) {
                const dv2 V = P(b, ring->data[v]);
                if (same(V, A) || same(V, B) || same(V, C))
                    continue;
                if (area2(A, B, V) >= 0 && area2(B, C, V) >= 0 && area2(C, A, V) >= 0)
                    ear = false;
            }
        }
        const bool degenerate = ar == 0 && stall >= left;
        if (ear || degenerate) {
            if (ear && (!buf_push(tris, ring->data[ia]) || !buf_push(tris, ring->data[cur]) ||
                        !buf_push(tris, ring->data[ic]))) {
                res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: triangles");
                break;
            }
            next[ia] = ic;
            prev[ic] = ia;
            left--;
            cur = ic;
            stall = 0;
            continue;
        }
        cur = ic;
        if (++stall >= 2u * left) {
            res = DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                          "de_nav2d_mesh_build: polygon cannot be triangulated (self-intersecting?)");
            break;
        }
    }
    if (res == DE_OK && left == 3u) {
        const u32 ia = prev[cur], ic = next[cur];
        if (area2(P(b, ring->data[ia]), P(b, ring->data[cur]), P(b, ring->data[ic])) > 0 &&
            (!buf_push(tris, ring->data[ia]) || !buf_push(tris, ring->data[cur]) ||
             !buf_push(tris, ring->data[ic])))
            res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: triangles");
    }
    buf_free(&links);
    return res;
}

/* ---------- sorting (insertion-free merge sort on u32 keys, stable, deterministic) ---------- */

typedef bool (*less_fn)(const void *ctx, u32 a, u32 b);

static void merge_sort(u32 *v, u32 *tmp, u32 n, less_fn less, const void *ctx) {
    for (u32 width = 1; width < n; width *= 2u) {
        for (u32 lo = 0; lo < n; lo += 2u * width) {
            const u32 mid = lo + width < n ? lo + width : n;
            const u32 hi = lo + 2u * width < n ? lo + 2u * width : n;
            u32 i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = less(ctx, v[j], v[i]) ? v[j++] : v[i++];
            while (i < mid)
                tmp[k++] = v[i++];
            while (j < hi)
                tmp[k++] = v[j++];
        }
        memcpy(v, tmp, sizeof(u32) * (size_t)n);
    }
}

static bool pt_less(const void *ctx, u32 a, u32 b) {
    const dv2 *p = (const dv2 *)ctx;
    if (p[a].x != p[b].x)
        return p[a].x < p[b].x;
    return p[a].y < p[b].y;
}

typedef struct edge_ctx {
    const u32 *tv; /* triangle vertex ids, 3 per triangle */
} edge_ctx;

static u64 edge_key(const edge_ctx *c, u32 e) {
    const u32 t = e / 3u, k = e % 3u;
    u32 a = c->tv[3u * t + k], b = c->tv[3u * t + (k + 1u) % 3u];
    if (a > b) {
        const u32 s = a;
        a = b;
        b = s;
    }
    return ((u64)a << 32) | b;
}

static bool edge_less(const void *ctx, u32 a, u32 b) {
    return edge_key((const edge_ctx *)ctx, a) < edge_key((const edge_ctx *)ctx, b);
}

static de_result validate_polys(const de_nav2d_polygon *p, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (p[i].points == NULL || p[i].count < 3u)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_mesh_build: polygon %u", i);
    return DE_OK;
}

de_result de_nav2d_mesh_build(const de_nav2d_mesh_desc *desc, const de_allocator *a,
                              de_nav2d_mesh **out) {
    return de__nav2d_mesh_build_opts(desc, a, false, out);
}

de_result de__nav2d_mesh_build_opts(const de_nav2d_mesh_desc *desc, const de_allocator *a,
                                    bool brute, de_nav2d_mesh **out) {
    if (out != NULL)
        *out = NULL;
    if (desc == NULL || out == NULL || desc->outline_count == 0 || desc->outlines == NULL ||
        (desc->hole_count > 0 && desc->holes == NULL))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_mesh_build: no outlines");
    DE_TRY(validate_polys(desc->outlines, desc->outline_count));
    DE_TRY(validate_polys(desc->holes, desc->hole_count));
    a = de__nav2d_alloc(a);

    builder b;
    memset(&b, 0, sizeof b);
    b.a = a;
    b.poly_n = desc->outline_count + desc->hole_count;
    b.outline_n = desc->outline_count;
    for (u32 i = 0; i < b.poly_n; ++i) {
        const de_nav2d_polygon *p = i < b.outline_n ? &desc->outlines[i]
                                                    : &desc->holes[i - b.outline_n];
        b.pt_count += p->count;
    }
    const size_t pts_bytes = sizeof(dv2) * (size_t)b.pt_count;
    const size_t poly_bytes = sizeof(u32) * 3u * (size_t)b.poly_n;
    b.pts = (dv2 *)DE_ALLOC(a, pts_bytes, 8, DE_MEM_SCRATCH);
    b.poly_first = (u32 *)DE_ALLOC(a, poly_bytes, 4, DE_MEM_SCRATCH);
    u32buf ring = {a, NULL, 0, 0}, tris = {a, NULL, 0, 0}, holes = {a, NULL, 0, 0};
    u32buf sort = {a, NULL, 0, 0};
    de_nav2d_mesh *m = NULL;
    de_result res = DE_OK;
    if (b.pts == NULL || b.poly_first == NULL) {
        res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: scratch");
        goto done;
    }
    b.poly_count = b.poly_first + b.poly_n;
    u32 *hole_owner = b.poly_count + b.poly_n;
    {
        u32 w = 0;
        for (u32 i = 0; i < b.poly_n; ++i) {
            const de_nav2d_polygon *p = i < b.outline_n ? &desc->outlines[i]
                                                        : &desc->holes[i - b.outline_n];
            b.poly_first[i] = w;
            b.poly_count[i] = p->count;
            for (u32 k = 0; k < p->count; ++k)
                b.pts[w++] = dv(p->points[k]);
            const f64 ar = poly_area2(&b, i);
            if (!(ar != 0)) {
                res = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_mesh_build: polygon %u has no area",
                              i);
                goto done;
            }
            if ((i < b.outline_n) != (ar > 0))
                poly_reverse(&b, i); /* outlines CCW, holes CW */
        }
    }
    for (u32 h = b.outline_n; h < b.poly_n; ++h) {
        hole_owner[h] = NONE;
        for (u32 o = 0; o < b.outline_n && hole_owner[h] == NONE; ++o)
            if (point_in_poly(&b, o, P(&b, b.poly_first[h])))
                hole_owner[h] = o;
        if (hole_owner[h] == NONE) {
            res = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_mesh_build: hole %u is in no outline",
                          h - b.outline_n);
            goto done;
        }
    }

    for (u32 o = 0; o < b.outline_n; ++o) {
        ring.count = 0;
        holes.count = 0;
        for (u32 k = 0; k < b.poly_count[o]; ++k)
            if (!buf_push(&ring, b.poly_first[o] + k))
                goto oom;
        for (u32 h = b.outline_n; h < b.poly_n; ++h)
            if (hole_owner[h] == o && !buf_push(&holes, h))
                goto oom;
        /* Rightmost hole first (by its largest x, then by index). */
        for (u32 i = 1; i < holes.count; ++i) {
            for (u32 j = i; j > 0; --j) {
                f64 xa = -1e300, xb = -1e300;
                const u32 ha = holes.data[j - 1u], hb = holes.data[j];
                for (u32 k = 0; k < b.poly_count[ha]; ++k)
                    xa = P(&b, b.poly_first[ha] + k).x > xa ? P(&b, b.poly_first[ha] + k).x : xa;
                for (u32 k = 0; k < b.poly_count[hb]; ++k)
                    xb = P(&b, b.poly_first[hb] + k).x > xb ? P(&b, b.poly_first[hb] + k).x : xb;
                if (!(xb > xa))
                    break;
                holes.data[j - 1u] = hb;
                holes.data[j] = ha;
            }
        }
        for (u32 i = 0; i < holes.count; ++i) {
            res = bridge_hole(&b, &ring, holes.data[i], holes.data + i + 1u, holes.count - i - 1u);
            if (res != DE_OK)
                goto done;
        }
        res = ear_clip(&b, &ring, &tris);
        if (res != DE_OK)
            goto done;
    }
    if (tris.count == 0) {
        res = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_mesh_build: no triangles");
        goto done;
    }

    /* Merge points by exact position: sort raw ids by (x, y). */
    if (!buf_reserve(&sort, 2u * b.pt_count + 3u * tris.count))
        goto oom;
    {
        u32 *order = sort.data, *tmp = sort.data + b.pt_count;
        for (u32 i = 0; i < b.pt_count; ++i)
            order[i] = i;
        merge_sort(order, tmp, b.pt_count, pt_less, b.pts);
        u32 *remap = tmp; /* raw id -> unique id */
        u32 uniq = 0;
        for (u32 i = 0; i < b.pt_count; ++i) {
            if (i > 0 && !same(P(&b, order[i]), P(&b, order[i - 1u])))
                uniq++;
            remap[order[i]] = uniq;
        }
        uniq++;
        /* Unique triangles (drop those with a repeated vertex). */
        u32 tri_n = 0;
        for (u32 t = 0; t < tris.count / 3u; ++t) {
            const u32 v0 = remap[tris.data[3u * t]], v1 = remap[tris.data[3u * t + 1u]],
                      v2 = remap[tris.data[3u * t + 2u]];
            if (v0 == v1 || v1 == v2 || v0 == v2)
                continue;
            tris.data[3u * tri_n] = v0;
            tris.data[3u * tri_n + 1u] = v1;
            tris.data[3u * tri_n + 2u] = v2;
            tri_n++;
        }
        const size_t mesh_bytes = sizeof(de_nav2d_mesh);
        m = (de_nav2d_mesh *)DE_ALLOC(a, mesh_bytes, DE_ALIGNOF(de_nav2d_mesh), DE_MEM_GENERAL);
        if (m == NULL)
            goto oom;
        memset(m, 0, sizeof *m);
        m->alloc = a;
        m->brute = brute;
        m->verts = (de_vec2 *)DE_ALLOC(a, sizeof(de_vec2) * (size_t)uniq, DE_ALIGNOF(de_vec2),
                                       DE_MEM_GENERAL);
        m->tris = (de__nav2d_tri *)DE_ALLOC(a, sizeof(de__nav2d_tri) * (size_t)tri_n,
                                            DE_ALIGNOF(de__nav2d_tri), DE_MEM_GENERAL);
        if (m->verts == NULL || m->tris == NULL)
            goto oom;
        m->vert_count = uniq;
        m->tri_count = tri_n;
        for (u32 i = 0; i < b.pt_count; ++i) {
            const dv2 p = P(&b, order[i]);
            m->verts[remap[order[i]]].x = (f32)p.x;
            m->verts[remap[order[i]]].y = (f32)p.y;
        }
        for (u32 t = 0; t < tri_n; ++t)
            for (u32 k = 0; k < 3u; ++k) {
                m->tris[t].v[k] = tris.data[3u * t + k];
                m->tris[t].adj[k] = NONE;
            }
        /* Adjacency: sort the 3 * tri_n edges by their (min, max) vertex key. */
        u32 *edges = sort.data, *etmp = sort.data + 3u * tri_n;
        if (!buf_reserve(&sort, 6u * tri_n))
            goto oom;
        edges = sort.data;
        etmp = sort.data + 3u * tri_n;
        for (u32 e = 0; e < 3u * tri_n; ++e)
            edges[e] = e;
        edge_ctx ec;
        ec.tv = tris.data;
        merge_sort(edges, etmp, 3u * tri_n, edge_less, &ec);
        for (u32 i = 0; i + 1u < 3u * tri_n;) {
            const u64 k = edge_key(&ec, edges[i]);
            u32 j = i + 1u;
            while (j < 3u * tri_n && edge_key(&ec, edges[j]) == k)
                j++;
            if (j - i >= 2u) {
                const u32 e0 = edges[i], e1 = edges[i + 1u];
                m->tris[e0 / 3u].adj[e0 % 3u] = e1 / 3u;
                m->tris[e1 / 3u].adj[e1 % 3u] = e0 / 3u;
            }
            i = j;
        }
    }
    res = build_visibility(m);
    if (res != DE_OK)
        goto done;
    *out = m;
    m = NULL;
    goto done;
oom:
    res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: out of memory");
done:
    if (m != NULL)
        de_nav2d_mesh_destroy(m);
    buf_free(&sort);
    buf_free(&holes);
    buf_free(&tris);
    buf_free(&ring);
    if (b.poly_first != NULL)
        DE_FREE(a, b.poly_first, poly_bytes);
    if (b.pts != NULL)
        DE_FREE(a, b.pts, pts_bytes);
    return res;
}

static void free_grid(de_nav2d_mesh *m) {
    const de_allocator *a = m->alloc;
    const de__nav2d_grid_index *g = &m->gi;
    const size_t cells = (size_t)g->gx * (size_t)g->gy + 1u;
    if (g->tri_off != NULL)
        DE_FREE(a, g->tri_off, sizeof(u32) * cells);
    if (g->tri_idx != NULL)
        DE_FREE(a, g->tri_idx, sizeof(u32) * (size_t)g->tri_idx_count);
    if (g->edge_off != NULL)
        DE_FREE(a, g->edge_off, sizeof(u32) * cells);
    if (g->edge_idx != NULL)
        DE_FREE(a, g->edge_idx, sizeof(u32) * (size_t)g->edge_idx_count);
}

void de_nav2d_mesh_destroy(de_nav2d_mesh *m) {
    if (m == NULL)
        return;
    const de_allocator *a = m->alloc;
    if (m->verts != NULL)
        DE_FREE(a, m->verts, sizeof(de_vec2) * (size_t)m->vert_count);
    if (m->tris != NULL)
        DE_FREE(a, m->tris, sizeof(de__nav2d_tri) * (size_t)m->tri_count);
    if (m->bedges != NULL)
        DE_FREE(a, m->bedges, sizeof(u32) * 2u * (size_t)m->bedge_count);
    free_grid(m);
    if (m->corners != NULL)
        DE_FREE(a, m->corners, sizeof(u32) * (size_t)m->corner_count);
    if (m->vis_off != NULL)
        DE_FREE(a, m->vis_off, sizeof(u32) * ((size_t)m->corner_count + 1u));
    if (m->vis_nbr != NULL)
        DE_FREE(a, m->vis_nbr, sizeof(u32) * (size_t)m->vis_count);
    DE_FREE(a, m, sizeof *m);
}

/* ---------- queries ---------- */

static dv2 V(const de_nav2d_mesh *m, u32 v) {
    return dv(m->verts[v]);
}

static bool tri_contains(const de_nav2d_mesh *m, u32 t, dv2 p) {
    const de__nav2d_tri *tr = &m->tris[t];
    const dv2 a = V(m, tr->v[0]), b = V(m, tr->v[1]), c = V(m, tr->v[2]);
    return area2(a, b, p) >= 0 && area2(b, c, p) >= 0 && area2(c, a, p) >= 0;
}

/* ---------- uniform grid index (DENG-860) ----------
 * Cells list the triangles and boundary edges that touch them, in ascending id order, so a
 * query that scans a cell sees candidates in the same order as the linear scan would. The
 * results are identical to the scans: a triangle or an edge is listed in every cell its extent
 * (padded by `pad`, far above f64 rounding) can touch, and the border cells take everything
 * beyond the bounds. Only f64 add/mul/div/compare and a truncating cast: no libm. */

static u32 cell_of(f64 v, f64 org, f64 size, u32 n) {
    const f64 t = (v - org) / size;
    if (!(t > 0))
        return 0;
    if (t >= (f64)n)
        return n - 1u;
    return (u32)t;
}

static bool grid_on(const de_nav2d_mesh *m) {
    return !m->brute && m->gi.gx != 0;
}

/* The cell rows that segment p-q touches in column ix (a conservative band). */
static void col_range(const de__nav2d_grid_index *g, dv2 p, dv2 q, u32 ix, u32 *lo, u32 *hi) {
    f64 ylo = p.y < q.y ? p.y : q.y, yhi = p.y < q.y ? q.y : p.y;
    const f64 dx = q.x - p.x;
    if (dx != 0) {
        const f64 minx = p.x < q.x ? p.x : q.x, maxx = p.x < q.x ? q.x : p.x;
        f64 xa = ix == 0 ? minx : g->x0 + (f64)ix * g->sx - g->pad;
        f64 xb = ix + 1u == g->gx ? maxx : g->x0 + (f64)(ix + 1u) * g->sx + g->pad;
        if (xa < minx)
            xa = minx;
        if (xb > maxx)
            xb = maxx;
        const f64 dy = q.y - p.y;
        const f64 ya = p.y + (xa - p.x) * dy / dx, yb = p.y + (xb - p.x) * dy / dx;
        ylo = ya < yb ? ya : yb;
        yhi = ya < yb ? yb : ya;
    }
    *lo = cell_of(ylo - g->pad, g->y0, g->sy, g->gy);
    *hi = cell_of(yhi + g->pad, g->y0, g->sy, g->gy);
}

static void col_span(const de__nav2d_grid_index *g, dv2 p, dv2 q, u32 *c0, u32 *c1) {
    const f64 minx = p.x < q.x ? p.x : q.x, maxx = p.x < q.x ? q.x : p.x;
    *c0 = cell_of(minx - g->pad, g->x0, g->sx, g->gx);
    *c1 = cell_of(maxx + g->pad, g->x0, g->sx, g->gx);
}

/* Counts (fill false: off[c + 1]++) or stores (fill true: idx[off[c]++] = id) the cells of one
 * item: the bounding box p-q when `rect`, else the segment p-q. */
static void index_item(const de__nav2d_grid_index *g, u32 *off, u32 *idx, dv2 p, dv2 q, bool rect,
                       u32 id, bool fill) {
    u32 c0, c1;
    col_span(g, p, q, &c0, &c1);
    for (u32 ix = c0; ix <= c1; ++ix) {
        u32 lo, hi;
        if (rect) {
            lo = cell_of(p.y - g->pad, g->y0, g->sy, g->gy);
            hi = cell_of(q.y + g->pad, g->y0, g->sy, g->gy);
        } else {
            col_range(g, p, q, ix, &lo, &hi);
        }
        for (u32 iy = lo; iy <= hi; ++iy) {
            const u32 c = iy * g->gx + ix;
            if (fill)
                idx[off[c]++] = id;
            else
                off[c + 1u]++;
        }
    }
}

static u32 isqrt_u32(u32 n) {
    u32 r = 0;
    while ((u64)(r + 1u) * (r + 1u) <= n)
        r++;
    return r;
}

static de_result build_grid(de_nav2d_mesh *m) {
    de__nav2d_grid_index *g = &m->gi;
    if (m->brute || m->tri_count == 0 || m->vert_count == 0)
        return DE_OK;
    const de_allocator *a = m->alloc;
    dv2 lo = V(m, 0), hi = lo;
    for (u32 v = 1; v < m->vert_count; ++v) {
        const dv2 p = V(m, v);
        lo.x = p.x < lo.x ? p.x : lo.x;
        lo.y = p.y < lo.y ? p.y : lo.y;
        hi.x = p.x > hi.x ? p.x : hi.x;
        hi.y = p.y > hi.y ? p.y : hi.y;
    }
    f64 ext = hi.x - lo.x > hi.y - lo.y ? hi.x - lo.x : hi.y - lo.y;
    if (!(ext > 0))
        ext = 1;
    u32 dim = isqrt_u32(m->tri_count + m->bedge_count) + 1u;
    if (dim > 256u)
        dim = 256u;
    const f64 ex = hi.x - lo.x > 0 ? hi.x - lo.x : 1.0, ey = hi.y - lo.y > 0 ? hi.y - lo.y : 1.0;
    g->x0 = lo.x;
    g->y0 = lo.y;
    g->sx = ex / (f64)dim;
    g->sy = ey / (f64)dim;
    g->pad = ext * 1e-9;
    const size_t cells = (size_t)dim * dim;
    g->tri_off = (u32 *)DE_ALLOC(a, sizeof(u32) * (cells + 1u), DE_ALIGNOF(u32), DE_MEM_GENERAL);
    g->edge_off = (u32 *)DE_ALLOC(a, sizeof(u32) * (cells + 1u), DE_ALIGNOF(u32), DE_MEM_GENERAL);
    /* gx and gy are set only now, so the destroy path frees what exists with the right size. */
    g->gx = g->gy = dim;
    if (g->tri_off == NULL || g->edge_off == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: grid index");
    memset(g->tri_off, 0, sizeof(u32) * (cells + 1u));
    memset(g->edge_off, 0, sizeof(u32) * (cells + 1u));
    for (u32 pass = 0; pass < 2u; ++pass) {
        const bool fill = pass == 1u;
        for (u32 t = 0; t < m->tri_count; ++t) {
            const dv2 a0 = V(m, m->tris[t].v[0]), b0 = V(m, m->tris[t].v[1]),
                      c0 = V(m, m->tris[t].v[2]);
            dv2 mn = a0, mx = a0;
            const dv2 pts[2] = {b0, c0};
            for (u32 k = 0; k < 2u; ++k) {
                mn.x = pts[k].x < mn.x ? pts[k].x : mn.x;
                mn.y = pts[k].y < mn.y ? pts[k].y : mn.y;
                mx.x = pts[k].x > mx.x ? pts[k].x : mx.x;
                mx.y = pts[k].y > mx.y ? pts[k].y : mx.y;
            }
            index_item(g, g->tri_off, g->tri_idx, mn, mx, true, t, fill);
        }
        for (u32 e = 0; e < m->bedge_count; ++e)
            index_item(g, g->edge_off, g->edge_idx, V(m, m->bedges[2u * e]),
                       V(m, m->bedges[2u * e + 1u]), false, e, fill);
        if (!fill) {
            for (size_t c = 0; c < cells; ++c) {
                g->tri_off[c + 1u] += g->tri_off[c];
                g->edge_off[c + 1u] += g->edge_off[c];
            }
            g->tri_idx_count = g->tri_off[cells];
            g->edge_idx_count = g->edge_off[cells];
            g->tri_idx = (u32 *)DE_ALLOC(a, sizeof(u32) * (size_t)g->tri_idx_count,
                                         DE_ALIGNOF(u32), DE_MEM_GENERAL);
            g->edge_idx = (u32 *)DE_ALLOC(a, sizeof(u32) * (size_t)g->edge_idx_count,
                                          DE_ALIGNOF(u32), DE_MEM_GENERAL);
            if (g->tri_idx == NULL || g->edge_idx == NULL)
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: grid index");
        }
    }
    /* The fill pass advanced every offset to the end of its cell: shift back by one cell. */
    for (size_t c = cells; c > 0; --c) {
        g->tri_off[c] = g->tri_off[c - 1u];
        g->edge_off[c] = g->edge_off[c - 1u];
    }
    g->tri_off[0] = g->edge_off[0] = 0;
    return DE_OK;
}

static u32 locate_d(const de_nav2d_mesh *m, dv2 q) {
    if (grid_on(m)) {
        const de__nav2d_grid_index *g = &m->gi;
        const u32 c = cell_of(q.y, g->y0, g->sy, g->gy) * g->gx + cell_of(q.x, g->x0, g->sx, g->gx);
        for (u32 k = g->tri_off[c]; k < g->tri_off[c + 1u]; ++k)
            if (tri_contains(m, g->tri_idx[k], q))
                return g->tri_idx[k];
        return NONE;
    }
    for (u32 t = 0; t < m->tri_count; ++t)
        if (tri_contains(m, t, q))
            return t;
    return NONE;
}

u32 de__nav2d_mesh_locate(const de_nav2d_mesh *m, de_vec2 p) {
    return locate_d(m, dv(p));
}

static bool edge_crosses(const de_nav2d_mesh *m, u32 e, dv2 p, dv2 q) {
    const dv2 a = V(m, m->bedges[2u * e]), b = V(m, m->bedges[2u * e + 1u]);
    const f64 d1 = area2(p, q, a), d2 = area2(p, q, b);
    const f64 d3 = area2(a, b, p), d4 = area2(a, b, q);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

static bool any_crossing(const de_nav2d_mesh *m, dv2 p, dv2 q) {
    if (!grid_on(m)) {
        for (u32 e = 0; e < m->bedge_count; ++e)
            if (edge_crosses(m, e, p, q))
                return true;
        return false;
    }
    const de__nav2d_grid_index *g = &m->gi;
    u32 c0, c1;
    col_span(g, p, q, &c0, &c1);
    for (u32 ix = c0; ix <= c1; ++ix) {
        u32 lo, hi;
        col_range(g, p, q, ix, &lo, &hi);
        for (u32 iy = lo; iy <= hi; ++iy) {
            const u32 c = iy * g->gx + ix;
            for (u32 k = g->edge_off[c]; k < g->edge_off[c + 1u]; ++k)
                if (edge_crosses(m, g->edge_idx[k], p, q))
                    return true;
        }
    }
    return false;
}

/* The smallest parameter t in (cur, 1) of a boundary vertex on the line p-q, or 1. A vertex is
 * the start of its boundary edge, so the edges of the cells the segment touches find them all.
 * A minimum does not depend on the order or on repeats. */
static f64 next_touch(const de_nav2d_mesh *m, dv2 p, dv2 q, f64 cur, f64 len2) {
    const f64 dx = q.x - p.x, dy = q.y - p.y;
    f64 next = 1;
    if (!grid_on(m)) {
        for (u32 e = 0; e < m->bedge_count; ++e) {
            const dv2 v = V(m, m->bedges[2u * e]);
            if (area2(p, q, v) != 0)
                continue;
            const f64 t = ((v.x - p.x) * dx + (v.y - p.y) * dy) / len2;
            if (t > cur && t < next)
                next = t;
        }
        return next;
    }
    const de__nav2d_grid_index *g = &m->gi;
    u32 c0, c1;
    col_span(g, p, q, &c0, &c1);
    for (u32 ix = c0; ix <= c1; ++ix) {
        u32 lo, hi;
        col_range(g, p, q, ix, &lo, &hi);
        for (u32 iy = lo; iy <= hi; ++iy) {
            const u32 c = iy * g->gx + ix;
            for (u32 k = g->edge_off[c]; k < g->edge_off[c + 1u]; ++k) {
                const dv2 v = V(m, m->bedges[2u * g->edge_idx[k]]);
                if (area2(p, q, v) != 0)
                    continue;
                const f64 t = ((v.x - p.x) * dx + (v.y - p.y) * dy) / len2;
                if (t > cur && t < next)
                    next = t;
            }
        }
    }
    return next;
}

/* Line of sight: the segment p-q lies in the walkable area (the boundary counts as walkable).
 * It may not cross a boundary edge at an interior point of both. Between consecutive boundary
 * vertices that lie on it (in order along it), each piece is wholly inside or wholly outside,
 * so its midpoint decides. No scratch: the next touching vertex is found by a scan, over the
 * grid cells the segment passes through (every edge with brute set). */
static bool seg_visible(const de_nav2d_mesh *m, dv2 p, dv2 q) {
    if (any_crossing(m, p, q))
        return false;
    const f64 dx = q.x - p.x, dy = q.y - p.y, len2 = dx * dx + dy * dy;
    if (len2 == 0)
        return locate_d(m, p) != NONE;
    f64 cur = 0;
    for (;;) {
        const f64 next = next_touch(m, p, q, cur, len2);
        const f64 mid = (cur + next) * 0.5;
        dv2 c;
        c.x = p.x + dx * mid;
        c.y = p.y + dy * mid;
        if (locate_d(m, c) == NONE)
            return false;
        if (next >= 1)
            return true;
        cur = next;
    }
}

/* Visibility graph over the corners (built once, by de_nav2d_mesh_build). Corners are the
 * reflex boundary vertices, plus any boundary vertex where the boundary touches itself; a
 * shortest path in a polygonal area bends only at such vertices. */
static de_result build_visibility(de_nav2d_mesh *m) {
    const de_allocator *a = m->alloc;
    const u32 nv = m->vert_count;
    u32 nb = 0;
    for (u32 t = 0; t < m->tri_count; ++t)
        for (u32 k = 0; k < 3u; ++k)
            nb += m->tris[t].adj[k] == NONE ? 1u : 0u;
    /* Scratch per vertex: incoming and outgoing boundary edge counts and neighbours. */
    const size_t sbytes = sizeof(u32) * 4u * (size_t)nv;
    u32 *sc = (u32 *)DE_ALLOC(a, sbytes, DE_ALIGNOF(u32), DE_MEM_SCRATCH);
    if (sc == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: visibility scratch");
    u32 *in_n = sc, *out_n = sc + nv, *prev = sc + 2u * nv, *next = sc + 3u * nv;
    memset(sc, 0, sbytes);
    de_result res = DE_OK;
    u32buf pairs = {a, NULL, 0, 0};
    if (nb > 0) {
        m->bedges = (u32 *)DE_ALLOC(a, sizeof(u32) * 2u * (size_t)nb, DE_ALIGNOF(u32),
                                    DE_MEM_GENERAL);
        if (m->bedges == NULL)
            goto oom;
    }
    m->bedge_count = nb;
    nb = 0;
    for (u32 t = 0; t < m->tri_count; ++t)
        for (u32 k = 0; k < 3u; ++k) {
            if (m->tris[t].adj[k] != NONE)
                continue;
            const u32 u = m->tris[t].v[k], w = m->tris[t].v[(k + 1u) % 3u];
            m->bedges[2u * nb] = u;
            m->bedges[2u * nb + 1u] = w;
            nb++;
            out_n[u]++;
            next[u] = w;
            in_n[w]++;
            prev[w] = u;
        }
    if (build_grid(m) != DE_OK)
        goto oom;
    u32 nc = 0;
    for (u32 v = 0; v < nv; ++v) {
        if (out_n[v] == 0 && in_n[v] == 0)
            continue;
        const bool simple = out_n[v] == 1u && in_n[v] == 1u;
        /* Interior on the left of each boundary edge: a right turn is a reflex vertex. */
        if (!simple || area2(V(m, prev[v]), V(m, v), V(m, next[v])) < 0)
            in_n[nc++] = v; /* in_n[] is free from here on: reuse it for the corner list */
    }
    if (nc > 0) {
        m->corners = (u32 *)DE_ALLOC(a, sizeof(u32) * (size_t)nc, DE_ALIGNOF(u32), DE_MEM_GENERAL);
        m->vis_off = (u32 *)DE_ALLOC(a, sizeof(u32) * ((size_t)nc + 1u), DE_ALIGNOF(u32),
                                     DE_MEM_GENERAL);
        if (m->corners == NULL || m->vis_off == NULL)
            goto oom;
        memcpy(m->corners, in_n, sizeof(u32) * (size_t)nc);
    }
    m->corner_count = nc;
    for (u32 i = 0; i < nc; ++i)
        for (u32 j = i + 1u; j < nc; ++j)
            if (seg_visible(m, V(m, m->corners[i]), V(m, m->corners[j])))
                if (!buf_push(&pairs, i) || !buf_push(&pairs, j))
                    goto oom;
    {
        const u32 np = pairs.count / 2u;
        if (np > 0) {
            m->vis_nbr = (u32 *)DE_ALLOC(a, sizeof(u32) * 2u * (size_t)np, DE_ALIGNOF(u32),
                                         DE_MEM_GENERAL);
            if (m->vis_nbr == NULL)
                goto oom;
        }
        m->vis_count = 2u * np;
        if (nc > 0) {
            u32 *deg = out_n; /* reuse: per-corner fill cursor */
            memset(deg, 0, sizeof(u32) * (size_t)nc);
            for (u32 k = 0; k < np; ++k) {
                deg[pairs.data[2u * k]]++;
                deg[pairs.data[2u * k + 1u]]++;
            }
            m->vis_off[0] = 0;
            for (u32 i = 0; i < nc; ++i) {
                m->vis_off[i + 1u] = m->vis_off[i] + deg[i];
                deg[i] = m->vis_off[i];
            }
            for (u32 k = 0; k < np; ++k) {
                const u32 i = pairs.data[2u * k], j = pairs.data[2u * k + 1u];
                m->vis_nbr[deg[i]++] = j;
                m->vis_nbr[deg[j]++] = i;
            }
        }
    }
    goto done;
oom:
    res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_build: visibility graph");
done:
    buf_free(&pairs);
    DE_FREE(a, sc, sbytes);
    return res;
}

typedef struct mesh_search {
    f64 *g, *h;
    u32 *parent, *heap, *heap_pos;
    u8 *state;
    u32 heap_count;
} mesh_search;

static bool mbefore(const mesh_search *s, u32 a, u32 b) {
    const f64 fa = s->g[a] + s->h[a], fb = s->g[b] + s->h[b];
    if (fa != fb)
        return fa < fb;
    if (s->h[a] != s->h[b])
        return s->h[a] < s->h[b];
    return a < b;
}

static void mset(mesh_search *s, u32 i, u32 t) {
    s->heap[i] = t;
    s->heap_pos[t] = i;
}

static void mup(mesh_search *s, u32 i) {
    const u32 t = s->heap[i];
    while (i > 0) {
        const u32 p = (i - 1u) / 2u;
        if (!mbefore(s, t, s->heap[p]))
            break;
        mset(s, i, s->heap[p]);
        i = p;
    }
    mset(s, i, t);
}

static u32 mpop(mesh_search *s) {
    const u32 top = s->heap[0];
    s->heap_count--;
    if (s->heap_count == 0)
        return top;
    const u32 t = s->heap[s->heap_count];
    u32 i = 0;
    for (;;) {
        u32 k = 2u * i + 1u;
        if (k >= s->heap_count)
            break;
        if (k + 1u < s->heap_count && mbefore(s, s->heap[k + 1u], s->heap[k]))
            k++;
        if (!mbefore(s, s->heap[k], t))
            break;
        mset(s, i, s->heap[k]);
        i = k;
    }
    mset(s, i, t);
    return top;
}

/* Opens or improves node `v` with cost `g` from `parent`. */
static void mrelax(mesh_search *s, u32 v, f64 g, u32 parent) {
    if (s->state[v] == 2u || (s->state[v] == 1u && g >= s->g[v]))
        return;
    s->g[v] = g;
    s->parent[v] = parent;
    if (s->state[v] == 0) {
        s->state[v] = 1u;
        mset(s, s->heap_count, v);
        s->heap_count++;
    }
    mup(s, s->heap_pos[v]);
}

static de_result emit_path(const de_allocator *a, const dv2 *pts, u32 n, de_vec2 from, de_vec2 to,
                           de_vec2 **points, u32 *count) {
    de_vec2 *out = (de_vec2 *)DE_ALLOC(a, sizeof(de_vec2) * (size_t)n, DE_ALIGNOF(de_vec2),
                                       DE_MEM_GENERAL);
    if (out == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_find_path: path points");
    for (u32 i = 0; i < n; ++i) {
        out[i].x = (f32)pts[i].x;
        out[i].y = (f32)pts[i].y;
    }
    out[0] = from;
    out[n - 1u] = to;
    *points = out;
    *count = n;
    return DE_OK;
}

de_result de_nav2d_mesh_find_path(const de_nav2d_mesh *m, de_vec2 from, de_vec2 to,
                                  const de_allocator *a, de_vec2 **points, u32 *count) {
    if (points != NULL)
        *points = NULL;
    if (count != NULL)
        *count = 0;
    if (m == NULL || points == NULL || count == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_mesh_find_path: argument");
    a = de__nav2d_alloc(a);
    const dv2 ps = dv(from), pg = dv(to);
    if (locate_d(m, ps) == NONE || locate_d(m, pg) == NONE)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_nav2d_mesh_find_path: point outside the mesh");

    const size_t nc = m->corner_count, n = nc + 1u; /* the corners, then the goal */
    const size_t f64_bytes = n * 2u * sizeof(f64) + (n + 1u) * sizeof(dv2);
    const size_t bytes = f64_bytes + n * 3u * sizeof(u32) + n;
    u8 *block = (u8 *)DE_ALLOC(a, bytes, 8, DE_MEM_SCRATCH);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_mesh_find_path: scratch");
    mesh_search s;
    s.g = (f64 *)(void *)block;
    s.h = s.g + n;
    dv2 *path = (dv2 *)(void *)(s.h + n);
    s.parent = (u32 *)(void *)(block + f64_bytes);
    s.heap = s.parent + n;
    s.heap_pos = s.heap + n;
    s.state = (u8 *)(s.heap_pos + n);
    s.heap_count = 0;
    memset(s.state, 0, n);
    de_result res;
    u32 pn = 0;
    if (seg_visible(m, ps, pg)) {
        path[0] = ps;
        path[1] = pg;
        pn = 2;
    } else {
        const u32 goal = (u32)nc;
        for (u32 i = 0; i < nc; ++i)
            s.h[i] = dist(V(m, m->corners[i]), pg);
        s.h[goal] = 0;
        for (u32 i = 0; i < nc; ++i) {
            const dv2 c = V(m, m->corners[i]);
            if (seg_visible(m, ps, c))
                mrelax(&s, i, dist(ps, c), NONE);
        }
        bool found = false;
        while (s.heap_count > 0) {
            const u32 u = mpop(&s);
            s.state[u] = 2u;
            if (u == goal) {
                found = true;
                break;
            }
            const dv2 cu = V(m, m->corners[u]);
            for (u32 k = m->vis_off[u]; k < m->vis_off[u + 1u]; ++k) {
                const u32 v = m->vis_nbr[k];
                mrelax(&s, v, s.g[u] + dist(cu, V(m, m->corners[v])), u);
            }
            if (s.state[goal] != 2u && seg_visible(m, cu, pg))
                mrelax(&s, goal, s.g[u] + dist(cu, pg), u);
        }
        if (!found) {
            DE_FREE(a, block, bytes);
            return DE_FAIL(DE_ERR_NOT_FOUND, "de_nav2d_mesh_find_path: no path");
        }
        u32 len = 1; /* the start */
        for (u32 v = goal; v != NONE; v = s.parent[v])
            len++;
        pn = len;
        path[0] = ps;
        u32 i = len - 1u;
        for (u32 v = goal; v != NONE; v = s.parent[v])
            path[i--] = v == goal ? pg : V(m, m->corners[v]);
    }
    res = emit_path(a, path, pn, from, to, points, count);
    DE_FREE(a, block, bytes);
    return res;
}

/* Closest point on triangle abc to p (Ericson, Real-Time Collision Detection §5.1.5). */
static dv2 closest_on_tri(dv2 p, dv2 a, dv2 b, dv2 c) {
    const dv2 ab = {b.x - a.x, b.y - a.y}, ac = {c.x - a.x, c.y - a.y}, ap = {p.x - a.x, p.y - a.y};
    const f64 d1 = ab.x * ap.x + ab.y * ap.y, d2 = ac.x * ap.x + ac.y * ap.y;
    if (d1 <= 0 && d2 <= 0)
        return a;
    const dv2 bp = {p.x - b.x, p.y - b.y};
    const f64 d3 = ab.x * bp.x + ab.y * bp.y, d4 = ac.x * bp.x + ac.y * bp.y;
    if (d3 >= 0 && d4 <= d3)
        return b;
    const f64 vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const f64 v = d1 / (d1 - d3);
        const dv2 r = {a.x + v * ab.x, a.y + v * ab.y};
        return r;
    }
    const dv2 cp = {p.x - c.x, p.y - c.y};
    const f64 d5 = ab.x * cp.x + ab.y * cp.y, d6 = ac.x * cp.x + ac.y * cp.y;
    if (d6 >= 0 && d5 <= d6)
        return c;
    const f64 vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const f64 w = d2 / (d2 - d6);
        const dv2 r = {a.x + w * ac.x, a.y + w * ac.y};
        return r;
    }
    const f64 va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const f64 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const dv2 r = {b.x + w * (c.x - b.x), b.y + w * (c.y - b.y)};
        return r;
    }
    const f64 denom = 1.0 / (va + vb + vc);
    const f64 v = vb * denom, w = vc * denom;
    const dv2 r = {a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w};
    return r;
}

bool de_nav2d_mesh_nearest_point(const de_nav2d_mesh *m, de_vec2 p, de_vec2 *out) {
    if (m == NULL || m->tri_count == 0)
        return false;
    const dv2 q = dv(p);
    if (de__nav2d_mesh_locate(m, p) != NONE) {
        if (out != NULL)
            *out = p;
        return true;
    }
    f64 best = 0;
    dv2 bp = q;
    for (u32 t = 0; t < m->tri_count; ++t) {
        const de__nav2d_tri *tr = &m->tris[t];
        const dv2 c = closest_on_tri(q, V(m, tr->v[0]), V(m, tr->v[1]), V(m, tr->v[2]));
        const f64 d = (c.x - q.x) * (c.x - q.x) + (c.y - q.y) * (c.y - q.y);
        if (t == 0 || d < best) {
            best = d;
            bp = c;
        }
    }
    if (out != NULL) {
        out->x = (f32)bp.x;
        out->y = (f32)bp.y;
    }
    return true;
}
