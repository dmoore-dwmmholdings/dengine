/*
 * render_world.h: the render world, the Extract systems and the render thread (SPEC §12.4, §8,
 * §10.2 step 5, §5.5; DENG-175). Internal to de_render; include as "render/world/render_world.h".
 *
 * The renderer never reads the main ECS world while it records. Once per frame, in the Extract
 * schedule, the systems that de_renderer_attach registers copy what rendering needs (cameras,
 * mesh renderers, lights, the environment) into one of `frames_in_flight + 1` render-world
 * buffers (de__render_world_frame). de__render_extract_and_kick then hands the buffer to the
 * render thread, which calls de_renderer_render_frame while the simulation runs the next frame.
 *
 * Files:
 *   render_world.c   the buffers, the slot pools, the render origin, the per-frame sync
 *   extract.c        the Extract systems (de_renderer_attach registers them)
 *   render_thread.c  the `render` thread, the handoff and de__render_extract_and_kick
 *
 * Render origin (DENG-2 addendum, SPEC §5.5). World positions are f64 (de_transform); every
 * transform in the render world is f32, relative to the render origin: an f64 position on a grid
 * of `r.origin_grid_m` metres (default 1024), the grid point nearest the main camera. The origin
 * moves (a rebase) only when the main camera leaves its cell, the cube of one grid step centred
 * on the origin, or when the grid size changes. The frame of a rebase has `origin_changed` set:
 * every transform and bound in it was recomputed, so the GPU scene (DENG-177) uploads them all.
 * The main camera is the first DeCamera (query order) whose `view` is zero, else the first
 * DeCamera; without one, the origin stays where it is.
 *
 * Slots. Mesh instances and lights live in slots that keep their index while the entity lives,
 * so the GPU scene can update them in place. The slot of an entity is found through a map
 * indexed by entity index and checked against the generation (never a hash map, never iterated
 * for output). New entities take free slots last-freed first, in query order (archetype, chunk,
 * row), so the slot numbering depends only on the world's operations. A frame lists the slots it
 * changed (`dirty_*`) and the slots it freed (`removed_*`); a freed slot is never reused in the
 * same frame. Changes come from DE_CHANGED queries (DeGlobalTransform, DeMeshRenderer, DeLight);
 * removals from a sweep: every slot whose entity was not seen by this frame's full query pass is
 * freed (despawn, or a component removed).
 *
 * Previous transforms. `prev_transform` is the transform of the frame before (motion vectors, S13).
 * An instance whose transform changed this frame has prev = last frame's transform; the frame
 * after, prev catches up (prev = current), so that frame lists it as dirty too. A new instance has
 * prev = current.
 *
 * Buffers and pipelining (SPEC §8). Frame k fills buffer k % (frames_in_flight + 1). Extract first
 * waits until the render thread has finished frame k - 1, so the simulation runs exactly one frame
 * ahead of rendering and never writes a buffer the render thread reads. A buffer is unchanged
 * until frame k + frames_in_flight + 1 is extracted, which is after de_renderer_render_frame of
 * frame k + frames_in_flight has waited for the GPU to finish frame k: GPU work may read a
 * buffer's arrays (persistently mapped memory) for as long as its frame is in flight. A buffer
 * copies only the slots changed since it was last filled.
 *
 * `r.pipelined` (bool, default true): false renders on the thread that calls
 * de__render_extract_and_kick, right after Extract (debugging, deterministic tests). Both modes
 * give the same render-world contents.
 *
 * Threads: attach, destroy and de__render_extract_and_kick on the main thread; the Extract
 * systems on any frame worker (not main-thread systems); de__render_world_current on the thread
 * inside de_renderer_render_frame.
 *
 * Memory: the pools and buffers grow (allocator of the renderer) when the number of instances,
 * lights or views grows past their capacity, and never shrink. A stable scene allocates nothing.
 */
#ifndef DE_RENDER_WORLD_RENDER_WORLD_H
#define DE_RENDER_WORLD_RENDER_WORLD_H

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "platform/de_atomic.h"
#include "render/components_internal.h"

DE_EXTERN_C_BEGIN

/** Materials per mesh instance (DeMeshRenderer.materials). */
#define DE__RW_MATERIALS 8u
/** Default of `r.origin_grid_m`, in metres. */
#define DE__RW_ORIGIN_GRID_DEFAULT 1024.0
/** Most render-world buffers (frames_in_flight + 1). */
#define DE__RW_MAX_FRAMES 4u
/** No index (de__render_world_frame.main_view without a camera). */
#define DE__RW_NONE 0xFFFFFFFFu

