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
 *  5. Far proxies step, and (DENG-830, with the render module) their entities follow them.
 *
 * DENG-831 (the editor's world.streaming, partition_editor.h): editor sources join the sources
 * after the entity ones; a cell forced to load is a candidate and never unloads whatever the
 * sources, a cell forced to unload never loads and unloads at the next update. A loaded cell
 * records its resident bytes (de_asset_status_list), its entity count and its content's z extent
 * when it is instantiated; a failed cell records why.
 *
 * DENG-830 (far proxies, with the render module): open requests the proxy bundle of each cell
 * that has one ("world/<name>/proxy_<cx>_<cy>", written by `de-cook world` when the world's proxy
 * is enabled). Once a proxy bundle is READY, the update spawns the proxy entity: a DeTransform at
 * the cell origin and a DeMeshRenderer of the mesh "<bundle>#proxy" (no shadows, not in ray
 * tracing). While the proxy is active it is drawn, with DeRenderFade { 1 - alpha } while it fades
 * out; when it is not active its DeMeshRenderer is removed. While a loaded cell cross-fades with
 * its proxy, each entity under the cell root that has a DeMeshRenderer gets DeRenderFade
 * { alpha, invert }, the complement of the proxy's mask, and loses it when the fade ends. A cell
 * with no proxy (or whose proxy has not loaded) is drawn whole at once.
 */
#include "partition_editor.h"
#include "streaming_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#if DE_HAS_RENDER
#include <dengine/de_render.h>
#endif

#include <stdlib.h>
#include <string.h>

#define WP_MAX_SOURCES (DE_ASSET_MAX_STREAMING_SOURCES + 48u + DE__WP_MAX_EDITOR_SOURCES)
#define WP_STATUS_MAX 1024u /* assets summed per cell bundle */
#define WP_ERROR_MAX 96
#define WP_TREE_DEPTH 32u
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
    de_load_handle proxy_h; /* DENG-830: the proxy bundle; invalid when the cell has none */
    de_entity proxy_e;      /* the proxy entity once its bundle is READY */
    bool proxy_failed;      /* the proxy bundle did not load (warned once) */
    bool cell_faded;        /* the cell's meshes carry DeRenderFade */
    de__wp_force force;
    u64 bytes;
    u32 entities;
    f64 zmin, zmax;
    char error[WP_ERROR_MAX]; /* "" unless FAILED */
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
    de__wp_source src_info[WP_MAX_SOURCES];
    u32 src_count;
    de_streaming_source ed_src[DE__WP_MAX_EDITOR_SOURCES];
    u32 ed_src_count;
    de_asset_status_entry *status; /* WP_STATUS_MAX */
    de_query q_src;
    de_component c_src, c_cell, c_pin, c_tf, c_gtf;
    f32 load_r, unload_r;
    u32 max_loads, max_resident;
    bool proxy_enabled;
    f32 fade;
    bool proxy_draw; /* DENG-830: the render components exist, so proxies are drawn */
    de_component c_mr, c_fade;
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

de_result de__wp_register_components(de_world *w, de_component *src, de_component *cell,
                                     de_component *pin) {
    static const de_field_info k_src[] = {
        DE_FIELD(DeStreamingSource, radius, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_cell[] = {
        DE_FIELD(DeWorldCell, cx, DE_FK_I32, DE_FIELD_SNAPSHOT),
        DE_FIELD(DeWorldCell, cy, DE_FK_I32, DE_FIELD_SNAPSHOT),
    };
    static const de_field_info k_pin[] = {
        DE_FIELD(DeWorldCellPin, cx, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(DeWorldCellPin, cy, DE_FK_I32, DE_FIELD_DEFAULT),
    };
    de_component c[3];
    DE_TRY(wp_component(w, DE_TYPE_DESC(DeStreamingSource, 1, k_src), &c[0]));
    DE_TRY(wp_component(w, DE_TYPE_DESC(DeWorldCell, 1, k_cell), &c[1]));
    DE_TRY(wp_component(w, DE_TYPE_DESC(DeWorldCellPin, 1, k_pin), &c[2]));
    if (src != NULL)
        *src = c[0];
    if (cell != NULL)
        *cell = c[1];
    if (pin != NULL)
        *pin = c[2];
    return DE_OK;
}

static de_result wp_register(de_world_partition *p) {
    DE_TRY(de__wp_register_components(p->world, &p->c_src, &p->c_cell, &p->c_pin));
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

/* ---- Far proxy entities (DENG-830) --------------------------------------------------------- */

#if DE_HAS_RENDER
/* Requests the proxy bundle of each cell that has one. */
static de_result wp_proxy_open(de_world_partition *p) {
    for (u32 i = 0; i < p->cell_count; ++i)
        p->cells[i].proxy_e = DE_ENTITY_NULL;
    if (!p->proxy_enabled || p->cell_count == 0)
        return DE_OK;
    DE_TRY(de_render_register_components(p->world));
    p->c_mr = de_component_find(p->world, "DeMeshRenderer");
    p->c_fade = de_component_find(p->world, "DeRenderFade");
    if (!DE_HANDLE_VALID(p->c_mr) || !DE_HANDLE_VALID(p->c_fade))
        return DE_FAIL(DE_ERR_INTERNAL, "world partition: the render components are missing");
    p->proxy_draw = true;
    u32 nb = de_asset_bundle_count(p->assets);
    for (u32 i = 0; i < nb; ++i) {
        const char *bn = de_asset_bundle_name(p->assets, i);
        i32 cx, cy;
        if (bn == NULL || !de__cell_proxy_parse(bn, p->name, &cx, &cy))
            continue;
        wp_cell *c = (wp_cell *)wp_find(p, cx, cy);
        if (c == NULL || DE_HANDLE_VALID(c->proxy_h))
            continue; /* a proxy without a cell is never drawn */
        c->proxy_h = de_asset_load_bundle(p->assets, bn, DE_LOAD_NO_EVENTS);
        if (!DE_HANDLE_VALID(c->proxy_h))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "world partition: cannot request proxy %s", bn);
    }
    return DE_OK;
}

static void wp_proxy_spawn(de_world_partition *p, wp_cell *c) {
    char name[WP_NAME_MAX + 48];
    if (!de__cell_proxy_name(name, sizeof name - 8, p->name, c->cx, c->cy))
        return;
    strcat(name, "#proxy");
    de_mesh mesh = de_asset_mesh(p->assets, de_asset_id_from(name));
    de_entity e = de_entity_spawn(p->world);
    bool added = de_entity_add(p->world, e, p->c_tf) != NULL &&
                 de_entity_add(p->world, e, p->c_mr) != NULL;
    de_transform *tf = added ? (de_transform *)de_entity_get(p->world, e, p->c_tf) : NULL;
    DeMeshRenderer *mr = added ? (DeMeshRenderer *)de_entity_get(p->world, e, p->c_mr) : NULL;
    if (tf == NULL || mr == NULL || !DE_HANDLE_VALID(mesh)) {
        de_entity_despawn(p->world, e);
        DE_WARN(p->log, "world %s: cannot spawn the far proxy of cell (%d, %d)", p->name,
                (int)c->cx, (int)c->cy);
        c->proxy_failed = true;
        return;
    }
    tf->position = de__cell_origin(c->cx, c->cy);
    tf->rotation.x = tf->rotation.y = tf->rotation.z = 0.0f;
    tf->rotation.w = 1.0f;
    tf->scale.x = tf->scale.y = tf->scale.z = 1.0f;
    mr->mesh = mesh;
    mr->layer_mask = ~0u;
    mr->cast_shadows = false;
    mr->visible_in_rt = false;
    c->proxy_e = e;
}

/* Sets (fade_out >= 0) or removes (fade_out < 0) DeRenderFade on `e`. */
static void wp_fade_set(de_world_partition *p, de_entity e, f32 fade_out, bool invert) {
    if (fade_out < 0.0f) {
        if (de_entity_has(p->world, e, p->c_fade))
            de_entity_remove(p->world, e, p->c_fade);
        return;
    }
    DeRenderFade *f = (DeRenderFade *)de_entity_get(p->world, e, p->c_fade);
    if (f == NULL)
        f = (DeRenderFade *)de_entity_add(p->world, e, p->c_fade);
    if (f != NULL) {
        f->fade_out = fade_out;
        f->invert = invert;
    }
}

/* The meshes under `e` (the cell root's tree). */
static void wp_fade_tree(de_world_partition *p, de_entity e, f32 fade_out, u32 depth) {
    for (de_entity ch = de_entity_children_first(p->world, e); ch.bits != 0;
         ch = de_entity_children_next(p->world, ch)) {
        if (de_entity_has(p->world, ch, p->c_mr))
            wp_fade_set(p, ch, fade_out, true);
        if (depth + 1 < WP_TREE_DEPTH)
            wp_fade_tree(p, ch, fade_out, depth + 1);
    }
}

static void wp_proxy_update(de_world_partition *p, wp_cell *c) {
    if (!DE_HANDLE_VALID(c->proxy_h) || c->proxy_failed)
        return;
    if (c->proxy_e.bits == 0) {
        de_load_state st = de_asset_status(p->assets, c->proxy_h, NULL);
        if (st == DE_LOAD_PENDING || st == DE_LOAD_LOADING)
            return;
        if (st != DE_LOAD_READY) {
            DE_WARN(p->log, "world %s: the far proxy of cell (%d, %d) failed to load", p->name,
                    (int)c->cx, (int)c->cy);
            c->proxy_failed = true;
            return;
        }
        wp_proxy_spawn(p, c);
        if (c->proxy_e.bits == 0)
            return;
    }
    if (!de_entity_alive(p->world, c->proxy_e)) {
        c->proxy_failed = true; /* despawned by someone else */
        c->proxy_e = DE_ENTITY_NULL;
        return;
    }
    const bool fading = c->state == DE_WORLD_CELL_LOADED && c->proxy.active;
    if (c->proxy.active) {
        if (!de_entity_has(p->world, c->proxy_e, p->c_mr)) {
            de_mesh mesh = {0};
            char name[WP_NAME_MAX + 48];
            if (de__cell_proxy_name(name, sizeof name - 8, p->name, c->cx, c->cy)) {
                strcat(name, "#proxy");
                mesh = de_asset_mesh(p->assets, de_asset_id_from(name));
            }
            DeMeshRenderer *mr = (DeMeshRenderer *)de_entity_add(p->world, c->proxy_e, p->c_mr);
            if (mr != NULL) {
                mr->mesh = mesh;
                mr->layer_mask = ~0u;
            }
        }
        wp_fade_set(p, c->proxy_e, fading ? 1.0f - c->proxy.alpha : -1.0f, false);
    } else {
        wp_fade_set(p, c->proxy_e, -1.0f, false);
        if (de_entity_has(p->world, c->proxy_e, p->c_mr))
            de_entity_remove(p->world, c->proxy_e, p->c_mr);
    }
    if (fading && de_entity_alive(p->world, c->root)) {
        wp_fade_tree(p, c->root, c->proxy.alpha, 0);
        c->cell_faded = true;
    } else if (c->cell_faded) {
        if (c->state == DE_WORLD_CELL_LOADED && de_entity_alive(p->world, c->root))
            wp_fade_tree(p, c->root, -1.0f, 0);
        c->cell_faded = false;
    }
}

/* Despawns the proxy entities (close) and releases the proxy bundles (close and abandon). */
static void wp_proxy_close(de_world_partition *p, bool despawn) {
    for (u32 i = 0; i < p->cell_count; ++i) {
        wp_cell *c = &p->cells[i];
        if (despawn && c->proxy_e.bits != 0 && de_entity_alive(p->world, c->proxy_e))
            de_entity_despawn(p->world, c->proxy_e);
        c->proxy_e = DE_ENTITY_NULL;
        if (DE_HANDLE_VALID(c->proxy_h))
            de_asset_release(p->assets, c->proxy_h);
        c->proxy_h = (de_load_handle){0};
    }
}
#else
static de_result wp_proxy_open(de_world_partition *p) {
    (void)p;
    return DE_OK;
}
static void wp_proxy_update(de_world_partition *p, wp_cell *c) {
    (void)p;
    (void)c;
}
static void wp_proxy_close(de_world_partition *p, bool despawn) {
    (void)p;
    (void)despawn;
}
#endif

/* ---- Open and close ------------------------------------------------------------------------ */

static void wp_free(de_world_partition *p) {
    const de_allocator *a = p->a;
    if (p->status != NULL)
        DE_FREE(a, p->status, sizeof *p->status * WP_STATUS_MAX);
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

    p->status = (de_asset_status_entry *)DE_ALLOC(a, sizeof *p->status * WP_STATUS_MAX,
                                                  DE_ALIGNOF(de_asset_status_entry), DE_MEM_ASSET);
    if (p->status == NULL) {
        wp_free(p);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_partition_open: out of memory");
    }
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
    r = wp_proxy_open(p);
    if (r != DE_OK) {
        de_world_partition_close(p);
        return r;
    }
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
    c->cell_faded = false;
    c->bytes = 0;
    c->error[0] = '\0';
}

void de_world_partition_close(de_world_partition *p) {
    if (p == NULL)
        return;
    for (u32 i = 0; i < p->cell_count; ++i)
        wp_unload(p, &p->cells[i]);
    wp_proxy_close(p, true);
    de_query_destroy(p->world, p->q_src);
    wp_free(p);
}

void de__wp_abandon(de_world_partition *p) {
    if (p == NULL)
        return;
    for (u32 i = 0; i < p->cell_count; ++i)
        if (DE_HANDLE_VALID(p->cells[i].h))
            de_asset_release(p->assets, p->cells[i].h);
    wp_proxy_close(p, false);
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
        de__wp_source *info = &p->src_info[p->src_count];
        de_streaming_source *o = &p->src[p->src_count++];
        o->x = x->position.x;
        o->y = x->position.y;
        o->z = x->position.z;
        o->radius = s[i].radius;
        info->kind = DE__WP_SOURCE_ENTITY;
        info->entity = ch->entities[i];
    }
}

/* Entities and z extent under `e`, whose parent sits at world z `z0`. Only translations are
 * composed (rotation and scale are left out): the extent is for the editor's cell boxes. */
static void wp_walk(de_world_partition *p, wp_cell *c, de_entity e, f64 z0, u32 depth) {
    for (de_entity ch = de_entity_children_first(p->world, e); ch.bits != 0;
         ch = de_entity_children_next(p->world, ch)) {
        const de_transform *tf = (const de_transform *)de_entity_get(p->world, ch, p->c_tf);
        const f64 z = z0 + (tf != NULL ? tf->position.z : 0.0);
        if (c->entities == 0 || z < c->zmin)
            c->zmin = z;
        if (c->entities == 0 || z > c->zmax)
            c->zmax = z;
        c->entities++;
        if (depth + 1 < WP_TREE_DEPTH)
            wp_walk(p, c, ch, z, depth + 1);
    }
}

static void wp_measure(de_world_partition *p, wp_cell *c) {
    c->entities = 0;
    c->zmin = c->zmax = 0.0;
    wp_walk(p, c, c->root, 0.0, 0);
    u32 n = de_asset_status_list(p->assets, c->h, p->status, WP_STATUS_MAX);
    n = n < WP_STATUS_MAX ? n : WP_STATUS_MAX;
    u64 bytes = 0;
    for (u32 i = 0; i < n; ++i)
        if (p->status[i].state == DE_LOAD_READY)
            bytes += p->status[i].bytes;
    c->bytes = bytes;
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
        snprintf(c->error, sizeof c->error, "cannot spawn the cell root");
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
        snprintf(c->error, sizeof c->error, "cannot instantiate the cell scene: %s",
                 de_result_str(r));
        c->state = DE_WORLD_CELL_FAILED;
        return;
    }
    c->root = root;
    c->state = DE_WORLD_CELL_LOADED;
    wp_measure(p, c);
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
    for (u32 i = 0; i < an && p->src_count < WP_MAX_SOURCES; ++i) {
        p->src_info[p->src_count].kind = DE__WP_SOURCE_GAME;
        p->src_info[p->src_count].entity = DE_ENTITY_NULL;
        p->src[p->src_count++] = as[i];
    }
    de_query_each(p->world, p->q_src, wp_gather_chunk, p);
    for (u32 i = 0; i < p->ed_src_count && p->src_count < WP_MAX_SOURCES; ++i) {
        p->src_info[p->src_count].kind = DE__WP_SOURCE_EDITOR;
        p->src_info[p->src_count].entity = DE_ENTITY_NULL;
        p->src[p->src_count++] = p->ed_src[i];
    }
    for (u32 i = 0; i < p->src_count; ++i) {
        p->src_info[i].s = p->src[i];
        if (!(p->src_info[i].s.radius > 0.0f))
            p->src_info[i].s.radius = p->load_r;
    }
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
                de_load_info li;
                memset(&li, 0, sizeof li);
                (void)de_asset_status(p->assets, c->h, &li);
                DE_WARN(p->log, "world %s: cell (%d, %d) failed to load", p->name, (int)c->cx,
                        (int)c->cy);
                snprintf(c->error, sizeof c->error, "%s",
                         li.first_error_msg[0] != '\0' ? li.first_error_msg
                                                       : "the cell bundle failed to load");
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
        if (c->force == DE__WP_FORCE_LOAD) {
            in_load = in_unload = true;
            if (best < 0.0)
                best = 0.0;
        } else if (c->force == DE__WP_FORCE_UNLOAD) {
            in_load = in_unload = false;
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
            snprintf(c->error, sizeof c->error, "the cell bundle could not be requested");
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
        if (p->proxy_draw)
            wp_proxy_update(p, c);
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

/* ---- Editor (DENG-831, partition_editor.h) ------------------------------------------------- */

u32 de__wp_cell_count(const de_world_partition *p) {
    return p != NULL ? p->cell_count : 0;
}

bool de__wp_cell_index(const de_world_partition *p, i32 cx, i32 cy, u32 *out) {
    const wp_cell *c = p != NULL ? wp_find(p, cx, cy) : NULL;
    if (c != NULL && out != NULL)
        *out = (u32)(c - p->cells);
    return c != NULL;
}

bool de__wp_cell_info_at(const de_world_partition *p, u32 i, de__wp_cell_info *out) {
    if (p == NULL || i >= p->cell_count || out == NULL)
        return false;
    const wp_cell *c = &p->cells[i];
    memset(out, 0, sizeof *out);
    out->cx = c->cx;
    out->cy = c->cy;
    out->state = c->state;
    out->force = c->force;
    out->bytes = c->state == DE_WORLD_CELL_LOADED ? c->bytes : 0;
    out->entities = c->entities;
    out->zmin = c->zmin;
    out->zmax = c->zmax;
    out->proxy_visible = p->proxy_enabled && c->proxy.active;
    out->proxy_alpha = p->proxy_enabled ? c->proxy.alpha : 0.0f;
    out->error = c->state == DE_WORLD_CELL_FAILED && c->error[0] != '\0' ? c->error : NULL;
    return true;
}

void de__wp_set_force(de_world_partition *p, u32 i, de__wp_force f) {
    if (p != NULL && i < p->cell_count)
        p->cells[i].force = f;
}

void de__wp_clear_forces(de_world_partition *p) {
    for (u32 i = 0; p != NULL && i < p->cell_count; ++i)
        p->cells[i].force = DE__WP_FORCE_NONE;
}

u32 de__wp_forced_loads(const de_world_partition *p) {
    u32 n = 0;
    for (u32 i = 0; p != NULL && i < p->cell_count; ++i)
        n += p->cells[i].force == DE__WP_FORCE_LOAD;
    return n;
}

void de__wp_set_editor_sources(de_world_partition *p, const de_streaming_source *s, u32 n) {
    if (p == NULL)
        return;
    n = n < DE__WP_MAX_EDITOR_SOURCES ? n : DE__WP_MAX_EDITOR_SOURCES;
    for (u32 i = 0; i < n; ++i)
        p->ed_src[i] = s[i];
    p->ed_src_count = s != NULL ? n : 0;
}

u32 de__wp_sources(const de_world_partition *p, const de__wp_source **out) {
    if (out != NULL)
        *out = p != NULL ? p->src_info : NULL;
    return p != NULL ? p->src_count : 0;
}

u32 de__wp_max_resident(const de_world_partition *p) {
    return p != NULL ? p->max_resident : 0;
}

u32 de__wp_max_loads(const de_world_partition *p) {
    return p != NULL ? p->max_loads : 0;
}

f32 de__wp_load_radius(const de_world_partition *p) {
    return p != NULL ? p->load_r : 0.0f;
}

f32 de__wp_unload_radius(const de_world_partition *p) {
    return p != NULL ? p->unload_r : 0.0f;
}

void de__wp_extra_stats(const de_world_partition *p, u32 *cells_failed, u64 *bytes_resident) {
    u32 failed = 0;
    u64 bytes = 0;
    for (u32 i = 0; p != NULL && i < p->cell_count; ++i) {
        failed += p->cells[i].state == DE_WORLD_CELL_FAILED;
        if (p->cells[i].state == DE_WORLD_CELL_LOADED)
            bytes += p->cells[i].bytes;
    }
    if (cells_failed != NULL)
        *cells_failed = failed;
    if (bytes_resident != NULL)
        *bytes_resident = bytes;
}

bool de__wp_cell_distance(const de_world_partition *p, u32 i, f64 *out) {
    if (p == NULL || i >= p->cell_count || p->src_count == 0)
        return false;
    f64 best = -1.0;
    for (u32 s = 0; s < p->src_count; ++s) {
        f64 d2 = de__cell_dist2(p->cells[i].cx, p->cells[i].cy, p->src[s].x, p->src[s].y);
        if (best < 0.0 || d2 < best)
            best = d2;
    }
    if (out != NULL)
        *out = sqrt(best);
    return true;
}

static int wp_preview_cmp(const void *a, const void *b) {
    const de__wp_preview_cell *x = (const de__wp_preview_cell *)a;
    const de__wp_preview_cell *y = (const de__wp_preview_cell *)b;
    if (x->distance != y->distance)
        return x->distance < y->distance ? -1 : 1;
    if (x->cx != y->cx)
        return x->cx < y->cx ? -1 : 1;
    if (x->cy != y->cy)
        return x->cy < y->cy ? -1 : 1;
    return 0;
}

void de__wp_preview_run(const de_world_partition *p, const de_streaming_source *src, u32 n,
                        bool ignore_forces, de__wp_preview *out) {
    out->load_count = out->keep_count = out->unload_count = 0;
    out->capped = false;
    if (p == NULL)
        return;
    const f64 ratio = (f64)p->unload_r / (f64)p->load_r;
    for (u32 i = 0; i < p->cell_count; ++i) {
        const wp_cell *c = &p->cells[i];
        f64 best = -1.0;
        bool load = false, stay = false;
        for (u32 s = 0; s < n; ++s) {
            const f64 rl = src[s].radius > 0.0f ? (f64)src[s].radius : (f64)p->load_r;
            const f64 ru = rl * ratio;
            const f64 d2 = de__cell_dist2(c->cx, c->cy, src[s].x, src[s].y);
            load = load || d2 <= rl * rl;
            stay = stay || d2 <= ru * ru;
            if (best < 0.0 || d2 < best)
                best = d2;
        }
        const de__wp_force f = ignore_forces ? DE__WP_FORCE_NONE : c->force;
        if (f == DE__WP_FORCE_LOAD)
            load = stay = true;
        else if (f == DE__WP_FORCE_UNLOAD)
            load = stay = false;
        const de__wp_preview_cell pc = {c->cx, c->cy, best < 0.0 ? 0.0 : sqrt(best)};
        switch (c->state) {
        case DE_WORLD_CELL_LOADED:
        case DE_WORLD_CELL_LOADING:
            if (stay)
                out->keep[out->keep_count++] = pc;
            else
                out->unloads[out->unload_count++] = pc;
            break;
        case DE_WORLD_CELL_FAILED:
            if (!stay)
                out->unloads[out->unload_count++] = pc;
            break;
        default:
            if (load)
                out->loads[out->load_count++] = pc;
            break;
        }
    }
    qsort(out->loads, out->load_count, sizeof *out->loads, wp_preview_cmp);
    const u32 room = p->max_resident > out->keep_count ? p->max_resident - out->keep_count : 0;
    if (out->load_count > room) {
        out->capped = true;
        out->load_count = room;
    }
}
