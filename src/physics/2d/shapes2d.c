/*
 * shapes2d.c: 2D shape slots, de_shape2d_create / destroy, the shape getters, support functions
 * and world AABBs (DENG-252). SPEC §13.3. Polygons are built in polygon2d.c, chains in chain2d.c,
 * mass properties in mass2d.c.
 */
#include "physics/2d/p2d_internal.h"

/* ---- Slots ----------------------------------------------------------------------------- */

de__shape2d *de__shape2d_get(const de_physics2d_world *p, de_shape h) {
    if (p == NULL || h.id == 0)
        return NULL;
    const u32 i = de__handle_index(h.id);
    if (i >= p->shape_slot_count)
        return NULL;
    const de__shape2d_slot *slot = &p->shape_slots[i];
    if (slot->shape == NULL || slot->generation != de__handle_gen(h.id))
        return NULL;
    return slot->shape;
}

/* Makes room for one more slot. Slot arrays may move; shapes never do. */
static de_result slots_reserve(de_physics2d_world *p) {
    if (p->shape_free_count > 0 || p->shape_slot_count < p->shape_slot_capacity)
        return DE_OK;
    const u32 limit = DE__HANDLE_INDEX_MASK - 1u;
    if (p->shape_slot_capacity >= limit)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape2d_create: too many shapes (%u)", limit);
    u32 cap = p->shape_slot_capacity < 16u ? 16u : p->shape_slot_capacity * 2u;
    if (cap > limit)
        cap = limit;
    const u32 old = p->shape_slot_capacity;
    /* New arrays, then copy: a failure leaves the old ones untouched. */
    de__shape2d_slot *slots = (de__shape2d_slot *)DE_ALLOC(
        p->alloc, sizeof(de__shape2d_slot) * cap, DE_ALIGNOF(de__shape2d_slot), DE_MEM_PHYSICS);
    u32 *free_list = slots != NULL ? (u32 *)DE_ALLOC(p->alloc, sizeof(u32) * cap, DE_ALIGNOF(u32),
                                                     DE_MEM_PHYSICS)
                                   : NULL;
    if (free_list == NULL) {
        if (slots != NULL)
            DE_FREE(p->alloc, slots, sizeof(de__shape2d_slot) * cap);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape2d_create: shape slots");
    }
    memset(slots, 0, sizeof(de__shape2d_slot) * cap);
    if (old > 0) {
        memcpy(slots, p->shape_slots, sizeof(de__shape2d_slot) * old);
        memcpy(free_list, p->shape_free, sizeof(u32) * old);
        DE_FREE(p->alloc, p->shape_slots, sizeof(de__shape2d_slot) * old);
        DE_FREE(p->alloc, p->shape_free, sizeof(u32) * old);
    }
    p->shape_slots = slots;
    p->shape_free = free_list;
    p->shape_slot_capacity = cap;
    return DE_OK;
}

void de__shape2d_free(de_physics2d_world *p, de__shape2d *s) {
    if (s->data != NULL)
        DE_FREE(p->alloc, s->data, s->data_size);
    DE_FREE(p->alloc, s, sizeof *s);
}

void de__shapes2d_free_all(de_physics2d_world *p) {
    for (u32 i = 0; i < p->shape_slot_count; ++i) {
        de__shape2d *s = p->shape_slots[i].shape;
        if (s == NULL)
            continue;
        de__shape2d_free(p, s);
        p->shape_slots[i].shape = NULL;
    }
    if (p->shape_slots != NULL)
        DE_FREE(p->alloc, p->shape_slots, sizeof(de__shape2d_slot) * p->shape_slot_capacity);
    if (p->shape_free != NULL)
        DE_FREE(p->alloc, p->shape_free, sizeof(u32) * p->shape_slot_capacity);
    p->shape_slots = NULL;
    p->shape_free = NULL;
    p->shape_slot_count = p->shape_slot_capacity = p->shape_free_count = 0;
    p->shape_live_count = 0;
}

