/*
 * loop.c: the main loop (SPEC §10.2). Owner: DENG-91.
 *
 * Startup once, then per frame:
 *   0. pacer begin, frame delta from the loop clock, timing changes (de_engine_set_timing) apply
 *   1. de_platform_pump (DE_EV_QUIT asks to quit; every app event also goes to
 *      de_input_app_event), input update (de_input_begin_frame, DENG-84), the asset system
 *      update (de_asset_system_update, DENG-143), frame-begin hooks
 *   2. First, PreUpdate
 *   3. FixedUpdate 0..N times (timing.c decides N, dt and alpha)
 *   4. Update, PostUpdate
 *   5. Extract, render thread kick (de__render_extract_and_kick, DENG-175)
 *   6. Last, frame-end hooks, frame arena swap, frame pacing
 *
 * Nothing here allocates per frame: the hook lists are fixed arrays in de__host, the event
 * buffer is on the stack, the timing state and the pacer are locals, and the frame arenas use
 * virtual memory. The only log calls in steady state are rate-limited warnings.
 */
#define _CRT_SECURE_NO_WARNINGS /* strtoull is fine; MSVC wants the _s forms otherwise */
#include "host/host_internal.h"
#include "host/timing.h"

#include <dengine/de_debug.h>
#include <dengine/de_anim.h>
#include "anim/anim_host.h"
#include <dengine/de_text.h>
#include <dengine/de_input.h> /* DENG-84 */
#if DE_HAS_RENDER
#include "render/world/render_world.h" /* DENG-175: de__render_extract_and_kick */
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_schedule_names[DE_SCHEDULE_COUNT_] = {
    "Startup", "First", "PreUpdate", "FixedUpdate", "Update", "PostUpdate", "Extract", "Last"};

/* App events taken per de_platform_pump call; the loop calls again while the buffer fills. */
#define LOOP_EVENT_BATCH 16u

/* The "fell behind" warning comes at most once per this much real time. */
#define LOOP_WARN_INTERVAL_NS (5ull * 1000000000ull)

/* ---- Hooks ------------------------------------------------------------------------------ */

static de_result loop_add_hook(de__loop_hook *list, u32 *count, de__loop_hook_fn fn, void *user,
                               const char *which) {
    if (fn == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__loop_add_%s_hook: fn is NULL", which);
    if (*count >= DE__LOOP_MAX_HOOKS)
        return DE_FAIL(DE_ERR_BUSY, "de__loop_add_%s_hook: %u hooks already (max)", which,
                       DE__LOOP_MAX_HOOKS);
    list[*count].fn = fn;
    list[*count].user = user;
    ++*count;
    return DE_OK;
}

de_result de__loop_add_frame_end_hook(de__host *h, de__loop_hook_fn fn, void *user) {
    if (h == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__loop_add_frame_end_hook: h is NULL");
    return loop_add_hook(h->loop_hooks.frame_end, &h->loop_hooks.frame_end_count, fn, user,
                         "frame_end");
}

de_result de__loop_add_frame_begin_hook(de__host *h, de__loop_hook_fn fn, void *user) {
    if (h == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__loop_add_frame_begin_hook: h is NULL");
    return loop_add_hook(h->loop_hooks.frame_begin, &h->loop_hooks.frame_begin_count, fn, user,
                         "frame_begin");
}

/* Runs one hook list in order. The count is read on every step, so a hook added by a hook runs
 * in the same pass. */
static de_result loop_run_hooks(de__host *h, const de__loop_hook *list, const u32 *count,
                                const char *which, u64 frame) {
    for (u32 i = 0; i < *count; ++i) {
        de_result r = list[i].fn(h, list[i].user);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "main loop: frame %llu: %s hook %u", (unsigned long long)frame,
                                which, i);
    }
    return DE_OK;
}

/* ---- Command line ----------------------------------------------------------------------- */

/* `--frames N` or `--frames=N` (N >= 1); the last one wins. `*out` is left alone when absent. */
static de_result loop_arg_frames(int argc, char **argv, u64 *out) {
    static const char opt[] = "--frames";
    const size_t opt_len = sizeof opt - 1;
    for (int i = 1; i < argc && argv != NULL; ++i) {
        const char *arg = argv[i];
        if (arg == NULL || strncmp(arg, opt, opt_len) != 0)
            continue;
        const char *v;
        if (arg[opt_len] == '=') {
            v = arg + opt_len + 1;
        } else if (arg[opt_len] == '\0') {
            if (i + 1 >= argc || argv[i + 1] == NULL)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "command line: --frames needs a value");
            v = argv[++i];
        } else {
            continue; /* --framesfoo: not ours */
        }
        char *end = NULL;
        errno = 0;
        unsigned long long n = (v[0] >= '0' && v[0] <= '9') ? strtoull(v, &end, 10) : 0;
        if (end == NULL || end == v || *end != '\0' || errno != 0 || n == 0)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "command line: --frames needs a whole number of at least 1, not '%s'",
                           v);
        *out = (u64)n;
    }
    return DE_OK;
}

