#ifndef DE_RENDER_RESTIR_DI_H
#define DE_RENDER_RESTIR_DI_H
#include "render/rt/rt_internal.h"
#include "render/lighting/lighting.h"
typedef struct de__di_reservoir {
    u32 source;
    f32 u, v;
    u32 m;
    f32 sum, weight, target;
    u32 age;
} de__di_reservoir;
DE_STATIC_ASSERT(sizeof(de__di_reservoir) == 32, "DI reservoir is 32 bytes");
typedef struct de__di_alias {
    f32 probability, pdf;
    u32 alias, reserved;
} de__di_alias;
void de__di_update(de__di_reservoir *, u32 source, de_vec2 sample, f32 target, f32 weight, u32 m,
                   f32 random);
void de__di_merge(de__di_reservoir *, const de__di_reservoir *, f32 target, u32 cap, f32 random);
void de__di_finalize(de__di_reservoir *, u32 normalization);
de_result de__di_alias_build(const f32 *power, u32 count, de__di_alias *, u32 *scratch);
typedef struct de__restir_di de__restir_di;
de_result de__restir_di_create(de_renderer *, de__restir_di **);
void de__restir_di_destroy(de__restir_di *);
de_result de__restir_di_prepare(de_renderer *);
de_result de__restir_di_build(de__restir_di *, de_rg *, de__vis_context *, de__lighting_context *,
                              de__lighting_inputs *);
typedef struct de__di_debug {
    bool reference;
} de__di_debug;
void de__restir_di_complete(de__restir_di *, de_view, const de_rg *, u64 frame, bool success);
void de__restir_di_debug(de__restir_di *, const de__di_debug *);
typedef struct de__di_info {
    de_gpu_texture diffuse, specular, distance;
    de_gpu_buffer reservoir, sources;
    u32 width, height, source_count, emissive_count;
    u64 alias_builds, epoch;
    bool history;
} de__di_info;
bool de__restir_di_get(de__restir_di *, de_view, de__di_info *);
#endif
