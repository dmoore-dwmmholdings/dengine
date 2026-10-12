/*
 * engine.c: de_engine create/destroy (SPEC §10.1, §4.2) and the de_engine calls of de_game.h.
 *
 * The engine owns every module it creates and creates each one with its allocator (§4.2: no
 * hidden globals). The process-wide services (profiler, log, VFS, cvar registry) are not
 * created here: de_host_run starts them before the engine and stops them after it.
 */
#include "host/engine_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_math.h>

#include <string.h>

/* Destroys what `e` holds, in the reverse order of de__engine_create; NULL members are skipped,
 * so it also unwinds a partial create. Frees `e`. */
static void engine_teardown(de__engine *e) {
    if (DE_HANDLE_VALID(e->pub.window)) {
        de_window_destroy(e->pub.window);
        e->pub.window = (de_window){0};
    }
    if (e->pub.world != NULL) {
        de_world_destroy(e->pub.world);
        e->pub.world = NULL;
    }
    if (e->pub.types != NULL) {
        de_type_registry_destroy(e->pub.types);
        e->pub.types = NULL;
    }
    if (e->pub.tasks != NULL) {
        de_task_system_destroy(e->pub.tasks); /* joins the workers */
        e->pub.tasks = NULL;
    }
    if (e->owns_platform) {
        de_platform_shutdown();
        e->owns_platform = false;
    }
    /* Log categories and cvars are process-wide and live on. */
    const de_allocator *a = e->pub.allocator;
    DE_FREE(a, e, sizeof *e);
}

/* Wraps the failed step's error, unwinds, and returns the step's result. */
static de_result engine_fail(de__engine *e, de_result r, const char *step) {
    r = DE_FAIL_WRAP(r, "engine create: %s", step);
    engine_teardown(e);
    return r;
}

