/*
 * convex_hull.c: deterministic quickhull (DENG-132).
 *
 * Steps, all in f64 (the f32 input is exact in f64):
 *   1. Initial tetrahedron from the extreme points (ties: lowest index). Fails when the points
 *      are all within `tol` of one point, one line or one plane.
 *   2. Every other point goes to the first face (creation order) it is more than `tol` above.
 *   3. Repeat: take the point farthest above its face (ties: lowest index); find the faces it
 *      sees (flood fill from its face over neighbours more than `tol` below it); replace them
 *      with a fan of new triangles from the horizon loop to the point; reassign the orphaned
 *      points to the first new face they are above. A point whose new triangles would be
 *      degenerate, or whose visible region has no single horizon loop (numerical corner cases),
 *      is dropped instead. Stops when no point is outside, or at DE_SHAPE_MAX_HULL_VERTICES.
 *   4. Merge coplanar triangles: seeds in order of decreasing area (ties: index), each grows
 *      over neighbours whose three corners are within `tol` of the seed's plane. Each group must
 *      have one simple boundary loop. Vertices left on fewer than three faces (in the middle of
 *      an edge) are removed from the loops. If any of this fails, the plain triangles are kept.
 *   5. Output: vertices in ascending input order; faces in seed order, each with a Newell
 *      normal and the plane through its outermost vertex; half-edges per face, contiguous, with
 *      twins found by a sorted key search.
 *
 * Nothing depends on addresses, hash maps or the C library: the same points in the same order
 * give the same bytes everywhere.
 */
#include "physics/shapes/shape_internal.h"

#include "core/de_array.h"
#include "core/de_sort.h"

#define QH_NONE 0xFFFFFFFFu

typedef struct qh_face {
    u32 v[3]; /* input point indices, counter-clockwise seen from outside */
    u32 n[3]; /* neighbour face across edge i (v[i] -> v[(i + 1) % 3]) */
    de_dvec3 normal;
    f64 d;
    u32 stamp; /* flood-fill stamp */
    u8 state;  /* during a flood fill: 1 visible, 2 checked and not visible */
    u8 alive;
} qh_face;

typedef struct qh_horizon {
    u32 a, b;    /* edge a -> b of a visible face */
    u32 outside; /* the face across it, not visible */
} qh_horizon;

typedef struct qh {
    const de_allocator *a;
    u32 n;
    f64 tol;
    de_dvec3 *pts;
    u32 *assign; /* face of each outside point, or QH_NONE */
    f64 *dist;   /* height of each point above its face */
    u8 *is_vertex;
    u32 vertex_count;
    de_array faces;   /* qh_face */
    de_array stack;   /* u32 */
    de_array visible; /* u32 */
    de_array horizon; /* qh_horizon */
    de_array loop;    /* qh_horizon, ordered */
    u32 stamp;
} qh;

static qh_face *face_at(qh *h, u32 i) {
    return &DE_ARRAY_AT(&h->faces, qh_face, i);
}

static f64 face_dist(const qh_face *f, de_dvec3 p) {
    return de__d3_dot(f->normal, p) + f->d;
}

/* Plane of triangle (a, b, c); false when it is degenerate (|normal| == 0). */
static bool tri_plane(const qh *h, u32 a, u32 b, u32 c, de_dvec3 *n, f64 *d) {
    const de_dvec3 pa = h->pts[a];
    const de_dvec3 nn = de__d3_cross(de__d3_sub(h->pts[b], pa), de__d3_sub(h->pts[c], pa));
    const f64 len2 = de__d3_dot(nn, nn);
    if (!(len2 > 0.0))
        return false;
    *n = de__d3_scale(nn, 1.0 / de_dsqrt64(len2));
    *d = -de__d3_dot(*n, pa);
    return true;
}

static de_result push_face(qh *h, u32 a, u32 b, u32 c, u32 *out_index) {
    qh_face f;
    memset(&f, 0, sizeof f);
    f.v[0] = a;
    f.v[1] = b;
    f.v[2] = c;
    f.n[0] = f.n[1] = f.n[2] = QH_NONE;
    f.alive = 1;
    if (!tri_plane(h, a, b, c, &f.normal, &f.d))
        return DE_FAIL(DE_ERR_INTERNAL, "quickhull: degenerate face");
    *out_index = h->faces.count;
    return DE_ARRAY_PUSH(&h->faces, qh_face, f);
}

