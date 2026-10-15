/*
 * views.c: views and the per-view frame pipeline (SPEC §12.4, §12.5, §12.9, §20.1; DENG-176).
 * views_internal.h describes the model, the pass-registration list and one frame.
 *
 * Two sides, one lock. The main thread (de_view_*, de_render_add_hook, de__view_register_stage)
 * writes the `main` side of the view table and the stage and hook lists under views->lock. The
 * render thread (de__views_render) copies them into its own `rs` side once per frame, under the
 * same lock, and from then on reads only its copy, so a view created, changed or destroyed during
 * a frame takes effect in the next one. A handle is (generation << 8) | (slot + 1); destroying a
 * view bumps the slot's generation, so the handle goes stale at once on both sides.
 *
 * GPU objects of a view: its de_rg (made at its first rendered frame) and its output texture
 * (remade when the internal size changes; the old one goes through the RHI's deferred
 * destruction). A destroyed view's graph and output are retired: released once the frame fence
 * passes the last frame that used them, so frames in flight never see them go.
 */
#include "render/views_internal.h"

#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"

#include "gpu/gpu_backend.h" /* de__gpu_get_texture: size, format and usage of a target */

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- State ---------- */

/** The main-thread side of a view slot (under the lock). */
typedef struct view_main {
    u32 gen;    /**< generation of the slot's current handle */
    bool alive; /**< the handle with `gen` is valid */
    de_gpu_texture target;
    f32 viewport[4];
    i32 order;
    bool pixel_art; /**< stored; S19 renders it */
    char name[DE__VIEW_NAME_MAX];
    bool has_settings;
    de_render_settings settings; /**< clamped to the device when set */
    f32 render_scale;            /**< 0: the settings' render_scale */
    u32 seq;                     /**< creation sequence: ties of `order` */
} view_main;

/** The render-thread side of a view slot. */
typedef struct view_rs {
    bool live; /**< has state for generation `gen` */
    u32 gen;
    view_main copy; /**< the main side as of this frame's sync */
    de_rg *graph;
    de_gpu_texture output; /**< DE__VIEW_OUTPUT_FORMAT, out_w x out_h */
    u32 out_w, out_h;
    u32 out_access;    /**< access the output is in between frames */
    u32 target_access; /**< access the target is in between frames (render-to-texture) */
    u64 executed;
    bool has_prev;
    de_mat4 prev_view_proj;
    bool failed_logged; /**< the last failure was logged; reset by a frame that works */
    /* This frame: */
    bool active;
    u32 camera; /**< index into the render world's views */
    bool ok;    /**< the graph was executed */
    u32 vp[4];
    u32 w, h;
    de_render_settings eff;
} view_rs;

/** One entry of the stage list, the hook list or the merged sequence. */
typedef struct seq_entry {
    bool hook;
    u32 slot;
    i32 order;
    u32 phase; /**< 0: before stages of the same order, 1: stages, 2: after them */
    u32 seq;   /**< registration sequence (stages and hooks share it) */
    de__view_stage_fn stage;
    de_rg_hook_fn hook_fn;
    void *user;
    char name[DE__VIEW_NAME_MAX];
} seq_entry;

typedef struct retired_view {
    de_rg *graph;
    de_gpu_texture output;
    u64 after; /**< frame-fence value after which nothing uses them */
} retired_view;

typedef struct de__views {
    de_mutex lock;
    /* ---- main side (lock) ---- */
    view_main main[DE__VIEW_MAX];
    u32 seq;
    seq_entry stages[DE__VIEW_MAX_STAGES];
    u32 stage_count;
    seq_entry hooks[DE__VIEW_MAX_HOOKS];
    u32 hook_count;
    u32 version; /**< bumped on every stage or hook change */
    de_view main_view;
    /* ---- render side ---- */
    view_rs rs[DE__VIEW_MAX];
    seq_entry list[DE__VIEW_MAX_STAGES + DE__VIEW_MAX_HOOKS];
    u32 list_count;
    u32 list_version;
    retired_view retired[DE__VIEW_MAX * 2u];
    u32 retired_count;
    u32 order[DE__VIEW_MAX];
    u32 order_count;
    de_view last_order[DE__VIEW_MAX];
    u32 last_order_count;
    de__composite *composite;
    u32 out_w, out_h;
    de_gpu_format out_format;
    /* The view whose graph is being built (de_view_color, de_view_size). */
    const de__view_frame *building;
    /* de_render_stats pass arrays (grow only). */
    const char **stat_names;
    f32 *stat_ms;
    u32 stat_cap;
    bool composite_warned;
    /* DENG-229: the visibility-buffer frame (passes/vis/vis_frame.c), the stage "vis.frame". */
    struct de__vis_shared *vis;
} de__views;

/* ---------- Handles ---------- */

static de_view make_handle(u32 slot, u32 gen) {
    return (de_view){.id = (gen << 8) | (slot + 1u)};
}

static u32 handle_slot(de_view v) {
    return (v.id & 0xFFu) - 1u; /* UINT32_MAX for id 0 */
}

static u32 handle_gen(de_view v) {
    return v.id >> 8;
}

/* The main-side slot of `v`, or NULL when stale. Lock held. */
static view_main *main_of(de__views *vs, de_view v) {
    const u32 s = handle_slot(v);
    if (s >= DE__VIEW_MAX || !vs->main[s].alive || vs->main[s].gen != handle_gen(v))
        return NULL;
    return &vs->main[s];
}

/* The render-side slot of `v` this frame, or NULL. */
static view_rs *rs_of(const de__views *vs, de_view v) {
    const u32 s = handle_slot(v);
    if (s >= DE__VIEW_MAX || !vs->rs[s].live || vs->rs[s].gen != handle_gen(v))
        return NULL;
    return (view_rs *)&vs->rs[s];
}

