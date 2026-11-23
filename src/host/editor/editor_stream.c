/* editor_stream.c: world.streaming (DENG-831; DIDE protocol 1.26, DIDE-127, DIDE-R173).
 *
 * Methods stream.info, stream.open, stream.cells, stream.force, stream.setEditorSources,
 * stream.preview, stream.assignments, stream.regenerateProxy, stream.cancelProxy and
 * stream.overlay, and the topics stream.cells and stream.proxy. The service owns one world
 * partition of the engine's world (de_world_partition_open), opened by stream.open in Edit mode
 * and updated every frame in Edit and Simulate; in Play and Paused it is left as it is (the game
 * opens its own). The partition's editor extensions are in src/world/streaming/partition_editor.h.
 *
 * Axes: the engine is Z-up, so the cell grid spans x and y (axes [0, 1]) and a cell's bounds take
 * the content's extent along z, measured when the cell loads (translations only).
 *
 * Session state: the editor sources and the forces belong to the IDE session. A disconnect drops
 * both; stream.open drops the forces (a new partition has none). The editor sources and the
 * overlay outlive stream.open.
 *
 * Limits: proxyBake is false (proxy meshes are DENG-830), so stream.regenerateProxy answers
 * unsupported after its other checks, no proxy job ever runs, cells report proxyRevision 0 and
 * the topic stream.proxy is never sent. A cell's entity count and z extent are 0 until it loads
 * once. If the engine swaps its ECS world, the partition is dropped without touching the old one.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"

#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"
#include "ecs/change_internal.h"
#include "world/streaming/partition_editor.h"
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define ST_CAP           "world.streaming"
#define ST_NAME_MAX      64u
#define ST_MAX_SOURCES   80u   /* every source of an update (16 game + 48 entity + 8 editor) */
#define ST_MAX_PREVIEW   64u   /* sources stream.preview takes */
#define ST_MAX_ORDER     4096u /* overlay loadOrder cells kept */
#define ST_DRAW_CELLS    4096u /* cells the overlay draws */
#define ST_SOURCE_NAME   "DeStreamingSource"
#define ST_CELL_NAME     "DeWorldCell"
#define ST_PIN_NAME      "DeWorldCellPin"
#define ST_TREE_DEPTH    256u

typedef struct st_cell_out {
    de_ep_stream_cell cell;
    f64 bounds[6];
} st_cell_out;

typedef struct st_state {
    de_world *world;       /* the world the partition streams into (the engine's) */
    de_asset_system *assets;
    de_world_partition *p; /* NULL: no world is open */
    char open[ST_NAME_MAX];
    u64 revision;
    u64 last_ns;

    /* Per cell, sized at open: the last state sent on stream.cells, and scratch. */
    u32 cell_count;
    de__wp_cell_info *sent;
    bool *sent_dist;
    f64 *sent_d;
    st_cell_out *out;
    de_ep_stream_cell *list;
    de__wp_preview_cell *pv; /* 3 * cell_count */
    de_ep_stream_preview_cell *pv_loads;
    de_ep_stream_cell_key *keys; /* 2 * cell_count */
    u8 *mark;

    de_streaming_source ed_src[DE__WP_MAX_EDITOR_SOURCES];
    u32 ed_src_count;

    de_ep_stream_overlay_params ov;
    de_ep_stream_cell_key ov_order[ST_MAX_ORDER];
    de_draw_category cat;
} st_state;

static st_state g_st;
static de_ep_stream_source g_src[ST_MAX_SOURCES];
static f64 g_src_pos[ST_MAX_SOURCES * 3];
static de_ep_stream_source_spec g_spec[DE__WP_MAX_EDITOR_SOURCES];
static f64 g_spec_pos[DE__WP_MAX_EDITOR_SOURCES * 3];

static const de_allocator *heap(void) {
    return de_heap_allocator();
}

static de_world *world_of(de__editor *ed) {
    return ed->e != NULL ? ed->e->world : NULL;
}

/* ---------- Scratch ---------- */

#define ST_FREE(ptr, n)                                                                            \
    do {                                                                                           \
        if ((ptr) != NULL)                                                                         \
            DE_FREE(heap(), (ptr), sizeof *(ptr) * (size_t)(n));                                   \
        (ptr) = NULL;                                                                              \
    } while (0)

static void scratch_free(void) {
    const u32 n = g_st.cell_count;
    ST_FREE(g_st.sent, n);
    ST_FREE(g_st.sent_dist, n);
    ST_FREE(g_st.sent_d, n);
    ST_FREE(g_st.out, n);
    ST_FREE(g_st.list, n);
    ST_FREE(g_st.pv, 3u * n);
    ST_FREE(g_st.pv_loads, n);
    ST_FREE(g_st.keys, 2u * n);
    ST_FREE(g_st.mark, n);
    g_st.cell_count = 0;
}

#define ST_ALLOC(ptr, n)                                                                           \
    ((ptr) = DE_ALLOC(heap(), sizeof *(ptr) * (size_t)(n), DE_ALIGNOF(f64), DE_MEM_GENERAL),       \
     (ptr) != NULL)