/* ---- Frame steps ------------------------------------------------------------------------ */

static u64 loop_now(const de__host *h) {
    return h->loop_clock != NULL ? h->loop_clock(h->loop_clock_user) : de_time_now_ns();
}

/* One profiler zone per schedule: DE_PROFILE_ZONE_BEGIN needs a string literal per call site. */
#define LOOP_ZONED_RUN(lit, s)                                                                     \
    do {                                                                                           \
        DE_PROFILE_ZONE_BEGIN(z_sched_, lit);                                                      \
        r = de_world_run_schedule(w, (s), dt, alpha);                                              \
        DE_PROFILE_ZONE_END(z_sched_);                                                             \
    } while (0)

static de_result loop_schedule(de_world *w, de_schedule s, f32 dt, f32 alpha, u64 frame) {
    de_result r = DE_OK;
    switch (s) {
    case DE_SCHEDULE_STARTUP:
        LOOP_ZONED_RUN("Startup", s);
        break;
    case DE_SCHEDULE_FIRST:
        LOOP_ZONED_RUN("First", s);
        break;
    case DE_SCHEDULE_PRE_UPDATE:
        LOOP_ZONED_RUN("PreUpdate", s);
        break;
    case DE_SCHEDULE_FIXED_UPDATE:
        LOOP_ZONED_RUN("FixedUpdate", s);
        break;
    case DE_SCHEDULE_UPDATE:
        LOOP_ZONED_RUN("Update", s);
        break;
    case DE_SCHEDULE_POST_UPDATE:
        LOOP_ZONED_RUN("PostUpdate", s);
        break;
    case DE_SCHEDULE_EXTRACT:
        LOOP_ZONED_RUN("Extract", s);
        break;
    case DE_SCHEDULE_LAST:
        LOOP_ZONED_RUN("Last", s);
        break;
    default:
        return DE_FAIL(DE_ERR_INTERNAL, "main loop: unknown schedule %d", (int)s);
    }
    if (r != DE_OK) {
        if (s == DE_SCHEDULE_STARTUP)
            return DE_FAIL_WRAP(r, "main loop: schedule Startup");
        return DE_FAIL_WRAP(r, "main loop: frame %llu: schedule %s", (unsigned long long)frame,
                            k_schedule_names[s]);
    }
    return DE_OK;
}

/* Step 1: drains the platform's event queue. DE_EV_QUIT (last window closed, Cmd-Q, SIGINT,
 * SIGTERM) asks the loop to stop after this frame. The other app events have no consumer in the
 * host yet (window resize goes to the renderer in S12). */
static void loop_pump(de__host *h) {
    de_app_event ev[LOOP_EVENT_BATCH];
    u32 n;
    do {
        n = de_platform_pump(ev, LOOP_EVENT_BATCH);
        for (u32 i = 0; i < n; ++i) {
            if (ev[i].kind == DE_EV_QUIT) {
                DE_INFO(h->log, "quit requested by the platform");
                de_engine_request_quit(h->engine);
            }
            /* DENG-84: window focus reaches the input state (a focus loss releases held keys). */
            if (h->engine->input != NULL)
                de_input_app_event(h->engine->input, &ev[i]);
        }
    } while (n == LOOP_EVENT_BATCH);
}

/* Step 1, second half: the input system's per-frame update (SPEC §15). The pump routed raw input
 * into the platform's ring; de_input_begin_frame drains it into the input state (DENG-84). The
 * action maps are evaluated later, by the input.actions system in PreUpdate. */
static void loop_input_update(de__host *h) {
    if (h->engine->input != NULL)
        de_input_begin_frame(h->engine->input);
}

/* Step 5, second half: kicks the render thread to record the extracted frame (DENG-175; a frame
 * error is logged by the renderer). Nothing without a renderer. */
static void loop_render_kick(de__host *h) {
#if DE_HAS_RENDER
    if (h->engine->renderer != NULL)
        (void)de__render_extract_and_kick(h->engine->renderer, h->engine->world);
#else
    (void)h;
#endif
}

