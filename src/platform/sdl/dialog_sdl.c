/* dialog_sdl.c: asynchronous OS file dialogs (SPEC §6; DENG-246).
 *
 * SDL calls the dialog callback on whatever thread the OS uses (a worker thread on Windows and
 * Linux portals; the main thread on macOS), possibly after de_platform_shutdown(). So:
 * - Each dialog has a slot in a static table guarded by a spinlock (static, so a late OS
 *   callback never touches freed platform state). A slot has a generation; releasing it bumps
 *   the generation, and a callback whose generation no longer matches is dropped.
 * - The request (filters and strings SDL reads until the callback) and the result copy are
 *   allocated with SDL's heap, because they are created or freed on the OS callback thread,
 *   which may run after the platform allocator is gone. Dialogs are rare and never per frame.
 * - de_platform_pump() calls de__platform_sdl_dialogs_dispatch(), which runs the user callbacks
 *   on the main thread. Shutdown cancels the dialogs that are still open.
 */
#include "platform/sdl/sdl_internal.h"

#include <string.h>

enum { DE__DLG_FREE, DE__DLG_OPEN, DE__DLG_DONE };

typedef struct de__dialog_slot {
    u32 gen;
    u8 state;
    /* Main thread only. */
    de_file_dialog_fn fn;
    void *user;
    /* Written by the OS callback under the lock (state OPEN → DONE). */
    de_result result;
    i32 filter;
    u32 count;
    char **paths; /* SDL_malloc block: count + 1 pointers, then the strings */
    char msg[160];
} de__dialog_slot;

/* What SDL reads until the callback: SDL_malloc block, freed by the callback. */
typedef struct de__dialog_req {
    u32 slot, gen;
    u32 filter_count;
    SDL_DialogFileFilter *filters; /* points into this block */
} de__dialog_req;

static de__dialog_slot de__slots[DE_PLATFORM_DIALOG_MAX];
static SDL_SpinLock de__lock;

static void SDLCALL de__show_default(SDL_FileDialogType type, SDL_DialogFileCallback cb,
                                     void *userdata, SDL_PropertiesID props) {
    SDL_ShowFileDialogWithProperties(type, cb, userdata, props);
}

static de__platform_sdl_dialog_show_fn de__show = de__show_default;

void de__platform_sdl_dialog_set_show(de__platform_sdl_dialog_show_fn fn) {
    de__show = fn ? fn : de__show_default;
}

/* ---------- OS callback (any thread) ---------- */

static void SDLCALL de__on_result(void *userdata, const char *const *list, int filter) {
    de__dialog_req *req = (de__dialog_req *)userdata;
    de_result result = DE_OK;
    u32 count = 0;
    char **paths = NULL;
    char msg[160] = {0};

    if (!list) {
        result = DE_ERR_IO;
        const char *err = SDL_GetError();
        SDL_strlcpy(msg, err && err[0] ? err : "the OS could not show the dialog", sizeof msg);
    } else if (!list[0]) {
        result = DE_ERR_CANCELLED;
    } else {
        size_t bytes = 0;
        while (list[count]) {
            bytes += strlen(list[count]) + 1u;
            count++;
        }
        size_t table = ((size_t)count + 1u) * sizeof(char *);
        paths = (char **)SDL_malloc(table + bytes);
        if (!paths) {
            result = DE_ERR_OUT_OF_MEMORY;
            count = 0;
            SDL_strlcpy(msg, "no memory for the dialog result", sizeof msg);
        } else {
            char *p = (char *)paths + table;
            for (u32 i = 0; i < count; i++) {
                size_t n = strlen(list[i]) + 1u;
                memcpy(p, list[i], n);
                paths[i] = p;
                p += n;
            }
            paths[count] = NULL;
        }
    }

    SDL_LockSpinlock(&de__lock);
    de__dialog_slot *s = &de__slots[req->slot];
    if (s->gen == req->gen && s->state == DE__DLG_OPEN) {
        s->result = result;
        s->filter = filter;
        s->count = count;
        s->paths = paths;
        memcpy(s->msg, msg, sizeof msg);
        s->state = DE__DLG_DONE;
        paths = NULL; /* the slot owns it now */
    }
    SDL_UnlockSpinlock(&de__lock);
    SDL_free(paths); /* only when the slot was released meanwhile (shutdown) */
    SDL_free(req);
}

/* ---------- main thread ---------- */

/* Takes the finished (or, with `cancel_open`, every used) slot `i` out of the table and runs its
 * callback. Returns false when there was nothing to deliver. */
static bool de__deliver(u32 i, bool cancel_open) {
    SDL_LockSpinlock(&de__lock);
    de__dialog_slot *s = &de__slots[i];
    bool done = s->state == DE__DLG_DONE;
    if (!done && !(cancel_open && s->state == DE__DLG_OPEN)) {
        SDL_UnlockSpinlock(&de__lock);
        return false;
    }
    de_file_dialog_fn fn = s->fn;
    void *user = s->user;
    de_result result = done ? s->result : DE_ERR_CANCELLED;
    i32 filter = done ? s->filter : -1;
    u32 count = done ? s->count : 0u;
    char **paths = done ? s->paths : NULL;
    char msg[sizeof s->msg];
    memcpy(msg, s->msg, sizeof msg);
    /* Release before the callback, so it may open a new dialog in this slot. */
    s->state = DE__DLG_FREE;
    s->gen++;
    s->fn = NULL;
    s->user = NULL;
    s->paths = NULL;
    s->count = 0;
    SDL_UnlockSpinlock(&de__lock);

    if (result != DE_OK && result != DE_ERR_CANCELLED)
        (void)DE_FAIL(result, "file dialog: %s", msg);
    if (fn)
        fn(user, result, (const char *const *)paths, count, filter);
    SDL_free(paths);
    return true;
}

