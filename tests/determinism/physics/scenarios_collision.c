/* scenarios_collision.c: the S22 (collision-only) physics determinism scenarios (DENG-136).
 *
 *   kinematic_sweep  1000 ticks. Kinematic bodies of every convex kind (and a compound) fly on
 *                    scripted de_dmath trajectories through a ring of static shapes of every kind
 *                    (sphere, capsule, box, cylinder, hull, mesh, heightfield with a hole,
 *                    compound); one more pair sits 36 km from the origin. Positions are
 *                    integrated from the bodies' own state, so a difference carries over. Bodies
 *                    are added and removed on the way (index reuse). The probe runs the
 *                    narrowphase on every overlapping pair that is not static-static and hashes
 *                    the contact sets, so contacts are cross-checked per tick before DENG-135
 *                    folds them into de_physics_checksum.
 *   hull_builder     50 ticks: one convex hull per tick from a de_rng point cloud (box, sphere
 *                    shell, thin slab, duplicates, far from the origin; every tenth cloud is
 *                    planar and must be rejected), on a dynamic body. The probe hashes every
 *                    hull's vertices, planes, faces, half-edges and unit mass.
 *   mass_props       64 ticks: one shape and body per tick, cycling through every shape kind with
 *                    random sizes, densities, rotations and mass overrides; every seventh tick
 *                    also destroys the oldest body. The probe hashes the shape and body mass
 *                    properties and bounds.
 *
 * SPEC §5.7: de_dmath / de_dvec3 / de_rng only, one random draw per statement, bodies visited in
 * ascending index. Built with DE_DETERMINISTIC_FLAGS. Goldens: golden/<name>.checksums. */
#include "harness.h"

#include "physics/narrow/narrowphase.h"
#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <string.h>

/* ================================ Helpers ============================================== */

#define SC_PI 3.14159265358979f

static f32 rf(de_rng *r, f32 lo, f32 hi) {
    const f32 u = de_rng_f32(r);
    return lo + (hi - lo) * u;
}

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_dvec3 dv3(f64 x, f64 y, f64 z) {
    de_dvec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_quat quat_axis_angle(de_vec3 axis, f32 angle) {
    const de_vec3 a = de_dnormalize_v3(axis);
    f32 s, c;
    de_dsincos(angle * 0.5f, &s, &c);
    de_quat q;
    q.x = a.x * s;
    q.y = a.y * s;
    q.z = a.z * s;
    q.w = c;
    return q;
}

static de_quat rng_quat(de_rng *r) {
    const f32 x = rf(r, -1.0f, 1.0f);
    const f32 y = rf(r, -1.0f, 1.0f);
    const f32 z = rf(r, -1.0f, 1.0f) + 0.01f; /* never the zero axis */
    const f32 angle = rf(r, -SC_PI, SC_PI);
    return quat_axis_angle(v3(x, y, z), angle);
}

static de_transform xform(de_dvec3 p, de_quat q) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = p;
    t.rotation = q;
    t.scale = v3(1.0f, 1.0f, 1.0f);
    return t;
}

static de_quat quat_identity(void) {
    de_quat q;
    q.x = 0.0f;
    q.y = 0.0f;
    q.z = 0.0f;
    q.w = 1.0f;
    return q;
}

/* Little-endian hashing of probe data (never raw struct bytes). */
typedef struct sc_hash {
    u8 b[256];
    u32 n;
    u64 h;
} sc_hash;

