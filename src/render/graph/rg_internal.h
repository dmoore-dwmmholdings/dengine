/*
 * rg_internal.h: the render graph's internal representation (SPEC §12.3). Not installed.
 *
 * Include as "render/graph/rg_internal.h". Three tasks share this file, one after the other:
 *   - DENG-171 (builder): declarations, versions, dependency edges, validation, culling.
 *   - DENG-172 (compiler) appends the compiled-graph state (schedule, aliasing, barriers, cache).
 *   - DENG-173 (recorder) appends the execute state (parallel recording, timers, markers).
 *
 * Everything is index-addressed with u32 indices in declaration order, so the compiler can sort
 * stably without iterating a hash map. All per-frame storage comes from the arena given to
 * de__rg_begin (the frame arena in the renderer); the de_rg itself is the only heap block.
 *
 * Frame lifecycle (one thread):
 *     de__rg_begin(g, arena);         // new frame: handles of earlier frames become stale
 *     ... de_rg_add_pass / de_rg_read / de_rg_write ...
 *     de__rg_finalize(g);             // versions, validation, culling, edges, r.rg_dump
 *     ... compile (DENG-172), record + submit (DENG-173) via de__rg_execute_pass ...
 *     de__rg_reset(g);                // drops the frame's storage (the caller resets the arena)
 */
#ifndef DE_RENDER_RG_INTERNAL_H
#define DE_RENDER_RG_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "core/de_strbuf.h"
#include "gpu/memory/gpu_heap.h" /* DENG-172: transient regions (render -> gpu) */

/** "No index" in every u32 index field. */
#define DE__RG_NONE UINT32_MAX

/** Resource handle layout: index in the low 20 bits, the graph's frame generation (never 0) in
 * the high 12 bits, so a handle from an earlier frame is detected. */
#define DE__RG_INDEX_BITS    20u
#define DE__RG_INDEX_MASK    ((1u << DE__RG_INDEX_BITS) - 1u)
#define DE__RG_GEN_MASK      0xFFFu
#define DE__RG_MAX_RESOURCES (1u << DE__RG_INDEX_BITS)

/** 1 when declarations are checked (Debug and Dev). */
#if defined(DE_DEBUG) || defined(DE_DEV)
#define DE__RG_VALIDATE 1
#else
#define DE__RG_VALIDATE 0
#endif

/** Access bits that write / read. de_rg_read takes read bits and DE__RG_LOAD_BITS. */
#define DE__RG_WRITE_BITS                                                                          \
    (DE_ACCESS_SHADER_WRITE | DE_ACCESS_RENDER_TARGET | DE_ACCESS_DEPTH_WRITE |                    \
     DE_ACCESS_COPY_DST | DE_ACCESS_ACCEL_WRITE)
#define DE__RG_READ_BITS                                                                           \
    (DE_ACCESS_SHADER_READ | DE_ACCESS_DEPTH_READ | DE_ACCESS_INDIRECT | DE_ACCESS_COPY_SRC |      \
     DE_ACCESS_ACCEL_READ)
/** Write bits that de_rg_read also takes: a render target the pass loads or blends onto. */
#define DE__RG_LOAD_BITS  DE_ACCESS_RENDER_TARGET
#define DE__RG_ACCESS_ALL ((1u << 11) - 1u)

typedef enum de__rg_res_kind { DE__RG_TEXTURE, DE__RG_BUFFER } de__rg_res_kind;

/** de__rg_resource.flags */
enum {
    DE__RG_RES_IMPORTED = 1u << 0, /**< external; `texture`/`buffer` set at import */
    DE__RG_RES_EXPORTED = 1u << 1, /**< de_rg_export: an output, never aliased */
    DE__RG_RES_USED = 1u << 2,     /**< accessed by a pass that is not culled (finalize) */
};

/** A resolved subresource range: [mip_first, mip_end) × [layer_first, layer_end). Buffers and
 * whole-resource accesses of imports use [0, UINT32_MAX). */
typedef struct de__rg_range {
    u32 mip_first, mip_end, layer_first, layer_end;
} de__rg_range;