static bool scratch_alloc(u32 n) {
    const u32 m = n > 0 ? n : 1u;
    g_st.cell_count = m;
    if (!ST_ALLOC(g_st.sent, m) || !ST_ALLOC(g_st.sent_dist, m) || !ST_ALLOC(g_st.sent_d, m) ||
        !ST_ALLOC(g_st.out, m) || !ST_ALLOC(g_st.list, m) || !ST_ALLOC(g_st.pv, 3u * m) ||
        !ST_ALLOC(g_st.pv_loads, m) || !ST_ALLOC(g_st.keys, 2u * m) || !ST_ALLOC(g_st.mark, m)) {
        scratch_free();
        return false;
    }
    memset(g_st.sent, 0, sizeof *g_st.sent * m);
    memset(g_st.sent_dist, 0, sizeof *g_st.sent_dist * m);
    memset(g_st.sent_d, 0, sizeof *g_st.sent_d * m);
    memset(g_st.mark, 0, m);
    return true;
}

/* ---------- The partition ---------- */

static void close_partition(bool world_alive) {
    if (g_st.p != NULL) {
        if (world_alive)
            de_world_partition_close(g_st.p);
        else
            de__wp_abandon(g_st.p);
    }
    g_st.p = NULL;
    g_st.open[0] = '\0';
    scratch_free();
}

static void sync_world(de__editor *ed) {
    de_world *w = world_of(ed);
    de_asset_system *as = ed->e != NULL ? ed->e->assets : NULL;
    if (w == g_st.world && as == g_st.assets)
        return;
    if (g_st.p != NULL) {
        close_partition(false); /* the old world may be gone: touch nothing in it */
        g_st.revision++;
    }
    g_st.world = w;
    g_st.assets = as;
    if (w != NULL)
        (void)de__wp_register_components(w, NULL, NULL, NULL);
}

static bool is_edit(const de__editor *ed) {
    return ed->mode == DE_EP_WORLD_MODE_EDIT;
}

/* ---------- Values ---------- */

static bool source_ok(const de_ep_stream_source_spec *s) {
    return s->position.count == 3 && isfinite(s->position.items[0]) &&
           isfinite(s->position.items[1]) && isfinite(s->position.items[2]) &&
           isfinite(s->radius) && s->radius >= 0.0f;
}

static de_streaming_source source_of(const de_ep_stream_source_spec *s) {
    de_streaming_source o;
    o.x = s->position.items[0];
    o.y = s->position.items[1];
    o.z = s->position.items[2];
    o.radius = s->radius;
    return o;
}

static de_ep_stream_cell_state state_of(de_world_cell_state s) {
    switch (s) {
    case DE_WORLD_CELL_LOADING:
        return DE_EP_STREAM_CELL_STATE_LOADING;
    case DE_WORLD_CELL_LOADED:
        return DE_EP_STREAM_CELL_STATE_LOADED;
    case DE_WORLD_CELL_FAILED:
        return DE_EP_STREAM_CELL_STATE_FAILED;
    default:
        return DE_EP_STREAM_CELL_STATE_UNLOADED;
    }
}

static de_ep_stream_force force_of(de__wp_force f) {
    return f == DE__WP_FORCE_LOAD     ? DE_EP_STREAM_FORCE_LOAD
           : f == DE__WP_FORCE_UNLOAD ? DE_EP_STREAM_FORCE_UNLOAD
                                      : DE_EP_STREAM_FORCE_NONE;
}

static de__wp_force wp_force_of(de_ep_stream_force f) {
    return f == DE_EP_STREAM_FORCE_LOAD     ? DE__WP_FORCE_LOAD
           : f == DE_EP_STREAM_FORCE_UNLOAD ? DE__WP_FORCE_UNLOAD
                                            : DE__WP_FORCE_NONE;
}

/* Cell `i` into g_st.out[i] (its bounds point into the same slot). */
static de_ep_stream_cell *fill_cell(u32 i, const de__wp_cell_info *c) {
    st_cell_out *o = &g_st.out[i];
    memset(&o->cell, 0, sizeof o->cell);
    const f64 x0 = (f64)c->cx * 256.0, y0 = (f64)c->cy * 256.0;
    o->bounds[0] = x0;
    o->bounds[1] = y0;
    o->bounds[2] = c->zmin;
    o->bounds[3] = x0 + 256.0;
    o->bounds[4] = y0 + 256.0;
    o->bounds[5] = c->zmax;
    de_ep_stream_cell *e = &o->cell;
    e->cx = c->cx;
    e->cy = c->cy;
    e->state = state_of(c->state);
    e->force = force_of(c->force);
    e->bytes = c->bytes;
    e->entities = c->entities;
    e->bounds.min.items = &o->bounds[0];
    e->bounds.min.count = 3;
    e->bounds.max.items = &o->bounds[3];
    e->bounds.max.count = 3;
    f64 d = 0.0;
    e->has_distance = de__wp_cell_distance(g_st.p, i, &d);
    e->distance = d;
    e->proxy_visible = c->proxy_visible;
    e->proxy_alpha = c->proxy_alpha;
    e->proxy_revision = 0; /* no proxy bake yet (DENG-830) */
    e->has_error = c->error != NULL;
    if (c->error != NULL)
        e->error = de_ep_cstr(c->error);
    return e;
}

