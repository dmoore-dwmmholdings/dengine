/*
 * DENG-271: world partition. Streams the 256 m cells of one [worlds.<name>] world around the
 * streaming sources (see the "World streaming" section of de_asset.h).
 *
 * Open finds the world's cells among the manifest bundles ("world/<name>/cell_<cx>_<cy>") and
 * allocates everything the update needs: the cell table (sorted by cx, cy), the candidate list
 * and the source list. The update then allocates nothing from the partition's allocator; the
 * asset system and the ECS allocate only when a load starts or a cell is instantiated.
 *
 * Per update:
 *  1. Sources: de_asset_get_streaming_sources, then each entity with DeStreamingSource (at its
 *     DeGlobalTransform, else its DeTransform), at most WP_MAX_SOURCES in all.
 *  2. Loading cells whose bundle request is final: READY -> instantiate the cell scene under a
 *     new cell root (DeTransform at the cell origin, DeWorldCell); otherwise FAILED.
 *  3. Loaded, loading or failed cells outside every source's unload radius are unloaded: the
 *     root is despawned with its children and the request released. A source's unload radius is
 *     its load radius times unload_radius / load_radius (hysteresis).
 *  4. Unloaded cells inside a source's load radius are candidates, sorted nearest first (then
 *     cx, cy); loads start in that order, at most max_loads_per_frame and up to
 *     max_resident_cells cells loaded or loading.
 *  5. Far proxies step.
 */
#include "streaming_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include <stdlib.h>
#include <string.h>

#define WP_MAX_SOURCES (DE_ASSET_MAX_STREAMING_SOURCES + 48u)
#define WP_NAME_MAX 64
#define WP_DEFAULT_MAX_RESIDENT 256u

typedef struct wp_cell {
    i32 cx, cy;
    de_asset_id scene;
    const char *bundle; /* in wp->names */
    de_load_handle h;
    de_entity root;
    de_world_cell_state state;
    de__far_proxy proxy;
} wp_cell;

struct de_world_partition {
    const de_allocator *a;
    de_asset_system *assets;
    de_world *world;
    char name[WP_NAME_MAX];
    wp_cell *cells;
    u32 cell_count;
    char *names;
    size_t names_bytes;
    de__cell_key *cand;
    de_streaming_source src[WP_MAX_SOURCES];
    u32 src_count;
    de_query q_src;
    de_component c_src, c_cell, c_tf, c_gtf;
    f32 load_r, unload_r;
    u32 max_loads, max_resident;
    bool proxy_enabled;
    f32 fade;
    void (*on_cell)(void *user, i32 cx, i32 cy, de_entity root, bool loaded);
    void *user;
    de_world_partition_stats stats;
    de_log_category log;
};

/* ---- Components ---------------------------------------------------------------------------- */

static de_result wp_component(de_world *w, de_type_desc d, de_component *out) {
    *out = de_component_find(w, d.name);
    if (DE_HANDLE_VALID(*out))
        return DE_OK;
    de_type_registry *reg = de_world_types(w);
    de_type_id t = de_type_find(reg, d.name);
    if (!DE_HANDLE_VALID(t)) {
        de_result r = de_type_register_ex(reg, &d, &t, NULL);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "world partition: registering %s", d.name);
    } else {
        const de_type_desc *cur = de_type_get(reg, t);
        if (cur == NULL || cur->size != d.size || cur->field_count != d.field_count)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "world partition: the type registry has another type called %s",
                           d.name);
    }
    *out = de_component_register(w, t, DE_COMP_SERIALIZE);
    if (!DE_HANDLE_VALID(*out))
        return DE_FAIL(DE_ERR_INTERNAL, "world partition: cannot register component %s", d.name);
    return DE_OK;
}

