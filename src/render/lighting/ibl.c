/*
 * ibl.c: image-based lighting's environment (SPEC §12.5 step 5, §12.7, §11.6; DENG-304). The
 * shader side is shaders/lighting/ibl.slang (de_ibl_evaluate, called by lighting_common.slang's
 * evaluate_indirect).
 *
 * The environment is the render world's first DeEnvironment: its HDRI (a texture cooked with
 * usage "environment", DENG-100: a GGX-prefiltered cube whose mip k holds roughness
 * k / (mips - 1), plus the SH9 irradiance in its ENVS trailer, which the DENG-105 loader keeps on
 * the CPU), times `intensity`, rotated by `rotation` about +Z. The kick snapshots it on the main
 * thread (de__lighting_prepare_environment; the render thread reads no asset record, DENG-422),
 * and lighting packs it into each view's lighting constants (DeLightingView.ibl): the SH9 and the
 * mapping are a few hundred bytes, repacked when they change.
 *
 * The strip. The bindless tables have no cube type (the Metal heap is a table of 2D textures; the
 * Vulkan and D3D12 views are typed 2D by the shaders), so the first graph after the environment
 * changed copies the cube into a 2D texture of the same format, the six faces stacked vertically
 * in layer order (face_size x 6 face_size, the same mips): "lighting.ibl_strip_read" copies every
 * face of every mip into a transient buffer with the rows of one strip mip contiguous, and
 * "lighting.ibl_strip_write" copies each mip into the strip. Block-compressed cubes (BC6H) keep
 * only the mips whose faces are at least one block (4 texels): the shader clamps the LOD there.
 * The shader picks the face of a direction itself and clamps inside it (ibl.slang).
 *
 * Without an environment the shader lights with a constant radiance, r.ambient_fallback (nits;
 * default 0, black) until the procedural sky (S34).
 */
#include "render/lighting/lighting.h"

#include "render/formats/de_cooked_texture.h"
#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"
#include "render/world/render_world.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* =================================================================================================
 * Pure parts
 * ============================================================================================== */

u32 de__ibl_strip_mips(u32 format, u32 face_size, u32 mip_count) {
    de_cooked_texture_block b;
    if (!de_cooked_texture_format_block(format, &b) || face_size == 0)
        return 0;
    u32 n = 0;
    while (n < mip_count && (face_size >> n) >= b.width && (face_size >> n) >= b.height &&
           (face_size >> n) > 0)
        ++n;
    return n;
}

void de__ibl_pack(const de__ibl_environment *env, u32 strip, u32 strip_mips, u32 sampler,
                  f32 ambient_nits, de__ibl_gpu *out) {
    memset(out, 0, sizeof *out);
    const f32 amb = ambient_nits > 0.0f ? ambient_nits : 0.0f;
    out->ambient[0] = out->ambient[1] = out->ambient[2] = amb;
    out->rotation[0] = 1.0f;
    if (env == NULL || !env->valid || strip == 0 || strip_mips == 0)
        return;
    const f32 k = env->intensity >= 0.0f ? env->intensity : 0.0f;
    for (u32 i = 0; i < 9; ++i)
        for (u32 c = 0; c < 3; ++c)
            out->sh[i][c] = env->sh9[i][c] * k;
    out->sh[0][3] = k;
    out->rotation[0] = cosf(env->rotation);
    out->rotation[1] = sinf(env->rotation);
    out->rotation[2] = env->mip_per_roughness;
    out->rotation[3] = (f32)(strip_mips - 1u);
    out->strip = strip;
    out->sampler = sampler;
    out->face_size = env->face_size;
    out->flags = DE__IBL_ENVIRONMENT;
}

void de__ibl_rotate(const de__ibl_gpu *g, const f32 d[3], f32 out[3]) {
    const f32 c = g->rotation[0], s = g->rotation[1];
    const f32 x = d[0], y = d[1];
    out[0] = c * x + s * y;
    out[1] = -s * x + c * y;
    out[2] = d[2];
}

void de__ibl_irradiance(const de__ibl_gpu *g, const f32 n[3], f32 out[3]) {
    if ((g->flags & DE__IBL_ENVIRONMENT) == 0u) {
        for (u32 c = 0; c < 3; ++c)
            out[c] = g->ambient[c] * 3.14159265358979323846f;
        return;
    }
    f32 d[3];
    de__ibl_rotate(g, n, d);
    const f32 x = d[0], y = d[1], z = d[2];
    const f32 b[9] = {0.282094791773878f,
                      0.488602511902920f * y,
                      0.488602511902920f * z,
                      0.488602511902920f * x,
                      1.092548430592079f * x * y,
                      1.092548430592079f * y * z,
                      0.315391565252520f * (3.0f * z * z - 1.0f),
                      1.092548430592079f * x * z,
                      0.546274215296040f * (x * x - y * y)};
    for (u32 c = 0; c < 3; ++c) {
        f32 e = 0.0f;
        for (u32 k = 0; k < 9; ++k)
            e += g->sh[k][c] * b[k];
        out[c] = e > 0.0f ? e : 0.0f;
    }
}

