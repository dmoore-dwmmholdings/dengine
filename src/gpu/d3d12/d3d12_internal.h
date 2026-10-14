/* d3d12_internal.h — the D3D12 backend's internal types and functions (DENG-285).
 *
 * Two parts:
 *   - Caps evaluation (every platform): the D3D12 feature-support results as a plain struct and
 *     the pure function that turns them into de_gpu_caps. No Windows header, so the unit test of
 *     the evaluator runs on every build host.
 *   - The backend (Windows only): device, queue, heap and resource payloads, and the functions the
 *     backend's files share. C COM through COBJMACROS (ID3D12Device_CreateHeap(...)); the IIDs
 *     are defined once, in d3d12_device.c (initguid.h).
 *
 * Files: d3d12_device.c (factory, adapters, debug layer, device, queues, vtable),
 * d3d12_caps.c (the evaluator), d3d12_memory.c (heaps, memory classes, requirements),
 * d3d12_resources.c (buffers, textures, samplers, formats), d3d12_bindless.c (descriptor heaps).
 * DENG-286: d3d12_cmd.c (command lists, render passes, copies), d3d12_barriers.c (barrier
 * translation, portable, and recording), d3d12_sync.c (submit, fences), d3d12_swapchain.c (DXGI).
 * DENG-324: d3d12_root_signature.c, d3d12_pipeline.c (PSOs, DXIL reader), d3d12_pipeline_library.c,
 * d3d12_draw.c (draws, dispatches, indirect).
 */
#ifndef DE_GPU_D3D12_INTERNAL_H
#define DE_GPU_D3D12_INTERNAL_H
#include <dengine/de_gpu.h>

/* ---------- Bindless table sizes (one CBV_SRV_UAV heap, one sampler heap) ---------- */

/* The shader-visible CBV/SRV/UAV heap holds every resource index space (shaders index
 * ResourceDescriptorHeap with them, shaders/common/bindless.slang), one range after the other:
 * the front-end's shared numbering (vtable bindless_shared_resource_heap). 1,000,000 descriptors
 * is the largest heap every resource binding tier allows. Acceleration structures get a range in
 * S16. After the numbered ranges comes one more buffer-sized range (DENG-410): the read-only view
 * (SRV) of the buffer at index i is at slot i + DE__D3D12_BUFFER_SRV_OFFSET, for bindless.slang's
 * de_robuffer / de_bytes (its DE__D3D12_BUFFER_SRV_OFFSET must be the same number; a change to
 * either range is a cooked-shader format change). */
#define DE__D3D12_BINDLESS_TEXTURES         500000u /* slots 0 .. 499999, 0 = the null descriptor */
#define DE__D3D12_BINDLESS_STORAGE_TEXTURES 100000u /* slots 500000 .. 599999 */
#define DE__D3D12_BINDLESS_BUFFERS          200000u /* slots 600000 .. 799999: the default view */
#define DE__D3D12_BINDLESS_ACCELS           0u
#define DE__D3D12_BUFFER_SRV_OFFSET         (DE__D3D12_BINDLESS_BUFFERS + DE__D3D12_BINDLESS_ACCELS)
#define DE__D3D12_RESOURCE_HEAP_SIZE                                                               \
    (DE__D3D12_BINDLESS_TEXTURES + DE__D3D12_BINDLESS_STORAGE_TEXTURES +                           \
     DE__D3D12_BINDLESS_BUFFERS + DE__D3D12_BINDLESS_ACCELS + DE__D3D12_BINDLESS_BUFFERS)
/* D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE. */
#define DE__D3D12_BINDLESS_SAMPLERS 2048u

/* ---------- Caps evaluation (portable) ---------- */

/** What the backend learned about an adapter and its device: CheckFeatureSupport results and the
 * DXGI adapter description, with D3D12 enum values as plain numbers. */
typedef struct de__d3d12_features {
    char adapter_name[128]; /**< UTF-8 */
    u32 vendor_id, device_id;
    u64 dedicated_vram, shared_memory;
    bool software;                    /**< DXGI_ADAPTER_FLAG_SOFTWARE: WARP */
    u32 feature_level;                /**< D3D_FEATURE_LEVEL: 0xc000 = 12_0 */
    u32 shader_model;                 /**< D3D_SHADER_MODEL: 0x66 = 6.6 */
    u32 resource_binding_tier;        /**< D3D12_RESOURCE_BINDING_TIER: 1 .. 3 */
    u32 resource_heap_tier;           /**< D3D12_RESOURCE_HEAP_TIER: 1 .. 2 */
    u32 raytracing_tier;              /**< D3D12_RAYTRACING_TIER: 0, 10 = 1.0, 11 = 1.1, 12 = 1.2 */
    u32 mesh_shader_tier;             /**< D3D12_MESH_SHADER_TIER: 0, 10 = tier 1 */
    bool enhanced_barriers;           /**< OPTIONS12.EnhancedBarriersSupported */
    u32 wave_lane_min, wave_lane_max; /**< OPTIONS1 */
    bool tearing;                     /**< DXGI_FEATURE_PRESENT_ALLOW_TEARING (variable refresh) */
    bool hdr_output;                  /**< an output of the adapter is in an HDR10 color space */
    u64 timestamp_frequency;          /**< direct queue ticks per second; 0 when unknown */
} de__d3d12_features;

/* The D3D12 values the evaluator compares with (d3d12.h is not included here). */
#define DE__D3D12_SHADER_MODEL_6_5    0x65u
#define DE__D3D12_SHADER_MODEL_6_6    0x66u
#define DE__D3D12_BINDING_TIER_3      3u
#define DE__D3D12_RAYTRACING_TIER_1_0 10u
#define DE__D3D12_RAYTRACING_TIER_1_1 11u
#define DE__D3D12_RAYTRACING_TIER_1_2 12u
#define DE__D3D12_MESH_SHADER_TIER_1  10u
#define DE__D3D12_FEATURE_LEVEL_12_0  0xc000u

/**
 * Fills `caps` (every field, backend = DE_GPU_D3D12) from `f`. Pure: no D3D12 call, no global
 * state. Base tier (SPEC §3.3): shader model >= 6.6 and resource binding tier 3; otherwise
 * base_tier_ok is false and base_tier_reason lists each missing item ("shader model 6.5 (needs
 * 6.6); resource binding tier 2 (needs 3)"). Ray queries need DXR 1.1, mesh shaders tier 1 and
 * shader model 6.5. WARP reports ray tracing as DE_RT_API_ONLY (it runs, in software).
 */
