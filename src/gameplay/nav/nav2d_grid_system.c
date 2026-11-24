/*
 * nav2d_grid_system.c: the system `nav2d.grid` (DENG-832). It keeps one de_nav2d_grid per
 * entity with DeNavGrid2D (DENG-582), so games can run de_nav2d_find_path (DENG-79) on authored
 * grids. See de_gameplay.h, "DENG-832".
 *
 * State: instances sorted by entity bits (binary search, stable order on every machine). Each
 * instance holds the walkable bytes of its grid in a fixed DE_NAV_GRID2D_CELLS buffer, so a
 * rebuild never allocates; only a new entity grows the instance array. Each run:
 *   1. every DeNavGrid2D entity: find or insert its instance (a new one is built), mark it seen,
 *      and read its origin from the transform;
 *   2. the DE_CHANGED rows (the system's change window; every row outside a system): build again;
 *   3. drop the instances not seen (component removed, entity despawned).
 */
#include "gameplay/nav/nav_components.h"

#include <dengine/de_gameplay.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include <string.h>

#define SYS_NAME "nav2d.grid"
#define REF_NAME "DeNavGrid2DSystemRef"

typedef struct de__nav2d_inst {
    de_entity entity;
    u32 seen;     /* pass stamp of the last run that found the entity */
    u32 built;    /* pass stamp of the last build */
    u32 revision; /* grows on every build */
    bool valid;   /* the component is a valid grid */
    u32 width, height;
    f32 cell_size;
    de_vec2 origin;
    u8 walkable[DE_NAV_GRID2D_CELLS];
} de__nav2d_inst;

typedef struct de__nav2d_grid_sys {
    const de_allocator *alloc;
    de_world *w;
    de_plane_2d plane;
    de_component grid, transform, global;
    de_resource_id resource;
    de_system update;
    de__nav2d_inst *inst;
    u32 count, cap;
    u32 stamp;
    bool oom_logged;
    de_log_category log;
} de__nav2d_grid_sys;

typedef struct DeNavGrid2DSystemRef {
    de__nav2d_grid_sys *ptr;
} DeNavGrid2DSystemRef;

static de__nav2d_grid_sys *sys_of(de_world *w) {
    if (w == NULL)
        return NULL;
    const de_type_id t = de_type_find(de_world_types(w), REF_NAME);
    if (!DE_HANDLE_VALID(t))
        return NULL;
    const de_resource_id r = de_resource_find(w, t);
    if (!DE_HANDLE_VALID(r))
        return NULL;
    const DeNavGrid2DSystemRef *ref = (const DeNavGrid2DSystemRef *)de_resource_get(w, r);
    return ref != NULL ? ref->ptr : NULL;
}

