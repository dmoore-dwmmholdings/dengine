/* mtl_internal.h — the Metal 3 backend's internal interface (DENG-287).
 *
 * Plain C: Objective-C stays inside the .m files of src/gpu/metal/. Every Metal object a payload
 * keeps is a retained `void *` (CFBridgingRetain on the way in, CFBridgingRelease on the way out),
 * because the front-end allocates payloads as raw zeroed memory that ARC cannot manage. The .m
 * files reach the object with `(__bridge id<MTLx>)ptr`.
 *
 * Files:
 *   mtl_device.m     vtable, device create/destroy, queues and their internal timelines
 *   mtl_caps.m       de_gpu_caps from the MTLDevice (Base tier: Metal 3, Apple7, AB tier 2)
 *   mtl_memory.m     MTLHeap placement heaps per memory class, the heap list
 *   mtl_resources.m  formats, memory requirements, buffers, textures (+ per-mip storage views),
 *                    samplers; placed creation
 *   mtl_bindless.m   the DeBindlessHeap argument buffer and its null descriptors
 *   mtl_cmd.m        (DENG-288) command lists: one MTLCommandBuffer, lazily switched encoders,
 *                    render passes, viewport, copies
 *   mtl_barriers.m   (DENG-288) barriers: MTLFence epochs between encoders, the stage table
 *   mtl_sync.m       (DENG-288) queue submit, queue fences, user fences (MTLSharedEvent)
 *   mtl_swapchain.m  (DENG-288) CAMetalLayer swapchain
 *   mtl_residency.m  (DENG-288) MTLResidencySet (macOS 15+) or useHeaps: per encoder (macOS 14)
 *
 * Residency (DENG-288): every resource the bindless table can name lives in a heap of the heap
 * list (de__mtl_heap_list): the class heaps and the internal heap of null descriptors. An encoder
 * that reads the table calls useHeap: for each of them and binds the argument buffer
 * (de__mtl_bindless_buffer) at buffer index 0.
 */
#ifndef DE_GPU_MTL_INTERNAL_H
#define DE_GPU_MTL_INTERNAL_H
#include "gpu/gpu_backend.h"

/* ---------- The bindless argument buffer (shaders/common/bindless.slang, DeBindlessHeap)
 * ---------- Four fixed-size arrays of 8-byte entries, in this order, in one shared MTLBuffer:
 * MTLResourceID (gpuResourceID) for sampled textures, storage textures and samplers, then MTLBuffer
 * gpuAddress values. Slang emits the struct as plain arrays (no [[id(n)]]), so with argument
 * buffers tier 2 this is its layout. The sizes are the DE_METAL_BINDLESS_* constants of
 * bindless.slang and the max_bindless_* caps; changing one is a cooked-shader format change. */
#define DE__MTL_BINDLESS_TEXTURES         65536u
#define DE__MTL_BINDLESS_STORAGE_TEXTURES 16384u
#define DE__MTL_BINDLESS_SAMPLERS         2048u
#define DE__MTL_BINDLESS_BUFFERS          65536u
#define DE__MTL_BINDLESS_ENTRY_BYTES      8u

#define DE__MTL_BINDLESS_OFFSET_TEXTURES 0u
#define DE__MTL_BINDLESS_OFFSET_STORAGE_TEXTURES                                                   \
    (DE__MTL_BINDLESS_OFFSET_TEXTURES + DE__MTL_BINDLESS_TEXTURES * DE__MTL_BINDLESS_ENTRY_BYTES)
#define DE__MTL_BINDLESS_OFFSET_SAMPLERS                                                           \
    (DE__MTL_BINDLESS_OFFSET_STORAGE_TEXTURES +                                                    \
     DE__MTL_BINDLESS_STORAGE_TEXTURES * DE__MTL_BINDLESS_ENTRY_BYTES)
#define DE__MTL_BINDLESS_OFFSET_BUFFERS                                                            \
    (DE__MTL_BINDLESS_OFFSET_SAMPLERS + DE__MTL_BINDLESS_SAMPLERS * DE__MTL_BINDLESS_ENTRY_BYTES)
#define DE__MTL_BINDLESS_BYTES                                                                     \
    (DE__MTL_BINDLESS_OFFSET_BUFFERS + DE__MTL_BINDLESS_BUFFERS * DE__MTL_BINDLESS_ENTRY_BYTES)

/** Byte size of the zeroed buffer behind bindless buffer slot 0. */
#define DE__MTL_NULL_BUFFER_BYTES 4096u

/* Block heap sizes of the CPU-visible classes (GPU_ONLY keeps the 256 MiB default): shared
 * memory is the app's own RAM on Apple silicon, so these grow in smaller steps. */
