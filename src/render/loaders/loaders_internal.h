/*
 * loaders_internal.h: the callbacks of the four render asset types, shared between their files
 * and loaders.c, which registers them (DENG-105). Internal to de_render; include as
 * "render/loaders/loaders_internal.h". Every `user` pointer is the de_render_uploader.
 */
#ifndef DE_RENDER_LOADERS_LOADERS_INTERNAL_H
#define DE_RENDER_LOADERS_LOADERS_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include "render/loaders/upload_ring.h"

#include <stdio.h>

DE_EXTERN_C_BEGIN

/* texture_loader.c: DTEX and DTEX + ENVS. */
de_result de__texture_load(de_asset_load_ctx *ctx);
void de__texture_unload(void *object, void *user);
de_result de__texture_ready(void *object, void *user);

/* mesh_loader.c: DMSH v2. */
de_result de__mesh_load(de_asset_load_ctx *ctx);
void de__mesh_unload(void *object, void *user);
de_result de__mesh_ready(void *object, void *user);

/* material_loader.c: DMAT. */
de_result de__material_load(de_asset_load_ctx *ctx);
void de__material_unload(void *object, void *user);
de_result de__material_ready(void *object, void *user);

/* loaders.c: DMDL. */
de_result de__model_load(de_asset_load_ctx *ctx);
void de__model_unload(void *object, void *user);
de_result de__model_ready(void *object, void *user);

/** The uploader of a load; fails the load (returns NULL) when registration did not finish. */
static inline de_render_uploader *de__loader_uploader(de_asset_load_ctx *ctx) {
    de_render_uploader *u = (de_render_uploader *)de_asset_load_ctx_user(ctx);
    if (u == NULL || u->broken) {
        (void)de_asset_load_ctx_fail(ctx, DE_ERR_INTERNAL,
                                     "the render asset types did not finish registering");
        return NULL;
    }
    return u;
}

/** A debug name for the GPU objects of asset `id` ("<kind> 0123456789abcdef"). */
static inline void de__loader_name(char *buf, size_t cap, const char *kind, de_asset_id id) {
    snprintf(buf, cap, "%s %016llx", kind, (unsigned long long)id.hash);
}

/** The error code the RHI left in the error context, or DE_ERR_INTERNAL. */
static inline de_result de__loader_rhi_error(void) {
    const de_error *e = de_error_last();
    return e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL;
}

/** The message of the error context, or "". */
static inline const char *de__loader_rhi_message(void) {
    const de_error *e = de_error_last();
    return e != NULL ? e->message : "";
}

DE_EXTERN_C_END

#endif /* DE_RENDER_LOADERS_LOADERS_INTERNAL_H */
