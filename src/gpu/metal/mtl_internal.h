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
 *   mtl_pipeline.m   (DENG-325) render, mesh and compute pipeline states, the library cache
 *   mtl_draw.m       (DENG-325) the per-list draw state: pipelines, push constants, viewport,
 *                    scissor, draws, dispatches, mesh dispatches
 *   mtl_icb.m        (DENG-325) indexed indirect draws; with a count buffer through an indirect
 *                    command buffer that a compute kernel encodes
 *   mtl_binary_archive.m (DENG-325) the MTLBinaryArchive pipeline cache
 *   mtl_errors.m     (DENG-328) command-buffer errors and encoder status, breadcrumbs
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

    /* DENG-325: pipelines (mtl_pipeline.m), the indirect-count kernel (mtl_icb.m) and the binary
     * archive (mtl_binary_archive.m). */
    de_mutex library_lock; /* the library cache and the ICB kernel */
    void *libraries;       /* NSMutableDictionary: blob hash -> id<MTLLibrary>, retained */
    void *icb_kernel; /* id<MTLComputePipelineState> of the ICB encoder (first use), retained */
    de_mutex archive_lock;   /* the archive's additions and counters */
    void *archive;           /* NSArray of one id<MTLBinaryArchive>, retained; NULL without one */
    u32 archive_state;       /* DE__MTL_ARCHIVE_* */
    u32 archive_hits;        /* pipelines found in the archive */
    u32 archive_misses;      /* pipelines compiled (and added) */
    bool archive_dirty;      /* pipelines added since it was loaded */
    u64 pipeline_ns;         /* time spent in pipeline creation (logged at destroy) */
    char archive_vpath[256]; /* virtual path of the archive file ("" without one) */
    char archive_path[1024]; /* its native path */

    /* DENG-407: the texture objects of the storage table that residency names with write usage
     * (mtl_bindless.m, mtl_residency.m), under heap_lock: every one on the useHeaps: path (it
     * names heap resources for reading only), only the per-mip views with a residency set (a set
     * of heaps does not cover views). Metal shader validation drops a shader write to a texture
     * the encoder does not name with write usage. Render and compute encoders pass the list to
     * useResources: with read-write usage. */
    void **storage_objs; /* id<MTLTexture>, not retained (the texture payload owns it); dense */
    u32 *storage_slots;  /* the storage-table slot of each storage_objs entry */
    u32 *storage_pos;    /* storage-table slot -> index in storage_objs + 1; 0 = none */
    u32 storage_count;

    /* DENG-328 (mtl_errors.m): crash diagnostics, with de_gpu_device_desc.crash_breadcrumbs. */
    bool crash_diag;                  /* command buffers report encoder execution status */
    void *fault_log;                  /* DEMtlFaultLog, retained; NULL when off */
    void *bc_values[DE_QUEUE_COUNT_]; /* id<MTLBuffer>: breadcrumb values to copy, retained */
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

/* DENG-325 (mtl_draw.m, mtl_icb.m): the state a command list applies to each new encoder. */
typedef struct mtl_draw_state {
    void *pipeline;    /* bound id<MTL*PipelineState>, not retained (deferred destruction) */
    void *depth_state; /* its id<MTLDepthStencilState>, not retained; NULL for compute */
    u32 kind;          /* de_gpu_pipeline_kind of `pipeline` */
    u32 cull;          /* MTLCullMode */
    u32 push_stages;   /* DE__MTL_PUSH_* stages that get the push constants */
    u32 push_bytes;    /* bytes setBytes passes at index 1 (0: none) */
    u32 threadgroup[3], object_threadgroup[3];
    u32 applied;        /* `encoders` count of the encoder the state was applied to; 0 = none */
    u32 dirty;          /* DE__MTL_DIRTY_* not yet applied to that encoder */
    void *index_buffer; /* id<MTLBuffer> of set_index_buffer, not retained */
    u64 index_offset, index_address;
    bool index_u16, has_viewport, has_scissor, has_depth;
    f32 viewport[4];
    i32 scissor_xy[2];
    u32 scissor_wh[2];
    u32 area[2];     /* width and height of the open render pass */
    u32 color_count; /* the open render pass, kept for a split (mtl_icb.m) */
    de_gpu_attachment colors[DE_GPU_MAX_COLOR_ATTACHMENTS], depth;
    /* Indirect-count draws (mtl_icb.m): one ICB and one range buffer per list, reused. */
    void *icb;     /* id<MTLIndirectCommandBuffer>, retained */
    u32 icb_cap;   /* commands */
    u32 icb_used;  /* commands encoded in this recording */
    void *ranges;  /* id<MTLBuffer> of MTLIndirectCommandBufferExecutionRange, retained */
    u32 range_cap; /* entries */
    u32 range_used;
    void *retired; /* NSMutableArray of ICBs and range buffers outgrown in this recording */
    u32 splits;    /* render passes split for an indirect-count draw in this recording */
    u8 push[DE_GPU_MAX_PUSH_CONSTANT_BYTES];
} mtl_draw_state;

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
    /* DENG-325: attachments whose store action is decided when the render encoder ends
     * (MTLStoreActionUnknown at begin_render): bit i = color i, DE__MTL_STORE_DEPTH, _STENCIL. */
    u32 deferred_store;
    mtl_draw_state draw; /* DENG-325: pipeline and draw state (mtl_draw.m) */
    /* DENG-326 (mtl_debug.m): per open marker level, the `encoders` count of the render encoder
     * its debug group was pushed on, or 0 when it is on the command buffer. Written at push,
     * read at pop of the same recording (the front-end's marker_depth indexes it). */
    u32 marker_encoder[DE__GPU_MARKER_LEVELS];
    /* DENG-326 (mtl_query.m): resolves wait for the CPU through this event (made on the list's
     * first resolve, reused by its later recordings; released by cmd_fini). */
    void *resolve_event;    /* id<MTLSharedEvent>, retained */
    void *resolve_listener; /* MTLSharedEventListener, retained */
    u64 resolve_value;      /* the last value used on resolve_event */
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