#define DE__MTL_SHARED_BLOCK_SIZE     ((u64)64 << 20)
#define DE__MTL_SHARED_DEDICATED_SIZE ((u64)32 << 20)

/* ---------- Payloads ---------- */

/** One MTLHeap (a de__gpu_heap payload, or the internal heap). */
typedef struct mtl_heap {
    void *heap; /* id<MTLHeap> */
    u32 slot;   /* index in mtl_device.heaps */
} mtl_heap;

typedef struct mtl_device {
    void *device;                    /* id<MTLDevice> */
    void *queues[DE_QUEUE_COUNT_];   /* id<MTLCommandQueue>: graphics, compute, copy */
    void *timeline[DE_QUEUE_COUNT_]; /* id<MTLSharedEvent>: the front-end's internal values */

    /* Bindless (mtl_bindless.m). */
    void *bindless;    /* id<MTLBuffer>, shared, DE__MTL_BINDLESS_BYTES */
    u8 *bindless_cpu;  /* its contents */
    mtl_heap internal; /* private heap of the null descriptors' resources */
    void *null_texture, *null_storage_texture, *null_buffer, *null_sampler;
    u64 null_ids[4]; /* their gpuResourceID / gpuAddress, by de__gpu_bindless_space */

    /* Every live MTLHeap (residency list, DENG-288), the internal heap first. */
    de_mutex heap_lock;
    de_mutex sampler_lock; /* sampler create/release (mtl_resources.m) */
    mtl_heap **heaps;
    u32 heap_count, heap_cap;

    u32 resource_options[DE__GPU_MEM_CLASS_BASE_COUNT]; /* MTLResourceOptions per base class */
    bool unified_memory;
    /* Shared-storage placement heaps exist (Apple-family GPUs). Without them (Mac2-family and
     * paravirtual devices, all below the Base tier) UPLOAD/READBACK buffers are standalone
     * MTLBuffers outside the heap manager, and shared transient regions fail. */
    bool shared_heaps;
    /* MTLSamplerAddressModeClampToBorderColor works (Apple7+); else DE_ADDR_BORDER is
     * ClampToZero (also transparent black for formats with alpha). */
    bool border_color;

    /* DENG-288: command buffers, sync and residency (mtl_sync.m, mtl_residency.m). */
    void *queue_fence[DE_QUEUE_COUNT_]; /* id<MTLFence>: chains the command buffers of a queue */
    u32 render_stages;   /* every MTLRenderStages the device has (object/mesh with Metal 3) */
    bool use_heaps;      /* encoders call useHeaps: (macOS 14, or gpu.metal.force_use_heap) */
    void *residency_set; /* id<MTLResidencySet> of every listed heap (macOS 15+), or NULL */
    u32 heap_gen;        /* bumped by each heap-list change; under heap_lock */
    u32 residency_gen;   /* heap_gen the residency set was built from; under heap_lock */
    de_mutex size_lock;  /* heapTextureSizeAndAlignWithDescriptor: (mtl_resources.m) */
    void *last_group[DE_QUEUE_COUNT_]; /* dispatch_group_t of the last submit (mtl_sync.m) */
} mtl_device;

typedef struct mtl_buffer {
    void *buffer; /* id<MTLBuffer> */
    u64 gpu_address;
    de__gpu_alloc alloc; /* zero for a placed (transient) buffer: the region owns the memory */
} mtl_buffer;

typedef struct mtl_texture {
    void *texture;                /* id<MTLTexture> */
    void *views[DE_GPU_MAX_MIPS]; /* storage view per mip; NULL when `texture` itself serves */
    de__gpu_alloc alloc;          /* zero for placed and external (swapchain) textures */
} mtl_texture;

typedef struct mtl_sampler {
    void *sampler; /* id<MTLSamplerState> */
} mtl_sampler;

static inline mtl_device *de__mtl_dev(de_gpu_device *d) {
    return (mtl_device *)de__gpu_device_payload(d);
}

/* ---------- mtl_caps.m ---------- */

/** Fills `caps` from the device (`id<MTLDevice>`); caps->backend is set by the front-end. */
void de__mtl_caps_fill(void *device, de_gpu_caps *caps);

/* ---------- mtl_memory.m ---------- */

/** Configures the base memory classes (storage mode, CPU cache mode, block sizes). */
de_result de__mtl_memory_init(de_gpu_device *d);
/** Adds / removes a heap of the residency list. */
de_result de__mtl_heap_list_add(de_gpu_device *d, mtl_heap *h);
void de__mtl_heap_list_remove(de_gpu_device *d, mtl_heap *h);
de_result de__mtl_heap_create(de_gpu_device *d, de__gpu_heap *h, const de__gpu_mem_class_desc *cls);
void de__mtl_heap_destroy(de_gpu_device *d, de__gpu_heap *h);