static void fill_stats(de_ep_stream_stats *o) {
    const de_world_partition_stats s = de_world_partition_get_stats(g_st.p);
    memset(o, 0, sizeof *o);
    o->cell_count = s.cell_count;
    o->cells_loaded = s.cells_loaded;
    o->cells_loading = s.cells_loading;
    o->loads_started = s.loads_started;
    o->unloads = s.unloads;
    o->total_loads = s.total_loads;
    o->total_unloads = s.total_unloads;
    de__wp_extra_stats(g_st.p, &o->cells_failed, &o->bytes_resident);
}

static void fill_sources(de_ep_list_stream_source *out) {
    const de__wp_source *s = NULL;
    u32 n = de__wp_sources(g_st.p, &s);
    n = n < ST_MAX_SOURCES ? n : ST_MAX_SOURCES;
    for (u32 i = 0; i < n; ++i) {
        de_ep_stream_source *o = &g_src[i];
        memset(o, 0, sizeof *o);
        o->kind = s[i].kind == DE__WP_SOURCE_ENTITY   ? DE_EP_STREAM_SOURCE_KIND_ENTITY
                  : s[i].kind == DE__WP_SOURCE_EDITOR ? DE_EP_STREAM_SOURCE_KIND_EDITOR
                                                      : DE_EP_STREAM_SOURCE_KIND_GAME;
        o->has_entity = s[i].kind == DE__WP_SOURCE_ENTITY;
        o->entity = o->has_entity ? s[i].entity.bits : 0;
        g_src_pos[i * 3 + 0] = s[i].s.x;
        g_src_pos[i * 3 + 1] = s[i].s.y;
        g_src_pos[i * 3 + 2] = s[i].s.z;
        o->position.items = &g_src_pos[i * 3];
        o->position.count = 3;
        o->radius = s[i].s.radius;
    }
    out->items = g_src;
    out->count = n;
}

/* stream.cells (all cells, or a region) into `o`. */
static void fill_cells(const de_ep_stream_cell_rect *region, de_ep_stream_cells *o) {
    memset(o, 0, sizeof *o);
    o->world = de_ep_cstr(g_st.open);
    o->revision = g_st.revision;
    u32 n = 0;
    const u32 count = de__wp_cell_count(g_st.p);
    for (u32 i = 0; i < count && i < g_st.cell_count; ++i) {
        de__wp_cell_info c;
        if (!de__wp_cell_info_at(g_st.p, i, &c))
            continue;
        if (region != NULL && (c.cx < region->min_x || c.cx > region->max_x ||
                               c.cy < region->min_y || c.cy > region->max_y))
            continue;
        g_st.list[n++] = *fill_cell(i, &c);
    }
    o->cells.items = g_st.list;
    o->cells.count = n;
    fill_sources(&o->sources);
    fill_stats(&o->stats);
}

static void fill_info(de__editor *ed, de_ep_arena *a, de_ep_stream_info *o, bool *fits) {
    static u32 axes[2] = {0, 1};
    memset(o, 0, sizeof *o);
    *fits = true;
    o->revision = g_st.revision;
    const de__manifest *m = ed->e != NULL && ed->e->assets != NULL ? ed->e->assets->manifest
                                                                   : NULL;
    const u32 wn = m != NULL ? m->world_count : 0;
    de_ep_stream_world *ws = wn > 0 ? de__ed_alloc(a, sizeof *ws * wn) : NULL;
    if (wn > 0 && ws == NULL) {
        *fits = false;
        return;
    }
    u32 k = 0;
    for (u32 i = 0; i < wn; ++i) {
        de_asset_world_info wi;
        if (de_asset_world_info_get(ed->e->assets, m->worlds[i].name, &wi) != DE_OK)
            continue;
        de_ep_stream_world *w = &ws[k++];
        memset(w, 0, sizeof *w);
        w->name = de_ep_cstr(wi.name);
        w->cells_dir = de_ep_cstr(wi.cells_dir != NULL ? wi.cells_dir : "");
        w->cell_size = wi.cell_size;
        w->load_radius = wi.load_radius;
        w->unload_radius = wi.unload_radius;
        w->max_loads_per_frame = wi.max_loads_per_frame;
        w->scene_count = wi.scene_count;
        w->proxy_enabled = wi.proxy_enabled;
        w->proxy_triangle_ratio = wi.proxy_triangle_ratio;
        w->proxy_fade_seconds = wi.proxy_fade_seconds;
    }
    o->worlds.items = ws;
    o->worlds.count = k;
    o->has_open = g_st.p != NULL;
    if (o->has_open)
        o->open = de_ep_cstr(g_st.open);
    o->axes.items = axes;
    o->axes.count = 2;
    o->max_editor_sources = DE__WP_MAX_EDITOR_SOURCES;
    o->max_resident_cells = g_st.p != NULL ? de__wp_max_resident(g_st.p) : 256u;
    o->source_component = de_ep_cstr(ST_SOURCE_NAME);
    o->cell_component = de_ep_cstr(ST_CELL_NAME);
    o->pin_component = de_ep_cstr(ST_PIN_NAME);
    o->proxy_bake = false; /* DENG-830 */
    for (u32 i = 0; i < g_st.ed_src_count; ++i) {
        g_spec_pos[i * 3 + 0] = g_st.ed_src[i].x;
        g_spec_pos[i * 3 + 1] = g_st.ed_src[i].y;
        g_spec_pos[i * 3 + 2] = g_st.ed_src[i].z;
        g_spec[i].position.items = &g_spec_pos[i * 3];
        g_spec[i].position.count = 3;
        g_spec[i].radius = g_st.ed_src[i].radius;
    }
    o->editor_sources.items = g_spec;
    o->editor_sources.count = g_st.ed_src_count;
    o->overlay = g_st.ov;
    o->has_stats = g_st.p != NULL;
    if (o->has_stats)
        fill_stats(&o->stats);
}