void de__d3d12_caps_evaluate(const de__d3d12_features *f, de_gpu_caps *caps);

/* ---------- Barrier translation (portable, DENG-286; d3d12_barriers.c) ----------
 *
 * The de_gpu_access -> D3D12 tables as pure functions, so the unit test runs on every build host.
 * The D3D12 values are plain numbers here; d3d12_barriers.c checks each one against d3d12.h with
 * _Static_assert when it is compiled for Windows. */

/* D3D12_BARRIER_SYNC */
#define DE__D3D12_SYNC_NONE              0x0u
#define DE__D3D12_SYNC_ALL               0x1u
#define DE__D3D12_SYNC_DRAW              0x2u
#define DE__D3D12_SYNC_INDEX_INPUT       0x4u
#define DE__D3D12_SYNC_VERTEX_SHADING    0x8u
#define DE__D3D12_SYNC_PIXEL_SHADING     0x10u
#define DE__D3D12_SYNC_DEPTH_STENCIL     0x20u
#define DE__D3D12_SYNC_RENDER_TARGET     0x40u
#define DE__D3D12_SYNC_COMPUTE_SHADING   0x80u
#define DE__D3D12_SYNC_RAYTRACING        0x100u
#define DE__D3D12_SYNC_COPY              0x200u
#define DE__D3D12_SYNC_EXECUTE_INDIRECT  0x800u
#define DE__D3D12_SYNC_ALL_SHADING       0x1000u
#define DE__D3D12_SYNC_NON_PIXEL_SHADING 0x2000u
#define DE__D3D12_SYNC_BUILD_RTAS        0x800000u /* BUILD_RAYTRACING_ACCELERATION_STRUCTURE */
/* D3D12_BARRIER_ACCESS */
#define DE__D3D12_ACCESS_VERTEX_BUFFER       0x1u
#define DE__D3D12_ACCESS_CONSTANT_BUFFER     0x2u
#define DE__D3D12_ACCESS_INDEX_BUFFER        0x4u
#define DE__D3D12_ACCESS_RENDER_TARGET       0x8u
#define DE__D3D12_ACCESS_UNORDERED_ACCESS    0x10u
#define DE__D3D12_ACCESS_DEPTH_STENCIL_WRITE 0x20u
#define DE__D3D12_ACCESS_DEPTH_STENCIL_READ  0x40u
#define DE__D3D12_ACCESS_SHADER_RESOURCE     0x80u
#define DE__D3D12_ACCESS_INDIRECT_ARGUMENT   0x200u
#define DE__D3D12_ACCESS_COPY_DEST           0x400u
#define DE__D3D12_ACCESS_COPY_SOURCE         0x800u
#define DE__D3D12_ACCESS_RTAS_READ           0x4000u /* RAYTRACING_ACCELERATION_STRUCTURE_READ */
#define DE__D3D12_ACCESS_RTAS_WRITE          0x8000u
#define DE__D3D12_ACCESS_NO_ACCESS           0x80000000u
#define DE__D3D12_ACCESS_COMMON              0x0u /* every access its layout allows (DENG-423) */
/* D3D12_BARRIER_LAYOUT */
#define DE__D3D12_LAYOUT_UNDEFINED                 0xffffffffu
#define DE__D3D12_LAYOUT_COMMON                    0u /* = PRESENT */
#define DE__D3D12_LAYOUT_GENERIC_READ              1u
#define DE__D3D12_LAYOUT_RENDER_TARGET             2u
#define DE__D3D12_LAYOUT_UNORDERED_ACCESS          3u
#define DE__D3D12_LAYOUT_DEPTH_STENCIL_WRITE       4u
#define DE__D3D12_LAYOUT_DEPTH_STENCIL_READ        5u
#define DE__D3D12_LAYOUT_SHADER_RESOURCE           6u
#define DE__D3D12_LAYOUT_COPY_SOURCE               7u
#define DE__D3D12_LAYOUT_COPY_DEST                 8u
#define DE__D3D12_LAYOUT_DIRECT_QUEUE_GENERIC_READ 19u
/* D3D12_RESOURCE_STATES (legacy barriers) */
#define DE__D3D12_STATE_COMMON                     0x0u /* = PRESENT */
#define DE__D3D12_STATE_VERTEX_AND_CONSTANT_BUFFER 0x1u
#define DE__D3D12_STATE_INDEX_BUFFER               0x2u
#define DE__D3D12_STATE_RENDER_TARGET              0x4u
#define DE__D3D12_STATE_UNORDERED_ACCESS           0x8u
#define DE__D3D12_STATE_DEPTH_WRITE                0x10u
#define DE__D3D12_STATE_DEPTH_READ                 0x20u
#define DE__D3D12_STATE_NON_PIXEL_SHADER_RESOURCE  0x40u
#define DE__D3D12_STATE_PIXEL_SHADER_RESOURCE      0x80u
#define DE__D3D12_STATE_INDIRECT_ARGUMENT          0x200u
#define DE__D3D12_STATE_COPY_DEST                  0x400u
#define DE__D3D12_STATE_COPY_SOURCE                0x800u
#define DE__D3D12_STATE_RTAS                       0x400000u /* RAYTRACING_ACCELERATION_STRUCTURE */

/** One side of a barrier in enhanced-barrier terms. `layout` is for textures (UNDEFINED for
 * buffers). */
typedef struct de__d3d12_access_info {
    u32 sync, access, layout;
} de__d3d12_access_info;

/** What the translation needs to know about a texture. */
typedef struct de__d3d12_texture_shape {
    u32 mips;   /**< mip levels */
    u32 layers; /**< array layers (cube faces count); 1 for 3D textures */
    u32 planes; /**< 2 for depth-stencil formats, else 1 */
    bool rt_ds; /**< has DE_USAGE_RENDER_TARGET or DE_USAGE_DEPTH (discard on first use) */
} de__d3d12_texture_shape;

