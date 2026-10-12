/*
 * loop.c: the main loop (SPEC §10.2). Owner of the body: DENG-91.
 *
 * DENG-90 stub: STARTUP once, then one frame of every other schedule, so de_host_run can run a
 * game end to end before the real loop (timing modes, catch-up, alpha, pacing, quit) exists.
 */
#include "host/host_internal.h"

static const char *const k_schedule_names[DE_SCHEDULE_COUNT_] = {
    "Startup", "First", "PreUpdate", "FixedUpdate", "Update", "PostUpdate", "Extract", "Last"};

de_result de__loop_run(de__host *h) {
    de_world *w = h->engine->world;
    de_timing t = de_engine_get_timing(h->engine);
    f32 dt = 1.0f / (f32)(t.fixed_hz != 0 ? t.fixed_hz : DE_TIMING_DEFAULT_FIXED_HZ);
    for (int s = DE_SCHEDULE_STARTUP; s < DE_SCHEDULE_COUNT_; ++s) {
        de_result r =
            de_world_run_schedule(w, (de_schedule)s, s == DE_SCHEDULE_STARTUP ? 0.0f : dt, 0.0f);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "main loop: schedule %s", k_schedule_names[s]);
    }
    return DE_OK;
}