typedef struct de__rg_resource {
    de__rg_res_kind kind;
    u32 flags;               /**< DE__RG_RES_* */
    const char *name;        /**< arena copy; "" when unnamed */
    de_gpu_texture_desc tex; /**< transient textures: the desc (name = `name`) */
    de_gpu_buffer_desc buf;  /**< transient buffers: the desc (name = `name`) */
    /** Physical resource: imports at declaration; transients filled by the compiler (DENG-172)
     * before execution. de_rg_texture / de_rg_buffer return these. */
    de_gpu_texture texture;
    de_gpu_buffer buffer;
    u32 initial_access; /**< imports: `current_access`; transients: DE_ACCESS_NONE */
    u32 final_access;   /**< exports: the requested state; otherwise DE_ACCESS_NONE */
    u32 mip_count;      /**< transient textures: resolved counts; 0 = unknown (imports, buffers) */
    u32 layer_count;
    /* Filled by de__rg_finalize: */
    u32 access_first, access_last; /**< chain of de__rg_access.res_prev/res_next, pass order */
    u32 version_count;             /**< writes declared on it (versions 1..version_count) */
    u32 first_pass, last_pass;     /**< first/last pass that is not culled and accesses it */
} de__rg_resource;

typedef struct de__rg_access {
    u32 pass, resource;
    u32 access; /**< de_gpu_access bits */
    de__rg_range range;
    bool write;
    /** Finalize: a write's new version (1..), or the version a read sees (0 = the contents the
     * resource had at import; for a transient, nothing). Versions count writes of the whole
     * resource, whatever their range. */
    u32 version;
    u32 pass_next;          /**< next access of the same pass, declaration order */
    u32 res_prev, res_next; /**< finalize: chain over the resource, pass order, reads first */
} de__rg_access;

/** Why `to` must run after `from`. */
typedef enum de__rg_edge_kind {
    DE__RG_EDGE_RAW, /**< `to` reads what `from` wrote */
    DE__RG_EDGE_WAR, /**< `to` overwrites what `from` read */
    DE__RG_EDGE_WAW, /**< `to` overwrites what `from` wrote */
} de__rg_edge_kind;

typedef struct de__rg_edge {
    u32 from, to; /**< pass indices, from < to */
    u32 resource;
    de__rg_edge_kind kind;
} de__rg_edge;

struct de_rg_pass {
    de_rg *graph;
    const char *name; /**< arena copy */
    de_gpu_queue queue;
    de_rg_execute_fn fn;
    void *user;
    u32 index;
    u32 access_first, access_last; /**< list through de__rg_access.pass_next */
    u32 access_count;
    /** Finalize: this pass's incoming edges are edges[edge_first .. edge_first + edge_count),
     * sorted by `from`, no duplicates of (from, resource, kind). Empty for culled passes. */
    u32 edge_first, edge_count;
    bool side_effect;
    bool culled;    /**< finalize: never executes */
    bool executing; /**< inside its execute callback (de__rg_execute_pass) */
};

/** de__rg_create settings. `(de__rg_desc){0}` is valid. */
typedef struct de__rg_desc {
    const de_allocator *allocator; /**< the de_rg block; NULL = de_heap_allocator() */
    de_gpu_device *device;         /**< for the compiler (DENG-172); the builder does not use it */
    const char *name;              /**< graph name in the dump ("frame" when NULL); copied */
} de__rg_desc;

struct de_rg {
    const de_allocator *alloc;
    de_gpu_device *device;
    char name[48];
    de_log_category log;
    de_cvar dump_cvar; /**< r.rg_dump (Dev-only; invalid in Shipping) */

    de_arena *arena; /**< this frame's storage; NULL outside begin .. reset */
    u32 gen;         /**< frame generation in resource handles, 1..4095 */
    bool open;       /**< between begin and finalize: declarations allowed */
    bool finalized;
    bool oom;        /**< an arena push failed this frame */
    u32 error_count; /**< declaration and finalize errors this frame */

