/*
 * classify.c: material classification, the stage after both raster phases (SPEC §12.5 step 3,
 * §12.6; DENG-237). shaders/visbuffer/classify.slang (engine shader `vis_classify`) has the
 * kernels; vis_stages.h the contract and the published fields.
 *
 * Per view and frame, on the graphics queue, when the frame has a vis-ID target:
 *
 *   vis.classify.copy   the vis IDs into a raw buffer      vis_ids: COPY_SRC; buffer: COPY_DST
 *   vis.classify.clear  the list counters to 0             counters: SHADER_WRITE
 *   vis.classify        one group per 64 / tile tiles      ID buffer, visible list, tables: READ;
 *                                                          counters: READ + WRITE; lists, masks:
 *                                                          SHADER_WRITE
 *   vis.classify.args   per-list dispatch arguments        counters: READ; args: SHADER_WRITE
 *
 * Why a buffer copy of the vis IDs: the target (de__vis_raster_targets, DENG-234) has no sampled or
 * storage view. The bindless tables are typed float4 (bindless.slang), and a uint image there fails
 * Vulkan validation (VVL 1.3.275 re-checks every index a shader ever read against what the slot
 * holds now, also after the slot is recycled). The copy (4 bytes per pixel) is read through
 * de_bytes, and resolve (DENG-238) reads the same buffer (de__vis_context.vis_ids_buffer).
 *
 * Every buffer is a transient of the view's graph sized from the view (the lists: one entry per
 * tile per list, so no list can overflow), so a steady frame allocates nothing and a resize makes
 * a new graph key. Nothing reads the outputs until resolve (DENG-238): the graph culls the passes
 * until then. The render thread reads only GPU tables, never asset records.
 */
#include "render/passes/vis/vis_stages.h"

#include <string.h>

/** The kernels' push constants (classify.slang ClassifyParams, 80 bytes). */
typedef struct cl_params {
    u32 ids, pitch, width, height, tile, tiles_x, tile_count, visible, visible_count;
    u32 instances, meshes, materials, material_count, lists, counters, args, masks;
    u32 reserved[3];
} cl_params;
DE_STATIC_ASSERT(sizeof(cl_params) == 80, "ClassifyParams is 80 bytes");

typedef struct cl_state {
    de__vis_kernel clear, classify, args;
    de_cvar cv_tile;
} cl_state;

static const de__vis_layout_const k_consts[] = {
    {"DE_VIS_CLASSIFY_LISTS", DE__VIS_CLASSIFY_LISTS},
    {"DE_VIS_CLASSIFY_BACKGROUND", DE__VIS_CLASSIFY_BACKGROUND},
    {"DE_VIS_CLASSIFY_TILE_BITS", DE__VIS_CLASSIFY_TILE_BITS},
    {"DE_VIS_CLASSIFY_GROUP", DE__VIS_CLASSIFY_GROUP},
};

const de__vis_layout_const *de__vis_classify_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

u32 de__vis_classify_model(const de__gs_material *materials, u32 material_count, u32 slot) {
    if (materials == NULL || slot == DE__GS_NONE || slot >= material_count ||
        (materials[slot].flags & DE__GS_MATERIAL_READY) == 0)
        return DE__GS_MATERIAL_MODEL_STANDARD;
    return materials[slot].model & (DE__GS_MATERIAL_MODELS - 1u);
}

/* =================================================================================================
 * Per renderer
 * ============================================================================================== */

static void state_free(de__vis_shared *s, cl_state *st) {
    de__vis_kernel_destroy(s, &st->clear);
    de__vis_kernel_destroy(s, &st->classify);
    de__vis_kernel_destroy(s, &st->args);
    DE_FREE(s->alloc, st, sizeof *st);
}