static u32 next_gen(u32 gen) {
    gen = (gen + 1u) & 0xFFFFFFu;
    return gen == 0 ? 1u : gen;
}

/* ---------- Settings ---------- */

/* The settings a view renders with: its own (or the renderer's), its render_scale, never above
 * the renderer's tier, clamped to the device. */
static de_render_settings effective_settings(const de_renderer *r, const view_main *m,
                                             const de_render_settings *renderer) {
    de_render_settings s = m->has_settings ? m->settings : *renderer;
    if (m->render_scale > 0.0f)
        s.render_scale = m->render_scale;
    if (renderer->tier == DE_TIER_BASE)
        s.tier = DE_TIER_BASE;
    de__render_settings_clamp(&r->caps, &r->report, &s);
    return s;
}

/* ---------- The debug-clear stage (until S13) ---------- */

typedef struct debug_clear {
    de_rg_resource color;
    f32 rgba[4];
} debug_clear;

/* A colour from the name's hash: hue from 16 bits, saturation 0.65, value 0.9. */
void de__view_debug_color(const char *name, f32 out[4]) {
    const u64 h = de_hash64_str(name);
    const f32 hue = (f32)(h & 0xFFFFu) / 65536.0f * 6.0f;
    const u32 sector = (u32)hue;
    const f32 f = hue - (f32)sector;
    const f32 v = 0.9f, s = 0.65f;
    const f32 p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    f32 rgb[3];
    switch (sector % 6u) {
    case 0:
        rgb[0] = v, rgb[1] = t, rgb[2] = p;
        break;
    case 1:
        rgb[0] = q, rgb[1] = v, rgb[2] = p;
        break;
    case 2:
        rgb[0] = p, rgb[1] = v, rgb[2] = t;
        break;
    case 3:
        rgb[0] = p, rgb[1] = q, rgb[2] = v;
        break;
    case 4:
        rgb[0] = t, rgb[1] = p, rgb[2] = v;
        break;
    default:
        rgb[0] = v, rgb[1] = p, rgb[2] = q;
        break;
    }
    out[0] = rgb[0];
    out[1] = rgb[1];
    out[2] = rgb[2];
    out[3] = 1.0f;
}

static void debug_clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const debug_clear *dc = (const debug_clear *)user;
    de_gpu_attachment a = {.texture = de_rg_texture(pass, dc->color), .clear = true, .store = true};
    memcpy(a.clear_color, dc->rgba, sizeof a.clear_color);
    de_gpu_cmd_begin_render(cmd, &a, 1, NULL);
    de_gpu_cmd_end_render(cmd);
}

static void debug_clear_stage(de__view_frame *vf, void *user) {
    (void)user;
    debug_clear *dc = (debug_clear *)de_arena_push(vf->graph->arena, sizeof *dc, 16);
    if (dc == NULL)
        return;
    dc->color = vf->color;
    de__view_debug_color(vf->name, dc->rgba);
    de_rg_pass *p =
        de_rg_add_pass(vf->graph, "view.debug_clear", DE_QUEUE_GRAPHICS, debug_clear_execute, dc);
    if (p != NULL)
        de_rg_write(p, vf->color, DE_ACCESS_RENDER_TARGET);
}

/* ---------- Main thread: views ---------- */

static de__views *views_of(const de_renderer *r) {
    return r != NULL ? r->views : NULL;
}

static bool viewport_ok(const f32 vp[4], f32 out[4]) {
    if (vp[0] == 0.0f && vp[1] == 0.0f && vp[2] == 0.0f && vp[3] == 0.0f) {
        out[0] = 0.0f, out[1] = 0.0f, out[2] = 1.0f, out[3] = 1.0f;
        return true;
    }
    for (u32 i = 0; i < 4; ++i)
        if (!(vp[i] >= 0.0f && vp[i] <= 1.0f)) /* also NaN */
            return false;
    const f32 eps = 1e-4f;
    if (!(vp[2] > 0.0f && vp[3] > 0.0f) || vp[0] + vp[2] > 1.0f + eps || vp[1] + vp[3] > 1.0f + eps)
        return false;
    memcpy(out, vp, sizeof(f32) * 4u);
    return true;
}

/* Creates a view under the lock (the caller checked `desc`). */
static de_view create_locked(de__views *vs, const de_view_desc *desc, const f32 vp[4]) {
    for (u32 s = 0; s < DE__VIEW_MAX; ++s) {
        view_main *m = &vs->main[s];
        if (m->alive)
            continue;
        const u32 gen = next_gen(m->gen);
        memset(m, 0, sizeof *m);
        m->gen = gen;
        m->alive = true;
        m->target = desc->target;
        memcpy(m->viewport, vp, sizeof m->viewport);
        m->order = desc->order;
        m->pixel_art = desc->pixel_art;
        if (desc->name != NULL && desc->name[0] != '\0')
            snprintf(m->name, sizeof m->name, "%s", desc->name);
        else
            snprintf(m->name, sizeof m->name, "view%u", s);
        m->seq = vs->seq++;
        return make_handle(s, gen);
    }
    return (de_view){0};
}