/* Index of the first instance with entity bits >= `bits`. */
static u32 lower_bound(const de__nav2d_grid_sys *s, u64 bits) {
    u32 lo = 0, hi = s->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (s->inst[mid].entity.bits < bits)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

static de__nav2d_inst *find(de__nav2d_grid_sys *s, de_entity e) {
    const u32 i = lower_bound(s, e.bits);
    return i < s->count && s->inst[i].entity.bits == e.bits ? &s->inst[i] : NULL;
}

static de__nav2d_inst *insert(de__nav2d_grid_sys *s, de_entity e) {
    if (s->count == s->cap) {
        const u32 cap = s->cap != 0 ? s->cap * 2u : 4u;
        de__nav2d_inst *n = (de__nav2d_inst *)DE_ALLOC(
            s->alloc, sizeof *n * cap, DE_ALIGNOF(de__nav2d_inst), DE_MEM_GENERAL);
        if (n == NULL) {
            if (!s->oom_logged)
                DE_WARN(s->log, "nav2d.grid: out of memory for %u grids", s->count + 1u);
            s->oom_logged = true;
            return NULL;
        }
        if (s->count != 0)
            memcpy(n, s->inst, sizeof *n * s->count);
        if (s->inst != NULL)
            DE_FREE(s->alloc, s->inst, sizeof *n * s->cap);
        s->inst = n;
        s->cap = cap;
    }
    const u32 i = lower_bound(s, e.bits);
    if (i < s->count)
        memmove(&s->inst[i + 1u], &s->inst[i], sizeof *s->inst * (s->count - i));
    ++s->count;
    de__nav2d_inst *in = &s->inst[i];
    memset(in, 0, sizeof *in);
    in->entity = e;
    return in;
}

static void build(de__nav2d_grid_sys *s, de__nav2d_inst *in, const DeNavGrid2D *g) {
    if (in->built == s->stamp)
        return; /* added this run: step 1 built it already */
    in->built = s->stamp;
    ++in->revision;
    in->valid = g->width >= 1u && g->width <= 64u && g->height >= 1u && g->height <= 64u &&
                g->cell_size > 0.0f && g->cell_size <= 1e30f; /* also rejects NaN */
    if (!in->valid)
        return;
    in->width = g->width;
    in->height = g->height;
    in->cell_size = g->cell_size;
    const u32 n = g->width * g->height;
    for (u32 i = 0; i < n; ++i)
        in->walkable[i] = g->cells[i] != DE_NAV_GRID2D_BLOCKED ? 1u : 0u;
}

static de_vec2 origin_of(const de__nav2d_grid_sys *s, de_entity e) {
    const de_transform *t = NULL;
    if (DE_HANDLE_VALID(s->global))
        t = (const de_transform *)de_entity_get_const(s->w, e, s->global);
    if (t == NULL && DE_HANDLE_VALID(s->transform))
        t = (const de_transform *)de_entity_get_const(s->w, e, s->transform);
    if (t == NULL) {
        const de_vec2 z = {0.0f, 0.0f};
        return z;
    }
    return de_nav2d_from_world(s->plane, t->position);
}

static void each_grid(const de_query_chunk *chunk, void *user) {
    de__nav2d_grid_sys *s = (de__nav2d_grid_sys *)user;
    const DeNavGrid2D *cols = (const DeNavGrid2D *)chunk->columns[0];
    for (u32 i = 0; i < chunk->count; ++i) {
        const de_entity e = chunk->entities[i];
        de__nav2d_inst *in = find(s, e);
        if (in == NULL) {
            in = insert(s, e);
            if (in == NULL)
                continue;
            build(s, in, &cols[i]);
        }
        in->seen = s->stamp;
        in->origin = origin_of(s, e);
    }
}

static void each_changed(const de_query_chunk *chunk, void *user) {
    de__nav2d_grid_sys *s = (de__nav2d_grid_sys *)user;
    const DeNavGrid2D *cols = (const DeNavGrid2D *)chunk->columns[0];
    for (u32 i = 0; i < chunk->count; ++i) {
        de__nav2d_inst *in = find(s, chunk->entities[i]);
        if (in != NULL)
            build(s, in, &cols[i]);
    }
}

static void process(de__nav2d_grid_sys *s, de_query all, de_query changed) {
    if (++s->stamp == 0u)
        s->stamp = 1u; /* 0 is "never" */
    if (DE_HANDLE_VALID(all))
        de_query_each(s->w, all, each_grid, s);
    if (DE_HANDLE_VALID(changed))
        de_query_each(s->w, changed, each_changed, s);
    u32 k = 0;
    for (u32 i = 0; i < s->count; ++i) {
        if (s->inst[i].seen != s->stamp)
            continue; /* lost DeNavGrid2D or despawned */
        if (k != i)
            memcpy(&s->inst[k], &s->inst[i], sizeof *s->inst);
        ++k;
    }
    s->count = k;
}

static void update_fn(de_system_ctx *ctx) {
    de__nav2d_grid_sys *s = (de__nav2d_grid_sys *)ctx->user;
    if (ctx->queries == NULL)
        return;
    process(s, ctx->queries[0], ctx->queries[1]);
}

void de_nav2d_grid_flush(de_world *w) {
    de__nav2d_grid_sys *s = sys_of(w);
    if (s == NULL)
        return;
    const de_query_term term = {s->grid, DE_READ};
    const de_query_filter filter = {s->grid, DE_CHANGED};
    const de_query all =
        de_query_create(w, &(de_query_desc){.terms = &term, .term_count = 1});
    const de_query changed = de_query_create(
        w, &(de_query_desc){.terms = &term, .term_count = 1, .filters = &filter, .filter_count = 1});
    process(s, all, changed);
    if (DE_HANDLE_VALID(changed))
        de_query_destroy(w, changed);
    if (DE_HANDLE_VALID(all))
        de_query_destroy(w, all);
}

bool de_nav2d_grid_get(de_world *w, de_entity e, de_nav2d_grid *out, u32 *revision) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (revision != NULL)
        *revision = 0;
    de__nav2d_grid_sys *s = sys_of(w);
    if (s == NULL || out == NULL)
        return false;
    const de__nav2d_inst *in = find(s, e);
    if (in == NULL || !in->valid)
        return false;
    out->width = in->width;
    out->height = in->height;
    out->cell_size = in->cell_size;
    out->origin = in->origin;
    out->walkable = in->walkable;
    if (revision != NULL)
        *revision = in->revision;
    return true;
}

