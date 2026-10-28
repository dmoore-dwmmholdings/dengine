/* editor_session.c: session methods (DENG-623), modes, step, restart and snapshots (DENG-627).
 *
 * Modes: in Edit and Paused the game's own systems (owner tag DE__HOST_GAME_SYSTEM_OWNER) are
 * disabled, so the world holds still while the engine's systems (transforms, UI, rendering) keep
 * running. Play and Simulate run them. Entering Play or Simulate from Edit takes a world
 * snapshot; going back to Edit restores it. `session.step` runs the game systems for N frames.
 *
 * Snapshots keep two forms: the ECS snapshot (de_world_snapshot, for restore) and every field
 * of every component as MessagePack (for snapshot.diff). Game state kept outside the world (a
 * game's static variables) is not part of either. */
#include "host/editor/editor_internal.h"

#include <dengine/de_memory.h>

#include "ecs/ecs_internal.h"
#include "ecs/system_internal.h"
#include "host/host_internal.h"

#include <stdlib.h>
#include <string.h>

#define ED_MAX_SNAPS 64u
#define ED_STEP_MAX  1000u

/* One component field of one entity, in a snapshot's diff form. */
typedef struct snap_field {
    u64 entity;
    u32 comp;  /* component id */
    u32 field; /* field index; 0xFFFFFFFF for a tag (no data) */
    u32 off, len;
} snap_field;

typedef struct ed_snap {
    u32 id; /* 0: free */
    de_world_snapshot ecs;
    snap_field *fields;
    u32 field_count;
    u8 *bytes;
    size_t byte_count;
} ed_snap;

typedef struct de__ed_session {
    ed_snap snaps[ED_MAX_SNAPS];
    u32 next_snap;
    de_world_snapshot play;  /* taken on entering Play/Simulate from Edit */
    bool has_play;
    de_world_snapshot initial; /* at service start, for session.restart */
    bool has_initial;
    u32 step_frames; /* frames left to step */
    bool game_on;    /* the game systems are enabled */
} de__ed_session;

static de__ed_session g_session;

void de__ed_session_build_info(de__editor *ed, de_ep_build_info *b);
void de__ed_edit_forget(de__editor *ed);

/* ---------- Game systems on/off ---------- */

static void game_systems(de__editor *ed, bool on) {
    if (g_session.game_on == on)
        return;
    g_session.game_on = on;
    de_world *w = ed->e->world;
    const ecs_system_registry *reg = w->systems;
    for (u32 i = 0; reg != NULL && i < reg->slots.count; ++i) {
        const ecs_system_slot *slot = (const ecs_system_slot *)de_array_at(&reg->slots, i);
        /* Simulation and input stop; presentation (PostUpdate and later) keeps running, so the
         * game's view follows edits in Edit mode. */
        if (slot->sys != NULL && slot->sys->owner == DE__HOST_GAME_SYSTEM_OWNER &&
            slot->sys->schedule < DE_SCHEDULE_POST_UPDATE)
            de_system_set_enabled(w, slot->sys->handle, on);
    }
}

static bool running_mode(de_ep_world_mode m) {
    return m == DE_EP_WORLD_MODE_PLAY || m == DE_EP_WORLD_MODE_SIMULATE;
}

static void send_state(de__editor *ed) {
    de_ep_session_state s;
    s.mode = ed->mode;
    s.frame = de__ed_frame(ed);
    mpack_writer_t w;
    const u64 seq = de__ed_note_begin(ed, &w);
    de_ep_write_notify_session_state(&w, seq, &s);
    de__ed_send(ed, &w, DE_EP_KIND_NOTIFICATION);
}

/* ---------- Snapshots ---------- */

static void snap_free(ed_snap *s) {
    const de_allocator *al = de_heap_allocator();
    if (s->ecs.data != NULL)
        de_world_snapshot_free(al, &s->ecs);
    if (s->fields != NULL)
        DE_FREE(al, s->fields, sizeof(snap_field) * s->field_count);
    if (s->bytes != NULL)
        DE_FREE(al, s->bytes, s->byte_count);
    memset(s, 0, sizeof *s);
}