/* Index of the edge of face f that goes a -> b, or 3. */
static u32 face_edge(const qh_face *f, u32 a, u32 b) {
    for (u32 i = 0; i < 3; ++i)
        if (f->v[i] == a && f->v[(i + 1) % 3] == b)
            return i;
    return 3;
}

/* ---- Initial tetrahedron ---------------------------------------------------------------- */

static de_result initial_simplex(qh *h) {
    const de_dvec3 *p = h->pts;
    u32 ext[6] = {0, 0, 0, 0, 0, 0}; /* min x, max x, min y, max y, min z, max z */
    for (u32 i = 1; i < h->n; ++i) {
        const f64 c[3] = {p[i].x, p[i].y, p[i].z};
        for (u32 ax = 0; ax < 3; ++ax) {
            const f64 lo[3] = {p[ext[0]].x, p[ext[2]].y, p[ext[4]].z};
            const f64 hi[3] = {p[ext[1]].x, p[ext[3]].y, p[ext[5]].z};
            if (c[ax] < lo[ax])
                ext[ax * 2] = i;
            if (c[ax] > hi[ax])
                ext[ax * 2 + 1] = i;
        }
    }
    u32 i0 = 0, i1 = 0;
    f64 best = -1.0;
    for (u32 ax = 0; ax < 3; ++ax) {
        const de_dvec3 dd = de__d3_sub(p[ext[ax * 2 + 1]], p[ext[ax * 2]]);
        const f64 l2 = de__d3_dot(dd, dd);
        if (l2 > best) {
            best = l2;
            i0 = ext[ax * 2];
            i1 = ext[ax * 2 + 1];
        }
    }
    if (!(best > h->tol * h->tol))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "convex hull: all points are the same point");

    const de_dvec3 dir = de__d3_sub(p[i1], p[i0]);
    const f64 dir2 = de__d3_dot(dir, dir);
    u32 i2 = QH_NONE;
    best = -1.0;
    for (u32 i = 0; i < h->n; ++i) {
        const de_dvec3 c = de__d3_cross(de__d3_sub(p[i], p[i0]), dir);
        const f64 d2 = de__d3_dot(c, c) / dir2; /* squared distance to the line */
        if (d2 > best) {
            best = d2;
            i2 = i;
        }
    }
    if (!(best > h->tol * h->tol))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "convex hull: all points are on one line");

    de_dvec3 nrm;
    f64 d;
    if (!tri_plane(h, i0, i1, i2, &nrm, &d))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "convex hull: all points are on one line");
    u32 i3 = QH_NONE;
    best = -1.0;
    for (u32 i = 0; i < h->n; ++i) {
        const f64 dist = de__absd(de__d3_dot(nrm, p[i]) + d);
        if (dist > best) {
            best = dist;
            i3 = i;
        }
    }
    if (!(best > h->tol))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "convex hull: all points are on one plane");

    /* Orient so that i3 is below face (i0, i1, i2). */
    if (de__d3_dot(nrm, p[i3]) + d > 0.0) {
        const u32 t = i1;
        i1 = i2;
        i2 = t;
    }
    const u32 tri[4][3] = {{i0, i1, i2}, {i0, i3, i1}, {i1, i3, i2}, {i2, i3, i0}};
    for (u32 f = 0; f < 4; ++f) {
        u32 idx;
        DE_TRY(push_face(h, tri[f][0], tri[f][1], tri[f][2], &idx));
    }
    /* Neighbours: the face holding the reverse edge. */
    for (u32 f = 0; f < 4; ++f) {
        qh_face *fa = face_at(h, f);
        for (u32 e = 0; e < 3; ++e) {
            const u32 a = fa->v[e], b = fa->v[(e + 1) % 3];
            for (u32 g = 0; g < 4; ++g)
                if (g != f && face_edge(face_at(h, g), b, a) < 3)
                    fa->n[e] = g;
        }
    }
    const u32 vs[4] = {i0, i1, i2, i3};
    for (u32 k = 0; k < 4; ++k)
        h->is_vertex[vs[k]] = 1;
    h->vertex_count = 4;
    return DE_OK;
}