/* ---------- mtl_resources.m ---------- */

/** MTLPixelFormat of a format (MTLPixelFormatInvalid = 0 when there is none). */
u32 de__mtl_pixel_format(de_gpu_format f);
/** Heap size and alignment (heapBufferSizeAndAlign / heapTextureSizeAndAlign) and classes. */
de_result de__mtl_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                      de__gpu_mem_req *out);
de_result de__mtl_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                       de__gpu_mem_req *out);

de_result de__mtl_buffer_create(de_gpu_device *d, de__gpu_buffer *b,
                                const de_gpu_buffer_desc *desc);
de_result de__mtl_buffer_create_placed(de_gpu_device *d, de__gpu_buffer *b,
                                       const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                       u64 offset);
void de__mtl_buffer_destroy(de_gpu_device *d, de__gpu_buffer *b);
de_result de__mtl_texture_create(de_gpu_device *d, de__gpu_texture *t,
                                 const de_gpu_texture_desc *desc);
de_result de__mtl_texture_create_placed(de_gpu_device *d, de__gpu_texture *t,
                                        const de_gpu_texture_desc *desc, de__gpu_heap *heap,
                                        u64 offset);
void de__mtl_texture_destroy(de_gpu_device *d, de__gpu_texture *t);
de_result de__mtl_sampler_create(de_gpu_device *d, de__gpu_sampler *s,
                                 const de_gpu_sampler_desc *desc);
void de__mtl_sampler_destroy(de_gpu_device *d, de__gpu_sampler *s);

/* ---------- mtl_bindless.m ---------- */

/** Creates the argument buffer, the internal heap and the null descriptors (slot 0 of every
 * table, and every unwritten slot). Needs the queues (zeroes the null resources with a blit). */
de_result de__mtl_bindless_init(de_gpu_device *d);
void de__mtl_bindless_shutdown(de_gpu_device *d);
void de__mtl_bindless_write_buffer(de_gpu_device *d, u32 index, de__gpu_buffer *b);
void de__mtl_bindless_write_texture(de_gpu_device *d, u32 index, de__gpu_texture *t, u32 mip,
                                    bool storage);
void de__mtl_bindless_write_sampler(de_gpu_device *d, u32 index, de__gpu_sampler *s);
/** Puts the null descriptor back at `index` of `space` (resource destroy). */
void de__mtl_bindless_clear(de_gpu_device *d, de__gpu_bindless_space space, u32 index);

/* ---------- For DENG-288 and tests (opaque Metal objects, not retained) ---------- */

/** id<MTLDevice>, or NULL when `d` is not a Metal device. */
void *de__mtl_device_object(de_gpu_device *d);
/** id<MTLCommandQueue> of `q`. */
void *de__mtl_queue_object(de_gpu_device *d, de_gpu_queue q);
/** The bindless argument buffer (id<MTLBuffer>) and its CPU address. */
void *de__mtl_bindless_buffer(de_gpu_device *d, const u8 **cpu);
/** Copies up to `cap` live heaps (id<MTLHeap>) into `out`, the internal heap first; returns the
 * total count. Residency: an encoder that reads the bindless table calls useHeap: on each. */
u32 de__mtl_heap_list(de_gpu_device *d, void **out, u32 cap);
/** id<MTLBuffer> of a buffer, id<MTLTexture> of a texture (mip -1) or of the storage view of
 * `mip`, id<MTLHeap> of a resource's heap; NULL for invalid handles. */
void *de__mtl_buffer_object(de_gpu_device *d, de_gpu_buffer b);
void *de__mtl_texture_object(de_gpu_device *d, de_gpu_texture t, i32 mip);
void *de__mtl_sampler_object(de_gpu_device *d, de_gpu_sampler s);
/** The heap and offset a buffer / texture lives at (front-end allocation or placement). */
bool de__mtl_buffer_placement(de_gpu_device *d, de_gpu_buffer b, void **heap, u64 *offset);
bool de__mtl_texture_placement(de_gpu_device *d, de_gpu_texture t, void **heap, u64 *offset);