/* ---------- DENG-325: pipelines, draws, indirect command buffers, binary archives ----------
 *
 * Binding (shaders/common/bindless.slang, push_constants.slang): the bindless argument buffer at
 * buffer index 0 (mtl_residency.m binds it on every new encoder) and the push constants with
 * setBytes at buffer index 1 of every stage the pipeline has. A command list keeps the bound
 * pipeline, its push constants, viewport and scissor (mtl_draw_state) and applies them to each
 * encoder it opens, because Metal state does not outlive an encoder. */

#define DE__MTL_STORE_DEPTH   (1u << 8)
#define DE__MTL_STORE_STENCIL (1u << 9)

/* Stages that get the push constants. */
#define DE__MTL_PUSH_VERTEX   1u
#define DE__MTL_PUSH_FRAGMENT 2u
#define DE__MTL_PUSH_OBJECT   4u
#define DE__MTL_PUSH_MESH     8u
#define DE__MTL_PUSH_COMPUTE  16u

/* Draw state not yet applied to the open encoder. */
#define DE__MTL_DIRTY_PIPELINE 1u
#define DE__MTL_DIRTY_PUSH     2u
#define DE__MTL_DIRTY_VIEWPORT 4u
#define DE__MTL_DIRTY_SCISSOR  8u
#define DE__MTL_DIRTY_ALL      15u

typedef struct mtl_pipeline {
    void *state;       /* id<MTLRenderPipelineState> or id<MTLComputePipelineState>, retained */
    void *depth_state; /* id<MTLDepthStencilState>, retained; NULL for compute */
    u32 cull;          /* MTLCullMode */
    u32 push_stages;   /* DE__MTL_PUSH_* */
    u32 push_bytes;    /* setBytes size: push_constant_bytes or the shaders' binding size */
    bool archive_hit;  /* found in the binary archive (tests) */
} mtl_pipeline;

/* ---------- mtl_pipeline.m ---------- */

de_result de__mtl_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                  const de_gpu_pipeline_desc *desc);
void de__mtl_pipeline_destroy(de_gpu_device *d, de__gpu_pipeline *p);
/** Releases the library cache and the ICB kernel (device destroy). */
void de__mtl_pipeline_shutdown(de_gpu_device *d);

/* ---------- mtl_draw.m ---------- */

/* Vtable entries that wrap mtl_cmd.m's (they keep the draw state) and the draw commands. */
de_result de__mtl_draw_cmd_begin(de_gpu_cmd *c, const char *name);
void de__mtl_draw_cmd_fini(de_gpu_device *d, de_gpu_cmd *c);
void de__mtl_draw_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                               const de_gpu_attachment *depth);
void de__mtl_draw_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h);
void de__mtl_cmd_set_pipeline(de_gpu_cmd *c, de__gpu_pipeline *p);
void de__mtl_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes);
void de__mtl_cmd_draw(de_gpu_cmd *c, u32 vertex_count, u32 instance_count, u32 first_vertex,
                      u32 first_instance);
void de__mtl_cmd_draw_indexed(de_gpu_cmd *c, u32 index_count, u32 instance_count, u32 first_index,
                              i32 vertex_offset, u32 first_instance);