/** de__rw_instances.flags bits. */
enum {
    DE__RW_INST_CAST_SHADOWS = 1u << 0,  /**< DeMeshRenderer.cast_shadows */
    DE__RW_INST_VISIBLE_IN_RT = 1u << 1, /**< DeMeshRenderer.visible_in_rt */
    DE__RW_INST_SPRITE = 1u << 3,
    DE__RW_INST_HAS_BOUNDS = 1u << 2 /**< `bounds` holds the mesh's bounds (mesh loaded) */
};

/**
 * Mesh instances, SoA. Each array has `count` entries (slots below the high-water mark); a free
 * slot has entity 0 and is skipped by consumers.
 */
typedef struct de__rw_instances {
    u32 count; /**< slots, used or free */
    u32 live;  /**< slots holding an instance */
    u64 *entity;
    de_mesh *mesh;
    de_material *materials;    /**< DE__RW_MATERIALS per slot; zero = the mesh's default */
    de_affine *transform;      /**< model matrix (f32 3x4) relative to the render origin */
    de_affine *prev_transform; /**< the previous frame's (see the header) */
    de_aabb *bounds;           /**< world AABB relative to the origin; empty while the mesh is not
                                    loaded (no asset system, or not READY yet) */
    u32 *layer_mask;           /**< DeMeshRenderer.layer_mask */
    u8 *flags;                 /**< DE__RW_INST_* */
    DeSprite *sprite;          /**< S19: component snapshot for sprite instances. */
} de__rw_instances;

/** Lights, SoA, as de__rw_instances. The light points along its transform; the direction
 * convention is the lighting code's (S14). */
typedef struct de__rw_lights {
    u32 count;
    u32 live;
    u64 *entity;
    de_affine *transform; /**< relative to the render origin (position in column 3) */
    DeLight *light;       /**< the component as extracted */
} de__rw_lights;

/** One camera. */
typedef struct de__rw_view {
    u64 entity;
    DeCamera camera;     /**< the component as extracted */
    de_vec3 offset;      /**< camera position - render origin, f32 */
    de_quat rotation;    /**< world rotation */
    de_affine transform; /**< camera to render-origin space (offset in column 3) */
} de__rw_view;

typedef struct de__probe_snapshot {
    u64 entity;
    de_dvec3 position;
    de_quat rotation;
    DeReflectionProbe probe;
} de__probe_snapshot;

typedef struct de__irradiance_snapshot {
    u64 entity;
    de_dvec3 position;
    de_quat rotation;
    DeIrradianceVolume volume;
} de__irradiance_snapshot;

/** The environment: the first DeEnvironment in query order. */
typedef struct de__rw_environment {
    u64 entity; /**< 0: no DeEnvironment */
    DeEnvironment environment;
} de__rw_environment;

/** de__render_world_frame.state (checked by tests; documents the handoff). */
enum {
    DE__RW_FREE = 0, /**< nobody uses it */
    DE__RW_FILLING,  /**< Extract writes it (main thread and frame workers) */
    DE__RW_READY,    /**< filled, handed to the render thread */
    DE__RW_RENDERING /**< de_renderer_render_frame reads it */
};

/**
 * One render-world buffer: everything one frame renders. Read-only for the renderer. Arrays are
 * owned by the buffer and stay valid until the buffer is refilled (see the header).
 */
typedef struct de__fog_snapshot {
    u64 entity;
    de_affine transform;
    DeFogVolume volume;
} de__fog_snapshot;

typedef struct de__render_world_frame {
    u64 frame;           /**< extract frame number, from 1 */
    de_dvec3 origin;     /**< the render origin (f64 world position) */
    bool origin_changed; /**< rebased this frame (also the first frame): everything changed */

    u32 view_count;
    u32 main_view; /**< index into views, DE__RW_NONE without a camera */
    de__rw_view *views;

    de__rw_instances instances;
    de__rw_lights lights;
    de__rw_environment environment;
    u32 probe_count;
    de__probe_snapshot probes[64];
    u32 irradiance_count;
    de__irradiance_snapshot irradiance[8];
    u32 fog_count;
    de__fog_snapshot fog[32]; /* S17: bounded, allocation-free local participating regions. */
    u32 environment_count;    /**< DeEnvironment entities seen (only the first is used) */

    /** Slots written this frame (new, changed, prev catch-up), ascending. After an origin change
     * every live slot changed too; only the slots changed for other reasons are listed. */
    u32 dirty_instance_count;
    u32 *dirty_instances;
    u32 removed_instance_count; /**< slots freed this frame, ascending */
    u32 *removed_instances;
    u32 dirty_light_count;
    u32 *dirty_lights;
    u32 removed_light_count;
    u32 *removed_lights;

    /* Internal. */
    u32 view_capacity, inst_capacity, light_capacity;
    u64 inst_synced, light_synced; /**< the last frame whose slot changes it holds (0 = none) */
    de_atomic_u32 state;
} de__render_world_frame;

