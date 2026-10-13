/*
 * model_internal.h: what the files of the model importer share (DENG-101): the byte builder, the
 * glTF front end, the material, mesh, model and prefab writers. Private to
 * tools/cook/importers/model/; the IR is in model_ir.h, the importer entry in model_import.h.
 */
#ifndef DE_MODEL_INTERNAL_H
#define DE_MODEL_INTERNAL_H

#include "model_ir.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/* ---- Byte builder ---------------------------------------------------------------------------- */

/** A growable byte buffer whose blocks belong to an IR (freed with it). `failed` sticks. */
typedef struct de_model_buf {
    de_model_ir *ir;
    u8 *data;
    u64 size, cap;
    bool failed;
} de_model_buf;

void de_model_buf_init(de_model_buf *b, de_model_ir *ir);
/** Appends `n` bytes (`p` NULL: zeros). Returns the offset they start at. */
u64 de_model_buf_put(de_model_buf *b, const void *p, u64 n);
void de_model_buf_u32(de_model_buf *b, u32 v);
void de_model_buf_u64(de_model_buf *b, u64 v);
void de_model_buf_f32(de_model_buf *b, f32 v);
/** Zero bytes up to a multiple of `align`. */
void de_model_buf_align(de_model_buf *b, u64 align);
/** printf-style text (without the NUL). */
void de_model_buf_printf(de_model_buf *b, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
/** Overwrites a u32 written earlier at `off`. */
void de_model_buf_patch_u32(de_model_buf *b, u64 off, u32 v);

/* ---- Front ends ------------------------------------------------------------------------------ */

/**
 * Reads a file named by a source (a glTF buffer or image URI), relative to the source's
 * directory. The bytes stay valid until the import ends. Returns DE_OK or an error with the
 * error context set.
 */
typedef de_result (*de_model_read_fn)(void *user, const char *rel_path, const void **data,
                                      u64 *size);

typedef struct de_model_source {
    const void *data; /**< the source file */
    u64 size;
    const char *path; /**< for messages */
    de_model_read_fn read_file;
    void *read_user;
} de_model_source;

/**
 * The glTF 2.0 front end (gltf_import.c): parses `.gltf` (JSON) or `.glb` (binary, detected by
 * its magic), resolves buffers and images (data URIs, buffer views, or files through
 * `read_file`), and fills `ir` in glTF coordinates (no axis conversion; see
 * de_model_ir_convert_axes). Missing normals are generated, missing tangents of primitives with a
 * normal map are generated with MikkTSpace, KHR_texture_transform is baked into the UVs.
 * @return DE_OK; DE_ERR_PARSE (not glTF, bad JSON, invalid data, an unsupported required
 *         extension), DE_ERR_UNSUPPORTED (Draco or meshopt compression required), DE_ERR_NOT_FOUND
 *         / DE_ERR_IO (a referenced file), DE_ERR_OUT_OF_MEMORY; the error context says why.
 */
de_result de_model_gltf_load(const de_model_source *src, de_model_ir *ir);

/* ---- Materials (model_material.c) ------------------------------------------------------------ */

/** How the texture of material slot `slot` is sampled. */
de_model_tex_usage de_model_tex_usage_of_slot(u32 slot);
/** The texture-importer settings of a usage: `srgb` and `usage` ("color", "normal", "mask"). */
bool de_model_tex_usage_srgb(de_model_tex_usage u);
const char *de_model_tex_usage_name(de_model_tex_usage u);
/** Appends the DMAT blob of `m`; `texture_ids[i]` is the asset ID of IR texture i. */
void de_model_material_write(de_model_buf *b, const de_model_material *m,
                             const de_asset_id *texture_ids);

/* ---- Meshes and the model (model_cook.c) ----------------------------------------------------- */

/** Appends the DMSH blob of mesh `m`; `material_ids[i]` is the asset ID of IR material i. */
void de_model_mesh_write(de_model_buf *b, const de_model_mesh *m, const de_asset_id *material_ids);

/* ---- Prefab (model_prefab.c) ----------------------------------------------------------------- */

/**
 * Appends the prefab TOML of the IR (the S08 scene/prefab source format, DENG-215): one root
 * entity for the model, then one entity per node of the imported scene in node order, each with
 * `DeTransform` and, for a node with a mesh, `DeMeshRenderer { mesh, materials }` naming
 * sub-assets of `model_id` (the model's asset ID string). Scene IDs are XXH3-64 of the node's
 * sub-asset name (the root: of "#root"), so they do not change when another node is added.
 * Returns false when two scene IDs collide (the TOML is not usable).
 */
bool de_model_prefab_toml(de_model_buf *b, const de_model_ir *ir, const de_model_names *names,
                          const char *model_id, const char *source_path);

DE_EXTERN_C_END

#endif /* DE_MODEL_INTERNAL_H */