    de_rg_pass **passes; /**< pass structs never move (each is its own arena block) */
    u32 pass_count, pass_cap;
    de__rg_resource *resources;
    u32 resource_count, resource_cap;
    de__rg_access *accesses;
    u32 access_count, access_cap;
    de__rg_edge *edges; /**< grouped by `to` in pass order */
    u32 edge_count, edge_cap;
    u32 culled_count;

    /** Largest counts of earlier frames: begin reserves this much, so a steady frame does one
     * arena push per array and never copies. */
    u32 hint_passes, hint_resources, hint_accesses, hint_edges;

    /* ---- DENG-172 compiled-graph state goes below this line ---- */
    struct de__rg_plan **cache; /**< compiled plans, at most r.rg_cache_size (heap) */
    u32 cache_count, cache_cap;
    u64 cache_clock;          /**< LRU stamps */
    de_cvar cache_size_cvar;  /**< r.rg_cache_size */
    struct de__rg_plan *plan; /**< this frame's plan (de__rg_compile); NULL otherwise */
    de_gpu_barrier *phys;     /**< this frame's barrier table with physical handles */
    bool no_async_compute;    /**< de__rg_set_async_compute(g, false) */
    u64 stat_compiles, stat_hits, stat_evictions;
    de__gpu_transient_region *pool[DE__GPU_MEM_CLASS_MAX]; /**< transient region per class */
    u64 pool_bytes[DE__GPU_MEM_CLASS_MAX];
    u32 pool_gen; /**< bumped when a region is replaced: plans place their resources again */

    /* ---- DENG-173 execute state goes below this line ---- */
};

/* ---------- Lifecycle (rg_builder.c) ---------- */

/** Creates a graph. Registers the `render` log category and the r.rg_dump cvar (Dev). */
de_result de__rg_create(const de__rg_desc *desc, de_rg **out);
/** Destroys a graph (NULL is ignored). The arena's memory belongs to the caller. */
void de__rg_destroy(de_rg *g);
/** Starts a frame on `arena` (not NULL), which must stay valid until de__rg_reset. Resets an
 * open frame first. Bumps the handle generation. */
de_result de__rg_begin(de_rg *g, de_arena *arena);
/**
 * Ends declaration: builds the per-resource access chains and versions, runs the finalize checks
 * (Debug/Dev: reads of never-written transients, read bits in writes without a matching read),
 * culls, computes the incoming edges of every live pass and the live range of every resource,
 * and writes the graph to the r.rg_dump path if it is set.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT when any declaration or check failed this frame (the
 *         error context wraps the last failure); DE_ERR_OUT_OF_MEMORY when the arena ran out;
 *         DE_ERR_BUSY when not between begin and finalize.
 */
de_result de__rg_finalize(de_rg *g);
/** Ends the frame: remembers the frame's sizes as hints and forgets its storage. The caller
 * resets or reuses the arena afterwards. */
void de__rg_reset(de_rg *g);

/** Runs pass `pass`'s execute callback with `cmd` (de_rg_texture / de_rg_buffer work inside it).
 * Culled passes and passes without a callback do nothing. Different passes may run on different
 * threads at once. */
void de__rg_execute_pass(de_rg *g, u32 pass, de_gpu_cmd *cmd);

/* ---------- Queries (rg_builder.c) ---------- */

/** Index of a resource handle of this frame, or DE__RG_NONE (invalid or stale). */
u32 de__rg_resource_index(const de_rg *g, de_rg_resource r);
/** Handle of resource index `i` in this frame. */
de_rg_resource de__rg_resource_handle(const de_rg *g, u32 i);
/** True if the two ranges share a subresource. */
bool de__rg_range_overlaps(de__rg_range a, de__rg_range b);
/** True if `outer` contains every subresource of `inner`. */
bool de__rg_range_covers(de__rg_range outer, de__rg_range inner);

/* ---------- Debug (rg_debug.c) ---------- */

/** "graphics", "compute", "copy" or "?". */
const char *de__rg_queue_name(de_gpu_queue q);
/** Writes `access` as "SHADER_READ|COPY_SRC" ("NONE" for 0) into `buf` and returns it. */
const char *de__rg_access_str(u32 access, char *buf, size_t cap);
/**
 * Appends the frame graph as Graphviz: one box per pass (culled ones grey and dashed), one
 * ellipse per resource version, edges for every access labelled with the access bits. Call after
 * de__rg_finalize. Allocates only through `out`.
 */