de_result de__engine_create(const de__engine_desc *d, de_engine **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__engine_create: out is NULL");
    *out = NULL;
    de__engine_desc desc;
    if (d != NULL)
        desc = *d;
    else
        memset(&desc, 0, sizeof desc);

    /* 1. Allocator. */
    const de_allocator *a = desc.allocator != NULL ? desc.allocator : de_heap_allocator();
    de__engine *e = DE_NEW(a, de__engine, DE_MEM_GENERAL);
    if (e == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "engine create: the engine (%zu bytes)",
                            sizeof(de__engine));
    memset(e, 0, sizeof *e);
    e->pub.allocator = a;
    if (desc.config != NULL)
        e->config = *desc.config;
    else
        de__game_config_defaults(&e->config);
    e->timing = e->config.timing;
    de_atomic_init_u32(&e->quit, 0);
    e->headless = desc.headless;

    /* 2. Log categories. Process-wide: registering an existing name returns it. */
    e->log_host = de_log_register_category("host", DE_LOG_INFO);
    e->log_game = de_log_register_category("game", DE_LOG_INFO);

    /* 3. Cvars: one process-wide registry, created on first use (Decision: Cvars are one
     * process-wide registry with lock-free getters). game.toml's [cvars], user:/config.toml and
     * the command line were applied by de_host_run before the engine; nothing to create. */

    /* 4. Platform. */
    de_platform_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.app_name = e->config.name;
    pd.flags = desc.headless ? (u32)DE_PLATFORM_HEADLESS : 0u;
    pd.allocator = a;
    de_result r = de_platform_init(&pd);
    if (r == DE_OK) {
        e->owns_platform = true;
    } else if (r == DE_ERR_BUSY) {
        /* The caller (a tool, an editor, a test) runs the platform already: use it, and leave
         * its shutdown to the caller. */
        de_error_clear();
        DE_LOG(DE_LOG_DEBUG, e->log_host, "platform already initialised; the engine uses it");
    } else {
        return engine_fail(e, r, "platform init");
    }
    if (de_platform_headless())
        e->headless = true;

    /* 5. Task system. The calling thread becomes frame worker 0 and runs tasks (deterministic
     * FixedUpdate systems among them), so it takes the deterministic FP mode first, like every
     * worker does (SPEC §5.7 rule 4; de_task_system_create checks it). */
    de_fp_env_init_thread();
    de_task_system_desc td;
    memset(&td, 0, sizeof td);
    td.worker_count = e->config.task_workers;
    td.allocator = a;
    r = de_task_system_create(&td, &e->pub.tasks);
    if (r != DE_OK)
        return engine_fail(e, r, "task system");

    /* 6. Type registry. */
    r = de_type_registry_create(a, &e->pub.types);
    if (r != DE_OK)
        return engine_fail(e, r, "type registry");

    /* 7. ECS world, on the task system. */
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.allocator = a;
    wd.types = e->pub.types;
    wd.tasks = e->pub.tasks;
    r = de_world_create(&wd, &e->pub.world);
    if (r != DE_OK)
        return engine_fail(e, r, "ECS world");

    /* 7b. The ECS built-ins (DeTransform, DeGlobalTransform, DeChildOf, DeChildren and the
     * de.transform_propagate PostUpdate system; DENG-351) register on first use. Their component
     * ids are part of the determinism contract, so the engine registers them at one fixed point:
     * right after the world, before the game registers anything and before any schedule runs
     * (Decision: ECS built-in components are registered on first use). */
    de_error_clear();
    de_builtin_component_ids builtins = de_builtin_components(e->pub.world);
    if (!DE_HANDLE_VALID(builtins.transform)) {
        const de_error *err = de_error_last();
        return engine_fail(e, err != NULL ? err->result : DE_ERR_INTERNAL,
                           "ECS built-in components");
    }

    /* 8. Window, unless headless. The renderer (S12) picks the surface; until then the window
     * has none. Assets (S07) and the optional modules stay NULL. */
    if (!e->headless) {
        de_window_desc wnd;
        memset(&wnd, 0, sizeof wnd);
        wnd.title = e->config.window_title[0] != '\0' ? e->config.window_title : e->config.name;
        wnd.width = e->config.window_width;
        wnd.height = e->config.window_height;
        wnd.mode = e->config.window_mode;
        wnd.display_index = -1;
        wnd.resizable = true;
        wnd.high_dpi = e->config.window_high_dpi;
        r = de_window_create(&wnd, &e->pub.window);
        if (r != DE_OK)
            return engine_fail(e, r, "window");
    }

    DE_LOG(DE_LOG_DEBUG, e->log_host, "engine created: %u frame workers%s",
           de_task_worker_count(e->pub.tasks), e->headless ? ", headless" : "");
    *out = &e->pub;
    return DE_OK;
}

void de__engine_destroy(de_engine *e) {
    if (e == NULL)
        return;
    engine_teardown(de__engine_from(e));
}

/* ---- de_game.h -------------------------------------------------------------------------- */

void de_engine_set_timing(de_engine *e, const de_timing *t) {
    if (e == NULL || t == NULL)
        return;
    de_timing v = *t;
    if (v.fixed_hz == 0)
        v.fixed_hz = DE_TIMING_DEFAULT_FIXED_HZ;
    if (v.max_catchup_steps == 0) /* DENG-91: zero fields keep their defaults */
        v.max_catchup_steps = DE_TIMING_DEFAULT_MAX_CATCHUP_STEPS;
    if (!(v.time_scale >= 0.0f)) /* negative or NaN */
        v.time_scale = DE_TIMING_DEFAULT_TIME_SCALE;
    de__engine_from(e)->timing = v;
}

de_timing de_engine_get_timing(const de_engine *e) {
    if (e == NULL) {
        de_timing zero;
        memset(&zero, 0, sizeof zero);
        return zero;
    }
    return de__engine_from_const(e)->timing;
}

void de_engine_request_quit(de_engine *e) {
    if (e == NULL)
        return;
    de_atomic_store_u32(&de__engine_from(e)->quit, 1u, DE_ATOMIC_RELEASE);
}

bool de_engine_quit_requested(const de_engine *e) {
    if (e == NULL)
        return false;
    return de_atomic_load_u32(&de__engine_from_const(e)->quit, DE_ATOMIC_ACQUIRE) != 0;
}
