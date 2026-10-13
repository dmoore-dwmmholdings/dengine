/*
 * anim_cook.h: skeleton and animation clip cooking of the model importer (DENG-104, SPEC §11.6,
 * §21.5). Private to tools/cook/importers/model/ (and its tests): anim_skeleton.c builds and
 * writes skeletons (DSKL, src/anim/formats/de_cooked_skeleton.h), anim_clip_compress.c resamples,
 * compresses and writes clips (DCLP, src/anim/formats/de_cooked_clip.h). model_cook.c calls
 * de_model_emit_skeletons_and_clips from its extension point.
 *
 * Importer settings (the model's manifest entry or `[defaults.model]`; all in the settings hash):
 *   - `anim_sample_rate` (float, Hz, default 30): the rate at which every track is sampled, on
 *     top of the source's own key times.
 *   - `anim_error` (float, metres, default 0.0001 = 0.1 mm): the largest error of a joint position
 *     in model space that compression may add.
 *   - `anim_error_angle` (float, degrees, default 0.05): the same for joint orientations.
 *   - `anim_joint_names` (bool, default true): keep the joint name strings in the skeleton (a Dev
 *     aid; the hashes are always there). A Shipping cook profile sets it false.
 */
#ifndef DE_COOK_MODEL_ANIM_COOK_H
#define DE_COOK_MODEL_ANIM_COOK_H

#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** The animation settings of one import. */
typedef struct de_model_anim_settings {
    f32 sample_rate; /**< Hz, > 0 */
    f32 error;       /**< metres, > 0 */
    f32 error_angle; /**< radians, > 0 */
    bool joint_names;
} de_model_anim_settings;

#define DE_MODEL_ANIM_DEFAULT_RATE        30.0
#define DE_MODEL_ANIM_DEFAULT_ERROR       0.0001
#define DE_MODEL_ANIM_DEFAULT_ERROR_ANGLE 0.05 /* degrees */

/** The defaults above (angle in radians), joint names on. */
void de_model_anim_settings_default(de_model_anim_settings *s);

/** Reads the settings of the import. DE_ERR_INVALID_ARGUMENT (error context set) on a bad value. */
de_result de_model_anim_settings_read(de_asset_import_ctx *ctx, de_model_anim_settings *out);

/* ---- Skeletons (anim_skeleton.c) ------------------------------------------------------------- */

/** A skeleton built from one IR skin. Arrays belong to the IR. */
typedef struct de_model_skeleton {
    u32 skin;             /**< IR skin index */
    u32 joint_count;      /**< skin joints and their ancestors */
    u32 *nodes;           /**< per joint: its IR node */
    u16 *parents;         /**< per joint: parent joint, DE_COOKED_SKEL_NO_PARENT for a root */
    i32 *joint_of_node;   /**< per IR node: its joint, -1 if not in this skeleton */
    u32 mesh_joint_count; /**< the skin's joint count */
    u16 *remap;           /**< per skin joint: its skeleton joint */
    u32 root_joint;       /**< the top skin joint (root motion) */
    f64 *rest_model;      /**< per joint: rest transform in model space, 12 f64 (3x4 col-major) */
    f32 *inverse_bind;    /**< per joint: 12 f32 (de_affine layout) */
    f64 *reach;           /**< per joint: distance to its farthest descendant, or bone length */
    i32 mesh_node;        /**< the first node skinned with it, -1 if none */
    i32 space_node;       /**< the node whose space is model space (below it: the joints), -1:
                               the scene */
    f64 mesh_from_model[12]; /**< model space to the mesh node's space */
} de_model_skeleton;

/**
 * Builds the skeleton of IR skin `skin` (the IR in engine coordinates): joints are the skin's
 * joints and their ancestors below the skinned mesh node's frame (see anim_skeleton.c), sorted
 * parent-before-child, stable in skin joint order.
 * @return DE_OK; DE_ERR_PARSE (a skin without joints, a parent cycle), DE_ERR_UNSUPPORTED (more
 *         than DE_COOKED_SKEL_MAX_JOINTS joints), DE_ERR_OUT_OF_MEMORY.
 */
