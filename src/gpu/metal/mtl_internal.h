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

#endif