de_result de__shape2d_insert(de_physics2d_world *p, de__shape2d *s) {
    DE_TRY(slots_reserve(p));
    u32 i;
    if (p->shape_free_count > 0) {
        i = p->shape_free[--p->shape_free_count];
    } else {
        i = p->shape_slot_count++;
        p->shape_slots[i].generation = 1; /* generations start at 1, like bodies */
    }
    de__shape2d_slot *slot = &p->shape_slots[i];
    slot->shape = s;
    s->handle.id = de__handle_make(i, slot->generation);
    ++p->shape_live_count;
    return DE_OK;
}

/* ---- AABB helpers ---------------------------------------------------------------------- */

/* Pads x away from the box by a few f64 ulps of |x| before rounding to f32 (as in 3D). */
static f32 lower(f64 x) {
    return de__f32_round_down(x - de__absd(x) * 0x1p-50);
}
static f32 upper(f64 x) {
    return de__f32_round_up(x + de__absd(x) * 0x1p-50);
}

de_aabb2d de__aabb2d_from_d(de__d2 lo, de__d2 hi) {
    de_aabb2d b;
    b.min = de__v2(lower(lo.x), lower(lo.y));
    b.max = de__v2(upper(hi.x), upper(hi.y));
    return b;
}

static de_aabb2d aabb_around(de__d2 c, f64 ex, f64 ey) {
    return de__aabb2d_from_d(de__d2_make(c.x - ex, c.y - ey), de__d2_make(c.x + ex, c.y + ey));
}

de_aabb2d de__aabb2d_transform(de_aabb2d local, de__d2 pos, de_rot2d rot) {
    if (de__aabb2d_is_empty(local))
        return local;
    const de__d2 c =
        de__d2_make(((f64)local.min.x + local.max.x) * 0.5, ((f64)local.min.y + local.max.y) * 0.5);
    const f64 ex = ((f64)local.max.x - local.min.x) * 0.5;
    const f64 ey = ((f64)local.max.y - local.min.y) * 0.5;
    const de__d2 wc = de__d2_add(pos, de__rot2d_apply_d(rot, c));
    /* Arvo: each world half extent is the sum of |R_ij| * e_j over j. */
    const f64 ac = de__absd((f64)rot.c), as = de__absd((f64)rot.s);
    return aabb_around(wc, ac * ex + as * ey, as * ex + ac * ey);
}

/* Running f64 min / max of transformed points. */
typedef struct box_acc {
    de__d2 lo, hi;
} box_acc;

static void box_init(box_acc *b, de__d2 p) {
    b->lo = p;
    b->hi = p;
}
static void box_add(box_acc *b, de__d2 p) {
    b->lo = de__d2_make(de__mind(b->lo.x, p.x), de__mind(b->lo.y, p.y));
    b->hi = de__d2_make(de__maxd(b->hi.x, p.x), de__maxd(b->hi.y, p.y));
}
static de_aabb2d box_finish(const box_acc *b, f64 r) {
    return de__aabb2d_from_d(de__d2_make(b->lo.x - r, b->lo.y - r),
                             de__d2_make(b->hi.x + r, b->hi.y + r));
}

static de__d2 xf_point(de__d2 pos, de_rot2d rot, de_vec2 v) {
    return de__d2_add(pos, de__rot2d_apply_d(rot, de__d2_from(v)));
}

de_aabb2d de__shape2d_world_aabb(const de__shape2d *s, de__d2 pos, de_rot2d rot) {
    box_acc b;
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE: {
        const f64 r = s->u.circle.radius;
        return aabb_around(xf_point(pos, rot, s->u.circle.center), r, r);
    }
    case DE_SHAPE2D_CAPSULE:
        box_init(&b, xf_point(pos, rot, s->u.capsule.p1));
        box_add(&b, xf_point(pos, rot, s->u.capsule.p2));
        return box_finish(&b, s->u.capsule.radius);
    case DE_SHAPE2D_SEGMENT:
        box_init(&b, xf_point(pos, rot, s->u.segment.p1));
        box_add(&b, xf_point(pos, rot, s->u.segment.p2));
        return box_finish(&b, 0.0);
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON: {
        const de__polygon2d *poly = &s->u.polygon;
        box_init(&b, xf_point(pos, rot, poly->vertices[0]));
        for (u32 i = 1; i < poly->count; ++i)
            box_add(&b, xf_point(pos, rot, poly->vertices[i]));
        return box_finish(&b, poly->radius);
    }
    default:
        return de__aabb2d_transform(s->local_aabb, pos, rot);
    }
}

