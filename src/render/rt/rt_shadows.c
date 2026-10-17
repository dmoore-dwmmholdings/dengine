/* DENG-45: clustered analytic-light visibility through the production scene query path. */
#include "render/rt/rt_shadows.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/shaders/engine_shaders.h"
#include <math.h>
#include <string.h>

de__rt_ray de__rt_shadow_ray(const de__cluster_light *l, de_vec3 p, de_vec3 normal, de_vec2 u) {
    u32 kind;
    memcpy(&kind, &l->shape[3], 4);
    const f32 phi = 6.28318530718f * u.y;
    de_vec3 target = {l->position[0], l->position[1], l->position[2]};
    de_vec3 direction = {l->direction[0], l->direction[1], l->direction[2]};
    de_vec3 center = kind == 0 ? de_v3_scale(direction, -1) : de_v3_normalize(de_v3_sub(target, p));
    de_vec3 x = de_v3_normalize(
        de_v3_cross(fabsf(center.z) < .999f ? (de_vec3){0, 0, 1} : (de_vec3){0, 1, 0}, center));
    de_vec3 y = de_v3_cross(center, x), wi;
    f32 distance = 1e6f;
    if (kind == 0) {
        f32 z = 1 - u.x * (1 - cosf(fminf(fmaxf(l->direction[3], 0), 1.55f)));
        wi = de_v3_normalize(
            de_v3_add(de_v3_scale(center, z),
                      de_v3_scale(de_v3_add(de_v3_scale(x, cosf(phi)), de_v3_scale(y, sinf(phi))),
                                  sqrtf(fmaxf(0, 1 - z * z)))));
    } else {
        if (kind == 3 || kind == 4) {
            x = de_v3_normalize((de_vec3){l->axis[0], l->axis[1], l->axis[2]});
            y = de_v3_normalize(de_v3_cross(direction, x));
            f32 a = kind == 3 ? 2 * u.x - 1 : sqrtf(u.x) * cosf(phi),
                b = kind == 3 ? 2 * u.y - 1 : sqrtf(u.x) * sinf(phi);
            target = de_v3_add(target, de_v3_add(de_v3_scale(x, a * l->shape[1]),
                                                 de_v3_scale(y, b * l->shape[2])));
        } else {
            f32 z = 1 - 2 * u.x, r = sqrtf(fmaxf(0, 1 - z * z));
            target = de_v3_add(
                target, de_v3_scale((de_vec3){r * cosf(phi), r * sinf(phi), z}, l->direction[3]));
        }
        de_vec3 delta = de_v3_sub(target, p);
        distance = de_v3_length(delta);
        wi = de_v3_scale(delta, 1 / fmaxf(distance, 1e-8f));
    }
    f32 bias = fmaxf(.0001f, .00001f * fmaxf(fabsf(p.x), fmaxf(fabsf(p.y), fabsf(p.z))));
    de_vec3 origin = de_v3_add(p, de_v3_scale(normal, de_v3_dot(normal, wi) >= 0 ? bias : -bias));
    return (de__rt_ray){{origin.x, origin.y, origin.z, .00001f},
                        {wi.x, wi.y, wi.z, fmaxf(0, distance - bias)}};
}
typedef struct shadow_view {
    de_view handle;
    de_gpu_buffer rays, hits, keys;
    de_gpu_texture raw, distance, temp, visibility;
    u32 width, height, slots, capacity;
    de__rt_shadows_info info;
} shadow_view;
struct de__rt_shadows {
    de_renderer *renderer;
    de_gpu_pipeline generate, resolve, blur;
    de_cvar max_lights;
    shadow_view views[32];
};
typedef struct shadow_params {
    u32 scene, scene_slot, view, entry;
    u32 depth, normal, lights, dir_lights;
    u32 grid, list, counters, rays;
    u32 hits, keys, visibility, distance;
    u32 source, output, width, height;
    u32 full_width, full_height, slots, slot;
    u32 scale, spp, frame, axis;
} shadow_params;
DE_STATIC_ASSERT(sizeof(shadow_params) == 112, "shadow constants");
typedef struct shadow_pass {
    de__rt_shadows *s;
    de__rt_context context;
    shadow_view *view;
    shadow_params params;
    de_rg_resource reads[8];
    u32 kind;
} shadow_pass;
static void release(de__rt_shadows *s, shadow_view *v) {
    de_gpu_device *d = s->renderer->dev;
    de_gpu_buffer_destroy(d, v->rays);
    de_gpu_buffer_destroy(d, v->hits);
    de_gpu_buffer_destroy(d, v->keys);
    de_gpu_texture_destroy(d, v->raw);
    de_gpu_texture_destroy(d, v->distance);
    de_gpu_texture_destroy(d, v->temp);
    de_gpu_texture_destroy(d, v->visibility);
    memset(v, 0, sizeof *v);
}
void de__rt_shadows_destroy(de__rt_shadows *s) {
    if (!s)
        return;
    for (u32 i = 0; i < 32; ++i)
        release(s, &s->views[i]);
    de_gpu_pipeline_destroy(s->renderer->dev, s->generate);
    de_gpu_pipeline_destroy(s->renderer->dev, s->resolve);
    de_gpu_pipeline_destroy(s->renderer->dev, s->blur);
    DE_FREE(s->renderer->alloc, s, sizeof *s);
}
de_result de__rt_shadows_create(de_renderer *r, de__rt_shadows **out) {
    *out = NULL;
    de__rt_shadows *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT shadows owner");
    memset(s, 0, sizeof *s);
    s->renderer = r;
    s->max_lights = de_cvar_register(&(de_cvar_desc){
        .name = "r.rt.shadows.max_lights",
        .help =
            "RT shadow-casting analytic lights per clustered pixel (remaining lights use raster)",
        .type = DE_CVAR_INT,
        .default_number = 4,
        .min = 1,
        .max = 4});
    if (r->caps.ray_query && r->caps.max_bindless_accels) {
        const char *names[] = {"cs_generate", "cs_resolve", "cs_blur"};
        de__engine_shader_code code;
        char why[512] = {0};
        de_result result = de__engine_shader_load(r->dev, r->alloc, "rt_shadows", 0, names, 3,
                                                  &code, why, sizeof why);
        if (result != DE_OK) {
            de__rt_shadows_destroy(s);
            return DE_FAIL(result, "RT shadow shaders: %s", why);
        }
        de_gpu_pipeline *p[] = {&s->generate, &s->resolve, &s->blur};
        for (u32 i = 0; i < 3; ++i)
            *p[i] = de_gpu_pipeline_create(
                r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                .cs = code.entries[i].blob,
                                                .push_constant_bytes = sizeof(shadow_params),
                                                .threadgroup_size = {8, 8, 1},
                                                .name = names[i]});
        de__engine_shader_code_free(&code);
        if (!s->generate.id || !s->resolve.id || !s->blur.id) {
            de__rt_shadows_destroy(s);
            return DE_FAIL(DE_ERR_UNSUPPORTED, "RT shadow pipelines");
        }
    }
    *out = s;
    return DE_OK;
}
static bool reserve(de__rt_shadows *s, shadow_view *v, u32 width, u32 height, u32 slots, u32 spp) {
    de_gpu_device *d = s->renderer->dev;
    const u64 count = (u64)width * height * spp;
    if (count > UINT32_MAX || (u64)height * slots > 16384)
        return false;
    if (v->width != width || v->height != height || v->slots != slots) {
        release(s, v);
        v->width = width;
        v->height = height;
        v->slots = slots;
        de_gpu_texture_desc desc = {.width = width,
                                    .height = height * slots,
                                    .format = DE_FMT_R16_FLOAT,
                                    .usage =
                                        DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                    .name = "RT shadow visibility"};
        v->raw = de_gpu_texture_create(d, &desc);
        v->distance = de_gpu_texture_create(d, &desc);
        v->temp = de_gpu_texture_create(d, &desc);
        v->visibility = de_gpu_texture_create(d, &desc);
        v->keys = de_gpu_buffer_create(
            d, &(de_gpu_buffer_desc){.size = (u64)width * height * slots * 4,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .name = "RT shadow light keys"});
    }
    if (count > v->capacity) {
        de_gpu_buffer_destroy(d, v->rays);
        de_gpu_buffer_destroy(d, v->hits);
        v->rays = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = count * 32,
                                                                .stride = 32,
                                                                .usage = DE_USAGE_STORAGE,
                                                                .name = "RT shadow rays"});
        v->hits = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = count * 32,
                                                                .stride = 32,
                                                                .usage = DE_USAGE_STORAGE,
                                                                .name = "RT shadow hits"});
        v->capacity = (u32)count;
    }
    return v->rays.id && v->hits.id && v->keys.id && v->raw.id && v->distance.id && v->temp.id &&
           v->visibility.id;
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    shadow_pass *p = user;
    shadow_params pc = p->params;
    de_gpu_device *d = p->s->renderer->dev;
    shadow_view *v = p->view;
    pc.rays = de_gpu_buffer_bindless(d, v->rays);
    pc.hits = de_gpu_buffer_bindless(d, v->hits);
    pc.keys = de_gpu_buffer_bindless(d, v->keys);
    pc.visibility = de_gpu_texture_bindless(d, v->raw, 0, true);
    pc.distance = de_gpu_texture_bindless(d, v->distance, 0, true);
    if (p->kind == 0) {
        pc.depth = de_gpu_texture_bindless(d, de_rg_texture(pass, p->reads[0]), 0, false);
        pc.normal = de_gpu_texture_bindless(d, de_rg_texture(pass, p->reads[1]), 0, false);
        pc.lights = de_gpu_buffer_bindless(d, de_rg_buffer(pass, p->reads[2]));
        pc.dir_lights = de_gpu_buffer_bindless(d, de_rg_buffer(pass, p->reads[3]));
        pc.grid = de_gpu_buffer_bindless(d, de_rg_buffer(pass, p->reads[4]));
        pc.list = de_gpu_buffer_bindless(d, de_rg_buffer(pass, p->reads[5]));
        pc.counters = de_gpu_buffer_bindless(d, de_rg_buffer(pass, p->reads[6]));
        de_gpu_cmd_set_pipeline(cmd, p->s->generate);
        de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, (pc.width + 7) / 8, (pc.height + 7) / 8, 1);
        de_gpu_barrier b = {
            .buffer = v->rays, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_SHADER_READ};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de__rt_trace(p->s->renderer, cmd, &p->context, v->rays, v->hits,
                     pc.width * pc.height * pc.spp, DE__RT_CAST_SHADOWS | DE__RT_ACCEPT_FIRST_HIT);
        b.buffer = v->hits;
        b.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_set_pipeline(cmd, p->s->resolve);
        de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, (pc.width + 7) / 8, (pc.height + 7) / 8, 1);
    } else {
        pc.source = de_gpu_texture_bindless(d, p->kind == 1 ? v->raw : v->temp, 0, false);
        pc.output = de_gpu_texture_bindless(d, p->kind == 1 ? v->temp : v->visibility, 0, true);
        pc.axis = p->kind - 1;
        de_gpu_cmd_set_pipeline(cmd, p->s->blur);
        de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, (pc.width + 7) / 8, (pc.height * pc.slots + 7) / 8, 1);
    }
}
de_result de__rt_shadows_build(void *user, de_rg *g, de__vis_context *vc, de__lighting_context *lc,
                               de__lighting_inputs *inputs) {
    de__rt_shadows *s = user;
    if (!s || !vc->settings)
        return DE_OK;
    const de_render_settings *settings = vc->settings;
    if (settings->tier != DE_TIER_RT || !settings->rt_shadows ||
        settings->shadows == DE_QUALITY_OFF)
        return DE_OK;
    de__rt_context *rt = de__rt_context_get(s->renderer, vc->view, g, vc->frame_index);
    if (!rt || !s->generate.id)
        return DE_OK;
    u32 slot = (vc->view.id & 255u) - 1u;
    if (slot >= 32)
        return DE_OK;
    shadow_view *v = &s->views[slot];
    const u32 scale = settings->shadows == DE_QUALITY_LOW ? 2 : 1,
              spp = settings->shadows == DE_QUALITY_ULTRA ? 2 : 1;
    const u32 width = (vc->width + scale - 1) / scale, height = (vc->height + scale - 1) / scale;
    u32 slots = (u32)de_cvar_int(s->max_lights);
    if (slots < 1)
        slots = 1;
    if (slots > 4)
        slots = 4;
    if (!reserve(s, v, width, height, slots, spp))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT shadow extent");
    v->handle = vc->view;
    de_rg_resource rays = de_rg_import_buffer(g, v->rays, DE_ACCESS_SHADER_READ),
                   hits = de_rg_import_buffer(g, v->hits, DE_ACCESS_SHADER_READ);
    de_rg_resource keys = de_rg_import_buffer(g, v->keys, DE_ACCESS_SHADER_READ);
    de_rg_resource raw = de_rg_import_texture(g, v->raw, DE_ACCESS_SHADER_READ),
                   distance = de_rg_import_texture(g, v->distance, DE_ACCESS_SHADER_READ);
    de_rg_resource temp = de_rg_import_texture(g, v->temp, DE_ACCESS_SHADER_READ),
                   visibility = de_rg_import_texture(g, v->visibility, DE_ACCESS_SHADER_READ);
    shadow_pass base = {.s = s,
                        .view = v,
                        .context = *rt,
                        .params = {.scene = vc->scene_views_bindless,
                                   .scene_slot = vc->scene_view_slot,
                                   .view = lc->constants_bindless,
                                   .entry = lc->constants_entry,
                                   .width = width,
                                   .height = height,
                                   .full_width = vc->width,
                                   .full_height = vc->height,
                                   .slots = slots,
                                   .scale = scale,
                                   .spp = spp,
                                   .frame = (u32)vc->frame_index},
                        .reads = {vc->depth, vc->gbuffer[DE__GB1], lc->lights, lc->dir_lights,
                                  lc->grid_buf, lc->list, lc->counters, vc->scene_views}};
    for (u32 i = 0; i < slots; ++i) {
        shadow_pass *p = de_arena_push_zero(g->arena, sizeof *p, 16);
        if (!p)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT shadow pass");
        *p = base;
        p->params.slot = i;
        de_rg_pass *pass = de_rg_add_pass(g, "RT.Shadows", DE_QUEUE_GRAPHICS, execute, p);
        if (!pass)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT shadow graph");
        de_rg_pass_side_effect(pass); /* Slots write disjoint regions of shared image resources. */
        for (u32 j = 0; j < 8; ++j)
            de_rg_read(pass, p->reads[j], DE_ACCESS_SHADER_READ);
        de_rg_read(pass, rt->dependency, DE_ACCESS_SHADER_READ);
        de_rg_write(pass, rays, DE_ACCESS_SHADER_WRITE);
        de_rg_write(pass, hits, DE_ACCESS_SHADER_WRITE);
        de_rg_write(pass, keys, DE_ACCESS_SHADER_WRITE);
        de_rg_write(pass, raw, DE_ACCESS_SHADER_WRITE);
        de_rg_write(pass, distance, DE_ACCESS_SHADER_WRITE);
    }
    for (u32 i = 1; i <= 2; ++i) {
        shadow_pass *p = de_arena_push_zero(g->arena, sizeof *p, 16);
        if (!p)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT shadow blur");
        *p = base;
        p->kind = i;
        de_rg_pass *pass = de_rg_add_pass(g, i == 1 ? "RT.Shadows.BlurX" : "RT.Shadows.BlurY",
                                          DE_QUEUE_GRAPHICS, execute, p);
        if (!pass)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT shadow blur graph");
        de_rg_read(pass, keys, DE_ACCESS_SHADER_READ);
        de_rg_read(pass, i == 1 ? raw : temp, DE_ACCESS_SHADER_READ);
        de_rg_write(pass, i == 1 ? temp : visibility, DE_ACCESS_SHADER_WRITE);
    }
    de_rg_export(g, rays, DE_ACCESS_SHADER_READ);
    de_rg_export(g, hits, DE_ACCESS_SHADER_READ);
    de_rg_export(g, keys, DE_ACCESS_SHADER_READ);
    de_rg_export(g, raw, DE_ACCESS_SHADER_READ);
    de_rg_export(g, temp, DE_ACCESS_SHADER_READ);
    de_rg_export(g, distance, DE_ACCESS_SHADER_READ);
    de_rg_export(g, visibility, DE_ACCESS_SHADER_READ);
    inputs->rt_shadow_visibility = visibility;
    inputs->rt_shadow_distance = distance;
    inputs->rt_shadow_keys = keys;
    inputs->rt_shadow_extent = (width & 65535u) | (height << 16);
    inputs->rt_shadow_config = slots | (scale << 8);
    lc->rt_shadow_visibility = visibility;
    lc->rt_shadow_distance = distance;
    lc->rt_shadow_keys = keys;
    const u64 rays_count = (u64)width * height * spp * slots;
    s->renderer->stats.rays_traced += rays_count;
    v->info =
        (de__rt_shadows_info){v->visibility, v->distance, v->keys, width,      height,
                              slots,         scale,       spp,     rays_count, vc->frame_index};
    return DE_OK;
}
bool de__rt_shadows_get(de__rt_shadows *s, de_view view, de__rt_shadows_info *out) {
    if (!s || !out)
        return false;
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= 32 || s->views[slot].handle.id != view.id)
        return false;
    *out = s->views[slot].info;
    return true;
}
