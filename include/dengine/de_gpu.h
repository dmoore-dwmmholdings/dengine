/* de_gpu.h — thin bindless RHI over D3D12 (Windows), Vulkan (Linux), Metal (macOS). SPEC.md §12.1.
 * No implicit state tracking: barriers come from the render graph (de_render.h). */
#ifndef DE_GPU_H
#define DE_GPU_H
#include "de_base.h"
#include "de_memory.h"
#include "de_platform.h"
DE_EXTERN_C_BEGIN

typedef struct de_gpu_device de_gpu_device;
typedef struct de_gpu_cmd    de_gpu_cmd;
DE_HANDLE(de_gpu_buffer);   DE_HANDLE(de_gpu_texture);  DE_HANDLE(de_gpu_sampler);
DE_HANDLE(de_gpu_pipeline); DE_HANDLE(de_gpu_accel);    DE_HANDLE(de_gpu_swapchain);
DE_HANDLE(de_gpu_fence);    DE_HANDLE(de_gpu_query_heap);

typedef enum de_gpu_backend { DE_GPU_D3D12, DE_GPU_VULKAN, DE_GPU_METAL, DE_GPU_NULL } de_gpu_backend;
typedef enum de_gpu_queue   { DE_QUEUE_GRAPHICS, DE_QUEUE_COMPUTE, DE_QUEUE_COPY, DE_QUEUE_COUNT_ } de_gpu_queue;

/* ---------- Capabilities ---------- */
typedef enum de_rt_support { DE_RT_NONE, DE_RT_API_ONLY, DE_RT_HARDWARE } de_rt_support;

typedef struct de_gpu_caps {
    de_gpu_backend backend;
    char           device_name[128];
    u32            vendor_id, device_id;
    u64            dedicated_vram, shared_memory;
    de_rt_support  raytracing;            /* API_ONLY e.g. Metal on M1/M2 */
    bool           ray_query, rt_pipelines, opacity_micromaps;
    bool           mesh_shaders;
    bool           hdr_output, vrr;
    bool           enhanced_barriers;     /* D3D12 */
    bool           async_compute;
    u32            wave_size_min, wave_size_max;
    u32            max_bindless_textures, max_bindless_buffers;
} de_gpu_caps;

typedef struct de_gpu_device_desc {
    de_gpu_backend backend;       /* DE_GPU_NULL = platform default */
    i32            adapter_index; /* -1 = best */
    bool           debug_layer, gpu_validation, crash_breadcrumbs;
    const de_allocator *allocator;
} de_gpu_device_desc;

DE_API de_result          de_gpu_device_create(const de_gpu_device_desc *desc, de_gpu_device **out);
DE_API void               de_gpu_device_destroy(de_gpu_device *d);
DE_API const de_gpu_caps *de_gpu_get_caps(const de_gpu_device *d);
DE_API void               de_gpu_wait_idle(de_gpu_device *d);

/* ---------- Formats and usage ---------- */
typedef enum de_gpu_format {
    DE_FMT_UNKNOWN, DE_FMT_R8_UNORM, DE_FMT_RG8_UNORM, DE_FMT_RGBA8_UNORM, DE_FMT_RGBA8_SRGB,
    DE_FMT_BGRA8_UNORM, DE_FMT_BGRA8_SRGB, DE_FMT_R16_FLOAT, DE_FMT_RG16_FLOAT, DE_FMT_RGBA16_FLOAT,
    DE_FMT_R32_UINT, DE_FMT_RG32_UINT, DE_FMT_R32_FLOAT, DE_FMT_RG32_FLOAT, DE_FMT_RGBA32_FLOAT,
    DE_FMT_R11G11B10_FLOAT, DE_FMT_RGB10A2_UNORM, DE_FMT_D32_FLOAT, DE_FMT_D32_FLOAT_S8,
    DE_FMT_BC1_SRGB, DE_FMT_BC3_SRGB, DE_FMT_BC4_UNORM, DE_FMT_BC5_UNORM, DE_FMT_BC6H_UFLOAT,
    DE_FMT_BC7_UNORM, DE_FMT_BC7_SRGB
} de_gpu_format;

