/*
 * sky.c: de_sky_attach (DENG-206, SPEC §12.13). A POST_UPDATE system reads the first DeSky,
 * places the sun and the moon (celestial.c), drives the first directional DeLight through the
 * CPU atmosphere (atmosphere.c), and publishes a snapshot for the render stage (sky_luts.c).
 * The update allocates nothing.
 */
#include "world/sky/sky_internal.h"

#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include <math.h>
#include <string.h>

#define SKY_PI 3.14159265358979323846

struct de_sky {
    de_world *world;
    de_renderer *renderer;
    const de_allocator *alloc;
    de_system system;
    de_component c_sky, c_light, c_transform;
    de_mutex mutex; /* guards snapshot, state, has_state */
    de__sky_snapshot snapshot;
    de_sky_state state;
    bool has_state;
    /* The game thread's cache: the inputs of the last computation. */
    DeSky last;
    bool has_last;
    f64 cloud_drift[2]; /* DENG-208: the weather map's drift, accumulated with dt */
    de_sky_state computed;
    de__sky_snapshot computed_snapshot;
#if DE_HAS_RENDER
    de__sky_render *render;
#endif
};

typedef struct find_sky {
    DeSky sky;
    bool found;
} find_sky;

static void find_sky_fn(const de_query_chunk *c, void *user) {
    find_sky *f = (find_sky *)user;
    if (f->found || c->count == 0)
        return;
    f->sky = ((const DeSky *)c->columns[0])[0];
    f->found = true;
}

typedef struct find_light {
    de_entity entity;
    bool found;
} find_light;

static void find_light_fn(const de_query_chunk *c, void *user) {
    find_light *f = (find_light *)user;
    const DeLight *lights = (const DeLight *)c->columns[0];
    for (u32 i = 0; i < c->count && !f->found; ++i) {
        if (lights[i].kind == DE_LIGHT_DIRECTIONAL) {
            f->entity = c->entities[i];
            f->found = true;
        }
    }
}

static bool same_inputs(const DeSky *a, const DeSky *b) {
    return a->time_of_day_hours == b->time_of_day_hours && a->latitude_deg == b->latitude_deg &&
           a->turbidity == b->turbidity && a->day_of_year == b->day_of_year &&
           a->moon_phase == b->moon_phase && a->ground_albedo == b->ground_albedo;
}

/* Illuminance of a body (top of the atmosphere `e_top`) at the ground, per channel. */
static void ground_light(de_vec3 dir, f64 e_top, f64 mie, f64 out[3]) {
    if (dir.z <= 0.0f || e_top <= 0.0) {
        out[0] = out[1] = out[2] = 0.0;
        return;
    }
    de__sky_transmittance_cpu(DE__SKY_BOTTOM, (f64)dir.z, mie, 256, out);
    for (int k = 0; k < 3; ++k)
        out[k] *= e_top;
}

static f32 fmax3(const f64 v[3]) {
    return (f32)fmax(v[0], fmax(v[1], v[2]));
}

static void compute(de_sky *s, const DeSky *in) {
    const f64 hours = fmin(fmax((f64)in->time_of_day_hours, 0.0), 24.0);
    const f64 doy = fmin(fmax((f64)in->day_of_year, 1.0), 366.0);
    const f64 lat = fmin(fmax((f64)in->latitude_deg, -90.0), 90.0);
    const f64 jd = de__sky_local_solar_jd(doy, hours);
    const de__sky_horizontal sun = de__sky_sun(jd, lat, 0.0);
    const f64 phase = in->moon_phase >= 0.0f ? fmin((f64)in->moon_phase, 1.0) : -1.0;
    const de__sky_moon_state moon = de__sky_moon(jd, lat, 0.0, phase);
    const f64 mie = de__sky_mie_scale(in->turbidity > 0.0f ? (f64)in->turbidity : 2.0);
    /* The moon as a Lambert sphere: the phase function of its phase angle. */
    f64 ci = 2.0 * moon.illuminated_fraction - 1.0;
    ci = ci < -1.0 ? -1.0 : (ci > 1.0 ? 1.0 : ci);
    const f64 i = acos(ci);
    const f64 moon_top =
        (f64)DE__SKY_MOON_FULL_ILLUMINANCE * (sin(i) + (SKY_PI - i) * cos(i)) / SKY_PI;

    de_sky_state *st = &s->computed;
    memset(st, 0, sizeof *st);
    st->sun_direction = de__sky_horizontal_dir(sun);
    st->moon_direction = de__sky_horizontal_dir(moon.pos);
    st->sun_elevation_deg = (f32)sun.altitude_deg;
    st->moon_elevation_deg = (f32)moon.pos.altitude_deg;
    st->moon_illuminated_fraction = (f32)moon.illuminated_fraction;
    f64 es[3], em[3];
    ground_light(st->sun_direction, (f64)DE__SKY_SOLAR_ILLUMINANCE, mie, es);
    ground_light(st->moon_direction, moon_top, mie, em);
    const bool use_moon = fmax3(em) > fmax3(es);
    const f64 *e = use_moon ? em : es;
    const f32 emax = fmax3(e);
    st->light_is_moon = use_moon;
    st->light_illuminance = emax;
    st->light_color = emax > 0.0f ? (de_vec3){(f32)(e[0] / emax), (f32)(e[1] / emax),
                                              (f32)(e[2] / emax)}
                                   : (de_vec3){1.0f, 1.0f, 1.0f};

    de__sky_snapshot *sn = &s->computed_snapshot;
    sn->sun_dir = st->sun_direction;
    sn->moon_dir = st->moon_direction;
    sn->sun_weight = DE__SKY_SOLAR_ILLUMINANCE;
    sn->moon_weight = (f32)moon_top;
    sn->mie_scale = (f32)mie;
    sn->albedo = fminf(fmaxf(in->ground_albedo, 0.0f), 1.0f);
    sn->valid = true;
}