de_view de_view_create(de_renderer *r, const de_view_desc *desc) {
    de__views *vs = views_of(r);
    if (vs == NULL || desc == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_view_create: NULL %s",
                      vs == NULL ? "renderer" : "desc");
        return (de_view){0};
    }
    f32 vp[4];
    if (!viewport_ok(desc->viewport, vp)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "de_view_create(\"%s\"): viewport (%g, %g, %g, %g) is not a rectangle "
                      "inside 0..1 (all zero = the whole target)",
                      desc->name != NULL ? desc->name : "", (double)desc->viewport[0],
                      (double)desc->viewport[1], (double)desc->viewport[2],
                      (double)desc->viewport[3]);
        return (de_view){0};
    }
    if (DE_HANDLE_VALID(desc->target)) {
        const de__gpu_texture *t = de__gpu_get_texture(r->dev, desc->target);
        const u32 need = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED;
        if (t == NULL || (t->usage & need) != need || t->dim != DE_TEX_2D) {
            (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                          "de_view_create(\"%s\"): the target is %s; a render-to-texture target "
                          "is a 2D texture with DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED",
                          desc->name != NULL ? desc->name : "",
                          t == NULL ? "not a live texture" : "missing a usage or not 2D");
            return (de_view){0};
        }
    }
    de_mutex_lock(&vs->lock);
    const de_view v = create_locked(vs, desc, vp);
    de_mutex_unlock(&vs->lock);
    if (!DE_HANDLE_VALID(v))
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_view_create: more than %u views", DE__VIEW_MAX);
    return v;
}

void de_view_destroy(de_renderer *r, de_view v) {
    de__views *vs = views_of(r);
    if (vs == NULL)
        return;
    if (v.id == vs->main_view.id) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_view_destroy: the main view stays");
        DE_WARN(r->log, "de_view_destroy: the main view cannot be destroyed");
        return;
    }
    de_mutex_lock(&vs->lock);
    view_main *m = main_of(vs, v);
    if (m != NULL) {
        m->alive = false;
        m->gen = next_gen(m->gen); /* stale at once; the render side retires its objects */
    }
    de_mutex_unlock(&vs->lock);
}

de_view de_renderer_main_view(const de_renderer *r) {
    const de__views *vs = views_of(r);
    return vs != NULL ? vs->main_view : (de_view){0};
}

void de_view_set_settings(de_renderer *r, de_view v, const de_render_settings *s) {
    de__views *vs = views_of(r);
    if (vs == NULL)
        return;
    de_render_settings c = {0};
    if (s != NULL) {
        c = *s;
        de__render_settings_clamp(&r->caps, &r->report, &c);
    }
    de_mutex_lock(&vs->lock);
    view_main *m = main_of(vs, v);
    if (m != NULL) {
        m->has_settings = s != NULL;
        m->settings = c;
    }
    de_mutex_unlock(&vs->lock);
}

void de_view_set_render_scale(de_renderer *r, de_view v, f32 scale) {
    de__views *vs = views_of(r);
    if (vs == NULL)
        return;
    de_mutex_lock(&vs->lock);
    view_main *m = main_of(vs, v);
    if (m != NULL)
        m->render_scale = scale > 0.0f ? scale : 0.0f; /* NaN and <= 0: the settings' */
    de_mutex_unlock(&vs->lock);
}

de_render_settings de_view_get_settings(de_renderer *r, de_view v) {
    const de_render_settings rs = de_renderer_get_settings(r);
    de__views *vs = views_of(r);
    if (vs == NULL)
        return rs;
    de_mutex_lock(&vs->lock);
    const view_main *m = main_of(vs, v);
    const view_main copy = m != NULL ? *m : (view_main){0};
    de_mutex_unlock(&vs->lock);
    return effective_settings(r, &copy, &rs);
}

/* ---------- Main thread: stages and hooks ---------- */

/* Where a de_rg_hook point sits in the sequence (views_internal.h). */
static bool hook_position(de_rg_hook at, u32 *slot, i32 *order, u32 *phase) {
    switch (at) {
    case DE_HOOK_AFTER_GBUFFER:
        *slot = DE__VIEW_SLOT_VISIBILITY, *order = INT32_MAX, *phase = 2;
        return true;
    case DE_HOOK_AFTER_LIGHTING:
        *slot = DE__VIEW_SLOT_RT, *order = INT32_MAX, *phase = 2;
        return true;
    case DE_HOOK_AFTER_TRANSPARENT:
        *slot = DE__VIEW_SLOT_TRANSPARENT, *order = INT32_MAX, *phase = 2;
        return true;
    case DE_HOOK_BEFORE_TONEMAP:
        *slot = DE__VIEW_SLOT_POST, *order = DE__VIEW_ORDER_TONEMAP, *phase = 0;
        return true;
    case DE_HOOK_AFTER_TONEMAP:
        *slot = DE__VIEW_SLOT_POST, *order = DE__VIEW_ORDER_TONEMAP, *phase = 2;
        return true;
    case DE_HOOK_UI:
        *slot = DE__VIEW_SLOT_OUTPUT, *order = DE__VIEW_ORDER_UI, *phase = 2;
        return true;
    }
    return false;
}

void de_render_add_hook(de_renderer *r, de_rg_hook at, de_rg_hook_fn fn, void *user) {
    de__views *vs = views_of(r);
    u32 slot = 0, phase = 0;
    i32 order = 0;
    if (vs == NULL || fn == NULL || !hook_position(at, &slot, &order, &phase)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_render_add_hook: %s",
                      vs == NULL   ? "NULL renderer"
                      : fn == NULL ? "NULL callback"
                                   : "unknown hook point");
        if (r != NULL)
            DE_ERROR(r->log, "de_render_add_hook: refused (%s)",
                     fn == NULL ? "NULL callback" : "unknown hook point");
        return;
    }
    de_mutex_lock(&vs->lock);
    const bool full = vs->hook_count == DE__VIEW_MAX_HOOKS;
    if (!full) {
        seq_entry *e = &vs->hooks[vs->hook_count++];
        memset(e, 0, sizeof *e);
        e->hook = true;
        e->slot = slot;
        e->order = order;
        e->phase = phase;
        e->seq = vs->seq++;
        e->hook_fn = fn;
        e->user = user;
        snprintf(e->name, sizeof e->name, "hook%u", (unsigned)at);
        ++vs->version;
    }
    de_mutex_unlock(&vs->lock);
    if (full) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_render_add_hook: more than %u hooks",
                      DE__VIEW_MAX_HOOKS);
        DE_ERROR(r->log, "de_render_add_hook: more than %u hooks", DE__VIEW_MAX_HOOKS);
    }
}