/**
 * Normalizes a de_gpu_access set for D3D12: a write access excludes every other bit (the first
 * of RENDER_TARGET, DEPTH_WRITE, SHADER_WRITE, COPY_DST, ACCEL_WRITE is kept), PRESENT combined
 * with anything else is dropped. Reads combine.
 */
u32 de__d3d12_access_normalize(u32 access);

/**
 * The enhanced-barrier sync, access and layout of `access` (de_gpu_access bits) used on queue
 * `q`, for a texture or a buffer, on the source (`before`) or destination side:
 *   - NONE before: wait for everything (SYNC_ALL), no access, layout UNDEFINED (contents
 *     discarded); NONE after: SYNC_NONE and NO_ACCESS (the caller keeps the layout).
 *   - PRESENT: layout PRESENT (COMMON), no access; SYNC_ALL before, SYNC_NONE after.
 *   - One bit: the table in d3d12_barriers.c. Several reads: their syncs and accesses, layout
 *     GENERIC_READ, or DIRECT_QUEUE_GENERIC_READ when depth reads are among them (graphics).
 *   - The queue drops what it cannot do: a copy queue keeps SYNC_ALL / COPY and copy accesses and
 *     uses layout COMMON; a compute queue has no draw, raster or depth stages (ALL_SHADING becomes
 *     COMPUTE_SHADING). An empty access is NO_ACCESS, an empty sync NONE (with NO_ACCESS).
 */
de__d3d12_access_info de__d3d12_access_info_of(de_gpu_queue q, u32 access, bool texture,
                                               bool before);

/** One de_gpu_barrier as an enhanced texture or buffer barrier. */
typedef struct de__d3d12_enhanced {
    u32 sync_before, sync_after, access_before, access_after;
    u32 layout_before, layout_after; /**< textures */
    bool discard;                    /**< D3D12_TEXTURE_BARRIER_FLAG_DISCARD */
    u32 mip_first, mip_count, layer_first, layer_count, plane_count; /**< resolved range */
} de__d3d12_enhanced;

/** Barrier halves of a queue-ownership transfer (src_queue != dst_queue). */
typedef enum de__d3d12_transfer {
    DE__D3D12_TRANSFER_NONE,    /**< same queue: an ordinary barrier */
    DE__D3D12_TRANSFER_RELEASE, /**< recorded on src_queue: ends in COMMON, no later access */
    DE__D3D12_TRANSFER_ACQUIRE, /**< recorded on dst_queue: starts from COMMON */
    DE__D3D12_TRANSFER_FOREIGN  /**< recorded on a third queue: invalid, nothing is recorded */
} de__d3d12_transfer;

/** Which half of a transfer `b` is when recorded on `cmd_queue`. */
de__d3d12_transfer de__d3d12_transfer_of(de_gpu_queue cmd_queue, const de_gpu_barrier *b);

/** The subresource range of `b` on `tex` (0 counts = to the end), clamped to the texture. */
void de__d3d12_barrier_range(const de_gpu_barrier *b, const de__d3d12_texture_shape *tex,
                             u32 *mip_first, u32 *mip_count, u32 *layer_first, u32 *layer_count);

/**
 * `b` (texture when `tex` is set, else buffer) recorded on `cmd_queue` as an enhanced barrier.
 * Returns false when nothing is recorded: NONE -> NONE, or a transfer recorded on a third queue.
 * A transfer goes through the COMMON layout, the one every queue type can use: the release half
 * ends in COMMON with SYNC_NONE / NO_ACCESS after it; the acquire half starts from COMMON with
 * SYNC_NONE / NO_ACCESS before it. NONE before on a render target or depth buffer sets `discard`
 * (graphics queue).
 */
bool de__d3d12_translate_enhanced(de_gpu_queue cmd_queue, const de_gpu_barrier *b,
                                  const de__d3d12_texture_shape *tex, de__d3d12_enhanced *out);

/**
 * The legacy D3D12_RESOURCE_STATES of `access` on queue `q`. NONE is COMMON. Reads combine
 * (SHADER_READ: NON_PIXEL | PIXEL_SHADER_RESOURCE, plus VERTEX_AND_CONSTANT_BUFFER | INDEX_BUFFER
 * for buffers). A copy queue knows only COMMON, COPY_SOURCE and COPY_DEST; a compute queue has no
 * render-target, depth, pixel-shader or index states. What the queue cannot hold is COMMON.
 */
u32 de__d3d12_legacy_state_of(de_gpu_queue q, u32 access, bool texture);

/** What a legacy barrier records for one subresource (or a buffer). */
typedef enum de__d3d12_legacy_op {
    DE__D3D12_LEGACY_NOTHING,    /**< already in the state (or not for this queue) */
    DE__D3D12_LEGACY_TRANSITION, /**< ResourceBarrier transition before -> after */
    DE__D3D12_LEGACY_UAV         /**< state stays UNORDERED_ACCESS: a UAV barrier */
} de__d3d12_legacy_op;

/**
 * `b` for one subresource whose tracked state is `current`, on `cmd_queue`. The state before is
 * the tracked one (legacy barriers must name the real state, and D3D12 decays some resources to
 * COMMON between submissions), or COMMON on the acquire half of a transfer; the state after is
 * the barrier's `after` (NONE keeps the state), or COMMON on the release half. `*before` and
 * `*after` are set in every case; `*after` is the new tracked state.
 */
de__d3d12_legacy_op de__d3d12_translate_legacy(de_gpu_queue cmd_queue, const de_gpu_barrier *b,
                                               bool texture, u32 current, u32 *before, u32 *after);

/** D3D12 subresource index: mip + layer * mips + plane * mips * layers. */
u32 de__d3d12_subresource(u32 mip, u32 layer, u32 plane, u32 mips, u32 layers);

/* ---------- Pipelines: DXIL reader, pipeline keys, library file, scissor (portable, DENG-324) ---
 *
 * d3d12_pipeline.c and d3d12_pipeline_library.c keep these parts free of Windows headers, so the
 * unit tests run them on every build host. */

/* DXIL program kinds: DXIL::ShaderKind, the high 16 bits of DxilProgramHeader.ProgramVersion. */
#define DE__DXIL_KIND_PIXEL         0u
#define DE__DXIL_KIND_VERTEX        1u
#define DE__DXIL_KIND_COMPUTE       5u
#define DE__DXIL_KIND_MESH          13u
#define DE__DXIL_KIND_AMPLIFICATION 14u
/** Constant buffers a de__d3d12_dxil_info lists (more are counted, not listed). */
#define DE__DXIL_MAX_CBUFFERS 8u

