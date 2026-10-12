/*
 * hot_reload.c: game library hot reload (DENG-93, SPEC §10.1). hot_reload.h has the design and
 * the reload order; the ECS side (system owners, re-layout, type sync) is src/ecs/hot_reload.c.
 *
 * The build writes the game library (`source`); the host never loads that file after start-up.
 * It loads copies, `cache:/hot/<stem>.reload-<n><ext>`, each under a new name: a Windows DLL
 * that is loaded cannot be overwritten, and macOS dlopen returns the image it already has for a
 * path it has seen. The `.pdb` next to a Windows DLL is copied with it under the same <n>.
 */
#include "host/host_internal.h"

#include <dengine/de_debug.h>

#include "ecs/in_schedule_internal.h"

#include <stdio.h>
#include <string.h>

#if DE__HOT_RELOAD

/* ---- Paths ---------------------------------------------------------------------------------- */

/* The last component of `path` ('/' or '\\' separated). */
static const char *path_file_name(const char *path) {
    const char *name = path;
    for (const char *c = path; *c != '\0'; ++c) {
        if (*c == '/' || *c == '\\')
            name = c + 1;
    }
    return name;
}

/* Splits a file name into stem and extension at its last '.' (not a leading one): "libg.dylib"
 * gives "libg" and ".dylib"; "game" gives "game" and "". */
static void split_name(const char *name, char *stem, size_t stem_cap, const char **ext) {
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name)
        dot = name + strlen(name);
    size_t n = (size_t)(dot - name);
    if (n >= stem_cap)
        n = stem_cap - 1u;
    memcpy(stem, name, n);
    stem[n] = '\0';
    *ext = dot;
}

static de_result path_join(char *buf, size_t cap, const char *a, const char *b, const char *c) {
    const int n = snprintf(buf, cap, "%s%s%s", a, b, c != NULL ? c : "");
    if (n < 0 || (size_t)n >= cap)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "hot reload: path too long (%s%s...)", a, b);
    return DE_OK;
}

/* ---- Copies --------------------------------------------------------------------------------- */

/* Copies virtual file `from` to virtual file `to` (written atomically). */
static de_result copy_file(const de_allocator *a, const char *from, const char *to) {
    void *data = NULL;
    u64 size = 0;
    DE_TRY(de_file_read_all(from, a, &data, &size));
    de_result r = de_file_write_all_atomic(to, data, size);
    DE_FREE(a, data, (size_t)size + 1u);
    return r;
}

/* Deletes a copy (and its .pdb on Windows); missing files are fine. */
static void delete_copy(const char *vpath) {
    if (vpath[0] == '\0')
        return;
    if (de_file_delete(vpath) != DE_OK)
        de_error_clear(); /* gone already, or still mapped somewhere: not worth a warning */
#if defined(_WIN32)
    char pdb[DE_FS_PATH_MAX], stem[DE_FS_PATH_MAX];
    const char *ext = NULL;
    split_name(vpath, stem, sizeof stem, &ext);
    if (path_join(pdb, sizeof pdb, stem, ".pdb", NULL) == DE_OK && de_path_exists(pdb) &&
        de_file_delete(pdb) != DE_OK)
        de_error_clear();
#endif
}

/*
 * Copies the source library to the next free `cache:/hot/<stem>.reload-<n><ext>`: `vpath` gets
 * the virtual path, `native` the native one. A leftover file of an earlier run with the same
 * name is deleted first; if that fails (another process has it loaded), the next <n> is used.
 */
