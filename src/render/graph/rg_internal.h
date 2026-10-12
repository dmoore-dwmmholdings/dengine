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

#endif /* DE_RENDER_RG_INTERNAL_H */
