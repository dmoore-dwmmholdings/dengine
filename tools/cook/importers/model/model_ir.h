/*
 * model_ir.h: the model IR (DENG-101, SPEC §11.6): what every model front end (glTF here, FBX in
 * DENG-102) produces and every model cook stage consumes (meshes and materials here, meshlets and
 * LODs in DENG-103, skeletons and clips in DENG-104). Cooker-library code (tools only); include
 * as "model_ir.h" inside tools/cook/importers/model/.
 *
 * Conventions:
 *   - A front end fills the IR in the source's own coordinates; de_model_ir_convert_axes (in
 *     model_axes.c) then converts everything to engine coordinates (Z-up, right-handed, metres,
 *     SPEC §5.5) and applies the import `scale`. After that, every position, direction, node
 *     transform, inverse bind matrix and animation track is in engine space.
 *   - Arrays keep source (document) order. Nothing is ever ordered by a hash or a pointer, so the
 *     same source gives the same IR on every run (SPEC §11.5).
 *   - Every allocation of the IR comes from de_model_ir_alloc (its allocator); de_model_ir_free
 *     frees them all. Strings are NUL-terminated copies.
 *   - Indices into IR arrays are u32; "none" is -1 in the i32 fields.
 *
 * Stability: DENG-102, DENG-103 and DENG-104 build on these types. Add fields at the end of a
 * struct; do not change the meaning of existing ones.
 */
#ifndef DE_MODEL_IR_H
#define DE_MODEL_IR_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "render/formats/de_cooked_material.h"

DE_EXTERN_C_BEGIN

/* ---- Meshes ---------------------------------------------------------------------------------- */

/** Vertex attributes; the bit of attribute `a` in a mask is (1u << a). Same order as
 * de_cooked_stream. */
typedef enum de_model_attr {
    DE_MODEL_ATTR_POSITION = 0, /**< f32 x3 */
    DE_MODEL_ATTR_NORMAL,       /**< f32 x3, unit length */
    DE_MODEL_ATTR_TANGENT,      /**< f32 x4: xyz unit length, w = bitangent sign */
    DE_MODEL_ATTR_UV0,          /**< f32 x2 */
    DE_MODEL_ATTR_UV1,          /**< f32 x2 */
    DE_MODEL_ATTR_COLOR0,       /**< f32 x4, linear RGBA */
    DE_MODEL_ATTR_JOINTS0,      /**< u16 x4, indices into the skin's joint list */
    DE_MODEL_ATTR_WEIGHTS0,     /**< f32 x4 */
    DE_MODEL_ATTR_COUNT
} de_model_attr;

#define DE_MODEL_ATTR_BIT(a) (1u << (u32)(a))

/** A range of a mesh drawn with one material (a glTF primitive). */
typedef struct de_model_submesh {
    u32 index_first, index_count;   /**< into de_model_mesh.indices; index_count % 3 == 0 */
    u32 vertex_first, vertex_count; /**< the vertices the range's indices use */
    i32 material;                   /**< into de_model_ir.materials, -1 = none */
    u32 attr_mask; /**< the attributes the source gave (or the importer generated) here; the others
                        are zero in this range (colour: 1) */
    f32 bounds_min[3], bounds_max[3];
} de_model_submesh;

/**
 * One mesh: non-interleaved vertex streams (each NULL when the mesh has no such attribute; an
 * attribute is present when any submesh has it), u32 triangle-list indices, submeshes.
 */
typedef struct de_model_mesh {
    const char *name; /**< source name, "" if none */
    u32 vertex_count, index_count, submesh_count;
    u32 attr_mask;
    f32 *position, *normal, *tangent;
    f32 *uv[2];
    f32 *color0;
    u16 *joints0;
    f32 *weights0;
    u32 *indices;
    de_model_submesh *submeshes;
    f32 bounds_min[3], bounds_max[3];
    u32 morph_target_count; /**< not imported (a warning); kept for DENG-104 */
} de_model_mesh;

/* ---- Nodes ----------------------------------------------------------------------------------- */

typedef struct de_model_node {
    const char *name; /**< source name, "" if none */
    i32 parent;       /**< -1 for a root */
    i32 mesh;         /**< into de_model_ir.meshes, -1 = none */
    i32 skin;         /**< into de_model_ir.skins, -1 = none */
    f32 translation[3];
    f32 rotation[4]; /**< unit quaternion x, y, z, w */
    f32 scale[3];
    bool in_scene; /**< part of the imported scene (de_model_ir.roots and their descendants) */
} de_model_node;

/* ---- Skins and animations (raw; DENG-104 cooks them) ----------------------------------------- */