static int cmp_field(const void *x, const void *y) {
    const snap_field *a = (const snap_field *)x, *b = (const snap_field *)y;
    if (a->entity != b->entity)
        return a->entity < b->entity ? -1 : 1;
    if (a->comp != b->comp)
        return a->comp < b->comp ? -1 : 1;
    return a->field < b->field ? -1 : a->field > b->field;
}

/* Every field of every component, as MessagePack, sorted by (entity, component, field). */
static bool snap_encode(de__editor *ed, ed_snap *s) {
    de_world *w = ed->e->world;
    const de_allocator *al = de_heap_allocator();
    u32 cap = 0;
    for (u32 ai = 0; ai < w->archetypes.count; ++ai) {
        const ecs_archetype *a = ((ecs_archetype *const *)w->archetypes.data)[ai];
        u32 per = 0;
        for (u32 i = 0; i < a->component_count; ++i) {
            const ecs_component_info *ci = ecs_component_get(w, a->components[i]);
            u32 fc = 0;
            if (ci != NULL && ci->size != 0)
                (void)de_type_fields(ed->e->types, ci->type, &fc);
            per += fc > 0 ? fc : 1u;
        }
        cap += a->count * per;
    }
    s->fields = (snap_field *)DE_ALLOC(al, sizeof(snap_field) * (cap + 1), 8, DE_MEM_GENERAL);
    if (s->fields == NULL)
        return false;
    /* The bytes go through the note buffer in pieces. */
    size_t bcap = 1u << 20, bn = 0;
    u8 *bytes = (u8 *)DE_ALLOC(al, bcap, 8, DE_MEM_GENERAL);
    if (bytes == NULL)
        return false;
    u32 n = 0;
    for (u32 ai = 0; ai < w->archetypes.count; ++ai) {
        const ecs_archetype *a = ((ecs_archetype *const *)w->archetypes.data)[ai];
        for (u32 ci_ = 0; ci_ < a->chunks.count; ++ci_) {
            const ecs_chunk *ch = ecs_table_chunk(a, ci_);
            for (u32 r = 0; r < ch->count; ++r) {
                const u64 ent = ecs_chunk_entities(ch)[r].bits;
                for (u32 i = 0; i < a->component_count; ++i) {
                    const ecs_component_info *ci = ecs_component_get(w, a->components[i]);
                    if (ci == NULL)
                        continue;
                    const u32 col = a->column_of[i];
                    u32 fc = 0;
                    const de_field_info *fields =
                        ci->size != 0 ? de_type_fields(ed->e->types, ci->type, &fc) : NULL;
                    if (col == ECS_INVALID_INDEX || fc == 0) {
                        s->fields[n++] = (snap_field){ent, a->components[i], 0xFFFFFFFFu, 0, 0};
                        continue;
                    }
                    const u8 *data = (const u8 *)ecs_chunk_column(a, ch, col) + (size_t)r * ci->size;
                    for (u32 f = 0; f < fc && n < cap; ++f) {
                        mpack_writer_t mw;
                        mpack_writer_init(&mw, ed->note, ed->note_cap);
                        de__ed_write_field(ed, &mw, ci->type, f, &fields[f], data + fields[f].offset);
                        const size_t len = mpack_writer_buffer_used(&mw);
                        if (mpack_writer_destroy(&mw) != mpack_ok)
                            continue;
                        if (bn + len > bcap) {
                            size_t ncap = bcap * 2;
                            while (bn + len > ncap)
                                ncap *= 2;
                            u8 *grown = (u8 *)DE_ALLOC(al, ncap, 8, DE_MEM_GENERAL);
                            if (grown == NULL) {
                                DE_FREE(al, bytes, bcap);
                                return false;
                            }
                            memcpy(grown, bytes, bn);
                            DE_FREE(al, bytes, bcap);
                            bytes = grown;
                            bcap = ncap;
                        }
                        memcpy(bytes + bn, ed->note, len);
                        s->fields[n++] = (snap_field){ent, a->components[i], f, (u32)bn, (u32)len};
                        bn += len;
                    }
                }
            }
        }
    }
    qsort(s->fields, n, sizeof(snap_field), cmp_field);
    /* Keep the allocation sizes the free uses. */
    if (n != cap + 1) {
        snap_field *exact = (snap_field *)DE_ALLOC(al, sizeof(snap_field) * (n + 1), 8, DE_MEM_GENERAL);
        if (exact != NULL) {
            memcpy(exact, s->fields, sizeof(snap_field) * n);
            DE_FREE(al, s->fields, sizeof(snap_field) * (cap + 1));
            s->fields = exact;
        }
    }
    s->field_count = n + 1; /* the free size counts the spare slot */
    s->bytes = bytes;
    s->byte_count = bcap;
    return true;
}

