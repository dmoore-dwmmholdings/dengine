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
    de__nav_mesh_destroy(n->mesh);
    const de_allocator *a = n->alloc;
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
    if (n->world != NULL) {
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
    return de__nav_mesh_create(n->alloc, tile_size, &n->mesh);
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
    de__nav_gather_obstacles(n, &j->soup);
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
        if (n->obstacles[i].entity.bits == e.bits) {
            n->obstacles[i].half_extents = half_extents;
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
    n->obstacles[n->obstacle_count].entity = e;
    n->obstacles[n->obstacle_count].half_extents = half_extents;
    ++n->obstacle_count;
}

void de__nav_gather_obstacles(de_nav *n, de__nav_soup *soup) {
    if (n->world == NULL)
        return;
    for (u32 i = 0; i < n->obstacle_count; ++i) {
        de__nav_pose p;
        if (de__nav_entity_pose(n->world, n->obstacles[i].entity, &p))
            de__nav_soup_add_obstacle(soup, &p, n->obstacles[i].half_extents);
    }
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

u32 de__nav_pending_jobs(const de_nav *n) {
    u32 c = 0;
    for (const de__nav_job *j = n != NULL ? n->jobs_head : NULL; j != NULL; j = j->next)
        ++c;
    return c;
}