de_result de__view_register_stage(de_renderer *r, de__view_slot slot, i32 order, const char *name,
                                  de__view_stage_fn fn, void *user) {
    de__views *vs = views_of(r);
    if (vs == NULL || fn == NULL || (u32)slot >= DE__VIEW_SLOT_COUNT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__view_register_stage(\"%s\"): %s",
                       name != NULL ? name : "",
                       vs == NULL   ? "NULL renderer"
                       : fn == NULL ? "NULL callback"
                                    : "bad slot");
    de_mutex_lock(&vs->lock);
    const bool full = vs->stage_count == DE__VIEW_MAX_STAGES;
    if (!full) {
        seq_entry *e = &vs->stages[vs->stage_count++];
        memset(e, 0, sizeof *e);
        e->slot = (u32)slot;
        e->order = order;
        e->phase = 1;
        e->seq = vs->seq++;
        e->stage = fn;
        e->user = user;
        snprintf(e->name, sizeof e->name, "%s", name != NULL ? name : "");
        ++vs->version;
    }
    de_mutex_unlock(&vs->lock);
    return full ? DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                          "de__view_register_stage(\"%s\"): more than %u "
                          "stages",
                          name != NULL ? name : "", DE__VIEW_MAX_STAGES)
                : DE_OK;
}

void de__view_unregister_stage(de_renderer *r, de__view_stage_fn fn, void *user) {
    de__views *vs = views_of(r);
    if (vs == NULL)
        return;
    de_mutex_lock(&vs->lock);
    u32 w = 0;
    for (u32 i = 0; i < vs->stage_count; ++i)
        if (!(vs->stages[i].stage == fn && vs->stages[i].user == user))
            vs->stages[w++] = vs->stages[i];
    if (w != vs->stage_count) {
        vs->stage_count = w;
        ++vs->version;
    }
    de_mutex_unlock(&vs->lock);
}

/* ---------- Render thread: hook-side accessors ---------- */

de_rg_resource de_view_color(de_renderer *r, de_view v) {
    const de__views *vs = views_of(r);
    if (vs == NULL || vs->building == NULL || vs->building->view.id != v.id)
        return (de_rg_resource){0};
    return vs->building->color;
}

void de_view_size(de_renderer *r, de_view v, u32 *width, u32 *height) {
    const de__views *vs = views_of(r);
    const bool ok = vs != NULL && vs->building != NULL && vs->building->view.id == v.id;
    if (width != NULL)
        *width = ok ? vs->building->width : 0;
    if (height != NULL)
        *height = ok ? vs->building->height : 0;
}

/* ---------- Render thread: sync and retire ---------- */

static bool seq_less(const seq_entry *a, const seq_entry *b) {
    if (a->slot != b->slot)
        return a->slot < b->slot;
    if (a->order != b->order)
        return a->order < b->order;
    if (a->phase != b->phase)
        return a->phase < b->phase;
    return a->seq < b->seq;
}

/* Merges the stages and hooks into the sequence (lock held). Insertion sort: small lists. */
static void rebuild_list(de__views *vs) {
    u32 n = 0;
    for (u32 i = 0; i < vs->stage_count; ++i)
        vs->list[n++] = vs->stages[i];
    for (u32 i = 0; i < vs->hook_count; ++i)
        vs->list[n++] = vs->hooks[i];
    for (u32 i = 1; i < n; ++i) {
        const seq_entry e = vs->list[i];
        u32 j = i;
        while (j > 0 && seq_less(&e, &vs->list[j - 1])) {
            vs->list[j] = vs->list[j - 1];
            --j;
        }
        vs->list[j] = e;
    }
    vs->list_count = n;
    vs->list_version = vs->version;
}

static void release_now(de_renderer *r, de_rg *g, de_gpu_texture out) {
    if (g != NULL)
        de__rg_destroy(g);
    if (DE_HANDLE_VALID(out))
        de_gpu_texture_destroy(r->dev, out);
}

/* Retires the render side of a slot: its objects go once the frame fence passes every frame
 * submitted so far. */
static void retire(de_renderer *r, de__views *vs, view_rs *s) {
    if (s->graph != NULL || DE_HANDLE_VALID(s->output)) {
        if (vs->retired_count == DE__VIEW_MAX * 2u) {
            /* Full (views destroyed faster than frames complete): wait for the GPU. */
            if (r->frame_index > 0)
                (void)de_gpu_fence_wait(r->dev, r->frame_fence, r->frame_index,
                                        DE__RENDER_FENCE_TIMEOUT_NS);
            release_now(r, s->graph, s->output);
        } else {
            vs->retired[vs->retired_count++] =
                (retired_view){.graph = s->graph, .output = s->output, .after = r->frame_index};
        }
    }
    memset(s, 0, sizeof *s);
}

