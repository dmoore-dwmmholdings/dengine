/*
 * render_thread.c: the render thread and the Extract handoff (SPEC §8, §10.2 step 5; DENG-175).
 *
 * Pipelining (Bevy style): the main thread extracts frame N into a render-world buffer and kicks
 * the render thread, which calls de_renderer_render_frame for N while the simulation runs N + 1.
 * The next Extract (in de__rw_begin) first waits until frame N is rendered, so the simulation is
 * exactly one frame ahead and the render thread never reads a buffer that Extract writes.
 *
 * Handoff: two semaphores from the platform layer (no <threads.h>). `kick` (main -> render) is
 * posted once per frame after the buffer pointer is published with a release store; `done`
 * (render -> main) is posted after the frame, after a release increment of `rendered`, which the
 * main thread reads with acquire after the wait. So every write to a buffer happens before the
 * render thread's reads of it, and every render-thread access happens before the buffer is
 * written again, also for tools that do not model semaphores.
 *
 * The thread (named `render`) starts with the first pipelined frame and stops in
 * de_renderer_destroy. `r.pipelined = 0` renders on the calling thread; switching at run time is
 * safe, because Extract always waits for a frame in flight first.
 */
#include "render/world/render_world.h"
#include "render/skin/skin_internal.h"

#include "render/renderer_internal.h"

#include <string.h>

de_result de__rt_init(de__render_world *rw) {
    de__render_thread *t = &rw->rt;
    memset(t, 0, sizeof *t);
    de_atomic_init_ptr(&t->pending, NULL);
    de_atomic_init_u32(&t->stop, 0);
    de_atomic_init_u64(&t->rendered, 0);
    de_atomic_init_u64(&t->completed_extract, 0);
    de_atomic_init_u32(&t->last_result, (u32)DE_OK);
    de_result res = de_sem_init(&t->kick, 0);
    if (res != DE_OK)
        return res;
    res = de_sem_init(&t->done, 0);
    if (res != DE_OK) {
        de_sem_destroy(&t->kick);
        return res;
    }
    t->sems_ok = true;
    return DE_OK;
}

void de__rt_destroy(de__render_world *rw) {
    de__render_thread *t = &rw->rt;
    if (!t->sems_ok)
        return;
    de_sem_destroy(&t->kick);
    de_sem_destroy(&t->done);
    t->sems_ok = false;
}

/* Renders `f` on the calling thread (the render thread, or the main thread unpipelined). */
static de_result render_one(de__render_world *rw, de__render_world_frame *f) {
    de_atomic_store_u32(&f->state, DE__RW_RENDERING, DE_ATOMIC_RELAXED);
    rw->current = f;
    if (rw->hook != NULL)
        rw->hook(rw->r, f, rw->hook_user);
    const de_result res = de_renderer_render_frame(rw->r);
    if (res != DE_OK) {
        const de_error *e = de_error_last();
        DE_ERROR(rw->log, "renderer: frame %llu: %s", (unsigned long long)f->frame,
                 e != NULL ? e->message : "failed");
    }
    de_atomic_store_u64(&rw->rt.completed_extract, f->frame, DE_ATOMIC_RELEASE);
    rw->current = NULL;
    de_atomic_store_u32(&f->state, DE__RW_FREE, DE_ATOMIC_RELEASE);
    return res;
}

static i32 render_thread_main(void *user) {
    de__render_world *rw = (de__render_world *)user;
    de__render_thread *t = &rw->rt;
    de_crash_thread_init();
    for (;;) {
        de_sem_wait(&t->kick);
        if (de_atomic_load_u32(&t->stop, DE_ATOMIC_ACQUIRE) != 0)
            break;
        de__render_world_frame *f =
            (de__render_world_frame *)de_atomic_load_ptr(&t->pending, DE_ATOMIC_ACQUIRE);
        const de_result res = render_one(rw, f);
        de_atomic_store_u32(&t->last_result, (u32)res, DE_ATOMIC_RELAXED);
        de_atomic_fetch_add_u64(&t->rendered, 1u, DE_ATOMIC_RELEASE);
        de_sem_post(&t->done, 1);
    }
    return 0;
}

/* Starts the thread if it does not run. false: render on the calling thread instead. */
static bool start_thread(de__render_world *rw) {
    de__render_thread *t = &rw->rt;
    if (t->thread != NULL)
        return true;
    if (!t->sems_ok)
        return false;
    de_atomic_store_u32(&t->stop, 0, DE_ATOMIC_RELAXED);
    const de_result res =
        de_thread_create(render_thread_main, rw, "render", DE_THREAD_HIGH, 0, &t->thread);
    if (res != DE_OK) {
        t->thread = NULL;
        const de_error *e = de_error_last();
        DE_ERROR(rw->log, "renderer: no render thread (%s); rendering after Extract instead",
                 e != NULL ? e->message : "?");
        return false;
    }
    return true;
}

void de__rt_wait(de__render_world *rw) {
    de__render_thread *t = &rw->rt;
    if (!t->in_flight)
        return;
    de_sem_wait(&t->done);
    (void)de_atomic_load_u64(&t->rendered, DE_ATOMIC_ACQUIRE);
    t->in_flight = false;
}