/* Rate-limited "fell behind" warning state. */
typedef struct loop_drop_warn {
    u64 last_ns;    /* real time of the last warning; 0 = never */
    u64 dropped_ns; /* scaled ns dropped since the last warning */
    u32 frames;     /* frames that dropped time since the last warning */
} loop_drop_warn;

static void loop_note_drop(de__host *h, loop_drop_warn *dw, const de__timing_state *ts,
                           u64 dropped_ns) {
    dw->dropped_ns += dropped_ns;
    ++dw->frames;
    const u64 now = de_time_now_ns();
    if (dw->last_ns != 0 && now - dw->last_ns < LOOP_WARN_INTERVAL_NS)
        return;
    DE_WARN(h->log,
            "main loop fell behind: dropped %.1f ms of simulation time in %u frame(s) "
            "(timing.max_catchup_steps = %u at %u Hz)",
            (double)dw->dropped_ns * 1e-6, dw->frames, ts->timing.max_catchup_steps,
            ts->timing.fixed_hz);
    dw->last_ns = now;
    dw->dropped_ns = 0;
    dw->frames = 0;
}

static void loop_pacer_desc(u32 cap_hz, de_frame_pacer_desc *out) {
    memset(out, 0, sizeof *out);
    out->target_hz = cap_hz;
    out->mode = cap_hz != 0 ? DE_FRAME_PACER_CAP : DE_FRAME_PACER_NONE;
}

/* ---- The loop --------------------------------------------------------------------------- */