de_result de__rg_dump_dot(const de_rg *g, de_strbuf *out);
/** Writes the dot dump to the virtual path `vpath` (de_file_write_all_atomic). */
de_result de__rg_dump_dot_file(const de_rg *g, const char *vpath);
/** If r.rg_dump is set: writes the dump there, logs it and clears the cvar. Called by
 * de__rg_finalize. */
void de__rg_dump_if_requested(de_rg *g);

/* =================================================================================================
 * DENG-172: the compiled graph (SPEC §12.3: schedule, async compute, transient aliasing,
 * barriers, compiled-graph cache). Files: rg_compile.c (driver, binding, recording helpers),
 * rg_schedule.c (steps, batches, fences), rg_barriers.c (state tracking, barriers, transfers),
 * rg_alias.c (memory requirements, aliasing, the transient pool), rg_cache.c (key, LRU cache).
 *
 * Per frame, after de__rg_finalize:
 *     de__rg_compile(g);          // cache hit: rebind; miss: compile and cache the plan
 *     for each batch b of g->plan (in order):
 *         cmd = de_gpu_cmd_begin(dev, b.queue, ...);
 *         for each step s of b: de__rg_record_barriers(g, s, DE__RG_PRE, cmd);
 *                               de__rg_execute_pass(g, step.pass, cmd)   (if step.pass is a pass)
 *                               de__rg_record_barriers(g, s, DE__RG_POST, cmd);
 *         submit after b's waits, signalling b.signal (DENG-173 maps signal numbers to values)
 *
 * The plan refers to resources and passes by index; physical handles are bound per frame (the
 * per-frame table g->phys is on the frame arena), so a plan is reused while the declarations are
 * the same. Plans live on the graph's allocator, not the frame arena.
 * ============================================================================================== */

/** Barrier slots of a step: recorded before the pass (transitions and acquires) or after it
 * (releases of queue-ownership transfers). */
enum { DE__RG_PRE = 0, DE__RG_POST = 1 };

/** An abstract barrier of a plan. Ranges as in de_gpu_barrier (count 0 = to the last). */
typedef struct de__rg_cbarrier {
    u32 resource; /**< resource index */
    u32 before, after;
    u32 mip_first, mip_count, layer_first, layer_count;
    de_gpu_queue src_queue, dst_queue; /**< differ for an ownership transfer (both halves) */
} de__rg_cbarrier;

/**
 * One step of the schedule. Step 0 is the prologue and the last step the epilogue: both run on
 * the graphics queue and have no pass, only barriers (imports handed to other queues at the
 * start; final states and imports returned to graphics at the end). Steps 1 .. n are the live
 * passes in declaration order, which is a topological order (every edge has from < to).
 */
typedef struct de__rg_step {
    u32 pass;           /**< pass index; DE__RG_NONE for the prologue and the epilogue */
    de_gpu_queue queue; /**< effective queue (compute folds into graphics without async compute) */
    u32 batch;          /**< the batch that records it */
    u32 barrier_first[2], barrier_count[2]; /**< [DE__RG_PRE], [DE__RG_POST] into barriers */
} de__rg_step;

/** A wait of a batch: `signal` n >= 1 is the n-th signal of `queue` in this frame; 0 is the last
 * value `queue` signalled before this frame (the frame-entry join). */
typedef struct de__rg_wait {
    de_gpu_queue queue;
    u32 signal;
} de__rg_wait;

/** One submission: its steps are recorded in order into command lists of `queue` and submitted
 * after `waits`; when they complete, the queue's `signal`-th signal of the frame is set (0 = no
 * signal). Batches are listed in a valid submission order (by their first step). */
typedef struct de__rg_batch {
    de_gpu_queue queue;
    u32 step_first, step_count; /**< into batch_steps */
    u32 wait_first, wait_count; /**< into waits */
    u32 signal;
} de__rg_batch;