/* ---------- DENG-288: command lists, barriers, sync, swapchain, residency ----------
 *
 * Sync model (untracked heaps, no hazard tracking): a barrier ends an "epoch" of encoders. Every
 * encoder updates its own MTLFence when it ends; an encoder opened after a barrier waits for
 * every fence of the epoch before that barrier (and so, transitively, for everything earlier).
 * A command buffer's first epoch waits for the queue fence, and every command buffer ends with a
 * small blit encoder that waits for its last epoch and updates the queue fence: a command-buffer
 * boundary is a full barrier on its queue. Fences resolve in commit order, which the front-end's
 * per-queue submit lock fixes. Cross-queue order and the CPU use MTLSharedEvents (fences). */

/** Render stages (MTLRenderStages values; mtl_barriers.m checks them). */
#define DE__MTL_STAGE_VERTEX   1u
#define DE__MTL_STAGE_FRAGMENT 2u
#define DE__MTL_STAGE_OBJECT   8u
#define DE__MTL_STAGE_MESH     16u
#define DE__MTL_STAGES_ALL                                                                         \
    (DE__MTL_STAGE_VERTEX | DE__MTL_STAGE_FRAGMENT | DE__MTL_STAGE_OBJECT | DE__MTL_STAGE_MESH)

/** Encoders one epoch tracks; more are chained (the next one waits for all of them). */
#define DE__MTL_EPOCH_MAX 16u

typedef enum de__mtl_encoder_kind {
    DE__MTL_ENC_NONE,
    DE__MTL_ENC_RENDER,
    DE__MTL_ENC_COMPUTE,
    DE__MTL_ENC_BLIT
} de__mtl_encoder_kind;

typedef struct mtl_cmd {
    void *cb;          /* id<MTLCommandBuffer> being recorded (unretained references), retained */
    void *encoder;     /* id<MTLCommandEncoder> open, retained; NULL when none */
    u32 kind;          /* de__mtl_encoder_kind of `encoder` */
    bool render_ended; /* end_render seen: the render encoder stays open for its fence update */
    void *fences;      /* NSMutableArray<id<MTLFence>>, retained; reused by every recording */
    u32 fence_used;    /* fences handed out in this recording */
    u32 enc_fence;     /* fence index of the open encoder */
    u32 epoch[DE__MTL_EPOCH_MAX]; /* fences of the encoders since the last barrier */
    u32 epoch_count;
    u32 wait[DE__MTL_EPOCH_MAX]; /* fences every new encoder waits for (the epoch before) */
    u32 wait_count;
    bool wait_queue;   /* new encoders also wait for the queue fence (first epoch) */
    u32 wait_stages;   /* render stages the `wait` fences guard (barrier `after` accesses) */
    u32 update_stages; /* render stages the open render encoder's update covers */
    u32 encoders;      /* encoders opened in this recording (statistics, tests) */
} mtl_cmd;

typedef struct mtl_fence {
    void *event; /* id<MTLSharedEvent>, retained */
} mtl_fence;

typedef struct mtl_swapchain {
    void *layer;            /* CAMetalLayer, retained */
    void *drawable;         /* id<CAMetalDrawable> acquired and not presented, retained */
    de_gpu_texture image;   /* the wrapper of `drawable`'s texture, or 0 */
    u32 width, height;      /* drawableSize */
    de_gpu_present present; /* the mode in use (MAILBOX falls back to VSYNC) */
} mtl_swapchain;

/* ---------- mtl_cmd.m ---------- */

de_result de__mtl_cmd_init(de_gpu_device *d, de_gpu_cmd *c);
void de__mtl_cmd_fini(de_gpu_device *d, de_gpu_cmd *c);
de_result de__mtl_cmd_begin(de_gpu_cmd *c, const char *name);
void de__mtl_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                              const de_gpu_attachment *depth);
void de__mtl_cmd_end_render(de_gpu_cmd *c);
void de__mtl_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h);
void de__mtl_cmd_copy_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off, de__gpu_buffer *src,
                             u64 src_off, u64 bytes);
void de__mtl_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de__gpu_texture *dst, u32 mip, u32 layer,
                                        de__gpu_buffer *src, u64 src_off, u32 row_pitch);
/** Texture -> buffer (the reverse of the above): de_gpu_cmd_copy_texture_to_buffer (DENG-284). */
void de__mtl_cmd_copy_texture_to_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off,
                                        u32 row_pitch, de__gpu_texture *src, u32 mip, u32 layer);
/** The open encoder of `kind` (id<MTL*CommandEncoder>, not retained), opening it when another kind
 * is open: COMPUTE for S10's dispatches, BLIT for copies. NULL for RENDER (begin_render opens it)
 * or when the command buffer cannot make one. */
void *de__mtl_cmd_encoder(de_gpu_cmd *c, de__mtl_encoder_kind kind);