static void sh_flush(sc_hash *s) {
    if (s->n > 0)
        s->h = de_hash64(s->b, s->n, s->h);
    s->n = 0;
}
static void sh_u32(sc_hash *s, u32 v) {
    if (s->n + 4 > sizeof s->b)
        sh_flush(s);
    for (u32 i = 0; i < 4; ++i)
        s->b[s->n++] = (u8)(v >> (8u * i));
}
static void sh_u64(sc_hash *s, u64 v) {
    sh_u32(s, (u32)v);
    sh_u32(s, (u32)(v >> 32));
}
static void sh_f32(sc_hash *s, f32 v) {
    u32 u;
    memcpy(&u, &v, sizeof u);
    sh_u32(s, u);
}
static void sh_f64(sc_hash *s, f64 v) {
    u64 u;
    memcpy(&u, &v, sizeof u);
    sh_u64(s, u);
}
static void sh_v3(sc_hash *s, de_vec3 v) {
    sh_f32(s, v.x);
    sh_f32(s, v.y);
    sh_f32(s, v.z);
}
static void sh_aabb(sc_hash *s, de_aabb b) {
    sh_v3(s, b.min);
    sh_v3(s, b.max);
}
static void sh_mass(sc_hash *s, const de_mass_props *m) {
    sh_f32(s, m->mass);
    sh_v3(s, m->center);
    for (u32 i = 0; i < 9; ++i)
        sh_f32(s, m->inertia.m[i]);
}
static u64 sh_end(sc_hash *s) {
    sh_flush(s);
    return s->h;
}

/* A point cloud for a hull. `mode`: 0 box, 1 sphere shell, 2 thin slab, 3 duplicates, 4 far from
 * the origin, 5 planar (degenerate: must be rejected). Returns the point count. */
#define SC_MAX_CLOUD 128u
static u32 make_cloud(de_rng *r, u32 mode, u32 count, de_vec3 *pts) {
    if (count > SC_MAX_CLOUD)
        count = SC_MAX_CLOUD;
    const f32 sx = rf(r, 0.3f, 1.5f);
    const f32 sy = rf(r, 0.3f, 1.5f);
    const f32 sz = rf(r, 0.3f, 1.5f);
    for (u32 i = 0; i < count; ++i) {
        const f32 x = rf(r, -1.0f, 1.0f);
        const f32 y = rf(r, -1.0f, 1.0f);
        const f32 z = rf(r, -1.0f, 1.0f);
        de_vec3 p = v3(x * sx, y * sy, z * sz);
        switch (mode) {
        case 1: {
            const de_vec3 n = de_dnormalize_v3(v3(x, y, z + 1e-3f));
            p = v3(n.x * sx, n.y * sx, n.z * sx);
            break;
        }
        case 2:
            p.z = z * 0.02f;
            break;
        case 3:
            if (i >= 2u && (i % 3u) == 0u)
                p = pts[i / 2u]; /* exact duplicate of an earlier point */
            break;
        case 4:
            p = v3(p.x + 500.0f, p.y - 300.0f, p.z + 200.0f);
            break;
        case 5:
            p.z = 0.25f; /* every point on one plane */
            break;
        default:
            break;
        }
        pts[i] = p;
    }
    return count;
}

static void hash_hull_shape(sc_hash *s, const de_physics_world *p, de_shape shape) {
    const de__shape *sh = de__shape_get(p, shape);
    if (sh == NULL) {
        sh_u32(s, 0xDEADu);
        return;
    }
    sh_u32(s, (u32)sh->kind);
    sh_aabb(s, sh->local_aabb);
    sh_f32(s, sh->margin);
    sh_f32(s, sh->bounding_radius);
    sh_u32(s, sh->has_volume ? 1u : 0u);
    sh_f64(s, sh->unit_mass.volume);
    sh_f64(s, sh->unit_mass.com.x);
    sh_f64(s, sh->unit_mass.com.y);
    sh_f64(s, sh->unit_mass.com.z);
    for (u32 i = 0; i < 9; ++i)
        sh_f64(s, sh->unit_mass.inertia[i]);
    if (sh->kind != DE_SHAPE_CONVEX)
        return;
    const de__hull *h = &sh->u.hull;
    sh_u32(s, h->vertex_count);
    sh_u32(s, h->face_count);
    sh_u32(s, h->edge_count);
    for (u32 i = 0; i < h->vertex_count; ++i)
        sh_v3(s, h->vertices[i]);
    for (u32 i = 0; i < h->face_count; ++i) {
        sh_v3(s, h->planes[i].n);
        sh_f32(s, h->planes[i].d);
        sh_u32(s, (u32)h->faces[i].first_edge | ((u32)h->faces[i].edge_count << 16));
    }
    for (u32 i = 0; i < h->edge_count; ++i) {
        const de__hull_edge *e = &h->edges[i];
        sh_u32(s, (u32)e->next | ((u32)e->twin << 16));
        sh_u32(s, (u32)e->origin | ((u32)e->face << 16));
    }
    sh_v3(s, h->centroid);
    sh_f32(s, h->tolerance);
}

