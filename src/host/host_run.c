/*
 * host_run.c: de_host_run (SPEC §10), the process from start to exit code.
 *
 * Start order (and the reverse at exit):
 *   de_prof_init (first, before any thread or long-lived mutex; DENG-94) → de_thread_set_name
 *   → de_log_init (DENG-150) → de_fs_init (before any virtual path; DENG-247)
 *   → game.toml with its [cvars] → user:/config.toml → command line → de_cvar_lock_startup
 *   → engine → [headless: stop handlers] → game attach → init → register_types
 *   → register_systems → main loop → [headless: handlers restored] → shutdown → game detach
 *   → engine destroy → de_fs_shutdown → de_log_shutdown
 *   → de_prof_shutdown (last, after every thread has stopped).
 * The profiler, the log and the VFS are process-wide: when the caller has started one already
 * (a test, a tool), the host uses it and leaves it running.
 *
 * Headless (DENG-95, headless.c): de_host_desc.headless (`dengine --headless`), any run of a
 * DE_HEADLESS build, or a platform without video. No window, GPU, renderer, audio or UI; the
 * event pump still runs. SIGINT/SIGTERM (POSIX) or a console Ctrl event (Windows) quit after the
 * current frame, and the run exits 0.
 *
 * Cvar precedence, lowest first: cvar defaults, game.toml [cvars], user:/config.toml, `+name
 * value` on the command line. All are startup values, so DE_CVAR_READONLY cvars take them.
 */
#include "host/host_internal.h"
#include "host/host_crash.h" /* DENG-96 */

#include <dengine/de_debug.h>

#include <string.h>

#define HOST_CONFIG_TOML "user:/config.toml"

void de__host_log_error(const char *what) {
    de_log_category cat = de_log_register_category("host", DE_LOG_INFO);
    const de_error *err = de_error_last();
    if (err == NULL) {
        DE_ERROR(cat, "%s", what);
        return;
    }
    DE_ERROR(cat, "%s: %s [%s]", what, err->message, de_result_str(err->result));
    for (const de_error *c = err->cause; c != NULL; c = c->cause)
        DE_ERROR(cat, "  caused by: %s [%s]", c->message, de_result_str(c->result));
}

/* `--game-toml <vpath>` or `--game-toml=<vpath>`; the last one wins. NULL when absent.
 * DE_ERR_INVALID_ARGUMENT for `--game-toml` without a value. */
static de_result host_arg_game_toml(int argc, char **argv, const char **out) {
    static const char opt[] = "--game-toml";
    const size_t opt_len = sizeof opt - 1;
    *out = NULL;
    for (int i = 1; i < argc && argv != NULL; ++i) {
        const char *arg = argv[i];
        if (arg == NULL || strncmp(arg, opt, opt_len) != 0)
            continue;
        if (arg[opt_len] == '=') {
            *out = arg + opt_len + 1;
        } else if (arg[opt_len] == '\0') {
            if (i + 1 >= argc || argv[i + 1] == NULL)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "command line: --game-toml needs a value");
            *out = argv[++i];
        }
    }
    if (*out != NULL && (*out)[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "command line: --game-toml is empty");
    return DE_OK;
}

/* Project file, user config and command line, then the startup lock. */
static de_result host_load_config(de__host *h) {
    const char *arg_toml = NULL;
    DE_TRY(host_arg_game_toml(h->desc.argc, h->desc.argv, &arg_toml));
    h->game_toml = arg_toml != NULL            ? arg_toml
                   : h->desc.game_toml != NULL ? h->desc.game_toml
                                               : DE_HOST_DEFAULT_GAME_TOML;
    de_result r = de__game_config_load(h->game_toml, &h->config);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "project file %s", h->game_toml);

    /* A broken user config or command line value must not keep the game from starting: they
     * are logged, and the values that did apply stay. */
    r = de_cvar_load(HOST_CONFIG_TOML);
    if (r == DE_ERR_NOT_FOUND) {
        de_error_clear();
    } else if (r != DE_OK) {
        const de_error *err = de_error_last();
        DE_WARN(h->log, "%s: %s", HOST_CONFIG_TOML, err != NULL ? err->message : "failed");
        de_error_clear();
    }
    if (h->desc.argc > 0 && h->desc.argv != NULL) {
        r = de_cvar_apply_args(h->desc.argc, h->desc.argv);
        if (r != DE_OK) {
            const de_error *err = de_error_last();
            DE_WARN(h->log, "%s", err != NULL ? err->message : "command line: bad cvar value");
            de_error_clear();
        }
    }
    de_cvar_lock_startup();
    return DE_OK;
}