void de__platform_sdl_dialogs_dispatch(void) {
    for (u32 i = 0; i < DE_PLATFORM_DIALOG_MAX; i++)
        de__deliver(i, false);
}

void de__platform_sdl_dialogs_shutdown(void) {
    for (u32 i = 0; i < DE_PLATFORM_DIALOG_MAX; i++)
        de__deliver(i, true);
}

static void de__release(u32 slot) {
    SDL_LockSpinlock(&de__lock);
    de__slots[slot].state = DE__DLG_FREE;
    de__slots[slot].gen++;
    de__slots[slot].fn = NULL;
    de__slots[slot].user = NULL;
    SDL_UnlockSpinlock(&de__lock);
}

static size_t de__len(const char *s) {
    return s ? strlen(s) + 1u : 0u;
}

static char *de__put(char **cursor, const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1u;
    char *dst = *cursor;
    memcpy(dst, s, n);
    *cursor += n;
    return dst;
}

de_result de_platform_file_dialog(const de_file_dialog_desc *desc, de_file_dialog_fn fn,
                                  void *user) {
    if (!desc || !fn)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "file dialog: NULL desc or callback");
    SDL_FileDialogType type;
    switch (desc->kind) {
    case DE_FILE_DIALOG_OPEN:
        type = SDL_FILEDIALOG_OPENFILE;
        break;
    case DE_FILE_DIALOG_SAVE:
        type = SDL_FILEDIALOG_SAVEFILE;
        break;
    case DE_FILE_DIALOG_FOLDER:
        type = SDL_FILEDIALOG_OPENFOLDER;
        break;
    default:
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "file dialog: bad kind %d", (int)desc->kind);
    }
    u32 nfilters = desc->kind == DE_FILE_DIALOG_FOLDER ? 0u : desc->filter_count;
    if (nfilters > 0 && !desc->filters)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "file dialog: filter_count without filters");
    if (nfilters > 1024)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "file dialog: %u filters", (unsigned)nfilters);
    for (u32 i = 0; i < nfilters; i++)
        if (!desc->filters[i].name || !desc->filters[i].pattern)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "file dialog: filter %u has a NULL string",
                           (unsigned)i);

    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || st->headless || !st->devices)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "file dialog: no video (headless)");
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    SDL_Window *parent = NULL;
    if (DE_HANDLE_VALID(desc->parent)) {
        parent = de__platform_sdl_window_get(desc->parent);
        if (!parent)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "file dialog: stale parent window");
    }

    /* Reserve a slot. */
    u32 slot = DE_PLATFORM_DIALOG_MAX, gen = 0;
    SDL_LockSpinlock(&de__lock);
    for (u32 i = 0; i < DE_PLATFORM_DIALOG_MAX; i++) {
        if (de__slots[i].state == DE__DLG_FREE) {
            slot = i;
            gen = de__slots[i].gen;
            de__slots[i].state = DE__DLG_OPEN;
            de__slots[i].fn = fn;
            de__slots[i].user = user;
            break;
        }
    }
    SDL_UnlockSpinlock(&de__lock);
    if (slot == DE_PLATFORM_DIALOG_MAX)
        return DE_FAIL(DE_ERR_BUSY, "file dialog: %d dialogs are already open",
                       DE_PLATFORM_DIALOG_MAX);

    /* One block: the request, the filter array and its strings. */
    size_t bytes = sizeof(de__dialog_req) + (size_t)nfilters * sizeof(SDL_DialogFileFilter);
    for (u32 i = 0; i < nfilters; i++)
        bytes += de__len(desc->filters[i].name) + de__len(desc->filters[i].pattern);
    de__dialog_req *req = (de__dialog_req *)SDL_malloc(bytes);
    if (!req) {
        de__release(slot);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "file dialog: no memory for the request");
    }
    req->slot = slot;
    req->gen = gen;
    req->filter_count = nfilters;
    req->filters = nfilters ? (SDL_DialogFileFilter *)(void *)(req + 1) : NULL;
    char *cursor = (char *)(req + 1) + (size_t)nfilters * sizeof(SDL_DialogFileFilter);
    for (u32 i = 0; i < nfilters; i++) {
        req->filters[i].name = de__put(&cursor, desc->filters[i].name);
        req->filters[i].pattern = de__put(&cursor, desc->filters[i].pattern);
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        SDL_free(req);
        de__release(slot);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "file dialog: properties: %s", SDL_GetError());
    }
    if (nfilters) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, req->filters);
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, (Sint64)nfilters);
    }
    if (parent)
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, parent);
    if (desc->default_path)
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, desc->default_path);
    if (desc->title)
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, desc->title);
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN,
                           desc->allow_many && desc->kind != DE_FILE_DIALOG_SAVE);

    /* SDL copies what it needs from `props` before returning (filters excepted, which live in
     * `req` until the callback). The callback may already have run when this returns. */
    de__show(type, de__on_result, req, props);
    SDL_DestroyProperties(props);
    return DE_OK;
}
