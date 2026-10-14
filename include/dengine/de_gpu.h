/* de_gpu.h — thin bindless RHI over D3D12 (Windows), Vulkan (Linux), Metal (macOS).
 * SPEC.md §12.1.
 *
 * Shape (S09, DENG-281):
 *   - One device per backend. `(de_gpu_device_desc){0}` picks the platform default backend;
 *     DE_GPU_NULL is a device-less backend for headless code and tests (buffers live in CPU
 *     memory, commands are no-ops, fences complete at submit).
 *   - Objects are typed handles (DE_HANDLE): 20-bit slot index and 12-bit generation, never 0.
 *     A stale handle (destroyed object, or a slot that was reused) is detected in every build:
 *     the call fails (invalid handle, NULL or DE_GPU_BINDLESS_INVALID) and sets the error context.
 *   - Bindless: buffers, textures, samplers (and TLASes from S16) get uint32 indices into global
 *     descriptor tables at creation. Shaders index with them (shaders/common/bindless.slang).
 *     Index 0 of every table is reserved and never handed out: DE_GPU_BINDLESS_INVALID.
 *   - Destruction is deferred: a destroyed object's handle is invalid at once, but the object and
 *     its bindless indices are released only after every queue has completed the work submitted
 *     before the destroy call. Destroy an object only after the command lists that use it have
 *     been submitted.
 *   - No implicit state tracking: barriers come from the caller (the render graph, de_render.h).
 *   - Validation: create descs and submits are checked in every build (a bad desc fails with
 *     DE_ERR_INVALID_ARGUMENT and a message naming the field); command recording is checked in
 *     Debug and Dev builds. Messages go to the error context (de_error_last) and to the `gpu`
 *     log category.
 *
 * Threading: device create/destroy on one thread. Object create/destroy, bindless queries, fence
 * calls and queue submits are thread-safe. A de_gpu_cmd is recorded by one thread at a time.
 */
#ifndef DE_GPU_H
#define DE_GPU_H
#include "de_base.h"
#include "de_memory.h"
#include "de_platform.h"
DE_EXTERN_C_BEGIN

typedef struct de_gpu_device de_gpu_device;
typedef struct de_gpu_cmd de_gpu_cmd;
DE_HANDLE(de_gpu_buffer);
DE_HANDLE(de_gpu_texture);
DE_HANDLE(de_gpu_sampler);
DE_HANDLE(de_gpu_pipeline);
DE_HANDLE(de_gpu_accel);
DE_HANDLE(de_gpu_swapchain);
DE_HANDLE(de_gpu_fence);
DE_HANDLE(de_gpu_query_heap);

/** The reserved bindless index 0: "no index". Returned when an object has no index. */
#define DE_GPU_BINDLESS_INVALID 0u
/** Root/push-constant limit per pipeline (SPEC §12.1). */
#define DE_GPU_MAX_PUSH_CONSTANT_BYTES 128u
/** Most color attachments of a pipeline or render pass. */
#define DE_GPU_MAX_COLOR_ATTACHMENTS 8u
/** Most mip levels of a texture (a 32768-texel edge). */
#define DE_GPU_MAX_MIPS 16u

/**
 * Backend. DE_GPU_DEFAULT (0) picks the platform default: D3D12 on Windows, Metal on macOS,
 * Vulkan elsewhere (de_gpu_platform_default_backend). The numbering is not stable across versions;
 * never store these values in files (cooked shaders use their own target enum).
 */
typedef enum de_gpu_backend {
    DE_GPU_DEFAULT = 0,
    DE_GPU_D3D12,
    DE_GPU_VULKAN,
    DE_GPU_METAL,
    DE_GPU_NULL, /**< no device: CPU-memory buffers, no-op commands, fences done at submit */
    DE_GPU_BACKEND_COUNT_
} de_gpu_backend;
typedef enum de_gpu_queue {
    DE_QUEUE_GRAPHICS,
    DE_QUEUE_COMPUTE,
    DE_QUEUE_COPY,
    DE_QUEUE_COUNT_
} de_gpu_queue;

/** "d3d12", "vulkan", "metal", "null", "default"; "unknown" when out of range. Never NULL. */
DE_API const char *de_gpu_backend_name(de_gpu_backend b);
/** The backend DE_GPU_DEFAULT stands for on this platform (never DE_GPU_DEFAULT or NULL). */
DE_API de_gpu_backend de_gpu_platform_default_backend(void);

/* ---------- Capabilities ---------- */
typedef enum de_rt_support { DE_RT_NONE, DE_RT_API_ONLY, DE_RT_HARDWARE } de_rt_support;

/** What the device can do. Filled by the backend at device creation; constant afterwards. */
typedef struct de_gpu_caps {
    de_gpu_backend backend; /**< the backend actually used (never DE_GPU_DEFAULT) */
    char device_name[128];
    u32 vendor_id, device_id;
    u64 dedicated_vram, shared_memory;
    bool unified_memory; /**< CPU and GPU share one memory pool (Apple silicon, integrated GPUs) */
    de_rt_support raytracing; /* API_ONLY e.g. Metal on M1/M2 */
    bool ray_query, rt_pipelines, opacity_micromaps;
    bool mesh_shaders;
    bool hdr_output, vrr;
    bool enhanced_barriers; /* D3D12 */
    bool async_compute;
    u32 wave_size_min, wave_size_max;
    /** Bindless table sizes, one index space each (index 0 reserved in every space). */
    u32 max_bindless_textures;         /**< sampled textures */
    u32 max_bindless_buffers;          /**< buffers */
    u32 max_bindless_storage_textures; /**< storage (read-write) texture mips */
    u32 max_bindless_samplers;         /**< samplers */
    u32 max_bindless_accels;           /**< TLASes; 0 without ray tracing */
    u32 max_push_constant_bytes;       /**< at most DE_GPU_MAX_PUSH_CONSTANT_BYTES */
    u64 timestamp_frequency;           /**< timestamp ticks per second (de_gpu_cmd_timestamp) */
    u32 mesh_max_output_vertices;      /**< per mesh-shader group; 0 without mesh shaders */
    u32 mesh_max_output_primitives;    /**< per mesh-shader group; 0 without mesh shaders */
    bool base_tier_ok;                 /**< meets the SPEC §3.3 Base tier for its backend */
    char base_tier_reason[128];        /**< why not, when !base_tier_ok; "" otherwise */
} de_gpu_caps;

