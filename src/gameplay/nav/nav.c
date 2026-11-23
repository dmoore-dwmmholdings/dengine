/*
 * nav.c: the nav runtime (DENG-76, SPEC §21.2): lifetime, the world attachment and its
 * `nav.update` system, background tile builds and their in-order application, obstacles and
 * cooked navmeshes. Input gathering is in nav_input.c, the `navmesh` asset type in nav_asset.c,
 * the build itself in nav_build.c and the Recast / Detour bridge.
 *
 * Threading: every public function is main-thread only. A rebuild gathers its input on the main
 * thread, then builds its tiles in one DE_TASK_POOL_BACKGROUND task (inline when the nav has no
 * task system); de_nav_update applies finished builds to the navmesh in the order they were
 * asked for. The navmesh itself is only touched on the main thread.
 */
#include "gameplay/nav/nav_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>

#include <string.h>

/* ---- Jobs ------------------------------------------------------------------------------------ */

static void job_free(de__nav_job *j) {
    const de_allocator *a = j->nav->alloc;
    for (u32 i = 0; j->data != NULL && i < j->tile_count; ++i)
        de__nav_rc_free(j->data[i]);
    if (j->data != NULL)
        DE_FREE(a, j->data, sizeof(u8 *) * j->tile_count);
    if (j->sizes != NULL)
        DE_FREE(a, j->sizes, sizeof(i32) * j->tile_count);
    de__nav_soup_free(&j->soup);
    DE_FREE(a, j, sizeof *j);
}

static void job_run(void *user) {
    de__nav_job *j = (de__nav_job *)user;
    const i32 w = j->tx1 - j->tx0 + 1;
    for (u32 i = 0; i < j->tile_count && j->result == DE_OK; ++i) {
        const i32 tx = j->tx0 + (i32)i % w, tz = j->tz0 + (i32)i / w;
        j->result = de__nav_build_tile(j->nav->alloc, &j->settings, &j->soup, tx, tz, &j->data[i],
                                       &j->sizes[i]);
    }
}

static void job_apply(de_nav *n, de__nav_job *j) {
    if (j->result != DE_OK) {
        DE_WARN(n->log, "nav: a tile rebuild failed (%s); the old tiles stay",
                de_result_str(j->result));
        return;
    }
    const i32 w = j->tx1 - j->tx0 + 1;
    for (u32 i = 0; i < j->tile_count; ++i) {
        const i32 tx = j->tx0 + (i32)i % w, tz = j->tz0 + (i32)i / w;
        u8 *data = j->data[i];
        j->data[i] = NULL; /* the navmesh owns it now (or freed it) */
        const de_result r = de__nav_mesh_set_tile(n->mesh, tx, tz, data, j->sizes[i]);
        if (r != DE_OK)
            DE_WARN(n->log, "nav: tile (%d, %d) not added (%s)", (int)tx, (int)tz,
                    de_result_str(r));
    }
}

static void wait_all(de_nav *n) {
    for (de__nav_job *j = n->jobs_head; j != NULL; j = j->next)
        if (j->on_worker)
            de_task_wait(n->tasks, &j->counter);
}

void de_nav_update(de_nav *n) {
    if (n == NULL)
        return;
    de__nav_update_obstacles(n);
    if (n->deterministic)
        wait_all(n); /* every pending rebuild lands on this frame, on every machine */
    while (n->jobs_head != NULL) {
        de__nav_job *j = n->jobs_head;
        if (j->on_worker && !de_task_done(&j->counter))
            break; /* later jobs wait for it: tiles apply in request order */
        n->jobs_head = j->next;
        if (n->jobs_head == NULL)
            n->jobs_tail = NULL;
        job_apply(n, j);
        job_free(j);
    }
}

/* ---- Lifetime -------------------------------------------------------------------------------- */