void de__mtl_cmd_set_index_buffer(de_gpu_cmd *c, de__gpu_buffer *b, u64 offset, bool index_u16);
void de__mtl_cmd_scissor(de_gpu_cmd *c, i32 x, i32 y, u32 w, u32 h);
void de__mtl_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z);
void de__mtl_cmd_dispatch_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset);
void de__mtl_cmd_dispatch_mesh(de_gpu_cmd *c, u32 x, u32 y, u32 z);
void de__mtl_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                        de__gpu_buffer *count, u64 count_offset, u32 max);
/** The open render encoder with the draw state applied (id<MTLRenderCommandEncoder>, not
 * retained), or NULL outside a render pass. */
void *de__mtl_draw_render_encoder(de_gpu_cmd *c);
/** Pure: the scissor rectangle {x, y, w, h} of (x, y, w, h) cut to a w x h render area. */
void de__mtl_scissor_rect(i32 x, i32 y, u32 w, u32 h, u32 area_w, u32 area_h, u32 out[4]);

/* ---------- mtl_cmd.m (DENG-325 additions) ---------- */

/** Sets the store actions that begin_render left open (MTLStoreActionUnknown): Store when
 * `store_all`, else the attachments' own `store` (DontCare). Before the render encoder ends. */
void de__mtl_render_resolve_store(de_gpu_cmd *c, void *render_encoder, bool store_all);

/* ---------- mtl_icb.m ---------- */

/** draw_indexed_indirect: plain indirect draws, or with a count buffer an indirect command
 * buffer that a compute kernel encodes (the render pass is split around that kernel). */
void de__mtl_cmd_draw_indexed_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                       de__gpu_buffer *count, u64 count_offset, u32 max_draws);
/** Releases a list's ICB objects (cmd_fini); `keep` = only the retired ones (cmd_begin). */
void de__mtl_icb_release(mtl_draw_state *ds, bool keep);

/* ---------- mtl_binary_archive.m ---------- */

enum {
    DE__MTL_ARCHIVE_NONE,     /* no pipeline cache ("" or no such mount) */
    DE__MTL_ARCHIVE_EMPTY,    /* no file yet: a new archive */
    DE__MTL_ARCHIVE_LOADED,   /* the file loaded */
    DE__MTL_ARCHIVE_REJECTED, /* the file did not load: a new archive replaces it */
};
/** Opens (or starts) the archive of desc->pipeline_cache_dir at device creation. Never fails:
 * without one, pipelines are compiled every time. */
void de__mtl_archive_open(de_gpu_device *d, const de_gpu_device_desc *desc);
/** Writes the archive back (atomically) when pipelines were added; device destroy. */
void de__mtl_archive_save(de_gpu_device *d);
void de__mtl_archive_close(de_gpu_device *d);
/** NSArray<id<MTLBinaryArchive>> for a pipeline descriptor's binaryArchives (not retained), or
 * NULL without an archive. */
void *de__mtl_archive_list(de_gpu_device *d);
/** Counts a pipeline found in the archive. */
void de__mtl_archive_hit(de_gpu_device *d);
/** Counts a miss and adds the pipeline's functions from `descriptor` (MTLRenderPipelineDescriptor,
 * MTLMeshRenderPipelineDescriptor or MTLComputePipelineDescriptor) to the archive, compiling
 * them. False when the archive did not take them. */
bool de__mtl_archive_add(de_gpu_device *d, void *descriptor);
/** State, counters and native path of the archive (tests). */
u32 de__mtl_archive_state(de_gpu_device *d, u32 *hits, u32 *misses, const char **path);
/** Pure: the file name `metal-<registryID hex>-<os build>.metallib`, the device name (letters and
 * digits) standing in when `registry_id` is 0. */
void de__mtl_archive_file_name(u64 registry_id, const char *device_name, const char *os_build,
                               char *out, size_t cap);

/* ---------- DENG-326: timestamps, debug groups, labels, capture ----------
 *
 * Timestamps (mtl_query.m): Apple GPUs sample the timestamp counter set only at stage (encoder)
 * boundaries, so de_gpu_cmd_timestamp is a small blit encoder of its own whose start is sampled
 * into the heap's MTLCounterSampleBuffer, ordered after every earlier encoder and before every
 * later one (a full barrier). GPU-side resolveCounters: writes zeros on these GPUs (checked on an
 * M5 Pro), so a resolve signals an event, a listener resolves the samples on the CPU
 * (resolveCounterRange:) into the heap's staging buffer and signals back, and a blit copies them
 * into the destination: later commands of the list wait for it. */

/** Most timestamps in one heap: a counter sample buffer holds 32 KiB of 8-byte samples. */
#define DE__MTL_MAX_QUERIES 4096u