void de__rt_stop(de__render_world *rw) {
    de__render_thread *t = &rw->rt;
    de__rt_wait(rw);
    if (t->thread == NULL)
        return;
    de_atomic_store_u32(&t->stop, 1, DE_ATOMIC_RELEASE);
    de_sem_post(&t->kick, 1);
    (void)de_thread_join(t->thread);
    t->thread = NULL;
}

/* The Extract systems did not run this frame: a frame with the last extracted state (the pools
 * as they are, the views and the environment of the last frame). */
static de__render_world_frame *repeat_frame(de__render_world *rw) {
    const de__render_world_frame *last = rw->last;
    de__render_world_frame *f = de__rw_begin(rw);
    f->origin = rw->origin;
    if (last != NULL && de__rw_frame_reserve_views(rw, f, last->view_count) == DE_OK) {
        if (last->view_count > 0)
            memcpy(f->views, last->views, sizeof(de__rw_view) * last->view_count);
        f->view_count = last->view_count;
        f->main_view = last->main_view;
        f->frozen_view_count = last->frozen_view_count;
        memcpy(f->frozen_views, last->frozen_views, sizeof f->frozen_views);
        memcpy(f->frozen_settings, last->frozen_settings, sizeof f->frozen_settings);
        memcpy(f->frozen_targets, last->frozen_targets, sizeof f->frozen_targets);
        for (u32 i = 0; i < f->frozen_view_count; ++i)
            f->frozen_views[i].extract_frame = f->frame;
        f->environment = last->environment;
        f->environment_count = last->environment_count;
        if (de__skin_frame_copy(rw->alloc, f, last) != DE_OK)
            DE_ERROR(rw->log, "render world: repeated skin snapshot allocation failed");
    }
    static const de__rw_kind kinds[2] = {DE__RW_KIND_INSTANCE, DE__RW_KIND_LIGHT};
    for (u32 i = 0; i < 2; ++i) {
        de__rw_pool_begin(rw, kinds[i]);
        if (de__rw_pool_end(rw, kinds[i], f, false) != DE_OK)
            DE_ERROR(rw->log, "render world: frame %llu: out of memory",
                     (unsigned long long)rw->frame);
    }
    return f;
}

de_result de__render_extract_and_kick(de_renderer *r, de_world *w) {
    if (r == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__render_extract_and_kick: NULL renderer");
    de__render_world *rw = de__rw_of(r);
    if (rw == NULL) /* never attached: no render world */
        return de_renderer_render_frame(r);
    (void)w; /* the systems know their world; a frame that did not extract repeats the last */
    de__render_world_frame *f = rw->filling != NULL ? rw->filling : repeat_frame(rw);
    rw->filling = NULL;
    rw->last = f;
    r->stats.extract_instances = f->instances.live;
    r->stats.extract_lights = f->lights.live;
    r->stats.extract_cpu_ms = (f32)((f64)(de_time_now_ns() - rw->fill_start_ns) * 1e-6);
    de_atomic_store_u32(&f->state, DE__RW_READY, DE_ATOMIC_RELAXED);
    /* DENG-176: the uploader's flush and acquire run here, on the main thread, while the render
     * thread is idle (de__rw_begin waited for it), never during de_asset_system_update. */
    de__renderer_kick_prepare(r);

    if (de_cvar_bool(rw->cv_pipelined) && start_thread(rw)) {
        /* The frame before is rendered (de__rw_begin waited): report its result once. */
        const de_result prev =
            (de_result)de_atomic_exchange_u32(&rw->rt.last_result, (u32)DE_OK, DE_ATOMIC_RELAXED);
        de_atomic_store_ptr(&rw->rt.pending, f, DE_ATOMIC_RELEASE);
        rw->rt.in_flight = true;
        de_sem_post(&rw->rt.kick, 1);
        return prev == DE_OK ? DE_OK
                             : DE_FAIL(prev, "renderer: frame %llu failed on the render thread",
                                       (unsigned long long)(f->frame - 1u));
    }
    return render_one(rw, f);
}

/* ---------- Consumers and test hooks ---------- */

const de__render_world_frame *de__render_world_current(const de_renderer *r) {
    const de__render_world *rw = de__rw_of(r);
    return rw != NULL ? rw->current : NULL;
}

void de__render_world_set_frame_hook(de_renderer *r, de__render_frame_hook fn, void *user) {
    de__render_world *rw = de__rw_of(r);
    if (rw == NULL)
        return;
    de__rt_wait(rw);
    rw->hook = fn;
    rw->hook_user = user;
}

void de__render_world_wait_idle(de_renderer *r) {
    de__render_world *rw = de__rw_of(r);
    if (rw != NULL)
        de__rt_wait(rw);
}

const de__render_world_frame *de__render_world_last(const de_renderer *r) {
    const de__render_world *rw = de__rw_of(r);
    return rw != NULL ? rw->last : NULL;
}

u64 de__render_world_rendered(const de_renderer *r) {
    const de__render_world *rw = de__rw_of(r);
    return rw != NULL ? de_atomic_load_u64(&rw->rt.rendered, DE_ATOMIC_ACQUIRE) : 0;
}
