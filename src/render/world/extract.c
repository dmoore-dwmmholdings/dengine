/*
 * extract.c: the Extract systems (SPEC §12.4, §10.2 step 5; DENG-175). de_renderer_attach adds
 * three systems to DE_SCHEDULE_EXTRACT of the world; none of them needs the main thread:
 *
 *   de.render.extract_views   takes the next render-world buffer (waiting for the render thread
 *                             to finish the frame before), moves the render origin to the main
 *                             camera, copies every DeCamera and the first DeEnvironment.
 *   de.render.extract_meshes  (after views) DeGlobalTransform + DeMeshRenderer -> instance slots.
 *   de.render.extract_lights  (after views) DeGlobalTransform + DeLight -> light slots.
 *
 * Meshes and lights touch disjoint state, so they run in parallel. Each runs three queries over
 * the same terms: the full pass (every matching entity: marks it seen, takes a slot for a new
 * one and fills it), then DE_CHANGED of the component and DE_CHANGED of DeGlobalTransform (they
 * update existing slots; a slot taken this frame is already complete). The pool's end frees the
 * slots no full pass saw, so a despawn or a removed component is one removal. The systems read
 * only through their queries (the component ids are copied at attach), so the ECS access checks
 * hold.
 */
#include "render/world/render_world.h"

#include "render/loaders/render_assets.h"
#include "render/renderer_internal.h"

#include <string.h>

#define SYS_VIEWS  "de.render.extract_views"
#define SYS_MESHES "de.render.extract_meshes"
#define SYS_LIGHTS "de.render.extract_lights"

/* ---------- Views and environment ---------- */

/* The main camera: the first DeCamera whose view is zero, else the first DeCamera. */
typedef struct main_cam {
    bool found;
    bool primary;
    de_dvec3 pos;
} main_cam;

static void find_main(const de_query_chunk *c, void *user) {
    main_cam *m = (main_cam *)user;
    const DeGlobalTransform *g = (const DeGlobalTransform *)c->columns[0];
    const DeCamera *cam = (const DeCamera *)c->columns[1];
    for (u32 i = 0; i < c->count && !m->primary; ++i) {
        const bool primary = !DE_HANDLE_VALID(cam[i].view);
        if (!m->found || primary) {
            m->found = true;
            m->primary = primary;
            m->pos = g[i].position;
        }
    }
}

typedef struct views_ctx {
    de__render_world *rw;
    de__render_world_frame *f;
    bool main_primary;
    u32 dropped;
} views_ctx;

static void fill_views(const de_query_chunk *c, void *user) {
    views_ctx *v = (views_ctx *)user;
    de__render_world_frame *f = v->f;
    const de_dvec3 origin = v->rw->origin;
    const DeGlobalTransform *g = (const DeGlobalTransform *)c->columns[0];
    const DeCamera *cam = (const DeCamera *)c->columns[1];
    for (u32 i = 0; i < c->count; ++i) {
        if (f->view_count >= f->view_capacity) {
            ++v->dropped;
            continue;
        }
        de__rw_view *out = &f->views[f->view_count];
        out->entity = c->entities[i].bits;
        out->camera = cam[i];
        out->offset = de_dv3_relative(g[i].position, origin);
        out->rotation = g[i].rotation;
        out->transform = de_transform_to_affine_relative(&g[i], origin);
        const bool primary = !DE_HANDLE_VALID(cam[i].view);
        if (f->main_view == DE__RW_NONE || (primary && !v->main_primary)) {
            f->main_view = f->view_count;
            v->main_primary = primary;
        }
        ++f->view_count;
    }
}

static void fill_environment(const de_query_chunk *c, void *user) {
    de__render_world_frame *f = (de__render_world_frame *)user;
    const DeEnvironment *env = (const DeEnvironment *)c->columns[0];
    if (f->environment_count == 0 && c->count > 0) {
        f->environment.entity = c->entities[0].bits;
        f->environment.environment = env[0];
    }
    f->environment_count += c->count;
}

