/* editor_water.c: water.info, water.surface, water.flowMap, water.probes and water.showProbes
 * (DENG-776; DIDE protocol 1.13, capability world.water).
 *
 * A water body is an entity with DeWater (a render component): an ocean, a river or a lake. A
 * physics body with DeBuoyancy floats on them. The IDE edits both through the ordinary world
 * edits; these methods answer with what the engine computes from the current components, so a
 * changed wind shows in the next reply. Every reply is in world space.
 *
 * The numbers come from the CPU water model in src/world/water (the model of the DIDE mock
 * engine). The runtime FFT ocean, river and lake rendering and the buoyancy system (DENG-268,
 * DENG-269, DENG-270) take over the evaluation; these handlers stay.
 */
#include "host/editor/editor_internal.h"

#include "world/spline/spline.h"
#include "world/water/water.h"
#include <dengine/de_game.h>
#include <dengine/de_render.h>
#include <math.h>
#include <string.h>

#define MAX_SHOWN     64u  /* bodies water.showProbes keeps */
#define MAX_ASKED     256u /* bodies one water.probes asks for */
#define MAX_BODIES    256u /* water bodies a probe is tested against */

struct de__ed_water {
    u64 shown[MAX_SHOWN];
    u32 shown_count;
};
static struct de__ed_water g_wt;

typedef struct water_ids {
    de_component water, buoyancy;
    de_spline_ids spline;
} water_ids;

static de_world *world_of(de__editor *ed) {
    return ed->e != NULL ? ed->e->world : NULL;
}

/* The water components of the world, registered on first use (the world may be replaced). */
static bool ids_of(de_world *w, water_ids *out) {
    if (w == NULL)
        return false;
    out->water = de_component_find(w, DE_WATER_COMPONENT);
    if (!DE_HANDLE_VALID(out->water)) {
        if (de_render_register_components(w) != DE_OK)
            return false;
        out->water = de_component_find(w, DE_WATER_COMPONENT);
        if (!DE_HANDLE_VALID(out->water))
            return false;
    }
    return de__water_register(w, &out->buoyancy) == DE_OK &&
           de__spline_register(w, &out->spline) == DE_OK;
}

/* Game time in seconds: the world tick over the fixed rate. It moves in Play and Simulate. */
static f32 game_time(de__editor *ed, de_world *w) {
    u32 hz = DE_TIMING_DEFAULT_FIXED_HZ;
    if (ed->h != NULL) {
        const de_timing t = de_engine_get_timing(ed->e);
        if (t.fixed_hz != 0)
            hz = t.fixed_hz;
    }
    return (f32)((f64)de_world_tick(w) / (f64)hz);
}

static de_vec3 pos_of(de_world *w, de_entity e) {
    const de_transform t = de__ed_world_xf(w, e);
    return (de_vec3){(f32)t.position.x, (f32)t.position.y, (f32)t.position.z};
}

/* The body of water entity `e` with component `c`. A river's curve goes in `poly` (scratch);
 * `rev` (may be NULL) gets the newest revision of the entity and its spline. */
static void body_of(de__editor *ed, const water_ids *ids, de_entity e, const DeWater *c,
                    de_spline_poly *poly, de_water_body *b, u64 *rev) {
    de_world *w = world_of(ed);
    const de_spline_poly *curve = NULL;
    if (rev != NULL)
        *rev = de__ed_entity_rev(ed, e);
    if (c->kind == DE_WATER_RIVER && poly != NULL && c->spline.bits != 0 &&
        de_entity_alive(w, c->spline)) {
        const DeSpline *s = (const DeSpline *)de_entity_get_const(w, c->spline, ids->spline.spline);
        if (s != NULL) {
            const de_transform t = de__ed_world_xf(w, c->spline);
            de__spline_poly(s, &t, poly);
            curve = poly;
            if (rev != NULL) {
                const u64 sr = de__ed_entity_rev(ed, c->spline);
                if (sr > *rev)
                    *rev = sr;
            }
        }
    }
    de__water_body(c, pos_of(w, e), curve, b);
}

