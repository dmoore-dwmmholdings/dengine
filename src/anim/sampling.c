#include "anim_internal.h"
#include "anim/anim_math.h"

/* Limit optional decoded expansion, including clips sharing compressed key ranges.
 * This is a cache limit, not an asset limit: larger clips use the original sampler. */
#define PREPARED_LIMIT (64u * 1024u * 1024u)
bool de__anim_prepare_clip(const de_allocator *alloc, const de_cooked_clip_view *clip,
                           de__anim_prepared_clip **out) {
    *out = NULL;
    u64 bytes =
        sizeof(de__anim_prepared_clip) + (u64)clip->track_count * sizeof(de__anim_prepared_track);
    for (u32 i = 0; i < clip->track_count; ++i) {
        de_cooked_clip_track track = de_cooked_clip_get_track(clip, i);
        bytes += (u64)track.key_count * 5u * sizeof(f32);
        if (bytes > PREPARED_LIMIT)
            return true;
    }
    de__anim_prepared_clip *cache = DE_ALLOC(alloc, (size_t)bytes, 16, DE_MEM_ASSET);
    if (!cache)
        return false;
    cache->bytes = (size_t)bytes;
    cache->base = clip->base;
    cache->track_count = clip->track_count;
    de__anim_prepared_track *tracks = (de__anim_prepared_track *)(cache + 1);
    cache->tracks = tracks;
    f32 *values = (f32 *)(tracks + clip->track_count);
    for (u32 i = 0; i < clip->track_count; ++i) {
        de__anim_prepared_track *prepared = &tracks[i];
        de_cooked_clip_track track = de_cooked_clip_get_track(clip, i);
        prepared->joint = track.joint;
        prepared->channel = track.channel;
        prepared->components = track.components;
        prepared->key_count = track.key_count;
        prepared->times = values;
        for (u32 k = 0; k < track.key_count; ++k)
            *values++ = track.key_count == 1
                            ? 0
                            : de_cooked_clip_time(clip, de_canim_get_u16(track.times + 2u * k));
        prepared->values = values;
        for (u32 k = 0; k < track.key_count; ++k) {
            memset(values, 0, 4u * sizeof(f32));
            de_cooked_clip_key_value(&track, k, values);
            values += 4;
        }
    }
    *out = cache;
    return true;
}

static f32 key_time(const de_cooked_clip_view *clip, const de_cooked_clip_track *track, u32 key) {
    return de_cooked_clip_time(clip, de_canim_get_u16(track->times + 2u * key));
}

static void sample_track(const de_cooked_clip_view *clip, const de_cooked_clip_track *track,
                         f32 time, bool forward, u32 *key, de__anim_clip_cursor *cursor,
                         f32 out[4]) {
    if (track->encoding == DE_COOKED_CLIP_ENC_CONST) {
        de_cooked_clip_key_value(track, 0, out);
        return;
    }
    u32 last = track->key_count - 1u;
    if (!(time > key_time(clip, track, 0))) {
        *key = 0;
        de_cooked_clip_key_value(track, 0, out);
        return;
    }
    if (!(time < key_time(clip, track, last))) {
        *key = last - 1u;
        de_cooked_clip_key_value(track, last, out);
        return;
    }
    u32 lo = forward ? *key : 0, hi = last;
    /* Bound walking for a forward seek that skips many keys. Ordinary playback
     * reuses its interval or advances a few keys; reverse/wrap seeks use search. */
    if (forward) {
        for (u32 steps = 0; steps < 4 && lo + 1u < last && key_time(clip, track, lo + 1u) <= time;
             ++steps) {
            ++lo;
            ++cursor->forward_steps;
        }
        if (key_time(clip, track, lo) <= time && time < key_time(clip, track, lo + 1u))
            hi = lo + 1u;
    }
    if (hi != lo + 1u) {
        ++cursor->binary_searches;
        while (hi - lo > 1u) {
            u32 mid = lo + (hi - lo) / 2u;
            if (key_time(clip, track, mid) <= time)
                lo = mid;
            else
                hi = mid;
        }
    }
    *key = lo;
    f32 a[4] = {0}, b[4] = {0};
    de_cooked_clip_key_value(track, lo, a);
    f32 t_lo = key_time(clip, track, lo);
    if (time == t_lo) {
        for (u32 c = 0; c < track->components; ++c)
            out[c] = a[c];
        return;
    }
    de_cooked_clip_key_value(track, hi, b);
    f32 alpha = (time - t_lo) / (key_time(clip, track, hi) - t_lo);
    if (track->channel == DE_COOKED_CLIP_CH_ROTATION) {
        de_quat q = de__anim_nlerp((de_quat){a[0], a[1], a[2], a[3]},
                                   (de_quat){b[0], b[1], b[2], b[3]}, alpha, true);
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    } else
        for (u32 c = 0; c < track->components; ++c)
            out[c] = de__anim_lerp_scalar(a[c], b[c], alpha);
}