static void fill_probes(const de_query_chunk *c, void *user) {
    de__render_world_frame *f = (de__render_world_frame *)user;
    const DeGlobalTransform *g = (const DeGlobalTransform *)c->columns[0];
    const DeReflectionProbe *p = (const DeReflectionProbe *)c->columns[1];
    for (u32 i = 0; i < c->count; ++i) {
        de__probe_snapshot v = {c->entities[i].bits, g[i].position, g[i].rotation, p[i]};
        u32 at = 0;
        while (at < f->probe_count && (f->probes[at].probe.priority > v.probe.priority ||
                                       (f->probes[at].probe.priority == v.probe.priority &&
                                        f->probes[at].entity < v.entity)))
            ++at;
        if (at >= 64)
            continue;
        u32 n = f->probe_count < 64 ? f->probe_count++ : 63;
        for (u32 k = n; k > at; --k)
            f->probes[k] = f->probes[k - 1];
        f->probes[at] = v;
    }
}

static void sys_views(de_system_ctx *ctx) {
    de__render_world *rw = (de__render_world *)ctx->user;
    if (ctx->world != rw->world)
        return;
    de__render_world_frame *f = de__rw_begin(rw);
    main_cam m = {0};
    de_query_each(ctx->world, ctx->queries[0], find_main, &m);
    de__rw_update_origin(rw, m.found, m.pos);
    f->origin = rw->origin;
    f->origin_changed = rw->origin_changed;
    if (de__rw_frame_reserve_views(rw, f, de_query_count(ctx->world, ctx->queries[0])) != DE_OK)
        DE_ERROR(rw->log, "render world: out of memory for the views of frame %llu",
                 (unsigned long long)rw->frame);
    views_ctx vc = {.rw = rw, .f = f};
    de_query_each(ctx->world, ctx->queries[0], fill_views, &vc);
    if (vc.dropped > 0)
        DE_ERROR(rw->log, "render world: %u cameras dropped in frame %llu", vc.dropped,
                 (unsigned long long)rw->frame);
    de_query_each(ctx->world, ctx->queries[1], fill_environment, f);
    f->probe_count = 0;
    de_query_each(ctx->world, ctx->queries[2], fill_probes, f);
    u32 probes_seen = de_query_count(ctx->world, ctx->queries[2]);
    if (probes_seen > 64)
        DE_WARN(rw->log, "probe extraction: %u exceed capacity; retaining highest priority 64",
                probes_seen);
}

/* ---------- Mesh instances ---------- */

typedef struct cat_ctx {
    de__render_world *rw;
    de_asset_system *assets;
    u32 failed;
} cat_ctx;

/* The mesh's object-space bounds, once it is loaded. */
static bool inst_try_bounds(de__render_world *rw, de_asset_system *assets, u32 s) {
    if (assets == NULL || !DE_HANDLE_VALID(rw->inst.mesh[s]))
        return false;
    const de_render_mesh *m = de_render_mesh_get(assets, rw->inst.mesh[s]);
    if (m == NULL)
        return false;
    rw->inst_pool.local_bounds[s] =
        (de_aabb){{m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]},
                  {m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]}};
    rw->inst.flags[s] |= DE__RW_INST_HAS_BOUNDS;
    de__rw_inst_update_bounds(rw, s);
    return true;
}

static void inst_fields(de__render_world *rw, de_asset_system *assets, u32 s,
                        const DeMeshRenderer *m) {
    const bool same_mesh = rw->inst.mesh[s].id == m->mesh.id;
    rw->inst.mesh[s] = m->mesh;
    memcpy(&rw->inst.materials[(size_t)s * DE__RW_MATERIALS], m->materials,
           sizeof(de_material) * DE__RW_MATERIALS);
    rw->inst.layer_mask[s] = m->layer_mask;
    u8 flags = (u8)((m->cast_shadows ? DE__RW_INST_CAST_SHADOWS : 0u) |
                    (m->visible_in_rt ? DE__RW_INST_VISIBLE_IN_RT : 0u));
    if (same_mesh)
        flags |= (u8)(rw->inst.flags[s] & DE__RW_INST_HAS_BOUNDS);
    rw->inst.flags[s] = flags;
    if (!(flags & DE__RW_INST_HAS_BOUNDS) && !inst_try_bounds(rw, assets, s))
        de__rw_inst_update_bounds(rw, s); /* empty */
    de__rw_pool_touch(rw, DE__RW_KIND_INSTANCE, s);
}