static de_result copy_library(de__host *h, char *vpath, char *native) {
    de__hot_reload *hr = &h->hot_reload;
    const de_allocator *a = h->engine->allocator;
    char stem[DE__HOT_RELOAD_NAME_MAX];
    const char *ext = NULL;
    split_name(hr->file_name, stem, sizeof stem, &ext);

    char src[DE_FS_PATH_MAX];
    DE_TRY(path_join(src, sizeof src, DE__HOT_RELOAD_ROOT ":/", hr->file_name, NULL));
    de_result r = de_dir_create(DE__HOT_RELOAD_DIR);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "hot reload: cannot create %s", DE__HOT_RELOAD_DIR);

    for (u32 tries = 0; tries < 1000u; ++tries) {
        char name[DE__HOT_RELOAD_NAME_MAX + 32u];
        const int n = snprintf(name, sizeof name, "/%s.reload-%u%s", stem, hr->next_copy++, ext);
        if (n < 0 || (size_t)n >= sizeof name)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "hot reload: file name too long");
        DE_TRY(path_join(vpath, DE_FS_PATH_MAX, DE__HOT_RELOAD_DIR, name, NULL));
        if (de_path_exists(vpath) && de_file_delete(vpath) != DE_OK) {
            de_error_clear();
            continue; /* in use (another process): take the next number */
        }
        r = copy_file(a, src, vpath);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "hot reload: cannot copy %s to %s", hr->source, vpath);
#if defined(_WIN32)
        /* The debug symbols, under the same number. Optional: a build without a .pdb is fine. */
        char pdb_src[DE_FS_PATH_MAX], pdb_dst[DE_FS_PATH_MAX], src_stem[DE__HOT_RELOAD_NAME_MAX];
        const char *src_ext = NULL;
        split_name(hr->file_name, src_stem, sizeof src_stem, &src_ext);
        char dst_stem[DE_FS_PATH_MAX];
        const char *dst_ext = NULL;
        split_name(vpath, dst_stem, sizeof dst_stem, &dst_ext);
        if (path_join(pdb_src, sizeof pdb_src, DE__HOT_RELOAD_ROOT ":/", src_stem, ".pdb") ==
                DE_OK &&
            path_join(pdb_dst, sizeof pdb_dst, dst_stem, ".pdb", NULL) == DE_OK &&
            de_path_exists(pdb_src) && copy_file(a, pdb_src, pdb_dst) != DE_OK) {
            DE_WARN(h->log, "hot reload: cannot copy %s; the debugger may not find symbols",
                    pdb_src);
            de_error_clear();
        }
#endif
        r = de_fs_resolve(vpath, native, DE_FS_PATH_MAX);
        if (r != DE_OK) {
            delete_copy(vpath);
            return DE_FAIL_WRAP(r, "hot reload: %s", vpath);
        }
        return DE_OK;
    }
    return DE_FAIL(DE_ERR_IO, "hot reload: no free copy name for %s in %s", hr->file_name,
                   DE__HOT_RELOAD_DIR);
}

/* Copies the library and opens the copy into `next` (validated). On failure nothing is left. */
static de_result load_copy(de__host *h, de__game_module *next, char *vpath) {
    char native[DE_FS_PATH_MAX];
    vpath[0] = '\0';
    DE_TRY(copy_library(h, vpath, native));
    memset(next, 0, sizeof *next);
    de_result r = de__game_module_load(next, native);
    if (r != DE_OK) {
        delete_copy(vpath);
        vpath[0] = '\0';
    }
    return r;
}

/* The library de__game_attach is going to load, chosen as it chooses it (game_module.c):
 * desc.game_library (a bare file name means the current directory), else nothing when a static
 * game is given, else game.toml's [game] library. "" when there is none (or it cannot be
 * resolved: attach reports that). */
static void source_library(const de__host *h, char *buf, size_t cap) {
    buf[0] = '\0';
    const char *lib = h->desc.game_library;
    if (lib != NULL && lib[0] != '\0') {
        const bool bare = strchr(lib, '/') == NULL && strchr(lib, '\\') == NULL;
        if (path_join(buf, cap, bare ? "./" : "", lib, NULL) != DE_OK) {
            buf[0] = '\0';
            de_error_clear();
        }
        return;
    }
    if (h->desc.static_api != NULL || h->config.library[0] == '\0')
        return;
    if (de__game_library_resolve(h->config.library, buf, cap) != DE_OK) {
        buf[0] = '\0';
        de_error_clear();
    }
}