/** de_gpu_device_create() parameters. `(de_gpu_device_desc){0}` is valid. */
typedef struct de_gpu_device_desc {
    de_gpu_backend backend; /**< DE_GPU_DEFAULT (0) = platform default */
    u32 adapter;            /**< 0 = best adapter; n = the n-th adapter (1-based) */
    bool debug_layer, gpu_validation, crash_breadcrumbs;
    const de_allocator *allocator; /**< front-end and CPU-side memory; NULL = de_heap_allocator() */
    /**
     * Directory (virtual path) of the persistent pipeline cache (S10, DENG-323): the backend loads
     * its cache file from it at device creation and saves it there, atomically, at device destroy.
     * NULL = "cache:/pipelines"; "" = no persistent cache. The file name names the backend, the
     * vendor, the device and the driver version, so a driver update starts a new cache. A missing
     * file is normal; an unreadable, corrupt or foreign one is ignored with a warning.
     */
    const char *pipeline_cache_dir;
} de_gpu_device_desc;

/**
 * Creates a device. Fails with DE_ERR_UNSUPPORTED when the backend is not compiled into this
 * build (or not registered), or the machine cannot run it; DE_ERR_DEVICE_LOST / DE_ERR_* from the
 * backend otherwise. `*out` is NULL on failure.
 */
DE_API de_result de_gpu_device_create(const de_gpu_device_desc *desc, de_gpu_device **out);
/**
 * Waits for the GPU, releases every deferred destruction, destroys objects that are still alive
 * (logging a warning with their count per kind), then the device. NULL is ignored.
 */
DE_API void de_gpu_device_destroy(de_gpu_device *d);
DE_API const de_gpu_caps *de_gpu_get_caps(const de_gpu_device *d);
/** Waits until every queue is idle, then releases every deferred destruction. */
DE_API void de_gpu_wait_idle(de_gpu_device *d);

/* ---------- Formats and usage ---------- */
typedef enum de_gpu_format {
    DE_FMT_UNKNOWN,
    DE_FMT_R8_UNORM,
    DE_FMT_RG8_UNORM,
    DE_FMT_RGBA8_UNORM,
    DE_FMT_RGBA8_SRGB,
    DE_FMT_BGRA8_UNORM,
    DE_FMT_BGRA8_SRGB,
    DE_FMT_R16_FLOAT,
    DE_FMT_RG16_FLOAT,
    DE_FMT_RGBA16_FLOAT,
    DE_FMT_R32_UINT,
    DE_FMT_RG32_UINT,
    DE_FMT_R32_FLOAT,
    DE_FMT_RG32_FLOAT,
    DE_FMT_RGBA32_FLOAT,
    DE_FMT_R11G11B10_FLOAT,
    DE_FMT_RGB10A2_UNORM,
    DE_FMT_D32_FLOAT,
    DE_FMT_D32_FLOAT_S8,
    DE_FMT_BC1_SRGB,
    DE_FMT_BC3_SRGB,
    DE_FMT_BC4_UNORM,
    DE_FMT_BC5_UNORM,
    DE_FMT_BC6H_UFLOAT,
    DE_FMT_BC7_UNORM,
    DE_FMT_BC7_SRGB,
    DE_FMT_COUNT_
} de_gpu_format;

typedef enum de_gpu_usage {
    DE_USAGE_VERTEX = 1u << 0,
    DE_USAGE_INDEX = 1u << 1,
    DE_USAGE_CONSTANT = 1u << 2,
    DE_USAGE_STORAGE = 1u << 3,
    DE_USAGE_INDIRECT = 1u << 4,
    DE_USAGE_SAMPLED = 1u << 5,
    DE_USAGE_RENDER_TARGET = 1u << 6,
    DE_USAGE_DEPTH = 1u << 7,
    DE_USAGE_ACCEL_INPUT = 1u << 8,
    DE_USAGE_ACCEL_STORAGE = 1u << 9,
    DE_USAGE_COPY_SRC = 1u << 10,
    DE_USAGE_COPY_DST = 1u << 11
} de_gpu_usage;

typedef enum de_gpu_memory { DE_MEM_GPU_ONLY, DE_MEM_UPLOAD, DE_MEM_READBACK } de_gpu_memory;

/* ---------- Resources (each gets a bindless index on creation) ---------- */

/**
 * Buffer. `size` > 0 and `usage` != 0 are required. Usage rules: buffers take VERTEX, INDEX,
 * CONSTANT, STORAGE, INDIRECT, ACCEL_*, COPY_*; UPLOAD buffers no STORAGE writes, ACCEL_STORAGE
 * or COPY_DST; READBACK buffers only COPY_DST. `stride` (0 = raw) must be a multiple of 4 that
 * divides `size`. A buffer with VERTEX, INDEX, CONSTANT or STORAGE usage gets a bindless index.
 * `name` is copied (debug names, messages); it may be NULL.
 */