/** One constant buffer of a DXIL module's `dx.resources` metadata. */
typedef struct de__d3d12_dxil_cbuffer {
    u32 space, lower_bound; /**< register space, first `b` register */
    u32 size;               /**< bytes (the cbuffer layout's size, not rounded to 16) */
} de__d3d12_dxil_cbuffer;

/** What de__d3d12_dxil_scan read from a DXIL container. */
typedef struct de__d3d12_dxil_info {
    u32 kind;                /**< DE__DXIL_KIND_* */
    u32 shader_model;        /**< 0x66 = 6.6 (D3D_SHADER_MODEL numbering) */
    u32 threadgroup[3];      /**< compute, mesh, amplification (PSV0 v2+); 0 when unknown */
    u32 push_constant_bytes; /**< size of the constant buffer at b0 space0 (root constants), or 0 */
    u32 target_mask;         /**< pixel shaders: bit i set = SV_Target i is an output */
    u32 cbuffer_count;       /**< constant buffers in dx.resources */
    de__d3d12_dxil_cbuffer cbuffers[DE__DXIL_MAX_CBUFFERS];
    bool has_psv;     /**< a PSV0 part with runtime info v2+ (threadgroup known) */
    bool has_outputs; /**< an OSG1 or OSGN part (target_mask known) */
    bool has_bitcode; /**< the module's metadata was read (push_constant_bytes known) */
    /** What the bitcode reader saw (diagnostics for tests): module values counted, module-level
     * integer constants, metadata ids, metadata record codes seen (bit n = code n < 64), named
     * metadata names (the first ones, ';'-separated), and the !dx.resources tuple's id (~0u when
     * none). */
    struct {
        u32 globals, constants, metadata;
        u64 md_codes;
        char names[96];
        u32 resources;
        char trace[256]; /**< how the !dx.resources walk went */
    } bc;
} de__d3d12_dxil_info;

/**
 * Reads a DXIL container (the bytes a pipeline gets for one stage): the DXBC container and its
 * parts, the DXIL program header (kind, shader model), PSV0 (threadgroup size), OSG1/OSGN (pixel
 * outputs) and the LLVM bitcode of the module, of which only the `dx.resources` constant buffers
 * are read (a small bitstream reader, no LLVM). DE_ERR_PARSE with the reason in `why` when the
 * bytes are not a well-formed DXIL container; DE_ERR_OUT_OF_MEMORY when `a` fails.
 */
de_result de__d3d12_dxil_scan(const void *code, u64 size, const de_allocator *a,
                              de__d3d12_dxil_info *out, char *why, size_t why_cap);
/** "pixel", "vertex", "compute", "mesh", "amplification", or "kind <n>". Never NULL. */
const char *de__d3d12_dxil_kind_name(u32 kind);

/** A pipeline's key in the pipeline library: a hash of everything that goes into its D3D12 state
 * stream (the stage blobs' bytes, formats, blend, cull, depth). Equal descs give equal keys; the
 * name and the threadgroup sizes are not part of it. */
u64 de__d3d12_pipeline_key(const de_gpu_pipeline_desc *desc);

/** A scissor rectangle as D3D12_RECT holds it. */
typedef struct de__d3d12_rect {
    i32 left, top, right, bottom;
} de__d3d12_rect;
/** D3D12_VIEWPORT_BOUNDS_MAX: scissor edges stay inside 0 .. this. */
#define DE__D3D12_SCISSOR_MAX 32767
/** de_gpu_cmd_scissor's rectangle cut to 0 .. DE__D3D12_SCISSOR_MAX (left <= right, top <=
 * bottom). */
de__d3d12_rect de__d3d12_scissor_rect(i32 x, i32 y, u32 w, u32 h);

/* The pipeline library file (d3d12_pipeline_library.c): this header, `key_count` u64 keys (the
 * pipelines stored in the library, de__d3d12_pipeline_key), then the bytes of
 * ID3D12PipelineLibrary::Serialize. */
#define DE__D3D12_PIPELINE_LIBRARY_MAGIC   0x4C504444u /* "DDPL" */
#define DE__D3D12_PIPELINE_LIBRARY_VERSION 1u
typedef struct de__d3d12_pipeline_library_file {
    u32 magic, version;
    u32 vendor_id, device_id;
    u64 driver_version; /**< the adapter's user-mode driver version (4 x u16, high first) */
    u32 key_count;
    u32 reserved;  /**< 0 */
    u64 data_size; /**< library bytes after the keys */
    u64 data_hash; /**< de_hash64 of the keys and the library bytes, seed MAGIC */
} de__d3d12_pipeline_library_file;
DE_STATIC_ASSERT(sizeof(de__d3d12_pipeline_library_file) == 48, "pipeline library header");

/** What de__d3d12_pipeline_library_check found in a file. */
typedef struct de__d3d12_pipeline_library_view {
    const u8 *keys; /**< key_count little-endian u64 (maybe unaligned) */
    u32 key_count;
    const u8 *data; /**< the serialized library */
    u64 data_size;
} de__d3d12_pipeline_library_view;

/**
 * Checks a pipeline library file for this adapter: size, magic and version, the checksum, the
 * vendor and device IDs and the driver version. True with `out` set when it may go to
 * CreatePipelineLibrary; false with the reason in `why` (a file made by another driver version
 * says "driver").
 */
bool de__d3d12_pipeline_library_check(const void *file, u64 size, u32 vendor_id, u32 device_id,
                                      u64 driver_version, de__d3d12_pipeline_library_view *out,
                                      char *why, size_t why_cap);
/** `<dir>/d3d12-<vendor>-<device>-<driver a.b.c.d>.bin` (hex IDs). False when it does not fit. */
bool de__d3d12_pipeline_library_path(char *out, size_t cap, const char *dir, u32 vendor_id,
                                     u32 device_id, u64 driver_version);

/* ---------- The backend (Windows) ---------- */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "gpu/gpu_backend.h"
#define DE_D3D12_AGILITY_NO_EXPORTS
#include "gpu/d3d12/d3d12_agility_exports.h"
#undef DE_D3D12_AGILITY_NO_EXPORTS

