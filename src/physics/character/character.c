/*
 * character.c: character create / destroy, de_character_move's checks and the controller state
 * (DENG-221; see character_internal.h).
 */
#include "physics/character/character_internal.h"

#include "physics/shapes/shape_internal.h"

#include <dengine/de_debug.h>

/* Largest max_slope_rad: just below pi/2 (a vertical wall is never walkable). */
#define CHAR_MAX_SLOPE_LIMIT 1.5707963f

/* ================================ State ================================================ */

static void state_destroy(de_physics_world *p, void *state) {
    DE_FREE(p->alloc, state, sizeof(de__character_state));
}

static const de__phys_ext_vtbl state_vtbl = {state_destroy, NULL, NULL};

static de_cvar float_cvar(const char *name, const char *help, f64 def, f64 lo, f64 hi) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.help = help;
    d.type = DE_CVAR_FLOAT;
    d.default_number = def;
    d.min = lo;
    d.max = hi;
    return de_cvar_register(&d);
}

static f32 cvar_f32(de_cvar cv, f32 def, f32 lo) {
    if (!DE_HANDLE_VALID(cv))
        return def;
    const f32 v = (f32)de_cvar_float(cv);
    return de__f32_finite(v) && v >= lo ? v : def;
}

de_result de__character_ensure(de_physics_world *p, de__character_state **out) {
    de__character_state *s = de__character_get(p);
    if (s == NULL) {
        s = DE_NEW(p->alloc, de__character_state, DE_MEM_PHYSICS);
        if (s == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_character_create: controller state");
        memset(s, 0, sizeof *s);
        /* Registration takes the cvar registry lock: create time only. */
        const de_cvar mass = float_cvar(DE__CHAR_MASS_CVAR,
                                        "Character mass (kg) in the push mass ratio; read when a "
                                        "world creates its first character.",
                                        DE__CHAR_DEFAULT_MASS, 0.001, 1e6);
        const de_cvar push = float_cvar(DE__CHAR_PUSH_FORCE_CVAR,
                                        "Largest force (N) a character pushes dynamic bodies "
                                        "with; read when a world creates its first character.",
                                        DE__CHAR_DEFAULT_PUSH_FORCE, 0.0, 1e9);
        s->settings.mass = cvar_f32(mass, DE__CHAR_DEFAULT_MASS, 0.001f);
        s->settings.push_force = cvar_f32(push, DE__CHAR_DEFAULT_PUSH_FORCE, 0.0f);
        de__phys_ext_set(p, DE__PHYS_EXT_CHARACTER, s, &state_vtbl);
    }
    *out = s;
    return DE_OK;
}

/* ================================ Parameters =========================================== */

static f32 or_default(f32 v, f32 def) {
    return v == 0.0f ? def : v;
}

/* Resolves defaults and checks the per-move parameters. */
static bool resolve_params(const DeCharacter *c, de__character_params *out) {
    const f32 step = or_default(c->step_height, DE_CHARACTER_DEFAULT_STEP_HEIGHT);
    const f32 slope = or_default(c->max_slope_rad, DE_CHARACTER_DEFAULT_MAX_SLOPE_RAD);
    const f32 skin = or_default(c->skin_width, DE_CHARACTER_DEFAULT_SKIN_WIDTH);
    if (!de__f32_finite(step) || !de__f32_finite(slope) || !de__f32_finite(skin) || step < 0.0f ||
        slope < 0.0f || slope >= CHAR_MAX_SLOPE_LIMIT || skin < 0.0f)
        return false;
    out->step_height = step;
    out->cos_max_slope = de_dcos(slope);
    out->skin = skin;
    return true;
}

/* The yaw part of q (rotation about world +Z). */
static de_quat yaw_only(de_quat q) {
    const de_vec3 fwd = de__quat_rotate(de_dnormalize_quat(q), de__v3(1.0f, 0.0f, 0.0f));
    de_quat r = {0.0f, 0.0f, 0.0f, 1.0f};
    if (fwd.x == 0.0f && fwd.y == 0.0f)
        return r;
    f32 s, co;
    de_dsincos(0.5f * de_datan2(fwd.y, fwd.x), &s, &co);
    r.z = s;
    r.w = co;
    return r;
}

/* ================================ Public API =========================================== */

de_body de_character_create(de_physics_world *p, const DeCharacter *c, const de_transform *at,
                            de_entity owner) {
    de_body none = {0};
    if (p == NULL || c == NULL || at == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_character_create: NULL world, character or pose");
        return none;
    }
    const f32 radius = or_default(c->radius, DE_CHARACTER_DEFAULT_RADIUS);
    const f32 height = or_default(c->height, DE_CHARACTER_DEFAULT_HEIGHT);
    de__character_params params;
    if (!de__f32_finite(radius) || !de__f32_finite(height) || radius < 0.0f ||
        height < 2.0f * radius || !resolve_params(c, &params) || !de__d3_finite(at->position) ||
        !de__f32_finite(at->rotation.x) || !de__f32_finite(at->rotation.y) ||
        !de__f32_finite(at->rotation.z) || !de__f32_finite(at->rotation.w)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_character_create: bad parameters (radius %g, height %g, step %g, slope %g, "
                "skin %g) or non-finite pose",
                (double)radius, (double)height, (double)c->step_height, (double)c->max_slope_rad,
                (double)c->skin_width);
        return none;
    }
    de__character_state *state;
    if (de__character_ensure(p, &state) != DE_OK)
        return none;
    de_shape_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = DE_SHAPE_CAPSULE;
    sd.radius = radius;
    sd.half_height = 0.5f * height - radius;
    if (sd.half_height < 0.0f)
        sd.half_height = 0.0f;
    const de_shape shape = de_shape_create(p, &sd);
    if (!DE_HANDLE_VALID(shape))
        return none;
    de_body_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = DE_BODY_KINEMATIC;
    bd.transform.position = at->position;
    bd.transform.rotation = yaw_only(at->rotation);
    bd.transform.scale = de__v3(1.0f, 1.0f, 1.0f);
    bd.shape = shape;
    bd.owner = owner;
    const de_body b = de_body_create(p, &bd);
    if (!DE_HANDLE_VALID(b)) {
        (void)de_shape_destroy(p, shape);
        return none;
    }
    state->live += 1u;
    return b;
}

/* true when `index` is a character proxy: a kinematic body with a capsule. */
static bool is_character(const de_physics_world *p, u32 index) {
    if (p->bodies.type[index] != (u8)DE_BODY_KINEMATIC)
        return false;
    const de__shape *s = de__shape_get(p, p->bodies.shape[index]);
    return s != NULL && s->kind == DE_SHAPE_CAPSULE;
}

void de_character_destroy(de_physics_world *p, de_body character) {
    u32 i;
    if (!de__body_resolve(p, character, &i) || !is_character(p, i))
        return;
    const de_shape shape = p->bodies.shape[i];
    de_body_destroy(p, character);
    (void)de_shape_destroy(p, shape);
    de__character_state *s = de__character_get(p);
    if (s != NULL && s->live > 0u)
        s->live -= 1u;
}

de_result de_character_move(de_physics_world *p, de_body character, DeCharacter *io, f32 dt,
                            de_transform *out) {
    u32 i;
    if (p == NULL || io == NULL || !de__body_resolve(p, character, &i) || !is_character(p, i))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_character_move: NULL argument or not a character body");
    de__character_params params;
    if (!(dt > 0.0f) || !de__f32_finite(dt) || !resolve_params(io, &params) ||
        !de__v3_finite(io->desired_velocity) || !de__v3_finite(io->velocity))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_character_move: bad dt %g, parameters or velocities", (double)dt);
    de__character_move(p, i, &params, io, dt, out);
    return DE_OK;
}
