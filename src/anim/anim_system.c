#include "anim_internal.h"

de_result de__anim_features_init(de_anim_system *s, de_world *w) {
    DE_TRY(de_anim__graph_init(s, w));
    return de_anim__sprite_init(s, w);
}

de_result de_anim__register_stage(de_anim_system *s, de__anim_stage stage, de__anim_stage_fn fn,
                                  void *user) {
    if (!s || (u32)stage >= DE_ANIM_STAGE_COUNT || !fn || s->dispatching_stages)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "animation stage registration");
    u32 count = s->stage_counts[stage];
    for (u32 i = 0; i < count; ++i)
        if (s->stages[stage][i].fn == fn && s->stages[stage][i].user == user)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS, "animation stage already registered");
    if (count == DE__ANIM_STAGE_HOOK_CAPACITY)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "animation stage hook capacity");
    s->stages[stage][count] = (de__anim_stage_hook){fn, user};
    s->stage_counts[stage] = count + 1;
    return DE_OK;
}

void de__anim_run_stage(de_anim_system *s, de__anim_stage stage) {
    s->dispatching_stages = true;
    for (u32 i = 0; i < s->stage_counts[stage]; ++i) {
        de__anim_stage_hook hook = s->stages[stage][i];
        hook.fn(s, hook.user);
    }
    s->dispatching_stages = false;
}