typedef struct de_gpu_buffer_desc {
    u64 size;
    u32 usage;
    de_gpu_memory memory;
    u32 stride;
    const char *name;
} de_gpu_buffer_desc;

typedef enum de_gpu_texture_dim {
    DE_TEX_2D,
    DE_TEX_2D_ARRAY,
    DE_TEX_3D,
    DE_TEX_CUBE
} de_gpu_texture_dim;

/**
 * Texture. `format` and `width` are required; `height` 0 means 1 for 2D, and `depth_or_layers` 0
 * means 1 (6 for a cube, whose count must be a multiple of 6 with width == height). `mip_count` 0
 * means 1; at most the full chain and DE_GPU_MAX_MIPS. `sample_count` 0 means 1; 2, 4 or 8 only
 * for single-mip 2D textures without STORAGE. `usage` != 0: SAMPLED, STORAGE, RENDER_TARGET,
 * DEPTH, COPY_*. DEPTH needs a depth format and excludes RENDER_TARGET and STORAGE; depth formats
 * need DEPTH; block-compressed formats take only SAMPLED and COPY_* and need a width and height
 * that are multiples of 4; sRGB formats cannot be STORAGE.
 */
typedef struct de_gpu_texture_desc {
    de_gpu_texture_dim dim;
    de_gpu_format format;
    u32 width, height, depth_or_layers, mip_count, sample_count, usage;
    bool transient; /* render-graph aliasable */
    const char *name;
} de_gpu_texture_desc;

typedef enum de_gpu_filter { DE_FILTER_NEAREST, DE_FILTER_LINEAR } de_gpu_filter;
typedef enum de_gpu_address {
    DE_ADDR_WRAP,
    DE_ADDR_CLAMP,
    DE_ADDR_MIRROR,
    DE_ADDR_BORDER
} de_gpu_address;
/** Sampler. `(de_gpu_sampler_desc){0}`: nearest, wrap, no anisotropy. `max_aniso` 0..16. */
typedef struct de_gpu_sampler_desc {
    de_gpu_filter min, mag, mip;
    de_gpu_address u, v, w;
    f32 max_aniso;
    bool compare_reverse_z;
} de_gpu_sampler_desc;

/** Create functions return an invalid handle (id 0) on failure and set the error context. */
DE_API de_gpu_buffer de_gpu_buffer_create(de_gpu_device *d, const de_gpu_buffer_desc *desc);
DE_API de_gpu_texture de_gpu_texture_create(de_gpu_device *d, const de_gpu_texture_desc *desc);
DE_API de_gpu_sampler de_gpu_sampler_create(de_gpu_device *d, const de_gpu_sampler_desc *desc);
/** Destroy functions: the handle is invalid at once; the object and its bindless indices are
 * released after the GPU is done with the work submitted before the call. Invalid handles
 * (including id 0) are ignored; stale ones also set the error context. */
DE_API void de_gpu_buffer_destroy(de_gpu_device *d, de_gpu_buffer b); /* deferred until GPU done */
DE_API void de_gpu_texture_destroy(de_gpu_device *d, de_gpu_texture t);
DE_API void de_gpu_sampler_destroy(de_gpu_device *d, de_gpu_sampler s);
/**
 * CPU pointer to an UPLOAD or READBACK buffer, valid until the buffer is destroyed (persistent
 * mapping). NULL, with an error, for a GPU_ONLY buffer or an invalid handle.
 */
DE_API void *de_gpu_buffer_map(de_gpu_device *d, de_gpu_buffer b); /* UPLOAD/READBACK only */
/** The buffer's bindless index, or DE_GPU_BINDLESS_INVALID when it has none. */
DE_API u32 de_gpu_buffer_bindless(de_gpu_device *d, de_gpu_buffer b);
/**
 * Bindless index of a texture view. Sampled and storage views are separate index spaces (tables):
 *   - `storage == false`: the sampled view of the whole texture (all mips and layers), from the
 *     sampled-texture table (`de_tex2d(idx)` and friends). Needs DE_USAGE_SAMPLED; `mip` must be 0.
 *   - `storage == true`: the read-write view of mip `mip` (all layers), from the storage-texture
 *     table (`de_rwtex2d(idx)`). Needs DE_USAGE_STORAGE; each mip has its own index.
 * The same number can be valid in both spaces for different textures. Returns
 * DE_GPU_BINDLESS_INVALID (and sets the error context) when the view does not exist.
 */
DE_API u32 de_gpu_texture_bindless(de_gpu_device *d, de_gpu_texture t, u32 mip, bool storage);
DE_API u32 de_gpu_sampler_bindless(de_gpu_device *d, de_gpu_sampler s);

/* ---------- Pipelines (shader blobs from the cooker: DXIL / SPIR-V / metallib) ---------- */
typedef struct de_gpu_shader_blob {
    const void *code;
    u64 size;
    const char *entry;
} de_gpu_shader_blob;

typedef enum de_gpu_pipeline_kind {
    DE_PIPE_GRAPHICS,
    DE_PIPE_MESH,
    DE_PIPE_COMPUTE,
    DE_PIPE_RAYTRACING
} de_gpu_pipeline_kind;
typedef enum de_gpu_cull { DE_CULL_NONE, DE_CULL_BACK, DE_CULL_FRONT } de_gpu_cull;
typedef enum de_gpu_blend {
    DE_BLEND_OPAQUE,
    DE_BLEND_ALPHA,
    DE_BLEND_PREMUL,
    DE_BLEND_ADD
} de_gpu_blend;