/* =================================================================================================
 * State
 * ============================================================================================== */

de_result de__ibl_init(de__lighting *lt) {
    lt->cv_ambient = de_cvar_register(&(de_cvar_desc){
        .name = "r.ambient_fallback",
        .help = "Image-based lighting without an environment: a constant radiance (nits) from "
                "every direction (0: black; the procedural sky comes in S34)",
        .type = DE_CVAR_FLOAT,
        .default_number = 0.0,
        .min = 0.0,
        .max = 1.0e9});
    lt->cv_ev100_default = de_cvar_register(&(de_cvar_desc){
        .name = "r.exposure_ev100_default",
        .help = "Exposure (EV100) of cameras whose exposure_ev100 is NaN (automatic) until the "
                "auto exposure (S18)",
        .type = DE_CVAR_FLOAT,
        .default_number = DE__EXPOSURE_EV100_DEFAULT,
        .min = -20.0,
        .max = 30.0});
    lt->ibl_sampler = de_gpu_sampler_create(lt->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                            .mag = DE_FILTER_LINEAR,
                                                                            .mip = DE_FILTER_LINEAR,
                                                                            .u = DE_ADDR_CLAMP,
                                                                            .v = DE_ADDR_CLAMP,
                                                                            .w = DE_ADDR_CLAMP});
    if (!DE_HANDLE_VALID(lt->ibl_sampler))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "lighting: the IBL sampler");
    lt->ibl_sampler_bindless = de_gpu_sampler_bindless(lt->dev, lt->ibl_sampler);
    return DE_OK;
}

void de__ibl_shutdown(de__lighting *lt) {
    if (DE_HANDLE_VALID(lt->strip))
        de_gpu_texture_destroy(lt->dev, lt->strip); /* deferred until the GPU is done */
    if (DE_HANDLE_VALID(lt->ibl_sampler))
        de_gpu_sampler_destroy(lt->dev, lt->ibl_sampler);
    lt->strip = (de_gpu_texture){0};
    lt->ibl_sampler = (de_gpu_sampler){0};
}

void de__lighting_set_environment(de__lighting *lt, const de__ibl_environment *env) {
    if (lt == NULL)
        return;
    if (env != NULL && env->valid)
        lt->env = *env;
    else
        memset(&lt->env, 0, sizeof lt->env);
}

void de__lighting_prepare_environment(de__lighting *lt, de_asset_system *assets,
                                      const de__render_world_frame *frame) {
    if (lt == NULL)
        return;
    de__ibl_environment env;
    memset(&env, 0, sizeof env);
    if (assets != NULL && frame != NULL && frame->environment.entity != 0) {
        const DeEnvironment *e = &frame->environment.environment;
        const de_render_texture *t =
            DE_HANDLE_VALID(e->hdri) ? de_render_texture_get(assets, e->hdri) : NULL;
        if (t != NULL && t->has_env && t->dim == DE_TEX_CUBE && t->mip_count > 0) {
            env.valid = true;
            env.cube = t->texture;
            env.format = t->format;
            env.face_size = t->width;
            env.mip_count = t->mip_count;
            env.mip_per_roughness = t->env.mip_per_roughness;
            memcpy(env.sh9, t->env.sh9, sizeof env.sh9);
            env.intensity = e->intensity;
            env.rotation = e->rotation;
        }
    }
    de__lighting_set_environment(lt, &env);
}

/* =================================================================================================
 * The strip copy
 * ============================================================================================== */

typedef struct ibl_copy {
    de_rg_resource cube, buffer, strip;
    u32 mips, face;
    u64 base[16];  /**< each strip mip's offset in the buffer */
    u32 pitch[16]; /**< its row pitch */
    u32 rows[16];  /**< block rows per face */
} ibl_copy;

static void strip_read_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ibl_copy *c = (const ibl_copy *)user;
    const de_gpu_texture cube = de_rg_texture(pass, c->cube);
    const de_gpu_buffer buf = de_rg_buffer(pass, c->buffer);
    for (u32 m = 0; m < c->mips; ++m)
        for (u32 f = 0; f < DE__IBL_STRIP_FACES; ++f)
            de_gpu_cmd_copy_texture_to_buffer(
                cmd, buf, c->base[m] + (u64)f * c->rows[m] * c->pitch[m], c->pitch[m], cube, m, f);
}

static void strip_write_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ibl_copy *c = (const ibl_copy *)user;
    const de_gpu_texture strip = de_rg_texture(pass, c->strip);
    const de_gpu_buffer buf = de_rg_buffer(pass, c->buffer);
    for (u32 m = 0; m < c->mips; ++m)
        de_gpu_cmd_copy_buffer_to_texture(cmd, strip, m, 0, buf, c->base[m], c->pitch[m]);
}

static bool same_cube(const de__ibl_environment *a, const de__ibl_environment *b) {
    return a->valid && b->valid && a->cube.id == b->cube.id && a->format == b->format &&
           a->face_size == b->face_size && a->mip_count == b->mip_count;
}

