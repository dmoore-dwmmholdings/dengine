/*
 * character2d.c: the 2D kinematic character controller (DENG-258; see character2d_internal.h):
 * state, de_character2d_create / destroy / move, and the movement core.
 *
 * One move, for a proxy at position x (its center, f64), with up = plane +y:
 *
 *   1. Depenetrate (at most DE__CHAR2D_MAX_DEPEN_ITER passes): narrowphase contacts
 *      (de__collide2d, speculative skin) against the static and kinematic bodies near the proxy,
 *      in ascending body index (chains: per segment, one-way rule); a contact closer than half
 *      the skin pushes the proxy out along its normal to the skin. Dynamic bodies never push the
 *      character: the solver pushes them out of the kinematic proxy instead.
 *   2. Ground at the start (only when io->grounded): a short cast down finds the body the
 *      character stands on, for the platform carry (step 6).
 *   3. Velocity. Walking (gravity.y < 0): x from desired_velocity; y = the jump speed when
 *      grounded and desired y > 0, 0 when grounded, else the last vertical speed + gravity.y * dt.
 *      Free (gravity.y >= 0): both components from desired_velocity.
 *   4. Collide and slide (at most DE__CHAR2D_MAX_SLIDE_ITER casts of the proxy along the remaining
 *      displacement): move to the hit, backed off so the gap along the normal is the skin; try a
 *      step (up step_height, forward, down onto a supported surface) at the first wall when
 *      grounded; then clip the rest onto the hit line, stop in a corner (a motion back into the
 *      previous line), and against a too-steep floor clip only the horizontal part (no climbing)
 *      while a fall still slides down along it.
 *   5. Ground probe (walking, unless moving up): cast down by step_height + 2 skin (grounded last
 *      tick: snap) or 2 skin (airborne: landing); a supported hit makes the character grounded
 *      and moves it down to the skin above the ground.
 *   6. Platform carry: the start ground body's motion over the step (de__p2d_predict_kinematic:
 *      its linear velocity and its rotation about its center of mass) applied to the final
 *      position. Not collision-checked (the character moves in the platform's frame).
 *   7. Outputs, and the proxy's velocity = (final + carry - x) / dt, so the next step's kinematic
 *      integration lands it on the returned pose.
 *
 * Casts are the scene queries' per-body casts (de__q2_cast_body) over the candidates of the cast's
 * swept box, selected by (fraction, body index). A body the proxy already touches at the start of
 * a cast (the query's "inside" hit) is resolved with its contact normal: it blocks only when the
 * motion goes into it, so a touching body never freezes the character.
 *
 * Supported surfaces: a hit is ground when its normal is walkable (y >= cos(max slope)), or when
 * it touches the lower half at an edge and a short ray just inside the edge finds a walkable face
 * (stair noses, curbs). The ray's face normal is then the ground normal.
 *
 * Determinism (SPEC §5.7): + - * / and de_dmath only, in f64 where positions are involved; fixed
 * iteration caps; selections by fixed keys. Nothing allocates in a move.
 */
#include "physics/2d/character2d_internal.h"

/* ================================ State ================================================= */

static void state_destroy(de_physics2d_world *p, void *state) {
    de__character2d_state *s = (de__character2d_state *)state;
    DE_FREE(p->alloc, s, s->size);
}

static void state_body_removed(de_physics2d_world *p, void *state, u32 index) {
    de__character2d_state *s = (de__character2d_state *)state;
    (void)p;
    if (index < s->capacity && s->kind[index] != (u8)DE__CHAR2D_NONE) {
        s->kind[index] = (u8)DE__CHAR2D_NONE;
        if (s->live > 0u)
            s->live -= 1u;
    }
}

static const de__p2d_ext_vtbl state_vtbl = {state_destroy, NULL, state_body_removed};