static bool sample_into(const de_cooked_clip_view *clip, const de_cooked_skeleton_view *skel,
                        f32 time, de_cooked_joint_pose *out, de__anim_local_pose *pose,
                        de__anim_clip_cursor *cursor) {
    if (!clip || !skel || (!out && !pose) || !cursor || clip->joint_count != skel->joint_count ||
        (pose && (pose->count != skel->joint_count ||
                  (pose->count && (!pose->rotations || !pose->translations || !pose->scales)))) ||
        cursor->capacity < clip->track_count || (clip->track_count && !cursor->keys))
        return false;
    if (!(time > 0))
        time = 0;
    if (time > clip->duration)
        time = clip->duration;
    bool forward = cursor->valid && cursor->clip_base == clip->base && time >= cursor->time;
    if (pose) {
        pose->root_translation = (de_vec3){0};
        pose->root_rotation = (de_quat){0, 0, 0, 1};
    }
    for (u32 j = 0; j < skel->joint_count; ++j) {
        de_cooked_joint_pose rest = de_cooked_skeleton_rest(skel, j);
        if (pose)
            de__anim_pose_set_joint(pose, j, rest);
        else
            out[j] = rest;
    }
    for (u32 i = 0; i < clip->track_count; ++i) {
        de_cooked_clip_track track = de_cooked_clip_get_track(clip, i);
        f32 value[4] = {0};
        sample_track(clip, &track, time, forward, &cursor->keys[i], cursor, value);
        if (!pose)
            de_cooked_clip_apply(&track, value, &out[track.joint]);
        else if (track.channel == DE_COOKED_CLIP_CH_ROTATION)
            pose->rotations[track.joint] = (de_quat){value[0], value[1], value[2], value[3]};
        else if (track.channel == DE_COOKED_CLIP_CH_TRANSLATION)
            pose->translations[track.joint] = (de_vec3){value[0], value[1], value[2]};
        else
            pose->scales[track.joint] = track.components == 1
                                            ? (de_vec3){value[0], value[0], value[0]}
                                            : (de_vec3){value[0], value[1], value[2]};
    }
    cursor->clip_base = clip->base;
    cursor->time = time;
    cursor->valid = true;
    return true;
}

bool de__anim_sample_cached(const de_cooked_clip_view *clip, const de_cooked_skeleton_view *skel,
                            f32 time, de_cooked_joint_pose *out, de__anim_clip_cursor *cursor) {
    return sample_into(clip, skel, time, out, NULL, cursor);
}
bool de__anim_sample_pose_cached(const de_cooked_clip_view *clip,
                                 const de_cooked_skeleton_view *skel, f32 time,
                                 de__anim_local_pose *pose, de__anim_clip_cursor *cursor) {
    return sample_into(clip, skel, time, NULL, pose, cursor);
}

/* This path only reads compact native metadata/keys. Keep the interval and
 * arithmetic recipe equal to sample_track; reference tests compare every bit
 * and both cursor counters across encodings, endpoints, wraps and seeks. */
