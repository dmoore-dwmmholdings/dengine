/* Generic depth-only view pool. Public cameras and capture caches keep all their slots. */
#include "render/shadows/shadows_internal.h"
#include "render/graph/rg_internal.h"
#include <string.h>

typedef struct shadow_block {
    struct shadow_block *next;
    de__vis_shared *vis;
    de_gpu_buffer constants;
    u8 *mapped;
    u64 used_frame[DE__VIEW_MAX];
    bool used[DE__VIEW_MAX];
    const de_rg *pending[DE__VIEW_MAX];
    u64 ring_frame[DE__VIEW_MAX][DE__VIS_FRAME_RING];
    bool ring_used[DE__VIEW_MAX][DE__VIS_FRAME_RING];
} shadow_block;
typedef struct shadow_transfer {
    struct shadow_transfer *next;
    de_gpu_buffer buffer;
    u64 bytes;
    u32 pending;
} shadow_transfer;
typedef struct shadow_transfer_use {
    struct shadow_transfer_use *next;
    const de_rg *graph;
    shadow_transfer *transfer;
} shadow_transfer_use;
struct de__shadow_views {
    de_gpu_device *dev;
    const de_allocator *alloc;
    shadow_block *first;
    u32 blocks;
    shadow_transfer *transfer, *transfers;
    shadow_transfer_use *transfer_uses;
};