/**
 * Pipeline. Required blobs by kind: GRAPHICS `vs` (`ps` optional), MESH `ms` (`as`, `ps`
 * optional; needs caps.mesh_shaders, else DE_ERR_UNSUPPORTED), COMPUTE `cs`. RAYTRACING is
 * DE_ERR_UNSUPPORTED until S16. Blobs of other stages must be empty. `push_constant_bytes` <=
 * DE_GPU_MAX_PUSH_CONSTANT_BYTES, a multiple of 4, and at least what the shaders use (their
 * reflection's push_constant_bytes). Color formats must be renderable color formats, one per
 * pixel-shader output; `depth_format` a depth format or UNKNOWN; depth test/write need one.
 *
 * Fixed state, the same on every backend (S10, DENG-323):
 *   - Every pipeline uses the bindless tables (shaders/common/bindless.slang) and one
 *     push-constant block; there are no vertex buffers (shaders pull vertices from bindless
 *     buffers) and the topology is a triangle list.
 *   - Front faces are counter-clockwise in clip space (y up); `cull` drops back or front faces.
 *   - Depth test (`depth_test`): reverse-Z, a fragment passes when its depth is >= the stored one
 *     (GREATER_EQUAL). Depth is not clamped. No stencil.
 *   - Blend (color, alpha): OPAQUE off; ALPHA (src*sa + dst*(1-sa), a + da*(1-sa)); PREMUL
 *     (src + dst*(1-sa), a + da*(1-sa)); ADD (src + dst, a + da). All channels are written.
 *   - Viewport and scissor are command state (de_gpu_cmd_viewport, de_gpu_cmd_scissor), set to
 *     the whole render area by de_gpu_cmd_begin_render. One sample per pixel.
 */
typedef struct de_gpu_pipeline_desc {
    de_gpu_pipeline_kind kind;
    de_gpu_shader_blob vs, ps, as, ms, cs; /* by kind */
    de_gpu_shader_blob raygen, miss, closest_hit, any_hit;
    de_gpu_format color_formats[8];
    u32 color_count;
    de_gpu_format depth_format;
    de_gpu_blend blend[8];
    de_gpu_cull cull;
    bool depth_test, depth_write; /* reverse-Z: GREATER_EQUAL */
    u32 push_constant_bytes;      /* <= 128 */
    u32 rt_max_recursion, rt_payload_bytes;
    const char *name;
    /**
     * Threadgroup size of `cs` (COMPUTE) or of `ms` (MESH), from the shader's reflection
     * (de__shader_reflection.threadgroup). All 0 = not given: Vulkan and D3D12 read it from the
     * blob; Metal needs it for COMPUTE and MESH pipelines. When given, every element is > 0, and
     * backends that can read it from the blob check that it matches.
     */
    u32 threadgroup_size[3];
    /** Threadgroup size of `as` (amplification / task / object shader) of a MESH pipeline, from its
     * reflection. All 0 = not given (as above). */
    u32 object_threadgroup_size[3];
} de_gpu_pipeline_desc;

DE_API de_gpu_pipeline de_gpu_pipeline_create(de_gpu_device *d, const de_gpu_pipeline_desc *desc);
DE_API void de_gpu_pipeline_destroy(de_gpu_device *d, de_gpu_pipeline p);

/* ---------- Acceleration structures (S16: DE_ERR_UNSUPPORTED until then) ---------- */
typedef struct de_gpu_blas_geometry {
    de_gpu_buffer vertices;
    u64 vertex_offset;
    u32 vertex_stride, vertex_count;
    de_gpu_format vertex_format;
    de_gpu_buffer indices;
    u64 index_offset;
    u32 index_count;
    bool index_u16;
    bool opaque;
} de_gpu_blas_geometry;

typedef struct de_gpu_accel_desc {
    bool is_tlas;
    const de_gpu_blas_geometry *geometries;
    u32 geometry_count; /* BLAS */
    u32 max_instances;  /* TLAS */
    bool allow_update, allow_compaction, prefer_fast_trace;
    const char *name;
} de_gpu_accel_desc;

typedef struct de_gpu_tlas_instance {
    f32 transform[12];
    unsigned custom_index : 24;
    unsigned mask : 8;
    unsigned sbt_offset : 24;
    unsigned flags : 8;
    de_gpu_accel blas;
} de_gpu_tlas_instance;

/** Returns an invalid handle with DE_ERR_UNSUPPORTED on every backend until S16. */
DE_API de_gpu_accel de_gpu_accel_create(de_gpu_device *d, const de_gpu_accel_desc *desc);
DE_API void de_gpu_accel_destroy(de_gpu_device *d, de_gpu_accel a);
DE_API u32 de_gpu_accel_bindless(de_gpu_device *d, de_gpu_accel tlas);

/* ---------- Queries ---------- */
typedef enum de_gpu_query_type { DE_QUERY_TIMESTAMP } de_gpu_query_type;
/**
 * Query heap (S10, DENG-326): `count` timestamp queries, 1 .. DE_GPU_MAX_QUERIES. `queue` is the
 * queue whose command lists write it: a heap for DE_QUEUE_GRAPHICS or DE_QUEUE_COMPUTE serves
 * both of those queues; a DE_QUEUE_COPY heap serves only copy lists. A device that cannot time
 * copies refuses a copy heap with DE_ERR_UNSUPPORTED (D3D12 without copy-queue timestamps, Vulkan
 * with a transfer-only copy family): time copies on the graphics or compute queue there.
 * A query holds its value until it is written again; resolve only queries written by this list
 * or by a list submitted before it.
 */
typedef struct de_gpu_query_heap_desc {
    de_gpu_query_type type;
    u32 count;
    const char *name;
    de_gpu_queue queue; /**< DENG-326; 0 = DE_QUEUE_GRAPHICS */
} de_gpu_query_heap_desc;
/** Most queries in one heap. Metal holds at most 4096 (its counter sample buffer limit) and
 * refuses more with DE_ERR_UNSUPPORTED. */