static void collect_retired(de_renderer *r, de__views *vs) {
    if (vs->retired_count == 0)
        return;
    const u64 done = de_gpu_fence_value(r->dev, r->frame_fence);
    u32 w = 0;
    for (u32 i = 0; i < vs->retired_count; ++i) {
        retired_view *rv = &vs->retired[i];
        if (rv->after <= done)
            release_now(r, rv->graph, rv->output);
        else
            vs->retired[w++] = *rv;
    }
    vs->retired_count = w;
}

static void sync(de_renderer *r, de__views *vs) {
    de_mutex_lock(&vs->lock);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i) {
        const view_main *m = &vs->main[i];
        view_rs *s = &vs->rs[i];
        if (s->live && (!m->alive || m->gen != s->gen))
            retire(r, vs, s);
        if (m->alive && !s->live) {
            memset(s, 0, sizeof *s);
            s->live = true;
            s->gen = m->gen;
        }
        if (s->live)
            s->copy = *m;
    }
    if (vs->list_version != vs->version)
        rebuild_list(vs);
    de_mutex_unlock(&vs->lock);
}

/* ---------- Render thread: one view ---------- */

/* Rounds `x` to the nearest u32 in [lo, hi]. */
static u32 round_px(f32 x, u32 lo, u32 hi) {
    const f32 v = floorf(x + 0.5f);
    const u32 u = v <= (f32)lo ? lo : v >= (f32)hi ? hi : (u32)v;
    return u;
}

/* The viewport of `vp` (normalised) in a `w` x `h` target, in whole pixels, at least 1 x 1. */
static void viewport_px(const f32 vp[4], u32 w, u32 h, u32 out[4]) {
    const u32 x0 = round_px(vp[0] * (f32)w, 0, w - 1u);
    const u32 y0 = round_px(vp[1] * (f32)h, 0, h - 1u);
    const u32 x1 = round_px((vp[0] + vp[2]) * (f32)w, x0 + 1u, w);
    const u32 y1 = round_px((vp[1] + vp[3]) * (f32)h, y0 + 1u, h);
    out[0] = x0;
    out[1] = y0;
    out[2] = x1 - x0;
    out[3] = y1 - y0;
}

static void matrices(const de__rw_view *cam, const u32 vp[4], de__view_frame *vf) {
    const de_affine inv = de_affine_inverse(&cam->transform);
    vf->view_matrix = de_affine_to_m4(&inv);
    const f32 aspect = (f32)vp[2] / (f32)vp[3];
    const f32 near_z = cam->camera.near_z > 0.0f ? cam->camera.near_z : 0.1f;
    if (cam->camera.projection == DE_PROJ_ORTHOGRAPHIC) {
        const f32 hh = (cam->camera.ortho_height > 0.0f ? cam->camera.ortho_height : 10.0f) * 0.5f;
        vf->proj = de_m4_ortho_rz(-hh * aspect, hh * aspect, -hh, hh, near_z, DE__VIEW_ORTHO_FAR);
    } else {
        const f32 fov = cam->camera.fov_y > 0.0f ? cam->camera.fov_y : 1.0471976f;
        vf->proj = de_m4_perspective_rz(fov, aspect, near_z);
    }
    vf->view_proj = de_m4_mul(&vf->proj, &vf->view_matrix);
}

/* Logs a view failure once until the view works again. */
static void view_failed(de_renderer *r, view_rs *s, const char *what) {
    if (!s->failed_logged) {
        const de_error *e = de_error_last();
        DE_ERROR(r->log, "renderer: view \"%s\": %s: %s", s->copy.name, what,
                 e != NULL ? e->message : "?");
    }
    s->failed_logged = true;
}

/* Makes the view's graph and output texture of the right size. */
static bool ensure_objects(de_renderer *r, view_rs *s) {
    if (s->graph == NULL) {
        char name[48];
        snprintf(name, sizeof name, "view:%s", s->copy.name);
        if (de__rg_create(&(de__rg_desc){.allocator = r->alloc, .device = r->dev, .name = name},
                          &s->graph) != DE_OK) {
            view_failed(r, s, "render graph");
            return false;
        }
    }
    if (DE_HANDLE_VALID(s->output) && s->out_w == s->w && s->out_h == s->h)
        return true;
    if (DE_HANDLE_VALID(s->output))
        de_gpu_texture_destroy(r->dev, s->output); /* deferred until the GPU is done with it */
    char name[48];
    snprintf(name, sizeof name, "view:%s output", s->copy.name);
    s->output = de_gpu_texture_create(
        r->dev, &(de_gpu_texture_desc){.format = DE__VIEW_OUTPUT_FORMAT,
                                       .width = s->w,
                                       .height = s->h,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                DE_USAGE_COPY_SRC,
                                       .name = name});
    s->out_access = DE_ACCESS_NONE;
    s->out_w = s->w;
    s->out_h = s->h;
    if (!DE_HANDLE_VALID(s->output)) {
        s->out_w = s->out_h = 0;
        view_failed(r, s, "output texture");
        return false;
    }
    return true;
}

/* The access a render-to-texture target is in before view `slot` this frame: what an earlier view
 * of this frame with the same target left it in, else what this view left it in. */
static u32 target_access_before(const de__views *vs, u32 slot, u32 pos) {
    const view_rs *s = &vs->rs[slot];
    for (u32 k = pos; k-- > 0;) {
        const view_rs *o = &vs->rs[vs->order[k]];
        if (o->ok && o->copy.target.id == s->copy.target.id)
            return DE_ACCESS_SHADER_READ;
    }
    return s->target_access;
}