de_result de__shadow_views_create(de_gpu_device *dev, const de_allocator *alloc,
                                  de__shadow_views **out) {
    if (!dev || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow views: missing device/output");
    *out = NULL;
    if (!alloc)
        alloc = de_heap_allocator();
    de__shadow_views *s = DE_NEW(alloc, de__shadow_views, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow views pool");
    *s = (de__shadow_views){.dev = dev, .alloc = alloc};
    *out = s;
    return DE_OK;
}
void de__shadow_views_destroy(de__shadow_views *s) {
    if (!s)
        return;
    shadow_block *b = s->first;
    while (b) {
        shadow_block *next = b->next;
        de__vis_shared_destroy(b->vis);
        if (DE_HANDLE_VALID(b->constants))
            de_gpu_buffer_destroy(s->dev, b->constants);
        DE_FREE(s->alloc, b, sizeof *b);
        b = next;
    }
    for (shadow_transfer *t = s->transfers; t;) {
        shadow_transfer *next = t->next;
        de_gpu_buffer_destroy(s->dev, t->buffer);
        DE_FREE(s->alloc, t, sizeof *t);
        t = next;
    }
    DE_FREE(s->alloc, s, sizeof *s);
}
u32 de__shadow_views_capacity(const de__shadow_views *s) {
    return s ? s->blocks * DE__VIEW_MAX : 0u;
}
static de_result block_get(de__shadow_views *s, u32 index, shadow_block **out) {
    shadow_block **link = &s->first;
    for (u32 i = 0; i <= index; ++i) {
        if (!*link) {
            shadow_block *b = DE_NEW(s->alloc, shadow_block, DE_MEM_RENDER);
            if (!b)
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow view block");
            memset(b, 0, sizeof *b);
            de_result r = de__vis_shared_create(
                &(de__vis_shared_desc){.device = s->dev, .allocator = s->alloc, .depth_only = true},
                &b->vis);
            if (r != DE_OK) {
                DE_FREE(s->alloc, b, sizeof *b);
                return r;
            }
            b->constants = de_gpu_buffer_create(
                s->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIEW_MAX * DE__VIS_FRAME_RING *
                                                      sizeof(de__gs_view),
                                              .stride = sizeof(de__gs_view),
                                              .usage = DE_USAGE_STORAGE,
                                              .memory = DE_MEM_UPLOAD,
                                              .name = "shadow scene constants"});
            if (!DE_HANDLE_VALID(b->constants) ||
                !(b->mapped = de_gpu_buffer_map(s->dev, b->constants))) {
                de__vis_shared_destroy(b->vis);
                if (DE_HANDLE_VALID(b->constants))
                    de_gpu_buffer_destroy(s->dev, b->constants);
                DE_FREE(s->alloc, b, sizeof *b);
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow scene constants");
            }
            *link = b;
            ++s->blocks;
        }
        if (i == index) {
            *out = *link;
            return DE_OK;
        }
        link = &(*link)->next;
    }
    return DE_FAIL(DE_ERR_INTERNAL, "shadow block traversal");
}
static void rows(de__gs_float4 out[4], const de_mat4 *m) {
    for (u32 r = 0; r < 4; ++r)
        out[r] = (de__gs_float4){.v = {m->m[r], m->m[4 + r], m->m[8 + r], m->m[12 + r]}};
}
typedef struct shadow_clear {
    de_rg_resource target;
    u32 layer;
} shadow_clear;
static void clear_depth(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const shadow_clear *p = user;
    const de_gpu_attachment d = {.texture = de_rg_texture(pass, p->target),
                                 .layer = p->layer,
                                 .clear = true,
                                 .clear_depth = 0,
                                 .store = true};
    de_gpu_cmd_begin_render(cmd, NULL, 0, &d);
    de_gpu_cmd_end_render(cmd);
}
de_result de__shadow_view_build(de__shadow_views *s, de_rg *g, const de__shadow_view_desc *d,
                                de__vis_context **out) {
    if (out)
        *out = NULL;
    if (!s || !g || !g->arena || !d || !d->scene || !DE_HANDLE_VALID(d->depth) || !d->width ||
        !d->height)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow view descriptor");
    shadow_block *b = NULL;
    DE_TRY(block_get(s, d->context / DE__VIEW_MAX, &b));
    const u32 slot = d->context % DE__VIEW_MAX;
    /* Each context has one immutable upload entry per frame-in-flight. Reusing it twice in
     * one graph/frame would overwrite constants before recording, so reject that explicitly. */
    if (b->pending[slot] || (b->used[slot] && b->used_frame[slot] == d->frame))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow context reused in one frame");
    if (!b->vis->enabled)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "shadow raster unavailable: %s", b->vis->why);
    const u32 ring = (u32)(d->frame % DE__VIS_FRAME_RING);
    if (b->ring_used[slot][ring] && b->ring_frame[slot][ring] >= d->frames_completed)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow constants still in flight");

    const u32 entry = slot * DE__VIS_FRAME_RING + (u32)(d->frame % DE__VIS_FRAME_RING);
    de__gs_view sc = {0};
    const de_mat4 vp = de_m4_mul(&d->proj, &d->view), inv = de_m4_inverse(&vp);
    rows(sc.view, &d->view);
    rows(sc.proj, &d->proj);
    rows(sc.view_proj, &vp);
    rows(sc.inv_view_proj, &inv);
    rows(sc.prev_view_proj, &vp);
    sc.camera_position = (de__gs_float4){.v = {d->camera.x, d->camera.y, d->camera.z, 0}};
    sc.viewport = (de__gs_float4){
        .v = {(f32)d->width, (f32)d->height, 1.f / (f32)d->width, 1.f / (f32)d->height}};
    const de__gpu_scene_table_info it = de__gpu_scene_table(d->scene, DE__GS_INSTANCES);
    const de__gpu_scene_table_info mt = de__gpu_scene_table(d->scene, DE__GS_MESHES);
    const de__gpu_scene_table_info at = de__gpu_scene_table(d->scene, DE__GS_MATERIALS);
    sc.instances = it.bindless;
    sc.instance_count = it.count;
    sc.meshes = mt.bindless;
    sc.mesh_count = mt.count;
    sc.materials = at.bindless;
    sc.material_count = at.count;
    sc.view_slot = entry;
    memcpy(b->mapped + (size_t)entry * sizeof sc, &sc, sizeof sc);
    de__vis_context *vc = de_arena_push_zero(g->arena, sizeof *vc, 16);
    if (!vc)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow context arena");
    vc->shared = b->vis;
    vc->graph = g;
    vc->arena = g->arena;
    vc->gpu_scene = d->scene;
    vc->view = (de_view){.id = slot + 1u};
    vc->view_slot = slot;
    vc->scene_view_slot = entry;
    vc->frame_index = d->frame;
    vc->frames_completed = d->frames_completed;
    vc->width = d->width;
    vc->height = d->height;
    vc->view_matrix = d->view;
    vc->proj = d->proj;
    vc->view_proj = vp;
    vc->prev_view_proj = vp;
    vc->camera_position = d->camera;
    vc->origin = d->origin;
    vc->layer_mask = d->layer_mask ? d->layer_mask : ~0u;
    vc->lod_error_px = 1.f;
    vc->casters_only = true;
    vc->depth_only = true;
    vc->depth_target = true;
    vc->depth = d->depth;
    vc->depth_width = d->width;
    vc->depth_height = d->height;
    vc->depth_layer = d->layer;
    memcpy(vc->raster_rect, d->rect, sizeof vc->raster_rect);
    vc->instance_count = it.count;
    vc->scene_material_count = at.count;
    vc->scene_instances = de__gpu_scene_import(d->scene, g, DE__GS_INSTANCES);
    vc->scene_meshes = de__gpu_scene_import(d->scene, g, DE__GS_MESHES);
    vc->scene_materials = de__gpu_scene_import(d->scene, g, DE__GS_MATERIALS);
    vc->scene_views = de_rg_import_buffer(g, b->constants, DE_ACCESS_SHADER_READ);
    vc->scene_views_bindless = de_gpu_buffer_bindless(s->dev, b->constants);
    if (d->clear_depth) {
        shadow_clear *p = de_arena_push(g->arena, sizeof *p, 16);
        if (!p)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow depth clear");
        *p = (shadow_clear){d->depth, d->layer};
        de_rg_pass *pass =
            de_rg_add_pass(g, "shadow.depth_clear", DE_QUEUE_GRAPHICS, clear_depth, p);
        /* This clears one layer, not the entire graph resource. Keep earlier layers live
         * even when their consumers come after another shadow view's clear. */
        de_rg_read(pass, d->depth, DE_ACCESS_DEPTH_READ);
        de_rg_write(pass, d->depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);
    }
    de__vis_cull_instances(g, vc, DE__VIS_PHASE_1);
    de__vis_cull_meshlets(g, vc, DE__VIS_PHASE_1);
    de__vis_raster(g, vc, DE__VIS_PHASE_1);
    de__vis_raster_alpha(g, vc, DE__VIS_PHASE_1);
    bool missing = it.count &&
                   (!DE_HANDLE_VALID(vc->work_items[0]) || !DE_HANDLE_VALID(vc->visible_meshlets) ||
                    !DE_HANDLE_VALID(vc->raster_args[0]) || !vc->raster_recorded ||
                    (de__gpu_scene_stats_get(d->scene)->mask_materials && !vc->alpha_recorded));
    if (missing || g->oom || g->error_count) {
        de__vis_shadow_abort(b->vis, slot, d->frame);
        const de_error *err = de_error_last();
        de_result result = g->oom                        ? DE_ERR_OUT_OF_MEMORY
                           : err && err->result != DE_OK ? err->result
                                                         : DE_ERR_INTERNAL;
        return DE_FAIL_WRAP(result, "shadow view graph construction incomplete");
    }
    b->used[slot] = true;
    b->used_frame[slot] = d->frame;
    b->pending[slot] = g;
    if (out)
        *out = vc;
    return DE_OK;
}