/* The water of `bits`, or answers not_found and returns NULL. */
static const DeWater *water_of(de__editor *ed, mpack_writer_t *w, u64 id, u64 bits,
                               const char *method, water_ids *ids) {
    de_world *wd = world_of(ed);
    const de_entity e = {bits};
    const DeWater *c = NULL;
    if (ids_of(wd, ids) && de_entity_alive(wd, e))
        c = (const DeWater *)de_entity_get_const(wd, e, ids->water);
    if (c == NULL)
        de__ed_error(w, id, DE_EP_ERR_NOT_FOUND, "%s: entity %llu has no " DE_WATER_COMPONENT,
                     method, (unsigned long long)bits);
    return c;
}

static const char *kind_name(de_water_kind k) {
    switch (k) {
    case DE_WATER_RIVER:
        return "river";
    case DE_WATER_LAKE:
        return "lake";
    default:
        return "ocean";
    }
}

static void water_info(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    DE__ED_PARAMS(water_info, de_ep_water_params, q);
    water_ids ids;
    const DeWater *c = water_of(ed, w, req->id, q.entity, "water.info", &ids);
    if (c == NULL)
        return;
    static de_ep_str k_kinds[3], k_spectra[3];
    k_kinds[0] = de_ep_cstr("ocean");
    k_kinds[1] = de_ep_cstr("river");
    k_kinds[2] = de_ep_cstr("lake");
    k_spectra[0] = de_ep_cstr("phillips");
    k_spectra[1] = de_ep_cstr("jonswap");
    k_spectra[2] = de_ep_cstr("pierson_moskowitz");
    de_water_body b;
    u64 rev;
    body_of(ed, &ids, (de_entity){q.entity}, c, NULL, &b, &rev);
    de_ep_water_info r;
    memset(&r, 0, sizeof r);
    r.entity = q.entity;
    r.component = de_ep_cstr(DE_WATER_COMPONENT);
    r.buoyancy_component = de_ep_cstr(DE_BUOYANCY_COMPONENT);
    r.kinds = (de_ep_list_str){k_kinds, 3};
    r.spectra = (de_ep_list_str){k_spectra, 3};
    r.kind = de_ep_cstr(kind_name(c->kind));
    r.wave_height = de__water_wave_height(&b);
    r.max_probes = DE_WATER_MAX_PROBES;
    r.revision = rev;
    de_ep_write_response_water_info(w, req->id, &r);
}

static void water_surface(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    DE__ED_PARAMS(water_surface, de_ep_water_surface_params, q);
    water_ids ids;
    const DeWater *c = water_of(ed, w, req->id, q.entity, "water.surface", &ids);
    if (c == NULL)
        return;
    if (!isfinite(q.x) || !isfinite(q.z) || !isfinite(q.size) || q.size <= 0.0f ||
        q.resolution < 2 || q.resolution > DE_WATER_MAX_SURFACE) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "water.surface: x and z must be finite, size above 0 and resolution 2 to %u",
                     DE_WATER_MAX_SURFACE);
        return;
    }
    const u32 n = q.resolution;
    de_spline_poly *poly = (de_spline_poly *)de__ed_alloc(a, sizeof *poly);
    float *h = (float *)de__ed_alloc(a, sizeof(float) * n * n);
    if (poly == NULL || h == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "water.surface: out of scratch memory");
        return;
    }
    de_water_body b;
    u64 rev;
    body_of(ed, &ids, (de_entity){q.entity}, c, poly, &b, &rev);
    const f32 t = game_time(ed, world_of(ed));
    const f32 step = q.size / (f32)(n - 1), x0 = q.x - q.size * 0.5f, z0 = q.z - q.size * 0.5f;
    for (u32 j = 0; j < n; ++j)
        for (u32 i = 0; i < n; ++i) {
            f32 y;
            if (!de__water_surface_at(&b, x0 + (f32)i * step, z0 + (f32)j * step, t, &y))
                y = b.level; /* no water here: the body's level */
            h[j * n + i] = y;
        }
    de_ep_water_surface r;
    memset(&r, 0, sizeof r);
    r.revision = rev;
    r.time = t;
    r.wave_height = de__water_wave_height(&b);
    r.heights = (de_ep_list_f32){h, n * n};
    de_ep_write_response_water_surface(w, req->id, &r);
}