static void inst_all(const de_query_chunk *c, void *user) {
    cat_ctx *x = (cat_ctx *)user;
    de__render_world *rw = x->rw;
    const DeGlobalTransform *g = (const DeGlobalTransform *)c->columns[0];
    const DeMeshRenderer *mr = (const DeMeshRenderer *)c->columns[1];
    for (u32 i = 0; i < c->count; ++i) {
        u32 s = de__rw_pool_find(rw, DE__RW_KIND_INSTANCE, c->entities[i]);
        if (s == DE__RW_NONE) {
            s = de__rw_pool_take(rw, DE__RW_KIND_INSTANCE, c->entities[i]);
            if (s == DE__RW_NONE) {
                ++x->failed;
                continue;
            }
            inst_fields(rw, x->assets, s, &mr[i]);
            de__rw_pool_set_xform(rw, DE__RW_KIND_INSTANCE, s, &g[i], true);
            continue;
        }
        rw->inst_pool.seen[s] = rw->frame;
        if (!(rw->inst.flags[s] & DE__RW_INST_HAS_BOUNDS) && x->assets != NULL &&
            inst_try_bounds(rw, x->assets, s))
            de__rw_pool_touch(rw, DE__RW_KIND_INSTANCE, s);
    }
}

static void inst_changed_renderer(const de_query_chunk *c, void *user) {
    cat_ctx *x = (cat_ctx *)user;
    de__render_world *rw = x->rw;
    const DeMeshRenderer *mr = (const DeMeshRenderer *)c->columns[1];
    for (u32 i = 0; i < c->count; ++i) {
        const u32 s = de__rw_pool_find(rw, DE__RW_KIND_INSTANCE, c->entities[i]);
        if (s != DE__RW_NONE && rw->inst_pool.added[s] != rw->frame)
            inst_fields(rw, x->assets, s, &mr[i]);
    }
}

static void inst_changed_xform(const de_query_chunk *c, void *user) {
    cat_ctx *x = (cat_ctx *)user;
    de__render_world *rw = x->rw;
    const DeGlobalTransform *g = (const DeGlobalTransform *)c->columns[0];
    for (u32 i = 0; i < c->count; ++i) {
        const u32 s = de__rw_pool_find(rw, DE__RW_KIND_INSTANCE, c->entities[i]);
        if (s != DE__RW_NONE && rw->inst_pool.added[s] != rw->frame)
            de__rw_pool_set_xform(rw, DE__RW_KIND_INSTANCE, s, &g[i], false);
    }
}

/* ---------- Lights ---------- */

static void light_all(const de_query_chunk *c, void *user) {
    cat_ctx *x = (cat_ctx *)user;
    de__render_world *rw = x->rw;
    const DeGlobalTransform *g = (const DeGlobalTransform *)c->columns[0];
    const DeLight *l = (const DeLight *)c->columns[1];
    for (u32 i = 0; i < c->count; ++i) {
        u32 s = de__rw_pool_find(rw, DE__RW_KIND_LIGHT, c->entities[i]);
        if (s == DE__RW_NONE) {
            s = de__rw_pool_take(rw, DE__RW_KIND_LIGHT, c->entities[i]);
            if (s == DE__RW_NONE) {
                ++x->failed;
                continue;
            }
            rw->lights.light[s] = l[i];
            de__rw_pool_set_xform(rw, DE__RW_KIND_LIGHT, s, &g[i], true);
            continue;
        }
        rw->light_pool.seen[s] = rw->frame;
    }
}

static void light_changed(const de_query_chunk *c, void *user) {
    cat_ctx *x = (cat_ctx *)user;
    de__render_world *rw = x->rw;
    const DeLight *l = (const DeLight *)c->columns[1];
    for (u32 i = 0; i < c->count; ++i) {
        const u32 s = de__rw_pool_find(rw, DE__RW_KIND_LIGHT, c->entities[i]);
        if (s != DE__RW_NONE && rw->light_pool.added[s] != rw->frame) {
            rw->lights.light[s] = l[i];
            de__rw_pool_touch(rw, DE__RW_KIND_LIGHT, s);
        }
    }
}

