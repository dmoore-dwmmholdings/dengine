/* gpu_backend.h — the de_gpu front-end <-> backend contract (internal, DENG-281).
 *
 * The front-end (gpu.c) owns everything that is the same on every backend: handle pools, bindless
 * index allocation, deferred destruction, command-list recycling, validation and backend
 * selection. A backend fills one de__gpu_backend_vtable and keeps its own state in payloads that
 * the front-end allocates inline next to its records:
 *   - device:  de__gpu_device_payload(d), `device_payload_size` bytes;
 *   - object:  DE__GPU_PAYLOAD(rec), `payload_size[kind]` bytes after the front-end record;
 *   - command: de__gpu_cmd_payload(c), `cmd_payload_size` bytes after struct de_gpu_cmd.
 * Payloads are zeroed before the backend's create call and 16-byte aligned.
 *
 * Calling rules for backends:
 *   - The front-end has validated every desc and resolved every handle argument before a vtable
 *     call; records passed in are live. Handles inside structs (barriers, attachments, submit
 *     waits) are resolved by the backend with de__gpu_get_*; they were validated in Debug/Dev.
 *   - Create entries may fail: return a de_result and set the error context (DE_FAIL).
 *   - Resource create/destroy and bindless writes can come from several threads at once.
 *   - `X_destroy` runs once the GPU is done with the object (deferred), or at device destroy.
 *   - `queue_submit` gets the queue's next internal timeline value; the backend signals it when
 *     the submission completes and reports it through `queue_completed`. Deferred destruction and
 *     command-list recycling wait on these values.
 *   - A NULL entry means "not supported": the front-end returns DE_ERR_UNSUPPORTED (creates) or
 *     does nothing (commands).
 *
 * Registration: gpu.c has a static table of the backends compiled into the build, one entry per
 * backend guarded by DE_HAS_GPU_<NAME> (set by cmake/DeOptions.cmake). A backend task appends its
 * entry there and declares its vtable below. de__gpu_register_backend overrides an entry at run
 * time (tests).
 */
#ifndef DE_GPU_BACKEND_H
#define DE_GPU_BACKEND_H
#include <dengine/de_gpu.h>
#include "gpu/handle_pool.h"
#include "gpu/memory/gpu_heap.h"

/* ---------- Object kinds and records ---------- */

typedef enum de__gpu_kind {
    DE__GPU_BUFFER,
    DE__GPU_TEXTURE,
    DE__GPU_SAMPLER,
    DE__GPU_PIPELINE,
    DE__GPU_ACCEL,
    DE__GPU_SWAPCHAIN,
    DE__GPU_FENCE,
    DE__GPU_QUERY_HEAP,
    DE__GPU_KIND_COUNT
} de__gpu_kind;

/** Bindless index spaces: one descriptor table each (shaders/common/bindless.slang). */
typedef enum de__gpu_bindless_space {
    DE__GPU_BINDLESS_SAMPLED_TEXTURE,
    DE__GPU_BINDLESS_STORAGE_TEXTURE,
    DE__GPU_BINDLESS_BUFFER,
    DE__GPU_BINDLESS_SAMPLER,
    DE__GPU_BINDLESS_ACCEL,
    DE__GPU_BINDLESS_SPACE_COUNT
} de__gpu_bindless_space;

/** Copied debug name (messages, backend object labels). Longer names are cut. */
#define DE__GPU_NAME_MAX 48

typedef struct de__gpu_buffer {
    u64 size;
    u32 usage;
    de_gpu_memory memory;
    u32 stride;
    u32 bindless; /**< DE__GPU_BINDLESS_BUFFER index or DE_GPU_BINDLESS_INVALID */
    void *mapped; /**< UPLOAD/READBACK: set by the backend's buffer_create */
    char name[DE__GPU_NAME_MAX];
    de__gpu_transient_region *region; /**< placed in a transient region (DENG-282), or NULL */
} de__gpu_buffer;

typedef struct de__gpu_texture {
    de_gpu_texture_dim dim;
    de_gpu_format format;
    u32 width, height, depth_or_layers, mip_count, sample_count, usage; /**< defaults applied */
    bool transient;
    bool external; /**< wraps a backend-owned image (swapchain); see de__gpu_texture_wrap */
    u32 sampled;   /**< DE__GPU_BINDLESS_SAMPLED_TEXTURE index or 0 */
    u32 storage[DE_GPU_MAX_MIPS]; /**< DE__GPU_BINDLESS_STORAGE_TEXTURE index per mip, or 0 */
    char name[DE__GPU_NAME_MAX];
    de__gpu_transient_region *region; /**< placed in a transient region (DENG-282), or NULL */
} de__gpu_texture;

typedef struct de__gpu_sampler {
    de_gpu_sampler_desc desc;
    u32 bindless;
} de__gpu_sampler;