de_result de__vis_classify_init(de__vis_shared *s) {
    cl_state *st = DE_NEW(s->alloc, cl_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: classification state");
    memset(st, 0, sizeof *st);
    st->cv_tile = de_cvar_register(&(de_cvar_desc){
        .name = "r.classify_tile",
        .help = "Material classification: tile edge in pixels, 8 or 16 (resolve runs one group "
                "per tile of each material model)",
        .type = DE_CVAR_INT,
        .default_number = 8,
        .min = 8,
        .max = 16});
    de_error_clear();
    static const char *const k_entries[3] = {"cs_classify_clear", "cs_classify",
                                             "cs_classify_args"};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(s, "vis_classify", k_entries, 3, &code);
    if (r == DE_OK) {
        static const u32 group[3] = {DE__VIS_CLASSIFY_GROUP, 1u, 1u};
        const u32 pb = (u32)sizeof(cl_params);
        r = de__vis_kernel_create(s, &code, 0, "vis classify clear", group, pb, &st->clear);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 1, "vis classify", group, pb, &st->classify);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 2, "vis classify args", group, pb, &st->args);
        de__engine_shader_code_free(&code);
    }
    if (r != DE_OK) {
        state_free(s, st);
        return r;
    }
    s->stage[DE__VIS_STAGE_CLASSIFY] = st;
    return DE_OK;
}

void de__vis_classify_shutdown(de__vis_shared *s) {
    cl_state *st = (cl_state *)s->stage[DE__VIS_STAGE_CLASSIFY];
    if (st == NULL)
        return;
    state_free(s, st);
    s->stage[DE__VIS_STAGE_CLASSIFY] = NULL;
}

/* =================================================================================================
 * One view
 * ============================================================================================== */

typedef struct cl_pass {
    de_gpu_device *dev;
    de__vis_kernel kernel;
    cl_params pc;
    u32 groups_x, groups_y;
    /* Only the resources the pass declares (de_rg_buffer needs a declared access). */
    de_rg_resource ids, visible, instances, meshes, materials, lists, counters, args, masks;
    de_rg_resource vis_ids; /* the copy pass */
} cl_pass;

static u32 bindless_of(const cl_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r)) : 0u;
}

static void copy_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const cl_pass *p = (const cl_pass *)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->ids), 0, p->pc.pitch,
                                      de_rg_texture(pass, p->vis_ids), 0, 0);
}

static void kernel_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const cl_pass *p = (const cl_pass *)user;
    cl_params pc = p->pc;
    pc.ids = bindless_of(p, pass, p->ids);
    pc.visible = bindless_of(p, pass, p->visible);
    pc.instances = bindless_of(p, pass, p->instances);
    pc.meshes = bindless_of(p, pass, p->meshes);
    pc.materials = bindless_of(p, pass, p->materials);
    pc.lists = bindless_of(p, pass, p->lists);
    pc.counters = bindless_of(p, pass, p->counters);
    pc.args = bindless_of(p, pass, p->args);
    pc.masks = bindless_of(p, pass, p->masks);
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, p->groups_x, p->groups_y, 1);
}

static cl_pass *pass_data(de__vis_context *vc, const cl_pass *base) {
    cl_pass *p = (cl_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL)
        *p = *base;
    else
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: classification: the frame arena is full");
    return p;
}