static void sample_prepared_track(const de__anim_prepared_track *track, f32 time, bool forward,
                                  u32 *key, de__anim_clip_cursor *cursor, f32 out[4]) {
    const f32 *times = track->times, *values = track->values;
    if (track->key_count == 1) {
        memcpy(out, values, 4u * sizeof(f32));
        return;
    }
    u32 last = track->key_count - 1u;
    if (!(time > times[0])) {
        *key = 0;
        memcpy(out, values, 4u * sizeof(f32));
        return;
    }
    if (!(time < times[last])) {
        *key = last - 1u;
        memcpy(out, values + (size_t)last * 4u, 4u * sizeof(f32));
        return;
    }
    u32 lo = forward ? *key : 0, hi = last;
    if (forward) {
        for (u32 steps = 0; steps < 4 && lo + 1u < last && times[lo + 1u] <= time; ++steps) {
            ++lo;
            ++cursor->forward_steps;
        }
        if (times[lo] <= time && time < times[lo + 1u])
            hi = lo + 1u;
    }
    if (hi != lo + 1u) {
        ++cursor->binary_searches;
        while (hi - lo > 1u) {
            u32 mid = lo + (hi - lo) / 2u;
            if (times[mid] <= time)
                lo = mid;
            else
                hi = mid;
        }
    }
    *key = lo;
    const f32 *a = values + (size_t)lo * 4u, *b = values + (size_t)hi * 4u;
    if (time == times[lo]) {
        memcpy(out, a, 4u * sizeof(f32));
        return;
    }
    f32 alpha = (time - times[lo]) / (times[hi] - times[lo]);
    if (track->channel == DE_COOKED_CLIP_CH_ROTATION) {
        de_quat q = de__anim_nlerp((de_quat){a[0], a[1], a[2], a[3]},
                                   (de_quat){b[0], b[1], b[2], b[3]}, alpha, true);
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    } else
        for (u32 c = 0; c < track->components; ++c)
            out[c] = de__anim_lerp_scalar(a[c], b[c], alpha);
}

bool de__anim_sample_prepared(const de_cooked_clip_view *clip,
                              const de__anim_prepared_clip *prepared,
                              const de_cooked_skeleton_view *skel, f32 time,
                              de__anim_local_pose *pose, de__anim_clip_cursor *cursor) {
    if (!prepared)
        return de__anim_sample_pose_cached(clip, skel, time, pose, cursor);
    if (!clip || !skel || !pose || !cursor || clip->joint_count != skel->joint_count ||
        pose->count != skel->joint_count ||
        (pose->count && (!pose->rotations || !pose->translations || !pose->scales)) ||
        prepared->base != clip->base || prepared->track_count != clip->track_count ||
        cursor->capacity < clip->track_count || (clip->track_count && !cursor->keys))
        return false;
    if (!(time > 0))
        time = 0;
    if (time > clip->duration)
        time = clip->duration;
    bool forward = cursor->valid && cursor->clip_base == clip->base && time >= cursor->time;
    pose->root_translation = (de_vec3){0};
    pose->root_rotation = (de_quat){0, 0, 0, 1};
    if (clip->track_count != skel->joint_count * 3u)
        for (u32 j = 0; j < skel->joint_count; ++j)
            de__anim_pose_set_joint(pose, j, de_cooked_skeleton_rest(skel, j));
    for (u32 i = 0; i < prepared->track_count; ++i) {
        const de__anim_prepared_track *track = &prepared->tracks[i];
        f32 value[4] = {0};
        sample_prepared_track(track, time, forward, &cursor->keys[i], cursor, value);
        if (track->channel == DE_COOKED_CLIP_CH_ROTATION)
            pose->rotations[track->joint] = (de_quat){value[0], value[1], value[2], value[3]};
        else if (track->channel == DE_COOKED_CLIP_CH_TRANSLATION)
            pose->translations[track->joint] = (de_vec3){value[0], value[1], value[2]};
        else
            pose->scales[track->joint] = track->components == 1
                                             ? (de_vec3){value[0], value[0], value[0]}
                                             : (de_vec3){value[0], value[1], value[2]};
    }
    cursor->clip_base = clip->base;
    cursor->time = time;
    cursor->valid = true;
    return true;
}
