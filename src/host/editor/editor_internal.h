/*
 * editor_internal.h: the Dev-only editor service (DENG-555, DENG-622). DIDE starts the game with
 * `--editor-service <endpoint>` and the session token in DE_EDITOR_TOKEN; the service listens
 * there, and DIDE connects and speaks the editor IPC (third_party/dide_protocol, the DIDE IDL).
 *
 * The connection runs on its own service thread (editor_ipc.h, DENG-784): it answers the small
 * reads (`ping`, `session.info`) from a frame-boundary copy and queues every other request. A
 * frame-begin hook applies the queued requests between frames, where the world can be read and
 * edited safely; all fields below are frame-thread only. Each area registers
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
/** Applies an edit op an area owns (editor_edit.c asks it for its prefix); fills `inv` with the
 * inverse, or `code` and `msg` and returns false. Must change nothing when it fails. */
typedef bool (*de__ed_op_fn)(de__editor *ed, de_ep_arena *a, const de_ep_edit_op *o,
                             de_ep_edit_op *inv, de_ep_error *code, char *msg, size_t cap);

#define DE__ED_MAX_TICKS 16u
#define DE__ED_MAX_CAPS  32u

struct de__editor {
    struct de__host *h;
    de_engine *e;
    de_log_category log;

    struct de__ed_ipc *ipc; /**< the service thread: listener, connection, second-IDE refusal */
    bool connected, authed;
    bool reload_outcomes; /**< negotiated peer understands optional reload failure (IPC >= 1.6) */
    /** The agreed protocol version (hello); 0.0 before a hello. DENG-785 gates the 1.15 ui.editor
     * methods and PickResult.uiStack on it. */
    u16 proto_major, proto_minor;
    char token[160];
    u32 epoch;         /**< bumped on each connection: ids from an older one are stale */
    u64 seq;           /**< notification sequence number */
    u64 idle_since_ns; /**< when the service last had no connection */
    u64 grace_ns;      /**< quit after this long without a connection */
    u64 blob_next;     /**< next blob id */