bool de_nav2d_grid_plane(de_world *w, de_plane_2d *out) {
    const de__nav2d_grid_sys *s = sys_of(w);
    if (s == NULL || out == NULL)
        return false;
    *out = s->plane;
    return true;
}

/* ---- Attach ---------------------------------------------------------------------------------- */

static void ref_destruct(void *obj) {
    DeNavGrid2DSystemRef *ref = (DeNavGrid2DSystemRef *)obj;
    de__nav2d_grid_sys *s = ref->ptr;
    if (s == NULL)
        return;
    const de_allocator *a = s->alloc;
    if (s->inst != NULL)
        DE_FREE(a, s->inst, sizeof *s->inst * s->cap);
    DE_FREE(a, s, sizeof *s);
    ref->ptr = NULL;
}

static void ref_copy(void *dst, const void *src) {
    memcpy(dst, src, sizeof(DeNavGrid2DSystemRef));
}

de_result de_nav2d_grid_attach(de_world *w, const de_nav2d_grid_system_desc *desc) {
    if (w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_grid_attach: NULL world");
    if (sys_of(w) != NULL)
        return DE_OK;
    de__nav_component_ids ids;
    DE_TRY(de__nav_components_register(w, &ids));
    const de_builtin_component_ids b = de_builtin_components(w);
    if (!DE_HANDLE_VALID(b.transform))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "de_nav2d_grid_attach: ECS built-ins");

    de_type_registry *types = de_world_types(w);
    de_type_id rt = de_type_find(types, REF_NAME);
    if (!DE_HANDLE_VALID(rt)) {
        de_type_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.name = REF_NAME;
        rd.size = sizeof(DeNavGrid2DSystemRef);
        rd.align = DE_ALIGNOF(DeNavGrid2DSystemRef);
        rd.version = 1;
        rd.hooks.destruct = ref_destruct;
        rd.hooks.copy = ref_copy;
        rt = de_type_register(types, &rd);
        if (!DE_HANDLE_VALID(rt))
            return DE_FAIL_WRAP(DE_ERR_INTERNAL, "de_nav2d_grid_attach: " REF_NAME " type");
    }

    const de_allocator *a =
        desc != NULL && desc->alloc != NULL ? desc->alloc : de_heap_allocator();
    de__nav2d_grid_sys *s = DE_NEW(a, de__nav2d_grid_sys, DE_MEM_GENERAL);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_grid_attach: system state");
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->w = w;
    s->plane = desc != NULL ? desc->plane : DE_PLANE_SIDE_XZ;
    s->grid = ids.grid;
    s->transform = b.transform;
    s->global = b.global_transform;
    s->log = de_log_register_category("nav", DE_LOG_INFO);
    const DeNavGrid2DSystemRef ref = {s};
    s->resource = de_resource_insert(w, rt, &ref);
    if (!DE_HANDLE_VALID(s->resource)) {
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_nav2d_grid_attach: resource");
    }
    const de_query_term term = {s->grid, DE_READ};
    const de_query_filter filter = {s->grid, DE_CHANGED};
    const de_query_desc qd[2] = {
        {.terms = &term, .term_count = 1},
        {.terms = &term, .term_count = 1, .filters = &filter, .filter_count = 1},
    };
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = SYS_NAME;
    sd.schedule = DE_SCHEDULE_PRE_UPDATE;
    sd.fn = update_fn;
    sd.user = s;
    sd.flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE;
    sd.queries = qd;
    sd.query_count = 2;
    s->update = de_system_add(w, &sd);
    if (!DE_HANDLE_VALID(s->update)) {
        de_resource_remove(w, s->resource); /* frees `s` through ref_destruct */
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "de_nav2d_grid_attach: " SYS_NAME " system");
    }
    return DE_OK;
}

void de_nav2d_grid_detach(de_world *w) {
    de__nav2d_grid_sys *s = sys_of(w);
    if (s == NULL)
        return;
    de_system_remove(w, s->update);
    de_resource_remove(w, s->resource); /* ref_destruct frees `s` */
}
