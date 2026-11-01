#include "render/post/tonemap.h"
#include "render/post/settings.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/shaders/engine_shaders.h"
#include "gpu/shader/shader_internal.h"
#include "asset/asset_internal.h"
#include <dengine/de_shader.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TM_MAX        32u
#define TM_BUILTINS   5u
#define TM_PUSH_BYTES 80u

typedef struct tm_entry {
    char name[64];
    de_tonemapper handle;
    de_gpu_pipeline pipeline;
    de_shader shader;
    u32 param_bytes, attempted_version;
    bool version_seen;
} tm_entry;
struct de__tonemappers {
    tm_entry entries[TM_MAX];
    u32 count;
    de_asset_system *assets;
    de__asset_destroy_observer asset_destroy;
};

static void assets_destroyed(void *user) {
    ((de__tonemappers *)user)->assets = NULL;
}

typedef struct tm_params {
    u32 src, dst, width, height;
    f32 paper_white_nits, peak_nits;
    u32 hdr, reserved;
    f32 data[8];
    f32 exposure_multiplier;
    u32 padding[3];
} tm_params;
DE_STATIC_ASSERT(sizeof(tm_params) == TM_PUSH_BYTES, "TonemapPassParams ABI");

typedef struct tm_pass {
    de_gpu_device *dev;
    de_gpu_pipeline pipeline;
    de_rg_resource src, dst;
    tm_params params;
} tm_pass;

static bool contract(const de_shader_reflection *r) {
    return r->stage == DE_SHADER_STAGE_COMPUTE && r->threadgroup[0] == 8 &&
           r->threadgroup[1] == 8 && r->threadgroup[2] == 1 &&
           r->push_constant_bytes <= TM_PUSH_BYTES;
}
static de_gpu_pipeline pipeline(de_renderer *r, const de_gpu_shader_blob *code, const char *name) {
    return de_gpu_pipeline_create(r->dev,
                                  &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                          .cs = *code,
                                                          .push_constant_bytes = TM_PUSH_BYTES,
                                                          .threadgroup_size = {8, 8, 1},
                                                          .name = name});
}
static de_result custom_pipeline(de_renderer *r, tm_entry *e, de_gpu_pipeline *out) {
    de_gpu_shader_blob code = {0};
    de_shader_reflection refl = {0};
    DE_TRY(
        de_shader_get_code(r->assets, e->shader, r->caps.backend, 0, "cs_tonemap", &code, &refl));
    if (!contract(&refl))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "tonemapper %s: cs_tonemap must be compute 8x8x1 with <=80 push bytes",
                       e->name);
    *out = pipeline(r, &code, e->name);
    return out->id ? DE_OK : DE_FAIL_WRAP(DE_ERR_INTERNAL, "tonemapper %s: pipeline", e->name);
}

de_result de__tonemappers_create(de_renderer *r) {
    de__tonemappers *t = DE_NEW(r->alloc, de__tonemappers, DE_MEM_RENDER);
    if (!t)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "tonemappers: registry");
    memset(t, 0, sizeof *t);
    r->tonemappers = t;
    t->assets = r->assets;
    if (t->assets)
        de__asset_destroy_observe(t->assets, &t->asset_destroy, assets_destroyed, t);
    static const char *const names[TM_BUILTINS] = {"agx", "aces", "pbr_neutral", "reinhard",
                                                   "none"};
    static const char *const entries[TM_BUILTINS] = {"cs_agx", "cs_aces", "cs_pbr_neutral",
                                                     "cs_reinhard", "cs_none"};
    de__engine_shader_code code;
    char why[256];
    DE_TRY(de__engine_shader_load(r->dev, r->alloc, "post_tonemap", 0, entries, TM_BUILTINS, &code,
                                  why, sizeof why));
    de_result result = DE_OK;
    for (u32 i = 0; i < TM_BUILTINS; ++i) {
        tm_entry *e = &t->entries[t->count];
        snprintf(e->name, sizeof e->name, "%s", names[i]);
        e->pipeline = pipeline(r, &code.entries[i].blob, e->name);
        if (!e->pipeline.id) {
            result = DE_FAIL_WRAP(DE_ERR_INTERNAL, "tonemapper %s: builtin pipeline", e->name);
            break;
        }
        /* The initial generational GPU handle is an immutable registry token across reloads. */
        e->handle.id = e->pipeline.id;
        e->param_bytes = 32;
        ++t->count;
    }
    de__engine_shader_code_free(&code);
    return result;
}

void de__tonemappers_destroy(de_renderer *r) {
    de__tonemappers *t = r->tonemappers;
    if (!t)
        return;
    de__asset_destroy_unobserve(&t->asset_destroy);
    for (u32 i = 0; i < t->count; ++i) {
        tm_entry *e = &t->entries[i];
        if (e->pipeline.id)
            de_gpu_pipeline_destroy(r->dev, e->pipeline);
        if (e->shader.id && t->assets)
            de_render_unpin_asset(r, e->shader.id);
    }
    DE_FREE(r->alloc, t, sizeof *t);
    r->tonemappers = NULL;
}

de_tonemapper de_tonemapper_builtin(de_renderer *r, de_builtin_tonemapper which) {
    if (!r || !r->tonemappers || (u32)which >= TM_BUILTINS) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "tonemapper: invalid renderer or builtin");
        return (de_tonemapper){0};
    }
    return r->tonemappers->entries[(u32)which].handle;
}

