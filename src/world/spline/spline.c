/* spline.c: spline components and curve math (DENG-775). See spline.h. */
#include "world/spline/spline.h"

#include <dengine/de_reflect.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* ---- Registration ------------------------------------------------------------------------- */

#define AUTHORED  DE_FIELD_DEFAULT
#define ASSET_REF (DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF)
#define HIDDEN    (DE_FIELD_DEFAULT | DE_FIELD_EDITOR_HIDDEN)

static de_result last_error_or(de_result fallback) {
    const de_error *e = de_error_last();
    return e != NULL && e->result != DE_OK ? e->result : fallback;
}

/* The id of `d->name`: the registered type when its layout matches, or a new registration. */
static de_result ensure_type(de_type_registry *reg, const de_type_desc *d, de_type_id *out) {
    *out = de_type_find(reg, d->name);
    if (DE_HANDLE_VALID(*out)) {
        const de_type_desc *cur = de_type_get(reg, *out);
        if (cur == NULL || cur->size != d->size || cur->align != d->align ||
            cur->version != d->version || cur->field_count != d->field_count)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "de__spline_register: the type registry has another type called %s",
                           d->name);
        return DE_OK;
    }
    const de_result r = de_type_register_ex(reg, d, out, NULL);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de__spline_register: registering %s", d->name);
    return DE_OK;
}

static void construct_spline(void *obj) {
    DeSpline *s = (DeSpline *)obj;
    memset(s, 0, sizeof *s);
    /* As the mock engine: a 4 m line along +X. */
    s->kind = DE_SPLINE_CATMULL_ROM;
    s->count = 2;
    s->points[0] = (de_spline_point){{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}};
    s->points[1] = (de_spline_point){{4, 0, 0}, {-1, 0, 0}, {1, 0, 0}};
}

static void construct_meshes(void *obj) {
    DeSplineMeshes *m = (DeSplineMeshes *)obj;
    memset(m, 0, sizeof *m);
    m->spacing = 2.0f;
    m->length = 2.0f;
}

static de_result component(de_world *w, de_type_desc d, void (*construct)(void *),
                           de_component *out) {
    *out = de_component_find(w, d.name);
    if (DE_HANDLE_VALID(*out))
        return DE_OK;
    d.hooks.construct = construct;
    de_type_id t;
    DE_TRY(ensure_type(de_world_types(w), &d, &t));
    *out = de_component_register(w, t, DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE);
    if (!DE_HANDLE_VALID(*out))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL), "de__spline_register: component %s",
                            d.name);
    return DE_OK;
}

