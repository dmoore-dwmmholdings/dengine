#ifndef DE_TEST_RT_GOLDEN_H
#define DE_TEST_RT_GOLDEN_H
#include "golden.h"
#include <dengine/de_render.h>
typedef enum de__golden_rt_effect {
    DE__GOLDEN_RT_SHADOWS = 1,
    DE__GOLDEN_RT_DI,
    DE__GOLDEN_RT_AO,
    DE__GOLDEN_RT_REFLECTIONS,
    DE__GOLDEN_RT_GI,
    DE__GOLDEN_RT_REFRACTION,
    DE__GOLDEN_RT_FOLIAGE
} de__golden_rt_effect;
void de__golden_rt_assets(de_golden_ctx *, const char *scene, u32 effect, bool base);
void *de__golden_rt_prepare(de_golden_ctx *, de_renderer *, de_view, u32 effect, bool base);
void de__golden_rt_sample(u32 sample);
void de__golden_rt_finish(void *);
#endif