/** A copy between a buffer and mip `mip`, layer `layer` of a texture, in Metal's terms. */
typedef struct de__mtl_copy_region {
    u32 width, height, depth; /* texels of the mip (all slices of a 3D mip) */
    u32 slice;                /* destination/source slice: `layer`, 0 for 3D */
    u32 bytes_per_row;        /* row_pitch: one row of texels, or of 4x4 blocks */
    u32 bytes_per_image;      /* row_pitch * rows (texel or block rows) */
} de__mtl_copy_region;
/** Pure: the region of a buffer <-> texture copy (BC formats count rows of 4x4 blocks). */
de__mtl_copy_region de__mtl_copy_region_of(const de__gpu_texture *t, u32 mip, u32 layer,
                                           u32 row_pitch);

/* ---------- mtl_barriers.m ---------- */

/** Pure: the render stages that use `access` (de_gpu_access bits); 0 for copy, present and none. */
u32 de__mtl_access_stages(u32 access);
/** Pure: the stages a barrier set waits for (`before`) and guards (`after`), within `all`. */
void de__mtl_barrier_stages(const de_gpu_barrier *b, u32 count, u32 all, u32 *before, u32 *after);
void de__mtl_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count);
/** After an encoder is made: waits (epoch and queue fences), residency, the bindless table. */
void de__mtl_encoder_started(de_gpu_cmd *c, void *encoder, de__mtl_encoder_kind kind);
/** Updates the open encoder's fence and ends it; nothing when none is open. */
void de__mtl_encoder_end(de_gpu_cmd *c);
/** Ends the recording: the open encoder, then the tail encoder that updates the queue fence. */
void de__mtl_cmd_finish(de_gpu_cmd *c);
/** A fence of the list's pool (id<MTLFence>, not retained); NULL when out of memory. */
void *de__mtl_cmd_fence(de_gpu_cmd *c, u32 index);

/* ---------- mtl_sync.m ---------- */

/** Queue fences (each initialised by one update, so every later wait has an update before it). */
de_result de__mtl_sync_init(de_gpu_device *d);
void de__mtl_sync_shutdown(de_gpu_device *d);
/** Commits an empty command buffer on every queue and waits for it: work that is not on the
 * timeline (device-init fence updates, presents) is done before the queues go away. */
void de__mtl_sync_drain(de_gpu_device *d);
/** Releases the per-queue submit groups (device destroy, after wait_idle). */
void de__mtl_sync_release_groups(de_gpu_device *d);
de_result de__mtl_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                               u64 queue_value);
de_result de__mtl_fence_create(de_gpu_device *d, de__gpu_fence *f, u64 initial);
void de__mtl_fence_destroy(de_gpu_device *d, de__gpu_fence *f);
u64 de__mtl_fence_value(de_gpu_device *d, de__gpu_fence *f);
de_result de__mtl_fence_wait(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout_ns);

/* ---------- mtl_swapchain.m ---------- */

de_result de__mtl_swapchain_create(de_gpu_device *d, de__gpu_swapchain *s,
                                   const de_gpu_swapchain_desc *desc);
void de__mtl_swapchain_destroy(de_gpu_device *d, de__gpu_swapchain *s);
de_result de__mtl_swapchain_acquire(de_gpu_device *d, de__gpu_swapchain *s, de_gpu_texture *image);
de_result de__mtl_swapchain_present(de_gpu_device *d, de__gpu_swapchain *s);
de_result de__mtl_swapchain_resize(de_gpu_device *d, de__gpu_swapchain *s, u32 w, u32 h);
de_gpu_colorspace de__mtl_swapchain_colorspace(de_gpu_device *d, de__gpu_swapchain *s);

/* ---------- mtl_residency.m ---------- */

/** Picks the residency path: an MTLResidencySet on macOS 15+, useHeaps: per encoder on macOS 14
 * or when the cvar gpu.metal.force_use_heap is true at device creation. */
de_result de__mtl_residency_init(de_gpu_device *d);
void de__mtl_residency_shutdown(de_gpu_device *d);
/** Brings the residency set up to date with the heap list (before each commit). */
void de__mtl_residency_sync(de_gpu_device *d);
/** Residency and the bindless table (buffer index 0) for a new render or compute encoder. */
void de__mtl_residency_encoder(de_gpu_device *d, void *encoder, de__mtl_encoder_kind kind);
/** True when the device uses useHeaps: per encoder, false with a residency set (tests). */
bool de__mtl_residency_uses_heaps(de_gpu_device *d);
/** True when this build has the residency-set path (built with the macOS 15 SDK or later). */
bool de__mtl_residency_sets_built(void);

#endif