typedef struct de_model_skin {
    const char *name;
    u32 joint_count;
    u32 *joints;       /**< node indices */
    f32 *inverse_bind; /**< 16 per joint, column-major (identity when the source has none) */
    i32 skeleton_root; /**< node index, -1 if not given */
} de_model_skin;

typedef enum de_model_anim_path {
    DE_MODEL_ANIM_TRANSLATION = 0, /**< 3 floats per key */
    DE_MODEL_ANIM_ROTATION,        /**< 4 floats per key (x, y, z, w) */
    DE_MODEL_ANIM_SCALE,           /**< 3 floats per key */
    DE_MODEL_ANIM_WEIGHTS          /**< morph target count floats per key */
} de_model_anim_path;

typedef enum de_model_anim_interp {
    DE_MODEL_INTERP_LINEAR = 0,
    DE_MODEL_INTERP_STEP,
    DE_MODEL_INTERP_CUBIC /**< per key: in-tangent, value, out-tangent */
} de_model_anim_interp;

typedef struct de_model_anim_channel {
    u32 node;
    de_model_anim_path path;
    de_model_anim_interp interp;
    u32 key_count;
    u32 components; /**< floats per value */
    f32 *times;     /**< key_count seconds */
    f32 *values;    /**< key_count x components (x 3 for CUBIC) */
} de_model_anim_channel;

typedef struct de_model_anim {
    const char *name;
    u32 channel_count;
    de_model_anim_channel *channels;
    f32 duration; /**< the largest key time */
} de_model_anim;

/* ---- Materials and textures ------------------------------------------------------------------ */

/** How a texture is sampled: decides its texture-importer settings (srgb, usage). */
typedef enum de_model_tex_usage {
    DE_MODEL_TEX_COLOR = 0, /**< sRGB colour: usage "color", srgb = true */
    DE_MODEL_TEX_NORMAL,    /**< normal map: usage "normal", srgb = false */
    DE_MODEL_TEX_LINEAR     /**< linear data (masks, roughness, ...): usage "mask", srgb = false */
} de_model_tex_usage;

/**
 * One image as one texture sub-asset: an image used with two usages (colour and linear) becomes
 * two textures. Created in order of first use (materials in order, then slots in order).
 */
typedef struct de_model_texture {
    const char *name; /**< source image name, "" if none */
    u32 image;        /**< source image index */
    de_model_tex_usage usage;
    const void *data; /**< the encoded image file (PNG, JPEG, ...) */
    u64 size;
    const char *mime; /**< "" if unknown */
} de_model_texture;

typedef struct de_model_tex_ref {
    i32 texture;  /**< into de_model_ir.textures, -1 = none */
    u32 texcoord; /**< 0 or 1 */
} de_model_tex_ref;

typedef struct de_model_material {
    const char *name; /**< source name, "" if none */
    de_cooked_material_params params;
    u32 flags;      /**< DE_COOKED_MAT_* */
    u32 alpha_mode; /**< de_cooked_alpha_mode */
    de_model_tex_ref tex[DE_COOKED_TEX_COUNT];
} de_model_material;

/* ---- The IR ---------------------------------------------------------------------------------- */

typedef struct de__model_block de__model_block;

typedef struct de_model_ir {
    de_model_mesh *meshes;
    u32 mesh_count;
    de_model_node *nodes;
    u32 node_count;
    u32 *roots; /**< the imported scene's root nodes, in scene order */
    u32 root_count;
    de_model_skin *skins;
    u32 skin_count;
    de_model_anim *anims;
    u32 anim_count;
    de_model_material *materials;
    u32 material_count;
    de_model_texture *textures;
    u32 texture_count;

    /** Warnings of the import, in order (also logged to the `cook` category). */
    const char **warnings;
    u32 warning_count, warning_cap;
    /** Prefix of logged warnings (the asset ID or source path). */
    const char *log_name;

    const de_allocator *alloc;
    de__model_block *blocks;
} de_model_ir;

/** Starts an empty IR whose memory comes from `a` (NULL = the heap). */
void de_model_ir_init(de_model_ir *ir, const de_allocator *a, const char *log_name);
/** Frees everything de_model_ir_alloc gave out; the IR is empty afterwards. */
void de_model_ir_free(de_model_ir *ir);
/** `size` zeroed bytes (16-byte aligned) owned by the IR, or NULL with DE_ERR_OUT_OF_MEMORY in the
 * error context (also for a size that overflows). */
void *de_model_ir_alloc(de_model_ir *ir, u64 size);
/** `count` x `elem` zeroed bytes (overflow-checked). */
void *de_model_ir_array(de_model_ir *ir, u64 count, u64 elem);
/** A copy of `s` (NULL → ""). */
const char *de_model_ir_strdup(de_model_ir *ir, const char *s);
/** Records and logs a warning (printf-style). */
void de_model_ir_warn(de_model_ir *ir, const char *fmt, ...) DE_PRINTF_FMT(2, 3);

