/*
 * clusters.c: the froxel clusters and light lists of one view (SPEC §12.5 step 5; DENG-303).
 * shaders/lighting/cluster_build.slang (engine shader `lighting_clusters`) builds the bounds,
 * light_cull.slang (`lighting_cull`) the lists. lighting_pass.c calls de__clusters_build per view
 * and frame, after it wrote the frame's lighting constants:
 *
 *   lighting.cluster_bounds  the cluster AABBs (view space)                   bounds: WRITE
 *   lighting.lights_clear    the light counters and the tile counters to 0      counters: WRITE
 *   lighting.lights_prep     GPU-scene lights and emissive proxies -> the       scene: READ;
 *                            local and directional lists                        lists: WRITE
 *   lighting.lights_count    lights per cluster (capped)                        grid: WRITE
 *   lighting.lights_scan     the offsets (one 1024-thread group)                grid: READ + WRITE
 *   lighting.lights_fill     the index list                                     list: WRITE
 *
 * Every buffer is a transient of the view's graph, sized from the grid and the light table's
 * slots, so the graph culls all of it when nothing reads the lists, and a steady frame allocates
 * nothing. The bounds are rebuilt each frame: the projection jitters every frame with temporal AA,
 * and the pass is one thread per cluster. Nothing here reads the lists back: lighting_pass.c's
 * readback copies the counters.
 */
#include "render/lighting/lighting.h"

#include <stdio.h>
#include <string.h>

/** light_cull.slang CullParams (64 bytes). */
typedef struct cl_cull_params {
    u32 view, entry, scene, view_slot;
    u32 bounds, lights, dir_lights, grid;
    u32 list, counters, clear_bytes, reserved1;
    u32 contact_records, hidden_entities, hidden_count, padding;
} cl_cull_params;
DE_STATIC_ASSERT(sizeof(cl_cull_params) == 64, "CullParams is 64 bytes");

/** cluster_build.slang ClusterBuildParams (32 bytes). */
typedef struct cl_build_params {
    u32 view, entry, bounds, width, height, reserved0, reserved1, reserved2;
} cl_build_params;
DE_STATIC_ASSERT(sizeof(cl_build_params) == 32, "ClusterBuildParams is 32 bytes");

/** One pass's data: the resources it declared (others invalid) and what it dispatches. */
typedef struct cl_pass {
    de_gpu_device *dev;
    const de__vis_kernel *kernel;
    cl_cull_params pc;
    u32 groups;
    de_rg_resource bounds, lights, dir_lights, grid, list, counters, tile_counters;
    de_rg_resource atlas_records, contact_records;
    u32 width, height; /* cluster_build */
} cl_pass;

static u32 buf_index(const cl_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r)) : 0u;
}

static cl_cull_params cull_params(const cl_pass *p, const de_rg_pass *pass) {
    cl_cull_params pc = p->pc;
    pc.bounds = buf_index(p, pass, p->bounds);
    pc.lights = buf_index(p, pass, p->lights);
    pc.dir_lights = buf_index(p, pass, p->dir_lights);
    pc.grid = buf_index(p, pass, p->grid);
    pc.list = buf_index(p, pass, p->list);
    pc.counters = buf_index(p, pass, p->counters);
    pc.reserved1 = buf_index(p, pass, p->atlas_records);
    pc.contact_records = buf_index(p, pass, p->contact_records);
    return pc;
}

static void build_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const cl_pass *p = (const cl_pass *)user;
    const cl_build_params pc = {.view = p->pc.view,
                                .entry = p->pc.entry,
                                .bounds = buf_index(p, pass, p->bounds),
                                .width = p->width,
                                .height = p->height};
    de_gpu_cmd_set_pipeline(cmd, p->kernel->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, p->groups, 1, 1);
}

