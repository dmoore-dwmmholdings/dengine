#ifndef DE_RENDER_FORWARD_PARTICIPANTS_H
#define DE_RENDER_FORWARD_PARTICIPANTS_H
#include "render/passes/forward/forward.h"
#include "render/lighting/lighting.h"
struct de_forward_context {
    de_renderer *renderer;
    de__vis_context *vis;
    de__lighting_context *lighting;
    de_view_frame_info info;
    de_forward_participant_desc participants[16];
    u32 participant_count;
    de_forward_item *items;
};
de_result de__forward_collect(de_forward_context *context, de__transparent_item **items,
                              u32 *count);
void de__forward_declare_lighting(de_rg_pass *pass, const de__lighting_context *lighting);
#endif