/* Builds, compiles and executes the graph of the view at position `pos` of the order. */
static void render_view(de_renderer *r, de__views *vs, de_arena *arena,
                        const de__render_world_frame *world, u32 pos, u64 frame_index) {
    const u32 slot = vs->order[pos];
    view_rs *s = &vs->rs[slot];
    const de_view handle = make_handle(slot, s->gen);
    s->ok = false;

    /* Where it goes: its target, or the swapchain / offscreen target. */
    u32 tw = vs->out_w, th = vs->out_h;
    de_gpu_format tfmt = vs->out_format;
    const bool rtt = DE_HANDLE_VALID(s->copy.target);
    if (rtt) {
        const de__gpu_texture *t = de__gpu_get_texture(r->dev, s->copy.target);
        if (t == NULL) {
            (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "the target texture was destroyed");
            view_failed(r, s, "render-to-texture target");
            return;
        }
        tw = t->width;
        th = t->height;
        tfmt = t->format;
    }
    if (tw == 0 || th == 0)
        return;
    s->eff = effective_settings(r, &s->copy, &r->frame);
    viewport_px(s->copy.viewport, tw, th, s->vp);
    s->w = round_px((f32)s->vp[2] * s->eff.render_scale, 1u, 16384u);
    s->h = round_px((f32)s->vp[3] * s->eff.render_scale, 1u, 16384u);
    const bool full = s->vp[0] == 0 && s->vp[1] == 0 && s->vp[2] == tw && s->vp[3] == th;
    if (rtt && !de__composite_ready(vs->composite)) {
        if (!s->failed_logged)
            DE_WARN(r->log, "renderer: view \"%s\" cannot reach its target without the composite",
                    s->copy.name);
        s->failed_logged = true;
        return;
    }
    if (!ensure_objects(r, s))
        return;

    de_rg *g = s->graph;
    if (de__rg_begin(g, arena) != DE_OK) {
        view_failed(r, s, "graph begin");
        return;
    }
    de__view_frame vf;
    memset(&vf, 0, sizeof vf);
    vf.renderer = r;
    vf.view = handle;
    vf.graph = g;
    vf.name = s->copy.name;
    vf.world = world;
    vf.camera = &world->views[s->camera];
    vf.settings = &s->eff;
    vf.frame_index = frame_index;
    vf.width = s->w;
    vf.height = s->h;
    memcpy(vf.viewport_px, s->vp, sizeof vf.viewport_px);
    matrices(vf.camera, s->vp, &vf);
    vf.prev_view_proj = s->has_prev ? s->prev_view_proj : vf.view_proj;
    const u32 t_access = rtt ? target_access_before(vs, slot, pos) : DE_ACCESS_NONE;
    de_rg_resource target_res = {0};
    if (rtt) {
        target_res = de_rg_import_texture(g, s->copy.target, t_access);
        de_rg_set_name(g, target_res, "view.target");
    }
    vf.output = de_rg_import_texture(g, s->output, s->out_access);
    de_rg_set_name(g, vf.output, "view.output");
    vf.color = vf.output;
    vf.color_format = DE__VIEW_OUTPUT_FORMAT;

    /* The stages and hooks, in slot, order, phase and registration order. */
    vs->building = &vf;
    for (u32 i = 0; i < vs->list_count; ++i) {
        const seq_entry *e = &vs->list[i];
        if (e->hook)
            e->hook_fn(g, handle, e->user);
        else
            e->stage(&vf, e->user);
    }
    vs->building = NULL;

    if (vf.color.id != vf.output.id && DE_HANDLE_VALID(vf.color)) {
        const de__composite_item it = {.src = vf.color, .rect = {0.0f, 0.0f, (f32)s->w, (f32)s->h}};
        (void)de__composite_add(vs->composite, g, arena, "view.resolve", vf.output,
                                DE__VIEW_OUTPUT_FORMAT, (const f32[4]){0.0f, 0.0f, 0.0f, 1.0f}, &it,
                                1);
    }
    if (rtt) {
        const de__composite_item it = {
            .src = vf.output, .rect = {(f32)s->vp[0], (f32)s->vp[1], (f32)s->vp[2], (f32)s->vp[3]}};
        (void)de__composite_add(vs->composite, g, arena, "view.to_target", target_res, tfmt,
                                full ? (const f32[4]){0.0f, 0.0f, 0.0f, 1.0f} : NULL, &it, 1);
    }
    if (rtt)
        de_rg_export(g, target_res, DE_ACCESS_SHADER_READ);
    de_rg_export(g, vf.output, DE_ACCESS_SHADER_READ);

    de_result res = de__rg_finalize(g);
    if (res == DE_OK)
        res = de__rg_compile(g);
    if (res == DE_OK) {
        res = de__rg_execute(g, &(de__rg_execute_desc){.frame_index = frame_index});
        /* Submitted (perhaps partly): the states below are what the epilogue leaves, or, when a
         * list failed, unknown, so the next frame discards. */
        const u32 after = res == DE_OK ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE;
        s->out_access = after;
        if (rtt)
            s->target_access = after;
        s->ok = res == DE_OK;
    }
    de__rg_reset(g);
    if (res != DE_OK) {
        view_failed(r, s, "frame graph");
        return;
    }
    s->failed_logged = false;
    ++s->executed;
    s->prev_view_proj = vf.view_proj;
    s->has_prev = true;
}

/* ---------- Render thread: the frame ---------- */

/* Binds each camera of `world` to its view (zero: the main view); the first camera wins. */
static void bind_cameras(de__views *vs, const de__render_world_frame *world) {
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        vs->rs[i].active = false;
    if (world == NULL)
        return;
    for (u32 k = 0; k < world->view_count; ++k) {
        const de_view want = world->views[k].camera.view;
        view_rs *s = rs_of(vs, DE_HANDLE_VALID(want) ? want : vs->main_view);
        if (s != NULL && !s->active) {
            s->active = true;
            s->camera = k;
        }
    }
}