/* The include order must give the Agility d3d12.h (third_party/d3d12_agility), not the Windows
 * SDK's older copy, and the headers must match the pinned runtime DLLs. */
_Static_assert(D3D12_SDK_VERSION == DE_D3D12_AGILITY_SDK_VERSION,
               "d3d12.h is not the Agility SDK header pinned in third_party/vendor.toml");

/** A shader-visible descriptor heap with a CPU-only mirror of the same size. Descriptors are
 * created in the mirror and copied to the shader-visible heap at the same slot: the mirror is the
 * readable copy (shader-visible heaps are write-combined memory) and the CPU handle that
 * ClearUnorderedAccessView* needs (DENG-286). */
typedef struct d3d12_desc_heap {
    ID3D12DescriptorHeap *visible;
    ID3D12DescriptorHeap *mirror;
    D3D12_CPU_DESCRIPTOR_HANDLE visible_cpu, mirror_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE visible_gpu;
    u32 increment;
    u32 count;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
} d3d12_desc_heap;

/** One hardware queue and its internal timeline fence (the front-end's queue values). */
typedef struct d3d12_queue {
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    de_atomic_u64 signaled; /**< highest value signaled on the queue */
} d3d12_queue;

/** Device payload (de__gpu_device_payload). */
typedef struct d3d12_device {
    IDXGIFactory4 *factory;
    IDXGIAdapter1 *adapter;
    ID3D12Device *dev;
    ID3D12Device10 *dev10; /**< enhanced barriers, CreatePlacedResource2; NULL on older runtimes */
    ID3D12InfoQueue *info; /**< debug layer messages, or NULL */
    ID3D12InfoQueue1 *info1;
    DWORD info_cookie;  /**< RegisterMessageCallback cookie (info1); 0 = no callback */
    de_mutex info_lock; /**< draining stored messages (without a callback) */
    de_atomic_u32 debug_errors, debug_warnings;
    de_log_category log;
    d3d12_queue queues[DE_QUEUE_COUNT_];
    d3d12_desc_heap resources; /**< CBV_SRV_UAV, DE__D3D12_RESOURCE_HEAP_SIZE */
    d3d12_desc_heap samplers;  /**< SAMPLER, DE__D3D12_BINDLESS_SAMPLERS */
    u32 class_textures;        /**< memory class of non-RT/DS textures */
    u32 class_rt_ds;           /**< memory class of render-target and depth textures */
    de__d3d12_features features;
    /* DENG-286: command lists and barriers. */
    de_atomic_u32 barrier_mode; /**< DE__D3D12_BARRIERS_*; chosen by the first cmd_init */
    de_atomic_u64 cmd_serial;   /**< numbers command-list recordings (legacy state tracking) */
    /* DENG-324: root signature, command signatures, pipeline library (d3d12_pipeline.c). */
    struct d3d12_pipe_state *pipe;
    /* DENG-328 (d3d12_dred.c): DRED was switched on before the device was made. */
    bool dred;
} d3d12_device;

typedef struct d3d12_buffer {
    ID3D12Resource *res;
    de__gpu_alloc alloc; /**< zero for a placed transient buffer (the region owns the memory) */
    /* DENG-286, legacy barriers: the state set by the recording numbered `legacy_serial`. Buffers
     * decay to COMMON at the end of every ExecuteCommandLists, so another serial means COMMON. */
    u32 legacy_state;
    u64 legacy_serial;
} d3d12_buffer;

typedef struct d3d12_texture {
    ID3D12Resource *res;
    de__gpu_alloc alloc;
    /* DENG-286, legacy barriers: the tracked state of each subresource (index mip + layer * mips;
     * every plane shares it). Inline when mips * layers <= DE_GPU_MAX_MIPS, else `legacy_sub`
     * (allocated on the first legacy barrier, freed by texture_destroy). A texture last
     * transitioned on the copy queue (`legacy_copy_serial`, another recording) decayed to COMMON.
     */
    u32 legacy_inline[DE_GPU_MAX_MIPS];
    u32 *legacy_sub;
    u64 legacy_copy_serial;
} d3d12_texture;

typedef struct d3d12_heap {
    ID3D12Heap *heap;
} d3d12_heap;

static inline d3d12_device *de__d3d12(de_gpu_device *d) {
    return (d3d12_device *)de__gpu_device_payload(d);
}

/* d3d12_device.c */
/** DE_FAIL with the HRESULT mapped to a de_result (E_OUTOFMEMORY -> DE_ERR_OUT_OF_MEMORY, device
 * removed -> DE_ERR_DEVICE_LOST, ...) and appended to the message as hex. */
de_result de__d3d12_fail(HRESULT hr, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
/** Sets the object's debug name (UTF-8 -> UTF-16). Empty names are skipped. */
void de__d3d12_set_name(ID3D12Object *obj, const char *name);
/** Logs and counts the debug layer messages stored since the last call (when there is no message
 * callback). */
void de__d3d12_drain_messages(d3d12_device *dd);

/* d3d12_caps.c: see de__d3d12_caps_evaluate above. */

/* d3d12_memory.c */
/** Registers the memory classes for the device's resource heap tier (device_create). */
de_result de__d3d12_memory_init(de_gpu_device *d);
de_result de__d3d12_heap_create(de_gpu_device *d, de__gpu_heap *h,
                                const de__gpu_mem_class_desc *cls);
void de__d3d12_heap_destroy(de_gpu_device *d, de__gpu_heap *h);
de_result de__d3d12_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                        de__gpu_mem_req *out);
de_result de__d3d12_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                         de__gpu_mem_req *out);
/** The D3D12 resource desc of a buffer / texture (desc validated, texture defaults applied). */
void de__d3d12_buffer_resource_desc(const de_gpu_buffer_desc *desc, D3D12_RESOURCE_DESC *out);
void de__d3d12_texture_resource_desc(const de_gpu_texture_desc *desc, D3D12_RESOURCE_DESC *out);
/** The memory class of a texture with this resource desc (heap tier 1 keeps render targets and
 * depth buffers in their own heaps). */
u32 de__d3d12_texture_class(de_gpu_device *d, const D3D12_RESOURCE_DESC *rd);

