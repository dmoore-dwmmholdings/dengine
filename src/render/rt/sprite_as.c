#include "sprite_as.h"
#include "render/world/render_world.h"
#include <string.h>

typedef struct sprite_geometry {
    de_gpu_buffer positions, indices, ranges;
    de_gpu_accel blas;
    u32 vertex_count, index_count;
    de_gpu_texture texture;
    bool seen;
} sprite_geometry;
typedef struct de__sprite_as {
    sprite_geometry quad, box;
    sprite_geometry *proxies;
    u32 count, capacity;
} de__sprite_as;

static void destroy_geometry(de_renderer *r, sprite_geometry *g) {
    de_gpu_accel_destroy(r->dev, g->blas);
    de_gpu_buffer_destroy(r->dev, g->positions);
    de_gpu_buffer_destroy(r->dev, g->indices);
    de_gpu_buffer_destroy(r->dev, g->ranges);
    memset(g, 0, sizeof *g);
}
static de_result build_geometry(de_renderer *r, sprite_geometry *g, const f32 *positions,
                                u32 vertices, const u32 *indices, u32 index_count, bool opaque) {
    g->vertex_count = vertices;
    g->index_count = index_count;
    g->positions = de_gpu_buffer_create(
        r->dev, &(de_gpu_buffer_desc){.size = (u64)vertices * 12,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT,
                                      .memory = DE_MEM_UPLOAD,
                                      .name = "sprite RT vertices"});
    g->indices = de_gpu_buffer_create(
        r->dev, &(de_gpu_buffer_desc){.size = (u64)index_count * 4,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT,
                                      .memory = DE_MEM_UPLOAD,
                                      .name = "sprite RT indices"});
    g->ranges = de_gpu_buffer_create(r->dev, &(de_gpu_buffer_desc){.size = 8,
                                                                   .usage = DE_USAGE_STORAGE,
                                                                   .memory = DE_MEM_UPLOAD,
                                                                   .name = "sprite RT range"});
    if (!g->positions.id || !g->indices.id || !g->ranges.id)
        goto fail;
    memcpy(de_gpu_buffer_map(r->dev, g->positions), positions, (size_t)vertices * 12);
    memcpy(de_gpu_buffer_map(r->dev, g->indices), indices, (size_t)index_count * 4);
    u32 range[2] = {0, index_count};
    memcpy(de_gpu_buffer_map(r->dev, g->ranges), range, 8);
    de_gpu_blas_geometry geometry = {.vertices = g->positions,
                                     .vertex_stride = 12,
                                     .vertex_count = vertices,
                                     .vertex_format = DE_FMT_RGB32_FLOAT,
                                     .indices = g->indices,
                                     .index_count = index_count,
                                     .opaque = opaque};
    g->blas = de_gpu_accel_create(r->dev, &(de_gpu_accel_desc){.geometries = &geometry,
                                                               .geometry_count = 1,
                                                               .prefer_fast_trace = true,
                                                               .name = "shared sprite BLAS"});
    if (!g->blas.id)
        goto fail;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->dev, DE_QUEUE_GRAPHICS, "Sprite.BuildBLAS");
    if (!cmd)
        goto fail;
    de_gpu_cmd_build_blas(cmd, g->blas, false);
    de_gpu_barrier barrier = {
        .accel = g->blas, .before = DE_ACCESS_ACCEL_WRITE, .after = DE_ACCESS_ACCEL_READ};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    return de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS,
                               &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