/* ---- Watching ------------------------------------------------------------------------------- */

static void on_watch(const de_watch_event *ev, void *user) {
    de__hot_reload *hr = (de__hot_reload *)user;
    if (ev->is_dir) {
        if (ev->change == DE_WATCH_MODIFIED)
            hr->pending = true; /* the OS dropped events: look at the library again */
        return;
    }
    /* A rebuild shows up as CREATED, MODIFIED or a RENAMED onto the name (DENG-248). */
    if (ev->change != DE_WATCH_DELETED && strcmp(path_file_name(ev->path), hr->file_name) == 0)
        hr->pending = true;
}

static void stop_watching(de__hot_reload *hr) {
    de_file_watch_destroy(hr->watch);
    hr->watch = NULL;
    if (hr->mounted) {
        if (de_fs_unmount(DE__HOT_RELOAD_ROOT, hr->source_dir) != DE_OK)
            de_error_clear();
        hr->mounted = false;
    }
}

/* ---- Init and shutdown ---------------------------------------------------------------------- */

/* False when path[0, n) (n >= 2, ending in a separator) is a root: "C:\\" or "C:/". */
static bool hr_is_not_root(const char *path, size_t n) {
    return !(n == 3u && path[1] == ':');
}

static bool hot_reload_cvar(void) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = "host.hot_reload";
    d.help = "Reload the game library when it changes (Debug and Dev builds)";
    d.type = DE_CVAR_BOOL;
    d.flags = DE_CVAR_DEV_ONLY;
    d.default_number = 1.0;
    const de_cvar cv = de_cvar_register(&d);
    if (!DE_HANDLE_VALID(cv)) {
        de_error_clear();
        return true;
    }
    return de_cvar_bool(cv);
}

/* Sets up the mount, the first copy and the watch for library `path`. Errors leave hot reload
 * off. */
static de_result setup(de__host *h, const char *path) {
    de__hot_reload *hr = &h->hot_reload;
    const char *name = path_file_name(path);
    const size_t dir_len = (size_t)(name - path);
    if (strlen(name) >= sizeof hr->file_name || name[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "hot reload: bad library file name '%s'", path);
    memcpy(hr->source, path, strlen(path) + 1u);
    memcpy(hr->file_name, name, strlen(name) + 1u);
    /* The directory without its trailing separator, except for a root ("/", "C:\\"). */
    size_t n = dir_len;
    if (n > 1u && hr_is_not_root(path, n))
        --n;
    memcpy(hr->source_dir, path, n);
    hr->source_dir[n] = '\0';

    de_result r = de_fs_mount(DE__HOT_RELOAD_ROOT, hr->source_dir, DE_MOUNT_READ_ONLY, 0);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "hot reload: cannot mount %s", hr->source_dir);
    hr->mounted = true;

    /* The game runs from a copy from the start: the build output is never loaded, so the build
     * can always overwrite it, and no library is loaded and closed again before the game
     * starts. */
    r = copy_library(h, hr->current_copy, hr->loaded);
    if (r != DE_OK) {
        hr->current_copy[0] = '\0';
        stop_watching(hr);
        return r;
    }

    de_file_watch_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.dir = DE__HOT_RELOAD_ROOT ":/";
    wd.fn = on_watch;
    wd.user = hr;
    wd.allocator = h->engine->allocator;
    if (de_file_watch_create(&wd, &hr->watch) != DE_OK) {
        /* Requests still reload; only the automatic trigger is missing. */
        de__host_log_error("hot reload: cannot watch the game library; it is not reloaded on "
                           "change");
        de_error_clear();
        hr->watch = NULL;
    }
    return DE_OK;
}