static bool require_open(const de_ep_request *req, mpack_writer_t *w, const char *m) {
    if (g_st.p != NULL)
        return true;
    de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "%s: no world is open (stream.open)", m);
    return false;
}

static bool cell_index(i32 cx, i32 cy, u32 *i) {
    return de__wp_cell_index(g_st.p, cx, cy, i) && *i < g_st.cell_count;
}

/* ---------- stream.info and stream.open ---------- */

static void on_info(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    sync_world(ed);
    de_ep_stream_info o;
    bool fits;
    fill_info(ed, a, &o, &fits);
    if (!fits) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "stream.info: the response does not fit");
        return;
    }
    de_ep_write_response_stream_info(w, req->id, &o);
}

static void on_open(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(stream_open, de_ep_stream_open_params, q);
    sync_world(ed);
    if (!is_edit(ed)) {
        de__ed_error(w, req->id, DE_EP_ERR_WRONG_MODE,
                     "stream.open: Edit mode only (in Play the game opens its own partition)");
        return;
    }
    char name[ST_NAME_MAX];
    if (q.has_world) {
        if (q.world.len == 0 || q.world.len >= ST_NAME_MAX) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "stream.open: no world %.*s",
                         (int)q.world.len, q.world.ptr);
            return;
        }
        memcpy(name, q.world.ptr, q.world.len);
        name[q.world.len] = '\0';
        de_asset_world_info wi;
        if (g_st.assets == NULL || g_st.world == NULL ||
            de_asset_world_info_get(g_st.assets, name, &wi) != DE_OK) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "stream.open: no world %s", name);
            return;
        }
    }
    close_partition(true);
    g_st.revision++;
    if (q.has_world) {
        de_world_partition *p = NULL;
        const de_result r = de_world_partition_open(g_st.assets, g_st.world, name, NULL, &p);
        if (r != DE_OK) {
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "stream.open: cannot open world %s: %s",
                         name, de_result_str(r));
            return;
        }
        if (!scratch_alloc(de__wp_cell_count(p))) {
            de_world_partition_close(p);
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "stream.open: out of memory");
            return;
        }
        g_st.p = p;
        snprintf(g_st.open, sizeof g_st.open, "%s", name);
        de__wp_set_editor_sources(p, g_st.ed_src, g_st.ed_src_count);
        g_st.last_ns = de_time_now_ns();
    }
    de_ep_stream_info o;
    bool fits;
    fill_info(ed, a, &o, &fits);
    if (!fits) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "stream.open: the response does not fit");
        return;
    }
    de_ep_write_response_stream_open(w, req->id, &o);
}

/* ---------- stream.cells and stream.force ---------- */

static void on_cells(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                     mpack_writer_t *w) {
    DE__ED_PARAMS(stream_cells, de_ep_stream_cells_params, q);
    sync_world(ed);
    if (!require_open(req, w, "stream.cells"))
        return;
    de_ep_stream_cells o;
    fill_cells(q.has_region ? &q.region : NULL, &o);
    de_ep_write_response_stream_cells(w, req->id, &o);
}