/* Assigns point i to the first face in [first, faces.count) that it is more than tol above. */
static void assign_point(qh *h, u32 i, u32 first) {
    h->assign[i] = QH_NONE;
    for (u32 f = first; f < h->faces.count; ++f) {
        const qh_face *fa = face_at(h, f);
        if (!fa->alive)
            continue;
        const f64 dd = face_dist(fa, h->pts[i]);
        if (dd > h->tol) {
            h->assign[i] = f;
            h->dist[i] = dd;
            return;
        }
    }
}

/* ---- One quickhull step ----------------------------------------------------------------- */

/* Visible faces of point `pi` (flood fill from `start`) and the horizon edges around them. */
static de_result find_visible(qh *h, u32 pi, u32 start) {
    const de_dvec3 p = h->pts[pi];
    ++h->stamp;
    de_array_clear(&h->visible);
    de_array_clear(&h->horizon);
    de_array_clear(&h->stack);
    qh_face *f0 = face_at(h, start);
    f0->stamp = h->stamp;
    f0->state = 1;
    DE_TRY(DE_ARRAY_PUSH(&h->stack, u32, start));
    DE_TRY(DE_ARRAY_PUSH(&h->visible, u32, start));
    /* Depth-first, edges in order 0, 1, 2: a fixed order for a fixed mesh. */
    while (h->stack.count > 0) {
        u32 fi;
        DE_ARRAY_POP(&h->stack, u32, &fi);
        for (u32 e = 0; e < 3; ++e) {
            const u32 ni = face_at(h, fi)->n[e];
            qh_face *nb = face_at(h, ni);
            if (nb->stamp != h->stamp) {
                nb->stamp = h->stamp;
                nb->state = (u8)(face_dist(nb, p) > h->tol ? 1u : 2u);
                if (nb->state == 1) {
                    DE_TRY(DE_ARRAY_PUSH(&h->stack, u32, ni));
                    DE_TRY(DE_ARRAY_PUSH(&h->visible, u32, ni));
                }
            }
        }
    }
    /* Horizon edges, in visible-face order then edge order. */
    for (u32 k = 0; k < h->visible.count; ++k) {
        const qh_face *fa = face_at(h, DE_ARRAY_AT(&h->visible, u32, k));
        for (u32 e = 0; e < 3; ++e) {
            const qh_face *nb = face_at(h, fa->n[e]);
            if (nb->state != 1) {
                qh_horizon hz = {fa->v[e], fa->v[(e + 1) % 3], fa->n[e]};
                DE_TRY(DE_ARRAY_PUSH(&h->horizon, qh_horizon, hz));
            }
        }
    }
    return DE_OK;
}

/* Orders the horizon into one loop (h->loop). false when it is not a single simple loop. */
static bool order_horizon(qh *h) {
    const u32 count = h->horizon.count;
    de_array_clear(&h->loop);
    if (count < 3 || de_array_reserve(&h->loop, count) != DE_OK)
        return false;
    const qh_horizon *hz = DE_ARRAY_DATA(&h->horizon, qh_horizon);
    u32 cur = 0;
    for (u32 step = 0; step < count; ++step) {
        (void)DE_ARRAY_PUSH(&h->loop, qh_horizon, hz[cur]); /* reserved: cannot fail */
        const u32 b = hz[cur].b;
        u32 next = QH_NONE;
        for (u32 k = 0; k < count; ++k) {
            if (hz[k].a == b) {
                if (next != QH_NONE)
                    return false; /* vertex visited twice: not a simple loop */
                next = k;
            }
        }
        if (next == QH_NONE)
            return false;
        cur = next;
    }
    return cur == 0;
}

