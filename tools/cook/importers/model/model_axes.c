/*
 * model_axes.c: conversion of a model IR to engine coordinates (SPEC §5.5; DENG-101) and matrix
 * decomposition.
 *
 * The engine is Z-up, right-handed, +Y forward, metres. glTF is Y-up, right-handed, metres, with
 * the model's front along +Z. The conversion is the proper rotation of +90° about X (DENG-2
 * addendum): (x, y, z) → (x, −z, y). glTF +Y (up) becomes +Z, and glTF +Z (front) becomes −Y, so
 * Blender content exported with "+Y up" lands at its original Blender coordinates. A rotation has
 * det = +1: winding and tangent handedness stay as they are.
 *
 * Every product is computed in f64 and rounded to f32 once, in a fixed order, so a cook gives the
 * same bits on every run.
 */
#include "model_ir.h"

#include <dengine/de_base.h>

#include <math.h>
#include <string.h>

void de_model_axes_matrix(de_model_axes from, f32 m[3][3]) {
    memset(m, 0, sizeof(f32) * 9);
    if (from == DE_MODEL_AXES_GLTF) {
        m[0][0] = 1.0f;  /* x' = x */
        m[1][2] = -1.0f; /* y' = -z */
        m[2][1] = 1.0f;  /* z' = y */
        return;
    }
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

/* The helpers take C as non-const f64[3][3]: before C23, passing an f64[3][3] to a
 * const f64 (*)[3] parameter is a constraint violation (GCC -Wpedantic). */

/* v' = s * C * v (v has 3 floats). */
static void xform_vec(f64 c[3][3], f64 s, f32 *v) {
    f64 in[3] = {v[0], v[1], v[2]};
    for (u32 i = 0; i < 3; ++i)
        v[i] = (f32)(s * (c[i][0] * in[0] + c[i][1] * in[1] + c[i][2] * in[2]));
}

/* For a signed permutation C: C · diag(s) · Cᵀ = diag(s'), s'[i] = s[column of row i]. */
static void xform_scale(f64 c[3][3], f32 *s) {
    f32 in[3] = {s[0], s[1], s[2]};
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j)
            if (c[i][j] != 0.0)
                s[i] = in[j];
}

/* M' = U·C·M·C⁻¹·U⁻¹ for a column-major affine 4x4 (C⁻¹ = Cᵀ for a rotation). */
static void xform_mat4(f64 c[3][3], f64 s, f32 m[16]) {
    f64 a[3][3], t[3], r[3][3];
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row)
            a[row][col] = m[4u * col + row];
    for (u32 k = 0; k < 3; ++k)
        t[k] = m[12u + k];
    /* r = C · a · Cᵀ */
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j) {
            f64 acc = 0.0;
            for (u32 k = 0; k < 3; ++k)
                for (u32 l = 0; l < 3; ++l)
                    acc += c[i][k] * a[k][l] * c[j][l];
            r[i][j] = acc;
        }
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row)
            m[4u * col + row] = (f32)r[row][col];
    for (u32 i = 0; i < 3; ++i)
        m[12u + i] = (f32)(s * (c[i][0] * t[0] + c[i][1] * t[1] + c[i][2] * t[2]));
}

/* q' = C·q·C⁻¹ as a quaternion: the axis is rotated, the angle and w are kept. */
static void xform_quat(f64 c[3][3], f32 *q) {
    xform_vec(c, 1.0, q);
}

void de_model_ir_convert_axes(de_model_ir *ir, de_model_axes from, f32 scale) {
    f32 cf[3][3];
    de_model_axes_matrix(from, cf);
    f64 c[3][3];
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j)
            c[i][j] = cf[i][j];
    f64 s = (f64)scale;

    for (u32 m = 0; m < ir->mesh_count; ++m) {
        de_model_mesh *mesh = &ir->meshes[m];
        for (u32 v = 0; v < mesh->vertex_count; ++v) {
            xform_vec(c, s, mesh->position + 3u * v);
            if (mesh->normal != NULL)
                xform_vec(c, 1.0, mesh->normal + 3u * v);
            if (mesh->tangent != NULL)
                xform_vec(c, 1.0, mesh->tangent + 4u * v); /* w (handedness) unchanged */
        }
    }
    for (u32 n = 0; n < ir->node_count; ++n) {
        de_model_node *node = &ir->nodes[n];
        xform_vec(c, s, node->translation);
        xform_quat(c, node->rotation);
        xform_scale(c, node->scale);
    }
    for (u32 k = 0; k < ir->skin_count; ++k) {
        de_model_skin *skin = &ir->skins[k];
        for (u32 j = 0; j < skin->joint_count; ++j)
            xform_mat4(c, s, skin->inverse_bind + 16u * j);
    }
    for (u32 a = 0; a < ir->anim_count; ++a) {
        de_model_anim *anim = &ir->anims[a];
        for (u32 ch = 0; ch < anim->channel_count; ++ch) {
            de_model_anim_channel *chan = &anim->channels[ch];
            u32 per_key = chan->interp == DE_MODEL_INTERP_CUBIC ? 3u : 1u;
            u32 values = chan->key_count * per_key;
            for (u32 v = 0; v < values; ++v) {
                f32 *x = chan->values + (u64)v * chan->components;
                switch (chan->path) {
                case DE_MODEL_ANIM_TRANSLATION:
                    xform_vec(c, s, x);
                    break;
                case DE_MODEL_ANIM_ROTATION:
                    xform_quat(c, x);
                    break;
                case DE_MODEL_ANIM_SCALE:
                    xform_scale(c, x);
                    break;
                case DE_MODEL_ANIM_WEIGHTS:
                    break;
                }
            }
        }
    }
}

