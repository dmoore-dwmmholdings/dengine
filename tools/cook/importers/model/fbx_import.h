/*
 * fbx_import.h: the FBX front end of the model importer (DENG-102, SPEC §11.6, §5.5).
 * Cooker-library code (tools only). Include as "cook/importers/model/fbx_import.h" (or
 * "fbx_import.h" inside tools/cook/importers/model/). The fuzz target of DENG-106 calls
 * de_model_fbx_load directly.
 *
 * The model importer (model_cook.c) sends a source here when de_model_source_is_fbx says so; the
 * IR it gets back is already in engine axes and metres, so the importer then applies only the
 * `scale` setting (de_model_ir_convert_axes with DE_MODEL_AXES_ENGINE).
 */
#ifndef DE_COOK_MODEL_FBX_IMPORT_H
#define DE_COOK_MODEL_FBX_IMPORT_H

#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/**
 * Per-file axis overrides (import settings `axis_up` and `axis_front`), for files whose
 * GlobalSettings are wrong. NULL or "auto" keeps what the file says.
 *   - `axis_up`: "x", "y", "z", optionally signed ("+z", "-y").
 *   - `axis_front`: a signed axis ("+y", "-y", "+z", ...; "y" means "+y"), perpendicular to up.
 * With either override the source is taken as right-handed (right = up × front). An `axis_up`
 * override with `axis_front` "auto" keeps the file's front when it is perpendicular to the new up,
 * else uses the usual front for that up: +Z for X- and Y-up, -Y for Z-up.
 */
typedef struct de_model_fbx_options {
    const char *axis_up;
    const char *axis_front;
} de_model_fbx_options;

/** True when `src` is an FBX file: a `.fbx` path (any case) or the binary FBX magic. */
bool de_model_source_is_fbx(const de_model_source *src);

/**
 * Loads an FBX file (binary or ASCII, versions ufbx reads) from memory with ufbx and fills `ir`
 * in engine coordinates: Z-up, right-handed, metres, model front along -Y (SPEC §5.5, DENG-2).
 *   - Units: ufbx scales geometry, translations, bind matrices and animation to metres
 *     (target_unit_meters = 1, UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY).
 *   - Axes: a basis change from the file's axes (or `opt`) to engine axes, applied to everything
 *     (vertices, node TRS as C·M·C⁻¹, inverse bind matrices, animation), the way the glTF path
 *     converts; a left-handed source also flips the winding and the tangent sign.
 *   - Meshes: n-gons triangulated (ufbx_triangulate_face), one submesh per material in material
 *     order, vertices deduplicated per submesh in first-use order, UV v flipped (top-left origin,
 *     as glTF), at most 4 skin influences per vertex (the largest, renormalised).
 *   - Materials: PBR shader models through ufbx_material.pbr, FBX Lambert/Phong through a legacy
 *     conversion (fbx_material.c); embedded or relative-path textures become IR textures.
 *   - Skins from the skin deformers (inverse binds = geometry_to_bone); animation stacks baked
 *     with ufbx_bake_anim to linear keys at the file's frame rate.
 * `opt` may be NULL (both "auto"). `src->read_file` reads external textures (may be NULL).
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (a bad override); DE_ERR_PARSE (not FBX, corrupt or
 *         truncated, too large, bad skin data); DE_ERR_OUT_OF_MEMORY. The error context says why.
 */
de_result de_model_fbx_load(const de_model_source *src, const de_model_fbx_options *opt,
                            de_model_ir *ir);

DE_EXTERN_C_END

#endif /* DE_COOK_MODEL_FBX_IMPORT_H */