/* Adds point pi (assigned to face start). Returns DE_OK also when the point is dropped. */
static de_result add_point(qh *h, u32 pi, u32 start) {
    DE_TRY(find_visible(h, pi, start));
    bool ok = order_horizon(h);
    /* Every new triangle must be well-formed: pi at least tol away from each horizon edge's
     * line. */
    const de_dvec3 p = h->pts[pi];
    for (u32 k = 0; ok && k < h->loop.count; ++k) {
        const qh_horizon *e = &DE_ARRAY_AT(&h->loop, qh_horizon, k);
        const de_dvec3 ab = de__d3_sub(h->pts[e->b], h->pts[e->a]);
        const de_dvec3 c = de__d3_cross(de__d3_sub(p, h->pts[e->a]), ab);
        const f64 ab2 = de__d3_dot(ab, ab);
        if (!(ab2 > 0.0) || !(de__d3_dot(c, c) > h->tol * h->tol * ab2))
            ok = false;
    }
    if (!ok) {
        h->assign[pi] = QH_NONE; /* dropped */
        return DE_OK;
    }

    const u32 first_new = h->faces.count;
    const u32 count = h->loop.count;
    DE_TRY(de_array_reserve(&h->faces, h->faces.count + count));
    for (u32 k = 0; k < count; ++k) {
        const qh_horizon e = DE_ARRAY_AT(&h->loop, qh_horizon, k);
        u32 idx;
        DE_TRY(push_face(h, e.a, e.b, pi, &idx));
        qh_face *nf = face_at(h, idx);
        nf->n[0] = e.outside;
        nf->n[1] = first_new + (k + 1) % count;         /* across b -> p */
        nf->n[2] = first_new + (k + count - 1) % count; /* across p -> a */
        qh_face *out = face_at(h, e.outside);
        const u32 j = face_edge(out, e.b, e.a);
        DE_ASSERT(j < 3);
        out->n[j] = idx;
    }
    for (u32 k = 0; k < h->visible.count; ++k)
        face_at(h, DE_ARRAY_AT(&h->visible, u32, k))->alive = 0;
    h->is_vertex[pi] = 1;
    ++h->vertex_count;
    h->assign[pi] = QH_NONE;
    /* Orphans: points whose face just died, in index order. */
    for (u32 i = 0; i < h->n; ++i) {
        const u32 f = h->assign[i];
        if (f != QH_NONE && !face_at(h, f)->alive)
            assign_point(h, i, first_new);
    }
    return DE_OK;
}

static de_result run_quickhull(qh *h) {
    DE_TRY(initial_simplex(h));
    for (u32 i = 0; i < h->n; ++i) {
        if (h->is_vertex[i])
            h->assign[i] = QH_NONE;
        else
            assign_point(h, i, 0);
    }
    for (;;) {
        u32 best = QH_NONE;
        f64 best_d = 0.0;
        for (u32 i = 0; i < h->n; ++i) {
            if (h->assign[i] != QH_NONE && (best == QH_NONE || h->dist[i] > best_d)) {
                best = i;
                best_d = h->dist[i];
            }
        }
        if (best == QH_NONE || h->vertex_count >= DE_SHAPE_MAX_HULL_VERTICES)
            return DE_OK;
        DE_TRY(add_point(h, best, h->assign[best]));
    }
}

/* ---- Faces: merge, loops, output ------------------------------------------------------- */

typedef struct out_face {
    u32 first; /* into the loop vertex array */
    u32 count;
} out_face;

typedef struct merge_ctx {
    const qh_face *tris;
    const f64 *area2;
} merge_ctx;

static int cmp_area_desc(const void *a, const void *b, void *user) {
    const merge_ctx *m = (const merge_ctx *)user;
    const f64 x = m->area2[*(const u32 *)a], y = m->area2[*(const u32 *)b];
    return x > y ? -1 : (x < y ? 1 : 0);
}

typedef struct faces_out {
    de_array faces; /* out_face */
    de_array verts; /* u32 input indices, face loops back to back */
} faces_out;

/* Faces = the triangles themselves (the fallback). */
static de_result faces_from_tris(const qh_face *tris, u32 nt, faces_out *o) {
    de_array_clear(&o->faces);
    de_array_clear(&o->verts);
    for (u32 t = 0; t < nt; ++t) {
        out_face f = {o->verts.count, 3};
        DE_TRY(DE_ARRAY_PUSH(&o->faces, out_face, f));
        DE_TRY(de_array_push_n(&o->verts, tris[t].v, 3));
    }
    return DE_OK;
}

/*
 * Coplanar merge (step 4). `tris` are the alive triangles; `remap[i]` maps a face index of the
 * quickhull to its position in `tris`. Returns false (o unchanged meaning) when a group is not a
 * disc with one loop or a face would lose too many vertices; the caller then uses the triangles.
 */