/* d3d12_resources.c */
de_result de__d3d12_buffer_create(de_gpu_device *d, de__gpu_buffer *b,
                                  const de_gpu_buffer_desc *desc);
de_result de__d3d12_buffer_create_placed(de_gpu_device *d, de__gpu_buffer *b,
                                         const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                         u64 offset);
void de__d3d12_buffer_destroy(de_gpu_device *d, de__gpu_buffer *b);
de_result de__d3d12_texture_create(de_gpu_device *d, de__gpu_texture *t,
                                   const de_gpu_texture_desc *desc);
de_result de__d3d12_texture_create_placed(de_gpu_device *d, de__gpu_texture *t,
                                          const de_gpu_texture_desc *desc, de__gpu_heap *heap,
                                          u64 offset);
void de__d3d12_texture_destroy(de_gpu_device *d, de__gpu_texture *t);
de_result de__d3d12_sampler_create(de_gpu_device *d, de__gpu_sampler *s,
                                   const de_gpu_sampler_desc *desc);
/** DXGI format of a texture's resource (typeless for sampled depth), of its SRV, of its UAV. */
DXGI_FORMAT de__d3d12_format_resource(de_gpu_format f, u32 usage);
DXGI_FORMAT de__d3d12_format_srv(de_gpu_format f);
DXGI_FORMAT de__d3d12_format_uav(de_gpu_format f);

/* d3d12_bindless.c */
/** Creates both descriptor heaps and writes the null descriptors at slot 0. */
de_result de__d3d12_bindless_init(de_gpu_device *d);
void de__d3d12_bindless_destroy(de_gpu_device *d);
void de__d3d12_bindless_write_buffer(de_gpu_device *d, u32 index, de__gpu_buffer *b);
void de__d3d12_bindless_write_texture(de_gpu_device *d, u32 index, de__gpu_texture *t, u32 mip,
                                      bool storage);
void de__d3d12_bindless_write_sampler(de_gpu_device *d, u32 index, de__gpu_sampler *s);
/** The sampler desc of a de_gpu_sampler_desc. */
void de__d3d12_sampler_desc(const de_gpu_sampler_desc *s, D3D12_SAMPLER_DESC *out);
/** CPU handle of slot `index` of the mirror (`visible` false) or the shader-visible heap. */
D3D12_CPU_DESCRIPTOR_HANDLE de__d3d12_desc_cpu(const d3d12_desc_heap *h, u32 index, bool visible);

/* ---------- Test hooks (tests/unit/gpu/d3d12/) ---------- */

/** Debug layer messages of severity ERROR or CORRUPTION (resp. WARNING) seen on a D3D12 device
 * since it was created; 0 for other devices or without the debug layer. Drains stored messages
 * first. */
u32 de__d3d12_debug_error_count(de_gpu_device *d);
u32 de__d3d12_debug_warning_count(de_gpu_device *d);
/** True when `d` is a D3D12 device whose debug layer is on. */
bool de__d3d12_debug_layer_active(de_gpu_device *d);
/** The device's ID3D12Device (borrowed), or NULL for other devices. */
ID3D12Device *de__d3d12_native_device(de_gpu_device *d);
/** The resource of a buffer / texture of a D3D12 device (borrowed), or NULL. */
ID3D12Resource *de__d3d12_buffer_resource(de_gpu_device *d, de_gpu_buffer b);
ID3D12Resource *de__d3d12_texture_resource(de_gpu_device *d, de_gpu_texture t);
/**
 * Copies the bytes of descriptor slot `index` of the resource heap (`sampler_heap` false) or the
 * sampler heap, from the mirror (`visible` false) or the shader-visible heap, into `out` (at most
 * `cap` bytes). Returns the descriptor size, 0 when out of range or not a D3D12 device.
 */
u32 de__d3d12_descriptor_bytes(de_gpu_device *d, bool sampler_heap, bool visible, u32 index,
                               void *out, u32 cap);
/** UTF-8 path of the D3D12Core.dll the process loaded ("" before the first device): the Agility
 * runtime in <exe dir>\D3D12\, or the OS copy in System32. */
void de__d3d12_runtime_path(char *out, size_t cap);

/* ---------- Command lists, barriers, sync, swapchains (DENG-286) ---------- */

/** d3d12_device.barrier_mode. */
enum { DE__D3D12_BARRIERS_UNSET, DE__D3D12_BARRIERS_ENHANCED, DE__D3D12_BARRIERS_LEGACY };

/** Command-list payload (de__gpu_cmd_payload): one allocator and one list per front-end command
 * list object, which the front-end reuses only after its submission completed. */
typedef struct d3d12_cmd {
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12GraphicsCommandList7 *list7; /**< for Barrier(); NULL with legacy barriers */
    ID3D12DescriptorHeap *rtv_heap;    /**< graphics lists: begin_render's color views */
    ID3D12DescriptorHeap *dsv_heap;    /**< graphics lists: begin_render's depth view */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_start, dsv_start;
    u32 rtv_increment;
    u64 serial;    /**< this recording's number (d3d12_device.cmd_serial) */
    bool enhanced; /**< the device records enhanced barriers */
    bool open;     /**< recording: Close() before ExecuteCommandLists */
    /* DENG-324 (d3d12_draw.c): the recording (`serial`) that last set the graphics [0] and compute
     * [1] root signature; the bound pipeline's kind; the list as ID3D12GraphicsCommandList6
     * (DispatchMesh), queried on first use and not counted (the list holds the reference). */
    u64 root_serial[2];
    de_gpu_pipeline_kind bound_kind;
    ID3D12GraphicsCommandList6 *list6;
    /* DENG-328 (d3d12_dred.c): the list as ID3D12GraphicsCommandList2 (WriteBufferImmediate),
     * queried on the first breadcrumb and not counted, like list6. */
    ID3D12GraphicsCommandList2 *list2;
} d3d12_cmd;

/** User fence payload: a timeline ID3D12Fence. */
typedef struct d3d12_fence {
    ID3D12Fence *fence;
} d3d12_fence;

/** DXGI_MAX_SWAP_CHAIN_BUFFERS. */
#define DE__D3D12_SWAPCHAIN_MAX_IMAGES 16u