typedef struct mtl_query_heap {
    void *samples; /* id<MTLCounterSampleBuffer>, shared storage, retained */
    void *staging; /* id<MTLBuffer>, shared: count resolved u64, then 16 bytes the timestamp
                      encoders fill (their one command) */
} mtl_query_heap;

/* mtl_query.m */
de_result de__mtl_query_heap_create(de_gpu_device *d, de__gpu_query_heap *h,
                                    const de_gpu_query_heap_desc *desc);
void de__mtl_query_heap_destroy(de_gpu_device *d, de__gpu_query_heap *h);
void de__mtl_cmd_timestamp(de_gpu_cmd *c, de__gpu_query_heap *h, u32 index);
void de__mtl_cmd_resolve_queries(de_gpu_cmd *c, de__gpu_query_heap *h, u32 first, u32 count,
                                 de__gpu_buffer *dst, u64 dst_off);
/** The GPU time now (sampleTimestamps:gpuTimestamp:, nanoseconds). */
bool de__mtl_queue_timestamp_now(de_gpu_device *d, de_gpu_queue q, u64 *ticks);
/** Releases a list's resolve event and listener (cmd_fini). */
void de__mtl_query_cmd_fini(de_gpu_cmd *c);

/* mtl_debug.m */
/** Debug groups: on the open render encoder inside a render pass, else on the command buffer
 * (after ending the open blit or compute encoder, so the group holds exactly the encoders made
 * inside it). */
void de__mtl_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color);
void de__mtl_cmd_marker_pop(de_gpu_cmd *c);
/** Labels: buffers, textures (and their storage views), fences, query heaps. Pipeline and sampler
 * labels are fixed at creation by Metal: pipelines take theirs from the record's name. */
void de__mtl_object_name(de_gpu_device *d, de__gpu_kind kind, void *rec, const char *name);

/* Test hooks (mtl_debug.m). */
/** Starts a programmatic GPU capture of the device into the .gputrace document `path` (it must
 * not exist). DE_ERR_UNSUPPORTED when the process cannot capture: MTL_CAPTURE_ENABLED=1 must be
 * in the environment before the first Metal device (or the app's Info.plist enables capture). */
de_result de__mtl_capture_begin(de_gpu_device *d, const char *path);
/** Ends the capture and writes the document. */
void de__mtl_capture_end(de_gpu_device *d);
/** The Metal label of a buffer, texture, fence or query heap (UTF-8, cut to `cap`); false when
 * the object has none or `handle` (an id) is stale. */
bool de__mtl_object_label(de_gpu_device *d, de__gpu_kind kind, u32 handle, char *out, size_t cap);

/* ---------- DENG-407: storage textures and residency ---------- */

/** The storage-texture objects that render and compute encoders name with read-write usage
 * (mtl_device.storage_objs: every storage object on the useHeaps: path, the per-mip views with a
 * residency set). Copies up to `cap` (id<MTLTexture>) into `out`; returns the total count. */
u32 de__mtl_storage_texture_list(de_gpu_device *d, void **out, u32 cap);

/* ---------- DENG-328: GPU crash diagnostics (mtl_errors.m) ---------- */

/** With desc->crash_breadcrumbs: the fault log and the breadcrumb staging buffers. */
de_result de__mtl_errors_init(de_gpu_device *d, const de_gpu_device_desc *desc);
void de__mtl_errors_shutdown(de_gpu_device *d);
/** A new command buffer of queue `q` (retained id<MTLCommandBuffer>, NULL on failure): unretained
 * references, and encoder execution status when crash diagnostics are on. */
void *de__mtl_new_command_buffer(de_gpu_device *d, de_gpu_queue q);
/** Records a failed command buffer (completion handler): `fault` is the device's fault_log (NULL:
 * nothing), `command_buffer` the id<MTLCommandBuffer>. Marks the device lost. */
void de__mtl_errors_record(void *fault, void *command_buffer, const char *queue);
/** vtable device_lost / device_lost_report / cmd_breadcrumb. */
bool de__mtl_device_lost(de_gpu_device *d);
size_t de__mtl_device_lost_report(de_gpu_device *d, char *out, size_t cap);
void de__mtl_cmd_breadcrumb(de_gpu_cmd *c, de__gpu_buffer *ring, u64 offset, u32 value);
/** Test hook: records a simulated failed command buffer `list` on the compute queue (a page
 * fault, with three encoders: a completed breadcrumb, a faulted compute encoder and a pending
 * breadcrumb, all inside debug group `group`) exactly as the completion handler records a real
 * one, so the device counts as lost. False without crash diagnostics. Apple GPUs discard
 * out-of-bounds accesses instead of faulting, so tests cannot make a real fault on demand. */
bool de__mtl_errors_simulate(de_gpu_device *d, const char *list, const char *group);

#endif