static void light_changed_xform(const de_query_chunk *c, void *user) {
    cat_ctx *x = (cat_ctx *)user;
    de__render_world *rw = x->rw;
    const DeGlobalTransform *g = (const DeGlobalTransform *)c->columns[0];
    for (u32 i = 0; i < c->count; ++i) {
        const u32 s = de__rw_pool_find(rw, DE__RW_KIND_LIGHT, c->entities[i]);
        if (s != DE__RW_NONE && rw->light_pool.added[s] != rw->frame)
            de__rw_pool_set_xform(rw, DE__RW_KIND_LIGHT, s, &g[i], false);
    }
}

/* ---------- The category systems ---------- */

static void run_category(de_system_ctx *ctx, de__rw_kind k) {
    de__render_world *rw = (de__render_world *)ctx->user;
    if (ctx->world != rw->world)
        return;
    de__render_world_frame *f = rw->filling; /* taken by the views system, which runs first */
    if (f == NULL)
        return;
    cat_ctx x = {.rw = rw, .assets = rw->r->assets};
    const bool inst = k == DE__RW_KIND_INSTANCE;
    de__rw_pool_begin(rw, k);
    de_query_each(ctx->world, ctx->queries[0], inst ? inst_all : light_all, &x);
    de_query_each(ctx->world, ctx->queries[1], inst ? inst_changed_renderer : light_changed, &x);
    de_query_each(ctx->world, ctx->queries[2], inst ? inst_changed_xform : light_changed_xform, &x);
    if (x.failed > 0)
        DE_ERROR(rw->log, "render world: out of memory: %u %s not extracted in frame %llu",
                 x.failed, inst ? "mesh instances" : "lights", (unsigned long long)rw->frame);
    if (de__rw_pool_end(rw, k, f, true) != DE_OK)
        DE_ERROR(rw->log, "render world: %s", de_error_last() ? de_error_last()->message : "?");
    if (inst)
        rw->inst_frame = rw->frame;
    else
        rw->light_frame = rw->frame;
}

static void sys_meshes(de_system_ctx *ctx) {
    run_category(ctx, DE__RW_KIND_INSTANCE);
}

static void sys_lights(de_system_ctx *ctx) {
    run_category(ctx, DE__RW_KIND_LIGHT);
}

/* ---------- Attach ---------- */

static de_system add_category_system(de__render_world *rw, de_world *w, const char *name,
                                     de_system_fn fn, de_component comp) {
    const de_component global = rw->builtins.global_transform;
    const de_query_term terms[2] = {{global, DE_READ}, {comp, DE_READ}};
    const de_query_filter changed_comp = {comp, DE_CHANGED};
    const de_query_filter changed_global = {global, DE_CHANGED};
    const de_query_desc queries[3] = {
        {.terms = terms, .term_count = 2},
        {.terms = terms, .term_count = 2, .filters = &changed_comp, .filter_count = 1},
        {.terms = terms, .term_count = 2, .filters = &changed_global, .filter_count = 1},
    };
    const char *const after[1] = {SYS_VIEWS};
    return de_system_add(w, &(de_system_desc){.name = name,
                                              .schedule = DE_SCHEDULE_EXTRACT,
                                              .fn = fn,
                                              .user = rw,
                                              .queries = queries,
                                              .query_count = 3,
                                              .after = after,
                                              .after_count = 1});
}

static bool add_systems(de__render_world *rw, de_world *w) {
    const de_query_term cam_terms[2] = {{rw->builtins.global_transform, DE_READ},
                                        {rw->ids.camera, DE_READ}};
    const de_query_term env_terms[1] = {{rw->ids.environment, DE_READ}};
    const de_query_term probe_terms[2] = {{rw->builtins.global_transform, DE_READ},
                                          {rw->ids.reflection_probe, DE_READ}};
    const de_query_desc view_queries[3] = {{.terms = cam_terms, .term_count = 2},
                                           {.terms = env_terms, .term_count = 1},
                                           {.terms = probe_terms, .term_count = 2}};
    const de_system views = de_system_add(w, &(de_system_desc){.name = SYS_VIEWS,
                                                               .schedule = DE_SCHEDULE_EXTRACT,
                                                               .fn = sys_views,
                                                               .user = rw,
                                                               .queries = view_queries,
                                                               .query_count = 3});
    if (!DE_HANDLE_VALID(views))
        return false;
    const de_system meshes =
        add_category_system(rw, w, SYS_MESHES, sys_meshes, rw->ids.mesh_renderer);
    if (!DE_HANDLE_VALID(meshes)) {
        de_system_remove(w, views);
        return false;
    }
    const de_system lights = add_category_system(rw, w, SYS_LIGHTS, sys_lights, rw->ids.light);
    if (!DE_HANDLE_VALID(lights)) {
        de_system_remove(w, meshes);
        de_system_remove(w, views);
        return false;
    }
    rw->systems[0] = views;
    rw->systems[1] = meshes;
    rw->systems[2] = lights;
    return true;
}