static de_result state_ensure(de_physics2d_world *p, de__character2d_state **out) {
    de__character2d_state *s = de__character2d_get(p);
    if (s == NULL) {
        const u32 cap = p->bodies.capacity;
        const size_t head = (sizeof(de__character2d_state) + 15u) & ~(size_t)15u;
        const size_t size = head + (size_t)cap;
        void *block = DE_ALLOC(p->alloc, size, 16, DE_MEM_PHYSICS);
        if (block == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_character2d_create: controller state");
        memset(block, 0, size);
        s = (de__character2d_state *)block;
        s->kind = (u8 *)block + head;
        s->capacity = cap;
        s->size = size;
        s->last.ground_body = DE__Q2_NO_BODY;
        de__p2d_ext_set(p, DE__P2D_EXT_CHARACTER, s, &state_vtbl);
    }
    *out = s;
    return DE_OK;
}

/* ================================ Parameters =========================================== */

/* pi / 2 rounded down to f32: a max slope at or above it is refused. */
#define CHAR2D_MAX_SLOPE_LIMIT 1.57079625f

static f32 or_default(f32 v, f32 def) {
    return v == 0.0f ? def : v;
}

static bool resolve_params(const DeCharacter2D *c, de__character2d_params *out) {
    const f32 step = or_default(c->step_height, DE_CHARACTER2D_DEFAULT_STEP_HEIGHT);
    const f32 slope = or_default(c->max_slope_rad, DE_CHARACTER2D_DEFAULT_MAX_SLOPE_RAD);
    if (!de__f32_finite(step) || !de__f32_finite(slope) || step < 0.0f || slope < 0.0f ||
        slope >= CHAR2D_MAX_SLOPE_LIMIT)
        return false;
    out->step_height = step;
    out->cos_max_slope = de_dcos(slope);
    out->skin = DE_CHARACTER2D_SKIN_WIDTH;
    return true;
}

static u8 char_kind(const de_physics2d_world *p, u32 index) {
    const de__character2d_state *s = de__character2d_get(p);
    if (s == NULL || index >= s->capacity)
        return (u8)DE__CHAR2D_NONE;
    return s->kind[index];
}

/* ================================ Public API =========================================== */

de_body de_character2d_create(de_physics2d_world *p, const DeCharacter2D *c, de_dpoint2d at,
                              de_character2d_proxy proxy, de_entity owner) {
    de_body none = {0};
    if (p == NULL || c == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_character2d_create: NULL world or character");
        return none;
    }
    const f32 w = or_default(c->size.x, DE_CHARACTER2D_DEFAULT_WIDTH);
    const f32 h = or_default(c->size.y, DE_CHARACTER2D_DEFAULT_HEIGHT);
    de__character2d_params params;
    const bool kind_ok = proxy == DE_CHARACTER2D_PROXY_CAPSULE || proxy == DE_CHARACTER2D_PROXY_BOX;
    if (!kind_ok || !de__f32_finite(w) || !de__f32_finite(h) || !(w > 0.0f) || !(h > 0.0f) ||
        (proxy == DE_CHARACTER2D_PROXY_CAPSULE && h < w) || !resolve_params(c, &params) ||
        !de__f64_finite(at.x) || !de__f64_finite(at.y)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_character2d_create: bad parameters (proxy %d, size %g x %g, step %g, slope %g)"
                " or non-finite position",
                (int)proxy, (double)w, (double)h, (double)c->step_height, (double)c->max_slope_rad);
        return none;
    }
    de__character2d_state *state = NULL;
    if (state_ensure(p, &state) != DE_OK)
        return none;
    /* The queries walk the broadphase trees once they exist; create them now (create time) so a
     * move never allocates. */
    de__bp2d *bp = NULL;
    if (de__bp2d_ensure(p, &bp) != DE_OK)
        return none;

    de_shape2d_desc sd;
    memset(&sd, 0, sizeof sd);
    if (proxy == DE_CHARACTER2D_PROXY_CAPSULE) {
        const f32 r = 0.5f * w;
        f32 half = 0.5f * h - r;
        if (half < 0.0f)
            half = 0.0f;
        sd.kind = DE_SHAPE2D_CAPSULE;
        sd.p1 = de__v2(0.0f, -half);
        sd.p2 = de__v2(0.0f, half);
        sd.radius = r;
    } else {
        sd.kind = DE_SHAPE2D_BOX;
        sd.half_extents = de__v2(0.5f * w, 0.5f * h);
    }
    const de_shape shape = de_shape2d_create(p, &sd);
    if (!DE_HANDLE_VALID(shape))
        return none;
    de_body2d_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = DE_BODY_KINEMATIC;
    bd.transform.x = at.x;
    bd.transform.y = at.y;
    bd.transform.rotation = de__rot2d_identity();
    bd.shape = shape;
    bd.flags = DE_BODY2D_FLAG_FIXED_ROTATION;
    bd.owner = owner;
    const de_body b = de_body2d_create(p, &bd);
    if (!DE_HANDLE_VALID(b)) {
        (void)de_shape2d_destroy(p, shape);
        return none;
    }
    const u32 i = de__handle_index(b.id);
    state->kind[i] =
        (u8)(proxy == DE_CHARACTER2D_PROXY_CAPSULE ? DE__CHAR2D_CAPSULE : DE__CHAR2D_BOX);
    state->live += 1u;
    return b;
}