static de_result faces_merged(qh *h, const qh_face *tris, u32 nt, const u32 *remap, faces_out *o,
                              bool *merged) {
    *merged = false;
    const de_allocator *a = h->a;
    const size_t bytes = (sizeof(u32) * 3 + sizeof(f64)) * (size_t)nt + sizeof(u32) * h->n;
    u8 *mem = (u8 *)DE_ALLOC(a, bytes, 8, DE_MEM_PHYSICS);
    if (mem == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "convex hull: merge scratch");
    f64 *area2 = (f64 *)mem;
    u32 *order = (u32 *)(area2 + nt);
    u32 *group = order + nt;
    u32 *queue = group + nt;
    u32 *vcount = queue + nt;
    de_result r = DE_OK;

    for (u32 t = 0; t < nt; ++t) {
        const de_dvec3 pa = h->pts[tris[t].v[0]];
        const de_dvec3 c = de__d3_cross(de__d3_sub(h->pts[tris[t].v[1]], pa),
                                        de__d3_sub(h->pts[tris[t].v[2]], pa));
        area2[t] = de__d3_dot(c, c);
        order[t] = t;
        group[t] = QH_NONE;
    }
    memset(vcount, 0, sizeof(u32) * h->n); /* per-group loop marks first, counts later */
    merge_ctx mc = {tris, area2};
    r = de_sort_stable(order, nt, sizeof(u32), cmp_area_desc, &mc, a);
    if (r != DE_OK)
        goto done;

    de_array_clear(&o->faces);
    de_array_clear(&o->verts);
    u32 ngroups = 0;
    for (u32 oi = 0; oi < nt; ++oi) {
        const u32 seed = order[oi];
        if (group[seed] != QH_NONE)
            continue;
        const u32 g = ngroups++;
        const qh_face *sf = &tris[seed];
        u32 qn = 0, qh_ = 0;
        group[seed] = g;
        queue[qn++] = seed;
        while (qh_ < qn) {
            const u32 t = queue[qh_++];
            for (u32 e = 0; e < 3; ++e) {
                const u32 nb = remap[tris[t].n[e]];
                if (nb == QH_NONE || group[nb] != QH_NONE)
                    continue;
                const qh_face *nf = &tris[nb];
                bool in = de__d3_dot(nf->normal, sf->normal) > 0.0;
                for (u32 k = 0; in && k < 3; ++k)
                    in = de__absd(face_dist(sf, h->pts[nf->v[k]])) <= h->tol;
                if (in) {
                    group[nb] = g;
                    queue[qn++] = nb;
                }
            }
        }
        /* Boundary loop of the group (qn triangles in queue[0..qn)). */
        const u32 loop_start = o->verts.count;
        u32 boundary = 0, st = QH_NONE, se = 0;
        for (u32 k = 0; k < qn; ++k) {
            const u32 t = queue[k];
            for (u32 e = 0; e < 3; ++e) {
                const u32 nb = remap[tris[t].n[e]];
                if (nb == QH_NONE || group[nb] != g) {
                    ++boundary;
                    if (st == QH_NONE) {
                        st = t;
                        se = e;
                    }
                }
            }
        }
        if (boundary == 0) {
            r = DE_OK; /* a group with no boundary (degenerate): fall back to triangles */
            goto done;
        }
        /* Walk: from boundary edge (t, e) = a -> b, the next boundary edge starts at b. Turn
         * around b through the group's triangles until a boundary edge leaves b. */
        u32 t = st, e = se, walked = 0;
        bool ok = true;
        do {
            const u32 v = tris[t].v[e];
            if (vcount[v] == g + 1u) {
                ok = false; /* the loop passes a vertex twice: not a simple polygon */
                break;
            }
            vcount[v] = g + 1u;
            r = DE_ARRAY_PUSH(&o->verts, u32, v);
            if (r != DE_OK)
                goto done;
            ++walked;
            /* Next edge of the same triangle starts at b. */
            u32 nt_ = t, ne = (e + 1) % 3;
            for (u32 guard = 0; guard < nt + 1; ++guard) {
                const u32 nb = remap[tris[nt_].n[ne]];
                if (nb == QH_NONE || group[nb] != g)
                    break; /* boundary edge found */
                /* Cross into nb: the edge b -> x of nb follows its edge x -> b. */
                const u32 bvert = tris[nt_].v[ne];
                const u32 j = face_edge(&tris[nb], tris[nt_].v[(ne + 1) % 3], bvert);
                if (j >= 3) {
                    ok = false;
                    break;
                }
                nt_ = nb;
                ne = (j + 1) % 3;
            }
            t = nt_;
            e = ne;
        } while (ok && walked <= boundary && !(t == st && e == se));
        if (!ok || walked != boundary) {
            r = DE_OK; /* not a single simple loop: fall back to triangles */
            goto done;
        }
        out_face f = {loop_start, walked};
        r = DE_ARRAY_PUSH(&o->faces, out_face, f);
        if (r != DE_OK)
            goto done;
    }

    /* Remove vertices that lie on fewer than three faces (middle of an edge or a face). */
    memset(vcount, 0, sizeof(u32) * h->n);
    const u32 *lv = DE_ARRAY_DATA(&o->verts, u32);
    for (u32 k = 0; k < o->verts.count; ++k)
        ++vcount[lv[k]];
    {
        u32 w = 0;
        u32 *verts = DE_ARRAY_DATA(&o->verts, u32);
        out_face *faces = DE_ARRAY_DATA(&o->faces, out_face);
        for (u32 fi = 0; fi < o->faces.count; ++fi) {
            const u32 first = w;
            for (u32 k = 0; k < faces[fi].count; ++k) {
                const u32 v = verts[faces[fi].first + k];
                if (vcount[v] >= 3)
                    verts[w++] = v;
            }
            faces[fi].first = first;
            faces[fi].count = w - first;
            if (faces[fi].count < 3)
                goto done; /* r == DE_OK, merged false: fall back */
        }
        o->verts.count = w;
    }
    *merged = true;
done:
    DE_FREE(a, mem, bytes);
    return r;
}

