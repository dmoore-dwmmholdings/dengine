/*
 * model_internal.h: what the files of the model importer share (DENG-101): the byte builder, the
 * glTF front end, the material, model and prefab writers, and the mesh stage (DENG-103: LODs,
 * quantization, meshlets, the DMSH v2 writer). Private to
 * tools/cook/importers/model/; the IR is in model_ir.h, the importer entry in model_import.h.
 */
#ifndef DE_MODEL_INTERNAL_H
#define DE_MODEL_INTERNAL_H

#include "model_ir.h"

#include "render/formats/de_cooked_mesh.h"

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

/* ---- The mesh stage (DENG-103): DMSH v2 ------------------------------------------------------ */

/*
 * One IR mesh becomes one DMSH v2 blob (render/formats/de_cooked_mesh.h) in these steps:
 *   1. the LOD chain (mesh_lod.c): LOD 0 is the source triangles; each further LOD simplifies
 *      LOD 0 per submesh to half the triangles of the LOD before it;
 *   2. per LOD and submesh: vertex cache, then overdraw optimization (mesh_lod.c);
 *   3. vertex fetch order over all LODs, coarsest first, which also drops unused vertices;
 *   4. quantization of every attribute (mesh_quantize.c);
 *   5. meshlets per LOD and submesh, with bounds from the quantized positions (mesh_meshlets.c);
 *   6. the blob (mesh_cook.c).
 * Every step is deterministic for the same IR: meshoptimizer is, and nothing iterates a hash.
 */

/** The model importer settings the mesh stage reads (all are in the settings hash). */
typedef struct de_model_mesh_settings {
    bool generate_lods;     /**< `generate_lods`, default true: false keeps LOD 0 only */
    bool meshlets;          /**< `meshlets`, default true: false writes indexed LODs only */
    f32 lod_error;          /**< `lod_error`, default 0.01: stored in the header (DMSH v2) */
    f32 max_position_error; /**< `max_position_error`, metres, default 0.0005 */
} de_model_mesh_settings;

/** Simplification and meshlet parameters (shared with the tests that reproduce them). */
#define DE_MODEL_LOD_MIN_TRIANGLES   64u   /**< no further LOD below this many triangles */
#define DE_MODEL_LOD_KEEP_RATIO      0.9   /**< a LOD must have at most 90% of the triangles */
#define DE_MODEL_LOD_NORMAL_WEIGHT   0.5f  /**< simplifier weight of each normal component */
#define DE_MODEL_LOD_UV_WEIGHT       1.0f  /**< simplifier weight of each UV0 component */
#define DE_MODEL_OVERDRAW_THRESHOLD  1.05f /**< meshopt_optimizeOverdraw threshold */
#define DE_MODEL_MESHLET_CONE_WEIGHT 0.25f /**< meshopt_buildMeshlets cone weight */

/**
 * Appends the DMSH v2 blob of mesh `m` (with at least one submesh); `material_ids[i]` is the
 * asset ID of IR material i. Errors: DE_ERR_OUT_OF_MEMORY, DE_ERR_UNSUPPORTED (more than
 * DE_COOKED_MESH_MAX_SUBMESHES submeshes, a blob over 4 GiB), with the error context set.
 */
de_result de_model_mesh_cook(de_model_ir *ir, const de_model_mesh *m,
                             const de_model_mesh_settings *s, const de_asset_id *material_ids,
                             de_model_buf *out);

/* ---- LOD chain and index optimization (mesh_lod.c) ------------------------------------------- */

/** One LOD: the triangles of every submesh, submesh by submesh, as mesh vertex indices. */
typedef struct de_model_lod {
    u32 *indices;
    u32 index_count;
    u32 *sub_first, *sub_count; /**< per submesh: its range of `indices` */
    f32 error;                  /**< object-space error in metres (0 for LOD 0) */
} de_model_lod;

typedef struct de_model_lods {
    u32 count; /**< 1 to DE_COOKED_MESH_MAX_LODS */
    de_model_lod lod[DE_COOKED_MESH_MAX_LODS];
} de_model_lods;

/**
 * The seam locks of a mesh with several submeshes (NULL for one submesh): one byte per mesh
 * vertex, meshopt_SimplifyVertex_Lock when a vertex at the same position (bit-exact, -0 = +0)
 * belongs to another submesh, so simplification opens no crack between submeshes.
 */
u8 *de_model_lod_seam_locks(de_model_ir *ir, const de_model_mesh *m);

/**
 * Builds the LOD chain of `m`. LOD 0 is the source triangles. LOD k (k >= 1) simplifies the
 * LOD 0 triangles of each submesh (meshopt_simplifyWithAttributes, normals and UV0 weighted,
 * seam vertices locked) to floor(t0 / 2^k) triangles, t0 being the submesh's LOD 0 count; the
 * chain stops after DE_COOKED_MESH_MAX_LODS LODs, when the previous LOD has fewer than
 * DE_MODEL_LOD_MIN_TRIANGLES triangles, or when a LOD would keep more than
 * DE_MODEL_LOD_KEEP_RATIO of the previous one's triangles (it is dropped). The error of a LOD
 * is the largest over its submeshes of result_error x meshopt_simplifyScale(the submesh's
 * vertices), and at least the error of the LOD before it. `generate` false: LOD 0 only.
 */