/* A rotation whose local +Z points along `toward` (a light shines along its local -Z). */
static de_quat rotation_toward(de_vec3 toward) {
    const de_vec3 z = toward;
    de_vec3 ref = fabsf(z.z) < 0.99f ? (de_vec3){0.0f, 0.0f, 1.0f} : (de_vec3){0.0f, 1.0f, 0.0f};
    de_vec3 x = {ref.y * z.z - ref.z * z.y, ref.z * z.x - ref.x * z.z, ref.x * z.y - ref.y * z.x};
    const f32 lx = sqrtf(x.x * x.x + x.y * x.y + x.z * x.z);
    x = (de_vec3){x.x / lx, x.y / lx, x.z / lx};
    const de_vec3 y = {z.y * x.z - z.z * x.y, z.z * x.x - z.x * x.z, z.x * x.y - z.y * x.x};
    const de_mat3 m = {{x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z}};
    return de_quat_from_m3(&m);
}

static void drive_light(de_sky *s, de_world *w, de_entity e) {
    const de_sky_state *st = &s->computed;
    const de_vec3 dir = st->light_is_moon ? st->moon_direction : st->sun_direction;
    const DeLight *cl = (const DeLight *)de_entity_get_const(w, e, s->c_light);
    if (cl != NULL && (cl->intensity != st->light_illuminance ||
                       cl->color.x != st->light_color.x || cl->color.y != st->light_color.y ||
                       cl->color.z != st->light_color.z)) {
        DeLight *l = (DeLight *)de_entity_get(w, e, s->c_light);
        l->intensity = st->light_illuminance;
        l->color = st->light_color;
    }
    const DeTransform *ct = (const DeTransform *)de_entity_get_const(w, e, s->c_transform);
    if (ct != NULL) {
        const de_quat q = rotation_toward(dir);
        if (ct->rotation.x != q.x || ct->rotation.y != q.y || ct->rotation.z != q.z ||
            ct->rotation.w != q.w) {
            DeTransform *t = (DeTransform *)de_entity_get(w, e, s->c_transform);
            t->rotation = q;
        }
    }
}

static void sky_system(de_system_ctx *ctx) {
    de_sky *s = (de_sky *)ctx->user;
    find_sky fs = {.found = false};
    de_query_each(ctx->world, ctx->queries[0], find_sky_fn, &fs);
    if (!fs.found)
        return;
    /* DENG-208: the clouds follow DeSky every frame and drift with the wind. */
    const de__clouds_settings clouds = de__clouds_resolve(&fs.sky);
    const f64 dt = ctx->dt > 0.0f && ctx->dt < 1.0f ? (f64)ctx->dt : 0.0;
    s->cloud_drift[0] = fmod(s->cloud_drift[0] + (f64)clouds.wind_x * dt, DE__CLOUDS_DRIFT_PERIOD_M);
    s->cloud_drift[1] = fmod(s->cloud_drift[1] + (f64)clouds.wind_y * dt, DE__CLOUDS_DRIFT_PERIOD_M);
    s->computed_snapshot.clouds = clouds;
    s->computed_snapshot.cloud_drift[0] = s->cloud_drift[0];
    s->computed_snapshot.cloud_drift[1] = s->cloud_drift[1];
    if (!s->has_last || !same_inputs(&s->last, &fs.sky)) {
        compute(s, &fs.sky);
        s->last = fs.sky;
        s->has_last = true;
        de_mutex_lock(&s->mutex);
        s->snapshot = s->computed_snapshot;
        s->state = s->computed;
        s->has_state = true;
        de_mutex_unlock(&s->mutex);
    }
    de_mutex_lock(&s->mutex);
    s->snapshot.clouds = clouds;
    s->snapshot.cloud_drift[0] = s->cloud_drift[0];
    s->snapshot.cloud_drift[1] = s->cloud_drift[1];
    de_mutex_unlock(&s->mutex);
    if (!fs.sky.drive_sun)
        return;
    find_light fl = {.found = false};
    de_query_each(ctx->world, ctx->queries[1], find_light_fn, &fl);
    if (fl.found)
        drive_light(s, ctx->world, fl.entity);
}