static void water_flow_map(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(water_flow_map, de_ep_water_flow_params, q);
    water_ids ids;
    const DeWater *c = water_of(ed, w, req->id, q.entity, "water.flowMap", &ids);
    if (c == NULL)
        return;
    if (q.resolution < 2 || q.resolution > DE_WATER_MAX_FLOW) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "water.flowMap: resolution must be 2 to %u", DE_WATER_MAX_FLOW);
        return;
    }
    if (c->kind != DE_WATER_RIVER) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "water.flowMap: entity %llu is a %s, not a river",
                     (unsigned long long)q.entity, kind_name(c->kind));
        return;
    }
    de_spline_poly *poly = (de_spline_poly *)de__ed_alloc(a, sizeof *poly);
    float *flow = (float *)de__ed_alloc(a, sizeof(float) * 2 * q.resolution * q.resolution);
    float *origin = (float *)de__ed_alloc(a, sizeof(float) * 4);
    if (poly == NULL || flow == NULL || origin == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "water.flowMap: out of scratch memory");
        return;
    }
    de_water_body b;
    u64 rev;
    body_of(ed, &ids, (de_entity){q.entity}, c, poly, &b, &rev);
    de_water_flow f;
    if (!de__water_flow_map(&b, q.resolution, flow, &f)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "water.flowMap: river %llu has no spline (DeWater.spline)",
                     (unsigned long long)q.entity);
        return;
    }
    origin[0] = f.origin[0];
    origin[1] = f.origin[1];
    origin[2] = f.size[0];
    origin[3] = f.size[1];
    de_ep_water_flow_map r;
    memset(&r, 0, sizeof r);
    r.revision = rev;
    r.origin = (de_ep_list_f32){origin, 2};
    r.size = (de_ep_list_f32){origin + 2, 2};
    r.width = f.width;
    r.height = f.height;
    r.flow = (de_ep_list_f32){flow, 2 * f.width * f.height};
    r.max_speed = f.max_speed;
    de_ep_write_response_water_flow_map(w, req->id, &r);
}

/* ---- Probes -------------------------------------------------------------------------------- */

typedef struct probes {
    u32 n, cap;
    u64 *owner;
    de_dvec3 *pos;
    f32 *radius, *level;
    bool *wet;
} probes;

/* Appends the probes of the bodies `ents` that have DeBuoyancy (the others are skipped). */
static void gather(de_world *w, const water_ids *ids, const u64 *ents, u32 count, probes *p) {
    for (u32 i = 0; i < count; ++i) {
        const de_entity e = {ents[i]};
        if (!de_entity_alive(w, e))
            continue;
        const DeBuoyancy *b = (const DeBuoyancy *)de_entity_get_const(w, e, ids->buoyancy);
        if (b == NULL)
            continue;
        const de_transform t = de__ed_world_xf(w, e);
        const u32 n = b->count < DE_WATER_MAX_PROBES ? b->count : DE_WATER_MAX_PROBES;
        for (u32 k = 0; k < n && p->n < p->cap; ++k) {
            const de_vec3 o = b->probes[k];
            p->owner[p->n] = ents[i];
            p->pos[p->n] = de_transform_point(&t, (de_dvec3){o.x, o.y, o.z});
            p->radius[p->n] = b->radius;
            p->level[p->n] = 0.0f;
            p->wet[p->n] = false;
            ++p->n;
        }
    }
}

typedef struct body_list {
    u64 bits[MAX_BODIES];
    u32 n;
} body_list;