de_result de_model_lods_build(de_model_ir *ir, const de_model_mesh *m, bool generate,
                              de_model_lods *out);

/** Vertex cache, then overdraw (DE_MODEL_OVERDRAW_THRESHOLD) optimization of each submesh
 * range of each LOD. Triangles keep their winding; each range keeps its triangle set. */
de_result de_model_lods_optimize(de_model_ir *ir, const de_model_mesh *m, de_model_lods *lods);

/**
 * Vertex fetch order: numbers the vertices in order of first use over the LODs from the
 * coarsest to LOD 0, so each LOD uses a prefix of the vertices, and drops vertices no LOD
 * uses. Rewrites every LOD's indices; `*order` gets, per new vertex, its IR vertex index, and
 * `*vertex_count` the new count (0 when no LOD has a triangle).
 */
de_result de_model_lods_vertex_order(de_model_ir *ir, const de_model_mesh *m, de_model_lods *lods,
                                     u32 **order, u32 *vertex_count);

/* ---- Quantization (mesh_quantize.c) ---------------------------------------------------------- */

/** The vertex formats of one mesh and their dequantization parameters. */
typedef struct de_model_quant {
    u32 format[DE_MODEL_ATTR_COUNT]; /**< de_cooked_vertex_format per present attribute, else 0 */
    f32 pos_offset[3], pos_scale[3]; /**< UNORM16X4: p = offset + q * scale */
    f32 uv_offset[2][2], uv_scale[2][2];
} de_model_quant;

/**
 * Chooses the formats of the vertices `order[0 .. count)` of `m`:
 *   - positions UNORM16X4 in their AABB, unless a 16-bit step of an axis exceeds
 *     `max_position_error` or a decoded position would be further than it from the source
 *     position: then F32X3;
 *   - normals OCT16; tangents OCT16_SIGN;
 *   - UVs UNORM16X2 in their bounds, F32X2 when an axis range exceeds
 *     DE_COOKED_UV_UNORM_MAX_RANGE or a value is not finite;
 *   - colours and weights UNORM8X4; joints U8X4, U16X4 when an index is above 255.
 */
void de_model_quant_choose(const de_model_mesh *m, const u32 *order, u32 count,
                           f32 max_position_error, de_model_quant *q);

/** Encodes attribute `attr` of IR vertex `v` in its chosen format (the format's size, LE). */
void de_model_quant_encode(const de_model_quant *q, const de_model_mesh *m, u32 attr, u32 v,
                           u8 *out);

/** The position of IR vertex `v` as the runtime decodes it (de_cooked_mesh_position). */
void de_model_quant_position(const de_model_quant *q, const de_model_mesh *m, u32 v, f32 out[3]);

/** Octahedral snorm16 encoding of a unit vector: of the four nearest codes, the one that decodes
 * closest to `n`. With `sign_bit` >= 0, only codes whose y has bit 0 equal to it. */
void de_model_oct_encode(const f32 n[3], i32 sign_bit, i16 out[2]);

/** Skin weights as unorm8 that sum to exactly 255: largest remainders get the rounding; negative
 * weights count as 0, and all-zero weights become (255, 0, 0, 0). */
void de_model_weights_unorm8(const f32 w[4], u8 out[4]);

/* ---- Meshlets (mesh_meshlets.c) -------------------------------------------------------------- */

typedef struct de_model_meshlet {
    u32 vertex_offset, triangle_offset; /**< into the vertex list, the triangle bytes */
    u32 vertex_count, triangle_count, submesh;
    f32 center[3], radius;
    f32 cone_apex[3], cone_axis[3], cone_cutoff;
    i8 cone_s8[4];
} de_model_meshlet;

/** Meshlets of a whole mesh, appended LOD by LOD and submesh by submesh. */
typedef struct de_model_meshlets {
    de_model_meshlet *items;
    u32 count, cap;
    u32 *vertices; /**< mesh vertex index per meshlet vertex */
    u32 vertex_count, vertex_cap;
    u8 *triangles; /**< 3 local indices per triangle; each meshlet padded to 4 bytes */
    u32 triangle_bytes, triangle_cap;
} de_model_meshlets;

/** Reserves room for the meshlets of index lists of `index_total` indices in `range_count`
 * (LOD, submesh) ranges. */
de_result de_model_meshlets_init(de_model_ir *ir, de_model_meshlets *ml, u64 index_total,
                                 u32 range_count);

/**
 * Splits one (LOD, submesh) triangle list into meshlets of at most
 * DE_COOKED_MESHLET_MAX_VERTICES vertices and DE_COOKED_MESHLET_MAX_TRIANGLES triangles
 * (meshopt_buildMeshlets, cone weight DE_MODEL_MESHLET_CONE_WEIGHT, then
 * meshopt_optimizeMeshlet) and appends them with their bounds (meshopt_computeMeshletBounds on
 * `positions`, the decoded positions). Each sphere radius is widened until it contains every
 * vertex of its meshlet with a margin. Every triangle lands in exactly one meshlet, winding kept.
 */
de_result de_model_meshlets_add(de_model_ir *ir, de_model_meshlets *ml, const u32 *indices,
                                u32 index_count, const f32 *positions, u32 vertex_count,
                                u32 submesh);

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
