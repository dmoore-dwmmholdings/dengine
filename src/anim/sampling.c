#include "anim_internal.h"

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
    if (track->channel == DE_COOKED_CLIP_CH_ROTATION)
        de_cooked_clip_nlerp(a, b, alpha, out);
    else
        for (u32 c = 0; c < track->components; ++c)
            out[c] = a[c] + (b[c] - a[c]) * alpha;
}

bool de__anim_sample_cached(const de_cooked_clip_view *clip, const de_cooked_skeleton_view *skel,
                            f32 time, de_cooked_joint_pose *out, de__anim_clip_cursor *cursor) {
    if (!clip || !skel || !out || !cursor || clip->joint_count != skel->joint_count ||
        cursor->capacity < clip->track_count || (clip->track_count && !cursor->keys))
        return false;
    if (!(time > 0))
        time = 0;
    if (time > clip->duration)
        time = clip->duration;
    bool forward = cursor->valid && cursor->clip_base == clip->base && time >= cursor->time;
    for (u32 j = 0; j < skel->joint_count; ++j)
        out[j] = de_cooked_skeleton_rest(skel, j);
    for (u32 i = 0; i < clip->track_count; ++i) {
        de_cooked_clip_track track = de_cooked_clip_get_track(clip, i);
        f32 value[4] = {0};
        sample_track(clip, &track, time, forward, &cursor->keys[i], cursor, value);
        de_cooked_clip_apply(&track, value, &out[track.joint]);
    }
    cursor->clip_base = clip->base;
    cursor->time = time;
    cursor->valid = true;
    return true;
}