void de__shadow_views_end(de__shadow_views *s, const de_rg *graph, bool attempted) {
    if (!s || !graph)
        return;
    shadow_transfer_use **use = &s->transfer_uses;
    while (*use) {
        if ((*use)->graph == graph) {
            --(*use)->transfer->pending;
            *use = (*use)->next;
        } else
            use = &(*use)->next;
    }
    /* A replaced transfer can still be referenced by another unsubmitted graph. Retire it
     * only after every owner has submitted/aborted; the RHI then protects actual GPU use. */
    shadow_transfer **retired = &s->transfers;
    while (*retired) {
        shadow_transfer *t = *retired;
        if (t != s->transfer && !t->pending) {
            *retired = t->next;
            de_gpu_buffer_destroy(s->dev, t->buffer);
            DE_FREE(s->alloc, t, sizeof *t);
        } else
            retired = &t->next;
    }
    for (shadow_block *b = s->first; b; b = b->next)
        for (u32 slot = 0; slot < DE__VIEW_MAX; ++slot) {
            if (b->pending[slot] != graph)
                continue;
            b->pending[slot] = NULL;
            if (attempted) {
                u32 ring = (u32)(b->used_frame[slot] % DE__VIS_FRAME_RING);
                b->ring_used[slot][ring] = true;
                b->ring_frame[slot][ring] = b->used_frame[slot];
            } else {
                de__vis_shadow_abort(b->vis, slot, b->used_frame[slot]);
                b->used[slot] = false;
            }
        }
}