/** Swapchain payload. */
typedef struct d3d12_swapchain {
    IDXGISwapChain4 *sc;
    HANDLE waitable; /**< frame-latency waitable object */
    HWND hwnd;
    u32 image_count, width, height;
    UINT flags;   /**< DXGI_SWAP_CHAIN_FLAG_* it was created with */
    bool tearing; /**< ALLOW_TEARING is in `flags` */
    bool waited;  /**< acquire waited on `waitable` since the last present */
    de_gpu_texture images[DE__D3D12_SWAPCHAIN_MAX_IMAGES];
} d3d12_swapchain;

static inline d3d12_cmd *de__d3d12_cmd(de_gpu_cmd *c) {
    return (d3d12_cmd *)de__gpu_cmd_payload(c);
}

/* d3d12_cmd.c */
de_result de__d3d12_cmd_init(de_gpu_device *d, de_gpu_cmd *c);
void de__d3d12_cmd_fini(de_gpu_device *d, de_gpu_cmd *c);
de_result de__d3d12_cmd_begin(de_gpu_cmd *c, const char *name);
void de__d3d12_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                                const de_gpu_attachment *depth);
void de__d3d12_cmd_end_render(de_gpu_cmd *c);
void de__d3d12_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h);
void de__d3d12_cmd_copy_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off, de__gpu_buffer *src,
                               u64 src_off, u64 bytes);
void de__d3d12_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de__gpu_texture *dst, u32 mip, u32 layer,
                                          de__gpu_buffer *src, u64 src_off, u32 row_pitch);
/** Texture -> buffer, the reverse of the above (vtable entry cmd_copy_texture_to_buffer, DENG-284's
 * de_gpu_cmd_copy_texture_to_buffer). */
void de__d3d12_cmd_copy_texture_to_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off,
                                          u32 row_pitch, de__gpu_texture *src, u32 mip, u32 layer);
/** Closes an open list before ExecuteCommandLists (queue_submit). */
de_result de__d3d12_cmd_close(de_gpu_cmd *c);

/* d3d12_barriers.c */
/** Enhanced or legacy barriers for this device, chosen on the first call (see the file). */
bool de__d3d12_use_enhanced_barriers(de_gpu_device *d);
void de__d3d12_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count);
de__d3d12_texture_shape de__d3d12_texture_shape_of(const de__gpu_texture *t);
/** Frees a texture's legacy state array (texture_destroy). */
void de__d3d12_texture_legacy_release(de_gpu_device *d, de__gpu_texture *t);

/* d3d12_sync.c */
de_result de__d3d12_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                                 u64 queue_value);
de_result de__d3d12_fence_create(de_gpu_device *d, de__gpu_fence *f, u64 initial);
void de__d3d12_fence_destroy(de_gpu_device *d, de__gpu_fence *f);
u64 de__d3d12_fence_value(de_gpu_device *d, de__gpu_fence *f);
de_result de__d3d12_fence_wait(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout_ns);

/* d3d12_swapchain.c */
de_result de__d3d12_swapchain_create(de_gpu_device *d, de__gpu_swapchain *s,
                                     const de_gpu_swapchain_desc *desc);
void de__d3d12_swapchain_destroy(de_gpu_device *d, de__gpu_swapchain *s);
de_result de__d3d12_swapchain_acquire(de_gpu_device *d, de__gpu_swapchain *s,
                                      de_gpu_texture *image);
de_result de__d3d12_swapchain_present(de_gpu_device *d, de__gpu_swapchain *s);
de_result de__d3d12_swapchain_resize(de_gpu_device *d, de__gpu_swapchain *s, u32 w, u32 h);
de_gpu_colorspace de__d3d12_swapchain_colorspace(de_gpu_device *d, de__gpu_swapchain *s);

/** Test hook: true when `d` is a D3D12 device that records enhanced barriers (decided by its first
 * command list; false before that, for other devices and with legacy barriers). */
bool de__d3d12_enhanced_barriers_active(de_gpu_device *d);

/* ---------- Pipelines, root signature, pipeline library, draws (DENG-324) ----------
 *
 * d3d12_root_signature.c (the global root signature), d3d12_pipeline.c (PSOs from state streams,
 * the DXIL checks, device-level setup), d3d12_pipeline_library.c (ID3D12PipelineLibrary1 on disk),
 * d3d12_draw.c (pipeline binding, root constants, draws, dispatches, ExecuteIndirect). */

/** d3d12_pipe_state.library_state. */
enum {
    DE__D3D12_LIBRARY_OFF,     /**< no library: pipeline_cache_dir "" or no driver support */
    DE__D3D12_LIBRARY_EMPTY,   /**< no file yet: an empty library */
    DE__D3D12_LIBRARY_LOADED,  /**< started from the file */
    DE__D3D12_LIBRARY_REJECTED /**< the file was refused (warning logged): an empty library */
};

/** Device-level pipeline state (d3d12_device.pipe), made by de__d3d12_pipelines_init. */
typedef struct d3d12_pipe_state {
    ID3D12Device1 *dev1;       /**< CreatePipelineLibrary */
    ID3D12Device2 *dev2;       /**< CreatePipelineState (streams) */
    ID3D12RootSignature *root; /**< the global root signature; NULL when unsupported */
    char root_why[192];        /**< why there is no root signature */
    ID3D12CommandSignature *sig_draw_indexed, *sig_dispatch, *sig_dispatch_mesh;
    /* Pipeline library (d3d12_pipeline_library.c). */
    ID3D12PipelineLibrary1 *library;
    de_mutex lock; /**< library calls and `keys` */
    u32 library_state;
    char path[512]; /**< the library file, "" without one */
    void *file;     /**< the loaded file: CreatePipelineLibrary needs it while the library lives */
    u64 file_size;
    u64 *keys; /**< pipelines in the library, sorted */
    u32 key_count, key_cap;
    u64 loaded_bytes;
    u64 driver_version;
    de_atomic_u32 hits, misses, stored;
} d3d12_pipe_state;

/** Pipeline payload. */
typedef struct d3d12_pipeline {
    ID3D12PipelineState *pso;
    de_gpu_pipeline_kind kind;
} d3d12_pipeline;