de_result de__hot_reload_init(de__host *h) {
    if (h == NULL || h->engine == NULL)
        return DE_OK;
    de__hot_reload *hr = &h->hot_reload;
    memset(hr, 0, sizeof *hr);
    char path[DE_FS_PATH_MAX];
    source_library(h, path, sizeof path);
    if (path[0] == '\0')
        return DE_OK; /* a statically linked game: nothing to reload */
    if (!hot_reload_cvar()) {
        DE_INFO(h->log, "hot reload: off (host.hot_reload is false)");
        return DE_OK;
    }
    de_result r = setup(h, path);
    if (r != DE_OK) {
        de__host_log_error("hot reload: off");
        de_error_clear();
        return DE_OK;
    }
    r = de__loop_add_frame_end_hook(h, de__hot_reload_frame_end, NULL);
    if (r != DE_OK) {
        stop_watching(hr);
        delete_copy(hr->current_copy);
        hr->current_copy[0] = '\0';
        return DE_FAIL_WRAP(r, "hot reload: no room for the frame-end hook");
    }
    h->desc.game_library = hr->loaded; /* de__game_attach loads the copy */
    hr->enabled = true;
    DE_INFO(h->log, "hot reload: watching %s; running from %s", hr->source, hr->current_copy);
    return DE_OK;
}

void de__hot_reload_shutdown(de__host *h) {
    if (h == NULL)
        return;
    de__hot_reload *hr = &h->hot_reload;
    stop_watching(hr);
    /* The copy is closed by de__game_detach before this; one still attached stays. */
    if (h->game.lib == NULL)
        delete_copy(hr->current_copy);
    hr->current_copy[0] = '\0';
    hr->enabled = false;
    hr->pending = false;
}

/* ---- Reload --------------------------------------------------------------------------------- */

/* The game callbacks of a reload run under the game's system owner tag. */
static void owner_game(de_engine *e) {
    de__world_set_system_owner(e->world, DE__HOST_GAME_SYSTEM_OWNER);
}
static void owner_engine(de_engine *e) {
    de__world_set_system_owner(e->world, DE__SYSTEM_OWNER_ENGINE);
}

static bool sync_changed_something(const de__world_sync_stats *s) {
    return s->refreshed + s->relaid_out + s->resources + s->events > 0;
}

