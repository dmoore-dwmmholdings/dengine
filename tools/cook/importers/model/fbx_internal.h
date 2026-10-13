/*
 * fbx_internal.h: what fbx_import.c and fbx_material.c share (DENG-102). Private to
 * tools/cook/importers/model/; it names ufbx types, so only files that link de_tp_ufbx include it.
 * The entry point is in fbx_import.h.
 */
#ifndef DE_COOK_MODEL_FBX_INTERNAL_H
#define DE_COOK_MODEL_FBX_INTERNAL_H

#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_base.h>

#include <ufbx.h>

DE_EXTERN_C_BEGIN

/** One FBX load: the source, the IR being filled and the ufbx scene. */
typedef struct de__fbx_ctx {
    const de_model_source *src;
    de_model_ir *ir;
    const ufbx_scene *scene;
    u32 texture_cap; /**< capacity of ir->textures */
} de__fbx_ctx;

/**
 * Fills IR material `out` from `m` (fbx_material.c): ufbx_material.pbr for PBR shader models, a
 * legacy Lambert/Phong conversion for FBX's own models. Creates the IR textures it uses (one per
 * (file, usage), in order of first use). A texture that is neither embedded nor readable through
 * the source's reader is skipped with a warning.
 * @return DE_OK or DE_ERR_OUT_OF_MEMORY.
 */
de_result de__fbx_material(de__fbx_ctx *f, const ufbx_material *m, de_model_material *out);

DE_EXTERN_C_END

#endif /* DE_COOK_MODEL_FBX_INTERNAL_H */