/** Where a transient lives: `size` bytes at `offset` of the pool region of `mem_class`. */
typedef struct de__rg_transient {
    u32 resource;
    u32 mem_class;
    u64 offset, size, align;
    bool buffer;  /**< a buffer (never aliased); else a texture */
    bool aliased; /**< shares memory with another transient of the plan */
} de__rg_transient;

/** A compiled graph. One heap block (`block_size` bytes) holds the struct and every array. */
typedef struct de__rg_plan {
    u64 hash; /**< of `key` */
    u8 *key;  /**< the declarations the plan was compiled from (rg_cache.c) */
    u32 key_size;
    u64 last_use; /**< LRU stamp */
    size_t block_size;
    bool async_compute; /**< compute passes run on DE_QUEUE_COMPUTE */
    u32 pass_count, resource_count;
    de__rg_step *steps;
    u32 step_count;
    u32 *step_of_pass; /**< [pass_count]: the pass's step, DE__RG_NONE when culled */
    de__rg_batch *batches;
    u32 batch_count;
    u32 *batch_steps;
    de__rg_wait *waits;
    u32 wait_count;
    u32 signal_count[DE_QUEUE_COUNT_]; /**< signals per queue in a frame */
    de__rg_cbarrier *barriers;         /**< grouped by step, then slot */
    u32 barrier_count;
    u32 *end_access; /**< [resource_count]: state at the end of the frame (imports: pass it to
                          the next frame's import; NONE for transients that are not exported) */
    de__rg_transient *transients;
    u32 transient_count;
    u32 *transient_of; /**< [resource_count]: index into transients, or DE__RG_NONE */
    u64 class_bytes[DE__GPU_MEM_CLASS_MAX]; /**< pool bytes the plan needs per memory class */
    u32 *placed;    /**< [transient_count]: placed texture / buffer ids (0 = not placed) */
    u32 placed_gen; /**< g->pool_gen they were placed in; 0 = none */
} de__rg_plan;

/** Compile counters of a graph (de__rg_compile_stats). */
typedef struct de__rg_stats {
    u64 compiles;  /**< cache misses: plans compiled */
    u64 hits;      /**< frames that reused a cached plan */
    u64 evictions; /**< plans dropped by the LRU cap */
    u32 cached;    /**< plans in the cache now */
} de__rg_stats;

/* ---------- Public-to-the-renderer entry points (rg_compile.c) ---------- */

/**
 * Compiles the finalized frame (or reuses the cached plan of the same declarations), places the
 * transients and binds this frame's physical resources: after it, de_rg_texture / de_rg_buffer
 * return the placed transients and g->plan / g->phys describe the frame. Needs g->device when
 * any transient is used. No heap allocation on a cache hit once the pool is large enough.
 * @return DE_OK; DE_ERR_BUSY (not finalized); DE_ERR_INVALID_ARGUMENT (no device, finalize
 *         failed); DE_ERR_OUT_OF_MEMORY; errors of the transient placement.
 */
de_result de__rg_compile(de_rg *g);
/** Records the barriers of step `step`, slot DE__RG_PRE or DE__RG_POST, into `cmd` (one
 * de_gpu_cmd_barriers call; nothing when there are none). Thread-safe for different lists. */
void de__rg_record_barriers(const de_rg *g, u32 step, u32 slot, de_gpu_cmd *cmd);
/** The access resource `r` is left in at the end of the compiled frame (imports: pass it as
 * `current_access` of the next frame's import). DE_ACCESS_NONE before de__rg_compile. */
u32 de__rg_end_access(const de_rg *g, de_rg_resource r);
/** Lets compute passes use the async compute queue when the device has one (default true). */
void de__rg_set_async_compute(de_rg *g, bool allow);
de__rg_stats de__rg_compile_stats(const de_rg *g);
/** Drops every cached plan and the transient pool (deferred destruction keeps in-flight memory
 * alive). Called by de__rg_destroy; also when the device changes. */
void de__rg_compile_shutdown(de_rg *g);
/** Called by de__rg_reset: the frame's plan binding ends. */
void de__rg_compile_frame_end(de_rg *g);

/* ---------- Compiler internals (shared by the DENG-172 files) ---------- */

