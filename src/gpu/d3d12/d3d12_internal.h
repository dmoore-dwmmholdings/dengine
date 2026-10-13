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
 */
#ifndef DE_GPU_D3D12_INTERNAL_H
#define DE_GPU_D3D12_INTERNAL_H
#include <dengine/de_gpu.h>

/* ---------- Bindless table sizes (one CBV_SRV_UAV heap, one sampler heap) ---------- */

/* The shader-visible CBV/SRV/UAV heap holds every resource index space (shaders index
 * ResourceDescriptorHeap with them, shaders/common/bindless.slang), one range after the other:
 * the front-end's shared numbering (vtable bindless_shared_resource_heap). 1,000,000 descriptors
 * is the largest heap every resource binding tier allows. Acceleration structures get a range in
 * S16. */
#define DE__D3D12_BINDLESS_TEXTURES         500000u /* slots 0 .. 499999, 0 = the null descriptor */
#define DE__D3D12_BINDLESS_STORAGE_TEXTURES 100000u /* slots 500000 .. 599999 */
#define DE__D3D12_BINDLESS_BUFFERS          400000u /* slots 600000 .. 999999 */
#define DE__D3D12_BINDLESS_ACCELS           0u
#define DE__D3D12_RESOURCE_HEAP_SIZE                                                               \
    (DE__D3D12_BINDLESS_TEXTURES + DE__D3D12_BINDLESS_STORAGE_TEXTURES +                           \
     DE__D3D12_BINDLESS_BUFFERS + DE__D3D12_BINDLESS_ACCELS)
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
} d3d12_device;

typedef struct d3d12_buffer {
    ID3D12Resource *res;
    de__gpu_alloc alloc; /**< zero for a placed transient buffer (the region owns the memory) */
} d3d12_buffer;

typedef struct d3d12_texture {
    ID3D12Resource *res;
    de__gpu_alloc alloc;
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

#endif /* _WIN32 */
#endif
