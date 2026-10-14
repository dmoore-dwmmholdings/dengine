/*
 * renderer_internal.h: the de_renderer object and the settings internals (SPEC §12.8, §3.3;
 * DENG-174). Internal to de_render; include as "render/renderer_internal.h".
 *
 * renderer.c: create / destroy / attach / the frame skeleton. settings.c: the preset tables, the
 * clamp to the device caps, the caps-only auto selection and the r.* cvars.
 *
 * One frame (de_renderer_render_frame), in this order:
 *   1. wait until the frame that used this frame's slot (frame N - frames_in_flight) is done on
 *      the GPU (the frame fence), then reset that slot's arena: nothing touches per-frame storage
 *      before this;
 *   2. apply settings (a de_renderer_set_settings request and cvar changes);
 *   3. de_render_uploader_flush (the copy-queue list of this frame's uploads);
 *   4. acquire the swapchain image (or take the offscreen target);
 *   5. build the frame graph on the slot's arena (clear the target, export it for present);
 *   6. record one graphics list: the uploader's acquire barriers first (before anything could
 *      use a new asset), then the passes; submit it waiting for the uploader's fence value and
 *      signalling the frame fence with N + 1; present.
 * DENG-176 replaces steps 5 and 6 with the per-view loop (src/render/views.c).
 */
#ifndef DE_RENDER_RENDERER_INTERNAL_H
#define DE_RENDER_RENDERER_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

DE_EXTERN_C_BEGIN

typedef struct de_rg de_rg;
typedef struct de_render_uploader de_render_uploader;

/** Most frames in flight (de_renderer_desc.frames_in_flight is clamped to 2..this). */
#define DE__RENDER_MAX_FRAMES_IN_FLIGHT 3u
/** Graphics command lists the renderer puts into the device's pool at create. The pool only
 * recycles a list once the RHI has seen its work complete, which can lag the frame fence by a few
 * frames when the CPU is busy; a pool this deep keeps the frame loop from growing it. */
#define DE__RENDER_CMD_PRIME (2u * DE__RENDER_MAX_FRAMES_IN_FLIGHT + 2u)
/** Address space reserved per frame arena. */
#define DE__RENDER_FRAME_ARENA_RESERVE ((size_t)256 << 20)
/** Size and format of the offscreen target of a renderer without a window. */
#define DE__RENDER_OFFSCREEN_WIDTH  1280u
#define DE__RENDER_OFFSCREEN_HEIGHT 720u
#define DE__RENDER_OFFSCREEN_FORMAT DE_FMT_RGBA8_UNORM
/** The access the offscreen target is left in at the end of a frame (its "present"): readable
 * by a copy (screenshots, golden images, tests). */
#define DE__RENDER_OFFSCREEN_FINAL_ACCESS DE_ACCESS_COPY_SRC
/** How long a frame waits for the GPU before it fails with DE_ERR_TIMEOUT. */
#define DE__RENDER_FENCE_TIMEOUT_NS (10ull * 1000000000ull)

/* ---------- Cvars (settings.c) ---------- */

/** Selections of `r.tier`. */
typedef enum de__render_tier_sel {
    DE__RENDER_TIER_AUTO,
    DE__RENDER_TIER_SEL_BASE,
    DE__RENDER_TIER_SEL_RT,
    DE__RENDER_TIER_SEL_COUNT
} de__render_tier_sel;

/** Selections of `r.quality`: auto, then DE_QUALITY_LOW..ULTRA at 1..4, then custom. */
typedef enum de__render_quality_sel {
    DE__RENDER_QUALITY_AUTO = 0,
    DE__RENDER_QUALITY_CUSTOM = 5,
    DE__RENDER_QUALITY_SEL_COUNT
} de__render_quality_sel;

/** The r.* cvars, in this order. */
typedef enum de__render_cvar_id {
    DE__RCV_TIER,
    DE__RCV_QUALITY,
    DE__RCV_SHADOWS,
    DE__RCV_AO,
    DE__RCV_REFLECTIONS,
    DE__RCV_GI,
    DE__RCV_VOLUMETRICS,
    DE__RCV_SSS,
    DE__RCV_RT_SHADOWS,
    DE__RCV_RT_AO,
    DE__RCV_RT_REFLECTIONS,
    DE__RCV_RT_GI,
    DE__RCV_RT_REFRACTION,
    DE__RCV_UPSCALER,
    DE__RCV_RENDER_SCALE,
    DE__RCV_DYN_RES_MS,
    DE__RCV_PATH_TRACER,
    DE__RCV_PATH_BOUNCES,
    DE__RCV_COUNT
} de__render_cvar_id;

/** The values of the r.* cvars as numbers (enums as indices, bools 0/1), for change checks. */
typedef struct de__render_cvar_snapshot {
    f64 v[DE__RCV_COUNT];
} de__render_cvar_snapshot;