#define DE_GPU_MAX_QUERIES 65536u
DE_API de_gpu_query_heap de_gpu_query_heap_create(de_gpu_device *d,
                                                  const de_gpu_query_heap_desc *desc);
DE_API void de_gpu_query_heap_destroy(de_gpu_device *d, de_gpu_query_heap h);

/* ---------- Command recording ---------- */
typedef enum de_gpu_access {
    DE_ACCESS_NONE = 0,
    DE_ACCESS_SHADER_READ = 1u << 0,
    DE_ACCESS_SHADER_WRITE = 1u << 1,
    DE_ACCESS_RENDER_TARGET = 1u << 2,
    DE_ACCESS_DEPTH_READ = 1u << 3,
    DE_ACCESS_DEPTH_WRITE = 1u << 4,
    DE_ACCESS_INDIRECT = 1u << 5,
    DE_ACCESS_COPY_SRC = 1u << 6,
    DE_ACCESS_COPY_DST = 1u << 7,
    DE_ACCESS_ACCEL_READ = 1u << 8,
    DE_ACCESS_ACCEL_WRITE = 1u << 9,
    DE_ACCESS_PRESENT = 1u << 10
} de_gpu_access;

typedef struct de_gpu_barrier {
    de_gpu_texture texture;
    de_gpu_buffer buffer;
    de_gpu_accel accel;                                 /* exactly one set */
    u32 before, after;                                  /* de_gpu_access */
    u32 mip_first, mip_count, layer_first, layer_count; /* 0 count = all */
    de_gpu_queue src_queue, dst_queue;                  /* ownership transfer */
} de_gpu_barrier;

typedef struct de_gpu_attachment {
    de_gpu_texture texture;
    u32 mip, layer;
    bool clear;
    f32 clear_color[4];
    f32 clear_depth;
    bool store;
} de_gpu_attachment;

/**
 * Starts a command list for queue `q`. It is recorded by one thread, then handed to exactly one
 * de_gpu_queue_submit() on the same queue; the device recycles it once the GPU is done. NULL on
 * failure. In Debug and Dev builds every recording call checks its arguments and the command
 * list's state (render pass open or not, pipeline bound, push-constant size, handles); a failed
 * check logs to `gpu`, sets the error context and skips the command.
 */
DE_API de_gpu_cmd *de_gpu_cmd_begin(de_gpu_device *d, de_gpu_queue q, const char *name);
DE_API void de_gpu_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count);
DE_API void de_gpu_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                                    const de_gpu_attachment *depth);
DE_API void de_gpu_cmd_end_render(de_gpu_cmd *c);
/** Binds a pipeline (and, on the first bind of its kind in the list, the bindless tables). */
DE_API void de_gpu_cmd_set_pipeline(de_gpu_cmd *c, de_gpu_pipeline p);
/** Sets push-constant bytes [0, bytes) of the bound pipeline (at most its push_constant_bytes). */
DE_API void de_gpu_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes);
/** Viewport in pixels from the top-left of the render area (y down); depth range 0..1. */
DE_API void de_gpu_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h);
/**
 * Indexed draws whose arguments the GPU wrote: `max_draws` records of 5 u32 (index_count,
 * instance_count, first_index, vertex_offset as i32, first_instance), 20 bytes apart from
 * `offset` of `args`. With a valid `count` buffer, the u32 at `count_offset` gives the number of
 * draws, capped at `max_draws`. Both buffers need DE_USAGE_INDIRECT and DE_ACCESS_INDIRECT; the
 * index buffer comes from de_gpu_cmd_set_index_buffer.
 */
DE_API void de_gpu_cmd_draw_indexed_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset,
                                             de_gpu_buffer count, u64 count_offset, u32 max_draws);
/** Mesh dispatches whose arguments the GPU wrote: `max` records of 3 u32 (x, y, z), 12 bytes
 * apart, with an optional count buffer as in de_gpu_cmd_draw_indexed_indirect. */
DE_API void de_gpu_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset,
                                              de_gpu_buffer count, u64 count_offset, u32 max);
DE_API void de_gpu_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z);
DE_API void de_gpu_cmd_dispatch_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset);
DE_API void de_gpu_cmd_dispatch_rays(de_gpu_cmd *c, u32 w, u32 h, u32 depth);
DE_API void de_gpu_cmd_build_blas(de_gpu_cmd *c, de_gpu_accel blas, bool update);
DE_API void de_gpu_cmd_build_tlas(de_gpu_cmd *c, de_gpu_accel tlas, de_gpu_buffer instances,
                                  u32 count, bool update);
DE_API void de_gpu_cmd_copy_buffer(de_gpu_cmd *c, de_gpu_buffer dst, u64 dst_off, de_gpu_buffer src,
                                   u64 src_off, u64 bytes);
DE_API void de_gpu_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de_gpu_texture dst, u32 mip, u32 layer,
                                              de_gpu_buffer src, u64 src_off, u32 row_pitch);
/**
 * Copies mip `mip` of layer `layer` of `src` (every slice of a 3D mip) into `dst` at `dst_off`,
 * rows `row_pitch` bytes apart: the reverse of de_gpu_cmd_copy_buffer_to_texture, with the same
 * layout rules (row_pitch a multiple of 256 and at least one row of texels, or of 4x4 blocks for
 * BC formats; dst_off a multiple of 512). `src` needs DE_USAGE_COPY_SRC and must be in
 * DE_ACCESS_COPY_SRC; `dst` needs DE_USAGE_COPY_DST. For readback (DENG-284).
 */
