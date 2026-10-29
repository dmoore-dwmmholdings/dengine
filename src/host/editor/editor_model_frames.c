#include "editor_model_frames.h"
#include "render/formats/de_cooked_model.h"
#include <math.h>
#include <string.h>

static bool point_ok(de_dvec3 p) {
    return isfinite(p.x) && isfinite(p.y) && isfinite(p.z);
}
static bool transform(const de_transform *pose, de_vec3 local, de_dvec3 *out) {
    if (!pose || !point_ok(pose->position) || !isfinite(local.x) || !isfinite(local.y) ||
        !isfinite(local.z))
        return false;
    const de_affine matrix = de_affine_from_trs((de_vec3){0}, pose->rotation, pose->scale);
    const de_vec3 p = de_affine_transform_point(&matrix, local);
    *out = (de_dvec3){pose->position.x + p.x, pose->position.y + p.y, pose->position.z + p.z};
    return point_ok(*out);
}
static bool room(de__ed_frame_line *lines, u32 capacity, const u32 *count, u32 extra) {
    return lines && count && capacity <= DE__ED_FRAME_LINES && *count <= capacity &&
           extra <= capacity - *count;
}
static de__ed_frame_line line(de_dvec3 a, de_dvec3 b, bool marker, bool box) {
    de__ed_frame_line result = {.a = a, .b = b, .marker = marker};
    const u8 color[4] = {box || marker ? 255 : 64,
                         marker ? 255
                         : box  ? 208
                                : 220,
                         box ? 64 : 255, 255};
    memcpy(result.rgba, color, 4);
    return result;
}
de_result de__ed_model_skin(const de_render_model *model, u32 mesh, u32 *skin) {
    if (!model || !skin || mesh >= model->mesh_count || !model->meshes ||
        (model->node_count && !model->nodes) || (model->skeleton_count && !model->skeleton_ids))
        return DE_ERR_INVALID_ARGUMENT;
    u32 found = DE_COOKED_NONE;
    for (u32 i = 0; i < model->node_count; ++i) {
        const de_render_model_node *node = &model->nodes[i];
        if (node->mesh.id != model->meshes[mesh].id || node->skin == DE_COOKED_NONE)
            continue;
        if (node->skin >= model->skeleton_count)
            return DE_ERR_PARSE;
        if (found != DE_COOKED_NONE && found != node->skin)
            return DE_ERR_UNSUPPORTED;
        found = node->skin;
    }
    if (found == DE_COOKED_NONE)
        return DE_ERR_NOT_FOUND;
    *skin = found;
    return DE_OK;
}
de_result de__ed_model_box(const de_transform *pose, const f32 min[3], const f32 max[3],
                           de__ed_frame_line *lines, u32 capacity, u32 *count) {
    if (!min || !max || !pose || !count)
        return DE_ERR_INVALID_ARGUMENT;
    if (!room(lines, capacity, count, 12))
        return DE_ERR_UNSUPPORTED;
    for (u32 i = 0; i < 3; ++i)
        if (!isfinite(min[i]) || !isfinite(max[i]) || min[i] > max[i])
            return DE_ERR_PARSE;
    de_dvec3 corners[8];
    for (u32 i = 0; i < 8; ++i)
        if (!transform(pose,
                       (de_vec3){i & 1 ? max[0] : min[0], i & 2 ? max[1] : min[1],
                                 i & 4 ? max[2] : min[2]},
                       &corners[i]))
            return DE_ERR_PARSE;
    u32 n = *count;
    for (u32 i = 0; i < 8; ++i)
        for (u32 bit = 1; bit <= 4; bit <<= 1)
            if (!(i & bit))
                lines[n++] = line(corners[i], corners[i | bit], false, true);
    *count = n;
    return DE_OK;
}
de_result de__ed_model_bind(const de_cooked_skeleton_view *skeleton, const de_transform *pose,
                            de_dvec3 *joints, u32 joint_capacity, de__ed_frame_line *lines,
                            u32 capacity, u32 *count) {
    if (!skeleton || !pose || !joints || !count)
        return DE_ERR_INVALID_ARGUMENT;
    const u32 n = skeleton->joint_count;
    if (!n || n > DE__ED_FRAME_JOINTS || n > joint_capacity || !room(lines, capacity, count, n * 2))
        return DE_ERR_UNSUPPORTED;
    if (!skeleton->parents || !skeleton->inverse_bind)
        return DE_ERR_PARSE;
    for (u32 i = 0; i < n; ++i) {
        const u16 parent = de_cooked_skeleton_parent(skeleton, i);
        if (parent != DE_COOKED_SKEL_NO_PARENT && parent >= i)
            return DE_ERR_PARSE;
        de_affine inverse;
        de_cooked_skeleton_inverse_bind(skeleton, i, inverse.m);
        for (u32 k = 0; k < 12; ++k)
            if (!isfinite(inverse.m[k]))
                return DE_ERR_PARSE;
        const de_affine bind = de_affine_inverse(&inverse);
        bool linear = false;
        for (u32 k = 0; k < 12; ++k) {
            if (!isfinite(bind.m[k]))
                return DE_ERR_PARSE;
            if (k < 9)
                linear |= bind.m[k] != 0;
        }
        if (!linear || !transform(pose, (de_vec3){bind.m[9], bind.m[10], bind.m[11]}, &joints[i]))
            return DE_ERR_PARSE;
    }
    u32 at = *count;
    for (u32 i = 0; i < n; ++i) {
        u16 parent = de_cooked_skeleton_parent(skeleton, i);
        if (parent != DE_COOKED_SKEL_NO_PARENT)
            lines[at++] = line(joints[parent], joints[i], false, false);
        lines[at++] = line(joints[i], joints[i], true, false);
    }
    *count = at;
    return DE_OK;
}

