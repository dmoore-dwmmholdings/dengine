/*
 * editor_internal.h: the Dev-only editor service (DENG-555, DENG-622). DIDE starts the game with
 * `--editor-service <endpoint>` and the session token in DE_EDITOR_TOKEN; the service listens
 * there, and DIDE connects and speaks the editor IPC (third_party/dide_protocol, the DIDE IDL).
 *
 * Everything runs on the main thread: a frame-begin hook polls the connection, so requests are
 * handled between frames, where the world can be read and edited safely. Each area registers
 * its method handlers in `de__editor.handlers`; a method nobody handles answers `unsupported`.
 *
 * Include as "host/editor/editor_internal.h". Internal to src/host/.
 */
#ifndef DE_HOST_EDITOR_INTERNAL_H
#define DE_HOST_EDITOR_INTERNAL_H

#include <dengine/de_game.h>

#include "de_editor_protocol.h"
#include "de_editor_transport.h"

DE_EXTERN_C_BEGIN

struct de__host;
typedef struct de__editor de__editor;

/** Writes the response (or an error) for `req` into `w`. `a` holds decoded lists and maps. */
typedef void (*de__ed_handler)(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                               mpack_writer_t *w);
/** Runs once per frame while connected, after requests (for subscriptions and deltas). */
typedef void (*de__ed_tick_fn)(de__editor *ed);
/** Runs when a connection ends (state tied to the connection is dropped). */
typedef void (*de__ed_reset_fn)(de__editor *ed);

#define DE__ED_MAX_TICKS 16u
#define DE__ED_MAX_CAPS  8u

struct de__editor {
    struct de__host *h;
    de_engine *e;
    de_log_category log;

    de_ep_listener listener;
    de_ep_conn conn;
    bool connected, authed;
    bool reload_outcomes; /**< negotiated peer understands optional reload failure (IPC >= 1.6) */
    char token[160];
    u32 epoch;          /**< bumped on each connection: ids from an older one are stale */
    u64 seq;            /**< notification sequence number */
    u64 idle_since_ns;  /**< when the service last had no connection */
    u64 grace_ns;       /**< quit after this long without a connection */
    u64 blob_next;      /**< next blob id */

    char *in;   size_t in_cap;   /**< one received frame */
    char *out;  size_t out_cap;  /**< the response being written */
    char *note; size_t note_cap; /**< a notification being written */
    unsigned char *arena; size_t arena_cap;

    de__ed_handler handlers[DE_EP_METHOD_COUNT];
    de__ed_tick_fn ticks[DE__ED_MAX_TICKS];
    de__ed_reset_fn resets[DE__ED_MAX_TICKS];
    u32 tick_count, reset_count;
    const char *caps[DE__ED_MAX_CAPS];
    u32 cap_count;

    de_ep_world_mode mode; /**< P5 drives it; Edit until then */
    bool deferred;         /**< the handler answers later (de__ed_defer) */

    /* Per-area state, owned by the area's file. */
    struct de__ed_world *world;
    struct de__ed_edit *edit;
    struct de__ed_session *session;
    struct de__ed_scene *scene;
    struct de__ed_assets *assets;
    struct de__ed_logs *logs;
    struct de__ed_views *views;
    struct de__ed_debugdraw *debugdraw;
    struct de__ed_extra *extra;
};

/* ---------- Lifecycle (editor.c) ---------- */

/** Before the game's init, when the command line has `--editor-service`: what the service needs
 * from assets the game loads (meshes keep LOD 0 on the CPU for surface placement). */
void de__editor_preinit(struct de__host *h);
/** Starts the service when the command line has `--editor-service <endpoint>`; otherwise does
 * nothing. Needs DE_EDITOR_TOKEN. Call after the game registered its types and systems. */
de_result de__editor_start(struct de__host *h);
/** Closes the connection and the listener and frees the service. Safe when it never started. */
void de__editor_stop(struct de__host *h);

/* ---------- For the areas (editor.c) ---------- */

void de__ed_on(de__editor *ed, de_ep_method m, de__ed_handler fn);
void de__ed_on_tick(de__editor *ed, de__ed_tick_fn fn);
void de__ed_on_reset(de__editor *ed, de__ed_reset_fn fn);
void de__ed_add_capability(de__editor *ed, const char *cap);
#if DE_HAS_RENDER
void de__ed_preview_register(de__editor *ed);
void de__ed_preview_invalidate(void);
void de__ed_preview_shutdown(de__editor *ed);
void de__ed_settings_register(de__editor *ed);
void de__ed_settings_pump(de__editor *ed);
void de__ed_settings_invalidate(void);
void de__ed_settings_shutdown(de__editor *ed);
#endif

/** Called by a handler that answers later: the dispatcher sends nothing now. The area keeps the
 * request id and answers with de__ed_respond_begin / de__ed_send(kind response). */
void de__ed_defer(de__editor *ed);
/** Starts a writer over the response buffer, outside a dispatch. */
void de__ed_respond_begin(de__editor *ed, mpack_writer_t *w);

/** Writes an error response; `fmt` is printf-style. */
void de__ed_error(mpack_writer_t *w, u64 id, de_ep_error code, const char *fmt, ...);
/** Starts a notification writer over the notification buffer; returns its sequence number. */
u64 de__ed_note_begin(de__editor *ed, mpack_writer_t *w);
/** Sends a finished writer (`out` or `note` buffer) as a frame of `kind`. False on a lost link. */
bool de__ed_send(de__editor *ed, mpack_writer_t *w, u8 kind);
/** Sends `bytes` as blob frames and returns the blob id (0 when not connected). */
u64 de__ed_send_blob(de__editor *ed, const void *bytes, size_t n);
/** The current frame number. */
u64 de__ed_frame(const de__editor *ed);
/** A NUL-terminated copy of `s` in the arena (NULL when it's full). */
char *de__ed_cstr(de_ep_arena *a, de_ep_str s);
/** Arena memory (NULL when it's full). */
void *de__ed_alloc(de_ep_arena *a, size_t size);