de_result de__spline_register(de_world *w, de_spline_ids *out) {
    if (w == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__spline_register: NULL argument");
    memset(out, 0, sizeof *out);
    de_type_registry *reg = de_world_types(w);

    static const de_enum_value k_kind[] = {
        {"catmull_rom", DE_SPLINE_CATMULL_ROM},
        {"bezier", DE_SPLINE_BEZIER},
    };
    const de_type_id kind = de_type_find(reg, "de_spline_kind");
    if (DE_HANDLE_VALID(kind)) {
        const de_enum_info *e = de_enum_get(reg, kind);
        if (e == NULL || e->count != 2 || e->is_flags)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "de__spline_register: the type registry has another de_spline_kind");
    } else {
        const de_result r = de_enum_register(reg, "de_spline_kind", k_kind, 2, false);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "de__spline_register: registering de_spline_kind");
    }

    static const de_field_info k_point[] = {
        DE_FIELD_EX(de_spline_point, position, DE_FK_VEC3, AUTHORED, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(de_spline_point, in_tangent, DE_FK_VEC3, AUTHORED,
                    {0, 0, "m", "Offset from the point (Bezier)", 0, 0}),
        DE_FIELD_EX(de_spline_point, out_tangent, DE_FK_VEC3, AUTHORED,
                    {0, 0, "m", "Offset from the point (Bezier)", 0, 0}),
    };
    de_type_id point;
    DE_TRY(ensure_type(reg, &DE_TYPE_DESC(de_spline_point, 1, k_point), &point));

    static const de_field_info k_spline[] = {
        DE_FIELD_EX(DeSpline, kind, DE_FK_ENUM, AUTHORED, {0, 0, 0, 0, "de_spline_kind", 0}),
        DE_FIELD(DeSpline, closed, DE_FK_BOOL, AUTHORED),
        DE_FIELD_EX(DeSpline, count, DE_FK_U32, HIDDEN,
                    {0, DE_SPLINE_MAX_POINTS, 0, "Points in use", 0, 0}),
        DE_FIELD_EX(DeSpline, points, DE_FK_FIXED_ARRAY, HIDDEN,
                    {0, 0, 0, 0, "de_spline_point", DE_SPLINE_MAX_POINTS}),
    };
    DE_TRY(component(w, DE_TYPE_DESC(DeSpline, 1, k_spline), construct_spline, &out->spline));

    static const de_field_info k_meshes[] = {
        DE_FIELD_EX(DeSplineMeshes, mesh, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "model", 0}),
        DE_FIELD_EX(DeSplineMeshes, spacing, DE_FK_F32, AUTHORED, {0.1, 100, "m", 0, 0, 0}),
        DE_FIELD_EX(DeSplineMeshes, length, DE_FK_F32, AUTHORED,
                    {0.1, 100, "m", "The mesh's length along +X", 0, 0}),
        DE_FIELD(DeSplineMeshes, deform, DE_FK_BOOL, AUTHORED),
        DE_FIELD_EX(DeSplineMeshes, offset, DE_FK_F32, AUTHORED, {0, 100, "m", 0, 0, 0}),
    };
    DE_TRY(component(w, DE_TYPE_DESC(DeSplineMeshes, 1, k_meshes), construct_meshes, &out->meshes));
    return DE_OK;
}

/* ---- Curve -------------------------------------------------------------------------------- */