bool de__sky_snapshot_get(struct de_sky *s, de__sky_snapshot *out) {
    de_mutex_lock(&s->mutex);
    *out = s->snapshot;
    de_mutex_unlock(&s->mutex);
    return out->valid;
}

static void sky_free(de_sky *s) {
#if DE_HAS_RENDER
    if (s->render != NULL)
        de__sky_render_destroy(s->render);
#endif
    if (DE_HANDLE_VALID(s->system))
        de_system_remove(s->world, s->system);
    de_mutex_destroy(&s->mutex);
    DE_FREE(s->alloc, s, sizeof *s);
}

de_result de_sky_attach(de_world *world, de_renderer *renderer, de_sky **out) {
    if (out != NULL)
        *out = NULL;
    if (world == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_sky_attach: NULL argument");
    const de_component c_sky = de_component_find(world, "DeSky"),
                       c_light = de_component_find(world, "DeLight"),
                       c_transform = de_component_find(world, "DeTransform");
    if (!DE_HANDLE_VALID(c_sky) || !DE_HANDLE_VALID(c_light) || !DE_HANDLE_VALID(c_transform))
        return DE_FAIL(DE_ERR_NOT_FOUND,
                       "de_sky_attach: DeSky, DeLight or DeTransform is not registered");
#if !DE_HAS_RENDER
    if (renderer != NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_sky_attach: a renderer in a headless build");
#endif
    const de_allocator *alloc = de_heap_allocator();
    de_sky *s = DE_NEW(alloc, de_sky, DE_MEM_RENDER);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_sky_attach: no memory");
    memset(s, 0, sizeof *s);
    s->world = world;
    s->renderer = renderer;
    s->alloc = alloc;
    s->c_sky = c_sky;
    s->c_light = c_light;
    s->c_transform = c_transform;
    de_mutex_init(&s->mutex);

    const de_query_term sky_terms[] = {{c_sky, DE_READ}};
    const de_query_term light_terms[] = {{c_light, DE_READ}};
    const de_query_desc queries[] = {{.terms = sky_terms, .term_count = 1},
                                     {.terms = light_terms, .term_count = 1}};
    static const char *const k_before[] = {DE_TRANSFORM_PROPAGATE_SYSTEM};
    s->system = de_system_add(world, &(de_system_desc){.name = "de.sky",
                                                       .schedule = DE_SCHEDULE_POST_UPDATE,
                                                       .fn = sky_system,
                                                       .user = s,
                                                       .flags = DE_SYSTEM_EXCLUSIVE,
                                                       .queries = queries,
                                                       .query_count = 2,
                                                       .before = k_before,
                                                       .before_count = 1});
    if (!DE_HANDLE_VALID(s->system)) {
        sky_free(s);
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "de_sky_attach: the system");
    }
#if DE_HAS_RENDER
    if (renderer != NULL) {
        const de_result r = de__sky_render_create(renderer, s, alloc, &s->render);
        if (r != DE_OK) {
            sky_free(s);
            return r;
        }
    }
#endif
    *out = s;
    return DE_OK;
}

void de_sky_detach(de_sky *sky) {
    if (sky != NULL)
        sky_free(sky);
}

bool de_sky_get_state(const de_sky *sky, de_sky_state *out) {
    if (sky == NULL || out == NULL)
        return false;
    de_sky *s = (de_sky *)(uintptr_t)sky;
    de_mutex_lock(&s->mutex);
    const bool ok = s->has_state;
    if (ok)
        *out = s->state;
    de_mutex_unlock(&s->mutex);
    return ok;
}
