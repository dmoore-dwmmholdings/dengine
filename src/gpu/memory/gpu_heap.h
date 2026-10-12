/* gpu_heap.h — device memory heaps and the sub-allocator shared by the backends (internal,
 * DENG-282). Included by gpu_backend.h, so every backend sees it.
 *
 * Model (SPEC §12.1 "device memory heaps with an engine sub-allocator (TLSF)"):
 *   - A **memory class** is one kind of device memory a backend allocates heaps from. Classes 0, 1
 *     and 2 always exist and serve DE_MEM_GPU_ONLY, DE_MEM_UPLOAD and DE_MEM_READBACK (their ids
 *     equal the de_gpu_memory values). A backend adds sub-classes in its device_create, e.g.
 *     "render targets" on D3D12 resource heap tier 1 (buffers and textures cannot share a heap) or
 *     one class per Vulkan memory type it uses, and may reconfigure the three base classes.
 *   - Each class owns a list of **heaps**: blocks of `block_size` bytes (default 256 MiB) created
 *     on demand through the backend's `heap_create` callback and sub-allocated by an offset TLSF
 *     (gpu_tlsf.h). A request above `dedicated_threshold` (default 64 MiB) gets a heap of its own,
 *     released when it is freed. When a block heap becomes empty it stays as the class's spare;
 *     an empty heap beyond one spare per class is released at once.
 *   - An allocation is a **de__gpu_alloc** record: heap, offset, size, class, memory tag. Backends
 *     keep it in their resource payload and create the resource placed at (heap, offset).
 *   - One mutex per class. Allocation is not on a per-draw path: resources are created at load
 *     time, and render-graph transients come from regions reserved once (below).
 *
 * Backend contract (vtable entries in gpu_backend.h):
 *   - heap_create(d, heap, cls): create the native heap of heap->size bytes for class `cls`
 *     (cls->backend_bits says which memory) in the payload DE__GPU_PAYLOAD(T, heap)
 *     (`heap_payload_size` bytes). Set heap->mapped if the whole heap is persistently mapped
 *     (Vulkan host-visible memory); the CPU address of an allocation is then mapped + offset.
 *   - heap_destroy(d, heap): release it. Called with no live allocation in it, after the GPU is
 *     done with every resource placed in it (resources free their allocation from their deferred
 *     destroy), or at device destroy.
 *   - Both run under the class mutex; they must not call back into the heap manager.
 *
 * Transient aliasing (SPEC §12.3; the S12 render graph decides lifetimes and offsets):
 *   - de__gpu_transient_region_create reserves `bytes` of one class as one allocation.
 *   - de__gpu_place_transient_buffer / _texture create a resource at a caller-chosen offset in
 *     the region through the backend's placed-creation entries. Several resources may overlap;
 *     the caller orders their use with barriers.
 *   - Each placed resource holds a reference on its region; destroying the region frees its
 *     memory once the last placed resource has been reclaimed (after the GPU is done with it).
 */
#ifndef DE_GPU_HEAP_H
#define DE_GPU_HEAP_H
#include <dengine/de_gpu.h>

/* ---------- Memory classes ---------- */

/** Base classes; their ids equal the de_gpu_memory values. */
enum {
    DE__GPU_MEM_CLASS_GPU_ONLY = DE_MEM_GPU_ONLY,
    DE__GPU_MEM_CLASS_UPLOAD = DE_MEM_UPLOAD,
    DE__GPU_MEM_CLASS_READBACK = DE_MEM_READBACK,
    DE__GPU_MEM_CLASS_BASE_COUNT
};
/** At most this many classes per device (a class mask is a u32). */
#define DE__GPU_MEM_CLASS_MAX 32u
/** Defaults of a class desc whose field is 0. */
#define DE__GPU_HEAP_DEFAULT_BLOCK_SIZE     ((u64)256 << 20)
#define DE__GPU_HEAP_DEFAULT_DEDICATED_SIZE ((u64)64 << 20)
/** Alignment of a transient region, and the largest placement alignment backends need
 * (D3D12 and Vulkan: 64 KiB for textures; MSAA textures need 4 MiB on D3D12 and fall back to a
 * dedicated region). */
#define DE__GPU_TRANSIENT_REGION_ALIGN ((u64)64 << 10)