static de_vec3 v_add(de_vec3 a, de_vec3 b) {
    return (de_vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}
static de_vec3 v_sub(de_vec3 a, de_vec3 b) {
    return (de_vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}
static de_vec3 v_mul(de_vec3 a, f32 s) {
    return (de_vec3){a.x * s, a.y * s, a.z * s};
}
static f32 v_len(de_vec3 a) {
    return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}

/* A finite vector, or 0 (a NaN written by an edit must not poison the curve). */
static de_vec3 finite3(de_vec3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z) ? v : (de_vec3){0, 0, 0};
}

typedef struct curve {
    de_vec3 p[DE_SPLINE_MAX_POINTS], in[DE_SPLINE_MAX_POINTS], out[DE_SPLINE_MAX_POINTS];
    u32 n;
    bool bezier, closed;
} curve;

/* The control points in world space: positions by the full transform, tangents by its rotation
 * and scale (they are offsets). */
static void curve_of(const DeSpline *s, const de_transform *t, curve *c) {
    c->n = s->count < DE_SPLINE_MAX_POINTS ? s->count : DE_SPLINE_MAX_POINTS;
    c->bezier = s->kind == DE_SPLINE_BEZIER;
    c->closed = s->closed;
    for (u32 i = 0; i < c->n; ++i) {
        const de_spline_point *q = &s->points[i];
        const de_vec3 p = finite3(q->position), ti = finite3(q->in_tangent),
                      to = finite3(q->out_tangent);
        if (t == NULL) {
            c->p[i] = p;
            c->in[i] = ti;
            c->out[i] = to;
            continue;
        }
        const de_dvec3 w = de_transform_point(t, (de_dvec3){p.x, p.y, p.z});
        c->p[i] = (de_vec3){(f32)w.x, (f32)w.y, (f32)w.z};
        const de_vec3 sc = t->scale;
        c->in[i] = de_quat_rotate(t->rotation, (de_vec3){ti.x * sc.x, ti.y * sc.y, ti.z * sc.z});
        c->out[i] = de_quat_rotate(t->rotation, (de_vec3){to.x * sc.x, to.y * sc.y, to.z * sc.z});
    }
}

static u32 segments(const curve *c) {
    if (c->n < 2)
        return 0;
    return c->closed ? c->n : c->n - 1;
}

/* Catmull-Rom neighbour i: wraps when closed, else mirrors the end points. */
static de_vec3 pos(const curve *c, i32 i) {
    const i32 n = (i32)c->n;
    if (c->closed)
        return c->p[((i % n) + n) % n];
    if (i < 0)
        return v_sub(v_mul(c->p[0], 2.0f), c->p[1]);
    if (i >= n)
        return v_sub(v_mul(c->p[n - 1], 2.0f), c->p[n - 2]);
    return c->p[i];
}

static de_vec3 eval(const curve *c, u32 s, f32 t) {
    if (c->bezier) {
        const u32 b = (s + 1) % c->n;
        const de_vec3 b0 = c->p[s], b1 = v_add(c->p[s], c->out[s]), b2 = v_add(c->p[b], c->in[b]),
                      b3 = c->p[b];
        const f32 u = 1.0f - t;
        return v_add(v_add(v_mul(b0, u * u * u), v_mul(b1, 3.0f * u * u * t)),
                     v_add(v_mul(b2, 3.0f * u * t * t), v_mul(b3, t * t * t)));
    }
    const i32 i = (i32)s;
    const de_vec3 p0 = pos(c, i - 1), p1 = pos(c, i), p2 = pos(c, i + 1), p3 = pos(c, i + 2);
    const f32 t2 = t * t, t3 = t2 * t;
    const f32 a[3][4] = {
        {p0.x, p1.x, p2.x, p3.x}, {p0.y, p1.y, p2.y, p3.y}, {p0.z, p1.z, p2.z, p3.z}};
    f32 o[3];
    for (u32 k = 0; k < 3; ++k)
        o[k] = 0.5f * (2.0f * a[k][1] + (-a[k][0] + a[k][2]) * t +
                       (2.0f * a[k][0] - 5.0f * a[k][1] + 4.0f * a[k][2] - a[k][3]) * t2 +
                       (-a[k][0] + 3.0f * a[k][1] - 3.0f * a[k][2] + a[k][3]) * t3);
    return (de_vec3){o[0], o[1], o[2]};
}

void de__spline_poly(const DeSpline *s, const de_transform *world, de_spline_poly *out) {
    curve c;
    curve_of(s, world, &c);
    out->closed = c.closed;
    const u32 segs = segments(&c);
    if (segs == 0) {
        out->n = c.n;
        for (u32 i = 0; i < c.n; ++i) {
            out->p[i] = c.p[i];
            out->at[i] = 0.0f;
        }
        return;
    }
    u32 n = 0;
    for (u32 sg = 0; sg < segs; ++sg)
        for (u32 k = 0; k < DE_SPLINE_DENSE; ++k)
            out->p[n++] = eval(&c, sg, (f32)k / (f32)DE_SPLINE_DENSE);
    out->p[n++] = eval(&c, segs - 1, 1.0f);
    out->n = n;
    f32 total = 0.0f;
    out->at[0] = 0.0f;
    for (u32 i = 1; i < n; ++i) {
        total += v_len(v_sub(out->p[i], out->p[i - 1]));
        out->at[i] = total;
    }
}

void de__spline_at(const de_spline_poly *p, f32 d, de_vec3 *pos_out, de_vec3 *dir_out) {
    if (p->n < 2) {
        *pos_out = p->n ? p->p[0] : (de_vec3){0, 0, 0};
        *dir_out = (de_vec3){1, 0, 0};
        return;
    }
    const f32 len = de__spline_length(p);
    d = d < 0.0f ? 0.0f : d > len ? len : d;
    /* First index with at > d (partition point), clamped to [1, n - 1]. */
    u32 lo = 0, hi = p->n;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        if (p->at[mid] <= d)
            lo = mid + 1;
        else
            hi = mid;
    }
    const u32 i = lo < 1 ? 1 : lo > p->n - 1 ? p->n - 1 : lo;
    const de_vec3 a = p->p[i - 1], b = p->p[i];
    const f32 piece = p->at[i] - p->at[i - 1];
    const f32 t = piece > 0.0f ? (d - p->at[i - 1]) / piece : 0.0f;
    const de_vec3 dir = v_sub(b, a);
    const f32 l = v_len(dir);
    *dir_out = l > 0.0f ? v_mul(dir, 1.0f / l) : (de_vec3){1, 0, 0};
    *pos_out = v_add(a, v_mul(dir, t));
}