static void on_force(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                     mpack_writer_t *w) {
    DE__ED_PARAMS(stream_force, de_ep_stream_force_params, q);
    sync_world(ed);
    if (!is_edit(ed)) {
        de__ed_error(w, req->id, DE_EP_ERR_WRONG_MODE, "stream.force: Edit mode only");
        return;
    }
    if (!require_open(req, w, "stream.force"))
        return;
    for (u32 k = 0; k < q.cells.count; ++k) {
        u32 i;
        if (!cell_index(q.cells.items[k].cx, q.cells.items[k].cy, &i)) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "stream.force: no cell (%d, %d)",
                         (int)q.cells.items[k].cx, (int)q.cells.items[k].cy);
            return;
        }
    }
    const de__wp_force f = wp_force_of(q.force);
    if (f == DE__WP_FORCE_LOAD) {
        u32 loads = de__wp_forced_loads(g_st.p);
        memset(g_st.mark, 0, g_st.cell_count);
        for (u32 k = 0; k < q.cells.count; ++k) {
            u32 i;
            de__wp_cell_info c;
            (void)cell_index(q.cells.items[k].cx, q.cells.items[k].cy, &i);
            if (!g_st.mark[i] && de__wp_cell_info_at(g_st.p, i, &c) &&
                c.force != DE__WP_FORCE_LOAD)
                ++loads;
            g_st.mark[i] = 1;
        }
        if (loads > de__wp_max_resident(g_st.p)) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "stream.force: %u forced loads pass maxResidentCells (%u)", loads,
                         de__wp_max_resident(g_st.p));
            return;
        }
    }
    bool changed = false;
    for (u32 k = 0; k < q.cells.count; ++k) {
        u32 i;
        de__wp_cell_info c;
        if (cell_index(q.cells.items[k].cx, q.cells.items[k].cy, &i) &&
            de__wp_cell_info_at(g_st.p, i, &c) && c.force != f) {
            de__wp_set_force(g_st.p, i, f);
            changed = true;
        }
    }
    if (changed)
        g_st.revision++;
    de_ep_stream_cells o;
    fill_cells(NULL, &o);
    de_ep_write_response_stream_force(w, req->id, &o);
}

/* ---------- stream.setEditorSources and stream.preview ---------- */

static void on_set_sources(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(stream_set_editor_sources, de_ep_stream_editor_sources_params, q);
    sync_world(ed);
    if (ed->mode == DE_EP_WORLD_MODE_PLAY) {
        de__ed_error(w, req->id, DE_EP_ERR_WRONG_MODE, "stream.setEditorSources: not in Play");
        return;
    }
    if (q.sources.count > DE__WP_MAX_EDITOR_SOURCES) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "stream.setEditorSources: %u sources pass maxEditorSources (%u)",
                     q.sources.count, DE__WP_MAX_EDITOR_SOURCES);
        return;
    }
    for (u32 i = 0; i < q.sources.count; ++i)
        if (!source_ok(&q.sources.items[i])) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "stream.setEditorSources: a source is [x, y, z] of finite numbers with "
                         "a radius of 0 or more");
            return;
        }
    bool same = q.sources.count == g_st.ed_src_count;
    for (u32 i = 0; i < q.sources.count; ++i) {
        const de_streaming_source s = source_of(&q.sources.items[i]);
        same = same && memcmp(&s, &g_st.ed_src[i], sizeof s) == 0;
        g_st.ed_src[i] = s;
    }
    g_st.ed_src_count = q.sources.count;
    de__wp_set_editor_sources(g_st.p, g_st.ed_src, g_st.ed_src_count);
    if (!same)
        g_st.revision++;
    de_ep_empty o;
    memset(&o, 0, sizeof o);
    de_ep_write_response_stream_set_editor_sources(w, req->id, &o);
}

static void on_preview(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    DE__ED_PARAMS(stream_preview, de_ep_stream_preview_params, q);
    sync_world(ed);
    if (!require_open(req, w, "stream.preview"))
        return;
    if (q.sources.count > ST_MAX_PREVIEW) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "stream.preview: at most %u sources", ST_MAX_PREVIEW);
        return;
    }
    de_streaming_source src[ST_MAX_PREVIEW];
    for (u32 i = 0; i < q.sources.count; ++i) {
        if (!source_ok(&q.sources.items[i])) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "stream.preview: a source is [x, y, z] of finite numbers with a radius "
                         "of 0 or more");
            return;
        }
        src[i] = source_of(&q.sources.items[i]);
    }
    const u32 n = g_st.cell_count;
    de__wp_preview pv;
    memset(&pv, 0, sizeof pv);
    pv.loads = &g_st.pv[0];
    pv.keep = &g_st.pv[n];
    pv.unloads = &g_st.pv[2u * n];
    de__wp_preview_run(g_st.p, src, q.sources.count, q.has_ignore_forces && q.ignore_forces,
                       &pv);
    const u32 per = de__wp_max_loads(g_st.p) > 0 ? de__wp_max_loads(g_st.p) : 1u;
    for (u32 i = 0; i < pv.load_count; ++i) {
        de_ep_stream_preview_cell *c = &g_st.pv_loads[i];
        c->cx = pv.loads[i].cx;
        c->cy = pv.loads[i].cy;
        c->order = i + 1;
        c->frame = i / per + 1;
        c->distance = pv.loads[i].distance;
    }
    de_ep_stream_cell_key *keep = &g_st.keys[0], *unl = &g_st.keys[n];
    for (u32 i = 0; i < pv.keep_count; ++i) {
        keep[i].cx = pv.keep[i].cx;
        keep[i].cy = pv.keep[i].cy;
    }
    for (u32 i = 0; i < pv.unload_count; ++i) {
        unl[i].cx = pv.unloads[i].cx;
        unl[i].cy = pv.unloads[i].cy;
    }
    de_ep_stream_preview o;
    memset(&o, 0, sizeof o);
    o.loads.items = g_st.pv_loads;
    o.loads.count = pv.load_count;
    o.keep.items = keep;
    o.keep.count = pv.keep_count;
    o.unloads.items = unl;
    o.unloads.count = pv.unload_count;
    o.capped = pv.capped;
    de_ep_write_response_stream_preview(w, req->id, &o);
}