/* d3d12_pipeline.c */
/** Root signature, command signatures, pipeline library (`dir` as de_gpu_device_desc.
 * pipeline_cache_dir). Fails only when out of memory: a device without SM 6.6 and binding tier 3
 * gets no root signature, and its pipeline_create fails with DE_ERR_UNSUPPORTED. */
de_result de__d3d12_pipelines_init(de_gpu_device *d, const char *dir);
/** Saves the pipeline library and releases everything init made (also after a partial init). */
void de__d3d12_pipelines_shutdown(de_gpu_device *d);
de_result de__d3d12_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                    const de_gpu_pipeline_desc *desc);
void de__d3d12_pipeline_destroy(de_gpu_device *d, de__gpu_pipeline *p);

/* d3d12_root_signature.c */
/** Serializes the global root signature (version 1.1): heap-direct-indexed flags, 32 root
 * constants at b0 space0 for every stage. The caller releases `*out`. */
de_result de__d3d12_root_signature_serialize(ID3DBlob **out);
/** Makes d3d12_pipe_state.root, or leaves it NULL with root_why set. */
void de__d3d12_root_signature_init(de_gpu_device *d);

/* d3d12_pipeline_library.c */
void de__d3d12_library_init(de_gpu_device *d, const char *dir);
/** Saves the library when this device stored pipelines in it, then releases it. */
void de__d3d12_library_shutdown(de_gpu_device *d);
/** The PSO of `stream`: loaded from the library when it holds `key`, else created (and stored).
 * Without a library: CreatePipelineState. Returns the HRESULT of the failing call. */
HRESULT de__d3d12_library_pipeline(de_gpu_device *d, u64 key,
                                   const D3D12_PIPELINE_STATE_STREAM_DESC *stream,
                                   ID3D12PipelineState **out);

/* d3d12_draw.c */
void de__d3d12_command_signatures_init(de_gpu_device *d);
void de__d3d12_cmd_set_pipeline(de_gpu_cmd *c, de__gpu_pipeline *p);
void de__d3d12_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes);
void de__d3d12_cmd_draw(de_gpu_cmd *c, u32 vertex_count, u32 instance_count, u32 first_vertex,
                        u32 first_instance);
void de__d3d12_cmd_draw_indexed(de_gpu_cmd *c, u32 index_count, u32 instance_count, u32 first_index,
                                i32 vertex_offset, u32 first_instance);
void de__d3d12_cmd_set_index_buffer(de_gpu_cmd *c, de__gpu_buffer *b, u64 offset, bool index_u16);
void de__d3d12_cmd_scissor(de_gpu_cmd *c, i32 x, i32 y, u32 w, u32 h);
void de__d3d12_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z);
void de__d3d12_cmd_dispatch_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset);
void de__d3d12_cmd_draw_indexed_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                         de__gpu_buffer *count, u64 count_offset, u32 max_draws);
void de__d3d12_cmd_dispatch_mesh(de_gpu_cmd *c, u32 x, u32 y, u32 z);
void de__d3d12_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                          de__gpu_buffer *count, u64 count_offset, u32 max);

/* Test hooks (tests/unit/gpu/d3d12/test_d3d12_pipeline.c). */
/** True when `d` is a D3D12 device that can create pipelines; else false with the reason. */
bool de__d3d12_pipelines_supported(de_gpu_device *d, const char **why);
/** The pipeline library's DE__D3D12_LIBRARY_* state and counters: pipelines loaded from it
 * (`hits`), created because it lacked them (`misses`), bytes of the file it started from, and the
 * file's path. OFF for other devices. */
u32 de__d3d12_pipeline_library_state(de_gpu_device *d, u32 *hits, u32 *misses, u64 *loaded_bytes,
                                     const char **path);

/* ---------- Timestamps, markers, names (DENG-326) ----------
 * d3d12_query.c (query heaps, EndQuery, ResolveQueryData, per-queue frequency and calibration) and
 * d3d12_debug.c (PIX-compatible BeginEvent/EndEvent, SetName). */

/** Query heap payload. */
typedef struct d3d12_query_heap {
    ID3D12QueryHeap *heap;
} d3d12_query_heap;

/* d3d12_query.c */
de_result de__d3d12_query_heap_create(de_gpu_device *d, de__gpu_query_heap *h,
                                      const de_gpu_query_heap_desc *desc);
void de__d3d12_query_heap_destroy(de_gpu_device *d, de__gpu_query_heap *h);
void de__d3d12_cmd_timestamp(de_gpu_cmd *c, de__gpu_query_heap *h, u32 index);
void de__d3d12_cmd_resolve_queries(de_gpu_cmd *c, de__gpu_query_heap *h, u32 first, u32 count,
                                   de__gpu_buffer *dst, u64 dst_off);
u64 de__d3d12_queue_timestamp_frequency(de_gpu_device *d, de_gpu_queue q);
bool de__d3d12_queue_timestamp_now(de_gpu_device *d, de_gpu_queue q, u64 *ticks);

/* d3d12_debug.c */
void de__d3d12_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color);
void de__d3d12_cmd_marker_pop(de_gpu_cmd *c);
void de__d3d12_object_name(de_gpu_device *d, de__gpu_kind kind, void *rec, const char *name);
/** Test hook: the debug name (WKPDID_D3DDebugObjectNameW, as UTF-8) of the D3D12 object behind a
 * buffer, texture, pipeline, fence or query heap; false when it has none or `id` is stale. */
bool de__d3d12_object_debug_name(de_gpu_device *d, de__gpu_kind kind, u32 id, char *out,
                                 size_t cap);

/* ---------- DENG-328: GPU crash diagnostics (d3d12_dred.c) ---------- */

/** Turns DRED on (auto-breadcrumbs, contexts, page faults) for devices made after it; sets
 * dd->dred. Call right before D3D12CreateDevice. */
void de__d3d12_dred_enable(d3d12_device *dd);
/** vtable cmd_breadcrumb (WriteBufferImmediate, MARKER_OUT), device_lost, device_lost_report. */
void de__d3d12_cmd_breadcrumb(de_gpu_cmd *c, de__gpu_buffer *ring, u64 offset, u32 value);
bool de__d3d12_device_lost(de_gpu_device *d);
size_t de__d3d12_device_lost_report(de_gpu_device *d, char *out, size_t cap);

#endif /* _WIN32 */
#endif