de_result de_nav_create(const de_allocator *a, de_task_system *tasks, de_nav **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_create: NULL out");
    *out = NULL;
    if (a == NULL)
        a = de_heap_allocator();
    de_nav *n = DE_NEW(a, de_nav, DE_MEM_GENERAL);
    if (n == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav_create");
    memset(n, 0, sizeof *n);
    n->alloc = a;
    n->tasks = tasks;
    n->log = de_log_register_category("nav", DE_LOG_INFO);
    *out = n;
    return DE_OK;
}

void de_nav_destroy(de_nav *n) {
    if (n == NULL)
        return;
    wait_all(n);
    while (n->jobs_head != NULL) {
        de__nav_job *j = n->jobs_head;
        n->jobs_head = j->next;
        job_free(j);
    }
    de_nav_attach(n, NULL);
    de__nav_queries_free(n);
    de__nav_mesh_destroy(n->mesh);
    const de_allocator *a = n->alloc;
    if (n->links != NULL)
        DE_FREE(a, n->links, sizeof(de__nav_link) * n->link_cap);
    if (n->volumes != NULL)
        DE_FREE(a, n->volumes, sizeof(de__nav_volume) * n->volume_cap);
    if (n->obstacles != NULL)
        DE_FREE(a, n->obstacles, sizeof(de__nav_obstacle) * n->obstacle_cap);
    if (n->extra != NULL)
        DE_FREE(a, n->extra, sizeof(de_dvec3) * 3u * n->extra_tri_cap);
    DE_FREE(a, n, sizeof *n);
}

/* ---- World attachment ------------------------------------------------------------------------ */

static void ref_destruct(void *obj) {
    DeNavRef *ref = (DeNavRef *)obj;
    if (ref->ptr != NULL) {
        /* The world is going away, or de_nav_attach removed the resource. */
        ref->ptr->world = NULL;
        ref->ptr->resource = (de_resource_id){0};
        ref->ptr->system = (de_system){0};
        ref->ptr = NULL;
    }
}

static void ref_copy(void *dst, const void *src) {
    memcpy(dst, src, sizeof(DeNavRef));
}

static void update_fn(de_system_ctx *ctx) {
    de_nav_update((de_nav *)ctx->user);
}

void de_nav_attach(de_nav *n, de_world *w) {
    if (n == NULL || n->world == w)
        return;
    if (n->world != NULL && !DE_HANDLE_VALID(n->resource)) {
        n->world = NULL; /* bound with de__nav_bind_world, not attached */
    } else if (n->world != NULL) {
        de_world *old = n->world;
        de_system_remove(old, n->system);
        de_resource_remove(old, n->resource); /* ref_destruct clears n->world */
        n->world = NULL;
    }
    if (w == NULL)
        return;
    de_type_registry *types = de_world_types(w);
    de_type_id rt = de_type_find(types, "DeNavRef");
    if (!DE_HANDLE_VALID(rt)) {
        de_type_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.name = "DeNavRef";
        rd.size = sizeof(DeNavRef);
        rd.align = DE_ALIGNOF(DeNavRef);
        rd.version = 1;
        rd.hooks.destruct = ref_destruct;
        rd.hooks.copy = ref_copy;
        rt = de_type_register(types, &rd);
    }
    if (!DE_HANDLE_VALID(rt) || DE_HANDLE_VALID(de_resource_find(w, rt))) {
        DE_WARN(n->log, "nav: cannot attach (no DeNavRef type, or another nav is attached)");
        return;
    }
    DeNavRef ref = {n};
    n->resource = de_resource_insert(w, rt, &ref);
    if (!DE_HANDLE_VALID(n->resource)) {
        DE_WARN(n->log, "nav: out of memory attaching to a world");
        return;
    }
    n->world = w;
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "nav.update";
    sd.schedule = DE_SCHEDULE_PRE_UPDATE;
    sd.fn = update_fn;
    sd.user = n;
    sd.flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE;
    n->system = de_system_add(w, &sd);
    if (!DE_HANDLE_VALID(n->system)) {
        DE_WARN(n->log, "nav: cannot add the nav.update system");
        de_resource_remove(w, n->resource);
        n->world = NULL;
    }
}

/* ---- Builds ---------------------------------------------------------------------------------- */

static de_result ensure_mesh(de_nav *n, f32 tile_size) {
    if (n->mesh != NULL)
        return DE_OK;
    DE_TRY(de__nav_mesh_create(n->alloc, tile_size, &n->mesh));
    const de_result r = de__nav_queries_init(n);
    if (r != DE_OK) {
        de__nav_queries_free(n);
        de__nav_mesh_destroy(n->mesh);
        n->mesh = NULL;
    }
    return r;
}

static void gather_links(const de_nav *n, de__nav_soup *s) {
    for (u32 i = 0; i < n->link_count; ++i) {
        const de__nav_link *l = &n->links[i];
        de__nav_soup_add_offmesh(s, l->from, l->to, l->radius, l->bidirectional, l->area, l->id);
    }
}

static void gather_volumes(const de_nav *n, de__nav_soup *s) {
    for (u32 i = 0; i < n->volume_count; ++i) {
        const de__nav_volume *v = &n->volumes[i];
        if (v->carve)
            de__nav_soup_add_obstacle(s, &v->pose, v->half_extents);
        else
            de__nav_soup_add_area_volume(s, &v->pose, v->half_extents, v->area);
    }
}

static void gather_extra(const de_nav *n, de__nav_soup *s) {
    for (u32 i = 0; i < n->extra_tri_count; ++i)
        de__nav_soup_add_tri(s, n->extra[i * 3u], n->extra[i * 3u + 1u], n->extra[i * 3u + 2u]);
}

de_result de_nav_rebuild_tiles(de_nav *n, const de_navmesh_build_desc *desc, de_aabb region) {
    if (n == NULL || desc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_rebuild_tiles: NULL argument");
    if (!(region.min.x <= region.max.x) || !(region.min.y <= region.max.y) ||
        !(region.min.z <= region.max.z))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_rebuild_tiles: empty region");
    de__nav_settings st;
    const char *why = NULL;
    if (de__nav_settings_make(desc, &st, &why) != DE_OK)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_rebuild_tiles: %s", why);
    if (n->has_settings && st.desc.tile_size != n->settings.desc.tile_size)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_nav_rebuild_tiles: tile_size %g differs from the navmesh's %g",
                       (double)st.desc.tile_size, (double)n->settings.desc.tile_size);
    if (!n->has_origin) {
        de_dvec3 c;
        c.x = ((f64)region.min.x + (f64)region.max.x) * 0.5;
        c.y = ((f64)region.min.y + (f64)region.max.y) * 0.5;
        c.z = 0.0;
        n->origin = de__nav_origin_for(c);
        n->has_origin = true;
    }
    DE_TRY(ensure_mesh(n, st.desc.tile_size));
    n->settings = st;
    n->has_settings = true;

    de_dvec3 wmin, wmax;
    wmin.x = region.min.x;
    wmin.y = region.min.y;
    wmin.z = region.min.z;
    wmax.x = region.max.x;
    wmax.y = region.max.y;
    wmax.z = region.max.z;
    f32 rmin[3], rmax[3];
    de__nav_box_to_recast(&n->origin, wmin, wmax, rmin, rmax);
    i32 tx0, tz0, tx1, tz1;
    de__nav_tile_range(st.desc.tile_size, rmin, rmax, &tx0, &tz0, &tx1, &tz1);
    const i64 count = ((i64)tx1 - tx0 + 1) * ((i64)tz1 - tz0 + 1);
    if (count > DE__NAV_MAX_TILES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_rebuild_tiles: more than %d tiles",
                       DE__NAV_MAX_TILES);
    return de__nav_rebuild_range(n, &st, tx0, tz0, tx1, tz1);
}

de_result de__nav_rebuild_range(de_nav *n, const de__nav_settings *stp, i32 tx0, i32 tz0, i32 tx1,
                                i32 tz1) {
    const de__nav_settings st = *stp;
    const i64 count = ((i64)tx1 - tx0 + 1) * ((i64)tz1 - tz0 + 1);
    if (count <= 0 || count > DE__NAV_MAX_TILES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav: tile range of %lld tiles", (long long)count);
    const de_allocator *a = n->alloc;
    de__nav_job *j = DE_NEW(a, de__nav_job, DE_MEM_GENERAL);
    if (j == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav_rebuild_tiles");
    memset(j, 0, sizeof *j);
    j->nav = n;
    j->settings = st;
    j->tx0 = tx0;
    j->tz0 = tz0;
    j->tx1 = tx1;
    j->tz1 = tz1;
    j->tile_count = (u32)count;
    j->result = DE_OK;
    de__nav_soup_init(&j->soup, a, n->origin);
    /* Input that can reach the tiles: the tile range plus the border, every height. */
    const f32 pad = (f32)st.border * st.desc.cell_size, t = st.desc.tile_size;
    j->soup.clip = true;
    j->soup.clip_min[0] = (f32)tx0 * t - pad;
    j->soup.clip_min[2] = (f32)tz0 * t - pad;
    j->soup.clip_max[0] = (f32)(tx1 + 1) * t + pad;
    j->soup.clip_max[2] = (f32)(tz1 + 1) * t + pad;
    de__nav_gather_world(n, &j->soup, st.desc.collision_mask);
    gather_extra(n, &j->soup);
    de__nav_gather_obstacles(n, &j->soup, tx0, tz0, tx1, tz1);
    gather_volumes(n, &j->soup);
    gather_links(n, &j->soup);
    j->data = (u8 **)DE_ALLOC(a, sizeof(u8 *) * j->tile_count, 16, DE_MEM_GENERAL);
    j->sizes = (i32 *)DE_ALLOC(a, sizeof(i32) * j->tile_count, 16, DE_MEM_GENERAL);
    if (j->soup.failed || j->data == NULL || j->sizes == NULL) {
        if (j->data != NULL)
            DE_FREE(a, j->data, sizeof(u8 *) * j->tile_count);
        if (j->sizes != NULL)
            DE_FREE(a, j->sizes, sizeof(i32) * j->tile_count);
        j->data = NULL;
        j->sizes = NULL;
        job_free(j);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav_rebuild_tiles: input");
    }
    memset(j->data, 0, sizeof(u8 *) * j->tile_count);
    memset(j->sizes, 0, sizeof(i32) * j->tile_count);
    if (n->jobs_tail != NULL)
        n->jobs_tail->next = j;
    else
        n->jobs_head = j;
    n->jobs_tail = j;
    if (n->tasks != NULL) {
        j->on_worker = true;
        de_task_submit(n->tasks, DE_TASK_POOL_BACKGROUND, job_run, j, &j->counter);
    } else {
        job_run(j); /* applied by the next de_nav_update, as with a worker */
    }
    return DE_OK;
}

void de_nav_add_obstacle(de_nav *n, de_entity e, de_vec3 half_extents) {
    if (n == NULL || !(half_extents.x > 0.0f) || !(half_extents.y > 0.0f) ||
        !(half_extents.z > 0.0f))
        return;
    for (u32 i = 0; i < n->obstacle_count; ++i) {
        de__nav_obstacle *o = &n->obstacles[i];
        if (o->entity.bits == e.bits) {
            if (o->removed || o->half_extents.x != half_extents.x ||
                o->half_extents.y != half_extents.y || o->half_extents.z != half_extents.z)
                o->dirty = true;
            o->removed = false;
            o->half_extents = half_extents;
            return;
        }
    }
    if (n->obstacle_count == n->obstacle_cap) {
        const u32 nc = n->obstacle_cap != 0 ? n->obstacle_cap * 2u : 8u;
        void *p = n->obstacles == NULL
                      ? DE_ALLOC(n->alloc, sizeof(de__nav_obstacle) * nc, 16, DE_MEM_GENERAL)
                      : DE_REALLOC(n->alloc, n->obstacles,
                                   sizeof(de__nav_obstacle) * n->obstacle_cap,
                                   sizeof(de__nav_obstacle) * nc, 16, DE_MEM_GENERAL);
        if (p == NULL) {
            DE_WARN(n->log, "nav: out of memory adding an obstacle");
            return;
        }
        n->obstacles = (de__nav_obstacle *)p;
        n->obstacle_cap = nc;
    }
    de__nav_obstacle *o = &n->obstacles[n->obstacle_count];
    memset(o, 0, sizeof *o);
    o->entity = e;
    o->half_extents = half_extents;
    o->dirty = true;
    ++n->obstacle_count;
}

void de_nav_remove_obstacle(de_nav *n, de_entity e) {
    if (n == NULL)
        return;
    for (u32 i = 0; i < n->obstacle_count; ++i)
        if (n->obstacles[i].entity.bits == e.bits)
            n->obstacles[i].removed = true;
}

void de__nav_gather_obstacles(de_nav *n, de__nav_soup *soup, i32 tx0, i32 tz0, i32 tx1, i32 tz1) {
    if (n->world == NULL)
        return;
    for (u32 i = 0; i < n->obstacle_count; ++i) {
        de__nav_obstacle *o = &n->obstacles[i];
        de__nav_pose p;
        if (o->removed || !de__nav_entity_pose(n->world, o->entity, &p))
            continue;
        de__nav_soup_add_obstacle(soup, &p, o->half_extents);
        i32 a0, b0, a1, b1;
        de__nav_obstacle_tiles(n, &p, o->half_extents, &a0, &b0, &a1, &b1);
        if (a0 >= tx0 && b0 >= tz0 && a1 <= tx1 && b1 <= tz1) {
            o->carved = true;
            o->dirty = false;
            o->carved_pose = p;
            o->carved_half = o->half_extents;
        }
    }
}

void de__nav_obstacle_tiles(const de_nav *n, const de__nav_pose *p, de_vec3 h, i32 *tx0, i32 *tz0,
                            i32 *tx1, i32 *tz1) {
    de_dvec3 wmin = p->pos, wmax = p->pos;
    for (u32 i = 0; i < 8; ++i) {
        de_vec3 c;
        c.x = (i & 1u) ? h.x : -h.x;
        c.y = (i & 2u) ? h.y : -h.y;
        c.z = (i & 4u) ? h.z : -h.z;
        const de_dvec3 w = de__nav_pose_apply(p, c);
        wmin.x = w.x < wmin.x ? w.x : wmin.x;
        wmin.y = w.y < wmin.y ? w.y : wmin.y;
        wmin.z = w.z < wmin.z ? w.z : wmin.z;
        wmax.x = w.x > wmax.x ? w.x : wmax.x;
        wmax.y = w.y > wmax.y ? w.y : wmax.y;
        wmax.z = w.z > wmax.z ? w.z : wmax.z;
    }
    f32 rmin[3], rmax[3];
    de__nav_box_to_recast(&n->origin, wmin, wmax, rmin, rmax);
    /* Erosion grows the footprint by the agent radius, and a tile reads `border` cells of input
     * around itself: tiles within that distance change too. */
    const de__nav_settings *st = &n->settings;
    const f32 pad = (f32)st->border * st->desc.cell_size + st->desc.agent_radius;
    rmin[0] -= pad;
    rmin[2] -= pad;
    rmax[0] += pad;
    rmax[2] += pad;
    de__nav_tile_range(st->desc.tile_size, rmin, rmax, tx0, tz0, tx1, tz1);
}

de_result de_nav_add_offmesh_link(de_nav *n, de_dvec3 from, de_dvec3 to, f32 radius,
                                  bool bidirectional, u8 area) {
    return de__nav_add_link_id(n, from, to, radius, bidirectional, area,
                               n != NULL ? n->link_count : 0u);
}

void de__nav_bind_world(de_nav *n, de_world *w) {
    if (n == NULL || DE_HANDLE_VALID(n->resource))
        return;
    n->world = w;
}

void de__nav_clear_links(de_nav *n) {
    if (n != NULL)
        n->link_count = 0;
}

de_result de__nav_set_volumes(de_nav *n, const de__nav_volume *v, u32 count) {
    if (n == NULL || (count > 0 && v == NULL))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav: volumes");
    if (count > n->volume_cap) {
        void *p = n->volumes == NULL
                      ? DE_ALLOC(n->alloc, sizeof(de__nav_volume) * count, 16, DE_MEM_GENERAL)
                      : DE_REALLOC(n->alloc, n->volumes, sizeof(de__nav_volume) * n->volume_cap,
                                   sizeof(de__nav_volume) * count, 16, DE_MEM_GENERAL);
        if (p == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "nav: volumes");
        n->volumes = (de__nav_volume *)p;
        n->volume_cap = count;
    }
    if (count > 0)
        memcpy(n->volumes, v, sizeof(de__nav_volume) * count);
    n->volume_count = count;
    return DE_OK;
}

void de__nav_gather_input(de_nav *n, de__nav_soup *soup, u32 mask) {
    de__nav_gather_world(n, soup, mask);
    gather_extra(n, soup);
    if (n->world != NULL) {
        for (u32 i = 0; i < n->obstacle_count; ++i) {
            de__nav_pose p;
            if (!n->obstacles[i].removed &&
                de__nav_entity_pose(n->world, n->obstacles[i].entity, &p))
                de__nav_soup_add_obstacle(soup, &p, n->obstacles[i].half_extents);
        }
    }
    gather_volumes(n, soup);
    gather_links(n, soup);
}

de_result de__nav_add_link_id(de_nav *n, de_dvec3 from, de_dvec3 to, f32 radius,
                              bool bidirectional, u8 area, u32 id) {
    if (n == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_add_offmesh_link: NULL nav");
    if (!(radius > 0.0f) || area > 15u)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_nav_add_offmesh_link: radius must be > 0 and area 0..15");
    if (n->link_count == n->link_cap) {
        const u32 nc = n->link_cap != 0 ? n->link_cap * 2u : 8u;
        void *p = n->links == NULL
                      ? DE_ALLOC(n->alloc, sizeof(de__nav_link) * nc, 16, DE_MEM_GENERAL)
                      : DE_REALLOC(n->alloc, n->links, sizeof(de__nav_link) * n->link_cap,
                                   sizeof(de__nav_link) * nc, 16, DE_MEM_GENERAL);
        if (p == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav_add_offmesh_link");
        n->links = (de__nav_link *)p;
        n->link_cap = nc;
    }
    de__nav_link *l = &n->links[n->link_count++];
    l->from = from;
    l->to = to;
    l->radius = radius;
    l->bidirectional = bidirectional;
    l->area = area;
    l->id = id;
    return DE_OK;
}

/* ---- Cooked navmeshes ------------------------------------------------------------------------ */

de_result de__nav_add_container(de_nav *n, const u8 *data, u64 size) {
    de__nav_container_info info;
    const char *why = NULL;
    if (n == NULL || !de__nav_container_read(data, size, &info, &why))
        return DE_FAIL(DE_ERR_PARSE, "%s", why != NULL ? why : "navmesh: NULL argument");
    if (n->has_settings && info.desc.tile_size != n->settings.desc.tile_size)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "navmesh: tile_size differs from the nav's");
    if (n->has_origin && (info.origin.x != n->origin.x || info.origin.y != n->origin.y ||
                          info.origin.z != n->origin.z))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "navmesh: origin differs from the nav's");
    DE_TRY(ensure_mesh(n, info.desc.tile_size));
    if (!n->has_settings) {
        (void)de__nav_settings_make(&info.desc, &n->settings, NULL);
        n->has_settings = true;
    }
    n->origin = info.origin;
    n->has_origin = true;
    for (u32 i = 0; i < info.tile_count; ++i) {
        de__nav_tile_blob b;
        if (!de__nav_container_tile(data, size, i, &b))
            return DE_FAIL(DE_ERR_PARSE, "navmesh: tile %u", (unsigned)i);
        u8 *copy = de__nav_rc_alloc(n->alloc, (i32)b.size);
        if (copy == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "navmesh: tile copy");
        memcpy(copy, b.data, b.size);
        const de_result r = de__nav_mesh_set_tile(n->mesh, b.tx, b.tz, copy, (i32)b.size);
        if (r != DE_OK)
            return DE_FAIL(r, "navmesh: tile (%d, %d)", (int)b.tx, (int)b.tz);
    }
    return DE_OK;
}

void de_nav_add_navmesh(de_nav *n, de_navmesh mesh) {
    if (n == NULL)
        return;
    u64 size = 0;
    const u8 *bytes = de__nav_asset_bytes(n->assets, mesh, &size);
    if (bytes == NULL) {
        DE_WARN(n->log, "nav: de_nav_add_navmesh: the navmesh is not loaded (or "
                        "de_nav_use_assets was not called)");
        return;
    }
    if (de__nav_add_container(n, bytes, size) != DE_OK)
        DE_WARN(n->log, "nav: navmesh skipped: %s",
                de_error_last() != NULL ? de_error_last()->message : "error");
}

/* ---- Test hooks ------------------------------------------------------------------------------ */

de_result de__nav_add_test_tris(de_nav *n, const de_dvec3 *points, u32 tri_count) {
    if (n == NULL || (points == NULL && tri_count > 0))
        return DE_ERR_INVALID_ARGUMENT;
    const u32 need = n->extra_tri_count + tri_count;
    if (need > n->extra_tri_cap) {
        u32 nc = n->extra_tri_cap != 0 ? n->extra_tri_cap : 16u;
        while (nc < need)
            nc *= 2u;
        void *p = n->extra == NULL
                      ? DE_ALLOC(n->alloc, sizeof(de_dvec3) * 3u * nc, 16, DE_MEM_GENERAL)
                      : DE_REALLOC(n->alloc, n->extra, sizeof(de_dvec3) * 3u * n->extra_tri_cap,
                                   sizeof(de_dvec3) * 3u * nc, 16, DE_MEM_GENERAL);
        if (p == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        n->extra = (de_dvec3 *)p;
        n->extra_tri_cap = nc;
    }
    memcpy(&n->extra[n->extra_tri_count * 3u], points, sizeof(de_dvec3) * 3u * tri_count);
    n->extra_tri_count = need;
    return DE_OK;
}

bool de__nav_point_on_mesh(const de_nav *n, de_dvec3 p, f32 max_dz) {
    if (n == NULL || n->mesh == NULL || !n->has_origin)
        return false;
    f32 r[3];
    de__nav_world_to_recast(&n->origin, p, r);
    return de__nav_mesh_point_on_poly(n->mesh, r, max_dz);
}

u64 de__nav_tile_ref(const de_nav *n, i32 tx, i32 tz) {
    return n != NULL && n->mesh != NULL ? de__nav_mesh_tile_ref(n->mesh, tx, tz) : 0;
}

u32 de__nav_tile_count(const de_nav *n) {
    return n != NULL && n->mesh != NULL ? de__nav_mesh_tile_count(n->mesh) : 0;
}

bool de__nav_tile_stats(const de_nav *n, i32 tx, i32 tz, i32 *polys, i32 *links) {
    *polys = 0;
    *links = 0;
    return n != NULL && n->mesh != NULL && de__nav_mesh_tile_stats(n->mesh, tx, tz, polys, links);
}

u32 de__nav_tile_tris(const de_nav *n, i32 tx, i32 tz, de_dvec3 *pts, u8 *areas, u32 cap) {
    if (n == NULL || n->mesh == NULL || !n->has_origin)
        return 0;
    const i32 total = de__nav_mesh_tile_tris(n->mesh, tx, tz, NULL, NULL, 0);
    if (total <= 0)
        return 0;
    if (cap == 0 || pts == NULL || areas == NULL)
        return (u32)total;
    const u32 take = (u32)total < cap ? (u32)total : cap;
    f32 *xyz = (f32 *)DE_ALLOC(n->alloc, sizeof(f32) * 9u * (u32)total, 16, DE_MEM_GENERAL);
    u8 *ar = (u8 *)DE_ALLOC(n->alloc, (u32)total, 16, DE_MEM_GENERAL);
    if (xyz != NULL && ar != NULL) {
        (void)de__nav_mesh_tile_tris(n->mesh, tx, tz, xyz, ar, total);
        for (u32 i = 0; i < take; ++i) {
            for (u32 k = 0; k < 3; ++k)
                pts[i * 3u + k] = de__nav_recast_to_world(&n->origin, &xyz[i * 9u + k * 3u]);
            areas[i] = ar[i];
        }
    }
    if (xyz != NULL)
        DE_FREE(n->alloc, xyz, sizeof(f32) * 9u * (u32)total);
    if (ar != NULL)
        DE_FREE(n->alloc, ar, (u32)total);
    return (u32)total;
}

bool de__nav_tile_of(const de_nav *n, de_dvec3 p, i32 *tx, i32 *tz) {
    if (n == NULL || !n->has_origin || !n->has_settings)
        return false;
    f32 r[3];
    de__nav_world_to_recast(&n->origin, p, r);
    *tx = (i32)de_dfloor(r[0] / n->settings.desc.tile_size);
    *tz = (i32)de_dfloor(r[2] / n->settings.desc.tile_size);
    return true;
}

bool de__nav_origin(const de_nav *n, de_dvec3 *out) {
    if (n == NULL || !n->has_origin)
        return false;
    *out = n->origin;
    return true;
}

u64 de__nav_mesh_checksum(const de_nav *n) {
    return n != NULL ? de__nav_mesh_hash(n->mesh) : 0;
}

u32 de__nav_obstacle_count(const de_nav *n) {
    return n != NULL ? n->obstacle_count : 0;
}

u32 de__nav_pending_jobs(const de_nav *n) {
    u32 c = 0;
    for (const de__nav_job *j = n != NULL ? n->jobs_head : NULL; j != NULL; j = j->next)
        ++c;
    return c;
}