/* ---- Geometry helpers (model_ir.c) ----------------------------------------------------------- */

/**
 * Area-weighted vertex normals for the triangles `indices[0 .. index_count)` (indices into
 * `position`, relative to `vertex_first`): each vertex gets the normalised sum of the face
 * normals (cross products, so weighted by area) of the triangles that use it. A vertex without a
 * non-degenerate triangle gets (0, 0, 1). Writes `normal[3 * (vertex_first + i)]` for the
 * `vertex_count` vertices.
 */
void de_model_generate_normals(const f32 *position, f32 *normal, u32 vertex_first, u32 vertex_count,
                               const u32 *indices, u32 index_count);
/**
 * MikkTSpace tangents for one submesh: positions, normals and `uv` (2 per vertex) of the vertices
 * the indices name; writes `tangent` (4 per vertex, w = ±1). Vertices shared by triangles with
 * different tangent frames keep the frame MikkTSpace gives the last of them (the importer does
 * not split vertices). Returns false when MikkTSpace fails (out of memory).
 */
bool de_model_generate_tangents(const f32 *position, const f32 *normal, const f32 *uv, f32 *tangent,
                                const u32 *indices, u32 index_count);
/** Recomputes the bounds of every submesh and mesh from the positions. */
void de_model_ir_compute_bounds(de_model_ir *ir);

/* ---- Axis conversion (model_axes.c) ---------------------------------------------------------- */

/** Source coordinate systems a front end can declare. */
typedef enum de_model_axes {
    DE_MODEL_AXES_ENGINE = 0, /**< Z-up, right-handed: no rotation */
    DE_MODEL_AXES_GLTF        /**< Y-up, right-handed, +Z front: (x, y, z) → (x, −z, y) */
} de_model_axes;

/**
 * Converts the whole IR to engine coordinates (SPEC §5.5) and applies the uniform import `scale`
 * (> 0). With C the rotation of `from` (glTF: +90° about X) and U = scale·I:
 *   - positions p → U·C·p; normals n → C·n; tangents xyz → C·xyz, w unchanged (det C = +1);
 *   - node TRS (local matrix M) → U·C·M·C⁻¹·U⁻¹: translation → U·C·t, rotation q → C·q·C⁻¹
 *     (axis rotated, angle kept), scale → the axis-permuted scale;
 *   - inverse bind matrices B → U·C·B·C⁻¹·U⁻¹;
 *   - animation tracks: translations, rotations and scales like node TRS (cubic tangents too).
 * Bounds are not updated: call de_model_ir_compute_bounds afterwards.
 */
void de_model_ir_convert_axes(de_model_ir *ir, de_model_axes from, f32 scale);

/** The 3x3 rotation of `from` (row-major m[row][col]); identity for DE_MODEL_AXES_ENGINE. */
void de_model_axes_matrix(de_model_axes from, f32 m[3][3]);

/**
 * Splits a column-major affine 4x4 matrix into translation, rotation (unit quaternion x, y, z, w)
 * and scale. A negative determinant flips the scale's x. Returns false (and still writes the
 * best-effort split) when the matrix has shear or a projective row.
 */
bool de_model_decompose_matrix(const f32 m[16], f32 t[3], f32 r[4], f32 s[3]);

/* ---- Sub-asset names (model_cook.c) ---------------------------------------------------------- */

/**
 * The unique sub-asset names of one import, `<model>#<name>` (SPEC §11.1):
 *   - `prefab` is reserved for the prefab;
 *   - nodes `<NodeName>`, materials `material:<Name>`, textures `texture:<Name>`, skins
 *     `skeleton:<Name>`, animations `anim:<Name>`, meshes that no node uses `mesh:<Name>`;
 *   - an empty source name becomes `<kind><index>` (`node3`, `material0`, `texture2`, ...);
 *   - characters an asset ID refuses (control, `\`, `*`, `?`, `#`, invalid UTF-8) become `_`;
 *   - a name taken already gets `~2`, `~3`, ... in that order: prefab, nodes, materials, textures,
 *     skins, animations, unused meshes, each in document order.
 * A mesh used by nodes is named after the first node (in node order) that uses it: its ID is
 * `<model>#<that node's name>`; every node that uses it points at that ID.
 */
typedef struct de_model_names {
    const char *prefab;
    const char **nodes, **materials, **textures, **skins, **anims;
    const char **meshes; /**< per mesh: the sub-asset name of its cooked mesh */
} de_model_names;

/** Builds the names (allocated in the IR). DE_OK or DE_ERR_OUT_OF_MEMORY. */
de_result de_model_names_build(de_model_ir *ir, de_model_names *out);

DE_EXTERN_C_END

#endif /* DE_MODEL_IR_H */
