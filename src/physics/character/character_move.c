/*
 * character_move.c: de__character_move, the kinematic capsule controller's movement core
 * (DENG-221, SPEC §13.1; see character_internal.h).
 *
 * One move, for a proxy at position x (capsule center, f64):
 *
 *   1. Depenetrate (at most DE__CHAR_MAX_DEPEN_ITER passes): narrowphase contacts (de__collide,
 *      speculative skin) against the static and kinematic bodies near the capsule, in ascending
 *      body index; a contact closer than half the skin pushes the capsule out along its normal to
 *      the skin (Gauss-Seidel over the contacts of a pass). Dynamic bodies never push the
 *      character: the solver pushes them out of the kinematic proxy instead.
 *   2. Ground at the start (only when io->grounded): a short cast down finds the body the
 *      character stands on, for the platform carry (step 6).
 *   3. Velocity: x and y from desired_velocity. z: the jump speed when grounded and desired z > 0;
 *      0 when grounded; else the last vertical speed plus gravity.z * dt.
 *   4. Collide and slide (at most DE__CHAR_MAX_SLIDE_ITER casts of the capsule along the remaining
 *      displacement): move to the hit, backed off so the gap along the normal is skin_width;
 *      push a dynamic body; try a step (up step_height, forward, down onto a supported surface)
 *      at the first wall when grounded; then clip the rest: onto the hit plane, along the crease
 *      with the previous plane, and for a too-steep floor with its horizontal normal (no
 *      climbing) while falling still slides down it.
 *   5. Ground probe: unless moving up, cast down by step_height + 2 skin (grounded last tick:
 *      snap) or 2 skin (airborne: landing); a supported hit makes the character grounded and
 *      moves it down to skin above the ground.
 *   6. Platform carry: the start ground body's motion over the step (linear velocity and the
 *      solver's substep rotation about its center of mass) applied to the final position, and
 *      its yaw rate to the proxy. The carry is not collision-checked (the character moves in
 *      the platform's frame).
 *   7. Outputs, and the proxy's velocity = (final + carry - x) / dt, so the next step's
 *      kinematic integration lands it on the returned pose.
 *
 * Casts are the scene queries' per-body casts (de__query_cast_body) over the candidates of the
 * cast's swept box, selected by (fraction, body index). A body the capsule already touches at
 * the start of a cast (the query's "start overlap" hit) is resolved with its contact normal: it
 * blocks only when the motion goes into it, so a touching body never freezes the character.
 *
 * Supported surfaces: a hit is ground when its normal is walkable (z >= cos(max slope)), or when
 * it touches the lower hemisphere at an edge and a short ray just inside the edge finds a
 * walkable face (stair noses, curbs). The ray's face normal is then the ground normal.
 *
 * Determinism (SPEC §5.7): + - * / and de_dmath only, in f64 where positions are involved;
 * fixed iteration caps; selections by fixed keys. Nothing allocates.
 */
#include "physics/character/character_internal.h"

#include "physics/query/query_internal.h"
#include "physics/shapes/shape_internal.h"

/* ================================ Small helpers ========================================= */

