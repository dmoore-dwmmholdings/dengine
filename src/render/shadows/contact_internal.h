#ifndef DE_RENDER_SHADOWS_CONTACT_INTERNAL_H
#define DE_RENDER_SHADOWS_CONTACT_INTERNAL_H
#include "render/shadows/shadows_internal.h"
#include "render/lighting/lighting.h"
#include "render/world/render_world.h"
DE_EXTERN_C_BEGIN
typedef struct de__contact_light {
    u32 entity[2], kind, channel;
    f32 position_range[4], direction_radius[4];
} de__contact_light;
typedef struct de__contact_metadata {
    u32 header[4]; /* directional image, local image, local count, directional valid */
    de__contact_light light[5];
} de__contact_metadata;
DE_STATIC_ASSERT(sizeof(de__contact_light) == 48, "ContactLightRecord is 48 bytes");
DE_STATIC_ASSERT(sizeof(de__contact_metadata) == 256, "Contact metadata is 256 bytes");
DE_STATIC_ASSERT(offsetof(de__contact_metadata, light) == 16, "Contact header is 16 bytes");
typedef struct de__contact_config {
    f32 length, thickness, normal_bias;
    u32 steps, max_locals;
} de__contact_config;
typedef struct de__contact_params {
    u32 scene, slot, depth, normal;
    u32 records, packed, width, height;
    u32 pitch, local_offset, steps, frame;
    f32 length, thickness, normal_bias, reserved;
} de__contact_params;
DE_STATIC_ASSERT(sizeof(de__contact_params) == 64, "ContactParams is 64 bytes");
void de__contact_select(const de__rw_lights *lights, u64 directional, u32 max_locals, f32 cutoff,
                        de__contact_metadata *out);
void de__contact_select_view(const de__rw_lights *lights, u64 directional, u32 max_locals,
                             f32 cutoff, u32 excluded_light_mask, const de__view_hidden *hidden,
                             de__contact_metadata *out);
typedef struct de__contact de__contact;
de_result de__contact_create_ex(de_gpu_device *dev, const de_allocator *alloc, de__vis_shared *vis,
                                de__contact **out);
de_result de__contact_create(de_renderer *r, de__contact **out);
void de__contact_destroy(de__contact *s);
de_result de__contact_build(de__contact *s, const de__view_frame *vf, const de__vis_context *vc,
                            const de__contact_config *config, u64 directional,
                            de__lighting_inputs *inputs);
void de__contact_complete(de__contact *s, de_view view, const de_rg *graph, bool attempted);
typedef struct de__contact_info {
    de__contact_metadata metadata;
    de_gpu_texture directional, local;
    u32 steps;
} de__contact_info;
bool de__contact_get(const de__contact *s, de_view view, de__contact_info *out);
DE_EXTERN_C_END
#endif
