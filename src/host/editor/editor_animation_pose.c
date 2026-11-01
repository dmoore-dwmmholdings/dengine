#include "editor_animation_pose.h"
#include <dengine/de_math.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void *allocate(de_ep_arena *a, size_t n, size_t stride) {
    if (!a->base || a->used > a->size || n > SIZE_MAX / stride)
        return NULL;
    size_t pad = (16 - ((uintptr_t)(a->base + a->used) & 15)) & 15;
    size_t bytes = n * stride;
    if (pad > a->size - a->used || bytes > a->size - a->used - pad)
        return NULL;
    void *p = a->base + a->used + pad;
    a->used += pad + bytes;
    memset(p, 0, bytes);
    return p;
}
static de_result frames(const de_cooked_skeleton_view *s, const de_cooked_joint_pose *poses,
                        de_ep_arena *a, de_ep_list_animation_joint_frame *out) {
    de_affine *global = allocate(a, s->joint_count, sizeof *global);
    de_ep_animation_joint_frame *items = allocate(a, s->joint_count, sizeof *items);
    if (!global || !items)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 j = 0; j < s->joint_count; ++j) {
        de_cooked_joint_pose p = poses ? poses[j] : de_cooked_skeleton_rest(s, j);
        for (u32 k = 0; k < 3; ++k)
            if (!isfinite(p.t[k]) || !isfinite(p.s[k]))
                return DE_ERR_PARSE;
        f32 length = 0;
        for (u32 k = 0; k < 4; ++k) {
            if (!isfinite(p.r[k]))
                return DE_ERR_PARSE;
            length += p.r[k] * p.r[k];
        }
        if (!isfinite(length) || !(length > 0))
            return DE_ERR_PARSE;
        de_affine local = de_affine_from_trs((de_vec3){p.t[0], p.t[1], p.t[2]},
                                             (de_quat){p.r[0], p.r[1], p.r[2], p.r[3]},
                                             (de_vec3){p.s[0], p.s[1], p.s[2]});
        u16 parent = de_cooked_skeleton_parent(s, j);
        if (parent != DE_COOKED_SKEL_NO_PARENT && parent >= j)
            return DE_ERR_PARSE;
        global[j] =
            parent == DE_COOKED_SKEL_NO_PARENT ? local : de_affine_mul(&global[parent], &local);
        for (u32 k = 0; k < 12; ++k)
            if (!isfinite(global[j].m[k]))
                return DE_ERR_PARSE;
        items[j].matrix = (de_ep_list_f32){global[j].m, 12};
    }
    *out = (de_ep_list_animation_joint_frame){items, s->joint_count};
    return DE_OK;
}
de_result de__ed_animation_sample(const de_cooked_clip_view *clip, const de_cooked_skeleton_view *s,
                                  const de_ep_animation_preview_params *q, u32 clip_version,
                                  u32 skeleton_version, de_ep_str skeleton_id, de_ep_arena *arena,
                                  de_ep_animation_preview *out) {
    if (!clip || !s || !q || !arena || !out || !isfinite(q->time_seconds) ||
        q->has_clip_version != q->has_skeleton_version)
        return DE_ERR_INVALID_ARGUMENT;
    if (s->joint_count > DE__ED_ANIMATION_JOINTS)
        return DE_ERR_UNSUPPORTED;
    if (!s->joint_count || s->joint_count != clip->joint_count || !isfinite(clip->duration) ||
        clip->duration < 0 || !isfinite(clip->sample_rate) || clip->sample_rate < 0 ||
        clip->track_count > s->joint_count * 3)
        return DE_ERR_PARSE;
    de_ep_arena a = *arena;
    de_ep_animation_preview r = {0};
    r.clip_version = clip_version;
    r.skeleton_version = skeleton_version;
    r.skeleton = skeleton_id;
    r.duration_seconds = clip->duration;
    r.time_seconds = q->time_seconds < 0                ? 0
                     : q->time_seconds > clip->duration ? clip->duration
                                                        : q->time_seconds;
    r.sample_rate = clip->sample_rate;
    r.tracks = clip->track_count;
    r.loopable = (clip->flags & DE_COOKED_CLIP_LOOP) != 0;
    r.has_metadata = !q->has_clip_version || q->clip_version != clip_version ||
                     q->skeleton_version != skeleton_version;
    de_cooked_joint_pose *poses = allocate(&a, s->joint_count, sizeof *poses);
    if (!poses)
        return DE_ERR_OUT_OF_MEMORY;
    if (!de__anim_clip_sample(clip, s, r.time_seconds, poses))
        return DE_ERR_PARSE;
    de_result status = frames(s, poses, &a, &r.pose);
    if (status != DE_OK)
        return status;
    if (r.has_metadata) {
        r.metadata.joints.items = allocate(&a, s->joint_count, sizeof *r.metadata.joints.items);
        if (!r.metadata.joints.items)
            return DE_ERR_OUT_OF_MEMORY;
        size_t name_bytes = 0;
        for (u32 j = 0; j < s->joint_count; ++j) {
            const char *name = de_cooked_skeleton_name(s, j);
            if (!name) {
                char *fallback = allocate(&a, 32, 1);
                if (!fallback)
                    return DE_ERR_OUT_OF_MEMORY;
                snprintf(fallback, 32, "Joint %u", j);
                name = fallback;
            }
            size_t bytes = strlen(name);
            if (bytes > 4096 || bytes > 1024 * 1024 - name_bytes)
                return DE_ERR_UNSUPPORTED;
            name_bytes += bytes;
            u16 parent = de_cooked_skeleton_parent(s, j);
            r.metadata.joints.items[j] = (de_ep_animation_joint){
                de_ep_cstr(name), parent != DE_COOKED_SKEL_NO_PARENT, parent};
        }
        r.metadata.joints.count = s->joint_count;
        status = frames(s, NULL, &a, &r.metadata.rest_pose);
        if (status != DE_OK)
            return status;
    }
    *arena = a;
    *out = r;
    return DE_OK;
}