    char *out;
    size_t out_cap; /**< the response being written */
    char *note;
    size_t note_cap; /**< a notification being written */
    unsigned char *arena;
    size_t arena_cap;

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
    struct de__ed_water *water;
    de__ed_op_fn terrain_op; /* DENG-580: the terrain.* ops (editor_terrain.c), NULL without */
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
void de__ed_assets_shutdown(de__editor *ed);
void de__ed_add_capability(de__editor *ed, const char *cap);
/** True when `m` has a handler and needs no capability, or needs one the engine offered in hello.
 * The dispatcher answers `unsupported` otherwise (a method of an unoffered capability). */
bool de__ed_method_offered(const de__editor *ed, de_ep_method m);
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
/** Replaces an oversized response with the ordinary Busy error; caller still owns the writer. */
void de__ed_respond_limit(de__editor *ed, mpack_writer_t *w, u64 request);

/** How long a `busy` reply asks the IDE to wait before it retries (`data.retryAfterMs`). */
#define DE__ED_RETRY_AFTER_MS 100u

/** Writes an error response; `fmt` is printf-style. A `busy` error carries `data.retryAfterMs`. */
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
/** Reads a string-keyed map entry, borrowing the complete encoded value from a
 * mpack_reader_init_data reader only. On failure, clears the entry and retains the error. */
bool de__ed_raw_map_entry(mpack_reader_t *r, de_ep_kv_raw *out);

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
/** An entity's scene and stable scene id; false when no scene has it. */
bool de__ed_scene_member(u64 entity, u64 *scene, u64 *sid);
/** Puts an entity in `scene` with stable id `sid` (an undone removal gets its ids back). */
bool de__ed_scene_member_set(u64 entity, u64 scene, u64 sid);
/** Takes an entity out of its scene. */
void de__ed_scene_member_drop(u64 entity);
/** Every entity of the tree under `root`, root first, breadth-first, at most `cap`. */
u32 de__ed_scene_tree(de__editor *ed, de_entity root, u64 *out, u32 cap);
/** Instantiates prefab `asset` under `parent` into `scene`: new entities and stable ids, and an
 * instance record with the fields as instantiated. False with `code` and `msg` set; then nothing
 * changed. `a` is scratch (what is in it already stays). */
bool de__ed_prefab_spawn(de__editor *ed, de_ep_arena *a, const char *asset, u64 scene,
                         de_entity parent, de_entity *root, de_ep_error *code, char *msg,
                         size_t msg_cap);
/** The prefab asset of the instance whose root is `root`; NULL when `root` is not one. */
const char *de__ed_prefab_instance_asset(u64 root);
/** The instance record's fields as instantiated: an array of {entity, path, value} maps. */
void de__ed_prefab_instance_write_base(u64 root, mpack_writer_t *w);
/** Forgets the instance record whose root is `root`. */
void de__ed_prefab_instance_drop(u64 root);
/** The instance record's prefab-local ids: an array of [entity, scene id] pairs (DENG-571). */
void de__ed_prefab_instance_write_local(u64 root, mpack_writer_t *w);
/** Adds an instance record from what de__ed_prefab_instance_write_base and _write_local wrote
 * (`local` may be NULL: then the record has no prefab-local ids). */
bool de__ed_prefab_instance_restore(u64 root, const char *asset, const char *base, size_t len,
                                    const char *local, size_t local_len);
/** The `prefab.state` edit op (DENG-571): sets prefab definition values and instance base
 * values from `value` and writes the old ones into `*inv` (in `a`). False with `code`/`msg`. */
bool de__ed_prefab_state(de__editor *ed, de_ep_arena *a, de_ep_raw value, de_ep_raw *inv,
                         de_ep_error *code, char *msg, size_t msg_cap);
/** Runs `ops` as one edit, all or nothing (DENG-571): in open transaction `tx`, or, when `tx` is
 * 0, in a new transaction of `scene` that it commits. Returns the transaction id; 0 with
 * `code`/`msg` set on a failure, and then nothing changed. */
u32 de__ed_edit_run(de__editor *ed, de_ep_arena *a, u32 tx, u64 scene, const de_ep_edit_op *ops,
                    u32 count, de_ep_error *code, char *msg, size_t msg_cap);
/** The source text of asset `id` (heap memory the caller frees with `*size`); false when the
 * manifest has no source for it or the file can't be read. */
bool de__ed_asset_source(de__editor *ed, const char *id, void **data, u64 *size);
/** The virtual path of asset `id`'s source file (DENG-798); false when the manifest has none. */
bool de__ed_asset_source_vpath(de__editor *ed, const char *id, char *out, size_t cap);
/** The asset's source path as the manifest lists it (relative to the asset root), or NULL. */
const char *de__ed_asset_source_path(de__editor *ed, const char *id);
/** The id of the manifest's `i`th asset of type `type`, in manifest order, or NULL when there are
 * `i` or fewer (DENG-807). */
const char *de__ed_asset_nth(de__editor *ed, const char *type, u32 i);

/** One edit to write into a prefab source (DENG-798): `path` is "Component.field" and `val` the
 * editor's MessagePack value in prefab space; or `path` is "Component" and `val` the whole
 * component as a map of field to value, or nil to take the component away (DENG-810). A
 * `follow` edit is for a level that inherits the field or component: it only replaces a value
 * the source already sets, and only drops a component the source has. Path "" is the whole
 * entity (DENG-811): `val` nil drops its block, or {parent: id, components: {Comp: map}} appends
 * one. Path "@nest" appends {root, inner, outer} to the ids of the [[prefab.nested]] entry whose
 * root is `root`. */
typedef struct de__ed_src_edit {
    u64 sid;
    const char *path;
    const void *val;
    size_t val_len;
    bool follow;
} de__ed_src_edit;
/** Applies `edits` to the source text `src` in place, keeping comments, key order and line
 * endings. Returns NULL with `*out` (free with de__ed_prefab_source_free) on success, or a
 * reason ("source_layout", "value", "memory") with `msg` set. */
const char *de__ed_prefab_source_edit(const de_type_registry *types, const char *src, size_t len,
                                      const de__ed_src_edit *edits, u32 count, char **out,
                                      size_t *out_len, char *msg, size_t msg_cap);
void de__ed_prefab_source_free(char *text, size_t len);
/** A prefab asset was reloaded (DENG-803): reads the definitions that depend on it again; when
 * any changed, bumps the prefab revision and updates the instances' non-overridden fields. */
void de__ed_prefab_reloaded(de__editor *ed, de_asset_id id);
/** Writes `value` into `path` ("Component.field") of `entity` directly: no transaction, no undo
 * entry (DENG-803). `a` is scratch. */
bool de__ed_set_value(de__editor *ed, de_ep_arena *a, u64 entity, const char *path,
                      de_ep_raw value);
/** The reason of the last failed prefab.state op ("write", "source_changed", ...), or NULL. */
const char *de__ed_prefab_fail_reason(void);
/** The component called `name` in the world (id 0 when there is none). */
de_component de__ed_component(de__editor *ed, de_ep_str name);
/** A component's reflected type and data size; false for an unknown component. */
bool de__ed_component_type(de__editor *ed, de_component c, de_type_id *type, u32 *size);
/** Marks the world changed so the next world.delta sees the editor's own writes. */
void de__ed_world_touch(de__editor *ed);
/** The entities spawned by open edit transactions; fills up to `cap`, returns the count. */
u32 de__ed_edit_open_spawns(u64 *out, u32 cap);
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
void de__ed_animation_register(de__editor *ed);
void de__ed_animation_invalidate(de__editor *ed);
void de__ed_animation_shutdown(de__editor *ed);
void de__ed_logs_register(de__editor *ed);
void de__ed_views_register(de__editor *ed);
void de__ed_debugdraw_register(de__editor *ed);
void de__ed_extra_register(de__editor *ed);
/* DENG-769: materialGraph.* (editor_material_graph.c); Debug/Dev with the cooker and Slang only
 * (DE_ED_MATERIAL_GRAPH). */
void de__ed_material_graph_register(de__editor *ed);
/* DENG-775: spline.info, spline.sample, spline.instances (editor_spline.c); capability
 * world.splines (protocol 1.12). */
void de__ed_spline_register(de__editor *ed);
/** An entity's world transform, as the gizmo reads it: a root's local transform, else its global
 * one (or parent * local when that is not computed yet). Identity without DeTransform. */
de_transform de__ed_world_xf(de_world *w, de_entity e);
/* DENG-776: water.info, water.surface, water.flowMap, water.probes, water.showProbes
 * (editor_water.c, render builds only); capability world.water (protocol 1.13). */
void de__ed_water_register(de__editor *ed);
/* DENG-580: terrain.info, terrain.sample, terrain.exportHeightmap and the terrain.* edit ops
 * (editor_terrain.c, render builds only); capability world.terrain (protocol 1.11). */
void de__ed_terrain_register(de__editor *ed);
/* DENG-778: ui.layout (editor_ui.c, UI builds only); capability ui.editor (protocol 1.14). DENG-785
 * adds the 1.15 methods: ui.nodeCorners, ui.themes, ui.themeVars, ui.previewTheme, loc.locales,
 * loc.strings and loc.previewLocale (`unsupported` below 1.15). The
 * ui.editor EditOp.index is in editor_edit.c and works in every build. */
void de__ed_ui_register(de__editor *ed);
void de__ed_ui_shutdown(de__editor *ed);
/** True when the agreed protocol is at least major.minor. */
static inline bool de__ed_protocol_at_least(const de__editor *ed, u16 major, u16 minor) {
    return ed->proto_major > major || (ed->proto_major == major && ed->proto_minor >= minor);
}
/* DENG-785 (protocol 1.15, ui.editor): the UI pick stack for a viewport pick. */
#define DE__ED_UI_STACK 32u
/** One UI node under a pick point (de_ui_pick_hit): rect is [x, y, w, h] in the root's pixels. */
typedef struct de__ed_ui_hit {
    u64 entity, root;
    f32 rect[4];
    u32 depth;
} de__ed_ui_hit;
/** The UI nodes under a viewport point, topmost first, up to `capacity`; returns how many were
 * written. `origin`/`direction` NULL: the screen UI at (x, y) in viewport pixels (a game view).
 * Otherwise the world panels under the world-space ray (a scene view). 0 without a UI, below
 * protocol 1.15. Defined in editor_ui.c (UI builds only). */
u32 de__ed_ui_pick(de__editor *ed, f32 x, f32 y, const f64 *origin, const f32 *direction,
                   de__ed_ui_hit *out, u32 capacity);
/** Sets PickResult.uiStack from `n` hits (absent when n is 0). `items` holds `n` entries and
 * must outlive the write; each item's rect points into its hit. */
static inline void de__ed_pick_set_ui_stack(de_ep_pick_result *r, const de__ed_ui_hit *hits, u32 n,
                                            de_ep_ui_pick_hit *items) {
    r->has_ui_stack = n > 0;
    r->ui_stack.items = n > 0 ? items : NULL;
    r->ui_stack.count = n;
    for (u32 i = 0; i < n; ++i)
        items[i] = (de_ep_ui_pick_hit){.entity = hits[i].entity,
                                       .root = hits[i].root,
                                       .rect = {(f32 *)hits[i].rect, 4},
                                       .depth = hits[i].depth};
}
/** A probe water.showProbes shows: world centre, radius (m), depth below the surface (m). */
typedef struct de__ed_probe_mark {
    de_dvec3 p;
    f32 radius, depth;
    bool wet;
} de__ed_probe_mark;
/** Fills up to `max` probes of the bodies water.showProbes shows; returns how many. */
u32 de__ed_water_marks(de__editor *ed, de__ed_probe_mark *out, u32 max);
/* Fills the hello result for the agreed version (editor.c). `caps` holds DE__ED_MAX_CAPS. */
void de__ed_hello_result(de__editor *ed, const char *agreed, de_ep_str *caps,
                         de_ep_hello_result *r);

DE_EXTERN_C_END
#endif /* DE_HOST_EDITOR_INTERNAL_H */