/** The cvar side of a renderer. */
typedef struct de__render_cvars {
    de_cvar handle[DE__RCV_COUNT];
    bool owner; /**< this renderer reads and writes the cvars (the first renderer created) */
    de__render_cvar_snapshot seen; /**< values as of the last sync */
} de__render_cvars;

/* ---------- The renderer ---------- */

struct de_renderer {
    const de_allocator *alloc;
    de_gpu_device *dev;
    de_asset_system *assets;
    de_type_registry *types;
    de_world *world; /**< de_renderer_attach */
    de_log_category log;
    u32 frames_in_flight;
    bool hdr_output;

    de_gpu_caps caps;          /**< a copy (pure functions take it) */
    de_gpu_tier_report report; /**< de_gpu_evaluate_tier of `caps` */

    /* Settings: `settings` is written by de_renderer_set_settings (any time) and by the cvar
     * sync (frame start), under `lock`; `frame` is the copy one frame renders with. */
    de_mutex lock;
    de_render_settings settings;
    de_render_settings frame;
    de__render_cvars cvars;

    /* GPU uploads of the render asset types (NULL without an asset system). */
    de_render_uploader *uploader;
    bool uploader_orphaned; /**< registration failed half way; see de_renderer_create */

    /* Frames. */
    de_gpu_fence frame_fence; /**< frame N signals N + 1 */
    u64 frame_index;          /**< frames submitted */
    de_arena *arenas[DE__RENDER_MAX_FRAMES_IN_FLIGHT];
    de_rg *graph;

    /* Output: a swapchain, or an offscreen target. */
    de_window window;
    de_gpu_swapchain swapchain;
    de_gpu_texture offscreen;
    f32 clear_color[4];

    /* Per-frame scratch of the frame skeleton (DENG-176 replaces it). */
    de_rg_resource target_res; /**< the target's graph handle this frame */

    de_render_stats stats;
};

/* ---------- Settings (settings.c) ---------- */

/** RT tier allowed on this device: Base met, ray tracing (hardware or API only), ray queries. */
bool de__render_rt_allowed(const de_gpu_caps *caps, const de_gpu_tier_report *report);
/** Clamps `s` to the device (the rules of de_renderer_set_settings). Pure. */
void de__render_settings_clamp(const de_gpu_caps *caps, const de_gpu_tier_report *report,
                               de_render_settings *s);
/**
 * The caps-only auto selection: caps in, (tier, quality) out. S20 (DENG-276) routes
 * de_render_settings_auto to a GPU benchmark and keeps this as its fallback. Pure.
 */
void de__render_auto_select(const de_gpu_caps *caps, const de_gpu_tier_report *report,
                            de_tier *tier, de_quality *quality);
/** Field-by-field equality (padding ignored). */
bool de__render_settings_equal(const de_render_settings *a, const de_render_settings *b);
/** The preset quality `s` equals (true), or false for Custom. */
bool de__render_settings_match(const de_render_settings *s, de_quality *quality);

/** Registers the r.* cvars (idempotent) and claims them for `r` when no live renderer has. */
void de__render_cvars_init(de_renderer *r);
/** Releases the cvars when `r` holds them. */
void de__render_cvars_release(de_renderer *r);
/** Create: the settings the cvars select ("auto": de__render_auto_select), clamped; writes the
 * effective values back. For a renderer that does not own the cvars: the auto settings. */
de_render_settings de__render_cvars_initial(de_renderer *r);
/** Frame start (under `r->lock`): applies cvar changes since the last sync to `r->settings`.
 * Returns true when the settings changed. Allocates nothing when nothing changed. */
bool de__render_cvars_sync(de_renderer *r);
/** de__render_cvars_publish: also write `r.tier` (the explicit tier of the settings). */
#define DE__RCV_PUBLISH_TIER 1u
/** de__render_cvars_publish: also write `r.quality` (the matching preset, or "custom"). */
#define DE__RCV_PUBLISH_QUALITY 2u
/** Writes `r->settings` to the per-effect cvars (under `r->lock`), plus the selections that
 * `what` (DE__RCV_PUBLISH_*) names, then remembers the cvar values as seen. Only the owner. */
void de__render_cvars_publish(de_renderer *r, u32 what);

/* ---------- Test hooks (renderer.c) ---------- */

/** The offscreen target (invalid with a swapchain). Left in DE__RENDER_OFFSCREEN_FINAL_ACCESS
 * after each frame. */
de_gpu_texture de__renderer_offscreen(const de_renderer *r);
/** The frame fence and the number of frames submitted. */
de_gpu_fence de__renderer_frame_fence(const de_renderer *r, u64 *frames);
/** The uploader (NULL without an asset system). */
de_render_uploader *de__renderer_uploader(const de_renderer *r);

DE_EXTERN_C_END

#endif /* DE_RENDER_RENDERER_INTERNAL_H */