/* The active views by (order, creation sequence). */
static void sort_views(de__views *vs) {
    u32 n = 0;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        if (vs->rs[i].live && vs->rs[i].active)
            vs->order[n++] = i;
    for (u32 i = 1; i < n; ++i) {
        const u32 x = vs->order[i];
        const view_rs *a = &vs->rs[x];
        u32 j = i;
        while (j > 0) {
            const view_rs *b = &vs->rs[vs->order[j - 1]];
            if (b->copy.order < a->copy.order ||
                (b->copy.order == a->copy.order && b->copy.seq <= a->copy.seq))
                break;
            vs->order[j] = vs->order[j - 1];
            --j;
        }
        vs->order[j] = x;
    }
    vs->order_count = n;
}

/* Appends the published timings of `g` to the frame's stats. */
static void add_stats(de_renderer *r, de__views *vs, const de_rg *g, u32 *count) {
    const de__rg_timings *t = de__rg_timings_get(g);
    if (t == NULL || !t->valid)
        return;
    r->stats.gpu_ms += t->gpu_ms;
    r->stats.cpu_record_ms += t->cpu_record_ms;
    const u32 need = *count + t->pass_count;
    if (need > vs->stat_cap) {
        u32 cap = vs->stat_cap != 0 ? vs->stat_cap : 32u;
        while (cap < need)
            cap *= 2u;
        const char **names = (const char **)DE_ALLOC(r->alloc, sizeof(char *) * cap,
                                                     DE_ALIGNOF(const char *), DE_MEM_RENDER);
        f32 *ms = (f32 *)DE_ALLOC(r->alloc, sizeof(f32) * cap, DE_ALIGNOF(f32), DE_MEM_RENDER);
        if (names == NULL || ms == NULL) {
            if (names != NULL)
                DE_FREE(r->alloc, (void *)names, sizeof(char *) * cap);
            if (ms != NULL)
                DE_FREE(r->alloc, ms, sizeof(f32) * cap);
            return;
        }
        if (*count > 0) {
            memcpy((void *)names, (const void *)vs->stat_names, sizeof(char *) * *count);
            memcpy(ms, vs->stat_ms, sizeof(f32) * *count);
        }
        if (vs->stat_cap > 0) {
            DE_FREE(r->alloc, (void *)vs->stat_names, sizeof(char *) * vs->stat_cap);
            DE_FREE(r->alloc, vs->stat_ms, sizeof(f32) * vs->stat_cap);
        }
        vs->stat_names = names;
        vs->stat_ms = ms;
        vs->stat_cap = cap;
    }
    for (u32 i = 0; i < t->pass_count; ++i) {
        vs->stat_names[*count + i] = t->pass_names[i];
        vs->stat_ms[*count + i] = t->pass_gpu_ms[i];
    }
    *count = need;
}

/* The output graph: clear `target`, composite the swapchain views, leave it in `final_access`. */
static de_result render_output(de_renderer *r, de__views *vs, de_arena *arena,
                               de_gpu_texture target, u32 final_access, u64 frame_index) {
    de_rg *g = r->graph;
    de_result res = de__rg_begin(g, arena);
    if (res != DE_OK)
        return res;
    const de_rg_resource dst = de_rg_import_texture(g, target, DE_ACCESS_NONE);
    de_rg_set_name(g, dst, DE_HANDLE_VALID(r->swapchain) ? "swapchain" : "offscreen");
    de__composite_item items[DE__VIEW_MAX];
    u32 n = 0;
    for (u32 k = 0; k < vs->order_count; ++k) {
        const view_rs *s = &vs->rs[vs->order[k]];
        if (DE_HANDLE_VALID(s->copy.target) || !DE_HANDLE_VALID(s->output) ||
            s->out_access != DE_ACCESS_SHADER_READ)
            continue;
        const de_rg_resource src = de_rg_import_texture(g, s->output, DE_ACCESS_SHADER_READ);
        de_rg_set_name(g, src, s->copy.name);
        items[n++] = (de__composite_item){
            .src = src, .rect = {(f32)s->vp[0], (f32)s->vp[1], (f32)s->vp[2], (f32)s->vp[3]}};
    }
    if (n > 0 && !de__composite_ready(vs->composite) && !vs->composite_warned) {
        DE_WARN(r->log, "renderer: %u view(s) rendered but not composited (no composite shader)",
                n);
        vs->composite_warned = true;
    }
    (void)de__composite_add(vs->composite, g, arena, "output", dst, vs->out_format, r->clear_color,
                            items, n);
    de_rg_export(g, dst, final_access);
    res = de__rg_finalize(g);
    if (res == DE_OK)
        res = de__rg_compile(g);
    if (res == DE_OK)
        res = de__rg_execute(g, &(de__rg_execute_desc){.frame_index = frame_index});
    de__rg_reset(g);
    return res;
}

void de__views_set_output_size(de_renderer *r, u32 width, u32 height, de_gpu_format format) {
    de__views *vs = views_of(r);
    if (vs == NULL)
        return;
    vs->out_w = width;
    vs->out_h = height;
    vs->out_format = format;
}