static void clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const cl_pass *p = (const cl_pass *)user;
    cl_cull_params pc = cull_params(p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->kernel->pipeline);
    pc.clear_bytes = DE__LIGHTING_COUNTER_BYTES;
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, 1, 1, 1);
    if (DE_HANDLE_VALID(p->tile_counters)) {
        pc.counters = buf_index(p, pass, p->tile_counters);
        pc.clear_bytes = DE__LIGHTING_TILE_COUNTER_BYTES;
        de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
        de_gpu_cmd_dispatch(cmd, (DE__LIGHTING_TILE_COUNTER_BYTES / 4u + 63u) / 64u, 1, 1);
    }
}

static void cull_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const cl_pass *p = (const cl_pass *)user;
    const cl_cull_params pc = cull_params(p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->kernel->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, p->groups, 1, 1);
}

static cl_pass *pass_data(de__vis_context *vc, const cl_pass *base) {
    cl_pass *p = (cl_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL)
        *p = *base;
    else
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "lighting: clusters: the frame arena is full");
    return p;
}

static u32 groups_of(u32 n, u32 size) {
    const u32 g = (n + size - 1u) / size;
    return g > 0 ? g : 1u;
}

bool de__clusters_build(de__lighting *lt, de_rg *g, de__vis_context *vc,
                        de__lighting_view_state *vs, de__lighting_context *lc) {
    const de__cluster_grid *grid = &lc->grid;
    const u32 slot = vc->view_slot;
    lc->bounds = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = (u64)grid->count * 32u,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC, /* tests */
                                 .name = "lighting.cluster_bounds"});
    lc->lights = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = (u64)lc->local_capacity * sizeof(de__cluster_light),
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                 .stride = (u32)sizeof(de__cluster_light),
                                 .name = "lighting.lights"});
    lc->dir_lights = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = DE__LIGHTING_MAX_DIRECTIONAL * sizeof(de__cluster_light),
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                 .stride = (u32)sizeof(de__cluster_light),
                                 .name = "lighting.dir_lights"});
    lc->grid_buf =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = (u64)grid->count * 8u,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .stride = 8u,
                                                     .name = "lighting.grid"});
    lc->list =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = (u64)lc->list_capacity * 4u,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .stride = 4u,
                                                     .name = "lighting.list"});
    lc->counters =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = DE__LIGHTING_COUNTER_BYTES,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "lighting.counters"});
    lc->tile_counters =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = DE__LIGHTING_TILE_COUNTER_BYTES,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "lighting.tile_counters"});
    const de_rg_resource scene_lights = vc->gpu_scene != NULL
                                            ? de__gpu_scene_import(vc->gpu_scene, g, DE__GS_LIGHTS)
                                            : (de_rg_resource){0};
    if (!DE_HANDLE_VALID(lc->bounds) || !DE_HANDLE_VALID(lc->lights) ||
        !DE_HANDLE_VALID(lc->dir_lights) || !DE_HANDLE_VALID(lc->grid_buf) ||
        !DE_HANDLE_VALID(lc->list) || !DE_HANDLE_VALID(lc->counters) ||
        !DE_HANDLE_VALID(lc->tile_counters))
        return false; /* error set; finalize reports it */

    const cl_pass base = {.dev = lt->dev,
                          .pc = {.view = vs->constants_bindless,
                                 .entry = vs->entry,
                                 .scene = vc->scene_views_bindless,
                                 .view_slot = slot,
                                 .hidden_entities = vc->hidden_bindless,
                                 .hidden_count = vc->hidden_count},
                          .width = grid->width,
                          .height = grid->height};

    /* The bounds. */
    {
        cl_pass b = base;
        b.kernel = &lt->cluster_build;
        b.bounds = lc->bounds;
        b.groups = groups_of(grid->count, 64u);
        cl_pass *pd = pass_data(vc, &b);
        if (pd == NULL)
            return false;
        de_rg_pass *p =
            de_rg_add_pass(g, "lighting.cluster_bounds", DE_QUEUE_GRAPHICS, build_execute, pd);
        de_rg_write(p, lc->bounds, DE_ACCESS_SHADER_WRITE);
    }

    cl_pass b_clear = base, b_prep = base, b_count = base, b_scan = base, b_fill = base;
    b_clear.kernel = &lt->light_clear;
    b_clear.counters = lc->counters;
    b_clear.tile_counters = lc->tile_counters;
    b_prep.kernel = &lt->light_prep;
    b_prep.atlas_records = lc->atlas_records;
    b_prep.contact_records = lc->contact_records;
    b_prep.lights = lc->lights;
    b_prep.dir_lights = lc->dir_lights;
    b_prep.counters = lc->counters;
    b_prep.groups = groups_of(lc->view.light_slots + lc->view.instance_slots, 64u);
    b_count.kernel = &lt->light_count;
    b_count.bounds = lc->bounds;
    b_count.lights = lc->lights;
    b_count.grid = lc->grid_buf;
    b_count.counters = lc->counters;
    b_count.groups = groups_of(grid->count, 64u);
    b_scan.kernel = &lt->light_scan;
    b_scan.grid = lc->grid_buf;
    b_scan.counters = lc->counters;
    b_scan.groups = 1u;
    b_fill = b_count;
    b_fill.kernel = &lt->light_fill;
    b_fill.list = lc->list;
    cl_pass *pc = pass_data(vc, &b_clear), *pp = pass_data(vc, &b_prep),
            *pn = pass_data(vc, &b_count), *ps = pass_data(vc, &b_scan),
            *pf = pass_data(vc, &b_fill);
    if (pc == NULL || pp == NULL || pn == NULL || ps == NULL || pf == NULL)
        return false;

    de_rg_pass *p =
        de_rg_add_pass(g, "lighting.lights_clear", DE_QUEUE_GRAPHICS, clear_execute, pc);
    de_rg_write(p, lc->counters, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, lc->tile_counters, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, "lighting.lights_prep", DE_QUEUE_GRAPHICS, cull_execute, pp);
    if (vc->hidden_entities.id)
        de_rg_read(p, vc->hidden_entities, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_views))
        de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(scene_lights))
        de_rg_read(p, scene_lights, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_instances))
        de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_materials))
        de_rg_read(p, vc->scene_materials, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(lc->atlas_records))
        de_rg_read(p, lc->atlas_records, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(lc->contact_records))
        de_rg_read(p, lc->contact_records, DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->counters, DE_ACCESS_SHADER_READ); /* atomics */
    de_rg_write(p, lc->counters, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, lc->lights, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, lc->dir_lights, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, "lighting.lights_count", DE_QUEUE_GRAPHICS, cull_execute, pn);
    de_rg_read(p, lc->bounds, DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->lights, DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, lc->counters, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, lc->grid_buf, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, "lighting.lights_scan", DE_QUEUE_GRAPHICS, cull_execute, ps);
    de_rg_read(p, lc->grid_buf, DE_ACCESS_SHADER_READ);
    de_rg_write(p, lc->grid_buf, DE_ACCESS_SHADER_WRITE);
    de_rg_read(p, lc->counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, lc->counters, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, "lighting.lights_fill", DE_QUEUE_GRAPHICS, cull_execute, pf);
    de_rg_read(p, lc->bounds, DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->lights, DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->grid_buf, DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, lc->list, DE_ACCESS_SHADER_WRITE);
    return true;
}

/* DENG-340: probe lists use exactly the S14 froxel bounds and grid math. CPU writes a warm
 * upload ring; GPU intersects probes and constructs fixed-capacity index lists. */
void de__probe_cluster_bounds(const de__cluster_grid *grid, const de_mat4 *proj, f32 *bounds) {
    f32 inv[4][4];
    de__lighting_inv_proj(proj, inv);
    for (u32 i = 0; i < grid->count; ++i) {
        f32 lo[3], hi[3];
        de__cluster_bounds(grid, &inv[0][0], i, lo, hi);
        memcpy(bounds + (size_t)i * 8, lo, 3 * sizeof(f32));
        memcpy(bounds + (size_t)i * 8 + 4, hi, 3 * sizeof(f32));
        bounds[(size_t)i * 8 + 3] = bounds[(size_t)i * 8 + 7] = 0;
    }
}