/* ---------- Slot pools (render_world.c) ---------- */

/** The extract side of one slot category (instances or lights): the authoritative copy of its
 * arrays plus the bookkeeping. */
typedef struct de__rw_pool {
    u32 capacity;
    de_transform *xform;      /**< f64 world transform per slot */
    de_transform *prev_xform; /**< the previous frame's */
    de_aabb *local_bounds;    /**< instances: the mesh's bounds (object space) */
    u64 *version;             /**< frame that last wrote the slot (including its freeing) */
    u64 *seen;                /**< frame that last saw the slot's entity */
    u64 *added;               /**< frame the slot was taken */
    u64 *dirty_at;            /**< frame the slot was last put on the dirty list */
    u32 *free;                /**< free slots, a stack */
    u32 free_count;
    u32 *dirty, dirty_count;
    u32 *removed, removed_count;
    u32 *moved, moved_count;           /**< slots whose transform changed this frame */
    u32 *moved_prev, moved_prev_count; /**< ... the frame before (their prev catches up) */
    /* entity index -> slot (DE__RW_NONE), with the generation it belongs to */
    u32 map_capacity;
    u32 *map_slot;
    u32 *map_gen;
} de__rw_pool;

/* ---------- The render thread (render_thread.c) ---------- */

/** Test hook: called on the thread that renders, right before de_renderer_render_frame. */
typedef void (*de__render_frame_hook)(de_renderer *r, const de__render_world_frame *f, void *user);

typedef struct de__render_thread {
    de_thread *thread; /**< NULL until the first pipelined frame */
    de_sem kick;       /**< main -> render: a frame is ready (or stop) */
    de_sem done;       /**< render -> main: the frame is rendered */
    bool sems_ok;
    bool in_flight;            /**< main thread: a kicked frame not waited for yet */
    de_atomic_ptr pending;     /**< the frame to render (release by main, acquire by render) */
    de_atomic_u32 stop;        /**< 1: the thread exits at its next wake-up */
    de_atomic_u64 rendered;    /**< frames finished by the render thread */
    de_atomic_u32 last_result; /**< de_result of the last frame it rendered */
} de__render_thread;

/* ---------- The render world of one renderer ---------- */

typedef struct de__render_world {
    de_renderer *r;
    const de_allocator *alloc;
    de_log_category log;
    de_world *world; /**< the attached world; the Extract systems of other worlds do nothing */
    DeRenderComponents ids;
    de_builtin_component_ids builtins;
    de_cvar cv_pipelined, cv_grid;

    /* Render origin (main thread and the views system). */
    de_dvec3 origin;
    f64 origin_grid;
    bool origin_valid;
    bool origin_changed;

    /* Buffers. */
    u32 frame_count; /**< frames_in_flight + 1 */
    de__render_world_frame frames[DE__RW_MAX_FRAMES];
    u64 frame;                          /**< the frame being filled, or the last one kicked */
    de__render_world_frame *filling;    /**< NULL between a kick and the next begin */
    const de__render_world_frame *last; /**< the last frame kicked */
    u64 fill_start_ns;

    /* Slots (the authoritative copies; buffers copy from them). */
    de__rw_instances inst;
    de__rw_pool inst_pool;
    de__rw_lights lights;
    de__rw_pool light_pool;
    u64 inst_frame, light_frame; /**< frame each category last extracted */

    /* The render thread and the frame being rendered. */
    de_system systems[3]; /**< views, meshes, lights in `world` */

    de__render_thread rt;
    const de__render_world_frame *current; /**< set by the thread that renders */
    de__render_frame_hook hook;
    void *hook_user;
} de__render_world;

/* ---------- renderer.c calls ---------- */

/**
 * de_renderer_attach's call: creates the renderer's render world (first call), registers the
 * cvars `r.pipelined` and `r.origin_grid_m`, and adds the Extract systems to `w` (once per
 * world; the components must be registered: de__render_components_attach runs first). A failure
 * is logged to the `render` category (attach returns nothing).
 */
void de__render_world_attach(de_renderer *r, de_world *w);
/** de_renderer_destroy's call, before the GPU wait: stops the render thread (after the frame it
 * renders) and frees the render world. Nothing when attach never ran. */
void de__render_world_destroy(de_renderer *r);

/* ---------- The host (src/host/loop.c, step 5) ---------- */