void de_character2d_destroy(de_physics2d_world *p, de_body character) {
    u32 i;
    if (!de__body2d_resolve(p, character, &i) || char_kind(p, i) == (u8)DE__CHAR2D_NONE)
        return;
    const de_shape shape = p->bodies.shape[i];
    de_body2d_destroy(p, character); /* body_removed clears the kind */
    (void)de_shape2d_destroy(p, shape);
}

de_result de_character2d_move(de_physics2d_world *p, de_body character, DeCharacter2D *io, f32 dt,
                              de_transform2d *out) {
    u32 i;
    if (p == NULL || io == NULL || !de__body2d_resolve(p, character, &i) ||
        char_kind(p, i) == (u8)DE__CHAR2D_NONE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_character2d_move: NULL argument or not a character body");
    de__character2d_params params;
    if (!(dt > 0.0f) || !de__f32_finite(dt) || !resolve_params(io, &params) ||
        !de__v2_finite(io->desired_velocity) || !de__v2_finite(io->velocity))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_character2d_move: bad dt %g, parameters or velocities", (double)dt);
    de__character2d_move(p, i, &params, io, dt, out);
    return DE_OK;
}

/* ================================ Kinematic prediction ================================== */

/* Box2D's b2IntegrateRotation: q + h w perp(q), normalized (f64). */
static de_rot2d integrate_rot(de_rot2d q, f32 dangle) {
    de_rot2d r;
    r.c = q.c - dangle * q.s;
    r.s = q.s + dangle * q.c;
    return de__rot2d_normalize(r);
}

/* The f32 displacement and the rotation of one step: substeps of dt / substeps. */
static void step_motion(const de_physics2d_world *p, de_vec2 v, f32 w, f32 dt, de_vec2 *dp,
                        de_rot2d *dq) {
    const u32 substeps = p->substeps > 0u ? p->substeps : 1u;
    const f32 h = dt / (f32)substeps;
    *dp = de__v2(0.0f, 0.0f);
    *dq = de__rot2d_identity();
    for (u32 s = 0; s < substeps; ++s) {
        *dp = de__v2_add(*dp, de__v2_scale(v, h));
        if (w != 0.0f)
            *dq = integrate_rot(*dq, h * w);
    }
}

/* r1 * r2 (apply r2, then r1), normalized in f64. */
static de_rot2d rot_mul(de_rot2d a, de_rot2d b) {
    const f64 ac = a.c, as = a.s, bc = b.c, bs = b.s;
    de_rot2d r;
    r.c = (f32)(ac * bc - as * bs);
    r.s = (f32)(as * bc + ac * bs);
    return de__rot2d_normalize(r);
}

void de__p2d_predict_kinematic(const de_physics2d_world *p, u32 index, de_vec2 v, f32 w, f32 dt,
                               de_transform2d *out) {
    de_vec2 dp;
    de_rot2d dq;
    step_motion(p, v, w, dt, &dp, &dq);
    const de__d2 pos = p->bodies.position[index];
    const de_rot2d r0 = p->bodies.rotation[index];
    de__d2 np = de__d2_add(pos, de__d2_from(dp));
    de_rot2d r1 = r0;
    if (dq.c != 1.0f || dq.s != 0.0f) {
        r1 = rot_mul(dq, r0);
        /* Rotation about the center of mass: keep it where the translation put it. */
        const de__d2 lc = de__d2_from(p->bodies.local_com[index]);
        np = de__d2_add(np, de__d2_sub(de__rot2d_apply_d(r0, lc), de__rot2d_apply_d(r1, lc)));
    }
    out->x = np.x;
    out->y = np.y;
    out->rotation = r1;
}

/* The displacement ground body g's motion over the next step gives point `at`. */
static de__d2 platform_carry(const de_physics2d_world *p, u32 g, de__d2 at, f32 dt) {
    if (p->bodies.type[g] == (u8)DE_BODY_STATIC)
        return de__d2_make(0.0, 0.0);
    de_vec2 dp;
    de_rot2d dq;
    step_motion(p, p->bodies.linear_velocity[g], p->bodies.angular_velocity[g], dt, &dp, &dq);
    const de__d2 com =
        de__d2_add(p->bodies.position[g],
                   de__rot2d_apply_d(p->bodies.rotation[g], de__d2_from(p->bodies.local_com[g])));
    const de__d2 rel = de__d2_sub(at, com);
    const de__d2 moved = de__d2_add(de__d2_add(com, de__d2_from(dp)), de__rot2d_apply_d(dq, rel));
    return de__d2_sub(moved, at);
}

/* ================================ Context ============================================== */

typedef struct cctx {
    de_physics2d_world *p;
    u32 self;
    const de__shape2d *shape;
    de__q2filter filter;
    const de__character2d_params *prm;
    de__character2d_stats *stats;
    f64 bottom; /* center to feet */
    bool walking;
} cctx;

/* A cast hit. */
typedef struct chit {
    f64 t;         /* fraction of the cast displacement */
    de__d2 normal; /* unit, facing the character */
    de__d2 point;  /* on the body, world */
    u32 body;
    bool inside; /* touching at the start (contact normal) */
} chit;

static f64 d2_len(de__d2 v) {
    return de_dsqrt64(de__d2_dot(v, v));
}

static de__xf2d char_xf(de__d2 pos) {
    de__xf2d x;
    x.p = pos;
    x.q = de__rot2d_identity();
    return x;
}

static de__xf2d body_xf(const de_physics2d_world *p, u32 b) {
    de__xf2d x;
    x.p = p->bodies.position[b];
    x.q = p->bodies.rotation[b];
    return x;
}

static de__d2 body_point(const de_physics2d_world *p, u32 b, de_vec2 local) {
    return de__d2_add(p->bodies.position[b],
                      de__rot2d_apply_d(p->bodies.rotation[b], de__d2_from(local)));
}

/* The proxy's world box at `pos`, grown by `grow`. */
static de_aabb2d char_box(const cctx *x, de__d2 pos, f32 grow) {
    const de_aabb2d b = de__shape2d_world_aabb(x->shape, pos, de__rot2d_identity());
    return grow > 0.0f ? de__aabb2d_grow(&b, grow) : b;
}

/* One-way rule (de__q2_in_front): is the proxy at `pos` in front of chain segment `seg` of
 * body b? Computed in a frame at `pos`. */
static bool in_front(const cctx *x, u32 b, const de__chain_segment *seg, de__d2 pos) {
    const de_physics2d_world *p = x->p;
    const de__d2 a = de__d2_sub(body_point(p, b, seg->p1), pos);
    const de__d2 e = de__d2_sub(body_point(p, b, seg->p2), pos);
    const de_vec2 n = de__rot2d_apply(p->bodies.rotation[b], seg->normal);
    de__q2proxy core;
    de__q2proxy_shape(&core, x->shape, de__rot2d_identity(), de__d2_make(0.0, 0.0));
    return de__q2_in_front(&core, de__v2((f32)a.x, (f32)a.y), de__v2((f32)e.x, (f32)e.y), n);
}

/* The segments of chain body b near the proxy box `box` (world), lowest indices first. */
typedef struct seg_list {
    u32 idx[DE__CHAR2D_MAX_SEGMENTS];
    u32 count;
} seg_list;

static bool seg_cb(void *user, u32 segment) {
    seg_list *l = (seg_list *)user;
    if (l->count == DE__CHAR2D_MAX_SEGMENTS)
        return false; /* the chain query visits in ascending index: the rest are higher */
    l->idx[l->count++] = segment;
    return true;
}

static void chain_segments(const cctx *x, u32 b, de_aabb2d box, seg_list *out) {
    const de_physics2d_world *p = x->p;
    const de__shape2d *bs = de__shape2d_get(p, p->bodies.shape[b]);
    out->count = 0;
    const de_aabb2d local =
        de__q2_box_in_frame(box, p->bodies.position[b], p->bodies.rotation[b], 0.0f);
    (void)de__chain2d_query(&bs->u.chain, local, seg_cb, out);
}

/* ================================ Casts ================================================= */

/*
 * Body `b` touches or overlaps the proxy at `pos`: does it block a motion along `delta`? Uses the
 * deepest contact whose normal (character toward body) the motion goes into.
 */
static bool contact_block(const cctx *x, u32 b, de__d2 pos, de__d2 delta, chit *out) {
    const de_physics2d_world *p = x->p;
    const de__shape2d *bs = de__shape2d_get(p, p->bodies.shape[b]);
    if (bs == NULL)
        return false;
    seg_list segs;
    segs.count = 0;
    u32 n_child = 1;
    if (bs->kind == DE_SHAPE2D_CHAIN) {
        chain_segments(x, b, char_box(x, pos, x->prm->skin), &segs);
        n_child = segs.count;
    }
    bool found = false;
    f32 best_sep = 0.0f;
    for (u32 k = 0; k < n_child; ++k) {
        u32 child = DE__P2D_CHILD_NONE;
        if (bs->kind == DE_SHAPE2D_CHAIN) {
            child = segs.idx[k];
            if (!in_front(x, b, &bs->u.chain.segments[child], pos))
                continue;
        }
        de__contact_set2d cs;
        if (de__collide2d(x->shape, char_xf(pos), bs, body_xf(p, b), child, 0.0f, &cs) == 0u)
            continue;
        const de__d2 nrm = de__d2_from(cs.normal); /* character -> body */
        if (!(de__d2_dot(delta, nrm) > 0.0))
            continue;
        for (u32 i = 0; i < cs.point_count; ++i) {
            const f32 sep = cs.points[i].separation;
            if (found && !(sep < best_sep))
                continue;
            found = true;
            best_sep = sep;
            out->normal = de__d2_scale(nrm, -1.0);
            out->point = body_point(p, b, cs.points[i].local_b);
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
    const cctx *x;
    const de__q2cast *c;
    bool found;
    chit best;
} cast_job;

static bool cast_cb(void *user, u32 body) {
    cast_job *j = (cast_job *)user;
    const cctx *x = j->x;
    de__q2hit rh;
    if (!de__q2_cast_body(x->p, body, j->c, &rh))
        return true;
    chit h;
    memset(&h, 0, sizeof h);
    if (rh.inside) {
        /* Starts touching: decide with the contact normal. */
        if (!contact_block(x, body, j->c->origin, j->c->delta, &h))
            return true;
    } else {
        h.t = rh.t;
        h.normal = de__d2_from(rh.normal);
        const f64 nl = d2_len(h.normal);
        if (nl > 0.0)
            h.normal = de__d2_scale(h.normal, 1.0 / nl);
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

/* Casts the proxy from `pos` along `delta`; the first blocking hit by (fraction, body index). */
static bool char_cast(const cctx *x, de__d2 pos, de__d2 delta, chit *out) {
    const f64 len = d2_len(delta);
    if (!(len > DE__CHAR2D_MIN_MOVE))
        return false;
    x->stats->casts += 1u;
    de__q2cast c;
    if (!de__q2cast_init(&c, x->shape, pos, de__rot2d_identity(), delta))
        return false;
    cast_job j;
    memset(&j, 0, sizeof j);
    j.x = x;
    j.c = &c;
    (void)de__q2_each_aabb(x->p, c.swept, &x->filter, cast_cb, &j);
    if (j.found)
        *out = j.best;
    return j.found;
}

/* How far to move along a cast of length `len` with hit `h`: up to the hit, less the distance
 * that leaves the skin between the proxy and the hit line. Never negative. */
static f64 stop_distance(const cctx *x, de__d2 dir, f64 len, const chit *h) {
    f64 c = -de__d2_dot(dir, h->normal);
    if (c < DE__CHAR2D_MIN_BACKOFF_COS)
        c = DE__CHAR2D_MIN_BACKOFF_COS;
    const f64 d = h->t * len - (f64)x->prm->skin / c;
    return d > 0.0 ? d : 0.0;
}

/* ================================ Ground support ======================================== */

static bool supported(const cctx *x, const chit *h, de__d2 *ground_n) {
    const f64 cos_max = (f64)x->prm->cos_max_slope;
    if (h->normal.y >= cos_max) {
        *ground_n = h->normal;
        return true;
    }
    if (!(h->normal.y > (f64)DE__CHAR2D_EDGE_MIN_NY) || h->normal.x == 0.0)
        return false;
    /* An edge: a short ray down just inside it must find a walkable face. */
    const f64 inward = h->normal.x > 0.0 ? -1.0 : 1.0;
    const de__d2 origin = de__d2_make(h->point.x + inward * (f64)DE__CHAR2D_EDGE_IN,
                                      h->point.y + (f64)DE__CHAR2D_EDGE_UP);
    de__q2ray r;
    if (!de__q2ray_init(&r, origin, de__v2(0.0f, -1.0f), 2.0f * DE__CHAR2D_EDGE_UP))
        return false;
    de__q2hit rh;
    if (!de__q2_raycast_closest(x->p, &r, &x->filter, &rh))
        return false;
    if (!(rh.t > 0.0) || !((f64)rh.normal.y >= cos_max))
        return false;
    *ground_n = de__d2_from(rh.normal);
    return true;
}

static bool ground_probe(const cctx *x, de__d2 pos, f64 dist, chit *h, de__d2 *ground_n) {
    if (!char_cast(x, pos, de__d2_make(0.0, -dist), h))
        return false;
    return supported(x, h, ground_n);
}

/* ================================ Depenetration ======================================== */

typedef struct depen_job {
    const de_physics2d_world *p;
    u32 idx[DE__CHAR2D_MAX_DEPEN_BODIES];
    u32 count;
} depen_job;

/* Keeps the lowest body indices, sorted (the visiting order does not matter). */
static bool depen_cb(void *user, u32 body) {
    depen_job *j = (depen_job *)user;
    if (j->p->bodies.type[body] == (u8)DE_BODY_DYNAMIC)
        return true;
    u32 n = j->count;
    if (n == DE__CHAR2D_MAX_DEPEN_BODIES) {
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

static void depenetrate(const cctx *x, de__d2 *pos) {
    const de_physics2d_world *p = x->p;
    const f32 skin = x->prm->skin;
    const f64 trigger = DE__CHAR2D_DEPEN_TRIGGER * (f64)skin;
    for (u32 it = 0; it < DE__CHAR2D_MAX_DEPEN_ITER; ++it) {
        const de_aabb2d box = char_box(x, *pos, skin);
        depen_job j;
        j.p = p;
        j.count = 0;
        (void)de__q2_each_aabb(p, box, &x->filter, depen_cb, &j);
        de__d2 push = de__d2_make(0.0, 0.0);
        for (u32 bi = 0; bi < j.count; ++bi) {
            const u32 b = j.idx[bi];
            const de__shape2d *bs = de__shape2d_get(p, p->bodies.shape[b]);
            if (bs == NULL)
                continue;
            seg_list segs;
            segs.count = 0;
            u32 n_child = 1;
            if (bs->kind == DE_SHAPE2D_CHAIN) {
                chain_segments(x, b, box, &segs);
                n_child = segs.count;
            }
            for (u32 k = 0; k < n_child; ++k) {
                u32 child = DE__P2D_CHILD_NONE;
                if (bs->kind == DE_SHAPE2D_CHAIN) {
                    child = segs.idx[k];
                    if (!in_front(x, b, &bs->u.chain.segments[child], *pos))
                        continue;
                }
                de__contact_set2d cs;
                if (de__collide2d(x->shape, char_xf(*pos), bs, body_xf(p, b), child, skin, &cs) ==
                    0u)
                    continue;
                const de__d2 nrm = de__d2_from(cs.normal); /* character -> body */
                for (u32 i = 0; i < cs.point_count; ++i) {
                    const f64 sep = (f64)cs.points[i].separation - de__d2_dot(push, nrm);
                    if (sep < trigger)
                        push = de__d2_sub(push, de__d2_scale(nrm, (f64)skin - sep));
                }
            }
        }
        if (push.x == 0.0 && push.y == 0.0)
            return;
        *pos = de__d2_add(*pos, push);
        x->stats->depen_iterations += 1u;
    }
}

/* ================================ Slide ================================================= */

static bool is_wall(const cctx *x, de__d2 n) {
    return x->walking && n.y < (f64)x->prm->cos_max_slope && n.y > -(f64)DE__CHAR2D_EDGE_MIN_NY;
}

/* The rest of a motion `rem` after hitting a line with normal n (facing the character). */
static de__d2 clip_to_line(const cctx *x, de__d2 rem, de__d2 n) {
    if (!x->walking || n.y >= (f64)x->prm->cos_max_slope || !(n.y > 0.0))
        return de__d2_sub(rem, de__d2_scale(n, de__d2_dot(rem, n)));
    /* Too steep to walk: the horizontal motion meets the slope as a vertical wall (no climbing
     * by projection), a fall slides down along it. Neither part gains length. */
    f64 rx = rem.x;
    if (rx * n.x < 0.0)
        rx = 0.0; /* into the slope */
    de__d2 rv = de__d2_make(0.0, rem.y);
    if (rem.y < 0.0)
        rv = de__d2_sub(rv, de__d2_scale(n, de__d2_dot(rv, n)));
    return de__d2_make(rx + rv.x, rv.y);
}

/* Tries to climb a step in front: up by step_height, forward by `fwd` (horizontal), down onto a
 * supported surface. On success moves *pos and returns true. */
static bool try_step(const cctx *x, de__d2 *pos, f64 fwd) {
    const f64 step = (f64)x->prm->step_height;
    const f64 skin = (f64)x->prm->skin;
    if (!(de__absd(fwd) > DE__CHAR2D_MIN_MOVE) || !(step > DE__CHAR2D_MIN_MOVE))
        return false;
    chit h;
    memset(&h, 0, sizeof h);
    /* Up. */
    de__d2 p1 = *pos;
    if (char_cast(x, p1, de__d2_make(0.0, step), &h))
        p1.y += stop_distance(x, de__d2_make(0.0, 1.0), step, &h);
    else
        p1.y += step;
    const f64 raised = p1.y - pos->y;
    if (!(raised > DE__CHAR2D_MIN_MOVE))
        return false;
    /* Forward. */
    const f64 flen = de__absd(fwd);
    const de__d2 dir = de__d2_make(fwd > 0.0 ? 1.0 : -1.0, 0.0);
    f64 moved = flen;
    const f64 ext = flen + skin;
    if (char_cast(x, p1, de__d2_scale(dir, ext), &h)) {
        const f64 stop = stop_distance(x, dir, ext, &h);
        moved = stop < flen ? stop : flen;
    }
    if (!(moved >= DE__CHAR2D_STEP_MIN_FORWARD))
        return false;
    const de__d2 p2 = de__d2_add(p1, de__d2_scale(dir, moved));
    /* Down onto supported ground. */
    const f64 down = raised + skin;
    de__d2 gn;
    if (!ground_probe(x, p2, down, &h, &gn))
        return false;
    /* The surface touched must be at most step_height above the feet (an edge higher than that
     * can support a raised proxy, but it is not a step). */
    const f64 feet = pos->y - x->bottom;
    if (h.point.y - feet > step + skin)
        return false;
    de__d2 p3 = p2;
    p3.y -= stop_distance(x, de__d2_make(0.0, -1.0), down, &h);
    *pos = p3;
    x->stats->stepped = true;
    return true;
}

static void slide(const cctx *x, de__d2 *pos, de__d2 disp, bool can_step) {
    const f64 skin = (f64)x->prm->skin;
    de__d2 rem = disp;
    de__d2 prev_n = de__d2_make(0.0, 0.0);
    bool have_prev = false;
    bool step_tried = !can_step;
    for (u32 it = 0; it < DE__CHAR2D_MAX_SLIDE_ITER; ++it) {
        const f64 len = d2_len(rem);
        if (!(len > DE__CHAR2D_MIN_MOVE))
            return;
        x->stats->slide_iterations += 1u;
        const de__d2 dir = de__d2_scale(rem, 1.0 / len);
        /* Cast one skin further, so the move also stops a skin short of a surface it would end
         * closer to than that. */
        const f64 ext = len + skin;
        chit h;
        memset(&h, 0, sizeof h);
        if (!char_cast(x, *pos, de__d2_scale(dir, ext), &h)) {
            *pos = de__d2_add(*pos, rem);
            return;
        }
        const f64 stop = stop_distance(x, dir, ext, &h);
        if (stop >= len) {
            *pos = de__d2_add(*pos, rem);
            return;
        }
        *pos = de__d2_add(*pos, de__d2_scale(dir, stop));
        rem = de__d2_scale(dir, len - stop);
        if (!step_tried && is_wall(x, h.normal)) {
            step_tried = true;
            if (try_step(x, pos, rem.x))
                return; /* the step used the rest of the horizontal motion */
        }
        const de__d2 next = clip_to_line(x, rem, h.normal);
        if (have_prev && de__d2_dot(next, prev_n) < 0.0)
            return; /* a corner: in 2D the crease of two lines is a point */
        rem = next;
        prev_n = h.normal;
        have_prev = true;
    }
}

/* ================================ Move ================================================== */

void de__character2d_move(de_physics2d_world *p, u32 index, const de__character2d_params *params,
                          DeCharacter2D *io, f32 dt, de_transform2d *out) {
    de__character2d_stats stats;
    memset(&stats, 0, sizeof stats);
    stats.ground_body = DE__Q2_NO_BODY;
    cctx x;
    memset(&x, 0, sizeof x);
    x.p = p;
    x.self = index;
    x.shape = de__shape2d_get(p, p->bodies.shape[index]);
    x.prm = params;
    x.stats = &stats;
    x.walking = p->gravity.y < 0.0f;
    {
        const de_aabb2d lb = x.shape->local_aabb;
        x.bottom = -(f64)lb.min.y;
    }
    de_physics_query_filter qf;
    memset(&qf, 0, sizeof qf);
    qf.filter = p->bodies.filter[index];
    qf.ignore_body = de__body2d_handle(p, index);
    de__q2filter_init(p, &qf, &x.filter);

    /* Bodies teleported since the last step are found at their new place. */
    de__bp2d_sync(p);

    const f64 skin = (f64)params->skin;
    const de__d2 pos0 = p->bodies.position[index];
    de__d2 pos = pos0;

    /* 1. Depenetrate. */
    depenetrate(&x, &pos);
    const de__d2 start = pos;

    /* 2. Ground at the start (platform carry). */
    const bool was_grounded = x.walking && io->grounded;
    u32 ground0 = DE__Q2_NO_BODY;
    if (was_grounded) {
        chit gh;
        memset(&gh, 0, sizeof gh);
        de__d2 gn;
        if (ground_probe(&x, pos, 2.0 * skin + 1e-3, &gh, &gn))
            ground0 = gh.body;
    }

    /* 3. Velocity of this move. */
    const de_vec2 want = io->desired_velocity;
    f32 vx = want.x, vy = want.y;
    bool jumping = false;
    if (x.walking) {
        if (was_grounded && want.y > 0.0f) {
            jumping = true;
        } else if (was_grounded) {
            vy = 0.0f;
        } else {
            vy = io->velocity.y + p->gravity.y * dt;
        }
    }
    const de__d2 disp = de__d2_make((f64)vx * (f64)dt, (f64)vy * (f64)dt);

    /* 4. Collide and slide (steps only from the ground, not while jumping). */
    slide(&x, &pos, disp, was_grounded && !jumping);

    /* 5. Ground probe: snap when grounded before, land when airborne; never while moving up. */
    bool grounded = false;
    if (x.walking && !(vy > 0.0f)) {
        const f64 dist = (was_grounded ? (f64)params->step_height : 0.0) + 2.0 * skin + 1e-3;
        chit gh;
        memset(&gh, 0, sizeof gh);
        de__d2 gn;
        if (ground_probe(&x, pos, dist, &gh, &gn)) {
            grounded = true;
            stats.ground_body = gh.body;
            const f64 down = stop_distance(&x, de__d2_make(0.0, -1.0), dist, &gh);
            if (down > 0.0) {
                pos.y -= down;
                stats.snapped = true;
            }
        }
    }

    /* 6. Platform carry. */
    de__d2 carry = de__d2_make(0.0, 0.0);
    if (ground0 != DE__Q2_NO_BODY) {
        carry = platform_carry(p, ground0, pos, dt);
        stats.on_platform = p->bodies.type[ground0] != (u8)DE_BODY_STATIC;
    }

    /* 7. Outputs and the proxy's velocity. */
    const f64 inv_dt = 1.0 / (f64)dt;
    const de__d2 own = de__d2_scale(de__d2_sub(pos, start), inv_dt);
    io->velocity = de__v2((f32)own.x, (f32)own.y);
    io->grounded = grounded;
    const de__d2 total = de__d2_add(de__d2_sub(pos, pos0), carry);
    const de_vec2 lin = de__v2((f32)(total.x * inv_dt), (f32)(total.y * inv_dt));
    p->bodies.linear_velocity[index] = lin;
    p->bodies.angular_velocity[index] = 0.0f;
    if (out != NULL)
        de__p2d_predict_kinematic(p, index, lin, 0.0f, dt, out);

    de__character2d_state *st = de__character2d_get(p);
    if (st != NULL)
        st->last = stats;
}