/* ================================ kinematic_sweep ====================================== */

#define KS_TICKS     1000u
#define KS_RING      10.0f
#define KS_STATICS   8u
#define KS_MOVERS    9u /* 8 on the ring + 1 far from the origin */
#define KS_MAX_SETS  64u
#define KS_SPEC      0.02f
#define KS_FAR_X     30000.0
#define KS_FAR_Y     (-20000.0)
#define KS_FAR_Z     50.0
#define KS_FAR_ORBIT 1.6f

typedef struct ks_state {
    de_body statics[KS_STATICS + 1]; /* + the box far from the origin */
    de_body movers[KS_MOVERS];
    de_dvec3 center[KS_MOVERS];
    f32 radius[KS_MOVERS], omega[KS_MOVERS], phase[KS_MOVERS], bob[KS_MOVERS];
    de_body transient;
    de_shape transient_sphere, transient_box;
    u32 ticks_with_contacts;
    u64 contact_sets;
} ks_state;

static de_body make_body(de_physics_world *p, de_body_type type, de_shape shape, de_transform t,
                         f32 density) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.transform = t;
    d.shape = shape;
    d.material.friction = 0.5f;
    d.material.restitution = 0.1f;
    d.material.density = density;
    return de_body_create(p, &d);
}

static de_shape make_basic(de_physics_world *p, de_shape_kind kind, f32 a, f32 b, f32 c) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    d.radius = a;
    d.half_height = b;
    d.half_extents = v3(a, b, c);
    return de_shape_create(p, &d);
}

static de_shape make_hull(de_physics_world *p, de_rng *r, u32 mode, u32 count) {
    de_vec3 pts[SC_MAX_CLOUD];
    const u32 n = make_cloud(r, mode, count, pts);
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CONVEX;
    d.points = pts;
    d.point_count = n;
    return de_shape_create(p, &d);
}

/* A 4 x 4 quad grid (32 triangles), 4 m wide, heights from the stream. */
static de_shape make_mesh(de_physics_world *p, de_rng *r, f32 bump) {
    de_vec3 verts[25];
    u32 idx[96];
    for (u32 y = 0; y < 5u; ++y) {
        for (u32 x = 0; x < 5u; ++x) {
            const f32 h = rf(r, -bump, bump);
            verts[y * 5u + x] = v3((f32)x - 2.0f, (f32)y - 2.0f, h);
        }
    }
    u32 n = 0;
    for (u32 y = 0; y < 4u; ++y) {
        for (u32 x = 0; x < 4u; ++x) {
            const u32 a = y * 5u + x, b = a + 1u, c = a + 6u, d = a + 5u;
            idx[n++] = a;
            idx[n++] = b;
            idx[n++] = c;
            idx[n++] = a;
            idx[n++] = c;
            idx[n++] = d;
        }
    }
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = verts;
    d.vertex_count = 25;
    d.indices = idx;
    d.index_count = n;
    return de_shape_create(p, &d);
}

/* 9 x 9 samples, 0.5 m apart, one hole sample. */
static de_shape make_heightfield(de_physics_world *p, de_rng *r) {
    f32 h[81];
    for (u32 i = 0; i < 81u; ++i)
        h[i] = rf(r, -0.3f, 0.3f);
    h[3 * 9 + 5] = DE_HEIGHTFIELD_HOLE;
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_HEIGHTFIELD;
    d.heights = h;
    d.rows = 9;
    d.cols = 9;
    d.scale = v3(0.5f, 0.5f, 1.0f);
    return de_shape_create(p, &d);
}