DE_API void de_gpu_cmd_copy_texture_to_buffer(de_gpu_cmd *c, de_gpu_buffer dst, u64 dst_off,
                                              u32 row_pitch, de_gpu_texture src, u32 mip,
                                              u32 layer);
/**
 * Opens a named debug region (S10, DENG-326) that capture tools show: VK_EXT_debug_utils labels
 * on Vulkan, PIX-compatible events on D3D12, debug groups on Metal. `color` is 0xRRGGBB (0 = the
 * tool's default; D3D12 events carry no colour). Regions nest and must be closed by
 * de_gpu_cmd_marker_pop before the list is submitted; a region opened inside a render pass
 * closes before de_gpu_cmd_end_render (checked in Debug and Dev). The name is copied.
 */
DE_API void de_gpu_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color);
/** Closes the innermost region opened by de_gpu_cmd_marker_push. */
DE_API void de_gpu_cmd_marker_pop(de_gpu_cmd *c);
/** Writes breadcrumb `id` (GPU crash diagnostics, DENG-328): see "GPU crash diagnostics" below. */
DE_API void de_gpu_cmd_breadcrumb(de_gpu_cmd *c, u32 id);
/* S10 (DENG-323): draws, index buffer, scissor, mesh dispatch. Draws and mesh dispatches need a
 * render pass and a bound pipeline of their kind. SV_VertexID and SV_InstanceID do not portably
 * include `first_vertex`, `vertex_offset` or `first_instance`: pass per-draw offsets in push
 * constants (or in the index values) instead. */
/** Draws `vertex_count` vertices of `instance_count` instances (GRAPHICS pipeline). */
DE_API void de_gpu_cmd_draw(de_gpu_cmd *c, u32 vertex_count, u32 instance_count, u32 first_vertex,
                            u32 first_instance);
/** Draws `index_count` indices from the bound index buffer (de_gpu_cmd_set_index_buffer). */
DE_API void de_gpu_cmd_draw_indexed(de_gpu_cmd *c, u32 index_count, u32 instance_count,
                                    u32 first_index, i32 vertex_offset, u32 first_instance);
/**
 * Binds the index buffer for de_gpu_cmd_draw_indexed and de_gpu_cmd_draw_indexed_indirect:
 * u16 (`index_u16`) or u32 indices from `offset` (a multiple of the index size) of a buffer with
 * DE_USAGE_INDEX. It stays bound until the list ends or another one is bound.
 */
DE_API void de_gpu_cmd_set_index_buffer(de_gpu_cmd *c, de_gpu_buffer b, u64 offset, bool index_u16);
/** Scissor rectangle in pixels from the top-left of the render area (y down), like the viewport.
 * Parts left of or above the render area are cut. */
DE_API void de_gpu_cmd_scissor(de_gpu_cmd *c, i32 x, i32 y, u32 w, u32 h);
/** Dispatches x*y*z threadgroups of a MESH pipeline: of its amplification shader when it has
 * one, else of its mesh shader. */
DE_API void de_gpu_cmd_dispatch_mesh(de_gpu_cmd *c, u32 x, u32 y, u32 z);
/**
 * Writes the GPU time into query `index` of `h` once the commands recorded before it have
 * finished (S10, DENG-326). Ticks run at de_gpu_timestamp_frequency(d, queue) per second; convert
 * a difference with de_gpu_timestamp_to_ms. Outside render passes only. Values are monotonic in
 * the order of the timestamps within one list. Per backend:
 *   - Vulkan: vkCmdWriteTimestamp2 at ALL_COMMANDS (the query is reset just before).
 *   - D3D12: EndQuery on a TIMESTAMP (copy queue: COPY_QUEUE_TIMESTAMP) heap.
 *   - Metal: Apple GPUs sample time only at encoder (stage) boundaries, so a timestamp ends the
 *     open encoder and becomes a small blit encoder of its own that samples at its start. It
 *     waits for every earlier encoder of the list and every later encoder waits for it: a
 *     timestamp is also a full barrier on Metal.
 */
DE_API void de_gpu_cmd_timestamp(de_gpu_cmd *c, de_gpu_query_heap h, u32 index);
/**
 * Copies queries [first, first + count) of `h` into `dst` at `dst_off` (a multiple of 8) as u64
 * ticks, in order. Outside render passes. `dst` needs DE_USAGE_COPY_DST and must be in
 * DE_ACCESS_COPY_DST; typically a DE_MEM_READBACK buffer read after the list's fence. A query that
 * was never written resolves to an undefined value (UINT64_MAX on Metal).
 */
DE_API void de_gpu_cmd_resolve_queries(de_gpu_cmd *c, de_gpu_query_heap h, u32 first, u32 count,
                                       de_gpu_buffer dst, u64 dst_off);

/* ---------- Submission and sync (timeline fences) ---------- */
typedef struct de_gpu_wait {
    de_gpu_fence fence;
    u64 value;
} de_gpu_wait;
/** One submission: the command lists run in order after the waits; `signal` (optional) is set to
 * `signal_value` when they complete. */
typedef struct de_gpu_submit {
    de_gpu_cmd *const *cmds;
    u32 cmd_count;
    const de_gpu_wait *waits;
    u32 wait_count;
    de_gpu_fence signal;
    u64 signal_value;
} de_gpu_submit;

