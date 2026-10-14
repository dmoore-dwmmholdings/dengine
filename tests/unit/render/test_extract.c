/*
 * test_extract.c: the render world and the Extract systems (DENG-175; SPEC §12.4, §5.5, §8).
 * Executable test_render_world, CTest names render_world.extract.<test>.
 *
 *   - camera_relative_precision: instances around (1e6, -1e6, 50) m come out relative to the
 *     grid-snapped render origin with an error below 1 mm.
 *   - origin_rebase: the origin moves only when the main camera leaves its cell (or the grid
 *     changes); that frame has origin_changed and every transform relative to the new origin.
 *   - change_list_single_dirty: one modified DeMeshRenderer among 10k entities is one dirty
 *     entry; a moved one is dirty twice (its move, then its previous transform catching up).
 *   - despawn_removal: a despawn (or a removed component) is one removal; the slot is reused.
 *   - no_heap_after_warmup: a stable scene (some entities and the camera moving) extracts and
 *     renders without heap allocation, unpipelined and pipelined.
 *
 * The fixture (rwt_*) is shared with test_render_thread.c. Everything runs on the null GPU
 * backend without an asset system (instance bounds stay empty).
 */
#include "de_test.h"

#include "de_alloc_guard.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "render/components_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "test_render_world.h"

/* ---- Fixture -------------------------------------------------------------------------------- */

void rwt_set_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
}

void rwt_start(rwt *f, u32 workers, bool pipelined) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
    de_fp_env_init_thread(); /* the task system checks the main thread's FP mode */
    if (workers > 0)
        ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = workers}, &f->tasks),
                  DE_OK);
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(
        de_world_create(
            &(de_world_desc){.allocator = &f->alloc, .types = f->types, .tasks = f->tasks}, &f->w),
        DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL,
                                                         .allocator = &f->dev_alloc,
                                                         .pipeline_cache_dir = ""},
                                   &f->d),
              DE_OK);
    ASSERT_EQ(
        de_renderer_create(
            &(de_renderer_desc){.device = f->d, .types = f->types, .allocator = &f->alloc}, &f->r),
        DE_OK);
    de_renderer_attach(f->r, f->w);
    const DeRenderComponents *ids = de__render_components(f->w);
    ASSERT_TRUE(ids != NULL);
    f->ids = *ids;
    f->bi = de_builtin_components(f->w);
    ASSERT_TRUE(DE_HANDLE_VALID(f->bi.global_transform));
    ASSERT_TRUE(de__rw_of(f->r) != NULL);
    rwt_set_cvar("r.pipelined", pipelined ? "1" : "0");
    rwt_set_cvar("r.origin_grid_m", "1024");
}

void rwt_stop(rwt *f) {
    de_renderer_destroy(f->r); /* first: it removes its Extract systems from the world */
    f->r = NULL;
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->d != NULL)
        de_gpu_device_destroy(f->d);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    f->w = NULL;
    f->d = NULL;
    f->types = NULL;
    f->tasks = NULL;
    rwt_set_cvar("r.pipelined", "1");
    rwt_set_cvar("r.origin_grid_m", "1024");
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).bytes_live, 0u);
}

de_result rwt_frame(rwt *f) {
    de_result r = de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f);
    if (r == DE_OK)
        r = de_world_run_schedule(f->w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f);
    if (r == DE_OK)
        r = de__render_extract_and_kick(f->r, f->w);
    return r;
}

const de__render_world_frame *rwt_last(rwt *f) {
    de__render_world_wait_idle(f->r);
    return de__render_world_last(f->r);
}

static de_transform xf_at(f64 x, f64 y, f64 z) {
    de_transform t = de_transform_identity();
    t.position = (de_dvec3){x, y, z};
    return t;
}