static de_dvec3 d3_zero(void) {
    return de__d3(0.0, 0.0, 0.0);
}
static f64 d3_len(de_dvec3 v) {
    return de_dsqrt64(de__d3_dot(v, v));
}
static bool d3_eq(de_dvec3 a, de_dvec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
static f64 maxd(f64 a, f64 b) {
    return a > b ? a : b;
}

/* ================================ Context ============================================== */

typedef struct char_ctx {
    de_physics_world *p;
    u32 self;
    de_body self_handle;
    const de__shape *capsule;
    de_quat rot;
    de__qfilter filter;
    de_physics_query_filter public_filter;
    const de__character_params *prm;
    de__character_settings settings;
    de__character_stats *stats;
    f32 dt;
    de_vec3 move_velocity; /* the intended velocity of this move (pushes) */
    u32 pushed[DE__CHAR_MAX_PUSHES];
    u32 pushed_count;
} char_ctx;

/* A cast hit. */
typedef struct char_hit {
    f64 t;           /* fraction of the cast displacement */
    de_dvec3 normal; /* unit, facing the character */
    de_dvec3 point;  /* on the body, world */
    u32 body;
    bool inside; /* touching at the start (contact normal) */
} char_hit;

static de__pose capsule_pose(const char_ctx *x, de_dvec3 pos) {
    de__pose ps;
    ps.position = pos;
    ps.rotation = x->rot;
    return ps;
}

static de__pose body_pose(const de_physics_world *p, u32 b) {
    de__pose ps;
    ps.position = p->bodies.position[b];
    ps.rotation = p->bodies.rotation[b];
    return ps;
}

/* World position of a point in body b's frame. */
static de_dvec3 body_point(const de_physics_world *p, u32 b, de_vec3 local) {
    const de__dmat3 r = de__dmat3_from_quat(p->bodies.rotation[b]);
    return de__d3_add(p->bodies.position[b], de__dmat3_mul(&r, de__d3_from(local)));
}

/* ================================ Casts ================================================= */

/*
 * Body `b` touches or overlaps the capsule at `pos`: does it block a motion along `delta`? Uses
 * the deepest contact whose normal (character toward body) the motion goes into.
 */
static bool contact_block(const char_ctx *x, u32 b, de_dvec3 pos, de_dvec3 delta, char_hit *out) {
    const de__shape *bs = de__shape_get(x->p, x->p->bodies.shape[b]);
    if (bs == NULL)
        return false;
    de__contact_set sets[DE__CHAR_MAX_CONTACT_SETS];
    de__contact_buffer buf;
    buf.sets = sets;
    buf.capacity = DE__CHAR_MAX_CONTACT_SETS;
    buf.count = 0;
    buf.overflow = false;
    const u32 n = de__collide(x->capsule, capsule_pose(x, pos), bs, body_pose(x->p, b), 0.0f, &buf);
    bool found = false;
    f32 best_sep = 0.0f;
    for (u32 s = 0; s < n; ++s) {
        const de__contact_set *cs = &sets[s];
        const de_dvec3 nrm = de__d3_from(cs->normal);
        if (!(de__d3_dot(delta, nrm) > 0.0))
            continue;
        for (u32 k = 0; k < cs->point_count; ++k) {
            const f32 sep = cs->points[k].separation;
            if (found && !(sep < best_sep))
                continue;
            found = true;
            best_sep = sep;
            out->normal = de__d3_scale(nrm, -1.0);
            out->point = body_point(x->p, b, cs->points[k].local_b);
        }
    }
    if (!found)
        return false;
    out->t = 0.0;
    out->body = b;
    out->inside = true;
    return true;
}

typedef struct cast_job {
    const char_ctx *x;
    const de__qcast *c;
    bool found;
    char_hit best;
} cast_job;

static bool cast_cb(void *user, u32 body) {
    cast_job *j = (cast_job *)user;
    const char_ctx *x = j->x;
    de_ray_hit rh;
    if (!de__query_cast_body(x->p, body, j->c, 1.0, 0u, &rh))
        return true;
    char_hit h;
    memset(&h, 0, sizeof h);
    if (rh.fraction == 0.0f && d3_eq(rh.point, j->c->position)) {
        /* The query's start-overlap hit: decide with the contact normal. */
        if (!contact_block(x, body, j->c->position, j->c->delta, &h))
            return true;
    } else {
        h.t = (f64)rh.fraction;
        h.normal = de__q_dnorm(de__d3_from(rh.normal));
        h.point = rh.point;
        h.body = body;
        h.inside = false;
    }
    if (!j->found || h.t < j->best.t || (h.t == j->best.t && body < j->best.body)) {
        j->found = true;
        j->best = h;
    }
    return true;
}

/* Casts the capsule from `pos` along `delta`; the first blocking hit by (fraction, body index). */
static bool char_cast(const char_ctx *x, de_dvec3 pos, de_dvec3 delta, char_hit *out) {
    const f64 len = d3_len(delta);
    if (!(len > DE__CHAR_MIN_MOVE))
        return false;
    x->stats->casts += 1u;
    de_transform from;
    memset(&from, 0, sizeof from);
    from.position = pos;
    from.rotation = x->rot;
    const de_dvec3 dir = de__d3_scale(delta, 1.0 / len);
    de__qcast c;
    if (!de__qcast_init(&c, x->capsule, &from, de__q_to_f32(dir), (f32)len))
        return false;
    cast_job j;
    memset(&j, 0, sizeof j);
    j.x = x;
    j.c = &c;
    de__query_each_aabb(x->p, c.swept, &x->filter, cast_cb, &j);
    if (j.found)
        *out = j.best;
    return j.found;
}

/*
 * How far to move along a cast of length `len` with hit `h`: up to the hit, less the distance
 * that leaves skin_width between the capsule and the hit plane. Never negative.
 */
static f64 stop_distance(const char_ctx *x, de_dvec3 dir, f64 len, const char_hit *h) {
    const f64 c = maxd(-de__d3_dot(dir, h->normal), DE__CHAR_MIN_BACKOFF_COS);
    const f64 d = h->t * len - (f64)x->prm->skin / c;
    return d > 0.0 ? d : 0.0;
}

/* ================================ Ground support ======================================== */

/*
 * Is hit `h` ground? Walkable normal, or an edge under the lower hemisphere whose inner face is
 * walkable (a ray just inside the edge). `ground_n` gets the walkable normal.
 */
static bool supported(const char_ctx *x, const char_hit *h, de_vec3 *ground_n) {
    const f32 cos_max = x->prm->cos_max_slope;
    if (h->normal.z >= (f64)cos_max) {
        *ground_n = de__q_to_f32(h->normal);
        return true;
    }
    if (!(h->normal.z > (f64)DE__CHAR_EDGE_MIN_NZ))
        return false;
    const f64 hl = de_dsqrt64(h->normal.x * h->normal.x + h->normal.y * h->normal.y);
    if (!(hl > 0.0))
        return false;
    const de_dvec3 inward = de__d3(-h->normal.x / hl, -h->normal.y / hl, 0.0);
    const de_dvec3 origin =
        de__d3_add(h->point, de__d3_add(de__d3_scale(inward, (f64)DE__CHAR_EDGE_IN),
                                        de__d3(0.0, 0.0, (f64)DE__CHAR_EDGE_UP)));
    de_ray_hit rh;
    if (!de_physics_raycast_ex(x->p, origin, de__v3(0.0f, 0.0f, -1.0f), 2.0f * DE__CHAR_EDGE_UP,
                               &x->public_filter, &rh))
        return false;
    if (!(rh.fraction > 0.0f) || !(rh.normal.z >= cos_max))
        return false;
    *ground_n = rh.normal;
    return true;
}

/* Casts down by `dist`; true with the hit and the ground normal when it is supported ground. */
static bool ground_probe(const char_ctx *x, de_dvec3 pos, f64 dist, char_hit *h,
                         de_vec3 *ground_n) {
    if (!char_cast(x, pos, de__d3(0.0, 0.0, -dist), h))
        return false;
    return supported(x, h, ground_n);
}

/* ================================ Depenetration ======================================== */

typedef struct depen_job {
    const de_physics_world *p;
    u32 idx[DE__CHAR_MAX_DEPEN_BODIES];
    u32 count;
} depen_job;

/* Keeps the lowest body indices, sorted (the visiting order does not matter). */
static bool depen_cb(void *user, u32 body) {
    depen_job *j = (depen_job *)user;
    if (j->p->bodies.type[body] == (u8)DE_BODY_DYNAMIC)
        return true;
    u32 n = j->count;
    if (n == DE__CHAR_MAX_DEPEN_BODIES) {
        if (body > j->idx[n - 1u])
            return true;
        n -= 1u;
    }
    u32 k = n;
    while (k > 0u && j->idx[k - 1u] > body) {
        j->idx[k] = j->idx[k - 1u];
        --k;
    }
    j->idx[k] = body;
    j->count = n + 1u;
    return true;
}

static void depenetrate(const char_ctx *x, de_dvec3 *pos) {
    const f32 skin = x->prm->skin;
    const f32 trigger = (f32)DE__CHAR_DEPEN_TRIGGER * skin;
    for (u32 it = 0; it < DE__CHAR_MAX_DEPEN_ITER; ++it) {
        de_aabb box = de__shape_world_aabb(x->capsule, *pos, x->rot);
        box.min = de__v3_sub(box.min, de__v3(skin, skin, skin));
        box.max = de__v3_add(box.max, de__v3(skin, skin, skin));
        depen_job j;
        j.p = x->p;
        j.count = 0;
        de__query_each_aabb(x->p, box, &x->filter, depen_cb, &j);
        de_dvec3 push = d3_zero();
        for (u32 bi = 0; bi < j.count; ++bi) {
            const u32 b = j.idx[bi];
            const de__shape *bs = de__shape_get(x->p, x->p->bodies.shape[b]);
            if (bs == NULL)
                continue;
            de__contact_set sets[DE__CHAR_MAX_CONTACT_SETS];
            de__contact_buffer buf;
            buf.sets = sets;
            buf.capacity = DE__CHAR_MAX_CONTACT_SETS;
            buf.count = 0;
            buf.overflow = false;
            const u32 n =
                de__collide(x->capsule, capsule_pose(x, *pos), bs, body_pose(x->p, b), skin, &buf);
            for (u32 s = 0; s < n; ++s) {
                const de_dvec3 nrm = de__d3_from(sets[s].normal); /* character -> body */
                for (u32 k = 0; k < sets[s].point_count; ++k) {
                    const f64 sep = (f64)sets[s].points[k].separation - de__d3_dot(push, nrm);
                    if (sep < (f64)trigger)
                        push = de__d3_sub(push, de__d3_scale(nrm, (f64)skin - sep));
                }
            }
        }
        if (push.x == 0.0 && push.y == 0.0 && push.z == 0.0)
            return;
        *pos = de__d3_add(*pos, push);
        x->stats->depen_iterations += 1u;
    }
}

/* ================================ Pushing =============================================== */

/* Pushes dynamic body h->body at the hit point (once per move). */
static void push_body(char_ctx *x, const char_hit *h) {
    de_physics_world *p = x->p;
    const u32 b = h->body;
    if (p->bodies.type[b] != (u8)DE_BODY_DYNAMIC || !(p->bodies.mass[b] > 0.0f) ||
        !(x->settings.push_force > 0.0f))
        return;
    for (u32 k = 0; k < x->pushed_count; ++k)
        if (x->pushed[k] == b)
            return;
    if (x->pushed_count == DE__CHAR_MAX_PUSHES)
        return;
    x->pushed[x->pushed_count++] = b;

    const de_dvec3 dir = de__d3_scale(h->normal, -1.0); /* into the body */
    const de__dmat3 rot = de__dmat3_from_quat(p->bodies.rotation[b]);
    const de_dvec3 com =
        de__d3_add(p->bodies.position[b], de__dmat3_mul(&rot, de__d3_from(p->bodies.local_com[b])));
    const de_dvec3 r = de__d3_sub(h->point, com);
    const de_dvec3 v = de__d3_from(p->bodies.linear_velocity[b]);
    const de_dvec3 w = de__d3_from(p->bodies.angular_velocity[b]);
    const f64 u = de__d3_dot(de__d3_from(x->move_velocity), dir);
    const f64 vb = de__d3_dot(de__d3_add(v, de__d3_cross(w, r)), dir);
    const f64 dv = u - vb;
    if (!(dv > 0.0))
        return;
    const f64 mb = (f64)p->bodies.mass[b];
    const f64 mc = (f64)x->settings.mass;
    f64 j = dv * mb * mc / (mb + mc);
    const f64 cap = (f64)x->settings.push_force * (f64)x->dt;
    if (j > cap)
        j = cap;
    if (!(j > 0.0))
        return;
    const de_dvec3 impulse = de__d3_scale(dir, j);
    const de_dvec3 v1 = de__d3_add(v, de__d3_scale(impulse, 1.0 / mb));
    /* Angular: R I^-1 R^T (r x J), with the body's rotation locks (world axes). */
    const de_dvec3 tl = de__dmat3_tmul(&rot, de__d3_cross(r, impulse));
    const de_mat3 *ii = &p->bodies.local_inv_inertia[b];
    const de_dvec3 al = de__d3((f64)ii->m[0] * tl.x + (f64)ii->m[3] * tl.y + (f64)ii->m[6] * tl.z,
                               (f64)ii->m[1] * tl.x + (f64)ii->m[4] * tl.y + (f64)ii->m[7] * tl.z,
                               (f64)ii->m[2] * tl.x + (f64)ii->m[5] * tl.y + (f64)ii->m[8] * tl.z);
    de_dvec3 dw = de__dmat3_mul(&rot, al);
    const u32 flags = p->bodies.flags[b];
    if (flags & (u32)DE_BODY_FLAG_LOCK_ROT_X)
        dw.x = 0.0;
    if (flags & (u32)DE_BODY_FLAG_LOCK_ROT_Y)
        dw.y = 0.0;
    if (flags & (u32)DE_BODY_FLAG_LOCK_ROT_Z)
        dw.z = 0.0;
    const de_body handle = de__body_handle(p, b);
    (void)de_body_set_velocity(p, handle, de__q_to_f32(v1), de__q_to_f32(de__d3_add(w, dw)));
    de_body_wake(p, handle);
    x->stats->pushes += 1u;
}

/* ================================ Slide ================================================= */

static bool is_wall(const char_ctx *x, de_dvec3 n) {
    return n.z < (f64)x->prm->cos_max_slope && n.z > -(f64)DE__CHAR_EDGE_MIN_NZ;
}

/* The rest of a motion `rem` after hitting a plane with normal n (facing the character). */
static de_dvec3 clip_to_plane(const char_ctx *x, de_dvec3 rem, de_dvec3 n) {
    if (n.z >= (f64)x->prm->cos_max_slope || !(n.z > 0.0))
        return de__d3_sub(rem, de__d3_scale(n, de__d3_dot(rem, n)));
    /* Too steep to walk: the horizontal motion meets the slope as a vertical wall (no climbing by
     * projection), a fall slides down along it. Both parts stay in the plane, and they are
     * orthogonal, so the result is never longer than rem. */
    const f64 hl = de_dsqrt64(n.x * n.x + n.y * n.y);
    const de_dvec3 nh = de__d3(n.x / hl, n.y / hl, 0.0);
    de_dvec3 rh = de__d3(rem.x, rem.y, 0.0);
    const f64 into = de__d3_dot(rh, nh);
    if (into < 0.0)
        rh = de__d3_sub(rh, de__d3_scale(nh, into));
    de_dvec3 rv = de__d3(0.0, 0.0, rem.z);
    if (rem.z < 0.0)
        rv = de__d3_sub(rv, de__d3_scale(n, de__d3_dot(rv, n)));
    return de__d3_add(rh, rv);
}

/*
 * Tries to climb a step in front: up by step_height, forward by `fwd` (horizontal), down onto a
 * supported surface. On success moves *pos and returns true.
 */
static bool try_step(const char_ctx *x, de_dvec3 *pos, de_dvec3 fwd) {
    const f64 flen = d3_len(fwd);
    const f64 step = (f64)x->prm->step_height;
    if (!(flen > DE__CHAR_MIN_MOVE) || !(step > DE__CHAR_MIN_MOVE))
        return false;
    char_hit h;
    memset(&h, 0, sizeof h);
    /* Up. */
    de_dvec3 p1 = *pos;
    const de_dvec3 up = de__d3(0.0, 0.0, step);
    if (char_cast(x, p1, up, &h))
        p1.z += stop_distance(x, de__d3(0.0, 0.0, 1.0), step, &h);
    else
        p1.z += step;
    const f64 raised = p1.z - pos->z;
    if (!(raised > DE__CHAR_MIN_MOVE))
        return false;
    /* Forward. */
    const de_dvec3 dir = de__d3_scale(fwd, 1.0 / flen);
    f64 moved = flen;
    const f64 ext = flen + (f64)x->prm->skin;
    if (char_cast(x, p1, de__d3_scale(dir, ext), &h)) {
        const f64 stop = stop_distance(x, dir, ext, &h);
        moved = stop < flen ? stop : flen;
    }
    if (!(moved >= DE__CHAR_STEP_MIN_FORWARD))
        return false;
    const de_dvec3 p2 = de__d3_add(p1, de__d3_scale(dir, moved));
    /* Down onto supported ground. */
    const f64 down = raised + (f64)x->prm->skin;
    de_vec3 gn;
    if (!ground_probe(x, p2, down, &h, &gn))
        return false;
    /* The surface touched must be at most step_height above the feet (an edge higher than that
     * can support a raised capsule, but it is not a step). */
    const f64 feet = pos->z - (f64)(x->capsule->u.capsule.half_height + x->capsule->margin);
    if (h.point.z - feet > step + (f64)x->prm->skin)
        return false;
    de_dvec3 p3 = p2;
    p3.z -= stop_distance(x, de__d3(0.0, 0.0, -1.0), down, &h);
    *pos = p3;
    x->stats->stepped = true;
    return true;
}

static void slide(char_ctx *x, de_dvec3 *pos, de_dvec3 disp, bool can_step) {
    const f64 skin = (f64)x->prm->skin;
    de_dvec3 rem = disp;
    de_dvec3 prev_n = d3_zero();
    bool have_prev = false;
    bool step_tried = !can_step;
    for (u32 it = 0; it < DE__CHAR_MAX_SLIDE_ITER; ++it) {
        const f64 len = d3_len(rem);
        if (!(len > DE__CHAR_MIN_MOVE))
            return;
        x->stats->slide_iterations += 1u;
        const de_dvec3 dir = de__d3_scale(rem, 1.0 / len);
        /* Cast skin_width further, so the move also stops skin_width short of a surface it
         * would end closer to than that. */
        const f64 ext = len + skin;
        char_hit h;
        memset(&h, 0, sizeof h);
        if (!char_cast(x, *pos, de__d3_scale(dir, ext), &h)) {
            *pos = de__d3_add(*pos, rem);
            return;
        }
        const f64 stop = stop_distance(x, dir, ext, &h);
        if (stop >= len) {
            *pos = de__d3_add(*pos, rem);
            return;
        }
        *pos = de__d3_add(*pos, de__d3_scale(dir, stop));
        rem = de__d3_scale(dir, len - stop);
        push_body(x, &h);
        if (!step_tried && is_wall(x, h.normal)) {
            step_tried = true;
            const de_dvec3 fwd = de__d3(rem.x, rem.y, 0.0);
            if (try_step(x, pos, fwd))
                return; /* the step used the rest of the horizontal motion */
        }
        de_dvec3 next = clip_to_plane(x, rem, h.normal);
        if (have_prev && de__d3_dot(next, prev_n) < 0.0) {
            /* Into the previous plane again: move along the crease of the two. */
            const de_dvec3 c = de__d3_cross(prev_n, h.normal);
            const f64 cl = d3_len(c);
            if (!(cl > 1e-9))
                return;
            const de_dvec3 cu = de__d3_scale(c, 1.0 / cl);
            next = de__d3_scale(cu, de__d3_dot(rem, cu));
        }
        rem = next;
        prev_n = h.normal;
        have_prev = true;
    }
}

/* ================================ Platforms ============================================= */

/*
 * The displacement that ground body g's motion over the next step gives point `at`, computed as
 * the solver integrates it (substeps of dt / substeps: f32 displacement of the center of mass,
 * de_dintegrate_quat rotation about it). Static bodies give zero.
 */
static de_dvec3 platform_carry(const de_physics_world *p, u32 g, de_dvec3 at, f32 dt,
                               f32 *yaw_rate) {
    *yaw_rate = 0.0f;
    if (p->bodies.type[g] == (u8)DE_BODY_STATIC)
        return d3_zero();
    const de_vec3 v = p->bodies.linear_velocity[g];
    const de_vec3 w = p->bodies.angular_velocity[g];
    const u32 substeps = p->substeps > 0u ? p->substeps : 1u;
    const f32 h = dt / (f32)substeps;
    de_vec3 dp = de__v3(0.0f, 0.0f, 0.0f);
    de_quat dq = {0.0f, 0.0f, 0.0f, 1.0f};
    const bool spin = w.x != 0.0f || w.y != 0.0f || w.z != 0.0f;
    for (u32 s = 0; s < substeps; ++s) {
        dp = de__v3_add(dp, de__v3_scale(v, h));
        if (spin)
            dq = de_dintegrate_quat(dq, w, h);
    }
    const de__dmat3 r0 = de__dmat3_from_quat(p->bodies.rotation[g]);
    const de_dvec3 com =
        de__d3_add(p->bodies.position[g], de__dmat3_mul(&r0, de__d3_from(p->bodies.local_com[g])));
    const de__dmat3 rq = de__dmat3_from_quat(dq);
    const de_dvec3 rel = de__d3_sub(at, com);
    const de_dvec3 moved = de__d3_add(de__d3_add(com, de__d3_from(dp)), de__dmat3_mul(&rq, rel));
    *yaw_rate = w.z;
    return de__d3_sub(moved, at);
}

/* ================================ Prediction ============================================ */

/* f64 product a * b (apply b, then a), as the solver's finalize computes it. */
static de_quat quat_mul_f(de_quat a, de_quat b) {
    const f64 ax = a.x, ay = a.y, az = a.z, aw = a.w;
    const f64 bx = b.x, by = b.y, bz = b.z, bw = b.w;
    de_quat q;
    q.x = (f32)(aw * bx + ax * bw + ay * bz - az * by);
    q.y = (f32)(aw * by - ax * bz + ay * bw + az * bx);
    q.z = (f32)(aw * bz + ax * by - ay * bx + az * bw);
    q.w = (f32)(aw * bw - ax * bx - ay * by - az * bz);
    return q;
}

/* The pose kinematic body i reaches in the next step with velocities v, w (integrate.c). */
static void predict_pose(const de_physics_world *p, u32 i, de_vec3 v, de_vec3 w, f32 dt,
                         de_transform *out) {
    const u32 substeps = p->substeps > 0u ? p->substeps : 1u;
    const f32 h = dt / (f32)substeps;
    de_vec3 dp = de__v3(0.0f, 0.0f, 0.0f);
    de_quat dq = {0.0f, 0.0f, 0.0f, 1.0f};
    const bool spin = w.x != 0.0f || w.y != 0.0f || w.z != 0.0f;
    for (u32 s = 0; s < substeps; ++s) {
        dp = de__v3_add(dp, de__v3_scale(v, h));
        if (spin)
            dq = de_dintegrate_quat(dq, w, h);
    }
    memset(out, 0, sizeof *out);
    const de_quat q0 = p->bodies.rotation[i];
    out->position = de__d3_add(p->bodies.position[i], de__d3_from(dp));
    out->rotation = q0;
    out->scale = de__v3(1.0f, 1.0f, 1.0f);
    const bool rotated = dq.x != 0.0f || dq.y != 0.0f || dq.z != 0.0f || dq.w != 1.0f;
    if (rotated) {
        const de_quat qn = de_dnormalize_quat(quat_mul_f(dq, q0));
        /* The capsule's center of mass is its origin: no shift. Kept general all the same. */
        const de_dvec3 lcom = de__d3_from(p->bodies.local_com[i]);
        const de__dmat3 r0 = de__dmat3_from_quat(q0);
        const de__dmat3 r1 = de__dmat3_from_quat(qn);
        out->position = de__d3_add(out->position,
                                   de__d3_sub(de__dmat3_mul(&r0, lcom), de__dmat3_mul(&r1, lcom)));
        out->rotation = qn;
    }
}

/* ================================ Move ================================================== */

void de__character_move(de_physics_world *p, u32 index, const de__character_params *params,
                        DeCharacter *io, f32 dt, de_transform *out) {
    de__character_stats stats;
    memset(&stats, 0, sizeof stats);
    char_ctx x;
    memset(&x, 0, sizeof x);
    x.p = p;
    x.self = index;
    x.self_handle = de__body_handle(p, index);
    x.capsule = de__shape_get(p, p->bodies.shape[index]);
    x.rot = p->bodies.rotation[index];
    x.prm = params;
    x.stats = &stats;
    x.dt = dt;
    const de__character_state *state = de__character_get(p);
    if (state != NULL) {
        x.settings = state->settings;
    } else {
        x.settings.mass = DE__CHAR_DEFAULT_MASS;
        x.settings.push_force = DE__CHAR_DEFAULT_PUSH_FORCE;
    }
    x.public_filter.filter = p->bodies.filter[index];
    x.public_filter.flags = 0u;
    x.public_filter.ignore_body = x.self_handle;
    de__qfilter_init(p, &x.public_filter, &x.filter);

    const f64 skin = (f64)params->skin;
    const de_dvec3 pos0 = p->bodies.position[index];
    de_dvec3 pos = pos0;

    /* 1. Depenetrate. */
    depenetrate(&x, &pos);
    const de_dvec3 start = pos;

    /* 2. Ground at the start (platform carry). */
    const bool was_grounded = io->grounded;
    u32 ground0 = DE__Q_NO_BODY;
    if (was_grounded) {
        char_hit gh;
        memset(&gh, 0, sizeof gh);
        de_vec3 gn;
        if (ground_probe(&x, pos, 2.0 * skin + 1e-3, &gh, &gn))
            ground0 = gh.body;
    }

    /* 3. Velocity of this move. */
    const de_vec3 want = io->desired_velocity;
    f32 vz;
    bool jumping = false;
    if (was_grounded && want.z > 0.0f) {
        vz = want.z;
        jumping = true;
    } else if (was_grounded) {
        vz = 0.0f;
    } else {
        vz = io->velocity.z + p->gravity.z * dt;
    }
    x.move_velocity = de__v3(want.x, want.y, vz);
    const de_dvec3 disp = de__d3((f64)want.x * (f64)dt, (f64)want.y * (f64)dt, (f64)vz * (f64)dt);

    /* 4. Collide and slide (steps only from the ground, not while jumping). */
    slide(&x, &pos, disp, was_grounded && !jumping);

    /* 5. Ground probe: snap when grounded before, land when airborne; never while moving up. */
    bool grounded = false;
    de_vec3 ground_n = de__v3(0.0f, 0.0f, 0.0f);
    de_entity ground_e = DE_ENTITY_NULL;
    if (!(vz > 0.0f)) {
        const f64 dist = (was_grounded ? (f64)params->step_height : 0.0) + 2.0 * skin + 1e-3;
        char_hit gh;
        memset(&gh, 0, sizeof gh);
        if (ground_probe(&x, pos, dist, &gh, &ground_n)) {
            grounded = true;
            ground_e = p->bodies.owner[gh.body];
            const f64 down = stop_distance(&x, de__d3(0.0, 0.0, -1.0), dist, &gh);
            if (down > 0.0) {
                pos.z -= down;
                stats.snapped = true;
            }
        } else {
            ground_n = de__v3(0.0f, 0.0f, 0.0f);
        }
    }

    /* 6. Platform carry. */
    de_dvec3 carry = d3_zero();
    f32 yaw_rate = 0.0f;
    if (ground0 != DE__Q_NO_BODY) {
        carry = platform_carry(p, ground0, pos, dt, &yaw_rate);
        stats.on_platform = p->bodies.type[ground0] != (u8)DE_BODY_STATIC;
    }

    /* 7. Outputs and the proxy's velocity. */
    const f64 inv_dt = 1.0 / (f64)dt;
    io->velocity = de__q_to_f32(de__d3_scale(de__d3_sub(pos, start), inv_dt));
    io->grounded = grounded;
    io->ground_normal = ground_n;
    io->ground_entity = ground_e;
    const de_dvec3 total = de__d3_add(de__d3_sub(pos, pos0), carry);
    const de_vec3 lin = de__q_to_f32(de__d3_scale(total, inv_dt));
    const de_vec3 ang = de__v3(0.0f, 0.0f, yaw_rate);
    p->bodies.linear_velocity[index] = lin;
    p->bodies.angular_velocity[index] = ang;
    if (out != NULL)
        predict_pose(p, index, lin, ang, dt, out);

    de__character_state *st = de__character_get(p);
    if (st != NULL)
        st->last = stats;
}