typedef struct de__gpu_pipeline {
    de_gpu_pipeline_kind kind;
    u32 push_constant_bytes;
    u32 color_count;
    de_gpu_format color_formats[DE_GPU_MAX_COLOR_ATTACHMENTS];
    de_gpu_format depth_format;
    char name[DE__GPU_NAME_MAX];
    /** From the desc (S10, DENG-323): 0 when not given. */
    u32 threadgroup_size[3], object_threadgroup_size[3];
} de__gpu_pipeline;

typedef struct de__gpu_accel {
    bool is_tlas;
    u32 bindless;
    char name[DE__GPU_NAME_MAX];
} de__gpu_accel;

typedef struct de__gpu_swapchain {
    de_gpu_swapchain_desc desc;
} de__gpu_swapchain;

typedef struct de__gpu_fence {
    u64 initial;
} de__gpu_fence;

typedef struct de__gpu_query_heap {
    de_gpu_query_type type;
    u32 count;
    char name[DE__GPU_NAME_MAX];
    de_gpu_queue queue; /**< from the desc (DENG-326): COPY heaps serve copy lists only */
} de__gpu_query_heap;

#define DE__GPU_ALIGN16(n) (((n) + 15u) & ~(size_t)15u)
/** The backend payload behind a front-end record: `DE__GPU_PAYLOAD(my_vk_buffer, rec)`. */
#define DE__GPU_PAYLOAD(T, rec) ((T *)(void *)((u8 *)(rec) + DE__GPU_ALIGN16(sizeof *(rec))))

/* ---------- Command lists ---------- */

/** Open debug markers a command list keeps names for (deeper ones are counted only), and the bytes
 * of name text it keeps for them (DENG-326). */
#define DE__GPU_MARKER_LEVELS 32u
#define DE__GPU_MARKER_TEXT   1024u

/** Breadcrumbs (DENG-328): u32 slots in each queue's ring buffer, sequence numbers a command list
 * reserves at a time, and the chunks one recording keeps (DE__GPU_BC_RANGES * DE__GPU_BC_CHUNK
 * breadcrumbs per recording; more are dropped and counted). */
#define DE__GPU_BC_SLOTS  2048u
#define DE__GPU_BC_CHUNK  16u
#define DE__GPU_BC_RANGES 32u

/** Front-end part of a command list. The backend payload follows (de__gpu_cmd_payload). */
struct de_gpu_cmd {
    de_gpu_device *device;
    de_gpu_queue queue;
    u32 state;        /**< DE__GPU_CMD_* */
    bool in_render;   /**< between begin_render and end_render */
    u32 push_limit;   /**< push_constant_bytes of the bound pipeline */
    u32 marker_depth; /**< open marker_push scopes */
    de_gpu_pipeline_kind pipeline_kind;
    bool has_pipeline;
    u64 queue_value;             /**< internal timeline value of its submission */
    struct de_gpu_cmd *next;     /**< free / in-flight list */
    struct de_gpu_cmd *all_next; /**< every command list of the device */
    char name[DE__GPU_NAME_MAX];
    bool has_index_buffer; /**< de_gpu_cmd_set_index_buffer was called (S10, DENG-323) */
    /* DENG-326 (query.c): the open debug markers, outermost first. Names are copied into
     * `marker_text` (cut to fit), so a marker never allocates; read them with
     * de__gpu_cmd_marker_*. Levels past DE__GPU_MARKER_LEVELS are only counted (marker_depth). */
    u32 marker_render_base;                   /**< marker_depth when the open render pass began */
    u32 marker_text_used;                     /**< bytes of marker_text in use */
    u16 marker_offset[DE__GPU_MARKER_LEVELS]; /**< where each level's name starts */
    u32 marker_color[DE__GPU_MARKER_LEVELS];  /**< 0xRRGGBB of each level */
    char marker_text[DE__GPU_MARKER_TEXT];
    /* DENG-328 (breadcrumbs.c): the breadcrumb sequence numbers this recording took, as chunks of
     * DE__GPU_BC_CHUNK numbers reserved from the queue's counter (`bc_first[i]`, `bc_used[i]` of
     * them used). Breadcrumbs past DE__GPU_BC_RANGES chunks are counted in `bc_dropped`. */
    u32 bc_ranges;
    u32 bc_dropped;
    u32 bc_first[DE__GPU_BC_RANGES];
    u8 bc_used[DE__GPU_BC_RANGES];
};
enum { DE__GPU_CMD_FREE, DE__GPU_CMD_RECORDING, DE__GPU_CMD_SUBMITTED };

static inline void *de__gpu_cmd_payload(de_gpu_cmd *c) {
    return (u8 *)c + DE__GPU_ALIGN16(sizeof(struct de_gpu_cmd));
}

/* ---------- The vtable ---------- */