static ed_snap *snap_find(u32 id) {
    for (u32 i = 0; i < ED_MAX_SNAPS; ++i)
        if (g_session.snaps[i].id == id && id != 0)
            return &g_session.snaps[i];
    return NULL;
}

static bool take_world(de__editor *ed, de_world_snapshot *out) {
    memset(out, 0, sizeof *out);
    if (de_world_snapshot_take(ed->e->world, de_heap_allocator(), out) != DE_OK) {
        de_error_clear();
        return false;
    }
    return true;
}

static bool restore_world(de__editor *ed, const de_world_snapshot *s) {
    if (de_world_snapshot_restore(ed->e->world, s) != DE_OK) {
        de_error_clear();
        return false;
    }
    de__ed_world_touch(ed);
    de__ed_world_resync(ed);
    return true;
}

/* ---------- Methods ---------- */

static void on_ping(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    (void)a;
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_ping(w, req->id, &e);
}

static void on_goodbye(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    (void)ed;
    (void)a;
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_goodbye(w, req->id, &e);
}

static void on_info(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)a;
    de_ep_str caps[DE__ED_MAX_CAPS];
    for (u32 i = 0; i < ed->cap_count; ++i)
        caps[i] = de_ep_cstr(ed->caps[i]);
    de_ep_session_info s;
    memset(&s, 0, sizeof s);
    de__ed_session_build_info(ed, &s.build);
    s.capabilities.items = caps;
    s.capabilities.count = ed->cap_count;
    s.mode = ed->mode;
    s.frame = de__ed_frame(ed);
    de_ep_write_response_session_info(w, req->id, &s);
}

/** Starts the player at `at` (Play from here): the game's hook, when it has one. */
void (*de__ed_start_at_hook)(de_engine *e, const f64 *at, u32 n) = NULL;

static void on_set_mode(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                        mpack_writer_t *w) {
    DE__ED_PARAMS(session_set_mode, de_ep_set_mode_params, q);
    const de_ep_world_mode from = ed->mode, to = q.mode;
    if (running_mode(to) && !g_session.has_play && from == DE_EP_WORLD_MODE_EDIT) {
        g_session.has_play = take_world(ed, &g_session.play);
        if (q.has_start_at && de__ed_start_at_hook != NULL)
            de__ed_start_at_hook(ed->e, q.start_at.items, q.start_at.count);
    }
    if (to == DE_EP_WORLD_MODE_EDIT && g_session.has_play) {
        (void)restore_world(ed, &g_session.play);
        de_world_snapshot_free(de_heap_allocator(), &g_session.play);
        g_session.has_play = false;
    }
    ed->mode = to;
    g_session.step_frames = 0;
    game_systems(ed, running_mode(to));
    DE_INFO(ed->log, "editor: mode %s", de_ep_world_mode_name(to));
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_session_set_mode(w, req->id, &e);
    send_state(ed);
}

static void on_step(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(session_step, de_ep_step_params, q);
    if (ed->mode != DE_EP_WORLD_MODE_PAUSED) {
        de__ed_error(w, req->id, DE_EP_ERR_WRONG_MODE, "step needs Paused");
        return;
    }
    u32 n = q.has_frames ? q.frames : q.has_ticks ? q.ticks : 1u;
    if (n == 0)
        n = 1;
    if (n > ED_STEP_MAX)
        n = ED_STEP_MAX;
    g_session.step_frames = n;
    game_systems(ed, true);
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_session_step(w, req->id, &e);
}

