#ifndef DE_ANIM_SYNC_GROUPS_H
#define DE_ANIM_SYNC_GROUPS_H
#include <dengine/de_base.h>
#define DE__ANIM_SYNC_NONE UINT32_MAX
/* Evaluated leaf demand, in graph-node identity order or any permutation. */
typedef struct de__anim_sync_leaf {
    de_name group;
    u32 node;
    f32 demand, duration, speed;
} de__anim_sync_leaf;
/* Plain value state: callers must snapshot this alongside their graph clock.
 * Phase is unbounded cycles. Sampling applies each leaf's loop/clamp policy. */
typedef struct de__anim_sync_clock {
    de_name group;
    f64 time, phase;
    u32 leader;
    bool initialized;
} de__anim_sync_clock;
/* Two banks of group clocks: fixed followed by visual. Reflected component size
 * is offsetof(clocks) + 2 * group_count * sizeof(clock), not a maximum array. */
typedef struct de__anim_sync_state {
    u64 definition;
    bool fixed_valid, visual_valid;
    de__anim_sync_clock clocks[];
} de__anim_sync_state;
/* Highest demand above 1e-4 leads; exact ties choose the lowest graph node.
 * A leader change preserves phase and uses the new rate for this interval.
 * No active leader freezes phase while consuming elapsed time. Reset/first use
 * starts at time * leader.speed / leader.duration. Invalid input writes nothing. */
bool de__anim_sync_step(de__anim_sync_clock *clock, de_name group, const de__anim_sync_leaf *leaves,
                        u32 count, f64 time, bool reset);
bool de__anim_sync_sample_time(const de__anim_sync_clock *clock, f32 duration, bool loop,
                               f32 *time);
#endif