typedef struct de__gpu_backend_vtable {
    const char *name;
    u32 device_payload_size;
    u32 cmd_payload_size;
    u32 payload_size[DE__GPU_KIND_COUNT];

    /* Device. device_create fills `caps` (the front-end sets caps->backend) and may read the
     * desc; device_destroy runs after every object and command list is gone. */
    de_result (*device_create)(de_gpu_device *d, const de_gpu_device_desc *desc, de_gpu_caps *caps);
    void (*device_destroy)(de_gpu_device *d);
    void (*wait_idle)(de_gpu_device *d);

    /* Resources. buffer_create sets b->mapped for UPLOAD/READBACK buffers. */
    de_result (*buffer_create)(de_gpu_device *d, de__gpu_buffer *b, const de_gpu_buffer_desc *desc);
    void (*buffer_destroy)(de_gpu_device *d, de__gpu_buffer *b);
    de_result (*texture_create)(de_gpu_device *d, de__gpu_texture *t,
                                const de_gpu_texture_desc *desc);
    void (*texture_destroy)(de_gpu_device *d, de__gpu_texture *t);
    de_result (*sampler_create)(de_gpu_device *d, de__gpu_sampler *s,
                                const de_gpu_sampler_desc *desc);
    void (*sampler_destroy)(de_gpu_device *d, de__gpu_sampler *s);

    /* Bindless: write the descriptor at `index` of its table. Called after create (indices are
     * already in the record); the slot is never rewritten until the index is freed. */
    void (*bindless_write_buffer)(de_gpu_device *d, u32 index, de__gpu_buffer *b);
    void (*bindless_write_texture)(de_gpu_device *d, u32 index, de__gpu_texture *t, u32 mip,
                                   bool storage);
    void (*bindless_write_sampler)(de_gpu_device *d, u32 index, de__gpu_sampler *s);
    void (*bindless_write_accel)(de_gpu_device *d, u32 index, de__gpu_accel *a);

    /* Pipelines. */
    de_result (*pipeline_create)(de_gpu_device *d, de__gpu_pipeline *p,
                                 const de_gpu_pipeline_desc *desc);
    void (*pipeline_destroy)(de_gpu_device *d, de__gpu_pipeline *p);

    /* Acceleration structures (S16). The front-end refuses them until then. */
    de_result (*accel_create)(de_gpu_device *d, de__gpu_accel *a, const de_gpu_accel_desc *desc);
    void (*accel_destroy)(de_gpu_device *d, de__gpu_accel *a);

    /* Command lists. cmd_init/cmd_fini once per command-list object; cmd_begin each time it is
     * (re)used, which is only after its previous submission completed. */
    de_result (*cmd_init)(de_gpu_device *d, de_gpu_cmd *c);
    void (*cmd_fini)(de_gpu_device *d, de_gpu_cmd *c);
    de_result (*cmd_begin)(de_gpu_cmd *c, const char *name);
    void (*cmd_barriers)(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count);
    void (*cmd_begin_render)(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                             const de_gpu_attachment *depth);
    void (*cmd_end_render)(de_gpu_cmd *c);
    void (*cmd_set_pipeline)(de_gpu_cmd *c, de__gpu_pipeline *p);
    void (*cmd_push_constants)(de_gpu_cmd *c, const void *data, u32 bytes);
    void (*cmd_viewport)(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h);
    void (*cmd_draw_indexed_indirect)(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                      de__gpu_buffer *count, u64 count_offset, u32 max_draws);
    void (*cmd_dispatch_mesh_indirect)(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                       de__gpu_buffer *count, u64 count_offset, u32 max);
    void (*cmd_dispatch)(de_gpu_cmd *c, u32 x, u32 y, u32 z);
    void (*cmd_dispatch_indirect)(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset);
    void (*cmd_dispatch_rays)(de_gpu_cmd *c, u32 w, u32 h, u32 depth);
    void (*cmd_build_blas)(de_gpu_cmd *c, de__gpu_accel *blas, bool update);
    void (*cmd_build_tlas)(de_gpu_cmd *c, de__gpu_accel *tlas, de__gpu_buffer *instances, u32 count,
                           bool update);
    void (*cmd_copy_buffer)(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off, de__gpu_buffer *src,
                            u64 src_off, u64 bytes);
    void (*cmd_copy_buffer_to_texture)(de_gpu_cmd *c, de__gpu_texture *dst, u32 mip, u32 layer,
                                       de__gpu_buffer *src, u64 src_off, u32 row_pitch);
    void (*cmd_marker_push)(de_gpu_cmd *c, const char *name, u32 color);
    void (*cmd_marker_pop)(de_gpu_cmd *c);
    /* DENG-328: write the u32 `value` at byte `offset` (a multiple of 4) of `ring`, a READBACK
     * buffer, once every command recorded before it in the list has completed. Outside render
     * passes only (the front-end skips it inside one). NULL: no breadcrumbs. */
    void (*cmd_breadcrumb)(de_gpu_cmd *c, de__gpu_buffer *ring, u64 offset, u32 value);
    void (*cmd_timestamp)(de_gpu_cmd *c, de__gpu_query_heap *h, u32 index);
    void (*cmd_resolve_queries)(de_gpu_cmd *c, de__gpu_query_heap *h, u32 first, u32 count,
                                de__gpu_buffer *dst, u64 dst_off);

    /* Submission and sync. The front-end publishes `queue_value` (deferred destroys made from
     * now on wait for it) before the call, so queue_submit must make it complete eventually
     * even when it fails (device lost: report every value as complete). */
    de_result (*queue_submit)(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                              u64 queue_value);
    u64 (*queue_completed)(de_gpu_device *d, de_gpu_queue q);
    de_result (*fence_create)(de_gpu_device *d, de__gpu_fence *f, u64 initial);
    void (*fence_destroy)(de_gpu_device *d, de__gpu_fence *f);
    u64 (*fence_value)(de_gpu_device *d, de__gpu_fence *f);
    de_result (*fence_wait)(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout_ns);

    /* Swapchain. Images come from de__gpu_texture_wrap. */
    de_result (*swapchain_create)(de_gpu_device *d, de__gpu_swapchain *s,
                                  const de_gpu_swapchain_desc *desc);
    void (*swapchain_destroy)(de_gpu_device *d, de__gpu_swapchain *s);
    de_result (*swapchain_acquire)(de_gpu_device *d, de__gpu_swapchain *s, de_gpu_texture *image);
    de_result (*swapchain_present)(de_gpu_device *d, de__gpu_swapchain *s);
    de_result (*swapchain_resize)(de_gpu_device *d, de__gpu_swapchain *s, u32 w, u32 h);
    de_gpu_colorspace (*swapchain_colorspace)(de_gpu_device *d, de__gpu_swapchain *s);

    /* Queries. */
    de_result (*query_heap_create)(de_gpu_device *d, de__gpu_query_heap *h,
                                   const de_gpu_query_heap_desc *desc);
    void (*query_heap_destroy)(de_gpu_device *d, de__gpu_query_heap *h);

    /* Device memory heaps (DENG-282, gpu/memory/gpu_heap.h has the contract). heap_create makes
     * the native heap of h->size bytes for class `cls` in DE__GPU_PAYLOAD(T, h)
     * (`heap_payload_size` bytes) and may set h->mapped; heap_destroy releases it. Both run under
     * the class mutex. NULL heap_create: de__gpu_mem_alloc fails with DE_ERR_UNSUPPORTED. */
    u32 heap_payload_size;
    de_result (*heap_create)(de_gpu_device *d, de__gpu_heap *h, const de__gpu_mem_class_desc *cls);
    void (*heap_destroy)(de_gpu_device *d, de__gpu_heap *h);
    /* Memory a resource would need: size, alignment, the classes it may live in. The desc is
     * validated; texture descs have the defaults applied (height, layers, mips, samples >= 1). */
    de_result (*buffer_requirements)(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                     de__gpu_mem_req *out);
    de_result (*texture_requirements)(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                      de__gpu_mem_req *out);
    /* Placed creation (transient aliasing): like buffer_create / texture_create, but the
     * resource uses `heap` memory at `offset` (requirements already checked). The record's
     * `region` is set. buffer_destroy / texture_destroy release it without freeing memory. */
    de_result (*buffer_create_placed)(de_gpu_device *d, de__gpu_buffer *b,
                                      const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                      u64 offset);
    de_result (*texture_create_placed)(de_gpu_device *d, de__gpu_texture *t,
                                       const de_gpu_texture_desc *desc, de__gpu_heap *heap,
                                       u64 offset);

    /* Bindless numbering (DENG-285). false: each index space numbers its own table from 1
     * (Vulkan bindings, Metal arrays). true: the resource spaces (sampled textures, storage
     * textures, buffers, acceleration structures) share one numbering, because D3D12 shaders
     * index one ResourceDescriptorHeap with all of them: the front-end gives each space the range
     * after the previous one (de__gpu_bindless_share_resource_numbering), so an index is the
     * descriptor slot. Samplers keep their own table. */
    bool bindless_shared_resource_heap;

    /* Texture -> buffer copy (DENG-284), the reverse of cmd_copy_buffer_to_texture. */
    void (*cmd_copy_texture_to_buffer)(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off,
                                       u32 row_pitch, de__gpu_texture *src, u32 mip, u32 layer);

    /* S10 draws (DENG-323). The front-end has checked the render pass, the bound pipeline's kind
     * and (draw_indexed) that an index buffer is bound. cmd_scissor gets the rectangle as given
     * (x or y may be negative: the backend cuts it to the render area). */
    void (*cmd_draw)(de_gpu_cmd *c, u32 vertex_count, u32 instance_count, u32 first_vertex,
                     u32 first_instance);
    void (*cmd_draw_indexed)(de_gpu_cmd *c, u32 index_count, u32 instance_count, u32 first_index,
                             i32 vertex_offset, u32 first_instance);
    void (*cmd_set_index_buffer)(de_gpu_cmd *c, de__gpu_buffer *b, u64 offset, bool index_u16);
    void (*cmd_scissor)(de_gpu_cmd *c, i32 x, i32 y, u32 w, u32 h);
    void (*cmd_dispatch_mesh)(de_gpu_cmd *c, u32 x, u32 y, u32 z);

    /* Debug names and timestamps (DENG-326). object_name gives the native object(s) behind a
     * record the name `name` (never empty: the desc's name, or "<kind>#<slot>" in Debug/Dev);
     * the front-end calls it after every successful create of a buffer, texture, sampler,
     * pipeline, fence or query heap, with the record (`rec`) of that kind. NULL: no names.
     * queue_timestamp_frequency: ticks per second of queue `q` (NULL: caps.timestamp_frequency).
     * queue_timestamp_now: the GPU timestamp of queue `q` now, for profiler calibration (NULL or
     * false: unknown). */
    void (*object_name)(de_gpu_device *d, de__gpu_kind kind, void *rec, const char *name);
    u64 (*queue_timestamp_frequency)(de_gpu_device *d, de_gpu_queue q);
    bool (*queue_timestamp_now)(de_gpu_device *d, de_gpu_queue q, u64 *ticks);

    /* GPU crash diagnostics (DENG-328, breadcrumbs.c). device_lost: true once the backend knows
     * the device is lost or faulted, also when no call has returned DE_ERR_DEVICE_LOST yet (a
     * Metal command buffer that failed); polled by submits, fence waits, cmd_begin and the report
     * calls, so it must be cheap. NULL: only DE_ERR_DEVICE_LOST results count.
     * device_lost_report: appends the backend's own fault data (DRED, VK_EXT_device_fault, Metal
     * command-buffer errors) as text to `out` (`cap` bytes, NUL-terminated); returns the length
     * written. Called once, on the thread that saw the loss, never from a crash handler. NULL:
     * no backend section. */
    bool (*device_lost)(de_gpu_device *d);
    size_t (*device_lost_report)(de_gpu_device *d, char *out, size_t cap);
} de__gpu_backend_vtable;