de_tonemapper de_tonemapper_register(de_renderer *r, const char *name, de_shader shader,
                                     u32 param_bytes) {
    de_tonemapper none = {0};
    if (!r || !r->tonemappers || !r->tonemappers->assets || !name || !shader.id ||
        param_bytes > 32) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "tonemapper: needs renderer/assets/name/shader and <=32 parameter bytes");
        return none;
    }
    const size_t len = strlen(name);
    if (!len || len >= sizeof r->tonemappers->entries[0].name) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "tonemapper: name must be 1..63 bytes");
        return none;
    }
    de__tonemappers *t = r->tonemappers;
    for (u32 i = 0; i < t->count; ++i)
        if (!strcmp(t->entries[i].name, name)) {
            (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "tonemapper: duplicate name %s", name);
            return none;
        }
    if (t->count == TM_MAX) {
        (void)DE_FAIL(DE_ERR_BUSY, "tonemapper: registry is full");
        return none;
    }
    tm_entry entry = {.shader = shader, .param_bytes = param_bytes};
    snprintf(entry.name, sizeof entry.name, "%s", name);
    if (!de_render_pin_asset(r, shader.id)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "tonemapper %s: shader cannot be pinned", name);
        return none;
    }
    de_result result = custom_pipeline(r, &entry, &entry.pipeline);
    if (result != DE_OK) {
        de_render_unpin_asset(r, shader.id);
        return none;
    }
    entry.version_seen = de__shader_loaded_version(r->assets, shader, &entry.attempted_version);
    entry.handle.id = entry.pipeline.id;
    t->entries[t->count++] = entry;
    return entry.handle;
}

void de__tonemappers_prepare(de_renderer *r) {
    de__tonemappers *t = r->tonemappers;
    if (!t || !t->assets)
        return;
    for (u32 i = TM_BUILTINS; i < t->count; ++i) {
        tm_entry *e = &t->entries[i];
        u32 version;
        if (!de__shader_loaded_version(r->assets, e->shader, &version) ||
            (e->version_seen && version == e->attempted_version))
            continue;
        e->version_seen = true;
        e->attempted_version = version;
        de_gpu_pipeline replacement = {0};
        if (custom_pipeline(r, e, &replacement) != DE_OK) {
            DE_WARN(r->log, "tonemapper %s: reload rejected; keeping previous pipeline", e->name);
            de_error_clear();
            continue;
        }
        /* Recording is idle here. The RHI retires the old object after in-flight GPU work. */
        const de_gpu_pipeline previous = e->pipeline;
        e->pipeline = replacement;
        de_gpu_pipeline_destroy(r->dev, previous);
    }
}

static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const tm_pass *p = user;
    tm_params pc = p->params;
    pc.src = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->src), 0, false);
    pc.dst = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->dst), 0, true);
    de_gpu_cmd_set_pipeline(cmd, p->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7) / 8, (pc.height + 7) / 8, 1);
}

de_rg_resource de__tonemap_add(de_renderer *r, de_rg *g, de_arena *arena, de_rg_resource src,
                               u32 width, u32 height, de_tonemapper mapper, const f32 params[8],
                               f32 exposure_multiplier, bool hdr_output,
                               const de_hdr_settings *hdr) {
    if (!r || !r->tonemappers || !g || !arena || !src.id || !width || !height)
        return (de_rg_resource){0};
    de__tonemappers *t = r->tonemappers;
    const tm_entry *e = &t->entries[0];
    if (mapper.id) {
        e = NULL;
        for (u32 i = 0; i < t->count; ++i)
            if (t->entries[i].handle.id == mapper.id) {
                e = &t->entries[i];
                break;
            }
        if (!e) {
            (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                          "tonemapper: handle belongs to no registered mapper");
            return (de_rg_resource){0};
        }
    }
    tm_pass *p = de_arena_push(arena, sizeof *p, 16);
    if (!p)
        return (de_rg_resource){0};
    const de_rg_resource dst = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                  .width = width,
                                  .height = height,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC |
                                           DE_USAGE_RENDER_TARGET,
                                  .name = "post.tonemap"});
    if (!dst.id)
        return (de_rg_resource){0};
    const de_hdr_settings h = de__hdr_settings_resolve(hdr);
    *p = (tm_pass){
        .dev = r->dev,
        .pipeline = e->pipeline,
        .src = src,
        .dst = dst,
        .params = {.width = width,
                   .height = height,
                   .paper_white_nits = h.paper_white_nits,
                   .peak_nits = h.peak_nits,
                   .hdr = hdr_output ? 1u : 0u,
                   .exposure_multiplier = isfinite(exposure_multiplier) && exposure_multiplier > 0
                                              ? exposure_multiplier
                                              : 1}};
    if (params)
        memcpy(p->params.data, params, e->param_bytes);
    de_rg_pass *pass = de_rg_add_pass(g, "post.tonemap", DE_QUEUE_GRAPHICS, execute, p);
    if (!pass)
        return (de_rg_resource){0};
    de_rg_read(pass, src, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, dst, DE_ACCESS_SHADER_WRITE);
    return dst;
}