de_entity rwt_spawn_mesh(rwt *f, de_transform t, u32 mesh_id) {
    const de_component comps[2] = {f->bi.transform, f->ids.mesh_renderer};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(f->w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    *(DeTransform *)de_entity_get(f->w, e, f->bi.transform) = t;
    DeMeshRenderer *m = (DeMeshRenderer *)de_entity_get(f->w, e, f->ids.mesh_renderer);
    m->mesh.id = mesh_id;
    m->materials[0].id = mesh_id + 1000u;
    return e;
}

de_entity rwt_spawn_light(rwt *f, de_transform t, de_light_kind kind) {
    const de_component comps[2] = {f->bi.transform, f->ids.light};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(f->w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    *(DeTransform *)de_entity_get(f->w, e, f->bi.transform) = t;
    DeLight *l = (DeLight *)de_entity_get(f->w, e, f->ids.light);
    l->kind = kind;
    return e;
}

de_entity rwt_spawn_camera(rwt *f, de_transform t, u32 view_id) {
    const de_component comps[2] = {f->bi.transform, f->ids.camera};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(f->w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    *(DeTransform *)de_entity_get(f->w, e, f->bi.transform) = t;
    ((DeCamera *)de_entity_get(f->w, e, f->ids.camera))->view.id = view_id;
    return e;
}

void rwt_move(rwt *f, de_entity e, de_dvec3 pos) {
    ((DeTransform *)de_entity_get(f->w, e, f->bi.transform))->position = pos;
}

/* The slot of `e` in `fr`, or DE__RW_NONE. */
u32 rwt_inst_slot(const de__render_world_frame *fr, de_entity e) {
    for (u32 s = 0; s < fr->instances.count; ++s)
        if (fr->instances.entity[s] == e.bits)
            return s;
    return DE__RW_NONE;
}

static u32 light_slot(const de__render_world_frame *fr, de_entity e) {
    for (u32 s = 0; s < fr->lights.count; ++s)
        if (fr->lights.entity[s] == e.bits)
            return s;
    return DE__RW_NONE;
}

/* ---- Digest (field by field: padding never counts) ------------------------------------------ */

static u64 h_u64(u64 h, u64 v) {
    for (u32 i = 0; i < 8; ++i) {
        h ^= (v >> (8u * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

static u64 h_f32(u64 h, f32 v) {
    u32 b;
    memcpy(&b, &v, sizeof b);
    return h_u64(h, b);
}

static u64 h_f64(u64 h, f64 v) {
    u64 b;
    memcpy(&b, &v, sizeof b);
    return h_u64(h, b);
}

static u64 h_affine(u64 h, const de_affine *a) {
    for (u32 i = 0; i < 12; ++i)
        h = h_f32(h, a->m[i]);
    return h;
}

static u64 h_v3(u64 h, de_vec3 v) {
    return h_f32(h_f32(h_f32(h, v.x), v.y), v.z);
}

static u64 h_list(u64 h, const u32 *l, u32 n) {
    h = h_u64(h, n);
    for (u32 i = 0; i < n; ++i)
        h = h_u64(h, l[i]);
    return h;
}

u64 rwt_digest(const de__render_world_frame *f) {
    u64 h = 0xCBF29CE484222325ull;
    h = h_u64(h, f->frame);
    h = h_f64(h_f64(h_f64(h, f->origin.x), f->origin.y), f->origin.z);
    h = h_u64(h, f->origin_changed);
    h = h_u64(h_u64(h, f->view_count), f->main_view);
    for (u32 i = 0; i < f->view_count; ++i) {
        const de__rw_view *v = &f->views[i];
        h = h_u64(h, v->entity);
        h = h_u64(h, (u64)v->camera.projection);
        h = h_f32(h_f32(h_f32(h, v->camera.fov_y), v->camera.ortho_height), v->camera.near_z);
        h = h_u64(h_u64(h, v->camera.view.id), v->camera.tonemapper.id);
        h = h_f32(h, v->camera.exposure_ev100);
        for (u32 k = 0; k < 8; ++k)
            h = h_f32(h, v->camera.tonemap_params[k]);
        h = h_v3(h, v->offset);
        h = h_f32(h_f32(h_f32(h_f32(h, v->rotation.x), v->rotation.y), v->rotation.z),
                  v->rotation.w);
        h = h_affine(h, &v->transform);
    }
    const de__rw_instances *in = &f->instances;
    h = h_u64(h_u64(h, in->count), in->live);
    for (u32 s = 0; s < in->count; ++s) {
        h = h_u64(h, in->entity[s]);
        h = h_u64(h, in->mesh[s].id);
        for (u32 k = 0; k < DE__RW_MATERIALS; ++k)
            h = h_u64(h, in->materials[s * DE__RW_MATERIALS + k].id);
        h = h_affine(h_affine(h, &in->transform[s]), &in->prev_transform[s]);
        h = h_v3(h_v3(h, in->bounds[s].min), in->bounds[s].max);
        h = h_u64(h_u64(h, in->layer_mask[s]), in->flags[s]);
    }
    const de__rw_lights *li = &f->lights;
    h = h_u64(h_u64(h, li->count), li->live);
    for (u32 s = 0; s < li->count; ++s) {
        const DeLight *l = &li->light[s];
        h = h_u64(h, li->entity[s]);
        h = h_affine(h, &li->transform[s]);
        h = h_u64(h, (u64)l->kind);
        h = h_v3(h, l->color);
        h = h_f32(h_f32(h, l->intensity), l->range);
        h = h_f32(h_f32(h, l->spot_inner), l->spot_outer);
        h = h_f32(h_f32(h, l->area_size.x), l->area_size.y);
        h = h_f32(h, l->source_radius);
        h = h_u64(h, l->cast_shadows);
    }
    h = h_u64(h, f->environment.entity);
    h = h_u64(h, f->environment.environment.hdri.id);
    h = h_f32(h_f32(h, f->environment.environment.intensity), f->environment.environment.rotation);
    h = h_u64(h, f->environment_count);
    h = h_list(h, f->dirty_instances, f->dirty_instance_count);
    h = h_list(h, f->removed_instances, f->removed_instance_count);
    h = h_list(h, f->dirty_lights, f->dirty_light_count);
    h = h_list(h, f->removed_lights, f->removed_light_count);
    return h;
}

/* ---- Tests ----------------------------------------------------------------------------------- */

static void rwt_setup(rwt *f) {
    (void)f;
}

static void rwt_teardown(rwt *f) {
    if (f->r != NULL || f->w != NULL)
        rwt_stop(f);
}

#define FAR_X 1.0e6
#define FAR_Y (-1.0e6)
#define FAR_Z 50.0

TEST_F(extract, camera_relative_precision, rwt) {
    rwt_start(f, 0, false);
    (void)rwt_spawn_camera(f, xf_at(FAR_X + 3.25, FAR_Y + 7.5, FAR_Z + 2.0), 0);
    enum { N = 64 };
    de_entity e[N];
    de_transform t[N];
    u32 seed = 12345u;
    for (u32 i = 0; i < N; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const f64 dx = (f64)(seed % 1000000u) * 1e-3 - 500.0; /* +-500 m, 1 mm steps */
        seed = seed * 1664525u + 1013904223u;
        const f64 dy = (f64)(seed % 1000000u) * 1e-3 - 500.0;
        t[i] = xf_at(FAR_X + dx + 0.0001 * i, FAR_Y + dy, FAR_Z + 0.25 * (f64)(i % 8));
        if (i % 3 == 0) { /* some rotated and scaled ones */
            t[i].rotation = (de_quat){0.0f, 0.0f, 0.38268343f, 0.92387953f}; /* 45 deg about z */
            t[i].scale = (de_vec3){2.0f, 2.0f, 2.0f};
        }
        e[i] = rwt_spawn_mesh(f, t[i], i + 1u);
        ASSERT_TRUE(e[i].bits != 0);
    }
    ASSERT_EQ(rwt_frame(f), DE_OK);
    const de__render_world_frame *fr = rwt_last(f);
    ASSERT_TRUE(fr != NULL);
    /* The grid point nearest the camera: round(1e6 / 1024) = 977 -> 1000448. */
    EXPECT_FLOAT_EQ(fr->origin.x, 1000448.0, 0.0);
    EXPECT_FLOAT_EQ(fr->origin.y, -1000448.0, 0.0);
    EXPECT_FLOAT_EQ(fr->origin.z, 0.0, 0.0);
    EXPECT_TRUE(fr->origin_changed); /* the first frame */
    ASSERT_EQ(fr->view_count, 1u);
    ASSERT_EQ(fr->main_view, 0u);
    EXPECT_FLOAT_EQ(fr->views[0].offset.x, FAR_X + 3.25 - 1000448.0, 1e-3);
    EXPECT_FLOAT_EQ(fr->views[0].offset.y, FAR_Y + 7.5 + 1000448.0, 1e-3);
    EXPECT_FLOAT_EQ(fr->views[0].offset.z, FAR_Z + 2.0, 1e-3);
    ASSERT_EQ(fr->instances.live, (u32)N);
    f64 worst = 0.0, worst_naive = 0.0;
    for (u32 i = 0; i < N; ++i) {
        const u32 s = rwt_inst_slot(fr, e[i]);
        ASSERT_NE(s, DE__RW_NONE);
        const de_affine *a = &fr->instances.transform[s];
        /* origin + f32 translation, in f64, against the f64 world position. */
        const f64 ex = fabs(fr->origin.x + (f64)a->m[9] - t[i].position.x);
        const f64 ey = fabs(fr->origin.y + (f64)a->m[10] - t[i].position.y);
        const f64 ez = fabs(fr->origin.z + (f64)a->m[11] - t[i].position.z);
        const f64 err = ex > ey ? (ex > ez ? ex : ez) : (ey > ez ? ey : ez);
        worst = err > worst ? err : worst;
        const de_vec3 naive = de_dv3_to_v3(t[i].position);
        const f64 nerr = fabs((f64)naive.x - t[i].position.x);
        worst_naive = nerr > worst_naive ? nerr : worst_naive;
        /* The linear part is the f32 rotation and scale, unchanged by the origin. */
        const de_affine lin = de_affine_from_trs((de_vec3){0, 0, 0}, t[i].rotation, t[i].scale);
        for (u32 k = 0; k < 9; ++k)
            EXPECT_FLOAT_EQ(a->m[k], lin.m[k], 1e-6);
        /* A new instance: no motion. */
        for (u32 k = 0; k < 12; ++k)
            EXPECT_FLOAT_EQ(fr->instances.prev_transform[s].m[k], a->m[k], 0.0);
        EXPECT_TRUE(de_aabb_is_empty(fr->instances.bounds[s])); /* no asset system */
        EXPECT_EQ(fr->instances.mesh[s].id, i + 1u);
        EXPECT_EQ(fr->instances.materials[s * DE__RW_MATERIALS].id, i + 1001u);
    }
    printf("worst error relative to the origin: %.3g m (plain f32 world position: %.3g m)\n", worst,
           worst_naive);
    EXPECT_TRUE(worst < 1e-3);
    EXPECT_TRUE(worst_naive > 1e-3); /* the case the origin exists for */
}

TEST_F(extract, origin_rebase, rwt) {
    rwt_start(f, 0, false);
    const de_entity cam = rwt_spawn_camera(f, xf_at(10.0, 10.0, 0.0), 0);
    (void)rwt_spawn_camera(f, xf_at(5000.0, 0.0, 0.0), 7); /* not the main camera: view != 0 */
    const de_entity a = rwt_spawn_mesh(f, xf_at(600.0, 0.0, 0.0), 1);
    const de_entity l = rwt_spawn_light(f, xf_at(700.0, 20.0, 5.0), DE_LIGHT_POINT);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    const de__render_world_frame *fr = rwt_last(f);
    EXPECT_TRUE(fr->origin_changed);
    EXPECT_FLOAT_EQ(fr->origin.x, 0.0, 0.0);
    ASSERT_EQ(fr->view_count, 2u);
    EXPECT_EQ(fr->views[fr->main_view].entity, cam.bits);

    /* A frame without a move: no rebase. */
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_FALSE(fr->origin_changed);
    EXPECT_EQ(fr->dirty_instance_count, 0u);

    /* Inside the cell (|x| <= 512): the origin stays. */
    rwt_move(f, cam, (de_dvec3){511.0, -300.0, 100.0});
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_FALSE(fr->origin_changed);
    EXPECT_FLOAT_EQ(fr->origin.x, 0.0, 0.0);
    EXPECT_FLOAT_EQ(fr->views[fr->main_view].offset.x, 511.0, 1e-4);

    /* Out of the cell: rebase to the grid point nearest the camera. */
    rwt_move(f, cam, (de_dvec3){513.0, -300.0, 100.0});
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_TRUE(fr->origin_changed);
    EXPECT_FLOAT_EQ(fr->origin.x, 1024.0, 0.0);
    EXPECT_FLOAT_EQ(fr->origin.y, 0.0, 0.0);
    EXPECT_FLOAT_EQ(fr->views[fr->main_view].offset.x, 513.0 - 1024.0, 1e-4);
    u32 s = rwt_inst_slot(fr, a);
    ASSERT_NE(s, DE__RW_NONE);
    EXPECT_FLOAT_EQ(fr->instances.transform[s].m[9], 600.0 - 1024.0, 1e-4);
    EXPECT_FLOAT_EQ(fr->instances.prev_transform[s].m[9], 600.0 - 1024.0, 1e-4); /* static */
    const u32 ls = light_slot(fr, l);
    ASSERT_NE(ls, DE__RW_NONE);
    EXPECT_FLOAT_EQ(fr->lights.transform[ls].m[9], 700.0 - 1024.0, 1e-4);
    EXPECT_FLOAT_EQ(fr->lights.transform[ls].m[10], 20.0, 1e-4);

    /* The next frame: settled. */
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_FALSE(fr->origin_changed);
    EXPECT_FLOAT_EQ(fr->origin.x, 1024.0, 0.0);
    s = rwt_inst_slot(fr, a);
    EXPECT_FLOAT_EQ(fr->instances.transform[s].m[9], 600.0 - 1024.0, 1e-4);

    /* A different grid rebases at once. */
    rwt_set_cvar("r.origin_grid_m", "100");
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_TRUE(fr->origin_changed);
    EXPECT_FLOAT_EQ(fr->origin.x, 500.0, 0.0); /* round(513 / 100) * 100 */
    EXPECT_FLOAT_EQ(fr->origin.y, -300.0, 0.0);
    EXPECT_FLOAT_EQ(fr->origin.z, 100.0, 0.0);
    s = rwt_inst_slot(fr, a);
    EXPECT_FLOAT_EQ(fr->instances.transform[s].m[9], 100.0, 1e-4);
    EXPECT_FLOAT_EQ(fr->instances.transform[s].m[10], 300.0, 1e-4);
    EXPECT_FLOAT_EQ(fr->instances.transform[s].m[11], -100.0, 1e-4);
}

#define MANY 10000u

TEST_F(extract, change_list_single_dirty, rwt) {
    rwt_start(f, 0, false);
    (void)rwt_spawn_camera(f, xf_at(0.0, 0.0, 0.0), 0);
    static de_entity e[MANY];
    for (u32 i = 0; i < MANY; ++i) {
        e[i] = rwt_spawn_mesh(f, xf_at((f64)(i % 100) * 2.0, (f64)(i / 100) * 2.0, 0.0), i + 1u);
        ASSERT_TRUE(e[i].bits != 0);
    }
    de_entity lights[10];
    for (u32 i = 0; i < 10; ++i)
        lights[i] = rwt_spawn_light(f, xf_at((f64)i, 0.0, 3.0), DE_LIGHT_SPOT);

    ASSERT_EQ(rwt_frame(f), DE_OK);
    const de__render_world_frame *fr = rwt_last(f);
    EXPECT_EQ(fr->instances.live, MANY);
    EXPECT_EQ(fr->dirty_instance_count, MANY);
    EXPECT_EQ(fr->dirty_light_count, 10u);
    for (u32 i = 1; i < fr->dirty_instance_count; ++i)
        ASSERT_LT(fr->dirty_instances[i - 1], fr->dirty_instances[i]); /* ascending */

    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    EXPECT_EQ(fr->dirty_instance_count, 0u);
    EXPECT_EQ(fr->removed_instance_count, 0u);
    EXPECT_EQ(fr->dirty_light_count, 0u);

    /* One DeMeshRenderer modified: exactly one dirty entry, with the new data. */
    DeMeshRenderer *m = (DeMeshRenderer *)de_entity_get(f->w, e[4321], f->ids.mesh_renderer);
    m->layer_mask = 0x5u;
    m->cast_shadows = false;
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    ASSERT_EQ(fr->dirty_instance_count, 1u);
    const u32 s = rwt_inst_slot(fr, e[4321]);
    EXPECT_EQ(fr->dirty_instances[0], s);
    EXPECT_EQ(fr->instances.layer_mask[s], 0x5u);
    EXPECT_EQ(fr->instances.flags[s] & DE__RW_INST_CAST_SHADOWS, 0u);
    EXPECT_EQ(fr->instances.flags[s] & DE__RW_INST_VISIBLE_IN_RT, (u32)DE__RW_INST_VISIBLE_IN_RT);
    EXPECT_EQ(fr->dirty_light_count, 0u);

    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(rwt_last(f)->dirty_instance_count, 0u);

    /* One moved: dirty in its frame (prev = the old transform) and the next (prev catches up). */
    rwt_move(f, e[77], (de_dvec3){1000.0, 1000.0, 1.0});
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    const u32 s77 = rwt_inst_slot(fr, e[77]);
    ASSERT_EQ(fr->dirty_instance_count, 1u);
    EXPECT_EQ(fr->dirty_instances[0], s77);
    EXPECT_FLOAT_EQ(fr->instances.transform[s77].m[9], 1000.0, 1e-4);
    EXPECT_FLOAT_EQ(fr->instances.prev_transform[s77].m[9], 154.0, 1e-4); /* (77 % 100) * 2 */
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    ASSERT_EQ(fr->dirty_instance_count, 1u);
    EXPECT_EQ(fr->dirty_instances[0], s77);
    EXPECT_FLOAT_EQ(fr->instances.prev_transform[s77].m[9], 1000.0, 1e-4);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(rwt_last(f)->dirty_instance_count, 0u);

    /* One light modified: one dirty light. */
    ((DeLight *)de_entity_get(f->w, lights[3], f->ids.light))->intensity = 42.0f;
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    ASSERT_EQ(fr->dirty_light_count, 1u);
    EXPECT_EQ(fr->dirty_lights[0], light_slot(fr, lights[3]));
    EXPECT_FLOAT_EQ(fr->lights.light[fr->dirty_lights[0]].intensity, 42.0, 0.0);
    EXPECT_EQ(fr->dirty_instance_count, 0u);
    /* Stats of the last kick. */
    EXPECT_EQ(de_renderer_stats(f->r)->extract_instances, MANY);
    EXPECT_EQ(de_renderer_stats(f->r)->extract_lights, 10u);
    EXPECT_TRUE(de_renderer_stats(f->r)->extract_cpu_ms >= 0.0f);
}

TEST_F(extract, despawn_removal, rwt) {
    rwt_start(f, 0, false);
    static de_entity e[MANY];
    for (u32 i = 0; i < MANY; ++i)
        e[i] = rwt_spawn_mesh(f, xf_at((f64)i, 0.0, 0.0), i + 1u);
    const de_entity l0 = rwt_spawn_light(f, xf_at(0.0, 0.0, 0.0), DE_LIGHT_POINT);
    const de_entity l1 = rwt_spawn_light(f, xf_at(1.0, 0.0, 0.0), DE_LIGHT_POINT);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    const de__render_world_frame *fr = rwt_last(f);
    const u32 slot = rwt_inst_slot(fr, e[500]);
    ASSERT_NE(slot, DE__RW_NONE);

    /* A despawn: one removal, no dirty entry. */
    de_entity_despawn(f->w, e[500]);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    ASSERT_EQ(fr->removed_instance_count, 1u);
    EXPECT_EQ(fr->removed_instances[0], slot);
    EXPECT_EQ(fr->dirty_instance_count, 0u);
    EXPECT_EQ(fr->instances.live, MANY - 1u);
    EXPECT_EQ(fr->instances.entity[slot], 0u);
    EXPECT_EQ(rwt_inst_slot(fr, e[500]), DE__RW_NONE);

    ASSERT_EQ(rwt_frame(f), DE_OK);
    EXPECT_EQ(rwt_last(f)->removed_instance_count, 0u);

    /* A new entity takes the freed slot (last freed, first reused). */
    const de_entity n = rwt_spawn_mesh(f, xf_at(-5.0, 0.0, 0.0), 99999u);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    ASSERT_EQ(fr->dirty_instance_count, 1u);
    EXPECT_EQ(fr->dirty_instances[0], slot);
    EXPECT_EQ(fr->instances.entity[slot], n.bits);
    EXPECT_EQ(fr->instances.mesh[slot].id, 99999u);
    EXPECT_EQ(fr->removed_instance_count, 0u);

    /* Removing the component is a removal too. */
    const u32 s9 = rwt_inst_slot(fr, e[9]);
    de_entity_remove(f->w, e[9], f->ids.mesh_renderer);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    ASSERT_EQ(fr->removed_instance_count, 1u);
    EXPECT_EQ(fr->removed_instances[0], s9);
    EXPECT_EQ(fr->instances.live, MANY - 1u);

    /* Lights too. */
    const u32 ls = light_slot(fr, l1);
    de_entity_despawn(f->w, l1);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    ASSERT_EQ(fr->removed_light_count, 1u);
    EXPECT_EQ(fr->removed_lights[0], ls);
    EXPECT_EQ(fr->lights.live, 1u);
    EXPECT_NE(light_slot(fr, l0), DE__RW_NONE);
    EXPECT_EQ(fr->removed_instance_count, 0u);
}

/* A stable scene: no heap allocation per frame once warm (unpipelined, then pipelined). */
static void run_no_heap(rwt *f, bool pipelined) {
    rwt_start(f, 0, pipelined);
    const de_entity cam = rwt_spawn_camera(f, xf_at(0.0, 0.0, 0.0), 0);
    enum { STATIC = 2000, MOVING = 64, LIGHTS = 16 };
    for (u32 i = 0; i < STATIC; ++i)
        (void)rwt_spawn_mesh(f, xf_at((f64)i, 1.0, 0.0), i + 1u);
    de_entity moving[MOVING];
    for (u32 i = 0; i < MOVING; ++i)
        moving[i] = rwt_spawn_mesh(f, xf_at((f64)i, 2.0, 0.0), i + 1u);
    for (u32 i = 0; i < LIGHTS; ++i)
        (void)rwt_spawn_light(f, xf_at((f64)i, 3.0, 0.0), DE_LIGHT_POINT);
    u32 frame = 0;
    /* Moves, and a camera that crosses cells now and then (rebases). */
    for (u32 i = 0; i < 20; ++i, ++frame) {
        for (u32 k = 0; k < MOVING; ++k)
            rwt_move(f, moving[k], (de_dvec3){(f64)k + 0.01 * frame, 2.0, 0.0});
        rwt_move(f, cam, (de_dvec3){300.0 * frame, 0.0, 0.0});
        ASSERT_EQ(rwt_frame(f), DE_OK);
    }
    de__render_world_wait_idle(f->r);
    de_alloc_guard guard = {0};
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_alloc_guard_begin(&guard);
    de_result res = DE_OK;
    for (u32 i = 0; i < 100 && res == DE_OK; ++i, ++frame) {
        for (u32 k = 0; k < MOVING; ++k)
            rwt_move(f, moving[k], (de_dvec3){(f64)k + 0.01 * frame, 2.0, 0.0});
        rwt_move(f, cam, (de_dvec3){300.0 * frame, 0.0, 0.0});
        res = rwt_frame(f);
    }
    de__render_world_wait_idle(f->r);
    de_alloc_guard_end(&guard);
    ASSERT_EQ(res, DE_OK);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    const de__render_world_frame *fr = rwt_last(f);
    EXPECT_EQ(fr->instances.live, (u32)(STATIC + MOVING));
    EXPECT_EQ(fr->dirty_instance_count, (u32)MOVING);
    EXPECT_EQ(fr->lights.live, (u32)LIGHTS);
    if (pipelined)
        EXPECT_GE(de__render_world_rendered(f->r), 120u);
    rwt_stop(f);
}

TEST_F(extract, no_heap_after_warmup, rwt) {
    run_no_heap(f, false);
    run_no_heap(f, true);
}