/** Backends compiled into the build. Each backend task adds its declaration here. */
extern const de__gpu_backend_vtable de__gpu_null_vtable;
/** Vulkan 1.3 (src/gpu/vulkan/, DENG-283), compiled with DE_HAS_GPU_VULKAN. */
extern const de__gpu_backend_vtable de__gpu_vulkan_vtable;
/** Metal 3 (src/gpu/metal/, DENG-287); built when DE_HAS_GPU_METAL. */
extern const de__gpu_backend_vtable de__gpu_metal_vtable;
#if DE_HAS_GPU_D3D12
extern const de__gpu_backend_vtable de__gpu_d3d12_vtable; /* d3d12/d3d12_device.c, DENG-285 */
#endif

/**
 * Null backend test hooks (null/null_backend.c). With manual completion on, a queue's internal
 * timeline completes only up to the value given to de__gpu_null_complete (or everything on
 * de_gpu_wait_idle), so tests can hold deferred destruction back. User fences still signal at
 * submit. No effect on other backends' devices.
 */
void de__gpu_null_set_manual_completion(de_gpu_device *d, bool manual);
void de__gpu_null_complete(de_gpu_device *d, de_gpu_queue q, u64 value);
/** Null backend heap counters (DENG-282): heaps alive, their bytes, heaps ever created. */
void de__gpu_null_heap_counts(de_gpu_device *d, u32 *live, u64 *bytes, u32 *created);
/** Where the null backend placed a buffer or texture (de__gpu_kind DE__GPU_BUFFER or
 * DE__GPU_TEXTURE, handle id): false when it is not a placed resource of a null device. */
