/*
 * engine.c: de_engine create/destroy (SPEC §10.1, §4.2) and the de_engine calls of de_game.h.
 *
 * The engine owns every module it creates and creates each one with its allocator (§4.2: no
 * hidden globals). The process-wide services (profiler, log, VFS, cvar registry) are not
 * created here: de_host_run starts them before the engine and stops them after it.
 */
#include "host/engine_internal.h"
#include "host/host_crash.h" /* DENG-96 */
#include "host/host_packs.h" /* DENG-213 */

#include <dengine/de_debug.h>
#include <dengine/de_text.h>
#include <dengine/de_input.h> /* DENG-84 */
#include <dengine/de_math.h>

#include <string.h>

/* Destroys what `e` holds, in the reverse order of de__engine_create; NULL members are skipped,
 * so it also unwinds a partial create. Frees `e`. */
static void engine_teardown(de__engine *e) {
    if (DE_HANDLE_VALID(e->pub.window)) {
        de_window_destroy(e->pub.window);
        e->pub.window = (de_window){0};
    }
    /* DENG-84: input, created after the world: detached from the world (its two systems and its
     * resource) before the world goes away. */
    if (e->pub.input != NULL) {
        de_input_detach(e->pub.input);
        de_input_destroy(e->pub.input);
        e->pub.input = NULL;
    }
    if (e->pub.text) {
        de_text_destroy(e->pub.text);
        e->pub.text = NULL;
    }
#if DE_HAS_ASSETS
    /* DENG-143: before the world, which it detaches from (system, resource), and before the I/O
     * threads its loads use. */
    if (e->pub.assets != NULL) {
        de_asset_system_destroy(e->pub.assets);
        e->pub.assets = NULL;
    }
    if (e->owns_io) {
        de_io_shutdown();
        e->owns_io = false;
    }
#endif
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
    /* 4b. Crash reports and the `quit` command (DENG-96, host_crash.c); never fails. */
    de__host_crash_engine_init(e);

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

#if DE_HAS_ASSETS
    /* 7c. Assets (DENG-143). The process I/O threads first: the asset pipeline uses them, and
     * starts and stops its own only when nobody runs them, which would stop them under any other
     * user. A caller that runs them already keeps them. */
    de_io_desc iod;
    memset(&iod, 0, sizeof iod);
    iod.thread_count = e->config.asset_io_threads;
    r = de_io_init(&iod);
    if (r == DE_OK) {
        e->owns_io = true;
    } else if (r == DE_ERR_ALREADY_EXISTS) {
        de_error_clear();
    } else {
        return engine_fail(e, r, "I/O threads");
    }
    de_asset_system_desc ad;
    memset(&ad, 0, sizeof ad);
    ad.allocator = a;
    ad.tasks = e->pub.tasks;
#if defined(DE_SHIPPING)
    ad.mode = DE_ASSET_MODE_PACKED; /* cooked data from packs only (S08) */
#else
    ad.mode = DE_ASSET_MODE_SOURCE;
#endif
    ad.io_threads = e->config.asset_io_threads;
    ad.hot_reload = e->config.asset_hot_reload;
    ad.locale = e->config.asset_locale[0] != '\0' ? e->config.asset_locale : NULL;
    ad.types = e->pub.types; /* DENG-215: the scene and prefab importer reads components here */
    r = de_asset_system_create(&ad, &e->pub.assets);
    if (r != DE_OK)
        return engine_fail(e, r, "asset system");
    /* DENG-213: a packed game reads only packs. Every .dpak file of game:/ is mounted at
     * priority 0 in file-name order (later names override earlier ones); see host_packs.h. A
     * missing game:/ (an engine made without de_fs_init) counts as no pack. */
    if (ad.mode == DE_ASSET_MODE_PACKED) {
        u32 packs = 0;
        r = de__host_mount_packs(e->pub.assets, DE__HOST_PACK_DIR, a, &packs);
        if (r == DE_ERR_NOT_FOUND && packs == 0)
            de_error_clear();
        else if (r != DE_OK)
            return engine_fail(e, r, "packs");
        if (packs == 0)
            DE_LOG(DE_LOG_WARN, e->log_host,
                   "no .dpak pack in %s: a packed build loads assets from packs only",
                   DE__HOST_PACK_DIR);
        else
            DE_LOG(DE_LOG_INFO, e->log_host, "mounted %u pack%s from %s", (unsigned)packs,
                   packs == 1 ? "" : "s", DE__HOST_PACK_DIR);
    }
    /* The first update with the world attaches the system to it (the de_asset_event type, the
     * DeAssetSystemRef resource, the de.asset_events system in First). Here, at a fixed point
     * before the game, so register_systems can name the event type and the resource, and so the
     * engine owns the system (a hot reload removes only the game's systems). Nothing is loaded
     * yet, so the update does nothing else. */
    de_error_clear();
    de_asset_system_update(e->pub.assets, e->pub.world);
    if (!DE_HANDLE_VALID(de_asset_event_type(e->pub.assets)))
        return engine_fail(e, DE_ERR_OUT_OF_MEMORY, "asset events");
#endif

    /* 7d. Input (DENG-84). After the world, which it joins as the de_input_resource resource
     * with the input.actions (PreUpdate) and input.fixed_tick (FixedUpdate) systems, at a fixed
     * point before the game registers its systems. The input follows the focus window (desc
     * window 0), so it needs no window here; headless runs get an input state that sees no
     * events. The loop feeds it (loop.c). */
    de_input_desc indesc;
    memset(&indesc, 0, sizeof indesc);
    indesc.allocator = a;
    r = de_input_create(&indesc, &e->pub.input);
    if (r != DE_OK)
        return engine_fail(e, r, "input");
    r = de_input_attach(e->pub.input, e->pub.world);
    if (r != DE_OK)
        return engine_fail(e, r, "input attach");

    /* DENG-257: text also supports headless localization/layout. The loader's allocator
     * outlives text and remains valid until the asset system has unloaded fonts. */
    r = de_text_create(a, e->pub.assets, &e->pub.text);
    if (r != DE_OK)
        return engine_fail(e, r, "text");

    /* 8. Window, unless headless. The renderer (S12) picks the surface; until then the window
     * has none. The optional modules stay NULL. */
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