u32 de__spline_sample(const de_spline_poly *p, f32 step, f32 *pos_out, f32 *dir_out,
                      f32 *dist_out) {
    const f32 len = de__spline_length(p);
    const f32 min_step = len / (f32)(DE_SPLINE_MAX_OUT - 1);
    if (step < min_step)
        step = min_step;
    u32 n = 0;
    for (f32 d = 0.0f; d < len - 1e-4f && n < DE_SPLINE_MAX_OUT - 1; d += step, ++n) {
        de_vec3 q, t;
        de__spline_at(p, d, &q, &t);
        memcpy(pos_out + 3 * n, &q, sizeof q);
        memcpy(dir_out + 3 * n, &t, sizeof t);
        dist_out[n] = d;
    }
    de_vec3 q, t;
    de__spline_at(p, len, &q, &t);
    memcpy(pos_out + 3 * n, &q, sizeof q);
    memcpy(dir_out + 3 * n, &t, sizeof t);
    dist_out[n] = len;
    return n + 1;
}

/* The rotation taking +X to the unit vector `t` (the shortest arc). */
static void align_x(de_vec3 t, f32 q[4]) {
    f32 v[4] = {0.0f, -t.z, t.y, 1.0f + t.x};
    const f32 n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
    if (n < 1e-6f) {
        q[0] = 0.0f, q[1] = 1.0f, q[2] = 0.0f, q[3] = 0.0f; /* -X: half a turn about Y */
        return;
    }
    for (u32 k = 0; k < 4; ++k)
        q[k] = v[k] / n;
}

static void put(const de_spline_poly *p, f32 d, f32 sx, f32 *xf) {
    de_vec3 q, t;
    de__spline_at(p, d, &q, &t);
    xf[0] = q.x, xf[1] = q.y, xf[2] = q.z;
    align_x(t, xf + 3);
    xf[7] = sx, xf[8] = 1.0f, xf[9] = 1.0f;
}

static f32 finite_or(f32 v, f32 d) {
    return isfinite(v) ? v : d;
}

u32 de__spline_place(const de_spline_poly *p, const DeSplineMeshes *m, f32 *xf) {
    const f32 total = de__spline_length(p);
    if (!(total > 0.0f))
        return 0;
    f32 spacing = finite_or(m->spacing, 2.0f), mesh_len = finite_or(m->length, 2.0f),
        offset = finite_or(m->offset, 0.0f);
    spacing = spacing < 0.1f ? 0.1f : spacing;
    mesh_len = mesh_len < 0.1f ? 0.1f : mesh_len;
    offset = offset < 0.0f ? 0.0f : offset;
    u32 n = 0;
    if (m->deform) {
        const f32 usable = total - offset > 0.0f ? total - offset : 0.0f;
        f32 pieces = roundf(usable / mesh_len);
        pieces =
            pieces<1.0f ? 1.0f : pieces>(f32) DE_SPLINE_MAX_OUT ? (f32)DE_SPLINE_MAX_OUT : pieces;
        const u32 count = (u32)pieces;
        const f32 piece = usable / (f32)count;
        for (u32 k = 0; k < count; ++k)
            put(p, offset + ((f32)k + 0.5f) * piece, piece / mesh_len, xf + 10 * n++);
        return n;
    }
    const f32 end = p->closed ? total - 1e-4f : total + 1e-4f;
    for (u32 k = 0; n < DE_SPLINE_MAX_OUT; ++k) {
        const f32 d = offset + (f32)k * spacing;
        if (d > end)
            break;
        put(p, d, 1.0f, xf + 10 * n++);
    }
    return n;
}