/* Engine through shutdown. Returns DE_OK or the first failure (already wrapped). */
static de_result host_run_game(de__host *h) {
    de__engine_desc ed;
    memset(&ed, 0, sizeof ed);
    ed.config = &h->config;
    ed.headless = h->desc.headless;
    de_result r = de__engine_create(&ed, &h->engine);
    if (r != DE_OK)
        return r;

    /* Headless (asked for, a DE_HEADLESS build, or a platform without video): from here until
     * the loop ends, SIGINT/SIGTERM or a console Ctrl event quit after the current frame. */
    if (de__engine_from(h->engine)->headless) {
        r = de__headless_begin(h);
        if (r != DE_OK) {
            de__engine_destroy(h->engine);
            h->engine = NULL;
            return r;
        }
    }

    /* DENG-93: hot reload (Debug, Dev) makes the game attach from a copy of its library. */
    r = de__hot_reload_init(h);
    if (r == DE_OK)
        r = de__game_attach(h, &h->desc);
    if (r != DE_OK) {
        r = DE_FAIL_WRAP(r, "attaching the game");
        de__headless_end(h);
        de__hot_reload_shutdown(h);
        de__engine_destroy(h->engine);
        h->engine = NULL;
        return r;
    }
    const de_game_api *api = h->api;
    const char *name = api->name != NULL ? api->name : h->config.name;
    de_engine *e = h->engine;
    (void)de__host_crash_attach(h); /* DENG-96: crash annotations, frame hook; logs a failure */

    /* DENG-93: systems the game adds carry its owner tag, so a hot reload can remove them. */
    de__world_set_system_owner(e->world, DE__HOST_GAME_SYSTEM_OWNER);
    de_error_clear(); /* so a failed init without its own error context is told apart */
    r = api->init != NULL ? api->init(e) : DE_OK;
    if (r != DE_OK) {
        de__world_set_system_owner(e->world, DE__SYSTEM_OWNER_ENGINE);
        if (de_error_last() == NULL)
            r = DE_FAIL(r, "game '%s': init returned %s", name, de_result_str(r));
        else
            r = DE_FAIL_WRAP(r, "game '%s': init failed", name);
        /* No shutdown: init did not succeed. */
        de__headless_end(h);
        de__game_detach(h);
        de__hot_reload_shutdown(h); /* DENG-93 */
        de__engine_destroy(h->engine);
        h->engine = NULL;
        return r;
    }
    if (api->register_types != NULL)
        api->register_types(e, e->types);
    if (api->register_systems != NULL)
        api->register_systems(e, e->world);
    de__world_set_system_owner(e->world, DE__SYSTEM_OWNER_ENGINE); /* DENG-93 */

    r = de__loop_run(h);
    /* The previous handlers again: a second signal during shutdown is not swallowed. */
    de__headless_end(h);

    /* A hot reload swapped the table behind h->api (DENG-93): `api` names the current game. */
    if (api->shutdown != NULL)
        api->shutdown(e);
    de__game_detach(h);
    de__hot_reload_shutdown(h);    /* DENG-93: after the copy is closed */
    de__engine_destroy(h->engine); /* sets no error: a loop failure stays de_error_last() */
    h->engine = NULL;
    return r;
}

int de_host_run(const de_host_desc *desc) {
    de__host h;
    memset(&h, 0, sizeof h);
    if (desc != NULL)
        h.desc = *desc;
#if defined(DE_HEADLESS)
    h.desc.headless = true; /* a server build has no window, GPU, audio or UI to start */
#endif

    /* Process-wide services: start each one the caller has not. */
    bool own_prof = false;
    if (!de_prof_running()) {
        own_prof = de_prof_init() == DE_OK; /* DE_ERR_UNSUPPORTED without a backend: fine */
        de_error_clear();
    }
    de_thread_set_name("main");
    bool own_log = de_log_init(NULL) == DE_OK; /* DE_ERR_BUSY: the caller runs it */
    de_error_clear();
    h.log = de_log_register_category("host", DE_LOG_INFO);
    (void)de_log_register_category("game", DE_LOG_INFO);

    int exit_code = 0;
    bool own_fs = false;
    de_fs_desc fsd;
    memset(&fsd, 0, sizeof fsd);
    fsd.game_dir = h.desc.game_dir; /* NULL: the executable's directory */
    de_result r = de_fs_init(&fsd);
    if (r == DE_OK) {
        own_fs = true;
    } else if (r == DE_ERR_ALREADY_EXISTS) {
        de_error_clear();
        r = DE_OK;
    } else {
        r = DE_FAIL_WRAP(r, "file system init");
    }

    if (r == DE_OK)
        r = host_load_config(&h);
    if (r == DE_OK)
        r = host_run_game(&h);
    if (r != DE_OK) {
        de__host_log_error("de_host_run failed");
        exit_code = 1;
    } else {
        DE_INFO(h.log, "%s exited normally (frames: %llu, FixedUpdate steps: %llu)", h.config.name,
                (unsigned long long)h.loop_frames, (unsigned long long)h.loop_fixed_steps);
    }

    if (own_fs)
        de_fs_shutdown();
    if (own_log)
        de_log_shutdown();
    else
        de_log_flush();
    if (own_prof)
        de_prof_shutdown();
    return exit_code;
}