typedef enum de_gpu_usage {
    DE_USAGE_VERTEX = 1u << 0, DE_USAGE_INDEX = 1u << 1, DE_USAGE_CONSTANT = 1u << 2,
    DE_USAGE_STORAGE = 1u << 3, DE_USAGE_INDIRECT = 1u << 4, DE_USAGE_SAMPLED = 1u << 5,
    DE_USAGE_RENDER_TARGET = 1u << 6, DE_USAGE_DEPTH = 1u << 7, DE_USAGE_ACCEL_INPUT = 1u << 8,
    DE_USAGE_ACCEL_STORAGE = 1u << 9, DE_USAGE_COPY_SRC = 1u << 10, DE_USAGE_COPY_DST = 1u << 11
} de_gpu_usage;

typedef enum de_gpu_memory { DE_MEM_GPU_ONLY, DE_MEM_UPLOAD, DE_MEM_READBACK } de_gpu_memory;

/* ---------- Resources (each gets a bindless index on creation) ---------- */
typedef struct de_gpu_buffer_desc {
    u64 size; u32 usage; de_gpu_memory memory; u32 stride; const char *name;
} de_gpu_buffer_desc;

typedef enum de_gpu_texture_dim { DE_TEX_2D, DE_TEX_2D_ARRAY, DE_TEX_3D, DE_TEX_CUBE } de_gpu_texture_dim;
typedef struct de_gpu_texture_desc {
    de_gpu_texture_dim dim; de_gpu_format format;
    u32 width, height, depth_or_layers, mip_count, sample_count, usage;
    bool transient;            /* render-graph aliasable */
    const char *name;
} de_gpu_texture_desc;

typedef enum de_gpu_filter  { DE_FILTER_NEAREST, DE_FILTER_LINEAR } de_gpu_filter;
typedef enum de_gpu_address { DE_ADDR_WRAP, DE_ADDR_CLAMP, DE_ADDR_MIRROR, DE_ADDR_BORDER } de_gpu_address;
typedef struct de_gpu_sampler_desc {
    de_gpu_filter min, mag, mip; de_gpu_address u, v, w; f32 max_aniso; bool compare_reverse_z;
} de_gpu_sampler_desc;

DE_API de_gpu_buffer  de_gpu_buffer_create(de_gpu_device *d, const de_gpu_buffer_desc *desc);
DE_API de_gpu_texture de_gpu_texture_create(de_gpu_device *d, const de_gpu_texture_desc *desc);
DE_API de_gpu_sampler de_gpu_sampler_create(de_gpu_device *d, const de_gpu_sampler_desc *desc);
DE_API void           de_gpu_buffer_destroy(de_gpu_device *d, de_gpu_buffer b);    /* deferred until GPU done */
DE_API void           de_gpu_texture_destroy(de_gpu_device *d, de_gpu_texture t);
DE_API void          *de_gpu_buffer_map(de_gpu_device *d, de_gpu_buffer b);         /* UPLOAD/READBACK only */
DE_API u32            de_gpu_buffer_bindless(de_gpu_device *d, de_gpu_buffer b);
DE_API u32            de_gpu_texture_bindless(de_gpu_device *d, de_gpu_texture t, u32 mip, bool storage);
DE_API u32            de_gpu_sampler_bindless(de_gpu_device *d, de_gpu_sampler s);

/* ---------- Pipelines (shader blobs from the cooker: DXIL / SPIR-V / metallib) ---------- */
typedef struct de_gpu_shader_blob { const void *code; u64 size; const char *entry; } de_gpu_shader_blob;

typedef enum de_gpu_pipeline_kind { DE_PIPE_GRAPHICS, DE_PIPE_MESH, DE_PIPE_COMPUTE, DE_PIPE_RAYTRACING } de_gpu_pipeline_kind;
typedef enum de_gpu_cull { DE_CULL_NONE, DE_CULL_BACK, DE_CULL_FRONT } de_gpu_cull;
typedef enum de_gpu_blend { DE_BLEND_OPAQUE, DE_BLEND_ALPHA, DE_BLEND_PREMUL, DE_BLEND_ADD } de_gpu_blend;