static void on_restart(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    (void)a;
    if (g_session.has_play) {
        de_world_snapshot_free(de_heap_allocator(), &g_session.play);
        g_session.has_play = false;
    }
    if (g_session.has_initial)
        (void)restore_world(ed, &g_session.initial);
    de__ed_edit_forget(ed);
    ed->mode = DE_EP_WORLD_MODE_EDIT;
    g_session.step_frames = 0;
    game_systems(ed, false);
    DE_INFO(ed->log, "editor: restarted");
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_session_restart(w, req->id, &e);
    send_state(ed);
}

static void on_snap_take(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    (void)a;
    ed_snap *s = NULL;
    for (u32 i = 0; i < ED_MAX_SNAPS && s == NULL; ++i)
        if (g_session.snaps[i].id == 0)
            s = &g_session.snaps[i];
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many snapshots");
        return;
    }
    if (!take_world(ed, &s->ecs) || !snap_encode(ed, s)) {
        snap_free(s);
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "snapshot failed");
        return;
    }
    s->id = ++g_session.next_snap;
    de_ep_snap_result r = {s->id};
    de_ep_write_response_snapshot_take(w, req->id, &r);
}

static void on_snap_restore(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                            mpack_writer_t *w) {
    DE__ED_PARAMS(snapshot_restore, de_ep_snap_params, q);
    ed_snap *s = snap_find(q.snap);
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "snapshot %u", q.snap);
        return;
    }
    if (!restore_world(ed, &s->ecs)) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "restore failed");
        return;
    }
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_snapshot_restore(w, req->id, &e);
}

static void on_snap_release(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                            mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(snapshot_release, de_ep_snap_params, q);
    ed_snap *s = snap_find(q.snap);
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "snapshot %u", q.snap);
        return;
    }
    snap_free(s);
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_snapshot_release(w, req->id, &e);
}

static bool entity_in(const ed_snap *s, u64 ent) {
    u32 lo = 0, hi = s->field_count - 1;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (s->fields[mid].entity < ent)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < s->field_count - 1 && s->fields[lo].entity == ent;
}

static const char *field_name(de__editor *ed, u32 comp, u32 field) {
    const ecs_component_info *ci = ecs_component_get(ed->e->world, comp);
    if (ci == NULL || field == 0xFFFFFFFFu)
        return "";
    u32 fc = 0;
    const de_field_info *fs = de_type_fields(ed->e->types, ci->type, &fc);
    return fs != NULL && field < fc ? fs[field].name : "";
}

static void on_snap_diff(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(snapshot_diff, de_ep_snap_diff_params, q);
    const ed_snap *sa = snap_find(q.a), *sb = snap_find(q.b);
    if (sa == NULL || sb == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "snapshot %u", sa == NULL ? q.a : q.b);
        return;
    }
    const u32 na = sa->field_count - 1, nb = sb->field_count - 1;
    de_ep_snap_diff d;
    memset(&d, 0, sizeof d);
    d.spawned.items = (u64 *)de__ed_alloc(a, sizeof(u64) * (nb + 1));
    d.despawned.items = (u64 *)de__ed_alloc(a, sizeof(u64) * (na + 1));
    d.changed.items = (de_ep_field_change *)de__ed_alloc(a, sizeof(de_ep_field_change) * (na + nb + 1));
    de_ep_kv_raw *kv = (de_ep_kv_raw *)de__ed_alloc(a, sizeof(de_ep_kv_raw) * (na + nb + 1));
    if (!d.spawned.items || !d.despawned.items || !d.changed.items || !kv) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "snapshot.diff: too large for scratch memory");
        return;
    }
    u32 nkv = 0;
    /* Walk both sorted lists. */
    u32 i = 0, j = 0;
    u64 last_sp = 0, last_de = 0;
    de_ep_field_change *cur = NULL;
    while (i < na || j < nb) {
        const snap_field *fa = i < na ? &sa->fields[i] : NULL, *fb = j < nb ? &sb->fields[j] : NULL;
        int c = fa == NULL ? 1 : fb == NULL ? -1 : cmp_field(fa, fb);
        const snap_field *key = c <= 0 ? fa : fb;
        /* Entity-level spawn/despawn. */
        if (c > 0 && !entity_in(sa, fb->entity)) {
            if (fb->entity != last_sp)
                d.spawned.items[d.spawned.count++] = last_sp = fb->entity;
            ++j;
            continue;
        }
        if (c < 0 && !entity_in(sb, fa->entity)) {
            if (fa->entity != last_de)
                d.despawned.items[d.despawned.count++] = last_de = fa->entity;
            ++i;
            continue;
        }
        bool differs = c != 0;
        if (c == 0)
            differs = fa->len != fb->len || memcmp(sa->bytes + fa->off, sb->bytes + fb->off, fa->len) != 0;
        if (differs) {
            const ecs_component_info *ci = ecs_component_get(ed->e->world, key->comp);
            if (cur == NULL || cur->entity != key->entity ||
                !de_ep_str_eq(cur->component, ci && ci->name ? ci->name : "")) {
                cur = &d.changed.items[d.changed.count++];
                memset(cur, 0, sizeof *cur);
                cur->entity = key->entity;
                cur->component = de_ep_cstr(ci && ci->name ? ci->name : "");
                cur->fields.items = &kv[nkv];
            }
            /* A component only on one side is one entry keyed "". */
            const bool whole = c != 0;
            if (whole && cur->fields.count > 0 && cur->fields.items[0].key.len == 0) {
                /* already recorded */
            } else {
                kv[nkv].key = de_ep_cstr(whole ? "" : field_name(ed, key->comp, key->field));
                if (c == 0)
                    kv[nkv].value = (de_ep_raw){(const char *)sb->bytes + fb->off, fb->len};
                else if (c > 0)
                    kv[nkv].value = de__ed_raw_bool(a, true);  /* added */
                else
                    kv[nkv].value = de__ed_raw_bool(a, false); /* removed */
                ++nkv;
                ++cur->fields.count;
            }
        }
        if (c <= 0)
            ++i;
        if (c >= 0)
            ++j;
    }
    de_ep_write_response_snapshot_diff(w, req->id, &d);
}