de_result de__views_render(de_renderer *r, de_arena *arena, de_gpu_texture target, u32 final_access,
                           u64 frame_index) {
    de__views *vs = r->views;
    sync(r, vs);
    collect_retired(r, vs);
    const de__render_world_frame *world = de__render_world_current(r);
    bind_cameras(vs, world);
    sort_views(vs);

    r->stats.gpu_ms = 0.0f;
    r->stats.cpu_record_ms = 0.0f;
    u32 stat_count = 0;
    for (u32 k = 0; k < vs->order_count; ++k) {
        render_view(r, vs, arena, world, k, frame_index);
        const view_rs *s = &vs->rs[vs->order[k]];
        if (s->ok)
            add_stats(r, vs, s->graph, &stat_count);
    }
    vs->last_order_count = vs->order_count;
    for (u32 k = 0; k < vs->order_count; ++k)
        vs->last_order[k] = make_handle(vs->order[k], vs->rs[vs->order[k]].gen);

    const de_result out = render_output(r, vs, arena, target, final_access, frame_index);
    if (out == DE_OK) {
        add_stats(r, vs, r->graph, &stat_count);
    } else {
        /* Nothing (or not all) of the output graph ran: the target must still end in its final
         * state (a swapchain image must be presentable). */
        de_gpu_cmd *cmd = de_gpu_cmd_begin(r->dev, DE_QUEUE_GRAPHICS, "frame fallback");
        if (cmd != NULL) {
            const de_gpu_barrier b = {
                .texture = target, .before = DE_ACCESS_NONE, .after = final_access};
            de_gpu_cmd_barriers(cmd, &b, 1);
            (void)de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS,
                                      &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
        }
    }
    /* The frame fence: after everything of the frame, on the graphics queue (every graph joins
     * its async queues back to graphics at its end). Exactly once per frame. */
    const de_result fr = de_gpu_queue_submit(
        r->dev, DE_QUEUE_GRAPHICS,
        &(de_gpu_submit){.signal = r->frame_fence, .signal_value = frame_index + 1u});
    r->stats.pass_count = stat_count;
    r->stats.pass_names = stat_count > 0 ? vs->stat_names : NULL;
    r->stats.pass_gpu_ms = stat_count > 0 ? vs->stat_ms : NULL;
    if (out != DE_OK)
        return DE_FAIL_WRAP(out, "renderer: frame %llu: the output graph",
                            (unsigned long long)frame_index);
    return fr == DE_OK ? DE_OK
                       : DE_FAIL_WRAP(fr, "renderer: frame %llu: frame fence",
                                      (unsigned long long)frame_index);
}

/* ---------- Create / destroy ---------- */

de_result de__views_create(de_renderer *r) {
    de__views *vs = DE_NEW(r->alloc, de__views, DE_MEM_RENDER);
    if (vs == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "views");
    memset(vs, 0, sizeof *vs);
    de_mutex_init(&vs->lock);
    r->views = vs;
    de_result res = de__composite_create(r, &vs->composite);
    if (res != DE_OK)
        return res;
    vs->main_view = de_view_create(r, &(de_view_desc){.name = "main"});
    if (!DE_HANDLE_VALID(vs->main_view))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "views: the main view");
    /* Temporary view content until the visibility buffer (S13): a debug clear. */
    res = de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, INT32_MIN, "view.debug_clear",
                                  debug_clear_stage, NULL);
    if (res != DE_OK)
        return res;
    /* DENG-229: the visibility-buffer frame registers its stage "vis.frame". */
    return de__vis_frame_create(r, &vs->vis);
}

void de__views_destroy(de_renderer *r) {
    de__views *vs = r->views;
    if (vs == NULL)
        return;
    de__vis_frame_destroy(r, vs->vis); /* DENG-229 */
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        release_now(r, vs->rs[i].graph, vs->rs[i].output);
    for (u32 i = 0; i < vs->retired_count; ++i)
        release_now(r, vs->retired[i].graph, vs->retired[i].output);
    de__composite_destroy(r, vs->composite);
    if (vs->stat_cap > 0) {
        DE_FREE(r->alloc, (void *)vs->stat_names, sizeof(char *) * vs->stat_cap);
        DE_FREE(r->alloc, vs->stat_ms, sizeof(f32) * vs->stat_cap);
    }
    de_mutex_destroy(&vs->lock);
    DE_FREE(r->alloc, vs, sizeof *vs);
    r->views = NULL;
}

/* ---------- Test hooks ---------- */

bool de__view_info_get(const de_renderer *r, de_view v, de__view_info *out) {
    const de__views *vs = views_of(r);
    const view_rs *s = vs != NULL ? rs_of(vs, v) : NULL;
    if (s == NULL || out == NULL)
        return false;
    memset(out, 0, sizeof *out);
    out->active = s->active && s->ok;
    out->width = s->w;
    out->height = s->h;
    memcpy(out->viewport_px, s->vp, sizeof out->viewport_px);
    out->executed = (u32)s->executed;
    out->order = s->copy.order;
    out->settings = s->eff;
    return true;
}

void de__view_graph_stats(const de_renderer *r, de_view v, u64 *compiles, u64 *hits) {
    const de__views *vs = views_of(r);
    const view_rs *s = vs != NULL ? rs_of(vs, v) : NULL;
    const de__rg_stats st =
        s != NULL && s->graph != NULL ? de__rg_compile_stats(s->graph) : (de__rg_stats){0};
    if (compiles != NULL)
        *compiles = st.compiles;
    if (hits != NULL)
        *hits = st.hits;
}

u32 de__views_retired_count(const de_renderer *r) {
    const de__views *vs = views_of(r);
    return vs != NULL ? vs->retired_count : 0;
}

u32 de__views_last_order(const de_renderer *r, de_view *out, u32 cap) {
    const de__views *vs = views_of(r);
    if (vs == NULL)
        return 0;
    for (u32 k = 0; k < vs->last_order_count && k < cap; ++k)
        out[k] = vs->last_order[k];
    return vs->last_order_count;
}

de__composite *de__views_composite(const de_renderer *r) {
    const de__views *vs = views_of(r);
    return vs != NULL ? vs->composite : NULL;
}