static void collect_bodies(const de_query_chunk *ch, void *user) {
    body_list *l = (body_list *)user;
    for (u32 r = 0; r < ch->count && l->n < MAX_BODIES; ++r)
        l->bits[l->n++] = ch->entities[r].bits;
}

/* Sets each probe's level to the highest water surface over it, from every water body. */
static void sample(de__editor *ed, const water_ids *ids, de_spline_poly *poly, probes *p) {
    de_world *w = world_of(ed);
    static body_list bodies;
    bodies.n = 0;
    const de_query_term terms[1] = {{ids->water, DE_READ}};
    const de_query_desc qd = {terms, 1, NULL, 0};
    de_query q = de_query_create(w, &qd);
    de_query_each(w, q, collect_bodies, &bodies);
    de_query_destroy(w, q);
    const f32 t = game_time(ed, w);
    for (u32 i = 0; i < bodies.n; ++i) {
        const de_entity e = {bodies.bits[i]};
        const DeWater *c = (const DeWater *)de_entity_get_const(w, e, ids->water);
        if (c == NULL)
            continue;
        de_water_body b;
        body_of(ed, ids, e, c, poly, &b, NULL);
        for (u32 k = 0; k < p->n; ++k) {
            f32 y;
            if (!de__water_surface_at(&b, (f32)p->pos[k].x, (f32)p->pos[k].z, t, &y))
                continue;
            if (!p->wet[k] || y > p->level[k])
                p->level[k] = y;
            p->wet[k] = true;
        }
    }
}

/* Every listed entity alive, or answers not_found. */
static bool all_alive(de__editor *ed, mpack_writer_t *w, u64 id, const de_ep_list_u64 *l,
                      const char *method) {
    de_world *wd = world_of(ed);
    for (u32 i = 0; i < l->count; ++i)
        if (wd == NULL || !de_entity_alive(wd, (de_entity){l->items[i]})) {
            de__ed_error(w, id, DE_EP_ERR_NOT_FOUND, "%s: no entity %llu", method,
                         (unsigned long long)l->items[i]);
            return false;
        }
    return true;
}

static void water_probes(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(water_probes, de_ep_water_probes_params, q);
    water_ids ids;
    if (!ids_of(world_of(ed), &ids)) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "water.probes: no world");
        return;
    }
    const u64 *ents = q.has_entities ? q.entities.items : g_wt.shown;
    const u32 count = q.has_entities ? q.entities.count : g_wt.shown_count;
    if (q.has_entities) {
        if (!all_alive(ed, w, req->id, &q.entities, "water.probes"))
            return;
        if (count > MAX_ASKED) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "water.probes: at most %u entities", MAX_ASKED);
            return;
        }
    }
    const u32 cap = count * DE_WATER_MAX_PROBES + 1;
    probes p = {0, cap, (u64 *)de__ed_alloc(a, sizeof(u64) * cap),
                (de_dvec3 *)de__ed_alloc(a, sizeof(de_dvec3) * cap),
                (f32 *)de__ed_alloc(a, sizeof(f32) * cap),
                (f32 *)de__ed_alloc(a, sizeof(f32) * cap),
                (bool *)de__ed_alloc(a, sizeof(bool) * cap)};
    float *pos = (float *)de__ed_alloc(a, sizeof(float) * 3 * cap);
    float *depth = (float *)de__ed_alloc(a, sizeof(float) * cap);
    float *force = (float *)de__ed_alloc(a, sizeof(float) * cap);
    de_spline_poly *poly = (de_spline_poly *)de__ed_alloc(a, sizeof *poly);
    if (p.owner == NULL || p.pos == NULL || p.radius == NULL || p.level == NULL || p.wet == NULL ||
        pos == NULL || depth == NULL || force == NULL || poly == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "water.probes: out of scratch memory");
        return;
    }
    gather(world_of(ed), &ids, ents, count, &p);
    sample(ed, &ids, poly, &p);
    for (u32 k = 0; k < p.n; ++k) {
        pos[3 * k] = (float)p.pos[k].x;
        pos[3 * k + 1] = (float)p.pos[k].y;
        pos[3 * k + 2] = (float)p.pos[k].z;
        depth[k] = p.wet[k] ? p.level[k] - (f32)p.pos[k].y : 0.0f;
        force[k] = p.wet[k] ? de__water_probe_force(p.radius[k], depth[k]) : 0.0f;
    }
    de_ep_buoyancy_probes r;
    memset(&r, 0, sizeof r);
    r.entities = (de_ep_list_u64){p.owner, p.n};
    r.positions = (de_ep_list_f32){pos, 3 * p.n};
    r.wet = (de_ep_list_bool){p.wet, p.n};
    r.depths = (de_ep_list_f32){depth, p.n};
    r.forces = (de_ep_list_f32){force, p.n};
    r.shown = (de_ep_list_u64){g_wt.shown, g_wt.shown_count};
    de_ep_write_response_water_probes(w, req->id, &r);
}

