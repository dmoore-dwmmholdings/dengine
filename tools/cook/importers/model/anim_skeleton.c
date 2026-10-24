/*
 * anim_skeleton.c: skeletons of the model importer (DENG-104, SPEC §11.6, §21.5): one DSKL
 * sub-asset per IR skin (src/anim/formats/de_cooked_skeleton.h), the settings of the animation
 * cook, and the model_cook.c extension point that emits skeletons and clips.
 *
 * Joints: glTF skins a mesh with the joints' world transforms and ignores the mesh node's own
 * transform, and exporters write the inverse bind matrices for the mesh's frame: the world
 * transform of a joint times its inverse bind matrix is the mesh node's world transform. Nodes
 * above both the joints and the mesh node (a Y-up-to-Z-up node, often the "Armature" node) cancel
 * out, so the skeleton is the skin's joints plus their ancestors below that shared frame S, and
 * model space is the space of S. Ancestors below S (an "Armature" node beside the mesh) become
 * joints, so a clip can animate them. When the mesh node is not directly in S, mesh_from_model
 * (the inverse of its transform relative to S) makes the skin matrices exact; it is the identity
 * for the usual exports. Order: for each skin joint in skin order, its ancestors that are not
 * placed yet (top-down), then the joint. A source already sorted parent-first keeps its order.
 * Joints that are not skin joints get inverse(mesh_from_model x rest model transform) as inverse
 * bind matrix, so "bind pose x inverse bind = identity" holds for every joint.
 *
 * Everything is computed in f64 in a fixed order and rounded to f32 once.
 */
#include "anim_cook.h"
#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_math.h>

#include "anim/formats/de_cooked_skeleton.h"
#include "cook/lib/de_cooker.h"

#include <stdio.h>
#include <string.h>

/* ---- Settings -------------------------------------------------------------------------------- */

#define DEG_TO_RAD 0.017453292519943295

void de_model_anim_settings_default(de_model_anim_settings *s) {
    memset(s, 0, sizeof *s);
    s->sample_rate = (f32)DE_MODEL_ANIM_DEFAULT_RATE;
    s->error = (f32)DE_MODEL_ANIM_DEFAULT_ERROR;
    s->error_angle = (f32)(DE_MODEL_ANIM_DEFAULT_ERROR_ANGLE * DEG_TO_RAD);
    s->joint_names = true;
}