static int cmp_u32_key(const void *x, const void *y, void *user) {
    (void)user;
    const u32 a = ((const de_sort_key32 *)x)->key, b = ((const de_sort_key32 *)y)->key;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* Writes the output hull from face loops. false in *ok when a twin is missing. */
static de_result emit(qh *h, const faces_out *o, de__hull *out, void **out_block, size_t *out_size,
                      bool *ok) {
    *ok = false;
    const de_allocator *a = h->a;
    const u32 *lv = (const u32 *)o->verts.data;
    const out_face *of = (const out_face *)o->faces.data;
    const u32 nf = o->faces.count, ne = o->verts.count;

    /* Vertices: every loop vertex, ascending input index. */
    u32 *vmap = (u32 *)DE_ALLOC(a, sizeof(u32) * h->n, DE_ALIGNOF(u32), DE_MEM_PHYSICS);
    if (vmap == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "convex hull: vertex map");
    for (u32 i = 0; i < h->n; ++i)
        vmap[i] = QH_NONE;
    for (u32 k = 0; k < ne; ++k)
        vmap[lv[k]] = 0;
    u32 nv = 0;
    for (u32 i = 0; i < h->n; ++i)
        if (vmap[i] != QH_NONE)
            vmap[i] = nv++;
    if (nv > DE_SHAPE_MAX_HULL_VERTICES || ne > 0xFFFEu || nf > 0xFFFEu) {
        DE_FREE(a, vmap, sizeof(u32) * h->n);
        return DE_FAIL(DE_ERR_INTERNAL, "convex hull: output too large");
    }

    /* One block: vertices, planes, faces, edges. */
    const size_t off_planes = (sizeof(de_vec3) * nv + 15u) & ~(size_t)15u;
    const size_t off_faces = off_planes + ((sizeof(de_plane) * nf + 15u) & ~(size_t)15u);
    const size_t off_edges = off_faces + ((sizeof(de__hull_face) * nf + 15u) & ~(size_t)15u);
    const size_t size = off_edges + ((sizeof(de__hull_edge) * ne + 15u) & ~(size_t)15u);
    u8 *block = (u8 *)DE_ALLOC(a, size, 16, DE_MEM_PHYSICS);
    de_sort_key32 *keys = block != NULL
                              ? (de_sort_key32 *)DE_ALLOC(a, sizeof(de_sort_key32) * ne,
                                                          DE_ALIGNOF(de_sort_key32), DE_MEM_PHYSICS)
                              : NULL;
    if (keys == NULL) {
        if (block != NULL)
            DE_FREE(a, block, size);
        DE_FREE(a, vmap, sizeof(u32) * h->n);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "convex hull: output");
    }
    memset(block, 0, size);
    de__hull hull;
    memset(&hull, 0, sizeof hull);
    hull.vertex_count = nv;
    hull.face_count = nf;
    hull.edge_count = ne;
    hull.vertices = (de_vec3 *)block;
    hull.planes = (de_plane *)(block + off_planes);
    hull.faces = (de__hull_face *)(block + off_faces);
    hull.edges = (de__hull_edge *)(block + off_edges);
    hull.tolerance = (f32)h->tol;

    de_dvec3 sum = de__d3(0.0, 0.0, 0.0);
    for (u32 i = 0; i < h->n; ++i) {
        if (vmap[i] == QH_NONE)
            continue;
        const de_dvec3 p = h->pts[i];
        hull.vertices[vmap[i]] = de__v3((f32)p.x, (f32)p.y, (f32)p.z);
        sum = de__d3_add(sum, p);
    }
    hull.centroid = de__v3((f32)(sum.x / nv), (f32)(sum.y / nv), (f32)(sum.z / nv));

    for (u32 f = 0; f < nf; ++f) {
        const u32 first = of[f].first, cnt = of[f].count;
        hull.faces[f].first_edge = (u16)first;
        hull.faces[f].edge_count = (u16)cnt;
        /* Newell normal over the loop. */
        de_dvec3 n = de__d3(0.0, 0.0, 0.0);
        for (u32 k = 0; k < cnt; ++k) {
            const de_dvec3 p = h->pts[lv[first + k]];
            const de_dvec3 q = h->pts[lv[first + (k + 1) % cnt]];
            n.x += (p.y - q.y) * (p.z + q.z);
            n.y += (p.z - q.z) * (p.x + q.x);
            n.z += (p.x - q.x) * (p.y + q.y);
        }
        const f64 len2 = de__d3_dot(n, n);
        if (!(len2 > 0.0)) {
            DE_FREE(a, keys, sizeof(de_sort_key32) * ne);
            DE_FREE(a, block, size);
            DE_FREE(a, vmap, sizeof(u32) * h->n);
            return DE_OK; /* *ok false: caller falls back */
        }
        n = de__d3_scale(n, 1.0 / de_dsqrt64(len2));
        /* Plane through the outermost vertex of the face. */
        f64 dmax = de__d3_dot(n, h->pts[lv[first]]);
        for (u32 k = 1; k < cnt; ++k)
            dmax = de__maxd(dmax, de__d3_dot(n, h->pts[lv[first + k]]));
        hull.planes[f].n = de__v3((f32)n.x, (f32)n.y, (f32)n.z);
        hull.planes[f].d = (f32)(-dmax);
        for (u32 k = 0; k < cnt; ++k) {
            de__hull_edge *e = &hull.edges[first + k];
            e->origin = (u16)vmap[lv[first + k]];
            e->next = (u16)(first + (k + 1) % cnt);
            e->face = (u16)f;
            e->twin = (u16)DE__HULL_NONE;
            keys[first + k].key = ((u32)e->origin << 16) | (u32)vmap[lv[first + (k + 1) % cnt]];
            keys[first + k].index = first + k;
        }
    }
    de_result r = de_sort_stable(keys, ne, sizeof *keys, cmp_u32_key, NULL, a);
    bool twins_ok = r == DE_OK;
    for (u32 k = 0; twins_ok && k < ne; ++k) {
        de__hull_edge *e = &hull.edges[k];
        const u32 dest = hull.edges[e->next].origin;
        const u32 want = (dest << 16) | e->origin;
        u32 lo = 0, hi = ne;
        while (lo < hi) {
            const u32 mid = lo + (hi - lo) / 2u;
            if (keys[mid].key < want)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo >= ne || keys[lo].key != want || (lo + 1 < ne && keys[lo + 1].key == want))
            twins_ok = false;
        else
            e->twin = (u16)keys[lo].index;
    }
    DE_FREE(a, keys, sizeof(de_sort_key32) * ne);
    DE_FREE(a, vmap, sizeof(u32) * h->n);
    if (r != DE_OK || !twins_ok) {
        DE_FREE(a, block, size);
        return r;
    }
    *out = hull;
    *out_block = block;
    *out_size = size;
    *ok = true;
    return DE_OK;
}