/** A dependency between two steps on different queues: `to` waits for `from`. */
typedef struct de__rg_dep {
    u32 from, to;
} de__rg_dep;

/** A barrier while compiling: the plan barrier plus where it is recorded. */
typedef struct de__rg_tbarrier {
    de__rg_cbarrier b;
    u32 step, slot;
} de__rg_tbarrier;

/** Scratch state of one compilation, on the frame arena. Becomes a de__rg_plan (rg_cache.c). */
typedef struct de__rg_cbuild {
    de_rg *g;
    de_arena *arena;
    bool async_compute;
    bool oom;
    de__rg_step *steps;
    u32 step_count;
    u32 *step_of_pass;
    de__rg_tbarrier *tb;
    u32 tb_count, tb_cap;
    de__rg_dep *deps;
    u32 dep_count, dep_cap;
    u32 *end_access;
    de__rg_batch *batches;
    u32 batch_count;
    u32 *batch_steps;
    de__rg_wait *waits;
    u32 wait_count;
    u32 signal_count[DE_QUEUE_COUNT_];
    u32 *clock_in; /**< [step_count * DE_QUEUE_COUNT_]: per queue, 1 + the last step known done
                        when the step starts (0 = none) */
    de__rg_transient *transients;
    u32 transient_count;
    u32 *transient_of;
    u64 class_bytes[DE__GPU_MEM_CLASS_MAX];
} de__rg_cbuild;

/** Arena array growth for the compiler (sets b->oom on failure). */
void *de__rg_cgrow(de__rg_cbuild *b, void *old, u32 count, u32 *cap, u32 need, size_t elem);
/** rg_schedule.c: steps (prologue, live passes, epilogue) and their effective queues. */
bool de__rg_schedule_steps(de__rg_cbuild *b);
/** rg_barriers.c: state tracking, barriers, transfers and cross-queue dependencies. */
bool de__rg_plan_barriers(de__rg_cbuild *b);
/** rg_schedule.c: batches, waits and signals from the dependencies; fills clock_in. */
bool de__rg_schedule_batches(de__rg_cbuild *b);
/** rg_alias.c: requirements, memory classes and offsets of the used transients. */
de_result de__rg_alias_transients(de__rg_cbuild *b);
/** rg_alias.c: true if step `a` completes before step `s` starts (same queue: a < s, and a
 * first-use barrier from NONE waits for earlier work; other queues: through the waits). */
bool de__rg_step_before(const de__rg_cbuild *b, u32 a, u32 s);
/** rg_alias.c: grows the pool to the plan's needs and places its transients if needed. */
de_result de__rg_pool_bind(de_rg *g, de__rg_plan *plan);
/** rg_alias.c: destroys the plan's placed resources. */
void de__rg_plan_unplace(de_rg *g, de__rg_plan *plan);
/** rg_alias.c: releases the pool regions. */
void de__rg_pool_release(de_rg *g);

/** rg_cache.c: builds the cache key of the finalized frame on the arena. */
bool de__rg_cache_key(de_rg *g, bool async_compute, u8 **key, u32 *size, u64 *hash);
/** rg_cache.c: the cached plan with this key (its LRU stamp refreshed), or NULL. */
de__rg_plan *de__rg_cache_find(de_rg *g, const u8 *key, u32 size, u64 hash);
/** rg_cache.c: copies a compiled build into one heap block. NULL on out of memory. */
de__rg_plan *de__rg_plan_create(de_rg *g, const de__rg_cbuild *b, const u8 *key, u32 key_size,
                                u64 hash);
/** rg_cache.c: inserts a plan, evicting the least recently used beyond r.rg_cache_size. */
de_result de__rg_cache_insert(de_rg *g, de__rg_plan *plan);
/** rg_cache.c: frees a plan (and destroys its placed resources). */
void de__rg_plan_destroy(de_rg *g, de__rg_plan *plan);
/** rg_cache.c: the cache capacity from r.rg_cache_size (registers it on first use). */
u32 de__rg_cache_limit(de_rg *g);

#endif /* DE_RENDER_RG_INTERNAL_H */