/* A compound of a box, a sphere and a capsule (dynamic-capable). */
static de_shape make_compound(de_physics_world *p, de_rng *r, f32 scale) {
    de_shape kids[3];
    kids[0] = make_basic(p, DE_SHAPE_BOX, 0.4f * scale, 0.3f * scale, 0.2f * scale);
    kids[1] = make_basic(p, DE_SHAPE_SPHERE, 0.35f * scale, 0.0f, 0.0f);
    kids[2] = make_basic(p, DE_SHAPE_CAPSULE, 0.15f * scale, 0.4f * scale, 0.0f);
    de_transform xf[3];
    for (u32 i = 0; i < 3u; ++i) {
        const f32 x = rf(r, -0.6f, 0.6f);
        const f32 y = rf(r, -0.6f, 0.6f);
        const f32 z = rf(r, -0.3f, 0.3f);
        const de_quat q = rng_quat(r);
        xf[i] = xform(dv3((f64)(x * scale), (f64)(y * scale), (f64)(z * scale)), q);
    }
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_COMPOUND;
    d.children = kids;
    d.child_xforms = xf;
    d.child_count = 3;
    return de_shape_create(p, &d);
}

static de_result ks_setup(de_phys_det_ctx *c) {
    ks_state *st = (ks_state *)c->state;
    de_physics_world *p = c->physics;
    de_rng r = de_rng_seed(c->seed, 1);

    /* The static ring: one shape of every kind, 45 degrees apart. */
    de_shape ring[KS_STATICS];
    ring[0] = make_basic(p, DE_SHAPE_SPHERE, 1.0f, 0.0f, 0.0f);
    ring[1] = make_basic(p, DE_SHAPE_CAPSULE, 0.5f, 1.0f, 0.0f);
    ring[2] = make_basic(p, DE_SHAPE_BOX, 1.0f, 0.8f, 0.6f);
    ring[3] = make_basic(p, DE_SHAPE_CYLINDER, 0.7f, 1.0f, 0.0f);
    ring[4] = make_hull(p, &r, 0, 24);
    ring[5] = make_mesh(p, &r, 0.4f);
    ring[6] = make_heightfield(p, &r);
    ring[7] = make_compound(p, &r, 1.5f);
    for (u32 i = 0; i < KS_STATICS; ++i) {
        if (!DE_HANDLE_VALID(ring[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "kinematic_sweep: static shape %u", i);
        f32 s, co;
        de_dsincos((f32)i * (SC_PI / 4.0f), &s, &co);
        f64 x = (f64)(KS_RING * co), y = (f64)(KS_RING * s);
        if (i == 6u) { /* the heightfield's origin is a corner: center it on the ring */
            x -= 2.0;
            y -= 2.0;
        }
        /* Meshes and heightfields lie flat (they are one-sided, front +Z); the rest tumble. */
        const de_quat q = (i == 5u || i == 6u) ? quat_identity() : rng_quat(&r);
        st->statics[i] = make_body(p, DE_BODY_STATIC, ring[i], xform(dv3(x, y, 0.0), q), 0.0f);
    }
    const de_shape far_box = make_basic(p, DE_SHAPE_BOX, 1.0f, 1.0f, 1.0f);
    st->statics[KS_STATICS] = make_body(
        p, DE_BODY_STATIC, far_box,
        xform(dv3(KS_FAR_X, KS_FAR_Y, KS_FAR_Z), quat_axis_angle(v3(0.0f, 0.0f, 1.0f), 0.3f)),
        0.0f);

    /* Movers: every convex kind and a compound, on the ring at different speeds and phases. */
    de_shape mover[KS_MOVERS];
    mover[0] = make_basic(p, DE_SHAPE_SPHERE, 0.5f, 0.0f, 0.0f);
    mover[1] = make_basic(p, DE_SHAPE_CAPSULE, 0.3f, 0.6f, 0.0f);
    mover[2] = make_basic(p, DE_SHAPE_BOX, 0.5f, 0.4f, 0.3f);
    mover[3] = make_basic(p, DE_SHAPE_CYLINDER, 0.4f, 0.5f, 0.0f);
    mover[4] = make_hull(p, &r, 1, 32);
    mover[5] = make_compound(p, &r, 1.0f);
    mover[6] = make_hull(p, &r, 3, 20);
    mover[7] = make_basic(p, DE_SHAPE_BOX, 0.2f, 0.9f, 0.2f);
    mover[8] = make_basic(p, DE_SHAPE_CAPSULE, 0.4f, 0.5f, 0.0f);
    for (u32 k = 0; k < KS_MOVERS; ++k) {
        if (!DE_HANDLE_VALID(mover[k]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "kinematic_sweep: mover shape %u", k);
        const bool far = k == KS_MOVERS - 1u;
        st->center[k] = far ? dv3(KS_FAR_X, KS_FAR_Y, KS_FAR_Z) : dv3(0.0, 0.0, 0.0);
        st->radius[k] = far ? KS_FAR_ORBIT : KS_RING;
        st->omega[k] = (far ? 0.9f : 0.35f) + 0.05f * (f32)k;
        st->phase[k] = 0.785f * (f32)k + 0.3f;
        st->bob[k] = rf(&r, 0.3f, 0.8f);
        f32 s, co;
        de_dsincos(st->phase[k], &s, &co);
        const de_dvec3 pos = de_dv3_add(
            st->center[k], dv3((f64)(st->radius[k] * co), (f64)(st->radius[k] * s), 0.0));
        st->movers[k] = make_body(p, DE_BODY_KINEMATIC, mover[k], xform(pos, rng_quat(&r)), 0.0f);
        if (!DE_HANDLE_VALID(st->movers[k]))
            return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "kinematic_sweep: mover %u", k);
    }
    st->transient_sphere = make_basic(p, DE_SHAPE_SPHERE, 0.8f, 0.0f, 0.0f);
    st->transient_box = make_basic(p, DE_SHAPE_BOX, 0.6f, 0.6f, 0.6f);
    return DE_OK;
}

static void ks_script(de_phys_det_ctx *c, u64 tick) {
    ks_state *st = (ks_state *)c->state;
    de_physics_world *p = c->physics;
    const f32 dt = c->dt;
    const f32 time = (f32)tick * dt;

    /* Body churn on the ring path: a static sphere appears at tick 300 and goes at tick 600; a
     * static box takes its body index (LIFO reuse) at tick 650. */
    f32 s22, c22;
    de_dsincos(SC_PI / 8.0f, &s22, &c22);
    const de_transform on_ring =
        xform(dv3((f64)(KS_RING * c22), (f64)(KS_RING * s22), 0.0), quat_identity());
    if (tick == 300u)
        st->transient = make_body(p, DE_BODY_STATIC, st->transient_sphere, on_ring, 0.0f);
    if (tick == 600u)
        de_body_destroy(p, st->transient);
    if (tick == 650u)
        st->transient = make_body(p, DE_BODY_STATIC, st->transient_box, on_ring, 0.0f);

    for (u32 k = 0; k < KS_MOVERS; ++k) {
        const de_body b = st->movers[k];
        de_transform t;
        if (!de_body_get_transform(p, b, &t))
            continue;
        /* Velocity of the scripted path: a circle with a vertical bob. */
        const f32 w = st->omega[k];
        const f32 theta = st->phase[k] + w * time;
        f32 s, co, s2, c2;
        de_dsincos(theta, &s, &co);
        de_dsincos(2.0f * theta, &s2, &c2);
        const f32 rw = st->radius[k] * w;
        const de_vec3 v = v3(-rw * s, rw * co, 2.0f * st->bob[k] * w * c2);
        const de_vec3 av = v3(0.8f * de_dsin(0.7f * theta + (f32)k), 0.3f + 0.1f * (f32)k,
                              0.9f * de_dcos(0.5f * theta));
        /* Since DENG-217 the step moves kinematic bodies by their velocity, from the body's own
         * state, so a difference carries into later ticks. */
        de_body_set_velocity(p, b, v, av);
    }
}

static bool aabb_overlap(const de_aabb *a, const de_aabb *b) {
    return a->min.x <= b->max.x && b->min.x <= a->max.x && a->min.y <= b->max.y &&
           b->min.y <= a->max.y && a->min.z <= b->max.z && b->min.z <= a->max.z;
}

/* Narrowphase on every pair (ascending index) whose world AABBs overlap and that is not
 * static-static: the contacts DENG-135 will keep, hashed field by field. */
static u64 ks_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    ks_state *st = (ks_state *)c->state;
    const de_physics_world *p = c->physics;
    const de__body_pool *b = &p->bodies;
    de__contact_set sets[KS_MAX_SETS];
    sc_hash h;
    memset(&h, 0, sizeof h);
    u32 tick_sets = 0;
    for (u32 i = 0; i < b->high_water; ++i) {
        if (!b->alive[i] || !DE_HANDLE_VALID(b->shape[i]))
            continue;
        for (u32 j = i + 1u; j < b->high_water; ++j) {
            if (!b->alive[j] || !DE_HANDLE_VALID(b->shape[j]))
                continue;
            if (b->type[i] == DE_BODY_STATIC && b->type[j] == DE_BODY_STATIC)
                continue;
            if (!aabb_overlap(&b->world_aabb[i], &b->world_aabb[j]))
                continue;
            de__contact_buffer buf;
            buf.sets = sets;
            buf.capacity = KS_MAX_SETS;
            buf.count = 0;
            buf.overflow = false;
            de__pose pa, pb;
            pa.position = b->position[i];
            pa.rotation = b->rotation[i];
            pb.position = b->position[j];
            pb.rotation = b->rotation[j];
            const u32 n = de__collide(de__shape_get(p, b->shape[i]), pa,
                                      de__shape_get(p, b->shape[j]), pb, KS_SPEC, &buf);
            sh_u32(&h, i);
            sh_u32(&h, j);
            sh_u32(&h, n);
            sh_u32(&h, buf.overflow ? 1u : 0u);
            for (u32 k = 0; k < buf.count; ++k) {
                const de__contact_set *cs = &sets[k];
                sh_v3(&h, cs->normal);
                sh_u32(&h, cs->point_count);
                sh_u32(&h, cs->child_a);
                sh_u32(&h, cs->child_b);
                sh_u32(&h, cs->sub_a);
                sh_u32(&h, cs->sub_b);
                for (u32 q = 0; q < cs->point_count; ++q) {
                    sh_v3(&h, cs->points[q].local_a);
                    sh_v3(&h, cs->points[q].local_b);
                    sh_f32(&h, cs->points[q].separation);
                    sh_u32(&h, cs->points[q].feature_id);
                }
            }
            tick_sets += buf.count;
        }
    }
    st->contact_sets += tick_sets;
    st->ticks_with_contacts += tick_sets > 0 ? 1u : 0u;
    sh_u32(&h, tick_sets);
    return sh_end(&h);
}

const de_phys_det_scenario de_phys_det_scenario_kinematic_sweep = {
    .name = "kinematic_sweep",
    .setup = ks_setup,
    .script = ks_script,
    .probe = ks_probe,
    .ticks = KS_TICKS,
    .state_size = (u32)sizeof(ks_state),
    .max_bodies = 64,
};
DE_PHYS_DET_REGISTER(kinematic_sweep)

/* Statistics for the harness self tests (test_harness.c). */
u32 de_phys_det_kinematic_sweep_contact_ticks(const de_phys_det_ctx *c);
u32 de_phys_det_kinematic_sweep_contact_ticks(const de_phys_det_ctx *c) {
    return ((const ks_state *)c->state)->ticks_with_contacts;
}
de_body de_phys_det_kinematic_sweep_mover(const de_phys_det_ctx *c, u32 k);
de_body de_phys_det_kinematic_sweep_mover(const de_phys_det_ctx *c, u32 k) {
    return ((const ks_state *)c->state)->movers[k];
}

/* ================================ hull_builder ========================================= */

#define HB_TICKS 50u

typedef struct hb_state {
    u64 acc; /* running hash of every hull (and rejection) so far */
    u32 built, rejected;
} hb_state;

static de_result hb_setup(de_phys_det_ctx *c) {
    (void)c;
    return DE_OK;
}

static void hb_script(de_phys_det_ctx *c, u64 tick) {
    hb_state *st = (hb_state *)c->state;
    de_physics_world *p = c->physics;
    de_rng r = de_rng_seed(c->seed, 1000u + tick);
    const u32 count = de_rng_range(&r, 8u, 97u);
    const u32 mode = (tick % 10u) == 9u ? 5u : (u32)(tick % 5u);
    de_vec3 pts[SC_MAX_CLOUD];
    const u32 n = make_cloud(&r, mode, count, pts);
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CONVEX;
    d.points = pts;
    d.point_count = n;
    const de_shape shape = de_shape_create(p, &d);

    sc_hash h;
    memset(&h, 0, sizeof h);
    h.h = st->acc;
    sh_u64(&h, tick);
    sh_u32(&h, mode);
    sh_u32(&h, n);
    if (!DE_HANDLE_VALID(shape)) {
        const de_error *e = de_error_last();
        sh_u32(&h, e != NULL ? (u32)e->result : 0xFFFFFFFFu);
        ++st->rejected;
        st->acc = sh_end(&h);
        return;
    }
    hash_hull_shape(&h, p, shape);
    de_mass_props mp;
    if (de_shape_get_mass_props(p, shape, 0.0f, &mp) == DE_OK)
        sh_mass(&h, &mp);
    st->acc = sh_end(&h);
    ++st->built;

    const f32 x = rf(&r, -20.0f, 20.0f);
    const f32 y = rf(&r, -20.0f, 20.0f);
    const f32 z = rf(&r, 1.0f, 10.0f);
    const de_quat q = rng_quat(&r);
    const f32 density = rf(&r, 200.0f, 3000.0f);
    make_body(p, DE_BODY_DYNAMIC, shape, xform(dv3((f64)x, (f64)y, (f64)z), q), density);
}

static u64 hb_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    return ((const hb_state *)c->state)->acc;
}