f32 de__hull_auto_tolerance(const de_vec3 *points, u32 n) {
    if (n == 0)
        return 1e-5f;
    de_vec3 lo = points[0], hi = points[0];
    f32 amax = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        lo = de__v3_min(lo, points[i]);
        hi = de__v3_max(hi, points[i]);
        amax = de__maxf(amax, de__maxf(de__absf(points[i].x),
                                       de__maxf(de__absf(points[i].y), de__absf(points[i].z))));
    }
    const f32 ext = de__maxf(hi.x - lo.x, de__maxf(hi.y - lo.y, hi.z - lo.z));
    return de__maxf(1e-5f, de__maxf(1e-4f * ext, 4e-7f * amax));
}

de_result de__hull_build(const de_allocator *a, const de_vec3 *points, u32 n, f32 tol,
                         de__hull *out, void **out_block, size_t *out_size) {
    *out_block = NULL;
    *out_size = 0;
    memset(out, 0, sizeof *out);
    if (points == NULL || n < 4)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "convex hull: needs at least 4 points");
    if (!(tol > 0.0f) || !de__f32_finite(tol))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "convex hull: tolerance must be > 0");
    for (u32 i = 0; i < n; ++i)
        if (!de__v3_finite(points[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "convex hull: point %u is not finite", i);

    qh h;
    memset(&h, 0, sizeof h);
    h.a = a;
    h.n = n;
    h.tol = tol;
    const size_t bytes = (sizeof(de_dvec3) + sizeof(f64) + sizeof(u32) + 1u) * (size_t)n;
    u8 *mem = (u8 *)DE_ALLOC(a, bytes, 8, DE_MEM_PHYSICS);
    if (mem == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "convex hull: %u points", n);
    memset(mem, 0, bytes);
    h.pts = (de_dvec3 *)mem;
    h.dist = (f64 *)(h.pts + n);
    h.assign = (u32 *)(h.dist + n);
    h.is_vertex = (u8 *)(h.assign + n);
    for (u32 i = 0; i < n; ++i)
        h.pts[i] = de__d3_from(points[i]);
    DE_ARRAY_INIT_TAG(&h.faces, qh_face, a, DE_MEM_PHYSICS);
    DE_ARRAY_INIT_TAG(&h.stack, u32, a, DE_MEM_PHYSICS);
    DE_ARRAY_INIT_TAG(&h.visible, u32, a, DE_MEM_PHYSICS);
    DE_ARRAY_INIT_TAG(&h.horizon, qh_horizon, a, DE_MEM_PHYSICS);
    DE_ARRAY_INIT_TAG(&h.loop, qh_horizon, a, DE_MEM_PHYSICS);
    faces_out o;
    DE_ARRAY_INIT_TAG(&o.faces, out_face, a, DE_MEM_PHYSICS);
    DE_ARRAY_INIT_TAG(&o.verts, u32, a, DE_MEM_PHYSICS);
    de_array tris;
    DE_ARRAY_INIT_TAG(&tris, qh_face, a, DE_MEM_PHYSICS);
    u32 *remap = NULL;

    de_result r = run_quickhull(&h);
    if (r != DE_OK)
        goto done;

    /* Alive triangles, compacted; remap maps quickhull face indices to them. */
    remap = (u32 *)DE_ALLOC(a, sizeof(u32) * (h.faces.count + 1u), DE_ALIGNOF(u32), DE_MEM_PHYSICS);
    if (remap == NULL) {
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "convex hull: remap");
        goto done;
    }
    for (u32 f = 0; f < h.faces.count; ++f) {
        const qh_face *fa = face_at(&h, f);
        remap[f] = QH_NONE;
        if (fa->alive) {
            remap[f] = tris.count;
            r = DE_ARRAY_PUSH(&tris, qh_face, *fa);
            if (r != DE_OK)
                goto done;
        }
    }
    const qh_face *tv = DE_ARRAY_DATA(&tris, qh_face);
    bool merged = false;
    r = faces_merged(&h, tv, tris.count, remap, &o, &merged);
    if (r != DE_OK)
        goto done;
    bool ok = false;
    if (merged) {
        r = emit(&h, &o, out, out_block, out_size, &ok);
        if (r != DE_OK)
            goto done;
    }
    if (!ok) {
        r = faces_from_tris(tv, tris.count, &o);
        if (r != DE_OK)
            goto done;
        r = emit(&h, &o, out, out_block, out_size, &ok);
        if (r == DE_OK && !ok)
            r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "convex hull: degenerate input");
    }

done:
    if (remap != NULL)
        DE_FREE(a, remap, sizeof(u32) * (h.faces.count + 1u));
    de_array_free(&tris);
    de_array_free(&o.faces);
    de_array_free(&o.verts);
    de_array_free(&h.faces);
    de_array_free(&h.stack);
    de_array_free(&h.visible);
    de_array_free(&h.horizon);
    de_array_free(&h.loop);
    DE_FREE(a, mem, bytes);
    return r;
}