/* ---------- Per frame ---------- */

static void tick(de__editor *ed) {
    if (g_session.step_frames > 0) {
        if (--g_session.step_frames == 0) {
            game_systems(ed, false);
            send_state(ed);
        }
    }
}

static void reset(de__editor *ed) {
    (void)ed;
    for (u32 i = 0; i < ED_MAX_SNAPS; ++i)
        if (g_session.snaps[i].id != 0)
            snap_free(&g_session.snaps[i]);
}

void de__ed_session_register(de__editor *ed) {
    memset(&g_session, 0, sizeof g_session);
    ed->session = (struct de__ed_session *)&g_session;
    g_session.game_on = true;
    g_session.has_initial = take_world(ed, &g_session.initial);
    game_systems(ed, false); /* the session starts in Edit */
    de__ed_on(ed, DE_EP_METHOD_PING, on_ping);
    de__ed_on(ed, DE_EP_METHOD_GOODBYE, on_goodbye);
    de__ed_on(ed, DE_EP_METHOD_SESSION_INFO, on_info);
    de__ed_on(ed, DE_EP_METHOD_SESSION_SET_MODE, on_set_mode);
    de__ed_on(ed, DE_EP_METHOD_SESSION_STEP, on_step);
    de__ed_on(ed, DE_EP_METHOD_SESSION_RESTART, on_restart);
    de__ed_on(ed, DE_EP_METHOD_SNAPSHOT_TAKE, on_snap_take);
    de__ed_on(ed, DE_EP_METHOD_SNAPSHOT_RESTORE, on_snap_restore);
    de__ed_on(ed, DE_EP_METHOD_SNAPSHOT_RELEASE, on_snap_release);
    de__ed_on(ed, DE_EP_METHOD_SNAPSHOT_DIFF, on_snap_diff);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
}

/** Frees the session's snapshots (service stop). */
void de__ed_session_shutdown(void);
void de__ed_session_shutdown(void) {
    const de_allocator *al = de_heap_allocator();
    for (u32 i = 0; i < ED_MAX_SNAPS; ++i)
        if (g_session.snaps[i].id != 0)
            snap_free(&g_session.snaps[i]);
    if (g_session.has_play)
        de_world_snapshot_free(al, &g_session.play);
    if (g_session.has_initial)
        de_world_snapshot_free(al, &g_session.initial);
    memset(&g_session, 0, sizeof g_session);
}