bool de__gpu_null_placement(de_gpu_device *d, de__gpu_kind kind, u32 id, de__gpu_heap **heap,
                            u64 *offset);

/*
 * Null backend command log (DENG-172; DENG-173 extends it: add ops at the end, append-only). With
 * the log on, the null device records one entry per recorded command and per submit, in the order
 * the calls reach the backend (a device-wide order under a lock; one list is recorded by one
 * thread, so each list's entries are in its recording order). Tests compare it with golden lists.
 * Recording calls that the front-end refuses (Debug/Dev validation) never reach the log.
 */
typedef enum de__gpu_null_op {
    DE__GPU_NULL_OP_BARRIERS = 1, /**< de_gpu_cmd_barriers: the barriers are data[first, +count) */
    DE__GPU_NULL_OP_SUBMIT = 2,   /**< de_gpu_queue_submit: a[0] lists, a[1] waits (data), a[2]
                                       signal fence id, a[3] signal value */
} de__gpu_null_op;

typedef struct de__gpu_null_entry {
    u32 op;                      /**< de__gpu_null_op */
    de_gpu_queue queue;          /**< the list's queue / the submit's queue */
    char list[DE__GPU_NAME_MAX]; /**< the command list's name (submit: its first list's) */
    u64 a[4];                    /**< op arguments */
    u32 data_first, data_count;  /**< barriers (OP_BARRIERS) or waits (OP_SUBMIT) */
} de__gpu_null_entry;