typedef struct de_gpu_pipeline_desc {
    de_gpu_pipeline_kind kind;
    de_gpu_shader_blob   vs, ps, as, ms, cs;          /* by kind */
    de_gpu_shader_blob   raygen, miss, closest_hit, any_hit;
    de_gpu_format        color_formats[8]; u32 color_count;
    de_gpu_format        depth_format;
    de_gpu_blend         blend[8];
    de_gpu_cull          cull;
    bool                 depth_test, depth_write;     /* reverse-Z: GREATER_EQUAL */
    u32                  push_constant_bytes;         /* <= 128 */
    u32                  rt_max_recursion, rt_payload_bytes;
    const char          *name;
} de_gpu_pipeline_desc;

DE_API de_gpu_pipeline de_gpu_pipeline_create(de_gpu_device *d, const de_gpu_pipeline_desc *desc);
DE_API void            de_gpu_pipeline_destroy(de_gpu_device *d, de_gpu_pipeline p);

/* ---------- Acceleration structures ---------- */
typedef struct de_gpu_blas_geometry {
    de_gpu_buffer vertices; u64 vertex_offset; u32 vertex_stride, vertex_count; de_gpu_format vertex_format;
    de_gpu_buffer indices;  u64 index_offset;  u32 index_count; bool index_u16;
    bool opaque;
} de_gpu_blas_geometry;

typedef struct de_gpu_accel_desc {
    bool is_tlas;
    const de_gpu_blas_geometry *geometries; u32 geometry_count;  /* BLAS */
    u32  max_instances;                                          /* TLAS */
    bool allow_update, allow_compaction, prefer_fast_trace;
    const char *name;
} de_gpu_accel_desc;

typedef struct de_gpu_tlas_instance {
    f32 transform[12]; unsigned custom_index : 24; unsigned mask : 8;
    unsigned sbt_offset : 24; unsigned flags : 8; de_gpu_accel blas;
} de_gpu_tlas_instance;

DE_API de_gpu_accel de_gpu_accel_create(de_gpu_device *d, const de_gpu_accel_desc *desc);
DE_API void         de_gpu_accel_destroy(de_gpu_device *d, de_gpu_accel a);
DE_API u32          de_gpu_accel_bindless(de_gpu_device *d, de_gpu_accel tlas);

/* ---------- Command recording ---------- */
typedef enum de_gpu_access {
    DE_ACCESS_NONE = 0, DE_ACCESS_SHADER_READ = 1u << 0, DE_ACCESS_SHADER_WRITE = 1u << 1,
    DE_ACCESS_RENDER_TARGET = 1u << 2, DE_ACCESS_DEPTH_READ = 1u << 3, DE_ACCESS_DEPTH_WRITE = 1u << 4,
    DE_ACCESS_INDIRECT = 1u << 5, DE_ACCESS_COPY_SRC = 1u << 6, DE_ACCESS_COPY_DST = 1u << 7,
    DE_ACCESS_ACCEL_READ = 1u << 8, DE_ACCESS_ACCEL_WRITE = 1u << 9, DE_ACCESS_PRESENT = 1u << 10
} de_gpu_access;

typedef struct de_gpu_barrier {
    de_gpu_texture texture; de_gpu_buffer buffer; de_gpu_accel accel;   /* exactly one set */
    u32 before, after;   /* de_gpu_access */
    u32 mip_first, mip_count, layer_first, layer_count;                 /* 0 count = all */
    de_gpu_queue src_queue, dst_queue;                                  /* ownership transfer */
} de_gpu_barrier;

typedef struct de_gpu_attachment {
    de_gpu_texture texture; u32 mip, layer;
    bool clear; f32 clear_color[4]; f32 clear_depth;
    bool store;
} de_gpu_attachment;