void de__vis_classify(de_rg *g, de__vis_context *vc) {
    de__vis_shared *s = vc->shared;
    cl_state *st = (cl_state *)s->stage[DE__VIS_STAGE_CLASSIFY];
    if (st == NULL || !DE_HANDLE_VALID(vc->vis_ids) || !DE_HANDLE_VALID(vc->visible_meshlets) ||
        !DE_HANDLE_VALID(vc->scene_instances) || !DE_HANDLE_VALID(vc->scene_meshes) ||
        vc->width == 0 || vc->height == 0)
        return;
    const i64 tv = DE_HANDLE_VALID(st->cv_tile) ? de_cvar_int(st->cv_tile) : 8;
    const u32 tile = tv >= 16 ? 16u : 8u;
    const u32 tiles_x = (vc->width + tile - 1u) / tile, tiles_y = (vc->height + tile - 1u) / tile;
    const u32 tile_count = tiles_x * tiles_y;
    const u32 per_group = DE__VIS_CLASSIFY_GROUP / tile; /* tiles of a row per classify group */
    const u32 pitch = (vc->width * 4u + 255u) & ~255u;   /* copy_texture_to_buffer's row rule */

    const de_rg_resource ids = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = (u64)pitch * vc->height,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                                 .name = "vis.vis_ids_buffer"});
    const de_rg_resource counters =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = DE__VIS_CLASSIFY_LISTS * 4u,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "vis.classify_counters"});
    const de_rg_resource lists = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_CLASSIFY_LISTS * tile_count * 4u,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                 .name = "vis.classify_tiles"});
    const de_rg_resource args = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = DE__VIS_CLASSIFY_LISTS * 16u,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT | DE_USAGE_COPY_SRC,
                                 .name = "vis.classify_args"});
    const de_rg_resource masks =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = (u64)tile_count * 4u,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "vis.classify_masks"});
    if (!DE_HANDLE_VALID(ids) || !DE_HANDLE_VALID(counters) || !DE_HANDLE_VALID(lists) ||
        !DE_HANDLE_VALID(args) || !DE_HANDLE_VALID(masks))
        return; /* error set; finalize reports it */

    const bool materials = DE_HANDLE_VALID(vc->scene_materials) && vc->scene_material_count > 0;
    const cl_params pc = {.pitch = pitch,
                          .width = vc->width,
                          .height = vc->height,
                          .tile = tile,
                          .tiles_x = tiles_x,
                          .tile_count = tile_count,
                          /* both regions: the alpha bucket's entries (DENG-236) follow the
                           * opaque ones and share the packed IDs' index space */
                          .visible_count = vc->visible_capacity + vc->visible_alpha_capacity,
                          .material_count = materials ? vc->scene_material_count : 0u};
    cl_pass *pcopy =
        pass_data(vc, &(cl_pass){.dev = s->dev, .pc = pc, .ids = ids, .vis_ids = vc->vis_ids});
    cl_pass *pclear = pass_data(vc, &(cl_pass){.dev = s->dev,
                                               .kernel = st->clear,
                                               .pc = pc,
                                               .groups_x = 1,
                                               .groups_y = 1,
                                               .counters = counters});
    cl_pass *pcl =
        pass_data(vc, &(cl_pass){.dev = s->dev,
                                 .kernel = st->classify,
                                 .pc = pc,
                                 .groups_x = (tiles_x + per_group - 1u) / per_group,
                                 .groups_y = tiles_y,
                                 .ids = ids,
                                 .visible = vc->visible_meshlets,
                                 .instances = vc->scene_instances,
                                 .meshes = vc->scene_meshes,
                                 .materials = materials ? vc->scene_materials : (de_rg_resource){0},
                                 .lists = lists,
                                 .counters = counters,
                                 .masks = masks});
    cl_pass *pargs = pass_data(vc, &(cl_pass){.dev = s->dev,
                                              .kernel = st->args,
                                              .pc = pc,
                                              .groups_x = 1,
                                              .groups_y = 1,
                                              .counters = counters,
                                              .args = args});
    if (pcopy == NULL || pclear == NULL || pcl == NULL || pargs == NULL)
        return;

    de_rg_pass *p = de_rg_add_pass(g, "vis.classify.copy", DE_QUEUE_GRAPHICS, copy_execute, pcopy);
    de_rg_read(p, vc->vis_ids, DE_ACCESS_COPY_SRC);
    de_rg_write(p, ids, DE_ACCESS_COPY_DST);

    p = de_rg_add_pass(g, "vis.classify.clear", DE_QUEUE_GRAPHICS, kernel_execute, pclear);
    de_rg_write(p, counters, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, "vis.classify", DE_QUEUE_GRAPHICS, kernel_execute, pcl);
    de_rg_read(p, ids, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    if (materials)
        de_rg_read(p, vc->scene_materials, DE_ACCESS_SHADER_READ);
    de_rg_read(p, counters, DE_ACCESS_SHADER_READ); /* atomics */
    de_rg_write(p, counters, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, lists, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, masks, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, "vis.classify.args", DE_QUEUE_GRAPHICS, kernel_execute, pargs);
    de_rg_read(p, counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, args, DE_ACCESS_SHADER_WRITE);

    vc->vis_ids_buffer = ids;
    vc->vis_ids_pitch = pitch;
    vc->classify_tile = tile;
    vc->classify_tiles_x = tiles_x;
    vc->classify_tiles_y = tiles_y;
    vc->classify_tile_count = tile_count;
    vc->classify_tiles = lists;
    vc->classify_args = args;
    vc->classify_masks = masks;
}