/* ---------- stream.assignments ---------- */

typedef struct st_assign {
    de_world *w;
    de_component tf, gtf, pin;
    de_ep_stream_assignment *items; /* NULL: count only */
    u32 n, cap;
} st_assign;

/* Fills `o` for entity `e`; false when neither a pin nor a position places its root. */
static bool assign_one(st_assign *s, de_entity e, de_ep_stream_assignment *o) {
    de_entity root = e;
    for (u32 d = 0; d < ST_TREE_DEPTH; ++d) {
        const de_entity up = de_entity_parent(s->w, root);
        if (up.bits == 0)
            break;
        root = up;
    }
    const de_transform *t = NULL;
    if (DE_HANDLE_VALID(s->gtf))
        t = (const de_transform *)de_entity_get(s->w, root, s->gtf);
    if (t == NULL && DE_HANDLE_VALID(s->tf))
        t = (const de_transform *)de_entity_get(s->w, root, s->tf);
    const DeWorldCellPin *pin =
        DE_HANDLE_VALID(s->pin) ? (const DeWorldCellPin *)de_entity_get(s->w, root, s->pin) : NULL;
    if (t == NULL && pin == NULL)
        return false;
    memset(o, 0, sizeof *o);
    o->entity = e.bits;
    o->root = root.bits;
    i32 ax = 0, ay = 0;
    if (t != NULL)
        de_world_cell_of(t->position.x, t->position.y, &ax, &ay);
    o->auto_.cx = ax;
    o->auto_.cy = ay;
    o->pinned = pin != NULL;
    o->cx = pin != NULL ? pin->cx : ax;
    o->cy = pin != NULL ? pin->cy : ay;
    return true;
}

static void assign_chunk(const de_query_chunk *ch, void *user) {
    st_assign *s = (st_assign *)user;
    for (u32 i = 0; i < ch->count; ++i) {
        u64 scene, sid;
        const de_entity e = ch->entities[i];
        if (de__ed_views_owns(e.bits) || !de__ed_scene_member(e.bits, &scene, &sid))
            continue;
        de_ep_stream_assignment tmp;
        if (!assign_one(s, e, s->items != NULL && s->n < s->cap ? &s->items[s->n] : &tmp))
            continue;
        if (s->items == NULL || s->n < s->cap)
            s->n++;
    }
}

static void on_assignments(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(stream_assignments, de_ep_stream_assignments_params, q);
    sync_world(ed);
    if (!require_open(req, w, "stream.assignments"))
        return;
    st_assign s;
    memset(&s, 0, sizeof s);
    s.w = g_st.world;
    const de_builtin_component_ids b = de_builtin_components(s.w);
    s.tf = b.transform;
    s.gtf = b.global_transform;
    s.pin = de_component_find(s.w, ST_PIN_NAME);
    if (q.has_entities) {
        for (u32 i = 0; i < q.entities.count; ++i) {
            const de_entity e = {q.entities.items[i]};
            if (!de_entity_alive(s.w, e) || de__ed_views_owns(e.bits)) {
                de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "stream.assignments: no entity %llu",
                             (unsigned long long)e.bits);
                return;
            }
        }
        s.cap = q.entities.count;
        s.items = s.cap > 0 ? de__ed_alloc(a, sizeof *s.items * s.cap) : NULL;
        if (s.cap > 0 && s.items == NULL) {
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "stream.assignments: too many entities");
            return;
        }
        for (u32 i = 0; i < q.entities.count; ++i) {
            const de_entity e = {q.entities.items[i]};
            if (assign_one(&s, e, &s.items[s.n]))
                s.n++;
        }
    } else {
        const de_query qa = de_query_create(s.w, &(de_query_desc){NULL, 0, NULL, 0});
        de_query_each(s.w, qa, assign_chunk, &s); /* count */
        s.cap = s.n;
        s.n = 0;
        s.items = s.cap > 0 ? de__ed_alloc(a, sizeof *s.items * s.cap) : NULL;
        if (s.cap > 0 && s.items == NULL) {
            de_query_destroy(s.w, qa);
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "stream.assignments: too many entities");
            return;
        }
        if (s.items != NULL)
            de_query_each(s.w, qa, assign_chunk, &s);
        de_query_destroy(s.w, qa);
    }
    de_ep_stream_assignments o;
    memset(&o, 0, sizeof o);
    o.revision = ecs_change_tick(s.w);
    o.items.items = s.items;
    o.items.count = s.n;
    de_ep_write_response_stream_assignments(w, req->id, &o);
}

/* ---------- stream.regenerateProxy and stream.cancelProxy ---------- */