/** Turns the log on (clearing it) or off (keeping the entries). No effect on other backends. */
void de__gpu_null_log_enable(de_gpu_device *d, bool on);
/** Number of entries, and entry `i` (NULL when out of range). Read while nothing records. */
u32 de__gpu_null_log_count(de_gpu_device *d);
const de__gpu_null_entry *de__gpu_null_log_entry(de_gpu_device *d, u32 i);
/** The barriers of an OP_BARRIERS entry / the waits of an OP_SUBMIT entry (NULL otherwise). */
const de_gpu_barrier *de__gpu_null_log_barriers(de_gpu_device *d, const de__gpu_null_entry *e);
const de_gpu_wait *de__gpu_null_log_waits(de_gpu_device *d, const de__gpu_null_entry *e);

/**
 * Overrides the table entry of `backend` (not DE_GPU_DEFAULT) at run time; NULL restores the
 * built-in entry. For tests and out-of-tree backends; call before creating devices.
 */
void de__gpu_register_backend(de_gpu_backend backend, const de__gpu_backend_vtable *vt);
/** The vtable used for `backend` (DE_GPU_DEFAULT resolved), or NULL when none is available. */
const de__gpu_backend_vtable *de__gpu_find_backend(de_gpu_backend backend);

/* ---------- Bindless index allocation (bindless_alloc.c) ---------- */

typedef struct de__gpu_index_space {
    u32 capacity; /**< end of the range: indices first .. capacity - 1 are handed out */
    u32 next;     /**< next never-used index */
    u32 first;    /**< first index of the range: 1, or where a shared numbering puts it */
    u32 *free;    /**< LIFO stack of released indices */
    u32 free_count, free_cap;
    u32 live;
} de__gpu_index_space;

typedef struct de__gpu_bindless {
    const de_allocator *alloc;
    de_mutex lock;
    de__gpu_index_space spaces[DE__GPU_BINDLESS_SPACE_COUNT];
} de__gpu_bindless;

/** Table sizes from the caps (max_bindless_*). */
void de__gpu_bindless_init(de__gpu_bindless *b, const de_allocator *alloc, const de_gpu_caps *caps);
/**
 * One numbering for the resource spaces (vtable `bindless_shared_resource_heap`, DENG-285), right
 * after init: sampled textures take 1 .. T-1, storage textures T .. T+S-1, buffers the next B
 * indices, acceleration structures the next A (T, S, B, A = the caps' max_bindless_* sizes), so
 * the whole numbering needs T + S + B + A descriptor slots with slot 0 reserved. Samplers are
 * unchanged.
 */
void de__gpu_bindless_share_resource_numbering(de__gpu_bindless *b);
void de__gpu_bindless_destroy(de__gpu_bindless *b);
/** A free index of `space`, or DE_GPU_BINDLESS_INVALID with DE_ERR_OUT_OF_MEMORY when full. */
u32 de__gpu_bindless_alloc(de__gpu_bindless *b, de__gpu_bindless_space space);
/** Returns an index. Only call it once the GPU is done with it (deferred destruction). */
void de__gpu_bindless_free(de__gpu_bindless *b, de__gpu_bindless_space space, u32 index);
/** Indices of `space` handed out and not freed. */
u32 de__gpu_bindless_live(de__gpu_bindless *b, de__gpu_bindless_space space);

/* ---------- Deferred destruction (deferred_destroy.c) ---------- */

typedef struct de__gpu_deferred_entry {
    u64 values[DE_QUEUE_COUNT_]; /**< per-queue internal timeline values last submitted */
    u32 kind;                    /**< de__gpu_kind */
    u32 index;                   /**< pool slot */
} de__gpu_deferred_entry;

/** FIFO of retired objects. Values only grow, so the head is always the first to complete. */
typedef struct de__gpu_deferred {
    const de_allocator *alloc;
    de_mutex lock;
    de__gpu_deferred_entry *ring;
    u32 head, count, cap;
} de__gpu_deferred;

typedef void (*de__gpu_reclaim_fn)(void *user, u32 kind, u32 index);

void de__gpu_deferred_init(de__gpu_deferred *q, const de_allocator *alloc);
void de__gpu_deferred_destroy(de__gpu_deferred *q);
/** Queues a retired object. DE_ERR_OUT_OF_MEMORY when the ring cannot grow. */
de_result de__gpu_deferred_push(de__gpu_deferred *q, u32 kind, u32 index,
                                const u64 values[DE_QUEUE_COUNT_]);
/**
 * Drops every entry whose values are all <= `completed` and calls `fn` for it, in FIFO order,
 * outside the queue's lock (so `fn` may push). Returns the number reclaimed. `completed` NULL
 * reclaims everything.
 */
u32 de__gpu_deferred_collect(de__gpu_deferred *q, const u64 completed[DE_QUEUE_COUNT_],
                             de__gpu_reclaim_fn fn, void *user);