/* ---- Support --------------------------------------------------------------------------- */

static de_vec2 segment_support(de_vec2 p1, de_vec2 p2, de_vec2 dir) {
    return de__v2_dot(p2, dir) > de__v2_dot(p1, dir) ? p2 : p1;
}

de_vec2 de__shape2d_support_core(const de__shape2d *s, de_vec2 dir) {
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE:
        return s->u.circle.center;
    case DE_SHAPE2D_CAPSULE:
        return segment_support(s->u.capsule.p1, s->u.capsule.p2, dir);
    case DE_SHAPE2D_SEGMENT:
        return segment_support(s->u.segment.p1, s->u.segment.p2, dir);
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON: {
        const de__polygon2d *poly = &s->u.polygon;
        u32 best = 0;
        f32 best_d = de__v2_dot(poly->vertices[0], dir);
        for (u32 i = 1; i < poly->count; ++i) {
            const f32 d = de__v2_dot(poly->vertices[i], dir);
            if (d > best_d) {
                best_d = d;
                best = i;
            }
        }
        return poly->vertices[best];
    }
    default:
        return de__v2(0.0f, 0.0f);
    }
}

de_vec2 de__shape2d_support(const de__shape2d *s, de_vec2 dir) {
    const de_vec2 core = de__shape2d_support_core(s, dir);
    if (s->radius == 0.0f)
        return core;
    de_vec2 n = de__v2_normalize_d(de__d2_from(dir));
    if (n.x == 0.0f && n.y == 0.0f)
        n = de__v2(1.0f, 0.0f);
    return de__v2_add(core, de__v2_scale(n, s->radius));
}

/* ---- Builders -------------------------------------------------------------------------- */

static bool positive_finite(f32 x) {
    return de__f32_finite(x) && x > 0.0f;
}
static bool nonneg_finite(f32 x) {
    return de__f32_finite(x) && x >= 0.0f;
}

static f32 radius_up(f64 sq) {
    return de__f32_round_up(de_dsqrt64(sq));
}

/* Local box and bounding radius of a list of points swept by r. */
static void bounds_of_points(de__shape2d *s, const de_vec2 *pts, u32 n, f64 r) {
    box_acc b;
    f64 r2 = 0.0;
    box_init(&b, de__d2_from(pts[0]));
    for (u32 i = 0; i < n; ++i) {
        const de__d2 q = de__d2_from(pts[i]);
        box_add(&b, q);
        r2 = de__maxd(r2, de__d2_dot(q, q));
    }
    s->local_aabb = box_finish(&b, r);
    s->bounding_radius = de__f32_round_up((f64)radius_up(r2) + r);
}

static de_result build_polygon_shape(de__shape2d *s) {
    const de__polygon2d *poly = &s->u.polygon;
    s->has_area = true;
    s->radius = poly->radius;
    bounds_of_points(s, poly->vertices, poly->count, poly->radius);
    de__mass2d_polygon(poly, &s->unit_mass);
    return DE_OK;
}

static de_result build_box(const de_shape2d_desc *d, de__shape2d *s) {
    const de_vec2 e = d->half_extents;
    if (!positive_finite(e.x) || !positive_finite(e.y) || !de__v2_finite(d->center) ||
        !de__f32_finite(d->angle) || !nonneg_finite(d->radius))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape2d_create: box needs half_extents > 0, a finite center and angle "
                       "and radius >= 0");
    const de_rot2d r = d->angle == 0.0f ? de__rot2d_identity() : de_rot2d_from_angle(d->angle);
    const de__d2 c = de__d2_from(d->center);
    const f64 hx = e.x, hy = e.y;
    const de__d2 corners[4] = {de__d2_make(-hx, -hy), de__d2_make(hx, -hy), de__d2_make(hx, hy),
                               de__d2_make(-hx, hy)};
    de__polygon2d *poly = &s->u.polygon;
    memset(poly, 0, sizeof *poly);
    poly->count = 4;
    poly->radius = d->radius;
    for (u32 i = 0; i < 4; ++i) {
        const de__d2 v = de__d2_add(c, de__rot2d_apply_d(r, corners[i]));
        poly->vertices[i] = de__v2((f32)v.x, (f32)v.y);
    }
    if (!de__poly2d_finish(poly))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape2d_create: box too small for its position (degenerate in f32)");
    return build_polygon_shape(s);
}