static void water_show_probes(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                              mpack_writer_t *w) {
    DE__ED_PARAMS(water_show_probes, de_ep_water_show_probes_params, q);
    if (!all_alive(ed, w, req->id, &q.entities, "water.showProbes"))
        return;
    if (q.entities.count > MAX_SHOWN) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "water.showProbes: at most %u entities", MAX_SHOWN);
        return;
    }
    g_wt.shown_count = 0;
    for (u32 i = 0; i < q.entities.count; ++i) {
        bool dup = false;
        for (u32 j = 0; j < g_wt.shown_count; ++j)
            dup = dup || g_wt.shown[j] == q.entities.items[i];
        if (!dup)
            g_wt.shown[g_wt.shown_count++] = q.entities.items[i];
    }
    const de_ep_empty r = {0};
    de_ep_write_response_water_show_probes(w, req->id, &r);
}

u32 de__ed_water_marks(de__editor *ed, de__ed_probe_mark *out, u32 max) {
    water_ids ids;
    if (g_wt.shown_count == 0 || !ids_of(world_of(ed), &ids))
        return 0;
    enum { CAP = MAX_SHOWN * DE_WATER_MAX_PROBES };
    static u64 owner[CAP];
    static de_dvec3 pos[CAP];
    static f32 radius[CAP], level[CAP];
    static bool wet[CAP];
    static de_spline_poly poly; /* 33 KB; the overlay draws on the main thread only */
    probes p = {0, CAP, owner, pos, radius, level, wet};
    gather(world_of(ed), &ids, g_wt.shown, g_wt.shown_count, &p);
    sample(ed, &ids, &poly, &p);
    u32 n = 0;
    for (u32 k = 0; k < p.n && n < max; ++k, ++n) {
        out[n].p = pos[k];
        out[n].radius = radius[k];
        out[n].wet = wet[k];
        out[n].depth = wet[k] ? level[k] - (f32)pos[k].y : 0.0f;
    }
    return n;
}

static void reset(de__editor *ed) {
    (void)ed;
    g_wt.shown_count = 0;
}

void de__ed_water_register(de__editor *ed) {
    memset(&g_wt, 0, sizeof g_wt);
    ed->water = &g_wt;
    water_ids ids;
    (void)ids_of(world_of(ed), &ids); /* the IDE's reflection lists the components from the start */
    de__ed_on(ed, DE_EP_METHOD_WATER_INFO, water_info);
    de__ed_on(ed, DE_EP_METHOD_WATER_SURFACE, water_surface);
    de__ed_on(ed, DE_EP_METHOD_WATER_FLOW_MAP, water_flow_map);
    de__ed_on(ed, DE_EP_METHOD_WATER_PROBES, water_probes);
    de__ed_on(ed, DE_EP_METHOD_WATER_SHOW_PROBES, water_show_probes);
    de__ed_on_reset(ed, reset);
    de__ed_add_capability(ed, "world.water");
}