typedef struct shadow_copy {
    de_rg_resource depth, mirror, transfer;
    u32 layer, pitch;
} shadow_copy;
static void copy_depth(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const shadow_copy *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->transfer), 0, p->pitch,
                                      de_rg_texture(pass, p->depth), 0, p->layer);
}
static void copy_mirror(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const shadow_copy *p = user;
    de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->mirror), 0, 0,
                                      de_rg_buffer(pass, p->transfer), 0, p->pitch);
}
de_result de__shadow_copy_layer(de__shadow_views *s, de_rg *g, de_rg_resource depth, u32 layer,
                                de_rg_resource mirror, u32 width, u32 height) {
    if (!s || !g || !g->arena || !width || !height || width > 16384 || height > 16384 ||
        !DE_HANDLE_VALID(depth) || !DE_HANDLE_VALID(mirror))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow copy parameters");
    u32 pitch = (width * 4u + 255u) & ~255u;
    u64 bytes = (u64)pitch * height;
    if (!s->transfer || s->transfer->bytes < bytes) {
        shadow_transfer *t = DE_NEW(s->alloc, shadow_transfer, DE_MEM_RENDER);
        if (!t)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow transfer owner");
        *t = (shadow_transfer){.bytes = bytes, .next = s->transfers};
        t->buffer = de_gpu_buffer_create(
            s->dev, &(de_gpu_buffer_desc){.size = bytes,
                                          .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                          .name = "shadow transfer"});
        if (!DE_HANDLE_VALID(t->buffer)) {
            DE_FREE(s->alloc, t, sizeof *t);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow transfer scratch");
        }
        s->transfer = s->transfers = t;
    }
    shadow_transfer_use *use = s->transfer_uses;
    while (use && (use->graph != g || use->transfer != s->transfer))
        use = use->next;
    if (!use) {
        use = DE_ARENA_PUSH(g->arena, shadow_transfer_use, 1);
        if (!use)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow transfer graph ownership");
        *use = (shadow_transfer_use){.next = s->transfer_uses, .graph = g, .transfer = s->transfer};
        s->transfer_uses = use;
        ++s->transfer->pending;
    }
    shadow_copy *p = de_arena_push(g->arena, sizeof *p, 16);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow copy pass");
    /* Recycled GPU heap ranges may have an earlier transfer read. Keep a first-use execution
     * dependency even for a new buffer, just as the raw capture transfer path does. */
    *p = (shadow_copy){depth, mirror,
                       de_rg_import_buffer(g, s->transfer->buffer, DE_ACCESS_COPY_SRC), layer,
                       pitch};
    de_rg_pass *pass =
        de_rg_add_pass(g, "shadow.depth_to_buffer", DE_QUEUE_GRAPHICS, copy_depth, p);
    de_rg_read(pass, depth, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, p->transfer, DE_ACCESS_COPY_DST);
    pass = de_rg_add_pass(g, "shadow.buffer_to_mirror", DE_QUEUE_GRAPHICS, copy_mirror, p);
    de_rg_read(pass, p->transfer, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, mirror, DE_ACCESS_COPY_DST);
    de_rg_export(g, p->transfer, DE_ACCESS_COPY_SRC);
    de_rg_export(g, mirror, DE_ACCESS_SHADER_READ);
    return g->oom ? DE_ERR_OUT_OF_MEMORY : g->error_count ? DE_ERR_INVALID_ARGUMENT : DE_OK;
}

/* Both map owners may be constructed independently by internal tests. Registry ownership stays
 * in core; a second owner reuses the same user-configured handle without resetting its value. */
de_cvar de__shadow_pcss_register(void) {
    de_cvar cv = de_cvar_find("r.pcss_max_radius_texels");
    if (!DE_HANDLE_VALID(cv))
        cv = de_cvar_register(&(de_cvar_desc){
            .name = "r.pcss_max_radius_texels",
            .type = DE_CVAR_FLOAT,
            .default_number = 32,
            .min = 0,
            .max = 256,
            .help = "Maximum PCSS blocker-search and filter radius in full-map texels"});
    return cv;
}