void de__render_world_attach(de_renderer *r, de_world *w) {
    if (r == NULL || w == NULL)
        return;
    const DeRenderComponents *ids = de__render_components(w);
    const de_builtin_component_ids builtins = de_builtin_components(w);
    if (ids == NULL || !DE_HANDLE_VALID(builtins.global_transform)) {
        DE_ERROR(r->log, "de_renderer_attach: the render components are not registered; no "
                         "Extract systems");
        return;
    }
    de__render_world *rw = r->render_world;
    if (rw == NULL) {
        rw = DE_NEW(r->alloc, de__render_world, DE_MEM_RENDER);
        if (rw == NULL) {
            DE_ERROR(r->log, "de_renderer_attach: out of memory for the render world");
            return;
        }
        (void)de__rw_init(rw, r, r->frames_in_flight);
        if (de__rt_init(rw) != DE_OK) {
            DE_ERROR(r->log, "de_renderer_attach: render thread semaphores");
            de__rw_free(rw);
            DE_FREE(r->alloc, rw, sizeof *rw);
            return;
        }
        rw->cv_pipelined = de_cvar_register(&(de_cvar_desc){
            .name = "r.pipelined",
            .help = "Render frame N on the render thread while the simulation runs N+1 (SPEC 8); "
                    "0 renders right after Extract",
            .type = DE_CVAR_BOOL,
            .default_number = 1.0});
        rw->cv_grid = de_cvar_register(&(de_cvar_desc){
            .name = "r.origin_grid_m",
            .help = "Grid of the render origin, metres: render-world transforms are float "
                    "relative to the grid point nearest the main camera",
            .type = DE_CVAR_FLOAT,
            .default_number = DE__RW_ORIGIN_GRID_DEFAULT,
            .min = 1.0,
            .max = 1048576.0});
        r->render_world = rw;
    }
    if (rw->world == w)
        return; /* attached already: nothing changes */
    if (rw->world != NULL) {
        /* Another world before: no slot belongs to this world's entities. They are all removed
         * in the first frame of `w` (the map forgets them; the sweep frees them). */
        de__rt_wait(rw);
        if (rw->inst_pool.map_capacity > 0)
            memset(rw->inst_pool.map_gen, 0, sizeof(u32) * rw->inst_pool.map_capacity);
        if (rw->light_pool.map_capacity > 0)
            memset(rw->light_pool.map_gen, 0, sizeof(u32) * rw->light_pool.map_capacity);
    }
    rw->ids = *ids;
    rw->builtins = builtins;
    rw->world = w;
    memset(rw->systems, 0, sizeof rw->systems);
    if (!add_systems(rw, w)) {
        const de_error *e = de_error_last();
        DE_ERROR(r->log, "de_renderer_attach: Extract systems: %s", e != NULL ? e->message : "?");
    }
}

void de__render_world_destroy(de_renderer *r) {
    de__render_world *rw = de__rw_of(r);
    if (rw == NULL)
        return;
    de__rt_stop(rw);
    if (rw->world != NULL)
        for (u32 i = 0; i < 3; ++i)
            if (DE_HANDLE_VALID(rw->systems[i]))
                de_system_remove(rw->world, rw->systems[i]);
    de__rt_destroy(rw);
    de__rw_free(rw);
    DE_FREE(r->alloc, rw, sizeof *rw);
    r->render_world = NULL;
}