de_result de_model_anim_settings_read(de_asset_import_ctx *ctx, de_model_anim_settings *out) {
    de_model_anim_settings_default(out);
    f64 rate = de_asset_import_setting_float(ctx, "anim_sample_rate", DE_MODEL_ANIM_DEFAULT_RATE);
    f64 err = de_asset_import_setting_float(ctx, "anim_error", DE_MODEL_ANIM_DEFAULT_ERROR);
    f64 ang =
        de_asset_import_setting_float(ctx, "anim_error_angle", DE_MODEL_ANIM_DEFAULT_ERROR_ANGLE);
    /* NaN fails every comparison below. */
    if (!(rate > 0.0 && rate <= 1000.0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "setting `anim_sample_rate` must be in (0, 1000] Hz (got %g)", rate);
    if (!(err > 0.0 && err <= 1.0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "setting `anim_error` must be in (0, 1] metres (got %g)", err);
    if (!(ang > 0.0 && ang <= 45.0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "setting `anim_error_angle` must be in (0, 45] degrees (got %g)", ang);
    out->sample_rate = (f32)rate;
    out->error = (f32)err;
    out->error_angle = (f32)(ang * DEG_TO_RAD);
    out->joint_names = de_asset_import_setting_bool(ctx, "anim_joint_names", true);
    return DE_OK;
}

/* ---- f64 affine helpers ---------------------------------------------------------------------- */

void de_model_affine_from_trs(const f64 t[3], const f64 q[4], const f64 s[3], f64 out[12]) {
    f64 x = q[0], y = q[1], z = q[2], w = q[3];
    f64 n = x * x + y * y + z * z + w * w;
    f64 k = n > 0.0 ? 2.0 / n : 0.0;
    f64 r[9] = {/* column 0 */ 1.0 - k * (y * y + z * z),
                k * (x * y + z * w),
                k * (x * z - y * w),
                /* column 1 */ k * (x * y - z * w),
                1.0 - k * (x * x + z * z),
                k * (y * z + x * w),
                /* column 2 */ k * (x * z + y * w),
                k * (y * z - x * w),
                1.0 - k * (x * x + y * y)};
    for (u32 c = 0; c < 3; ++c)
        for (u32 row = 0; row < 3; ++row)
            out[c * 3 + row] = r[c * 3 + row] * s[c];
    for (u32 row = 0; row < 3; ++row)
        out[9 + row] = t[row];
}

void de_model_affine_mul(const f64 a[12], const f64 b[12], f64 out[12]) {
    f64 o[12];
    for (u32 c = 0; c < 4; ++c)
        for (u32 row = 0; row < 3; ++row) {
            f64 v = a[0 * 3 + row] * b[c * 3 + 0] + a[1 * 3 + row] * b[c * 3 + 1] +
                    a[2 * 3 + row] * b[c * 3 + 2];
            o[c * 3 + row] = c == 3 ? v + a[9 + row] : v;
        }
    memcpy(out, o, sizeof o);
}

bool de_model_affine_inverse(const f64 a[12], f64 out[12]) {
    /* m[row][col] = a[col * 3 + row] */
#define M(r, c) a[(c) * 3 + (r)]
    f64 c00 = M(1, 1) * M(2, 2) - M(1, 2) * M(2, 1);
    f64 c01 = M(1, 2) * M(2, 0) - M(1, 0) * M(2, 2);
    f64 c02 = M(1, 0) * M(2, 1) - M(1, 1) * M(2, 0);
    f64 det = M(0, 0) * c00 + M(0, 1) * c01 + M(0, 2) * c02;
    f64 scale = 0.0;
    for (u32 i = 0; i < 9; ++i)
        scale = scale > (a[i] < 0 ? -a[i] : a[i]) ? scale : (a[i] < 0 ? -a[i] : a[i]);
    if (!(det != 0.0) || (det < 0 ? -det : det) <= 1e-30 * scale * scale * scale) {
        memset(out, 0, sizeof(f64) * 12);
        out[0] = out[4] = out[8] = 1.0;
        return false;
    }
    f64 inv[9];
    /* inv[row][col] = cofactor[col][row] / det */
    inv[0 * 3 + 0] = c00 / det;
    inv[0 * 3 + 1] = c01 / det;
    inv[0 * 3 + 2] = c02 / det;
    inv[1 * 3 + 0] = (M(0, 2) * M(2, 1) - M(0, 1) * M(2, 2)) / det;
    inv[1 * 3 + 1] = (M(0, 0) * M(2, 2) - M(0, 2) * M(2, 0)) / det;
    inv[1 * 3 + 2] = (M(0, 1) * M(2, 0) - M(0, 0) * M(2, 1)) / det;
    inv[2 * 3 + 0] = (M(0, 1) * M(1, 2) - M(0, 2) * M(1, 1)) / det;
    inv[2 * 3 + 1] = (M(0, 2) * M(1, 0) - M(0, 0) * M(1, 2)) / det;
    inv[2 * 3 + 2] = (M(0, 0) * M(1, 1) - M(0, 1) * M(1, 0)) / det;
    f64 t[3] = {a[9], a[10], a[11]};
    for (u32 row = 0; row < 3; ++row) {
        out[0 * 3 + row] = inv[0 * 3 + row];
        out[1 * 3 + row] = inv[1 * 3 + row];
        out[2 * 3 + row] = inv[2 * 3 + row];
        out[9 + row] =
            -(inv[0 * 3 + row] * t[0] + inv[1 * 3 + row] * t[1] + inv[2 * 3 + row] * t[2]);
    }
#undef M
    return true;
}

/* ---- Skeletons ------------------------------------------------------------------------------- */

static void node_local(const de_model_node *n, f64 out[12]) {
    f64 t[3], q[4], s[3];
    for (u32 k = 0; k < 3; ++k) {
        t[k] = n->translation[k];
        s[k] = n->scale[k];
    }
    for (u32 k = 0; k < 4; ++k)
        q[k] = n->rotation[k];
    de_model_affine_from_trs(t, q, s, out);
}

de_result de_model_skeleton_build(de_model_ir *ir, u32 skin_index, de_model_skeleton *out) {
    memset(out, 0, sizeof *out);
    const de_model_skin *skin = &ir->skins[skin_index];
    out->skin = skin_index;
    if (skin->joint_count == 0)
        return DE_FAIL(DE_ERR_PARSE, "skin \"%s\" has no joints", skin->name);
    if (skin->joint_count > DE_COOKED_SKEL_MAX_JOINTS)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "skin \"%s\": %u joints (at most %u)", skin->name,
                       skin->joint_count, DE_COOKED_SKEL_MAX_JOINTS);
    u32 nn = ir->node_count;
    out->joint_of_node = (i32 *)de_model_ir_array(ir, nn + 1u, sizeof(i32));
    out->nodes = (u32 *)de_model_ir_array(ir, nn + 1u, sizeof(u32));
    u32 *chain = (u32 *)de_model_ir_array(ir, nn + 1u, sizeof(u32));
    out->mesh_joint_count = skin->joint_count;
    out->remap = (u16 *)de_model_ir_array(ir, skin->joint_count + 1u, sizeof(u16));
    if (out->joint_of_node == NULL || out->nodes == NULL || chain == NULL || out->remap == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    u8 *is_joint = (u8 *)de_model_ir_array(ir, nn + 1u, 1);
    u8 *above = (u8 *)de_model_ir_array(ir, nn + 1u, 1);
    if (is_joint == NULL || above == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 i = 0; i < nn; ++i)
        out->joint_of_node[i] = -1;
    for (u32 i = 0; i < skin->joint_count; ++i) {
        if (skin->joints[i] >= nn)
            return DE_FAIL(DE_ERR_PARSE, "skin \"%s\": joint %u is not a node", skin->name, i);
        is_joint[skin->joints[i]] = 1;
    }

    /* The skinned mesh node: the first node that uses the skin. Its ancestors (and itself) frame
     * the mesh and the joints alike, so they cancel out of the skin matrices: the skeleton stops
     * below them, at the same node S for every joint, and model space is the space of S. When the
     * joints reach different such nodes, or no node uses the skin, the skeleton keeps every
     * ancestor and model space is the scene's. */
    out->mesh_node = -1;
    for (u32 i = 0; i < nn && out->mesh_node < 0; ++i)
        if (ir->nodes[i].skin == (i32)skin_index)
            out->mesh_node = (i32)i;
    u32 steps = 0;
    for (i32 at = out->mesh_node; at >= 0 && steps <= nn; at = ir->nodes[at].parent, ++steps)
        above[at] = 1;
    i32 stop = -2;
    for (u32 i = 0; i < skin->joint_count && stop != -3; ++i) {
        i32 at = (i32)skin->joints[i];
        steps = 0;
        while (at >= 0 && (is_joint[at] || !above[at])) {
            if (++steps > nn)
                return DE_FAIL(DE_ERR_PARSE, "skin \"%s\": the node hierarchy has a cycle",
                               skin->name);
            at = ir->nodes[at].parent;
        }
        stop = stop == -2 || stop == at ? at : -3;
    }
    if (stop < 0) {
        memset(above, 0, nn + 1u);
        stop = -1;
    }
    out->space_node = stop;

    /* Stable parent-before-child order. */
    u32 count = 0;
    for (u32 i = 0; i < skin->joint_count; ++i) {
        u32 n = skin->joints[i];
        u32 len = 0;
        for (i32 at = (i32)n; at >= 0 && out->joint_of_node[at] < 0 && (is_joint[at] || !above[at]);
             at = ir->nodes[at].parent) {
            if (len == nn)
                return DE_FAIL(DE_ERR_PARSE, "skin \"%s\": the node hierarchy has a cycle",
                               skin->name);
            chain[len++] = (u32)at;
        }
        while (len > 0) {
            u32 node = chain[--len];
            out->joint_of_node[node] = (i32)count;
            out->nodes[count++] = node;
        }
    }
    if (count > DE_COOKED_SKEL_MAX_JOINTS)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "skin \"%s\": %u joints with their ancestors (at most %u)", skin->name,
                       count, DE_COOKED_SKEL_MAX_JOINTS);
    out->joint_count = count;
    out->parents = (u16 *)de_model_ir_array(ir, count, sizeof(u16));
    out->rest_model = (f64 *)de_model_ir_array(ir, (u64)count * 12u, sizeof(f64));
    out->inverse_bind = (f32 *)de_model_ir_array(ir, (u64)count * 12u, sizeof(f32));
    out->reach = (f64 *)de_model_ir_array(ir, count, sizeof(f64));
    bool *bound = (bool *)de_model_ir_array(ir, count, sizeof(bool));
    if (out->parents == NULL || out->rest_model == NULL || out->inverse_bind == NULL ||
        out->reach == NULL || bound == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 j = 0; j < count; ++j) {
        i32 p = ir->nodes[out->nodes[j]].parent;
        out->parents[j] = p >= 0 && out->joint_of_node[p] >= 0 ? (u16)out->joint_of_node[p]
                                                               : (u16)DE_COOKED_SKEL_NO_PARENT;
        f64 local[12];
        node_local(&ir->nodes[out->nodes[j]], local);
        if (out->parents[j] != DE_COOKED_SKEL_NO_PARENT)
            de_model_affine_mul(out->rest_model + 12u * out->parents[j], local,
                                out->rest_model + 12u * j);
        else
            memcpy(out->rest_model + 12u * j, local, sizeof local);
    }

    /* Mesh joints: remap, inverse bind matrices (the first wins for a joint listed twice). */
    u32 root = DE_COOKED_NONE;
    for (u32 i = 0; i < skin->joint_count; ++i) {
        u32 j = (u32)out->joint_of_node[skin->joints[i]];
        out->remap[i] = (u16)j;
        if (root == DE_COOKED_NONE || j < root)
            root = j;
        if (bound[j]) {
            de_model_ir_warn(ir, "skin \"%s\": node %u is listed twice as a joint", skin->name,
                             skin->joints[i]);
            continue;
        }
        bound[j] = true;
        const f32 *m = skin->inverse_bind + 16u * i; /* column-major 4x4 */
        for (u32 c = 0; c < 4; ++c)
            for (u32 row = 0; row < 3; ++row)
                out->inverse_bind[12u * j + c * 3 + row] = m[c * 4 + row];
    }
    /* The glTF `skeleton` hint names the top joint when it is a joint of the skin. */
    if (skin->skeleton_root >= 0 && (u32)skin->skeleton_root < nn &&
        out->joint_of_node[skin->skeleton_root] >= 0 &&
        bound[out->joint_of_node[skin->skeleton_root]])
        root = (u32)out->joint_of_node[skin->skeleton_root];
    out->root_joint = root;

    /* mesh_from_model: the inverse of the mesh node's rest transform in model space (identity
     * when the mesh node sits at S, the usual case). */
    f64 rel[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    steps = 0;
    for (i32 at = out->mesh_node; at >= 0 && at != stop && steps <= nn;
         at = ir->nodes[at].parent, ++steps) {
        f64 local[12];
        node_local(&ir->nodes[at], local);
        de_model_affine_mul(local, rel, rel);
    }
    if (!de_model_affine_inverse(rel, out->mesh_from_model))
        de_model_ir_warn(ir, "skin \"%s\": the skinned mesh node has a singular transform",
                         skin->name);

    /* Ancestors that are not skin joints: inverse bind = inverse(mesh_from_model x rest). */
    for (u32 j = 0; j < count; ++j) {
        if (bound[j])
            continue;
        f64 inv[12], bind[12];
        de_model_affine_mul(out->mesh_from_model, out->rest_model + 12u * j, bind);
        if (!de_model_affine_inverse(bind, inv))
            de_model_ir_warn(ir, "skin \"%s\": node \"%s\" has a singular rest transform",
                             skin->name, ir->nodes[out->nodes[j]].name);
        for (u32 k = 0; k < 12; ++k)
            out->inverse_bind[12u * j + k] = (f32)inv[k];
    }

    /* Reach: the distance from a joint to its farthest descendant (model space, rest pose), at
     * least its own bone length. Children come after parents, so walk backwards. */
    for (u32 j = 0; j < count; ++j) {
        u16 p = out->parents[j];
        if (p == DE_COOKED_SKEL_NO_PARENT)
            continue;
        const f64 *a = out->rest_model + 12u * p + 9, *b = out->rest_model + 12u * j + 9;
        f64 d = de_dsqrt64((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                           (a[2] - b[2]) * (a[2] - b[2]));
        if (d > out->reach[j])
            out->reach[j] = d;
    }
    for (u32 j = count; j-- > 0;) {
        const f64 *b = out->rest_model + 12u * j + 9;
        for (u16 a = out->parents[j]; a != DE_COOKED_SKEL_NO_PARENT; a = out->parents[a]) {
            const f64 *pa = out->rest_model + 12u * a + 9;
            f64 d = de_dsqrt64((pa[0] - b[0]) * (pa[0] - b[0]) + (pa[1] - b[1]) * (pa[1] - b[1]) +
                               (pa[2] - b[2]) * (pa[2] - b[2]));
            if (d > out->reach[a])
                out->reach[a] = d;
        }
    }
    return DE_OK;
}

void de_model_skeleton_write(de_model_buf *b, const de_model_ir *ir, const de_model_skeleton *s,
                             const de_model_names *names, bool joint_names) {
    u64 start = b->size;
    u32 j = s->joint_count;
    de_model_buf_put(b, DE_COOKED_SKEL_MAGIC, 4);
    de_model_buf_u32(b, DE_COOKED_SKEL_VERSION);
    de_model_buf_u32(b, joint_names ? DE_COOKED_SKEL_HAS_NAMES : 0u);
    de_model_buf_u32(b, j);
    de_model_buf_u32(b, s->mesh_joint_count);
    de_model_buf_u32(b, s->root_joint);
    u64 at_offsets = b->size;
    de_model_buf_put(b, NULL, 32); /* 6 offsets, name bytes, total: patched below */
    for (u32 k = 0; k < 12; ++k)
        de_model_buf_f32(b, (f32)s->mesh_from_model[k]);
    de_model_buf_u64(b, 0);

    u64 par = b->size - start;
    for (u32 i = 0; i < j; ++i) {
        u8 x[2] = {(u8)s->parents[i], (u8)(s->parents[i] >> 8)};
        de_model_buf_put(b, x, 2);
    }
    de_model_buf_align(b, 8);
    u64 hash = b->size - start;
    for (u32 i = 0; i < j; ++i)
        de_model_buf_u64(b, de_hash64_str(names->nodes[s->nodes[i]]));
    u64 rest = b->size - start;
    for (u32 i = 0; i < j; ++i) {
        const de_model_node *n = &ir->nodes[s->nodes[i]];
        for (u32 k = 0; k < 3; ++k)
            de_model_buf_f32(b, n->translation[k]);
        for (u32 k = 0; k < 4; ++k)
            de_model_buf_f32(b, n->rotation[k]);
        for (u32 k = 0; k < 3; ++k)
            de_model_buf_f32(b, n->scale[k]);
    }
    de_model_buf_align(b, 8);
    u64 ibm = b->size - start;
    for (u32 i = 0; i < 12u * j; ++i)
        de_model_buf_f32(b, s->inverse_bind[i]);
    u64 remap = b->size - start;
    for (u32 i = 0; i < s->mesh_joint_count; ++i) {
        u8 x[2] = {(u8)s->remap[i], (u8)(s->remap[i] >> 8)};
        de_model_buf_put(b, x, 2);
    }
    de_model_buf_align(b, 8);
    u64 name_off = 0, name_bytes = 0;
    if (joint_names) {
        name_off = b->size - start;
        u32 o = 4u * j;
        for (u32 i = 0; i < j; ++i) {
            de_model_buf_u32(b, o);
            o += (u32)strlen(names->nodes[s->nodes[i]]) + 1u;
        }
        for (u32 i = 0; i < j; ++i) {
            const char *nm = names->nodes[s->nodes[i]];
            de_model_buf_put(b, nm, strlen(nm) + 1u);
        }
        name_bytes = b->size - start - name_off;
        de_model_buf_align(b, 8);
    }
    de_model_buf_patch_u32(b, at_offsets, (u32)par);
    de_model_buf_patch_u32(b, at_offsets + 4, (u32)hash);
    de_model_buf_patch_u32(b, at_offsets + 8, (u32)rest);
    de_model_buf_patch_u32(b, at_offsets + 12, (u32)ibm);
    de_model_buf_patch_u32(b, at_offsets + 16, (u32)remap);
    de_model_buf_patch_u32(b, at_offsets + 20, (u32)name_off);
    de_model_buf_patch_u32(b, at_offsets + 24, (u32)name_bytes);
    de_model_buf_patch_u32(b, at_offsets + 28, (u32)(b->size - start));
}

/* ---- The extension point --------------------------------------------------------------------- */

static de_asset_id sub_asset_id(de_model_ir *ir, const char *model_id, const char *name) {
    size_t n = strlen(model_id) + 1u + strlen(name) + 1u;
    char *full = (char *)de_model_ir_alloc(ir, n);
    if (full == NULL)
        return (de_asset_id){0};
    snprintf(full, n, "%s#%s", model_id, name);
    return de_asset_id_from(full);
}

de_result de_model_emit_skeletons_and_clips(de_asset_import_ctx *ctx, de_model_ir *ir,
                                            const de_model_names *names, de_asset_id *skeleton_ids,
                                            u32 *skeleton_count, de_asset_id *clip_ids,
                                            u32 *clip_count) {
    *skeleton_count = 0;
    *clip_count = 0;
    de_model_anim_settings s;
    DE_TRY(de_model_anim_settings_read(ctx, &s));
    if (ir->skin_count == 0) {
        if (ir->anim_count != 0)
            de_model_ir_warn(ir,
                             "%u animation(s) not cooked: the model has no skin (node animation "
                             "without a skeleton is not supported yet)",
                             ir->anim_count);
        return DE_OK;
    }
    const char *model_id = de_cooker_import_id_str(ctx);
    de_model_skeleton *skels =
        (de_model_skeleton *)de_model_ir_array(ir, ir->skin_count, sizeof(de_model_skeleton));
    if (skels == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 i = 0; i < ir->skin_count; ++i) {
        DE_TRY(de_model_skeleton_build(ir, i, &skels[i]));
        de_model_buf b;
        de_model_buf_init(&b, ir);
        de_model_skeleton_write(&b, ir, &skels[i], names, s.joint_names);
        if (b.failed)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for a skeleton");
        DE_TRY(de_asset_import_add_subasset(ctx, names->skins[i], "skeleton", b.data, b.size));
        skeleton_ids[i] = sub_asset_id(ir, model_id, names->skins[i]);
        if (skeleton_ids[i].hash == 0)
            return DE_ERR_OUT_OF_MEMORY;
        *skeleton_count = i + 1u;
    }
    for (u32 i = 0; i < ir->anim_count; ++i) {
        const de_model_anim *an = &ir->anims[i];
        i32 k = de_model_anim_pick_skeleton(ir, an, skels, ir->skin_count);
        if (k < 0) {
            de_model_ir_warn(ir, "animation \"%s\" animates no skeleton joint; not cooked",
                             an->name);
            continue;
        }
        de_model_buf b;
        de_model_buf_init(&b, ir);
        DE_TRY(de_model_clip_write(&b, ir, an, &skels[k], skeleton_ids[k].hash, &s, NULL));
        if (b.failed)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for a clip");
        DE_TRY(de_asset_import_add_subasset(ctx, names->anims[i], "anim_clip", b.data, b.size));
        clip_ids[*clip_count] = sub_asset_id(ir, model_id, names->anims[i]);
        if (clip_ids[*clip_count].hash == 0)
            return DE_ERR_OUT_OF_MEMORY;
        ++*clip_count;
    }
    return DE_OK;
}