/**
 * After the Extract schedule of `w`: hands the filled render-world buffer to the render thread and
 * returns (`r.pipelined`), or renders it on the calling thread. When the Extract systems did not
 * run this frame (the renderer is attached to another world, or not at all), the frame renders
 * the last extracted state. Main thread.
 * @return DE_OK, or the error of the frame rendered here (unpipelined), or of the last frame the
 *         render thread finished (pipelined; its message is logged on the render thread).
 */
de_result de__render_extract_and_kick(de_renderer *r, de_world *w);

/* ---------- Consumers (DENG-176 views, DENG-177 GPU scene) ---------- */

/** The render-world buffer of the frame being rendered: valid inside de_renderer_render_frame,
 * on its thread. NULL when it is not called through de__render_extract_and_kick. */
const de__render_world_frame *de__render_world_current(const de_renderer *r);

/* ---------- Test hooks ---------- */

/** Sets the hook called before each de_renderer_render_frame of a kicked frame. Main thread,
 * while no frame is in flight (after de__render_world_wait_idle). */
void de__render_world_set_frame_hook(de_renderer *r, de__render_frame_hook fn, void *user);
/** Waits until the render thread has finished every kicked frame. Main thread. */
void de__render_world_wait_idle(de_renderer *r);
/** The last kicked buffer, or NULL. Main thread; its contents are stable after
 * de__render_world_wait_idle until the next Extract. */
const de__render_world_frame *de__render_world_last(const de_renderer *r);
/** Frames the render thread has finished (0 without a thread). Any thread. */
u64 de__render_world_rendered(const de_renderer *r);

/* ---------- Between the files of world/ ---------- */

/** The render world of `r` (NULL before attach). */
de__render_world *de__rw_of(const de_renderer *r);
/** Allocates the buffers and pools of a new render world. */
de_result de__rw_init(de__render_world *rw, de_renderer *r, u32 frames_in_flight);
/** Frees everything de__rw_init and the frames allocated. */
void de__rw_free(de__render_world *rw);
/** Creates the handoff semaphores (the thread starts with the first pipelined frame). */
de_result de__rt_init(de__render_world *rw);
/** Destroys the semaphores (after de__rt_stop). */
void de__rt_destroy(de__render_world *rw);
/** Waits for the frame in flight on the render thread, if any (main thread). */
void de__rt_wait(de__render_world *rw);
/** Stops and joins the render thread (after the frame it renders), if it runs. */
void de__rt_stop(de__render_world *rw);
/** Starts filling the next buffer: waits for the render thread to finish the frame before (only
 * when one is in flight), then resets the buffer's per-frame lists. Idempotent until the kick.
 * Returns the buffer. */
de__render_world_frame *de__rw_begin(de__render_world *rw);
/** Moves the render origin for a main camera at `cam` (`has_cam` false: it stays) and sets
 * rw->origin_changed for this frame. */
void de__rw_update_origin(de__render_world *rw, bool has_cam, de_dvec3 cam);
/** Grows the view array of `f` to hold `n` views. */
de_result de__rw_frame_reserve_views(de__render_world *rw, de__render_world_frame *f, u32 n);

/** Pool of instances (`is_light` false) or lights. */
typedef enum de__rw_kind { DE__RW_KIND_INSTANCE, DE__RW_KIND_LIGHT } de__rw_kind;
/** Starts a frame of a pool: clears its lists, rotates the moved lists, and lets the slots that
 * moved last frame catch up (prev = current; dirty). */
void de__rw_pool_begin(de__render_world *rw, de__rw_kind k);
/** The slot of entity `e`, or DE__RW_NONE. */
u32 de__rw_pool_find(const de__render_world *rw, de__rw_kind k, de_entity e);
/** Takes a slot for `e` (which has none). DE__RW_NONE when memory runs out (error set). */
u32 de__rw_pool_take(de__render_world *rw, de__rw_kind k, de_entity e);
/** Marks slot `s` written this frame (dirty list, version). */
void de__rw_pool_touch(de__render_world *rw, de__rw_kind k, u32 s);
/** Sets the world transform of slot `s`: prev = the old one unless `fresh`, then recomputes the
 * relative matrices (and bounds) and marks it moved and dirty. */
void de__rw_pool_set_xform(de__render_world *rw, de__rw_kind k, u32 s, const de_transform *t,
                           bool fresh);
/** Recomputes slot `s`'s instance bounds from its local bounds and current transform. */
void de__rw_inst_update_bounds(de__render_world *rw, u32 s);
/** Ends a frame of a pool: frees every slot not seen this frame (`sweep`), recomputes every
 * transform after a rebase, then copies the changes into `f`. */
de_result de__rw_pool_end(de__render_world *rw, de__rw_kind k, de__render_world_frame *f,
                          bool sweep);

DE_EXTERN_C_END

#endif /* DE_RENDER_WORLD_RENDER_WORLD_H */