const de_phys_det_scenario de_phys_det_scenario_hull_builder = {
    .name = "hull_builder",
    .setup = hb_setup,
    .script = hb_script,
    .probe = hb_probe,
    .ticks = HB_TICKS,
    .state_size = (u32)sizeof(hb_state),
    .max_bodies = 64,
};
DE_PHYS_DET_REGISTER(hull_builder)

u32 de_phys_det_hull_builder_counts(const de_phys_det_ctx *c, u32 *rejected);
u32 de_phys_det_hull_builder_counts(const de_phys_det_ctx *c, u32 *rejected) {
    const hb_state *st = (const hb_state *)c->state;
    *rejected = st->rejected;
    return st->built;
}

/* ================================ mass_props =========================================== */

#define MP_TICKS 64u

typedef struct mp_state {
    u64 acc;
    de_body bodies[MP_TICKS]; /* in creation order */
    u32 created, oldest;
} mp_state;

static de_result mp_setup(de_phys_det_ctx *c) {
    (void)c;
    return DE_OK;
}

static void mp_script(de_phys_det_ctx *c, u64 tick) {
    mp_state *st = (mp_state *)c->state;
    de_physics_world *p = c->physics;
    de_rng r = de_rng_seed(c->seed, 2000u + tick);
    const de_shape_kind kind = (de_shape_kind)(tick % 8u);
    de_shape shape;
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    switch (kind) {
    case DE_SHAPE_SPHERE:
    case DE_SHAPE_CAPSULE:
    case DE_SHAPE_CYLINDER: {
        const f32 radius = rf(&r, 0.05f, 2.0f);
        const f32 half = rf(&r, 0.05f, 3.0f);
        shape = make_basic(p, kind, radius, half, 0.0f);
        break;
    }
    case DE_SHAPE_BOX: {
        const f32 x = rf(&r, 0.05f, 3.0f);
        const f32 y = rf(&r, 0.05f, 3.0f);
        const f32 z = rf(&r, 0.05f, 3.0f);
        shape = make_basic(p, kind, x, y, z);
        break;
    }
    case DE_SHAPE_CONVEX: {
        const u32 count = de_rng_range(&r, 6u, 64u);
        const u32 mode = de_rng_range(&r, 0u, 5u);
        shape = make_hull(p, &r, mode, count);
        break;
    }
    case DE_SHAPE_MESH:
        shape = make_mesh(p, &r, 1.0f);
        break;
    case DE_SHAPE_HEIGHTFIELD:
        shape = make_heightfield(p, &r);
        break;
    case DE_SHAPE_COMPOUND:
    default: {
        const f32 scale = rf(&r, 0.5f, 3.0f);
        shape = make_compound(p, &r, scale);
        break;
    }
    }

    sc_hash h;
    memset(&h, 0, sizeof h);
    h.h = st->acc;
    sh_u64(&h, tick);
    sh_u32(&h, (u32)kind);
    sh_u32(&h, DE_HANDLE_VALID(shape) ? 1u : 0u);
    const f32 density = rf(&r, 100.0f, 5000.0f);
    const bool has_volume = kind != DE_SHAPE_MESH && kind != DE_SHAPE_HEIGHTFIELD;
    if (DE_HANDLE_VALID(shape)) {
        de_mass_props mp;
        de_aabb box;
        memset(&mp, 0, sizeof mp);
        sh_u32(&h, (u32)de_shape_get_mass_props(p, shape, density, &mp));
        sh_mass(&h, &mp);
        if (de_shape_get_local_aabb(p, shape, &box))
            sh_aabb(&h, box);
        hash_hull_shape(&h, p, shape); /* unit mass, bounds; the hull data for hulls */

        de_body_desc bd;
        memset(&bd, 0, sizeof bd);
        bd.type = has_volume ? DE_BODY_DYNAMIC : DE_BODY_STATIC;
        const f32 x = rf(&r, -50.0f, 50.0f);
        const f32 y = rf(&r, -50.0f, 50.0f);
        const f32 z = rf(&r, 0.0f, 20.0f);
        bd.transform = xform(dv3((f64)x, (f64)y, (f64)z), rng_quat(&r));
        bd.shape = shape;
        bd.material.density = density;
        if (has_volume && (tick % 3u) == 0u)
            bd.mass = rf(&r, 1.0f, 50.0f);
        const de_body body = de_body_create(p, &bd);
        if (DE_HANDLE_VALID(body)) {
            st->bodies[st->created++] = body;
            de_mass_props bm;
            if (de_body_get_mass_props(p, body, &bm))
                sh_mass(&h, &bm);
        }
    }
    st->acc = sh_end(&h);

    /* Every seventh tick the oldest body goes: the next create reuses its index. */
    if ((tick % 7u) == 6u && st->oldest < st->created)
        de_body_destroy(p, st->bodies[st->oldest++]);
}

static u64 mp_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    return ((const mp_state *)c->state)->acc;
}

const de_phys_det_scenario de_phys_det_scenario_mass_props = {
    .name = "mass_props",
    .setup = mp_setup,
    .script = mp_script,
    .probe = mp_probe,
    .ticks = MP_TICKS,
    .state_size = (u32)sizeof(mp_state),
    .max_bodies = 128,
};
DE_PHYS_DET_REGISTER(mass_props)
