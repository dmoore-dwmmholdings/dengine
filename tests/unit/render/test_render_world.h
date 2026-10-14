/*
 * test_render_world.h: the fixture shared by test_extract.c (which defines it) and
 * test_render_thread.c (DENG-175). A world (with or without a task system), a null-backend
 * renderer attached to it, and helpers that spawn cameras, mesh instances and lights and run one
 * frame the way the host loop does (PostUpdate, Extract, de__render_extract_and_kick).
 */
#ifndef DE_TEST_RENDER_WORLD_H
#define DE_TEST_RENDER_WORLD_H

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/components_internal.h"
#include "render/world/render_world.h"

typedef struct rwt {
    de_tracking_allocator track, dev_track;
    de_allocator alloc, dev_alloc;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *w;
    de_gpu_device *d;
    de_renderer *r;
    DeRenderComponents ids;
    de_builtin_component_ids bi;
} rwt;

/** Creates everything; `workers` 0: no task system (systems run serially). Sets `r.pipelined`. */
void rwt_start(rwt *f, u32 workers, bool pipelined);
/** Destroys everything (renderer first) and checks that nothing leaked. */
void rwt_stop(rwt *f);
/** One frame: PostUpdate, Extract, de__render_extract_and_kick. */
de_result rwt_frame(rwt *f);
/** Waits for the render thread, then the last kicked buffer. */
const de__render_world_frame *rwt_last(rwt *f);
de_entity rwt_spawn_mesh(rwt *f, de_transform t, u32 mesh_id);
de_entity rwt_spawn_light(rwt *f, de_transform t, de_light_kind kind);
de_entity rwt_spawn_camera(rwt *f, de_transform t, u32 view_id);
/** Sets the DeTransform position of `e`. */
void rwt_move(rwt *f, de_entity e, de_dvec3 pos);
/** Sets a cvar by name, if it exists. */
void rwt_set_cvar(const char *name, const char *value);
/** The slot of `e` in `fr`, or DE__RW_NONE. */
u32 rwt_inst_slot(const de__render_world_frame *fr, de_entity e);
/** A digest of every field of a buffer (field by field; padding never counts). */
u64 rwt_digest(const de__render_world_frame *f);

#endif /* DE_TEST_RENDER_WORLD_H */
