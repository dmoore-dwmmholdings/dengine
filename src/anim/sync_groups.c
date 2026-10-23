#include "anim/sync_groups.h"
#include "anim/anim_math.h"
#include "anim/anim_internal.h"
#include <string.h>
static bool finite64(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof bits);
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}
bool de__anim_sync_step(de__anim_sync_clock *clock, de_name group, const de__anim_sync_leaf *leaves,
                        u32 count, f64 time, bool reset) {
    if (!clock || !group.hash || (count && !leaves) || !finite64(time))
        return false;
    const de__anim_sync_leaf *leader = NULL;
    for (u32 i = 0; i < count; ++i) {
        const de__anim_sync_leaf *leaf = &leaves[i];
        if (!de__anim_finite(leaf->demand) || leaf->demand < 0 || leaf->demand > 1 ||
            !de__anim_finite(leaf->duration) || leaf->duration < 0 ||
            !de__anim_finite(leaf->speed) || leaf->node == DE__ANIM_SYNC_NONE)
            return false;
        if (leaf->group.hash != group.hash || !(leaf->demand > 1e-4f) || !(leaf->duration > 0))
            continue;
        if (!leader || leaf->demand > leader->demand ||
            (leaf->demand == leader->demand && leaf->node < leader->node))
            leader = leaf;
    }
    de__anim_sync_clock next = *clock;
    if (reset || next.group.hash != group.hash) {
        memset(&next, 0, sizeof next);
        next.group = group;
    }
    if (next.initialized && (!finite64(next.phase) || !finite64(next.time)))
        return false;
    next.leader = leader ? leader->node : DE__ANIM_SYNC_NONE;
    if (leader) {
        f64 rate = (f64)leader->speed / (f64)leader->duration;
        f64 phase = next.initialized ? next.phase + (time - next.time) * rate : time * rate;
        if (!finite64(phase))
            return false;
        next.phase = phase;
        next.initialized = true;
    }
    next.time = time;
    *clock = next;
    return true;
}
bool de__anim_sync_sample_time(const de__anim_sync_clock *clock, f32 duration, bool loop,
                               f32 *time) {
    if (!clock || !clock->initialized || !time || !finite64(clock->phase) ||
        !de__anim_finite(duration) || duration < 0)
        return false;
    f64 phase = clock->phase;
    if (loop)
        phase = de__anim_wrap_time(phase, 1);
    else
        phase = phase < 0 ? 0 : phase > 1 ? 1 : phase;
    *time = (f32)(phase * (f64)duration);
    return true;
}