/** Timeline fence starting at `initial`. Invalid handle on failure. */
DE_API de_gpu_fence de_gpu_fence_create(de_gpu_device *d, u64 initial);
DE_API void de_gpu_fence_destroy(de_gpu_device *d, de_gpu_fence f);
/** The fence's completed value; 0 for an invalid handle. */
DE_API u64 de_gpu_fence_value(de_gpu_device *d, de_gpu_fence f);
/** Waits until the fence reaches `value`. DE_ERR_TIMEOUT after `timeout_ns` (0 = poll). */
DE_API de_result de_gpu_fence_wait(de_gpu_device *d, de_gpu_fence f, u64 value, u64 timeout_ns);
/**
 * Submits command lists to queue `q` (every list must have been begun on `q`). Also releases
 * deferred destructions whose work has completed. The lists are owned by the device afterwards.
 */
DE_API de_result de_gpu_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s);

/* ---------- Swapchain ---------- */
typedef enum de_gpu_colorspace {
    DE_CS_SRGB,
    DE_CS_HDR10_PQ,
    DE_CS_SCRGB,
    DE_CS_EDR
} de_gpu_colorspace;
typedef enum de_gpu_present {
    DE_PRESENT_VSYNC,
    DE_PRESENT_MAILBOX,
    DE_PRESENT_IMMEDIATE
} de_gpu_present;
/**
 * Swapchain for a window created with the backend's surface kind (de_window_desc.surface). The
 * swapchain owns the drawable size: call de_gpu_swapchain_resize on DE_EV_WINDOW_RESIZED.
 * `image_count` 0 = backend default. The null backend has no swapchain (DE_ERR_UNSUPPORTED).
 */
typedef struct de_gpu_swapchain_desc {
    de_window window;
    u32 image_count;
    de_gpu_colorspace colorspace;
    de_gpu_present present;
    bool allow_vrr;
} de_gpu_swapchain_desc;

DE_API de_gpu_swapchain de_gpu_swapchain_create(de_gpu_device *d,
                                                const de_gpu_swapchain_desc *desc);
DE_API void de_gpu_swapchain_destroy(de_gpu_device *d, de_gpu_swapchain s);
DE_API de_result de_gpu_swapchain_acquire(de_gpu_device *d, de_gpu_swapchain s,
                                          de_gpu_texture *image);
DE_API de_result de_gpu_swapchain_present(de_gpu_device *d, de_gpu_swapchain s);
DE_API de_result de_gpu_swapchain_resize(de_gpu_device *d, de_gpu_swapchain s, u32 w, u32 h);
DE_API de_gpu_colorspace de_gpu_swapchain_colorspace(de_gpu_device *d, de_gpu_swapchain s);

/* ---------- Timestamps and GPU profiler zones (S10, DENG-326; SPEC §12.3, §18.1) ---------- */

/**
 * Timestamp ticks per second of queue `q` (de_gpu_cmd_timestamp). Usually caps.timestamp_frequency;
 * D3D12 asks each queue (its copy queue may count at another rate). 0 for a NULL device.
 */
DE_API u64 de_gpu_timestamp_frequency(de_gpu_device *d, de_gpu_queue q);
/** `ticks` (a difference of two timestamps of queue `q`) in milliseconds. 0 for a NULL device. */
DE_API f64 de_gpu_timestamp_to_ms(de_gpu_device *d, de_gpu_queue q, u64 ticks);

/**
 * One timed GPU region for de_gpu_profile_zones: `begin` and `end` index the resolved ticks.
 * Regions are listed in the order they were opened; `depth` is 0 at the top level, and a region
 * at depth n + 1 lies inside the nearest earlier region at depth n (what a stack of
 * marker_push / marker_pop scopes gives).
 */
typedef struct de_gpu_zone {
    const char *name; /**< copied */
    u32 color;        /**< 0xRRGGBB, 0 = default */
    u32 begin, end;   /**< indices into `ticks` */
    u32 depth;
} de_gpu_zone;

/**
 * Sends timed regions of queue `q` to the profiler as GPU zones (SPEC §18.1): one Tracy GPU
 * context per device and queue, made on first use. `ticks` are `tick_count` resolved timestamps
 * of `q` (de_gpu_cmd_resolve_queries, read back after the list completed). Regions with an index
 * out of range or an unwritten tick (0 or UINT64_MAX) are skipped, with their children. Does
 * nothing unless the build has DE_PROFILE and the profiler runs (de_prof_init); never refers to
 * Tracy otherwise. Thread-safe: it holds the queue's submit lock while it sends.
 */
DE_API void de_gpu_profile_zones(de_gpu_device *d, de_gpu_queue q, const de_gpu_zone *zones,
                                 u32 zone_count, const u64 *ticks, u32 tick_count);

/* ---------- Tier evaluation (DENG-290; SPEC §3.3, §12.8, §24 R5/R7) ---------- */

/** Rendering tier. NONE: the device meets neither tier (the renderer refuses it). */
typedef enum de_gpu_tier { DE_GPU_TIER_NONE, DE_GPU_TIER_BASE, DE_GPU_TIER_RT } de_gpu_tier;

/** Most reasons per tier in a de_gpu_tier_report; later ones are dropped. */
#define DE_GPU_TIER_MAX_REASONS 6u
/** Capacity of one reason string, including the NUL. */
#define DE_GPU_TIER_REASON_BYTES 160u

/** What de_gpu_evaluate_tier() decided, and why. Every string is NUL-terminated. */
typedef struct de_gpu_tier_report {
    bool base_ok;      /**< meets the Base tier (SPEC §3.3) for its backend */
    bool rt_ok;        /**< meets the RT tier: Base + inline ray queries + hardware RT */
    bool mesh_shaders; /**< mesh shaders usable (optional at both tiers; else compute expansion) */
    de_gpu_tier recommended; /**< the tier the renderer should pick by default */
    u32 base_reason_count;   /**< entries in base_reasons; 0 when base_ok */
    u32 rt_reason_count;     /**< entries in rt_reasons; 0 when rt_ok */
    char base_reasons[DE_GPU_TIER_MAX_REASONS][DE_GPU_TIER_REASON_BYTES]; /**< failed Base reqs */
    char rt_reasons[DE_GPU_TIER_MAX_REASONS][DE_GPU_TIER_REASON_BYTES];   /**< failed RT reqs */
    /** Why `recommended` is lower than the best tier met ("" when it is not). */
    char recommended_reason[DE_GPU_TIER_REASON_BYTES];
    /** The default-to-Base list entry the device matches ("Steam Deck", ...), or "". */
    char default_base_device[64];
} de_gpu_tier_report;

