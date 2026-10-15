/* vis_golden.h — the multi-frame helper of the visibility-buffer golden scenes (DENG-240).
 *
 * The DENG-329 harness runs one scene function per test and compares the image the scene leaves in
 * ctx->target. The vis-buffer scenes need more than one recording: they run the S13 vis frame
 * (src/render/passes/vis/) for several frames with their own render graphs and submissions, read
 * the vis buffer back after each frame, check it in C (culling on == culling off, mesh path ==
 * compute path), and finally upload one picture into the target. This helper does that without
 * changing the harness API:
 *
 *   vg_open         the vis frame, a GPU scene and a fake render world on ctx->device (skips the
 *                   scene below the Base tier, without the cooker, or with the frame off)
 *   vg_mesh_*       procedural meshes written as glTF 2.0 in memory and cooked through the S11
 *                   model importer (glTF front end, axis conversion, the DMSH v2 mesh stage:
 *                   LODs, quantization, meshlets with cones), then placed in GPU buffers as the
 *                   mesh loader lays them out
 *   vg_set          an instance of the fake render world (entity, mesh, transform, material)
 *   vg_frame        one frame: the GPU scene update, then for each requested variant (culling on
 *                   in view slot 0, culling off in view slot 1) one graph with the vis frame and a
 *                   readback, executed and waited for; the decoded vis buffer of each variant
 *   vg_compare      decoded (instance, triangle) per pixel of two variants: the pixels that differ
 *   vg_show_ids     the decoded IDs as flat colours (instance colour, a shade per meshlet) into
 *                   the harness target
 *
 * "Culling off" is r.cull_disable 1 (no frustum or occlusion test, no cone or small-meshlet test)
 * with r.occlusion 0; "on" is the default (r.occlusion 1). Vis IDs are compared decoded, never
 * raw: visible-meshlet indices legitimately differ between the variants and between the paths.
 *
 * Test code: it includes renderer and cooker internals.
 */
#ifndef DE_GOLDEN_VIS_GOLDEN_H
#define DE_GOLDEN_VIS_GOLDEN_H

#include "golden.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"
#include "render/passes/vis/gbuffer.h"
#include "render/passes/vis/vis_stages.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"

/** Meshes, materials and textures a scene may make. */
#define VG_MAX_MESHES    8u
#define VG_MAX_MATERIALS 6u
/** The edge-tie budget: at most this fraction of a frame's pixels may differ (SPEC note of
 * DENG-240: exact edge ties between the culling variants and between the raster paths). */
#define VG_TIE_BUDGET 0.0001

/** A decoded pixel: (instance << 32) | (meshlet << 7 | triangle); VG_EMPTY where nothing is. */
#define VG_EMPTY UINT64_MAX

typedef enum vg_variant { VG_ON = 0, VG_OFF = 1, VG_VARIANTS = 2 } vg_variant;

typedef struct vg_mesh {
    de_render_mesh rm;
    de_gpu_buffer buf[DE_RENDER_MESH_BUFFER_COUNT];
    de_aabb bounds;
    u32 triangles; /**< LOD 0 */
} vg_mesh;

/** The read-back result of one variant of one frame. */
typedef struct vg_image {
    bool valid;
    u64 *key;                  /**< width x height decoded pixels */
    u8 *gb0, *gb1, *gb2, *mot; /**< G-buffer rows (tight), when the frame asked for them */
    de__vis_stats stats;       /**< instance-cull stats read back so far (late) */
    de__vis_meshlet_stats mstats;
    u32 raster_path;
} vg_image;

typedef struct vg_frame_opts {
    bool on, off;     /**< variants to run (at least one) */
    const char *path; /**< r.raster_path for both: "auto", "mesh", "compute" (NULL: "auto") */
    bool gbuffer;     /**< read the G-buffer of the ON variant back (resolve debug readback) */
} vg_frame_opts;