static de_result wp_register(de_world_partition *p) {
    static const de_field_info k_src[] = {
        DE_FIELD(DeStreamingSource, radius, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_cell[] = {
        DE_FIELD(DeWorldCell, cx, DE_FK_I32, DE_FIELD_SNAPSHOT),
        DE_FIELD(DeWorldCell, cy, DE_FK_I32, DE_FIELD_SNAPSHOT),
    };
    DE_TRY(wp_component(p->world, DE_TYPE_DESC(DeStreamingSource, 1, k_src), &p->c_src));
    DE_TRY(wp_component(p->world, DE_TYPE_DESC(DeWorldCell, 1, k_cell), &p->c_cell));
    de_builtin_component_ids b = de_builtin_components(p->world);
    p->c_tf = b.transform;
    p->c_gtf = b.global_transform;
    de_query_term terms[] = {
        {p->c_src, DE_READ},
        {p->c_gtf, DE_OPTIONAL_READ},
        {p->c_tf, DE_OPTIONAL_READ},
    };
    p->q_src = de_query_create(p->world, &(de_query_desc){terms, 3, NULL, 0});
    if (!DE_HANDLE_VALID(p->q_src))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "world partition: source query");
    return DE_OK;
}

/* ---- Lookup -------------------------------------------------------------------------------- */

static int wp_cell_cmp(const void *a, const void *b) {
    const wp_cell *x = (const wp_cell *)a, *y = (const wp_cell *)b;
    if (x->cx != y->cx)
        return x->cx < y->cx ? -1 : 1;
    if (x->cy != y->cy)
        return x->cy < y->cy ? -1 : 1;
    return 0;
}

static const wp_cell *wp_find(const de_world_partition *p, i32 cx, i32 cy) {
    u32 lo = 0, hi = p->cell_count;
    wp_cell key;
    key.cx = cx;
    key.cy = cy;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        int c = wp_cell_cmp(&p->cells[mid], &key);
        if (c == 0)
            return &p->cells[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* ---- Open and close ------------------------------------------------------------------------ */

static void wp_free(de_world_partition *p) {
    const de_allocator *a = p->a;
    if (p->cand != NULL)
        DE_FREE(a, p->cand, sizeof *p->cand * p->cell_count);
    if (p->cells != NULL)
        DE_FREE(a, p->cells, sizeof *p->cells * p->cell_count);
    if (p->names != NULL)
        DE_FREE(a, p->names, p->names_bytes);
    DE_FREE(a, p, sizeof *p);
}

de_result de_world_partition_open(de_asset_system *assets, de_world *world, const char *name,
                                  const de_world_partition_desc *desc, de_world_partition **out) {
    if (out != NULL)
        *out = NULL;
    if (assets == NULL || world == NULL || name == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_partition_open: NULL argument");
    if (strlen(name) >= WP_NAME_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_partition_open: name too long");
    de_asset_world_info info;
    DE_TRY(de_asset_world_info_get(assets, name, &info));
    de_world_partition_desc d = {0};
    if (desc != NULL)
        d = *desc;
    const de_allocator *a = d.allocator != NULL ? d.allocator : de_heap_allocator();

    de_world_partition *p =
        (de_world_partition *)DE_ALLOC(a, sizeof *p, DE_ALIGNOF(de_world_partition), DE_MEM_ASSET);
    if (p == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_partition_open: out of memory");
    memset(p, 0, sizeof *p);
    p->a = a;
    p->assets = assets;
    p->world = world;
    memcpy(p->name, name, strlen(name) + 1);
    p->load_r = d.load_radius > 0.0f ? d.load_radius : info.load_radius;
    p->unload_r = d.unload_radius > 0.0f ? d.unload_radius : info.unload_radius;
    if (!(p->unload_r > p->load_r))
        p->unload_r = p->load_r * 1.25f;
    p->max_loads = d.max_loads_per_frame != 0 ? d.max_loads_per_frame : info.max_loads_per_frame;
    p->max_resident = d.max_resident_cells != 0 ? d.max_resident_cells : WP_DEFAULT_MAX_RESIDENT;
    p->proxy_enabled = info.proxy_enabled;
    p->fade = info.proxy_fade_seconds;
    p->on_cell = d.on_cell;
    p->user = d.user;
    p->log = de_log_register_category("streaming", DE_LOG_INFO);

    de_result r = wp_register(p);
    if (r != DE_OK) {
        wp_free(p);
        return r;
    }

    /* Count the cells and their name bytes, then copy. */
    u32 nb = de_asset_bundle_count(assets), n = 0;
    size_t bytes = 0;
    for (u32 i = 0; i < nb; ++i) {
        const char *bn = de_asset_bundle_name(assets, i);
        i32 cx, cy;
        if (bn != NULL && de__cell_name_parse(bn, name, &cx, &cy)) {
            ++n;
            bytes += strlen(bn) + 1;
        }
    }
    if (n != 0) {
        p->cells = (wp_cell *)DE_ALLOC(a, sizeof *p->cells * n, DE_ALIGNOF(wp_cell), DE_MEM_ASSET);
        p->cand = (de__cell_key *)DE_ALLOC(a, sizeof *p->cand * n, DE_ALIGNOF(de__cell_key),
                                           DE_MEM_ASSET);
        p->names = (char *)DE_ALLOC(a, bytes, 1, DE_MEM_ASSET);
        p->cell_count = n;
        p->names_bytes = bytes;
        if (p->cells == NULL || p->cand == NULL || p->names == NULL) {
            wp_free(p);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_partition_open: out of memory");
        }
        memset(p->cells, 0, sizeof *p->cells * n);
        size_t at = 0;
        u32 k = 0;
        for (u32 i = 0; i < nb; ++i) {
            const char *bn = de_asset_bundle_name(assets, i);
            i32 cx, cy;
            if (bn == NULL || !de__cell_name_parse(bn, name, &cx, &cy))
                continue;
            size_t len = strlen(bn) + 1;
            memcpy(p->names + at, bn, len);
            wp_cell *c = &p->cells[k++];
            c->cx = cx;
            c->cy = cy;
            c->bundle = p->names + at;
            c->scene = de_asset_id_from(c->bundle);
            c->root = DE_ENTITY_NULL;
            c->state = DE_WORLD_CELL_UNLOADED;
            de__far_proxy_reset(&c->proxy);
            at += len;
        }
        qsort(p->cells, n, sizeof *p->cells, wp_cell_cmp);
    }
    p->stats.cell_count = n;
    *out = p;
    return DE_OK;
}

static void wp_unload(de_world_partition *p, wp_cell *c) {
    if (c->state == DE_WORLD_CELL_LOADED) {
        if (p->on_cell != NULL)
            p->on_cell(p->user, c->cx, c->cy, c->root, false);
        if (de_entity_alive(p->world, c->root))
            de_entity_despawn_recursive(p->world, c->root);
        p->stats.unloads++;
        p->stats.total_unloads++;
    }
    if (DE_HANDLE_VALID(c->h))
        de_asset_release(p->assets, c->h);
    c->h = (de_load_handle){0};
    c->root = DE_ENTITY_NULL;
    c->state = DE_WORLD_CELL_UNLOADED;
}

void de_world_partition_close(de_world_partition *p) {
    if (p == NULL)
        return;
    for (u32 i = 0; i < p->cell_count; ++i)
        wp_unload(p, &p->cells[i]);
    de_query_destroy(p->world, p->q_src);
    wp_free(p);
}

/* ---- Update -------------------------------------------------------------------------------- */

static void wp_gather_chunk(const de_query_chunk *ch, void *user) {
    de_world_partition *p = (de_world_partition *)user;
    const DeStreamingSource *s = (const DeStreamingSource *)ch->columns[0];
    const de_transform *g = (const de_transform *)ch->columns[1];
    const de_transform *t = (const de_transform *)ch->columns[2];
    for (u32 i = 0; i < ch->count && p->src_count < WP_MAX_SOURCES; ++i) {
        const de_transform *x = g != NULL ? &g[i] : (t != NULL ? &t[i] : NULL);
        if (x == NULL)
            continue;
        de_streaming_source *o = &p->src[p->src_count++];
        o->x = x->position.x;
        o->y = x->position.y;
        o->z = x->position.z;
        o->radius = s[i].radius;
    }
}

static void wp_instantiate(de_world_partition *p, wp_cell *c) {
    de_entity root = de_entity_spawn(p->world);
    /* Add both, then look both up: each add moves the entity to another archetype. */
    bool added = de_entity_add(p->world, root, p->c_tf) != NULL &&
                 de_entity_add(p->world, root, p->c_cell) != NULL;
    de_transform *tf = added ? (de_transform *)de_entity_get(p->world, root, p->c_tf) : NULL;
    DeWorldCell *wc = added ? (DeWorldCell *)de_entity_get(p->world, root, p->c_cell) : NULL;
    if (tf == NULL || wc == NULL) {
        de_entity_despawn_recursive(p->world, root);
        DE_WARN(p->log, "world %s: cannot spawn the root of cell (%d, %d)", p->name, (int)c->cx,
                (int)c->cy);
        c->state = DE_WORLD_CELL_FAILED;
        return;
    }
    tf->position = de__cell_origin(c->cx, c->cy);
    tf->rotation.x = tf->rotation.y = tf->rotation.z = 0.0f;
    tf->rotation.w = 1.0f;
    tf->scale.x = tf->scale.y = tf->scale.z = 1.0f;
    wc->cx = c->cx;
    wc->cy = c->cy;
    de_prefab scene = de_asset_scene(p->assets, c->scene);
    de_result r = de_prefab_instantiate(p->assets, p->world, scene, root, NULL);
    if (r != DE_OK) {
        de_entity_despawn_recursive(p->world, root);
        DE_WARN(p->log, "world %s: cannot instantiate cell (%d, %d): %s", p->name, (int)c->cx,
                (int)c->cy, de_result_str(r));
        c->state = DE_WORLD_CELL_FAILED;
        return;
    }
    c->root = root;
    c->state = DE_WORLD_CELL_LOADED;
    p->stats.total_loads++;
    if (p->on_cell != NULL)
        p->on_cell(p->user, c->cx, c->cy, root, true);
}

de_result de_world_partition_update(de_world_partition *p, f32 dt) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_partition_update: NULL partition");
    p->stats.loads_started = 0;
    p->stats.unloads = 0;

    /* 1. sources */
    const de_streaming_source *as = NULL;
    u32 an = de_asset_get_streaming_sources(p->assets, &as);
    p->src_count = 0;
    for (u32 i = 0; i < an && p->src_count < WP_MAX_SOURCES; ++i)
        p->src[p->src_count++] = as[i];
    de_query_each(p->world, p->q_src, wp_gather_chunk, p);
    f64 ratio = (f64)p->unload_r / (f64)p->load_r;

    /* 2-4. */
    u32 ncand = 0, resident = 0;
    for (u32 i = 0; i < p->cell_count; ++i) {
        wp_cell *c = &p->cells[i];
        if (c->state == DE_WORLD_CELL_LOADING) {
            de_load_state st = de_asset_status(p->assets, c->h, NULL);
            if (st == DE_LOAD_READY) {
                wp_instantiate(p, c);
            } else if (st != DE_LOAD_PENDING && st != DE_LOAD_LOADING) {
                DE_WARN(p->log, "world %s: cell (%d, %d) failed to load", p->name, (int)c->cx,
                        (int)c->cy);
                c->state = DE_WORLD_CELL_FAILED;
            }
        }
        f64 best = -1.0;
        bool in_load = false, in_unload = false;
        for (u32 s = 0; s < p->src_count; ++s) {
            const de_streaming_source *src = &p->src[s];
            f64 rl = src->radius > 0.0f ? (f64)src->radius : (f64)p->load_r;
            f64 ru = rl * ratio;
            f64 d2 = de__cell_dist2(c->cx, c->cy, src->x, src->y);
            if (d2 <= rl * rl)
                in_load = true;
            if (d2 <= ru * ru)
                in_unload = true;
            if (best < 0.0 || d2 < best)
                best = d2;
        }
        if (c->state != DE_WORLD_CELL_UNLOADED && !in_unload)
            wp_unload(p, c);
        if (c->state == DE_WORLD_CELL_UNLOADED && in_load)
            p->cand[ncand++] = (de__cell_key){best, c->cx, c->cy};
        else if (c->state == DE_WORLD_CELL_LOADED || c->state == DE_WORLD_CELL_LOADING)
            ++resident;
    }
    de__cell_keys_sort(p->cand, ncand);
    for (u32 i = 0; i < ncand && p->stats.loads_started < p->max_loads; ++i) {
        if (resident >= p->max_resident)
            break;
        wp_cell *c = (wp_cell *)wp_find(p, p->cand[i].cx, p->cand[i].cy);
        c->h = de_asset_load_bundle(p->assets, c->bundle, DE_LOAD_NO_EVENTS);
        if (!DE_HANDLE_VALID(c->h)) {
            c->state = DE_WORLD_CELL_FAILED;
            continue;
        }
        c->state = DE_WORLD_CELL_LOADING;
        p->stats.loads_started++;
        ++resident;
    }

    /* 5. counters and proxies */
    u32 loaded = 0, loading = 0;
    for (u32 i = 0; i < p->cell_count; ++i) {
        wp_cell *c = &p->cells[i];
        loaded += c->state == DE_WORLD_CELL_LOADED;
        loading += c->state == DE_WORLD_CELL_LOADING;
        de__far_proxy_step(&c->proxy, c->state == DE_WORLD_CELL_LOADED, dt, p->fade);
    }
    p->stats.cells_loaded = loaded;
    p->stats.cells_loading = loading;
    return DE_OK;
}

/* ---- Queries ------------------------------------------------------------------------------- */

de_world_partition_stats de_world_partition_get_stats(const de_world_partition *p) {
    de_world_partition_stats z = {0};
    return p != NULL ? p->stats : z;
}

de_world_cell_state de_world_partition_cell_state(const de_world_partition *p, i32 cx, i32 cy) {
    const wp_cell *c = p != NULL ? wp_find(p, cx, cy) : NULL;
    return c != NULL ? c->state : DE_WORLD_CELL_UNLOADED;
}

bool de_world_partition_proxy(const de_world_partition *p, i32 cx, i32 cy, bool *visible,
                              f32 *alpha) {
    const wp_cell *c = p != NULL && p->proxy_enabled ? wp_find(p, cx, cy) : NULL;
    if (visible != NULL)
        *visible = c != NULL && c->proxy.active;
    if (alpha != NULL)
        *alpha = c != NULL ? c->proxy.alpha : 0.0f;
    return c != NULL;
}