/**
 * Decides the tiers a device meets and the tier to use by default. Pure: reads only `caps` (and a
 * built-in table), so it runs on synthetic caps anywhere.
 *   - Base: the backend's own SPEC §3.3 check (`caps->base_tier_ok`, its reason quoted), plus
 *     what the renderer needs from every backend: non-empty bindless tables for sampled and
 *     storage textures, buffers and samplers, and DE_GPU_MAX_PUSH_CONSTANT_BYTES push constants.
 *   - RT: Base, `caps->ray_query`, and `caps->raytracing == DE_RT_HARDWARE`.
 *   - Recommended: RT when RT is met and the device is not on the default-to-Base list (Steam
 *     Deck, Apple M1/M2 below Apple9: SPEC R7, DENG-2 Q4); else BASE when Base is met; else NONE.
 * Every failed requirement gets a reason. `caps` NULL gives a NONE report with a reason.
 */
DE_API void de_gpu_evaluate_tier(const de_gpu_caps *caps, de_gpu_tier_report *out);
/** "none", "base", "rt"; "unknown" when out of range. Never NULL. */
DE_API const char *de_gpu_tier_name(de_gpu_tier t);

/* ---------- GPU crash diagnostics (S10, DENG-328; SPEC §18.5, §12.3) ----------
 *
 * de_gpu_device_desc.crash_breadcrumbs turns on everything below that costs GPU time: breadcrumb
 * writes, D3D12 DRED (auto-breadcrumbs and page-fault reporting, set before the device is made),
 * and Metal encoder execution status on every command buffer. VK_EXT_device_fault is used
 * whenever the device has it. Device-lost handling itself is always on.
 *
 * Breadcrumbs. de_gpu_cmd_breadcrumb(c, id) records a GPU write that lands once every command
 * recorded before it in the list has completed (on Metal and Vulkan it waits for that work, so
 * it is also a barrier). The render graph writes one before each pass (`id` = pass index and
 * frame parity, chosen by the caller). Outside render passes only (checked in Debug and Dev;
 * ignored inside one in every build); a no-op without crash_breadcrumbs. No allocation: each
 * queue has a ring of 2048 slots, and a list keeps up to 512 breadcrumbs per recording (more are
 * dropped). Name ids with de_gpu_breadcrumb_name so reports can show names.
 *
 * Device lost. The first call that finds the device lost (a backend error DE_ERR_DEVICE_LOST:
 * D3D12 removal, VK_ERROR_DEVICE_LOST; or, with crash_breadcrumbs, a Metal command buffer that
 * failed on the GPU) builds a report: per queue the last breadcrumb that completed and the
 * breadcrumbs submitted after it that did not, with their names, lists, queue values and the
 * debug markers (de_gpu_cmd_marker_push) open when each was recorded; then the backend's data
 * (DRED operation history and page fault, VK_EXT_device_fault addresses and vendor codes, Metal
 * errors with each encoder's label, debug groups and status). It then sets the crash annotation
 * `gpu_fault` (a one-line summary), keeps the report for the crash handler (a crash report
 * written later in the process gets `<stem>.gpu_report.txt`, and the next launch's upload gets
 * it as de_crash_report.gpu_breadcrumbs) and calls the device-lost callback once. From then on
 * submits, fence waits, swapchain calls and creates fail with DE_ERR_DEVICE_LOST and recording
 * calls do nothing, without reaching the GPU; destroys and de_gpu_device_destroy still work.
 */

/** Most bytes of a device-lost report, with the NUL. */
#define DE_GPU_DEVICE_LOST_REPORT_MAX 16384u

/** Called once when the device is lost, on the thread that found it, with the report (valid
 * during the call). */
typedef void (*de_gpu_device_lost_fn)(const char *report, void *user);

/**
 * Names breadcrumb `id` in reports (copied; at most 47 bytes are kept). Register each id once
 * (again: the new name replaces the old). Thread-safe; not for per-frame use (may allocate).
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL device or name); DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_gpu_breadcrumb_name(de_gpu_device *d, u32 id, const char *name);
/**
 * Sets the device-lost callback (NULL removes it). It is called at most once per device; when the
 * device is already lost and the callback has not run, it runs at once on this thread.
 */
DE_API void de_gpu_set_device_lost_callback(de_gpu_device *d, de_gpu_device_lost_fn fn, void *user);
/**
 * Copies the device-lost report into `buf` (NUL-terminated; at most DE_GPU_DEVICE_LOST_REPORT_MAX
 * bytes are ever needed). Asks the backend first, so a loss no call has seen yet is reported.
 * @return DE_OK; DE_ERR_NOT_FOUND (the device is not lost); DE_ERR_INVALID_ARGUMENT (NULL device
 *         or buf, cap 0); DE_ERR_OUT_OF_MEMORY (cut to cap - 1 bytes).
 */
DE_API de_result de_gpu_device_lost_report(de_gpu_device *d, char *buf, u32 cap);
/** True when the device is lost (asks the backend too; see de_gpu_device_lost_report). */
DE_API bool de_gpu_device_is_lost(de_gpu_device *d);

DE_EXTERN_C_END
#endif
