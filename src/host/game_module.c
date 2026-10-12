/*
 * game_module.c: attaching the game's de_game_api (SPEC §10.1). DENG-92 owns the bodies;
 * DENG-93 adds the hot-reload swap on top of de__game_module_load / de__game_module_unload.
 *
 * Where the game comes from, first match wins:
 *   1. de_host_desc.game_library (`dengine --game <lib>`): a native path, loaded as it is.
 *   2. de_host_desc.static_api (Shipping: the game's executable passes de_game_get_api()).
 *   3. game.toml `[game] library`, resolved against game:/ (de__game_library_resolve).
 * The library is opened with de_dylib_open and must export DE__GAME_API_SYMBOL. The engine API
 * the game calls is not linked into the game library: it binds to the host's copy when the
 * library is loaded (cmake/DeAddGame.cmake has the per-platform rule).
 */
#include "host/host_internal.h"

#include <string.h>

/* ---- Table checks ----------------------------------------------------------------------- */

de_result de__game_api_check(const de_game_api *api) {
    if (api == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game API table is NULL");
    if (api->api_version != DE_GAME_API_VERSION)
        return DE_FAIL(DE_ERR_VERSION_MISMATCH,
                       "game '%s' was built for game API version %u, this engine has version %u",
                       api->name != NULL ? api->name : "(unnamed)", (unsigned)api->api_version,
                       (unsigned)DE_GAME_API_VERSION);
    return DE_OK;
}

de_result de__game_api_validate(const de_game_api *api, const char *source) {
    if (source == NULL)
        source = "game";
    if (api == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: %s() returned NULL", source,
                       DE__GAME_API_SYMBOL);
    de_result r = de__game_api_check(api);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "%s", source);
    if (api->name == NULL || api->name[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: de_game_api.name is not set", source);

    /* `init` returns a result; the others cannot report one, but every game needs them, and a
     * NULL here would only show up later as a crash in the middle of a frame or a reload. */
    const char *missing = api->init == NULL               ? "init"
                          : api->shutdown == NULL         ? "shutdown"
                          : api->register_types == NULL   ? "register_types"
                          : api->register_systems == NULL ? "register_systems"
                                                          : NULL;
    if (missing != NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: game '%s' has no de_game_api.%s callback",
                       source, api->name, missing);
    return DE_OK;
}

/* ---- One module ------------------------------------------------------------------------- */

static void game_noop(de_engine *e) {
    (void)e;
}

/* Copies a validated table into `m`, defaulting the optional callbacks. */
static void game_module_take(de__game_module *m, de_dylib *lib, const de_game_api *api,
                             const char *path) {
    m->lib = lib;
    m->api = api;
    m->table = *api;
    if (m->table.on_unload == NULL)
        m->table.on_unload = game_noop;
    if (m->table.on_reload == NULL)
        m->table.on_reload = game_noop;
    size_t n = path != NULL ? strlen(path) : 0; /* checked < DE_FS_PATH_MAX by the caller */
    memcpy(m->path, path != NULL ? path : "", n);
    m->path[n] = '\0';
    m->generation++;
}

de_result de__game_module_load(de__game_module *m, const char *path) {
    if (m == NULL || path == NULL || path[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game library: module and path are required");
    if (strlen(path) >= DE_FS_PATH_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game library: path longer than %d bytes",
                       DE_FS_PATH_MAX - 1);

    de_dylib *lib = NULL;
    de_result r = de_dylib_open(path, &lib);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "game library %s: cannot load it", path);

    de__game_get_api_fn get_api = (de__game_get_api_fn)de_dylib_function(lib, DE__GAME_API_SYMBOL);
    if (get_api == NULL) {
        r = DE_FAIL_WRAP(DE_ERR_NOT_FOUND, "game library %s does not export %s()", path,
                         DE__GAME_API_SYMBOL);
        de_dylib_close(lib);
        return r;
    }
    const de_game_api *api = get_api();
    r = de__game_api_validate(api, path);
    if (r != DE_OK) {
        r = DE_FAIL_WRAP(r, "game library %s: refused", path);
        de_dylib_close(lib);
        return r;
    }
    game_module_take(m, lib, api, path);
    return DE_OK;
}

de_result de__game_module_set_static(de__game_module *m, const de_game_api *api) {
    if (m == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game module is NULL");
    DE_TRY(de__game_api_validate(api, "statically linked game"));
    game_module_take(m, NULL, api, "");
    return DE_OK;
}

void de__game_module_unload(de__game_module *m) {
    if (m == NULL)
        return;
    if (m->lib != NULL) {
        /* Log records keep `__FILE__` pointers into the code that logged them, and the log
         * thread formats them later: drain the queue while the library's strings still exist. */
        de_log_flush();
        de_dylib_close(m->lib);
    }
    u32 generation = m->generation;
    memset(m, 0, sizeof *m);
    m->generation = generation;
}

/* ---- DENG-93: hot reload swap entry point ---------------------------------------------- */

void de__game_module_swap(de__game_module *m, de__game_module *other) {
    if (m == NULL || other == NULL || m == other)
        return;
    const u32 gm = m->generation, go = other->generation;
    de__game_module tmp = *m;
    *m = *other;
    *other = tmp;
    m->generation = gm;
    other->generation = go;
}

/* ---- [game] library --------------------------------------------------------------------- */

static bool game_path_is_absolute(const char *p) {
    if (p[0] == '/' || p[0] == '\\')
        return true; /* POSIX absolute, or a Windows UNC / root-relative path */
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
           (p[2] == '/' || p[2] == '\\');
}

/* A virtual path has a root name followed by ":/" (and is not a Windows drive letter). */
static bool game_path_is_virtual(const char *p) {
    const char *colon = strchr(p, ':');
    return colon != NULL && colon - p > 1 && colon[1] == '/';
}

/* True when the last path component has a '.' after its first character. */
static bool game_path_has_extension(const char *p) {
    const char *last = p;
    for (const char *c = p; *c != '\0'; ++c) {
        if (*c == '/' || *c == '\\')
            last = c + 1;
    }
    return last[0] != '\0' && strchr(last + 1, '.') != NULL;
}

de_result de__game_library_resolve(const char *library, char *buf, size_t cap) {
    if (buf != NULL && cap > 0)
        buf[0] = '\0';
    if (library == NULL || library[0] == '\0' || buf == NULL || cap == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game.library: empty value or no buffer");

    char named[DE_FS_PATH_MAX];
    const char *name = library;
    if (!game_path_has_extension(library)) {
        de_result r = de_dylib_platform_name(library, named, sizeof named);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "game.library '%s'", library);
        name = named;
    }

    if (game_path_is_absolute(name)) {
        size_t n = strlen(name);
        if (n >= cap)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game.library '%s': path too long", library);
        memcpy(buf, name, n + 1);
        return DE_OK;
    }

    char vpath[DE_FS_PATH_MAX];
    const char *v = name;
    if (!game_path_is_virtual(name)) {
        static const char game_root[] = "game:/";
        size_t n = strlen(name);
        if (sizeof game_root - 1 + n >= sizeof vpath)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game.library '%s': path too long", library);
        memcpy(vpath, game_root, sizeof game_root - 1);
        memcpy(vpath + sizeof game_root - 1, name, n + 1);
        v = vpath;
    }
    de_result r = de_fs_resolve(v, buf, cap);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "game.library '%s' (%s)", library, v);
    return DE_OK;
}

/* ---- The host seams --------------------------------------------------------------------- */

de_result de__game_attach(de__host *h, const de_host_desc *d) {
    de__game_module *m = &h->game;
    de_result r;
    if (d->game_library != NULL && d->game_library[0] != '\0') {
        /* A bare file name means the file in the current directory, not a search of the
         * system library path (which dlopen and LoadLibrary would do). */
        char path[DE_FS_PATH_MAX];
        const char *lib = d->game_library;
        if (strchr(lib, '/') == NULL && strchr(lib, '\\') == NULL) {
            size_t n = strlen(lib);
            if (n + 2 >= sizeof path)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game library: path longer than %d bytes",
                               DE_FS_PATH_MAX - 1);
            memcpy(path, "./", 2);
            memcpy(path + 2, lib, n + 1);
            lib = path;
        }
        r = de__game_module_load(m, lib);
    } else if (d->static_api != NULL) {
        r = de__game_module_set_static(m, d->static_api);
    } else if (h->config.library[0] != '\0') {
        char path[DE_FS_PATH_MAX];
        r = de__game_library_resolve(h->config.library, path, sizeof path);
        if (r == DE_OK)
            r = de__game_module_load(m, path);
        if (r != DE_OK)
            r = DE_FAIL_WRAP(r, "%s: [game] library",
                             h->game_toml != NULL ? h->game_toml : "game.toml");
    } else {
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "no game: no game library (--game, [game] library in %s) and no "
                       "statically linked game (de_host_desc.static_api)",
                       h->game_toml != NULL ? h->game_toml : "game.toml");
    }
    if (r != DE_OK)
        return r;

    h->api = &m->table;
    if (m->lib != NULL)
        DE_INFO(h->log, "game '%s' attached from library %s", m->table.name, m->path);
    else
        DE_INFO(h->log, "game '%s' attached (statically linked)", m->table.name);
    return DE_OK;
}

void de__game_detach(de__host *h) {
    h->api = NULL;
    de__game_module_unload(&h->game);
}