/** Starts a MessagePack writer over the arena's free space, for an `any` value. */
void de__ed_raw_begin(de_ep_arena *a, mpack_writer_t *w);
/** Ends it: the bytes written, kept in the arena (an empty raw when it didn't fit). */
de_ep_raw de__ed_raw_end(de_ep_arena *a, mpack_writer_t *w);
de_ep_raw de__ed_raw_str(de_ep_arena *a, const char *s);
de_ep_raw de__ed_raw_f64(de_ep_arena *a, f64 v);
de_ep_raw de__ed_raw_bool(de_ep_arena *a, bool v);
de_ep_raw de__ed_raw_range(de_ep_arena *a, f64 lo, f64 hi);

/** Decodes the params of `method` into `var`, or answers invalid_params and returns. */
#define DE__ED_PARAMS(method, type, var)                                                           \
    type var;                                                                                      \
    memset(&var, 0, sizeof var);                                                                   \
    if (!de_ep_read_params_##method(req, a, &var)) {                                               \
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, #method ": bad params");                \
        return;                                                                                    \
    }

/* ---------- Values (editor_value.c) and reflection (editor_reflect.c) ---------- */

const char *de__ed_kind_name(de_field_kind k);
/** A field's FieldInfo; `owner` (the struct's type name) finds its inspector hints. */
void de__ed_field_info(de_ep_arena *a, const char *owner, const de_field_info *f,
                       de_ep_field_info *out);
/** A struct as a map of field name to value. */
void de__ed_write_object(de__editor *ed, mpack_writer_t *w, de_type_id type, const void *obj);
/** One field. `owner`/`index` name the field in its struct (for fixed arrays); 0/0 otherwise. */
void de__ed_write_field(de__editor *ed, mpack_writer_t *w, de_type_id owner, u32 index,
                        const de_field_info *f, const void *p);
/** Reads a map of field name to value into `obj` (fields it leaves out stay). */
bool de__ed_read_object(de__editor *ed, mpack_node_t n, de_type_id type, void *obj);
bool de__ed_read_field(de__editor *ed, mpack_node_t n, de_type_id owner, u32 index,
                       const de_field_info *f, void *p);

/** An asset handle's ID string (NULL: none, or not in the manifest). */
const char *de__ed_asset_path(de__editor *ed, u32 handle);
/** The typed handle of the asset `path` (`type` may be NULL); 0 when there is none. */
u32 de__ed_asset_handle(de__editor *ed, const char *path, const char *type);
/** Remembers the sub-asset paths (`model#mesh`) quoted in `text` (`n` bytes; scene text), so a
 * value naming one reads back as its path: sub-assets are no manifest entries, and an asset ID is
 * a hash. */
void de__ed_note_asset_paths(const char *text, size_t n);

/* ---------- World (editor_world.c) ---------- */

/** The entity's revision: the newest added/changed tick across its components (0: not alive). */
u64 de__ed_entity_rev(de__editor *ed, de_entity e);
/** An entity as a row: every component, or only `components` (names), or only `fields`
 * ("Component.path"). False when the entity is not alive. */
bool de__ed_entity_row(de__editor *ed, de_ep_arena *a, de_entity e,
                       const de_ep_list_str *components, const de_ep_list_str *fields,
                       de_ep_entity_row *out);
/** The scene an entity belongs to (editor_scene.c; 1, the main scene, by default). */
u64 de__ed_scene_of(de__editor *ed, de_entity e);
/** True for the editor's own entities (its views' cameras): not part of any scene, so world
 * queries, deltas and saves leave them out and the IDE never lists them. */
bool de__ed_views_owns(u64 entity);
/** Supported selectable view outputs; render-graph pass names are not image outputs. */
u32 de__ed_views_render_modes(const char *const **names);
/** Entity `e`, spawned by an edit, joins `scene` (0: `parent`'s scene, or the active one) with a
 * new scene id, so a save writes it to that scene. */
void de__ed_scene_adopt(de__editor *ed, de_entity e, u64 scene, de_entity parent);
/** The component called `name` in the world (id 0 when there is none). */
de_component de__ed_component(de__editor *ed, de_ep_str name);
/** A component's reflected type and data size; false for an unknown component. */
bool de__ed_component_type(de__editor *ed, de_component c, de_type_id *type, u32 *size);
/** Marks the world changed so the next world.delta sees the editor's own writes. */
void de__ed_world_touch(de__editor *ed);
/** Forces the next world.delta of every subscription to be a full resync. */
void de__ed_world_resync(de__editor *ed);

/* ---------- Areas: each registers its handlers ---------- */

void de__ed_session_register(de__editor *ed);
/** Main-thread game-library reload telemetry; never called from a crash handler. */
void de__ed_game_reloading(struct de__host *h);
void de__ed_game_reloaded(struct de__host *h, u64 started_ns, de_result result);
void de__ed_reflect_register(de__editor *ed);
void de__ed_world_register(de__editor *ed);
void de__ed_edit_register(de__editor *ed);
void de__ed_scene_register(de__editor *ed);
void de__ed_assets_register(de__editor *ed);
void de__ed_logs_register(de__editor *ed);
void de__ed_views_register(de__editor *ed);
void de__ed_debugdraw_register(de__editor *ed);
void de__ed_extra_register(de__editor *ed);

DE_EXTERN_C_END
#endif /* DE_HOST_EDITOR_INTERNAL_H */