static de_result build_polygon(const de_shape2d_desc *d, de__shape2d *s) {
    if (d->points == NULL || d->point_count < 3 || d->point_count > DE_SHAPE2D_MAX_POLYGON_INPUT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape2d_create: polygon needs 3..%u points (got %u)",
                       DE_SHAPE2D_MAX_POLYGON_INPUT, d->point_count);
    if (!nonneg_finite(d->hull_tolerance) || !nonneg_finite(d->radius))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape2d_create: bad polygon hull_tolerance or radius");
    for (u32 i = 0; i < d->point_count; ++i)
        if (!de__v2_finite(d->points[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape2d_create: point %u is not finite", i);
    const f32 tol = d->hull_tolerance > 0.0f ? d->hull_tolerance
                                             : de__poly2d_auto_tolerance(d->points, d->point_count);
    DE_TRY(de__poly2d_build(d->points, d->point_count, tol, d->radius, &s->u.polygon));
    return build_polygon_shape(s);
}

static de_result build_primitive(const de_shape2d_desc *d, de__shape2d *s) {
    switch (d->kind) {
    case DE_SHAPE2D_CIRCLE:
        if (!positive_finite(d->radius) || !de__v2_finite(d->center))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape2d_create: circle needs radius > 0 and a finite center");
        s->u.circle.center = d->center;
        s->u.circle.radius = d->radius;
        s->radius = d->radius;
        s->has_area = true;
        bounds_of_points(s, &d->center, 1, d->radius);
        de__mass2d_circle(d->radius, de__d2_from(d->center), &s->unit_mass);
        return DE_OK;
    case DE_SHAPE2D_CAPSULE: {
        if (!positive_finite(d->radius) || !de__v2_finite(d->p1) || !de__v2_finite(d->p2))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape2d_create: capsule needs radius > 0 and finite centers");
        s->u.capsule.p1 = d->p1;
        s->u.capsule.p2 = d->p2;
        s->u.capsule.radius = d->radius;
        s->radius = d->radius;
        s->has_area = true;
        const de_vec2 pts[2] = {d->p1, d->p2};
        bounds_of_points(s, pts, 2, d->radius);
        de__mass2d_capsule(de__d2_from(d->p1), de__d2_from(d->p2), d->radius, &s->unit_mass);
        return DE_OK;
    }
    case DE_SHAPE2D_SEGMENT: {
        if (!de__v2_finite(d->p1) || !de__v2_finite(d->p2) ||
            (d->p1.x == d->p2.x && d->p1.y == d->p2.y))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape2d_create: segment needs two different finite points");
        s->u.segment.p1 = d->p1;
        s->u.segment.p2 = d->p2;
        const de__d2 e = de__d2_sub(de__d2_from(d->p2), de__d2_from(d->p1));
        s->u.segment.normal = de__v2_normalize_d(de__d2_make(e.y, -e.x));
        const de_vec2 pts[2] = {d->p1, d->p2};
        bounds_of_points(s, pts, 2, 0.0);
        return DE_OK;
    }
    default:
        return DE_FAIL(DE_ERR_INTERNAL, "build_primitive: not a primitive");
    }
}

/* ---- Public API ------------------------------------------------------------------------ */

static de__shape2d *shape_new(de_physics2d_world *p, de_shape2d_kind kind) {
    de__shape2d *s = DE_NEW(p->alloc, de__shape2d, DE_MEM_PHYSICS);
    if (s == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape2d_create: shape");
        return NULL;
    }
    memset(s, 0, sizeof *s);
    s->kind = kind;
    return s;
}

/* Inserts a built shape (r == DE_OK) or frees it; returns its handle or the invalid one. */
static de_shape shape_finish(de_physics2d_world *p, de__shape2d *s, de_result r) {
    de_shape none = {0};
    if (r == DE_OK)
        r = de__shape2d_insert(p, s);
    if (r != DE_OK) {
        de__shape2d_free(p, s);
        return none;
    }
    return s->handle;
}

de_shape de_shape2d_create(de_physics2d_world *p, const de_shape2d_desc *desc) {
    de_shape none = {0};
    if (p == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape2d_create: NULL world or desc");
        return none;
    }
    if (desc->kind == DE_SHAPE2D_CHAIN) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape2d_create: use de_shape2d_chain_create");
        return none;
    }
    if ((u32)desc->kind > (u32)DE_SHAPE2D_SEGMENT) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape2d_create: unknown kind %u", (u32)desc->kind);
        return none;
    }
    de__shape2d *s = shape_new(p, desc->kind);
    if (s == NULL)
        return none;
    de_result r;
    switch (desc->kind) {
    case DE_SHAPE2D_BOX:
        r = build_box(desc, s);
        break;
    case DE_SHAPE2D_POLYGON:
        r = build_polygon(desc, s);
        break;
    default:
        r = build_primitive(desc, s);
        break;
    }
    return shape_finish(p, s, r);
}

de_shape de_shape2d_chain_create(de_physics2d_world *p, const de_vec2 *points, u32 count,
                                 bool loop) {
    de_shape none = {0};
    if (p == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape2d_chain_create: NULL world");
        return none;
    }
    de__shape2d *s = shape_new(p, DE_SHAPE2D_CHAIN);
    if (s == NULL)
        return none;
    return shape_finish(p, s, de__chain2d_build(p, points, count, loop, s));
}

de_result de_shape2d_destroy(de_physics2d_world *p, de_shape h) {
    de__shape2d *s = de__shape2d_get(p, h);
    if (s == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape2d_destroy: stale or invalid shape 0x%08x",
                       h.id);
    if (s->refcount > 0) {
        DE_WARN(de__physics_log(), "de_shape2d_destroy: shape 0x%08x is used by %u bodies", h.id,
                s->refcount);
        return DE_FAIL(DE_ERR_BUSY, "de_shape2d_destroy: shape 0x%08x is still referenced (%u)",
                       h.id, s->refcount);
    }
    const u32 i = de__handle_index(h.id);
    de__shape2d_free(p, s);
    de__shape2d_slot *slot = &p->shape_slots[i];
    slot->shape = NULL;
    slot->generation = de__gen_next(slot->generation);
    p->shape_free[p->shape_free_count++] = i;
    --p->shape_live_count;
    return DE_OK;
}

bool de_shape2d_valid(const de_physics2d_world *p, de_shape s) {
    return de__shape2d_get(p, s) != NULL;
}

bool de_shape2d_get_kind(const de_physics2d_world *p, de_shape h, de_shape2d_kind *out) {
    const de__shape2d *s = de__shape2d_get(p, h);
    if (s == NULL || out == NULL)
        return false;
    *out = s->kind;
    return true;
}

bool de_shape2d_get_local_aabb(const de_physics2d_world *p, de_shape h, de_aabb2d *out) {
    const de__shape2d *s = de__shape2d_get(p, h);
    if (s == NULL || out == NULL)
        return false;
    *out = s->local_aabb;
    return true;
}

de_result de_shape2d_get_mass_props(const de_physics2d_world *p, de_shape h, f32 density,
                                    de_mass_props2d *out) {
    const de__shape2d *s = de__shape2d_get(p, h);
    if (s == NULL || out == NULL || !de__f32_finite(density) || density < 0.0f)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape2d_get_mass_props: bad arguments");
    if (density == 0.0f)
        density = DE_PHYSICS2D_DEFAULT_DENSITY;
    de__unit_mass2d_scale(&s->unit_mass, s->has_area ? (f64)density : 0.0, out);
    if (!s->has_area)
        out->area = 0.0f;
    return DE_OK;
}