static void on_regenerate(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    DE__ED_PARAMS(stream_regenerate_proxy, de_ep_stream_regenerate_proxy_params, q);
    sync_world(ed);
    if (!require_open(req, w, "stream.regenerateProxy"))
        return;
    de_asset_world_info wi;
    if (de_asset_world_info_get(g_st.assets, g_st.open, &wi) == DE_OK && !wi.proxy_enabled) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "stream.regenerateProxy: world %s has proxies off", g_st.open);
        return;
    }
    for (u32 k = 0; q.has_cells && k < q.cells.count; ++k) {
        u32 i;
        if (!cell_index(q.cells.items[k].cx, q.cells.items[k].cy, &i)) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND,
                         "stream.regenerateProxy: no cell (%d, %d)", (int)q.cells.items[k].cx,
                         (int)q.cells.items[k].cy);
            return;
        }
    }
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED,
                 "stream.regenerateProxy: the engine does not bake proxy meshes yet (DENG-830)");
}

static void on_cancel(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                      mpack_writer_t *w) {
    DE__ED_PARAMS(stream_cancel_proxy, de_ep_stream_cancel_proxy_params, q);
    (void)ed;
    de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "stream.cancelProxy: no proxy job %u is running",
                 q.job);
}

/* ---------- stream.overlay ---------- */

static void on_overlay(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    DE__ED_PARAMS(stream_overlay, de_ep_stream_overlay_params, q);
    (void)ed;
    g_st.ov = q;
    const u32 n = q.has_load_order ? q.load_order.count : 0;
    const u32 kept = n < ST_MAX_ORDER ? n : ST_MAX_ORDER;
    for (u32 i = 0; i < kept; ++i)
        g_st.ov_order[i] = q.load_order.items[i];
    g_st.ov.load_order.items = g_st.ov_order;
    g_st.ov.load_order.count = kept;
    de_ep_empty o;
    memset(&o, 0, sizeof o);
    de_ep_write_response_stream_overlay(w, req->id, &o);
}

static de_vec4 rgba(f32 r, f32 g, f32 b) {
    de_vec4 c;
    c.x = r, c.y = g, c.z = b, c.w = 1.0f;
    return c;
}

static void draw_overlay(const de__editor *ed) {
    if (g_st.p == NULL ||
        (ed->mode != DE_EP_WORLD_MODE_EDIT && ed->mode != DE_EP_WORLD_MODE_SIMULATE))
        return;
    const de_draw_category cat = g_st.cat;
    const u32 n = de__wp_cell_count(g_st.p);
    for (u32 i = 0; i < n && i < ST_DRAW_CELLS; ++i) {
        de__wp_cell_info c;
        if (!de__wp_cell_info_at(g_st.p, i, &c))
            continue;
        const f64 x0 = (f64)c.cx * 256.0, y0 = (f64)c.cy * 256.0, z = c.zmin;
        if (g_st.ov.grid) {
            const de_vec4 col = c.state == DE_WORLD_CELL_LOADED    ? rgba(0.2f, 0.8f, 0.3f)
                                : c.state == DE_WORLD_CELL_LOADING ? rgba(0.2f, 0.6f, 1.0f)
                                : c.state == DE_WORLD_CELL_FAILED  ? rgba(0.9f, 0.2f, 0.2f)
                                                                   : rgba(0.4f, 0.4f, 0.4f);
            const de_dvec3 p[4] = {
                {x0, y0, z}, {x0 + 256.0, y0, z}, {x0 + 256.0, y0 + 256.0, z}, {x0, y0 + 256.0, z}};
            for (u32 k = 0; k < 4; ++k)
                de_draw_line(cat, p[k], p[(k + 1) % 4], col, 0.0f);
        }
        if (g_st.ov.memory && c.state == DE_WORLD_CELL_LOADED) {
            char label[32];
            snprintf(label, sizeof label, "%.1f MiB", (f64)c.bytes / (1024.0 * 1024.0));
            de_draw_text(cat, (de_dvec3){x0 + 128.0, y0 + 128.0, z}, label,
                         rgba(1.0f, 1.0f, 1.0f), 0.0f);
        }
        if (g_st.ov.proxies && c.proxy_visible) {
            const de_dvec3 mn = {x0, y0, c.zmin}, mx = {x0 + 256.0, y0 + 256.0, c.zmax};
            de_draw_box(cat, mn, mx, rgba(0.7f, 0.5f, 1.0f), 0.0f);
        }
    }
    if (g_st.ov.sources) {
        const de__wp_source *s = NULL;
        const u32 sn = de__wp_sources(g_st.p, &s);
        const f32 ratio = de__wp_load_radius(g_st.p) > 0.0f
                              ? de__wp_unload_radius(g_st.p) / de__wp_load_radius(g_st.p)
                              : 1.0f;
        for (u32 i = 0; i < sn; ++i) {
            const de_dvec3 at = {s[i].s.x, s[i].s.y, s[i].s.z};
            de_draw_sphere(cat, at, s[i].s.radius, rgba(1.0f, 0.8f, 0.2f), 0.0f);
            de_draw_sphere(cat, at, s[i].s.radius * ratio, rgba(0.8f, 0.4f, 0.1f), 0.0f);
        }
    }
    for (u32 i = 0; g_st.ov.has_load_order && i < g_st.ov.load_order.count; ++i) {
        char label[16];
        snprintf(label, sizeof label, "%u", i + 1);
        const de_ep_stream_cell_key *k = &g_st.ov.load_order.items[i];
        de_draw_text(cat, (de_dvec3){(f64)k->cx * 256.0 + 128.0, (f64)k->cy * 256.0 + 128.0, 0.0},
                     label, rgba(1.0f, 1.0f, 0.2f), 0.0f);
    }
}

