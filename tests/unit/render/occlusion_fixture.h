/*
 * occlusion_fixture.h: the fixture of test_hzb.c and test_two_phase.c (DENG-233).
 *
 * A device of one backend, a render graph, a GPU scene fed by a fake render world of boxes (one
 * fake mesh: a unit cube with one LOD and one meshlet, so an instance's world AABB is exactly the
 * box it stands for), and the vis frame. Raster does not exist yet (DENG-234/DENG-235), so the
 * phase-1 depth is produced on the CPU: a ray cast of the boxes the test says phase 1 draws, at
 * the view's pixel centres, uploaded into an R32F texture that the frame uses as its depth
 * (de__vis_frame_desc.depth_override). The same ray cast gives the reference depth and the id of
 * the nearest box per pixel for the conservativeness check.
 *
 * Each frame reads back both phases' work lists and counters and the view's visibility flags.
 */
#ifndef DE_TEST_OCCLUSION_FIXTURE_H
#define DE_TEST_OCCLUSION_FIXTURE_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>

#include "render/passes/vis/vis_stages.h"
#include "render/world/render_world.h"

#define OCX_MAX_INSTANCES 4096u

typedef struct ocx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    de_gpu_backend backend;
    bool real_gpu;
    de_rg *g;
    de__gpu_scene *gs;
    de__vis_shared *vis;
    de_gpu_fence fence;
    u64 frames;
    u32 view_id;   /* the view handle the frame renders (1 by default) ... */
    u32 view_slot; /* ... and its slot (0) */

    /* The fake render world. */
    de__render_world_frame w;
    u32 cap;
    u32 *dirty_mark;

    /* Camera and view size. */
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;

    /* The phase-1 depth: CPU ray cast, uploaded each frame. */
    de_gpu_texture depth_tex;
    de_gpu_buffer depth_upload;
    u32 depth_pitch;
    u32 depth_access;
    f32 *depth_cpu;
    u32 *id_cpu; /* nearest box per pixel (slot), UINT32_MAX = none */

    /* Readback and the last frame's results. */
    de_gpu_buffer rb;
    u64 rb_size;
    u32 counters[2][4];
    de__vis_work_item *items[2];
    u32 item_count[2], item_cap[2];
    u32 *visibility; /* flags of slots [0, instance count) */
    u32 visibility_count;
    de__vis_cull_view cv;
    u32 hzb_width, hzb_height, hzb_mips;

    /* Objects a test made, destroyed at teardown. Metal's debug layer asserts when an object is
     * released while a command buffer that used it has not reported completion yet, which can
     * trail the fence a test waits for: destroying right after a frame would race that. */
    de_gpu_texture keep_tex[64];
    u32 keep_tex_count;
    de_gpu_buffer keep_buf[64];
    u32 keep_buf_count;
    struct de_rg *keep_graph[16]; /* graphs a test made (their transients), destroyed likewise */
    u32 keep_graph_count;
} ocx;

/** Fixture setup / teardown (de_test.h TEST_F naming). */
void ocx_setup(ocx *f);
void ocx_teardown(ocx *f);
/** Opens a device of `backend` (SKIPs without it, below the Base tier or without the vis
 * frame), a graph, a GPU scene with room for `instances` slots, and the vis frame. */
void ocx_open(ocx *f, de_gpu_backend backend, u32 instances);

/** Slot `s` holds a box of `half` extents at `center` (0 extents: a free slot). Marks it changed
 * for the next frame. */
void ocx_set_box(ocx *f, u32 s, de_vec3 center, de_vec3 half);
/** The world AABB of slot `s` (as the GPU scene packs it). */
de_aabb ocx_box(const ocx *f, u32 s);

/** The cull constants the frame builds for the camera (frustum, as de__vis_frame_build). */
de__vis_cull_view ocx_cpu_view(const ocx *f);
/** CPU frustum test of slot `s` (sphere then AABB, as instance_cull.slang). */
bool ocx_in_frustum(const ocx *f, const de__vis_cull_view *v, u32 s);
/** Ray casts the boxes of the slots with `draw[s]` set into f->depth_cpu and f->id_cpu (NULL:
 * every live slot). */
void ocx_raycast(ocx *f, const bool *draw);

/** One frame with f->depth_cpu as the phase-1 depth; reads the results back. */
bool ocx_frame(ocx *f);

/** True when slot `s` is in phase `p`'s (0 or 1) work list of the last frame. */
bool ocx_drawn(const ocx *f, u32 phase_index, u32 s);

/** Keeps a texture or buffer until teardown (see `keep_tex`). */
void ocx_keep_texture(ocx *f, de_gpu_texture t);
void ocx_keep_buffer(ocx *f, de_gpu_buffer b);
/** A new render graph on the fixture's device, destroyed at teardown. */
struct de_rg *ocx_graph(ocx *f, const char *name);

/** Sets a cvar by name (false when it does not exist, e.g. a Dev-only cvar in Shipping). */
bool ocx_cvar(const char *name, const char *value);

#endif /* DE_TEST_OCCLUSION_FIXTURE_H */