/* Makes the strip for lt->env and adds the copy passes. False: none this frame (logged). */
static bool strip_make(de__lighting *lt, de_rg *g) {
    const de__ibl_environment *env = &lt->env;
    de_cooked_texture_block blk;
    const u32 mips = de__ibl_strip_mips(env->format, env->face_size, env->mip_count);
    if (mips == 0 || mips > 16u || !de_cooked_texture_format_block(env->format, &blk))
        return false;
    if (DE_HANDLE_VALID(lt->strip))
        de_gpu_texture_destroy(lt->dev, lt->strip); /* deferred until the GPU is done */
    lt->strip = de_gpu_texture_create(
        lt->dev, &(de_gpu_texture_desc){.format = (de_gpu_format)env->format,
                                        .width = env->face_size,
                                        .height = env->face_size * DE__IBL_STRIP_FACES,
                                        .mip_count = mips,
                                        .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST |
                                                 DE_USAGE_COPY_SRC, /* tests read it back */
                                        .name = "lighting environment strip"});
    lt->strip_ready = false;
    lt->strip_mips = 0;
    lt->strip_bindless = 0;
    memset(&lt->strip_env, 0, sizeof lt->strip_env);
    if (!DE_HANDLE_VALID(lt->strip)) {
        const de_error *e = de_error_last();
        DE_ERROR(lt->log, "lighting: the environment strip: %s", e != NULL ? e->message : "?");
        return false;
    }
    ibl_copy *c = (ibl_copy *)de_arena_push_zero(g->arena, sizeof *c, 16);
    if (c == NULL)
        return false;
    c->mips = mips;
    c->face = env->face_size;
    u64 at = 0;
    for (u32 m = 0; m < mips; ++m) {
        const u32 f = env->face_size >> m;
        const u32 bw = (f + blk.width - 1u) / blk.width;
        c->rows[m] = (f + blk.height - 1u) / blk.height;
        c->pitch[m] = (bw * blk.bytes + 511u) & ~511u; /* also a multiple of 256 */
        c->base[m] = at;
        at += (u64)c->pitch[m] * c->rows[m] * DE__IBL_STRIP_FACES;
        at = (at + 511u) & ~(u64)511u;
    }
    c->cube = de_rg_import_texture(g, env->cube, DE_ACCESS_SHADER_READ);
    c->strip = de_rg_import_texture(g, lt->strip, DE_ACCESS_NONE);
    c->buffer =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = at,
                                                     .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                                     .name = "lighting.ibl_strip_staging"});
    if (!DE_HANDLE_VALID(c->cube) || !DE_HANDLE_VALID(c->strip) || !DE_HANDLE_VALID(c->buffer))
        return false;
    de_rg_set_name(g, c->cube, "lighting.environment");
    de_rg_set_name(g, c->strip, "lighting.ibl_strip");
    de_rg_pass *p =
        de_rg_add_pass(g, "lighting.ibl_strip_read", DE_QUEUE_GRAPHICS, strip_read_execute, c);
    de_rg_read(p, c->cube, DE_ACCESS_COPY_SRC);
    de_rg_write(p, c->buffer, DE_ACCESS_COPY_DST);
    p = de_rg_add_pass(g, "lighting.ibl_strip_write", DE_QUEUE_GRAPHICS, strip_write_execute, c);
    de_rg_read(p, c->buffer, DE_ACCESS_COPY_SRC);
    de_rg_write(p, c->strip, DE_ACCESS_COPY_DST);
    de_rg_export(g, c->strip, DE_ACCESS_SHADER_READ);
    de_rg_export(g, c->cube, DE_ACCESS_SHADER_READ); /* the loader's state between graphs */
    lt->strip_env = *env;
    lt->strip_mips = mips;
    lt->strip_bindless = de_gpu_texture_bindless(lt->dev, lt->strip, 0, false);
    lt->strip_ready = true;
    return true;
}

void de__ibl_build(de__lighting *lt, de_rg *g, de__lighting_context *lc) {
    const f32 ambient = DE_HANDLE_VALID(lt->cv_ambient) ? (f32)de_cvar_float(lt->cv_ambient) : 0.0f;
    lc->strip = (de_rg_resource){0};
    bool have = false;
    if (lt->env.valid) {
        if (lt->strip_ready && same_cube(&lt->strip_env, &lt->env)) {
            lc->strip = de_rg_import_texture(g, lt->strip, DE_ACCESS_SHADER_READ);
            de_rg_set_name(g, lc->strip, "lighting.ibl_strip");
            have = DE_HANDLE_VALID(lc->strip);
        } else if (strip_make(lt, g)) {
            /* The import of this frame: the copy passes wrote it. */
            lc->strip = de_rg_import_texture(g, lt->strip, DE_ACCESS_NONE);
            have = DE_HANDLE_VALID(lc->strip);
        }
    }
    de__ibl_pack(have ? &lt->env : NULL, have ? lt->strip_bindless : 0u, have ? lt->strip_mips : 0u,
                 lt->ibl_sampler_bindless, ambient, &lc->view.ibl);
}
