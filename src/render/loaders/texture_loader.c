/*
 * texture_loader.c: the runtime `texture` type (SPEC §11.4, §11.6; DENG-105). Parses DTEX
 * (render/formats/de_cooked_texture.h, DENG-98) or a cooked environment, DTEX + ENVS
 * (de_cooked_env.h, DENG-100), creates the GPU texture (2D, 2D array or cube, every mip) and
 * stages one copy per subresource. The cooked layout already follows the copy rules (256-byte row
 * pitch, 512-byte subresource offsets), so every subresource goes from the ring to the texture as
 * it is. An environment keeps its trailer (SH9, roughness mapping, luminance) on the CPU.
 */

#include "render/loaders/loaders_internal.h"

#include "render/formats/de_cooked_env.h"
#include "render/formats/de_cooked_texture.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

#include <string.h>

static void env_copy(de_render_env *dst, const de_cooked_env_view *ev) {
    dst->face_size = ev->face_size;
    dst->mip_count = ev->mip_count;
    dst->sample_count = ev->sample_count;
    dst->mip_per_roughness = ev->mip_per_roughness;
    dst->rotation_deg = ev->rotation_deg;
    dst->clamp_luminance = ev->clamp_luminance;
    dst->mean_luminance = ev->mean_luminance;
    dst->max_luminance = ev->max_luminance;
    memcpy(dst->sh9, ev->sh9, sizeof dst->sh9);
}

static void texture_free(de_render_uploader *u, de_render_texture *t) {
    if (t == NULL)
        return;
    de__upload_release(u, t->upload_); /* first: the flush must not copy into a dead texture */
    t->upload_ = NULL;
    if (DE_HANDLE_VALID(t->texture))
        de_gpu_texture_destroy(u->dev, t->texture);
    DE_FREE(u->alloc, t, t->block_size_);
}

/* The retire list's free (DENG-422): the upload was released at the unload. */
static void texture_retired(de_render_uploader *u, void *object) {
    texture_free(u, (de_render_texture *)object);
}

de_result de__texture_load(de_asset_load_ctx *ctx) {
    de_render_uploader *u = de__loader_uploader(ctx);
    if (u == NULL)
        return DE_ERR_INTERNAL;
    u64 size = 0;
    const u8 *p = (const u8 *)de_asset_load_ctx_data(ctx, &size);

    /* An environment is a DTEX whose usage says so, followed by its trailer. */
    de_cooked_texture_view tv;
    de_cooked_env_view ev;
    memset(&ev, 0, sizeof ev);
    const char *why = NULL;
    const bool env = size >= DE_COOKED_TEXTURE_HEADER_SIZE &&
                     memcmp(p, DE_COOKED_TEXTURE_MAGIC, 4) == 0 &&
                     de_cooked_get_u32(p + 36) == (u32)DE_COOKED_TEXTURE_USAGE_ENVIRONMENT;
    if (env) {
        if (!de_cooked_env_parse(p, size, &ev, &why))
            return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "not a cooked environment: %s", why);
        tv = ev.texture;
    } else if (!de_cooked_texture_parse(p, size, &tv, &why)) {
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "not a cooked texture: %s", why);
    }

    de_render_texture *t = (de_render_texture *)DE_ALLOC(
        u->alloc, sizeof *t, DE_ALIGNOF(de_render_texture), DE_MEM_RENDER);
    if (t == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for the texture");
    memset(t, 0, sizeof *t);
    t->block_size_ = sizeof *t;
    t->dim = tv.dim;
    t->format = tv.format;
    t->width = tv.width;
    t->height = tv.height;
    t->layers = tv.depth_or_layers;
    t->mip_count = tv.mip_count;
    t->flags = tv.flags;
    t->usage = tv.usage;
    t->has_env = env;
    if (env)
        env_copy(&t->env, &ev);

    char name[48];
    de__loader_name(name, sizeof name, "texture", de_asset_load_ctx_id(ctx));
    const de_gpu_texture_desc td = {.dim = (de_gpu_texture_dim)tv.dim,
                                    .format = (de_gpu_format)tv.format,
                                    .width = tv.width,
                                    .height = tv.height,
                                    .depth_or_layers = tv.depth_or_layers,
                                    .mip_count = tv.mip_count,
                                    /* COPY_SRC: S20 streaming copies resident mips into a texture
                                     * with more or fewer of them, and readback tests read it. */
                                    .usage =
                                        DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                                    .name = name};
    t->texture = de_gpu_texture_create(u->dev, &td);
    if (!DE_HANDLE_VALID(t->texture)) {
        const de_result r = de__loader_rhi_error();
        char msg[DE_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof msg, "%s", de__loader_rhi_message());
        texture_free(u, t);
        return de_asset_load_ctx_fail(ctx, r, "cannot create the GPU texture: %s", msg);
    }
    t->bindless = de_gpu_texture_bindless(u->dev, t->texture, 0, false);

    /* One copy per subresource, straight from the cooked layout. */
    de__upload *up = de__upload_new(u, tv.subresource_count);
    if (up == NULL) {
        texture_free(u, t);
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for the upload");
    }
    t->upload_ = up;
    up->targets[0].texture = t->texture;
    up->target_count = 1;
    for (u32 i = 0; i < tv.subresource_count; ++i) {
        de_cooked_texture_subresource sr;
        memset(&sr, 0, sizeof sr);
        (void)de_cooked_texture_subresource_at(&tv, i, &sr);
        de__upload_piece *pc = &up->pieces[i];
        pc->blob_off = sr.offset;
        pc->size = sr.size;
        pc->row_pitch = sr.row_pitch;
        pc->mip = i % tv.mip_count;
        pc->layer = i / tv.mip_count;
        pc->target = 0;
        /* Resident bytes: the tight size of the data (the pitch padding is staging only). */
        t->gpu_bytes += de_cooked_texture_row_bytes(&tv.block, sr.width) * sr.row_count;
    }
    de_result r = de__upload_layout(u, up);
    if (r == DE_OK)
        r = de__upload_submit(u, up, p);
    if (r != DE_OK) {
        char msg[DE_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof msg, "%s", de__loader_rhi_message());
        texture_free(u, t);
        return de_asset_load_ctx_fail(ctx, r, "%s", msg);
    }
    de_asset_load_ctx_set_object(ctx, t, t->gpu_bytes);
    return DE_OK;
}

void de__texture_unload(void *object, void *user) {
    de_render_uploader *u = (de_render_uploader *)user;
    de_render_texture *t = (de_render_texture *)object;
    if (t == NULL)
        return;
    /* Now: no more copies into it. Later (frame fence, DENG-422): the texture and the record. */
    de__upload_release(u, t->upload_);
    t->upload_ = NULL;
    de__uploader_retire(u, &t->retire_, t, texture_retired);
}

de_result de__texture_ready(void *object, void *user) {
    return de__upload_poll((de_render_uploader *)user, ((de_render_texture *)object)->upload_);
}