de_result de__hot_reload_reload(de__host *h, bool *fatal) {
    if (fatal != NULL)
        *fatal = false;
    if (h == NULL || !h->hot_reload.enabled || h->engine == NULL)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "hot reload is off");
    if (de__ecs_in_schedule())
        return DE_FAIL(DE_ERR_BUSY, "hot reload: not inside a schedule run");
    de__hot_reload *hr = &h->hot_reload;
    de_engine *e = h->engine;
    hr->pending = false;

    /* 1-2. Copy and open the new library; a refusal here leaves the game untouched. */
    de__game_module next;
    char vpath[DE_FS_PATH_MAX];
    de_result r = load_copy(h, &next, vpath);
    if (r != DE_OK) {
        ++hr->failures;
        de__host_log_error("hot reload: library refused");
        DE_WARN(h->log, "hot reload: '%s' keeps running (%u refused reloads)", h->game.table.name,
                hr->failures);
        return r;
    }
    DE_INFO(h->log, "hot reload: '%s' -> '%s' (%s)", h->game.table.name, next.table.name, vpath);

    /* 3-4. The old game lets go; its systems leave. */
    owner_game(e);
    h->game.table.on_unload(e);
    hr->last_systems_removed =
        de__world_remove_systems_by_owner(e->world, DE__HOST_GAME_SYSTEM_OWNER);

    /* 5-6. The new game's types, then the world follows them. */
    de__game_module_swap(&h->game, &next); /* `next` holds the old game now */
    de_error_clear();
    h->game.table.register_types(e, e->types);
    if (de_error_last() != NULL)
        r = DE_FAIL_WRAP(de_error_last()->result, "hot reload: '%s' register_types failed",
                         h->game.table.name);
    de__world_sync_stats st;
    memset(&st, 0, sizeof st);
    if (r == DE_OK) {
        r = de__world_sync_types(e->world, &st);
        if (r != DE_OK && sync_changed_something(&st)) {
            /* Only an allocation failure after the checks gets here. */
            owner_engine(e);
            if (fatal != NULL)
                *fatal = true;
            return DE_FAIL_WRAP(r, "hot reload: the world is partly re-laid out");
        }
    }
    if (r != DE_OK) {
        /* Undo: the old game's descriptions and hooks, systems and state come back. The error
         * is kept in the context while the old game runs its callbacks again. */
        ++hr->failures;
        de__host_log_error("hot reload: library refused");
        const de_result refused = de_error_last() != NULL ? de_error_last()->result : r;
        de__game_module_swap(&h->game, &next); /* the old game is current again */
        de_error_clear();
        h->game.table.register_types(e, e->types);
        if (de__world_sync_types(e->world, NULL) != DE_OK)
            de__host_log_error("hot reload: restoring the old types");
        h->game.table.register_systems(e, e->world);
        h->game.table.on_reload(e);
        owner_engine(e);
        de__game_module_unload(&next);
        delete_copy(vpath);
        DE_WARN(h->log, "hot reload: '%s' keeps running (%u refused reloads)", h->game.table.name,
                hr->failures);
        return DE_FAIL(refused, "hot reload: the new library was refused (logged)");
    }

    /* 7. The new game's systems, then its reload callback. */
    h->game.table.register_systems(e, e->world);
    if (de_error_last() != NULL) {
        de__host_log_error("hot reload: register_systems reported an error");
        de_error_clear();
    }
    h->game.table.on_reload(e);
    owner_engine(e);

    /* 8. Nothing points into the old library any more: close it and drop its copy. */
    de__game_module_unload(&next);
    delete_copy(hr->current_copy);
    memcpy(hr->current_copy, vpath, strlen(vpath) + 1u);
    ++h->game.generation;
    ++hr->reloads;
    hr->last_sync = st;
    de_error_clear();
    DE_INFO(h->log,
            "hot reload: '%s' reloaded (reload %u): %u systems replaced, %u components re-laid "
            "out, %u types refreshed",
            h->game.table.name, hr->reloads, hr->last_systems_removed, st.relaid_out, st.refreshed);
    if (st.migrate_refused > 0)
        DE_WARN(h->log,
                "hot reload: migrate returned false for %u objects; they kept the fields "
                "that matched",
                st.migrate_refused);
    return DE_OK;
}

de_result de__hot_reload_poll(de__host *h) {
    if (h == NULL || !h->hot_reload.enabled)
        return DE_OK;
    de__hot_reload *hr = &h->hot_reload;
    if (hr->watch != NULL)
        (void)de_file_watch_poll(hr->watch);
    if (!hr->pending || de__ecs_in_schedule())
        return DE_OK; /* inside a system: the frame-end hook runs it */
    bool fatal = false;
    const de_result r = de__hot_reload_reload(h, &fatal);
    if (r == DE_OK || fatal)
        return r;
    /* Refused and logged: the old game runs on; the next change is tried again. */
    de_error_clear();
    return DE_OK;
}

void de__hot_reload_request(de__host *h) {
    if (h != NULL && h->hot_reload.enabled)
        h->hot_reload.pending = true;
}

#else /* !DE__HOT_RELOAD: Release and Shipping */

de_result de__hot_reload_init(de__host *h) {
    (void)h;
    return DE_OK;
}

void de__hot_reload_shutdown(de__host *h) {
    (void)h;
}

void de__hot_reload_request(de__host *h) {
    (void)h;
}

de_result de__hot_reload_poll(de__host *h) {
    (void)h;
    return DE_OK;
}

de_result de__hot_reload_reload(de__host *h, bool *fatal) {
    (void)h;
    if (fatal != NULL)
        *fatal = false;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "hot reload: not in this build type");
}

#endif /* DE__HOT_RELOAD */

de_result de__hot_reload_frame_end(de__host *h, void *user) {
    (void)user;
    return de__hot_reload_poll(h);
}
