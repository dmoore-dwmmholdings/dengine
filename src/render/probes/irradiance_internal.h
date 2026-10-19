#ifndef DE_RENDER_IRRADIANCE_INTERNAL_H
#define DE_RENDER_IRRADIANCE_INTERNAL_H
#include "render/probes/probes_internal.h"
DE_EXTERN_C_BEGIN
#define DE__GI_VOLUMES     8u
#define DE__GI_PROBES      4096u
#define DE__GI_BLOB_HEADER 80u
#define DE__GI_ACCESS      (DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE)
/** X-fastest probes, tiles in rows of at most 64. Irradiance tile 10x10, distance tile18x18.
 * Both have one folded-octahedron border texel. Distances are normalized by range, no hit=1.
 * Reserved offsets/classification: float4 per probe (xyz offset metres, w=0 active). */
typedef struct de__gi_layout {
    u32 counts[3], probes, columns, rows, iw, ih, dw, dh, bounces;
    f32 range;
    u64 payload_bytes;
} de__gi_layout;
/** Shader GiVolume (144 bytes); rows transform render-origin world -> volume local. */
typedef struct de__gi_gpu {
    f32 local[3][4];
    f32 extent[4], step[4], rotation[4];
    u32 counts[4], textures[4];
    f32 params[4];
} de__gi_gpu;
DE_STATIC_ASSERT(sizeof(de__gi_gpu) == 144, "GiVolume 144 bytes");
DE_STATIC_ASSERT(offsetof(de__gi_gpu, extent) == 48, "GiVolume extent offset");
DE_STATIC_ASSERT(offsetof(de__gi_gpu, step) == 64, "GiVolume step offset");
DE_STATIC_ASSERT(offsetof(de__gi_gpu, rotation) == 80, "GiVolume rotation offset");
DE_STATIC_ASSERT(offsetof(de__gi_gpu, counts) == 96, "GiVolume counts offset");
DE_STATIC_ASSERT(offsetof(de__gi_gpu, textures) == 112, "GiVolume textures offset");
DE_STATIC_ASSERT(offsetof(de__gi_gpu, params) == 128, "GiVolume params offset");
de_result de__gi_layout_make(const DeIrradianceVolume *volume, de__gi_layout *out);
u64 de__gi_settings_hash(const de__irradiance_snapshot *volume);
de_result de__gi_blob_write(void *dst, u64 size, u64 hash, const de__gi_layout *layout,
                            const void *payload);
de_result de__gi_blob_read(const void *src, u64 size, u64 hash, de__gi_layout *layout,
                           const void **payload);
struct de__irradiance;
de_result de__irradiance_create(de_renderer *r, struct de__irradiance **out);
void de__irradiance_destroy(struct de__irradiance *s);
void de__irradiance_prepare(de_renderer *r, const de__render_world_frame *world);
void de__irradiance_view_complete(de_renderer *r, de_view view, bool success);
/** Read only after the render world is idle; Metal standalone-retirement guard counter. */
u64 de__irradiance_resource_drains(const de_renderer *r);
void de__irradiance_debug_skip_depth(de_renderer *r, u32 mask);
void de__irradiance_after_views(de_renderer *r, u64 frame);
typedef struct de__irradiance_info {
    de__gi_layout layout;
    de__gi_gpu gpu;
    u32 bounce, probe, faces, captures;
    bool ready, baked;
    de_gpu_texture irradiance, distance, raw_radiance, raw_distance;
    de_gpu_buffer classification, convolution;
    u64 convolution_capacity;
} de__irradiance_info;
bool de__irradiance_get(const de_renderer *r, u64 entity, de__irradiance_info *out);
de_result de__irradiance_readback(de_renderer *r, u64 entity, void *payload, u64 bytes);
/* DENG-49: optional production data source. Static resources/sampling remain owned here.
 * A successful callback may replace only this view's GI inputs; fallback leaves them intact. */
struct de__lighting_context;
struct de__lighting_inputs;
typedef de_result (*de__gi_update_source_fn)(void *, de_rg *, de__vis_context *,
                                             struct de__lighting_context *,
                                             struct de__lighting_inputs *);
void de__irradiance_set_update_source(struct de__irradiance *, de__gi_update_source_fn,
                                      bool (*owns_frame)(void *, const de__render_world_frame *),
                                      void *);
de_result de__irradiance_update_source(de_renderer *, de_rg *, de__vis_context *,
                                       struct de__lighting_context *, struct de__lighting_inputs *);
DE_EXTERN_C_END
#endif