typedef struct de__gpu_mem_class_desc {
    /** Which de_gpu_memory the class serves: the per-kind totals and profiler plots add it in. */
    de_gpu_memory memory;
    /** Debug name (copied, cut to 31 bytes). NULL = "gpu_only" / "upload" / "readback" etc. */
    const char *name;
    /** Size of a block heap; 0 = DE__GPU_HEAP_DEFAULT_BLOCK_SIZE. Rounded up to 256 bytes. */
    u64 block_size;
    /** Requests larger than this get a dedicated heap; 0 = DE__GPU_HEAP_DEFAULT_DEDICATED_SIZE. */
    u64 dedicated_threshold;
    /** Backend data for heap_create: a Vulkan memory type index, D3D12 heap flags, a Metal
     * storage mode. The front-end does not read it. */
    u32 backend_bits;
} de__gpu_mem_class_desc;

/**
 * Registers another class. Call from the backend's device_create (classes are fixed once the
 * device exists). DE_ERR_OUT_OF_MEMORY beyond DE__GPU_MEM_CLASS_MAX classes.
 */
de_result de__gpu_mem_add_class(de_gpu_device *d, const de__gpu_mem_class_desc *desc,
                                u32 *out_class);
/** Replaces the settings of an existing class (base classes included); device_create only. */
de_result de__gpu_mem_configure_class(de_gpu_device *d, u32 cls,
                                      const de__gpu_mem_class_desc *desc);
u32 de__gpu_mem_class_count(de_gpu_device *d);
/** The class's settings with defaults applied (name points at the copied name), or NULL. */
const de__gpu_mem_class_desc *de__gpu_mem_class_get(de_gpu_device *d, u32 cls);

/* ---------- Heaps and allocations ---------- */

typedef struct de__gpu_tlsf de__gpu_tlsf;
typedef struct de__gpu_tlsf_block de__gpu_tlsf_block;

/** One device heap. The backend payload follows it: DE__GPU_PAYLOAD(T, heap). */
typedef struct de__gpu_heap {
    u32 mem_class;
    u32 index;          /**< slot in its class's heap table */
    u64 size;           /**< bytes */
    bool dedicated;     /**< holds exactly one allocation, at offset 0 */
    void *mapped;       /**< set by heap_create when the whole heap is persistently mapped */
    u32 live;           /**< allocations in it */
    de__gpu_tlsf *tlsf; /**< NULL for a dedicated heap */
} de__gpu_heap;

/** A sub-allocation. All zero = none. Backends store it in their resource payload. */
typedef struct de__gpu_alloc {
    de__gpu_heap *heap;
    u64 offset;     /**< within the heap; a multiple of the requested alignment */
    u64 size;       /**< bytes reserved: the request rounded up to 256 */
    u32 heap_index; /**< heap->index */
    u16 mem_class;
    u8 tag; /**< de_mem tag (DE_MEM_RENDER, ...) for the per-tag statistics */
    u8 dedicated;
    de__gpu_tlsf_block *block; /**< NULL for a dedicated heap */
} de__gpu_alloc;

/**
 * Allocates `size` bytes aligned to `align` (a power of two; 0 = 256) from class `cls`, from an
 * existing heap, a new block heap, or a dedicated heap when `size` exceeds the class's threshold.
 * `tag` is a de_mem tag (out of range counts as DE_MEM_GENERAL). DE_ERR_UNSUPPORTED when the
 * backend has no heap_create; DE_ERR_OUT_OF_MEMORY (wrapped) when a new heap cannot be created.
 */
de_result de__gpu_mem_alloc(de_gpu_device *d, u32 cls, u64 size, u64 align, u32 tag,
                            de__gpu_alloc *out);
/** Frees an allocation and zeroes the record. Only once the GPU is done with it. NULL / zero
 * records are ignored. */
void de__gpu_mem_free(de_gpu_device *d, de__gpu_alloc *a);
/** CPU address of a host-visible allocation (heap->mapped + offset), or NULL. */
void *de__gpu_mem_mapped(const de__gpu_alloc *a);

/* ---------- Statistics ---------- */

typedef struct de__gpu_mem_stats {
    u64 used;         /**< bytes in live allocations (rounded sizes) */
    u64 reserved;     /**< bytes of all heaps, dedicated ones included */
    u64 free;         /**< reserved - used */
    u64 largest_free; /**< largest free block in any block heap of the class */
    u32 allocations;
    u32 heaps; /**< block heaps */
    u32 dedicated_heaps;
    u32 empty_heaps;   /**< block heaps with no allocation (at most one: the spare) */
    u32 free_blocks;   /**< free TLSF blocks over all block heaps */
    f32 fragmentation; /**< 1 - largest_free / free of the block heaps; 0 when unfragmented */
} de__gpu_mem_stats;