u32 de__gpu_deferred_pending(de__gpu_deferred *q);

/* ---------- The device (front-end part) ---------- */

struct de_gpu_device {
    const de__gpu_backend_vtable *vt;
    const de_allocator *alloc;
    de_gpu_device_desc desc;
    de_gpu_caps caps;
    de_log_category log;
    de__gpu_pool pools[DE__GPU_KIND_COUNT];
    de__gpu_bindless bindless;
    de__gpu_deferred deferred;
    de_mutex submit_lock[DE_QUEUE_COUNT_];
    de_atomic_u64 submitted[DE_QUEUE_COUNT_]; /**< internal timeline value of the last submit */
    de_mutex cmd_lock;
    de_gpu_cmd *cmd_free[DE_QUEUE_COUNT_];
    de_gpu_cmd *cmd_inflight_head[DE_QUEUE_COUNT_], *cmd_inflight_tail[DE_QUEUE_COUNT_];
    de_gpu_cmd *cmd_all;
    size_t cmd_bytes;
    size_t bytes;                /**< allocation size of this struct plus the backend payload */
    struct de__gpu_heaps *heaps; /**< device memory heaps (gpu/memory/gpu_heap.c, DENG-282) */
    /** DENG-326 (query.c): the profiler's GPU context of each queue: its id + 1 (0 = none) and
     * the profiler span it was made in. Under the queue's submit_lock. */
    u32 prof_gpu_context[DE_QUEUE_COUNT_];
    u32 prof_gpu_span[DE_QUEUE_COUNT_];
    /** DENG-328 (breadcrumbs.c): set once the device is lost (every later call fails fast), and
     * the crash diagnostics state (report, breadcrumb rings, names, callback). */
    de_atomic_u32 lost;
    struct de__gpu_crash *crash;
};

static inline void *de__gpu_device_payload(de_gpu_device *d) {
    return (u8 *)d + DE__GPU_ALIGN16(sizeof(struct de_gpu_device));
}

/* Handle -> record, NULL for invalid or stale handles (no error set): for backends. */
de__gpu_buffer *de__gpu_get_buffer(de_gpu_device *d, de_gpu_buffer h);
de__gpu_texture *de__gpu_get_texture(de_gpu_device *d, de_gpu_texture h);
de__gpu_sampler *de__gpu_get_sampler(de_gpu_device *d, de_gpu_sampler h);
de__gpu_pipeline *de__gpu_get_pipeline(de_gpu_device *d, de_gpu_pipeline h);
de__gpu_accel *de__gpu_get_accel(de_gpu_device *d, de_gpu_accel h);
de__gpu_fence *de__gpu_get_fence(de_gpu_device *d, de_gpu_fence h);
de__gpu_query_heap *de__gpu_get_query_heap(de_gpu_device *d, de_gpu_query_heap h);

/**
 * Creates a texture record (handle + bindless indices) for an image the backend owns, without
 * calling texture_create: swapchain images. The backend fills the payload, then calls
 * de__gpu_texture_wrap_finish to write the bindless descriptors. texture_destroy still runs for
 * it (the record has `external` set).
 */
de_gpu_texture de__gpu_texture_wrap(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                    de__gpu_texture **out);
void de__gpu_texture_wrap_finish(de_gpu_device *d, de__gpu_texture *t);

/**
 * Creates a buffer / texture placed at (`heap`, `offset`) through buffer_create_placed /
 * texture_create_placed, with the record's `region` set: de_gpu_buffer_create's path otherwise
 * (validation, slot, bindless). For gpu_heap.c's de__gpu_place_transient_*; it has checked the
 * requirements and takes the region reference on success.
 */
de_gpu_buffer de__gpu_buffer_create_placed(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                           de__gpu_heap *heap, u64 offset,
                                           de__gpu_transient_region *region);
de_gpu_texture de__gpu_texture_create_placed(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                             de__gpu_heap *heap, u64 offset,
                                             de__gpu_transient_region *region);

/** Releases deferred destructions and command lists whose work completed. Returns the number of
 * objects released. */
u32 de__gpu_collect(de_gpu_device *d);
/** Objects waiting for the GPU before they are released. */
u32 de__gpu_deferred_count(de_gpu_device *d);
/** Live objects of one kind (created, not destroyed). */
u32 de__gpu_live_count(de_gpu_device *d, de__gpu_kind kind);
/** Indices handed out in one bindless space. */
u32 de__gpu_bindless_count(de_gpu_device *d, de__gpu_bindless_space space);

/* ---------- Debug markers, names, timestamps (query.c, DENG-326) ---------- */