typedef struct clip_point {
    f64 v[4];
} clip_point;
static bool project(const de_view_frame_info *frame, de_dvec3 p, clip_point *clip) {
    const f64 v[] = {p.x - frame->render_origin.x, p.y - frame->render_origin.y,
                     p.z - frame->render_origin.z, 1};
    for (u32 row = 0; row < 4; ++row) {
        clip->v[row] = 0;
        for (u32 col = 0; col < 4; ++col)
            clip->v[row] += frame->view_projection.m[col * 4 + row] * v[col];
        if (!isfinite(clip->v[row]))
            return false;
    }
    return true;
}
static f64 plane(clip_point p, u32 side) {
    return side < 4    ? p.v[3] + (side & 1 ? -1 : 1) * p.v[side / 2]
           : side == 4 ? p.v[2]
                       : p.v[3] - p.v[2];
}
static bool clip_segment(clip_point *a, clip_point *b) {
    f64 lo = 0, hi = 1;
    for (u32 side = 0; side < 6; ++side) {
        const f64 x = plane(*a, side), y = plane(*b, side);
        if (x < 0 && y < 0)
            return false;
        if (x < 0 || y < 0) {
            const f64 t = x / (x - y);
            if (x < 0) {
                if (t > lo)
                    lo = t;
            } else if (t < hi)
                hi = t;
        }
    }
    if (lo > hi)
        return false;
    const clip_point begin = *a, end = *b;
    for (u32 i = 0; i < 4; ++i) {
        a->v[i] = begin.v[i] + (end.v[i] - begin.v[i]) * lo;
        b->v[i] = begin.v[i] + (end.v[i] - begin.v[i]) * hi;
    }
    return a->v[3] > 0 && b->v[3] > 0;
}
static void pixel(u8 *rgba, u32 size, i32 x, i32 y, const u8 color[4]) {
    if (x >= 0 && y >= 0 && (u32)x < size && (u32)y < size)
        memcpy(rgba + ((size_t)(u32)y * size + (u32)x) * 4, color, 4);
}
void de__ed_model_frame_draw(const de__ed_frame_line *lines, u32 count,
                             const de_view_frame_info *frame, u32 size, u8 *rgba) {
    if (!lines || count > DE__ED_FRAME_LINES || !frame || !rgba || !size || size > 1024)
        return;
    const u8 halo[] = {20, 20, 20, 255};
    for (u32 layer = 0; layer < 3; ++layer)
        for (u32 i = 0; i < count; ++i) {
            if (layer == 2 && !lines[i].marker)
                continue;
            clip_point a, b;
            if (!project(frame, lines[i].a, &a) || !project(frame, lines[i].b, &b) ||
                !clip_segment(&a, &b))
                continue;
            const f64 x0 = (a.v[0] / a.v[3] * .5 + .5) * (size - 1),
                      y0 = (.5 - a.v[1] / a.v[3] * .5) * (size - 1),
                      x1 = (b.v[0] / b.v[3] * .5 + .5) * (size - 1),
                      y1 = (.5 - b.v[1] / b.v[3] * .5) * (size - 1);
            const f64 extent = fmax(fabs(x1 - x0), fabs(y1 - y0));
            if (!isfinite(extent) || extent > size)
                continue;
            const u32 steps = (u32)ceil(extent);
            for (u32 step = 0; step <= steps; ++step) {
                const f64 t = steps ? (f64)step / steps : 0;
                const i32 x = (i32)round(x0 + (x1 - x0) * t), y = (i32)round(y0 + (y1 - y0) * t);
                if (layer == 0) {
                    for (i32 dy = -1; dy <= 1; ++dy)
                        for (i32 dx = -1; dx <= 1; ++dx)
                            pixel(rgba, size, x + dx, y + dy, halo);
                } else if (layer == 1)
                    pixel(rgba, size, x, y, lines[i].rgba);
                else
                    for (i32 d = -2; d <= 2; ++d) {
                        pixel(rgba, size, x + d, y, lines[i].rgba);
                        pixel(rgba, size, x, y + d, lines[i].rgba);
                    }
            }
        }
}