/* ---- Matrix decomposition -------------------------------------------------------------------- */

bool de_model_decompose_matrix(const f32 m[16], f32 t[3], f32 r[4], f32 s[3]) {
    f64 col[3][3];
    for (u32 c = 0; c < 3; ++c)
        for (u32 k = 0; k < 3; ++k)
            col[c][k] = m[4u * c + k];
    for (u32 k = 0; k < 3; ++k)
        t[k] = m[12u + k];
    bool ok = m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[15] == 1.0f;
    f64 len[3];
    for (u32 c = 0; c < 3; ++c)
        len[c] = sqrt(col[c][0] * col[c][0] + col[c][1] * col[c][1] + col[c][2] * col[c][2]);
    f64 det = col[0][0] * (col[1][1] * col[2][2] - col[1][2] * col[2][1]) -
              col[1][0] * (col[0][1] * col[2][2] - col[0][2] * col[2][1]) +
              col[2][0] * (col[0][1] * col[1][2] - col[0][2] * col[1][1]);
    if (det < 0.0)
        len[0] = -len[0];
    for (u32 c = 0; c < 3; ++c) {
        s[c] = (f32)len[c];
        if (len[c] == 0.0) {
            ok = false;
            r[0] = r[1] = r[2] = 0.0f;
            r[3] = 1.0f;
            return false;
        }
        for (u32 k = 0; k < 3; ++k)
            col[c][k] /= len[c];
    }
    /* Shear: the normalised columns must be orthogonal. */
    for (u32 a = 0; a < 3; ++a)
        for (u32 b = a + 1u; b < 3; ++b) {
            f64 d = col[a][0] * col[b][0] + col[a][1] * col[b][1] + col[a][2] * col[b][2];
            if (fabs(d) > 1e-4)
                ok = false;
        }
    /* Rotation matrix R[row][col] = col[col][row] → quaternion (Shepperd's method). */
    f64 r00 = col[0][0], r11 = col[1][1], r22 = col[2][2];
    f64 r01 = col[1][0], r10 = col[0][1], r02 = col[2][0], r20 = col[0][2];
    f64 r12 = col[2][1], r21 = col[1][2];
    f64 q[4];
    f64 tr = r00 + r11 + r22;
    if (tr > 0.0) {
        f64 w4 = 2.0 * sqrt(1.0 + tr);
        q[3] = 0.25 * w4;
        q[0] = (r21 - r12) / w4;
        q[1] = (r02 - r20) / w4;
        q[2] = (r10 - r01) / w4;
    } else if (r00 > r11 && r00 > r22) {
        f64 x4 = 2.0 * sqrt(1.0 + r00 - r11 - r22);
        q[3] = (r21 - r12) / x4;
        q[0] = 0.25 * x4;
        q[1] = (r01 + r10) / x4;
        q[2] = (r02 + r20) / x4;
    } else if (r11 > r22) {
        f64 y4 = 2.0 * sqrt(1.0 + r11 - r00 - r22);
        q[3] = (r02 - r20) / y4;
        q[0] = (r01 + r10) / y4;
        q[1] = 0.25 * y4;
        q[2] = (r12 + r21) / y4;
    } else {
        f64 z4 = 2.0 * sqrt(1.0 + r22 - r00 - r11);
        q[3] = (r10 - r01) / z4;
        q[0] = (r02 + r20) / z4;
        q[1] = (r12 + r21) / z4;
        q[2] = 0.25 * z4;
    }
    f64 ql = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (u32 k = 0; k < 4; ++k)
        r[k] = (f32)(q[k] / ql);
    return ok;
}
