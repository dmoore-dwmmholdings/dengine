#ifndef DE_RENDER_RT_RTAO_H
#define DE_RENDER_RT_RTAO_H
#include "render/rt/rt_internal.h"
typedef struct de__rtao de__rtao;
de_result de__rtao_create(de_renderer *, de__rtao **);
void de__rtao_destroy(de__rtao *);
bool de__rtao_published(const de__rtao *, de_view, const de_rg *, u64 frame);
typedef struct de__rtao_info {
    de_gpu_texture ao, distance, raw;
    u32 width, height, trace_width, trace_height, spp;
    u64 frame, rays;
    f32 radius;
} de__rtao_info;
bool de__rtao_get(const de__rtao *, de_view, de__rtao_info *);
#endif