typedef struct vg {
    de_golden_ctx *ctx;
    de_gpu_device *dev;
    de_arena *arena;
    de_rg *graph[VG_VARIANTS];
    de__gpu_scene *gs;
    de__vis_shared *vis;
    de_gpu_fence fence;
    u64 fence_value;
    u64 frames;
    de_gpu_buffer rb[VG_VARIANTS];
    u64 rb_size[VG_VARIANTS];
    /* The fake render world (DENG-175's contract). */
    de__render_world_frame world;
    u32 capacity;
    /* Assets. */
    vg_mesh mesh[VG_MAX_MESHES];
    u32 mesh_count;
    de_render_material mat[VG_MAX_MATERIALS];
    u32 mat_count;
    de_gpu_texture checker;
    de_gpu_buffer checker_up;
    u32 checker_bindless;
    /* The camera (world space, Z up). */
    de_vec3 eye, target;
    f32 fov;
    u32 width, height; /**< the vis buffer: the target's size unless a scene says otherwise */
    vg_image img[VG_VARIANTS];
    /** r.cull_disable exists: culling can be turned off (not in Shipping, which drops DEV_ONLY
     * cvars). */
    bool can_cull_off;
} vg;

/** Opens everything; false when the scene was skipped or failed (reason given to the harness).
 * `instances`: the world's slot capacity. */
bool vg_open(vg *v, de_golden_ctx *ctx, u32 instances);
/** Skips the scene (true) when this build cannot turn culling off (Shipping): the on == off
 * checks need r.cull_disable. */
bool vg_skip_without_cull_off(vg *v);
/** Waits for the GPU and frees everything. Safe after a failed open. */
void vg_close(vg *v);

/* ---------- Meshes (1-based ids; 0 on failure, the scene failed) ---------- */

/** A box of half extents `h`, each face subdivided `n` x `n`. */
u32 vg_mesh_box(vg *v, de_vec3 h, u32 n);
/** A UV sphere of radius `r` with `seg` segments around and `seg / 2` rings. */
u32 vg_mesh_sphere(vg *v, f32 r, u32 seg);
/** A square `size` wide in the XY plane facing +Z, subdivided `n` x `n`, UV0 over [0, uv]. */
u32 vg_mesh_plane(vg *v, f32 size, u32 n, f32 uv);
/** A square card `size` wide in the XZ plane facing -Y, subdivided `n` x `n`, UV0 [0, uv]. */
u32 vg_mesh_card(vg *v, f32 size, u32 n, f32 uv);

/* ---------- Materials (1-based ids) ---------- */

/** An opaque standard material of linear base colour `rgb` (metallic 0, roughness 0.5). */
u32 vg_material(vg *v, f32 r, f32 g, f32 b, bool double_sided);
/** An alpha-tested (mask, cutoff 0.5) double-sided material sampling an 8 x 8 checker whose
 * alpha alternates 255 / 0 (procedural; uploaded on the device's graphics queue). */
u32 vg_material_cutout(vg *v);

/* ---------- The world ---------- */

/** Instance slot `s`: entity bits 1000 + s, mesh `mesh`, material `mat` for every submesh. */
void vg_set(vg *v, u32 s, u32 mesh, de_vec3 pos, f32 yaw, de_vec3 scale, u32 mat);

/* ---------- Frames ---------- */

/** One frame (see the header). False on an error (the scene failed). */
bool vg_frame(vg *v, const vg_frame_opts *o);
/** Pixels whose decoded key differs between `a` and `b`; `first` gets the first one (x, y). */
u32 vg_compare(const vg *v, const vg_image *a, const vg_image *b, u32 *first_x, u32 *first_y);
/** Fails the scene when `diff` pixels exceed the tie budget; prints the count either way. */
bool vg_check_budget(vg *v, const char *what, u32 frame, u32 diff, u32 fx, u32 fy);
/** Pixels of `img` that hold a surface. */
u32 vg_covered(const vg *v, const vg_image *img);

/* ---------- Output into the harness target (ctx size; the vis size must fit) ---------- */

/** The decoded IDs of `img` as colours into the region (x0, y0) of an RGBA8 picture. */
void vg_paint_ids(const vg *v, const vg_image *img, u8 *rgba, u32 pitch, u32 x0, u32 y0);
/** Uploads an RGBA8 picture of the target's size (rows `pitch` bytes) into ctx->target. */
void vg_present(vg *v, const u8 *rgba, u32 pitch);
/** vg_paint_ids of `img` over the whole target (black elsewhere), then vg_present. */
void vg_show_ids(vg *v, const vg_image *img);

#endif