/** Statistics of one class (takes its mutex; walks its heaps). DE_ERR_INVALID_ARGUMENT for an
 * unknown class. */
de_result de__gpu_mem_class_stats(de_gpu_device *d, u32 cls, de__gpu_mem_stats *out);
/** Bytes allocated under de_mem tag `tag`, all classes (relaxed atomic read). */
u64 de__gpu_mem_tag_used(de_gpu_device *d, u32 tag);
/** Bytes used and reserved over every class serving `memory`. The profiler plots
 * "gpu.mem.<gpu_only|upload|readback>.used" / ".reserved" follow these after every change. */
u64 de__gpu_mem_kind_used(de_gpu_device *d, de_gpu_memory memory);
u64 de__gpu_mem_kind_reserved(de_gpu_device *d, de_gpu_memory memory);

/* ---------- Transient regions (render-graph aliasing hooks) ---------- */

typedef struct de__gpu_transient_region de__gpu_transient_region;

/** What a resource needs from memory. `class_mask` bit c set = it can live in class c. */
typedef struct de__gpu_mem_req {
    u64 size;
    u64 align;
    u32 class_mask;
} de__gpu_mem_req;

/** Size, alignment and classes a buffer / texture would need (backend memory_requirements).
 * The desc is validated first. DE_ERR_UNSUPPORTED when the backend cannot tell. */
de_result de__gpu_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                      de__gpu_mem_req *out);
de_result de__gpu_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                       de__gpu_mem_req *out);

/**
 * Reserves `bytes` of class `cls` (aligned to DE__GPU_TRANSIENT_REGION_ALIGN) for placed
 * transient resources. A region above the class's dedicated threshold gets a heap of its own.
 */
de_result de__gpu_transient_region_create(de_gpu_device *d, u32 cls, u64 bytes,
                                          de__gpu_transient_region **out);
/**
 * Drops the caller's reference. The memory is freed when no resource placed in the region is
 * left: at once if there is none, else when the last one is reclaimed by deferred destruction.
 * Destroy (or let the device reclaim) the placed resources; do not place new ones afterwards.
 */
void de__gpu_transient_region_destroy(de__gpu_transient_region *r);
u64 de__gpu_transient_region_size(const de__gpu_transient_region *r);
u32 de__gpu_transient_region_class(const de__gpu_transient_region *r);
/** The region's allocation (heap and base offset). */
const de__gpu_alloc *de__gpu_transient_region_alloc(const de__gpu_transient_region *r);
/** Resources placed in the region and not yet reclaimed. */
u32 de__gpu_transient_region_placed(const de__gpu_transient_region *r);

/**
 * Creates a resource at `offset` bytes into the region (the caller picks offsets; overlapping
 * placements alias). Checks the desc, the requirements (alignment, end within the region,
 * class), then calls the backend's buffer_create_placed / texture_create_placed. Textures are
 * created with `transient` set. Returns an invalid handle with the error context set on failure
 * (DE_ERR_UNSUPPORTED when the backend has no placed creation). Destroy with the usual
 * de_gpu_buffer_destroy / de_gpu_texture_destroy.
 */
de_gpu_buffer de__gpu_place_transient_buffer(de__gpu_transient_region *r, u64 offset,
                                             const de_gpu_buffer_desc *desc);
de_gpu_texture de__gpu_place_transient_texture(de__gpu_transient_region *r, u64 offset,
                                               const de_gpu_texture_desc *desc);

/* ---------- Front-end hooks (gpu.c) ---------- */

/** Creates the heap manager and the three base classes; before the backend's device_create. */
de_result de__gpu_heaps_init(de_gpu_device *d);
/**
 * Releases every heap (warning about live allocations) and the manager. `backend_alive`: call
 * heap_destroy for each heap (false when the backend device was never created).
 */
void de__gpu_heaps_shutdown(de_gpu_device *d, bool backend_alive);
/** A placed resource of `r` was reclaimed (gpu.c reclaim). */
void de__gpu_transient_region_release(de__gpu_transient_region *r);

#endif