de_result de__loop_run(de__host *h) {
    if (h == NULL || h->engine == NULL || h->engine->world == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "main loop: no engine");
    de_engine *e = h->engine;
    de_world *w = e->world;
    if (h->log.id == 0)
        h->log = de_log_register_category("host", DE_LOG_INFO);
    h->loop_frames = 0;

    u64 max_frames = h->loop_max_frames;
    DE_TRY(loop_arg_frames(h->desc.argc, h->desc.argv, &max_frames));

    /* Frame arenas are process-wide (Decision: Frame arenas and thread scratch are process-wide
     * core state). Like the other process-wide services, the loop creates them only when the
     * caller has not, and destroys only what it created. */
    bool own_arenas = false;
    de_result r = de_frame_arenas_init(0);
    if (r == DE_OK) {
        own_arenas = true;
    } else if (r == DE_ERR_ALREADY_EXISTS) {
        de_error_clear();
    } else {
        return DE_FAIL_WRAP(r, "main loop: frame arenas");
    }
    u64 frame = 0;
    de_frame_arena_begin(frame);

    /* [memory] frame_arena_mib: the frame arenas reserve their own address space, so this is a
     * budget, checked once per frame and warned about once per run. */
    const de__engine *ie = de__engine_from_const(e);
    const u64 arena_budget = (u64)ie->config.frame_arena_mib * 1024u * 1024u;
    bool arena_warned = false;

    de_timing t = de_engine_get_timing(e);
    de__timing_state ts;
    de__timing_init(&ts, &t);
    de_frame_pacer pacer;
    de_frame_pacer_desc pd;
    loop_pacer_desc(ts.timing.frame_cap_hz, &pd);
    de_frame_pacer_init(&pacer, &pd);
    loop_drop_warn dw;
    memset(&dw, 0, sizeof dw);

    r = loop_schedule(w, DE_SCHEDULE_STARTUP, 0.0f, 0.0f, 0);

    u64 last_ns = loop_now(h);
    bool first = true;
    while (r == DE_OK && !de_engine_quit_requested(e) &&
           (max_frames == 0 || h->loop_frames < max_frames)) {
        /* 0. Frame start. */
        de_frame_pacer_begin(&pacer);
        const u64 now_ns = loop_now(h);
        t = de_engine_get_timing(e);
        de__timing_set(&ts, &t);
        if (ts.timing.frame_cap_hz != pd.target_hz) {
            loop_pacer_desc(ts.timing.frame_cap_hz, &pd);
            de_frame_pacer_set(&pacer, &pd);
        }
        /* The first frame has no previous frame: it counts as one fixed step, so frame 0 runs
         * exactly one FixedUpdate (and variable mode gets a sensible dt) instead of none. */
        const u64 real_dt = first ? de__timing_step_ns(&ts) : now_ns - last_ns;
        last_ns = now_ns;
        first = false;
        de__timing_frame tf;
        de__timing_advance(&ts, real_dt, &tf);
        if (tf.dropped_ns != 0)
            loop_note_drop(h, &dw, &ts, tf.dropped_ns);

        /* 1. Events and input, then the frame-begin hooks. */
        {
            DE_PROFILE_ZONE_BEGIN(z_pump, "Pump");
            loop_pump(h);
            loop_input_update(h);
            DE_PROFILE_ZONE_END(z_pump);
        }
#if DE_HAS_ASSETS
        /* DENG-143: finished loads are published and their events queued before First, so this
         * frame's systems see the new load states; de.asset_events sends the events in First. */
        if (e->assets != NULL) {
            DE_PROFILE_ZONE_BEGIN(z_assets, "AssetUpdate");
            de_asset_system_update(e->assets, w);
            DE_PROFILE_ZONE_END(z_assets);
        }
#endif
        if (e->text)
            de_text_update(e->text); /* DENG-257: publish font generations before workers. */
        r = loop_run_hooks(h, h->loop_hooks.frame_begin, &h->loop_hooks.frame_begin_count,
                           "frame-begin", frame);
        if (r != DE_OK)
            break;

        /* 2. First, PreUpdate. The interpolation alpha belongs to the schedules after
         * FixedUpdate; before it they get 0. */
        r = loop_schedule(w, DE_SCHEDULE_FIRST, tf.frame_dt, 0.0f, frame);
        if (r == DE_OK)
            r = loop_schedule(w, DE_SCHEDULE_PRE_UPDATE, tf.frame_dt, 0.0f, frame);

        /* 3. FixedUpdate, 0..N times with the constant fixed dt (variable mode: once). */
        for (u32 i = 0; r == DE_OK && i < tf.steps; ++i)
            r = loop_schedule(w, DE_SCHEDULE_FIXED_UPDATE, tf.step_dt, 0.0f, frame);

        /* DENG-201: games may create or replace the optional renderer after host startup. */
        if (e->anim)
            de__anim_bind_renderer(e->anim, e->renderer);
        /* 4. Update, PostUpdate (transform propagation runs in PostUpdate). */
        if (r == DE_OK)
            r = loop_schedule(w, DE_SCHEDULE_UPDATE, tf.frame_dt, tf.alpha, frame);
        if (r == DE_OK)
            r = loop_schedule(w, DE_SCHEDULE_POST_UPDATE, tf.frame_dt, tf.alpha, frame);

        /* 5. Extract into the render world, then start the render thread. */
        if (r == DE_OK)
            r = loop_schedule(w, DE_SCHEDULE_EXTRACT, tf.frame_dt, tf.alpha, frame);
        if (r == DE_OK)
            loop_render_kick(h);

        /* 6. Last, frame-end hooks, frame arena swap, pacing. */
        if (r == DE_OK)
            r = loop_schedule(w, DE_SCHEDULE_LAST, tf.frame_dt, tf.alpha, frame);
        if (r != DE_OK)
            break;
        r = loop_run_hooks(h, h->loop_hooks.frame_end, &h->loop_hooks.frame_end_count, "frame-end",
                           frame);
        if (r != DE_OK)
            break;

        if (!arena_warned) {
            de_arena *fa = de_frame_arena();
            if (fa != NULL) {
                const de_arena_info ai = de_arena_get_info(fa);
                if ((u64)ai.used > arena_budget) {
                    DE_WARN(h->log,
                            "frame %llu used %.1f MiB of frame arena, more than "
                            "memory.frame_arena_mib = %u (warned once)",
                            (unsigned long long)frame, (double)ai.used / (1024.0 * 1024.0),
                            ie->config.frame_arena_mib);
                    arena_warned = true;
                }
            }
        }
        /* The next frame's arena is reset and made current: what frame N pushed stays valid
         * through frame N + count - 1 (count = 2: one frame in flight). */
        ++frame;
        de_frame_arena_begin(frame);
        ++h->loop_frames;

        DE_PROFILE_FRAME(NULL);
        /* No wait after the last frame: shutdown need not sit out the rest of a capped frame. */
        if (de_engine_quit_requested(e) || (max_frames != 0 && h->loop_frames >= max_frames))
            break;
        {
            DE_PROFILE_ZONE_BEGIN(z_pace, "FramePacing");
            de_frame_pacer_wait(&pacer);
            DE_PROFILE_ZONE_END(z_pace);
        }
    }

    if (own_arenas)
        de_frame_arenas_shutdown();
    h->loop_fixed_steps = ts.steps_total; /* DENG-95: the host's exit summary */
    if (r == DE_OK)
        DE_LOG(DE_LOG_DEBUG, h->log, "main loop: %llu frames, %llu FixedUpdate steps",
               (unsigned long long)h->loop_frames, (unsigned long long)ts.steps_total);
    return r;
}