/* ---------- Tick, reset, register ---------- */

static bool same_cell(const de__wp_cell_info *a, const de__wp_cell_info *b) {
    return a->state == b->state && a->force == b->force && a->bytes == b->bytes &&
           a->entities == b->entities && a->zmin == b->zmin && a->zmax == b->zmax &&
           a->proxy_visible == b->proxy_visible && a->proxy_alpha == b->proxy_alpha;
}

/* The topic stream.cells: the cells that changed since the last note. */
static void send_changes(de__editor *ed) {
    u32 n = 0;
    const u32 count = de__wp_cell_count(g_st.p);
    for (u32 i = 0; i < count && i < g_st.cell_count; ++i) {
        de__wp_cell_info c;
        if (!de__wp_cell_info_at(g_st.p, i, &c))
            continue;
        f64 d = 0.0;
        const bool has_d = de__wp_cell_distance(g_st.p, i, &d);
        const bool moved = has_d != g_st.sent_dist[i] || (has_d && d != g_st.sent_d[i]);
        const bool changed = !same_cell(&c, &g_st.sent[i]);
        g_st.sent_dist[i] = has_d;
        g_st.sent_d[i] = d;
        if (!changed && !moved)
            continue;
        g_st.sent[i] = c;
        g_st.sent[i].error = NULL;
        if (changed)
            g_st.list[n++] = *fill_cell(i, &c);
    }
    if (n == 0)
        return;
    g_st.revision++;
    if (!ed->connected)
        return;
    de_ep_stream_cells_changed p;
    memset(&p, 0, sizeof p);
    p.world = de_ep_cstr(g_st.open);
    p.revision = g_st.revision;
    p.cells.items = g_st.list;
    p.cells.count = n;
    fill_stats(&p.stats);
    mpack_writer_t nw;
    const u64 seq = de__ed_note_begin(ed, &nw);
    de_ep_write_notify_stream_cells(&nw, seq, &p);
    de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
}

static void tick(de__editor *ed) {
    sync_world(ed);
    if (g_st.p == NULL)
        return;
    const u64 now = de_time_now_ns();
    f64 dt = now > g_st.last_ns ? (f64)(now - g_st.last_ns) / 1e9 : 0.0;
    dt = dt < 0.25 ? dt : 0.25;
    g_st.last_ns = now;
    if (ed->mode == DE_EP_WORLD_MODE_EDIT || ed->mode == DE_EP_WORLD_MODE_SIMULATE)
        (void)de_world_partition_update(g_st.p, (f32)dt);
    send_changes(ed);
    draw_overlay(ed);
}

static void reset(de__editor *ed) {
    (void)ed;
    g_st.ed_src_count = 0;
    de__wp_set_editor_sources(g_st.p, NULL, 0);
    de__wp_clear_forces(g_st.p);
    g_st.revision++;
}

void de__ed_stream_register(de__editor *ed) {
    close_partition(true);
    memset(&g_st, 0, sizeof g_st);
    g_st.cat = de_draw_category_register("streaming", 0x40a0e0, false);
    sync_world(ed);
    de__ed_on(ed, DE_EP_METHOD_STREAM_INFO, on_info);
    de__ed_on(ed, DE_EP_METHOD_STREAM_OPEN, on_open);
    de__ed_on(ed, DE_EP_METHOD_STREAM_CELLS, on_cells);
    de__ed_on(ed, DE_EP_METHOD_STREAM_FORCE, on_force);
    de__ed_on(ed, DE_EP_METHOD_STREAM_SET_EDITOR_SOURCES, on_set_sources);
    de__ed_on(ed, DE_EP_METHOD_STREAM_PREVIEW, on_preview);
    de__ed_on(ed, DE_EP_METHOD_STREAM_ASSIGNMENTS, on_assignments);
    de__ed_on(ed, DE_EP_METHOD_STREAM_REGENERATE_PROXY, on_regenerate);
    de__ed_on(ed, DE_EP_METHOD_STREAM_CANCEL_PROXY, on_cancel);
    de__ed_on(ed, DE_EP_METHOD_STREAM_OVERLAY, on_overlay);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
    de__ed_add_capability(ed, ST_CAP);
}

void de__ed_stream_shutdown(de__editor *ed) {
    /* The engine's world is still alive here (de__editor_stop runs before it goes). */
    close_partition(world_of(ed) == g_st.world && g_st.world != NULL);
    g_st.world = NULL;
    g_st.assets = NULL;
    g_st.ed_src_count = 0;
}