/** Records a pushed marker (front-end stack) after c->marker_depth was incremented. */
void de__gpu_cmd_marker_record(de_gpu_cmd *c, const char *name, u32 color);
/** Drops the innermost recorded marker, after c->marker_depth was decremented. */
void de__gpu_cmd_marker_forget(de_gpu_cmd *c);
/** Open markers of a list being recorded (c->marker_depth, also those too deep to keep). */
u32 de__gpu_cmd_marker_depth(const de_gpu_cmd *c);
/** Name of open marker `level` (0 = outermost); "" past the levels kept or the depth. */
const char *de__gpu_cmd_marker_name(const de_gpu_cmd *c, u32 level);
/** Colour (0xRRGGBB) of open marker `level`; 0 past the levels kept. */
u32 de__gpu_cmd_marker_color(const de_gpu_cmd *c, u32 level);
/** The open markers as "outer/inner/innermost" (cut to `cap`, always terminated); returns the
 * length written. For crash reports (DENG-328). */
size_t de__gpu_cmd_marker_path(const de_gpu_cmd *c, char *out, size_t cap);
/** 1 in Debug and Dev builds: unnamed objects get "<kind>#<slot>" (record and native object). */
#define DE__GPU_DEFAULT_NAMES DE__GPU_VALIDATE_COMMANDS
/** Fills an empty record name with "<kind>#<slot>" (`id` is the handle id), in Debug and Dev. */
void de__gpu_default_name(char name[DE__GPU_NAME_MAX], de__gpu_kind kind, u32 id);
/** Calls the backend's object_name for a new object, when it has one and the name is not empty.
 * `name` NULL: the kind's default name in Debug/Dev, else nothing. */
void de__gpu_name_object(de_gpu_device *d, de__gpu_kind kind, u32 id, void *rec, const char *name);

/* ---------- Validation (validation.c) ---------- */

/** 1 in Debug and Dev builds: command recording is checked. Create descs are always checked. */
#if defined(DE_DEBUG) || defined(DE_DEV)
#define DE__GPU_VALIDATE_COMMANDS 1
#else
#define DE__GPU_VALIDATE_COMMANDS 0
#endif

/* Each check returns DE_OK, or fails with DE_ERR_INVALID_ARGUMENT (DE_ERR_UNSUPPORTED for
 * features the device lacks) and a message that names the function, the object and the field;
 * the message is also logged to the device's `gpu` category. */
de_result de__gpu_validate_buffer_desc(de_gpu_device *d, const de_gpu_buffer_desc *desc);
de_result de__gpu_validate_texture_desc(de_gpu_device *d, const de_gpu_texture_desc *desc);
de_result de__gpu_validate_sampler_desc(de_gpu_device *d, const de_gpu_sampler_desc *desc);
de_result de__gpu_validate_pipeline_desc(de_gpu_device *d, const de_gpu_pipeline_desc *desc);
de_result de__gpu_validate_query_heap_desc(de_gpu_device *d, const de_gpu_query_heap_desc *desc);
de_result de__gpu_validate_swapchain_desc(de_gpu_device *d, const de_gpu_swapchain_desc *desc);
de_result de__gpu_validate_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s);
/** Fails and logs: "<func>: <kind> handle 0x... is stale/invalid". */
de_result de__gpu_fail_handle(de_gpu_device *d, const char *func, de__gpu_kind kind, u32 id);
/** Fails with `r` and logs the formatted message to the device's `gpu` category. */
de_result de__gpu_fail(de_gpu_device *d, de_result r, const char *fmt, ...) DE_PRINTF_FMT(3, 4);

/** Format properties used by validation and backends. */
bool de__gpu_format_is_depth(de_gpu_format f);
bool de__gpu_format_is_compressed(de_gpu_format f);
bool de__gpu_format_is_srgb(de_gpu_format f);
/** Bytes per texel, or per 4x4 block for block-compressed formats; 0 for UNKNOWN. */
u32 de__gpu_format_block_bytes(de_gpu_format f);
const char *de__gpu_format_name(de_gpu_format f);
const char *de__gpu_kind_name(de__gpu_kind k);

/* ---------- GPU crash diagnostics (breadcrumbs.c, DENG-328) ---------- */

/** True once the device is lost (cheap: one atomic load). */
static inline bool de__gpu_is_lost(de_gpu_device *d) {
    return de_atomic_load_u32(&d->lost, DE_ATOMIC_ACQUIRE) != 0;
}
/** de__gpu_is_lost, or the backend's device_lost poll says so (then the loss is handled here, on
 * the calling thread, by de__gpu_device_lost(d, `where`)). */
bool de__gpu_poll_lost(de_gpu_device *d, const char *where);
/** Handles a loss seen by `where` (a function name): marks the device lost, builds the report,
 * sets the `gpu_fault` crash annotation and calls the callback. Only the first call does it. */
void de__gpu_device_lost(de_gpu_device *d, const char *where);
/** `r` unchanged; when it is DE_ERR_DEVICE_LOST, de__gpu_device_lost(d, where) first. */
de_result de__gpu_check_lost(de_gpu_device *d, de_result r, const char *where);
/** DE_ERR_DEVICE_LOST with the error context "<where>: the GPU device is lost" (no log line: a
 * lost device fails every call). */
de_result de__gpu_lost_error(const char *where);

#endif