de_result de_model_skeleton_build(de_model_ir *ir, u32 skin, de_model_skeleton *out);

/** Appends the DSKL blob of `s`; joint names are the nodes' sub-asset names (`names->nodes`). */
void de_model_skeleton_write(de_model_buf *b, const de_model_ir *ir, const de_model_skeleton *s,
                             const de_model_names *names, bool joint_names);

/* ---- f64 affine helpers (3x4, column-major like de_affine: m[c * 3 + r]) --------------------- */

void de_model_affine_from_trs(const f64 t[3], const f64 q[4], const f64 s[3], f64 out[12]);
void de_model_affine_mul(const f64 a[12], const f64 b[12], f64 out[12]);
/** False (and identity in `out`) when the linear part is singular. */
bool de_model_affine_inverse(const f64 a[12], f64 out[12]);

/* ---- Clips (anim_clip_compress.c) ------------------------------------------------------------ */

/** Numbers about one cooked clip (for logs and tests). */
typedef struct de_model_clip_stats {
    u32 tracks, const_tracks, uniform_tracks, raw_tracks;
    u32 times;        /**< samples in the time table */
    u32 keys;         /**< keys kept, over all non-constant tracks */
    u32 iterations;   /**< passes of the model-space check */
    u64 bytes;        /**< the DCLP blob */
    f64 max_position; /**< largest model-space position error at the samples (m) */
    f64 max_angle;    /**< largest model-space orientation error at the samples (rad) */
} de_model_clip_stats;

/**
 * The skeleton (index into `skels`) that animation `anim` animates most: the most channels whose
 * node is one of its joints; the first on a tie. -1 when none has a channel.
 */
i32 de_model_anim_pick_skeleton(const de_model_ir *ir, const de_model_anim *anim,
                                const de_model_skeleton *skels, u32 skel_count);

/**
 * Appends the DCLP blob of `anim` over skeleton `skel` (asset ID `skeleton_id`): resampling,
 * constant-track detection, quantization, key reduction and the model-space error check (see
 * anim_clip_compress.c). Channels on nodes outside the skeleton and morph weights are skipped with
 * a warning. `stats` is nullable.
 * @return DE_OK; DE_ERR_PARSE (key times not increasing, not finite or negative),
 *         DE_ERR_UNSUPPORTED (more than DE_COOKED_CLIP_MAX_TIMES samples), DE_ERR_OUT_OF_MEMORY.
 */
de_result de_model_clip_write(de_model_buf *b, de_model_ir *ir, const de_model_anim *anim,
                              const de_model_skeleton *skel, u64 skeleton_id,
                              const de_model_anim_settings *s, de_model_clip_stats *stats);

/**
 * Smallest-three code of a quaternion (normalised first): three u16 words of the 48-bit value (see
 * de_cooked_clip.h) and the value the sampler decodes from them. q and -q give the same code.
 */
void de_model_clip_s3_encode(const f64 q[4], u16 w[3], f32 dec[4]);

/* ---- The model_cook.c extension point -------------------------------------------------------- */

/**
 * Emits the skeleton sub-assets (`skeleton:<name>`, type `skeleton`, one per IR skin in skin
 * order) and the clip sub-assets (`anim:<name>`, type `anim_clip`, one per animation that
 * animates a skeleton, in animation order). Fills the IDs and counts for the model (DMDL).
 * Animations of a model without skins are not cooked (a warning).
 */
de_result de_model_emit_skeletons_and_clips(de_asset_import_ctx *ctx, de_model_ir *ir,
                                            const de_model_names *names, de_asset_id *skeleton_ids,
                                            u32 *skeleton_count, de_asset_id *clip_ids,
                                            u32 *clip_count);

DE_EXTERN_C_END

#endif /* DE_COOK_MODEL_ANIM_COOK_H */