fail:
    destroy_geometry(r, g);
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sprite BLAS resources");
}
static bool proxy_region(const DeSprite *s, const de_render_texture *texture) {
    if (!texture || !texture->sprite_proxy)
        return false;
    const f32 *r = texture->sprite_proxy->region;
    return s->uv_rect.x == r[0] && s->uv_rect.y == r[1] && s->uv_rect.z == r[2] &&
           s->uv_rect.w == r[3];
}
de_result de__sprite_as_prepare(de_renderer *r) {
    const de__render_world_frame *f = de__gpu_scene_world(r->gpu_scene);
    if (!f)
        return DE_OK;
    bool any = false;
    for (u32 i = 0; i < f->instances.count; ++i)
        any |= (f->instances.flags[i] & DE__RW_INST_SPRITE) != 0;
    if (!any)
        return DE_OK;
    if (!r->sprite_as) {
        de__sprite_as *s = DE_NEW(r->alloc, de__sprite_as, DE_MEM_RENDER);
        if (!s)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sprite AS owner");
        memset(s, 0, sizeof *s);
        r->sprite_as = s;
        const f32 quad[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        const u32 qindices[6] = {0, 1, 2, 0, 2, 3};
        const f32 box[24] = {0, 0, -.5f, 1, 0, -.5f, 1, 1, -.5f, 0, 1, -.5f,
                             0, 0, .5f,  1, 0, .5f,  1, 1, .5f,  0, 1, .5f};
        const u32 bindices[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                  1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
        DE_TRY(build_geometry(r, &s->quad, quad, 4, qindices, 6, false));
        DE_TRY(build_geometry(r, &s->box, box, 8, bindices, 36, false));
    }
    de__sprite_as *s = r->sprite_as;
    for (u32 i = 0; i < s->count; ++i)
        s->proxies[i].seen = false;
    for (u32 i = 0; i < f->instances.count; ++i) {
        if (!(f->instances.flags[i] & DE__RW_INST_SPRITE))
            continue;
        const DeSprite *sprite = &f->instances.sprite[i];
        if (!(sprite->thickness > 0))
            continue;
        const de_render_texture *texture =
            de__gpu_scene_texture_record(r->gpu_scene, sprite->texture);
        if (!proxy_region(sprite, texture))
            continue;
        u32 index = 0;
        /* CPU asset records may be freed and recycled while RT is disabled. The RHI
         * handle carries the loaded texture generation and does not retain that address. */
        while (index < s->count && s->proxies[index].texture.id != texture->texture.id)
            ++index;
        if (index == s->count) {
            if (s->count == s->capacity) {
                u32 cap = s->capacity ? s->capacity * 2 : 8;
                sprite_geometry *next =
                    DE_ALLOC(r->alloc, (size_t)cap * sizeof *next, 16, DE_MEM_RENDER);
                if (!next)
                    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sprite proxy table");
                memset(next, 0, (size_t)cap * sizeof *next);
                if (s->proxies) {
                    memcpy(next, s->proxies, (size_t)s->count * sizeof *next);
                    DE_FREE(r->alloc, s->proxies, (size_t)s->capacity * sizeof *next);
                }
                s->proxies = next;
                s->capacity = cap;
            }
            const de_cooked_sprite_proxy *proxy = texture->sprite_proxy;
            DE_TRY(build_geometry(r, &s->proxies[index], &proxy->positions[0][0],
                                  proxy->vertex_count, proxy->indices, proxy->index_count, true));
            s->proxies[index].texture = texture->texture;
            ++s->count;
        }
        s->proxies[index].seen = true;
    }
    for (u32 i = 0; i < s->count;) {
        if (s->proxies[i].seen) {
            ++i;
            continue;
        }
        destroy_geometry(r, &s->proxies[i]);
        s->proxies[i] = s->proxies[--s->count];
    }
    return DE_OK;
}
bool de__sprite_as_metadata(de_renderer *r, u32 mesh_slot, de__rt_mesh_gpu *out) {
    if (!r->sprite_as)
        return false;
    u32 slot;
    if (!de__gpu_scene_sprite_slot(r->gpu_scene, mesh_slot, &slot))
        return false;
    const de__render_world_frame *f = de__gpu_scene_world(r->gpu_scene);
    if (!f || slot >= f->instances.count)
        return false;
    const DeSprite *sprite = &f->instances.sprite[slot];
    de__sprite_as *s = r->sprite_as;
    sprite_geometry *geometry = sprite->thickness > 0 ? &s->box : &s->quad;
    u32 kind = sprite->thickness > 0 ? 1 : 0;
    const de_render_texture *texture = de__gpu_scene_texture_record(r->gpu_scene, sprite->texture);
    if (sprite->thickness > 0 && proxy_region(sprite, texture))
        for (u32 i = 0; i < s->count; ++i)
            if (s->proxies[i].texture.id == texture->texture.id) {
                geometry = &s->proxies[i];
                kind = 2;
                break;
            }
    out->blas[0] = geometry->blas.id;
    out->resident_mask = 1;
    out->ranges = de_gpu_buffer_bindless(r->dev, geometry->ranges);
    out->submeshes = 1;
    /* Sprite meshes have only LOD0. Unused LOD1+ metadata describes projected hit geometry. */
    out->blas[1] = kind;
    out->index_first[1] = de_gpu_buffer_bindless(r->dev, geometry->positions);
    out->index_first[2] = de_gpu_buffer_bindless(r->dev, geometry->indices);
    out->index_first[3] = geometry->index_count / 3;
    return true;
}
void de__sprite_as_destroy(de_renderer *r) {
    if (!r || !r->sprite_as)
        return;
    de__sprite_as *s = r->sprite_as;
    destroy_geometry(r, &s->quad);
    destroy_geometry(r, &s->box);
    for (u32 i = 0; i < s->count; ++i)
        destroy_geometry(r, &s->proxies[i]);
    if (s->proxies)
        DE_FREE(r->alloc, s->proxies, (size_t)s->capacity * sizeof *s->proxies);
    DE_FREE(r->alloc, s, sizeof *s);
    r->sprite_as = NULL;
}