DE_API de_gpu_cmd *de_gpu_cmd_begin(de_gpu_device *d, de_gpu_queue q, const char *name);
DE_API void de_gpu_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count);
DE_API void de_gpu_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                                    const de_gpu_attachment *depth);
DE_API void de_gpu_cmd_end_render(de_gpu_cmd *c);
DE_API void de_gpu_cmd_set_pipeline(de_gpu_cmd *c, de_gpu_pipeline p);
DE_API void de_gpu_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes);
DE_API void de_gpu_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h);
DE_API void de_gpu_cmd_draw_indexed_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset,
                                             de_gpu_buffer count, u64 count_offset, u32 max_draws);
DE_API void de_gpu_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset,
                                              de_gpu_buffer count, u64 count_offset, u32 max);
DE_API void de_gpu_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z);
DE_API void de_gpu_cmd_dispatch_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset);
DE_API void de_gpu_cmd_dispatch_rays(de_gpu_cmd *c, u32 w, u32 h, u32 depth);
DE_API void de_gpu_cmd_build_blas(de_gpu_cmd *c, de_gpu_accel blas, bool update);
DE_API void de_gpu_cmd_build_tlas(de_gpu_cmd *c, de_gpu_accel tlas, de_gpu_buffer instances, u32 count, bool update);
DE_API void de_gpu_cmd_copy_buffer(de_gpu_cmd *c, de_gpu_buffer dst, u64 dst_off, de_gpu_buffer src, u64 src_off, u64 bytes);
DE_API void de_gpu_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de_gpu_texture dst, u32 mip, u32 layer,
                                              de_gpu_buffer src, u64 src_off, u32 row_pitch);
DE_API void de_gpu_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color);
DE_API void de_gpu_cmd_marker_pop(de_gpu_cmd *c);
DE_API void de_gpu_cmd_breadcrumb(de_gpu_cmd *c, u32 id);   /* GPU crash diagnostics */
DE_API void de_gpu_cmd_timestamp(de_gpu_cmd *c, de_gpu_query_heap h, u32 index);

/* ---------- Submission and sync (timeline fences) ---------- */
typedef struct de_gpu_wait   { de_gpu_fence fence; u64 value; } de_gpu_wait;
typedef struct de_gpu_submit {
    de_gpu_cmd *const *cmds; u32 cmd_count;
    const de_gpu_wait *waits; u32 wait_count;
    de_gpu_fence signal; u64 signal_value;
} de_gpu_submit;

DE_API de_gpu_fence de_gpu_fence_create(de_gpu_device *d, u64 initial);
DE_API u64          de_gpu_fence_value(de_gpu_device *d, de_gpu_fence f);
DE_API de_result    de_gpu_fence_wait(de_gpu_device *d, de_gpu_fence f, u64 value, u64 timeout_ns);
DE_API de_result    de_gpu_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s);

/* ---------- Swapchain ---------- */
typedef enum de_gpu_colorspace { DE_CS_SRGB, DE_CS_HDR10_PQ, DE_CS_SCRGB, DE_CS_EDR } de_gpu_colorspace;
typedef enum de_gpu_present    { DE_PRESENT_VSYNC, DE_PRESENT_MAILBOX, DE_PRESENT_IMMEDIATE } de_gpu_present;
typedef struct de_gpu_swapchain_desc {
    de_window window; u32 image_count; de_gpu_colorspace colorspace; de_gpu_present present; bool allow_vrr;
} de_gpu_swapchain_desc;

DE_API de_gpu_swapchain de_gpu_swapchain_create(de_gpu_device *d, const de_gpu_swapchain_desc *desc);
DE_API de_result        de_gpu_swapchain_acquire(de_gpu_device *d, de_gpu_swapchain s, de_gpu_texture *image);
DE_API de_result        de_gpu_swapchain_present(de_gpu_device *d, de_gpu_swapchain s);
DE_API de_result        de_gpu_swapchain_resize(de_gpu_device *d, de_gpu_swapchain s, u32 w, u32 h);
DE_API de_gpu_colorspace de_gpu_swapchain_colorspace(de_gpu_device *d, de_gpu_swapchain s);

DE_EXTERN_C_END
#endif
