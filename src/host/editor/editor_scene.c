/* editor_scene.c: scenes and prefabs over IPC (DENG-628).
 *
 * Scene 1 is the game's own world: every entity no loaded scene owns. `scene.load` reads a TOML
 * scene file (the DENG-215 format: `[[entity]]` with `id`, `parent` and one table per component)
 * and spawns it as a new scene; the service keeps each entity's scene and stable scene ID, so
 * `scene.serialize` writes the same IDs back. Prefab instances remember their fields at
 * instantiation, which is what `prefab.overrides` compares against. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#endif
#include "host/editor/editor_internal.h"
#include "host/editor/editor_gizmo.h"

#include <dengine/de_asset.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/scene/scene_format.h"
#include "asset/hot_reload_internal.h"
#include "asset/slots_internal.h"
#include "core/de_toml_internal.h"
#include "ecs/ecs_internal.h"
#include "host/host_internal.h"
#include "reflect/ser_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_MAX_SCENES 32u
#define ED_GAME_SCENE 1u

typedef struct ed_scene {
    u64 id; /* 0: free */
    char path[512];
    bool active, dirty;
} ed_scene;

typedef struct ed_member {
    u64 entity, scene, sid;
} ed_member;

/* One prefab field as instantiated: what an override differs from. */
typedef struct ed_base {
    u64 entity;
    char path[128]; /* "Component.field", or "Component" (value nil): the entity has it */
    u32 off, len;
} ed_base;

typedef struct ed_instance {
    u64 root;
    char asset[256];
    ed_base *base;
    u32 base_count, base_cap;
    u8 *bytes;
    size_t byte_count;
    /* DENG-571: each entity's prefab-local scene id (the id in the prefab file). */
    u64 *local_ent, *local_sid;
    u32 local_count;
} ed_instance;

typedef struct de__ed_scene {
    ed_scene scenes[ED_MAX_SCENES];
    u64 next_scene;
    ed_member *members;
    u32 member_count, member_cap;
    ed_instance *instances;
    u32 instance_count, instance_cap;
} de__ed_scene;

static de__ed_scene g_scene;

/* ---------- Membership ---------- */

static ed_member *member_find(u64 entity) {
    for (u32 i = 0; i < g_scene.member_count; ++i)
        if (g_scene.members[i].entity == entity)
            return &g_scene.members[i];
    return NULL;
}

static bool member_add(u64 entity, u64 scene, u64 sid) {
    ed_member *m = member_find(entity);
    if (m != NULL) {
        m->scene = scene;
        m->sid = sid;
        return true;
    }
    if (g_scene.member_count == g_scene.member_cap) {
        const de_allocator *al = de_heap_allocator();
        const u32 cap = g_scene.member_cap ? g_scene.member_cap * 2 : 256;
        ed_member *p = (ed_member *)DE_ALLOC(al, sizeof(ed_member) * cap, 8, DE_MEM_GENERAL);
        if (p == NULL)
            return false;
        if (g_scene.members != NULL) {
            memcpy(p, g_scene.members, sizeof(ed_member) * g_scene.member_count);
            DE_FREE(al, g_scene.members, sizeof(ed_member) * g_scene.member_cap);
        }
        g_scene.members = p;
        g_scene.member_cap = cap;
    }
    g_scene.members[g_scene.member_count++] = (ed_member){entity, scene, sid};
    return true;
}

/* Drops members whose entity is gone. */
static void members_prune(de__editor *ed) {
    u32 k = 0;
    for (u32 i = 0; i < g_scene.member_count; ++i)
        if (de_entity_alive(ed->e->world, (de_entity){g_scene.members[i].entity}))
            g_scene.members[k++] = g_scene.members[i];
    g_scene.member_count = k;
}

bool de__ed_scene_member(u64 entity, u64 *scene, u64 *sid) {
    const ed_member *m = member_find(entity);
    if (m == NULL)
        return false;
    *scene = m->scene;
    *sid = m->sid;
    return true;
}

bool de__ed_scene_member_set(u64 entity, u64 scene, u64 sid) {
    return member_add(entity, scene, sid);
}

void de__ed_scene_member_drop(u64 entity) {
    ed_member *m = member_find(entity);
    if (m != NULL)
        *m = g_scene.members[--g_scene.member_count];
}

static ed_scene *scene_find(u64 id);

void de__ed_scene_adopt(de__editor *ed, de_entity e, u64 scene, de_entity parent) {
    if (scene == 0 && parent.bits != 0 && de_entity_alive(ed->e->world, parent))
        scene = de__ed_scene_of(ed, parent);
    for (u32 i = 0; i < ED_MAX_SCENES && scene == 0; ++i)
        if (g_scene.scenes[i].id != 0 && g_scene.scenes[i].active)
            scene = g_scene.scenes[i].id;
    if (scene_find(scene) == NULL)
        scene = ED_GAME_SCENE;
    (void)member_add(e.bits, scene, de_scene_id_new());
}

u64 de__ed_scene_of(de__editor *ed, de_entity e) {
    (void)ed;
    const ed_member *m = member_find(e.bits);
    return m != NULL ? m->scene : ED_GAME_SCENE;
}

static ed_scene *scene_find(u64 id) {
    for (u32 i = 0; i < ED_MAX_SCENES; ++i)
        if (g_scene.scenes[i].id == id && id != 0)
            return &g_scene.scenes[i];
    return NULL;
}

static bool in_scene(de__editor *ed, de_entity e, u64 scene) {
    return de__ed_scene_of(ed, e) == scene;
}

/* ---------- Reading scene text ---------- */

/* Why scene text was refused: scene.load and scene.reloadFromText reply `invalid_params` with
 * `data: { reason, line?, entity?, component? }` (DENG-570). `reason` is one of "syntax" (not
 * TOML), "shape" (`entity` or an entity is not a table), "id" (a key or `id` that is no scene
 * ID, or an `id` in a keyed table), "duplicate" (two entities with one scene ID), "parent" (a
 * parent that is no entity of the text, or a cycle) and "value" (a component value its type
 * can't hold). `line` is 1-based, `entity` the scene ID as "0x" and 16 hex digits. */
typedef struct scene_err {
    const char *reason;
    char msg[400];
    int line;
    u64 sid;
    char component[64];
} scene_err;

static bool scene_fail(scene_err *e, const char *reason, int line, u64 sid, const char *component,
                       const char *fmt, ...) DE_PRINTF_FMT(6, 7);
static bool scene_fail(scene_err *e, const char *reason, int line, u64 sid, const char *component,
                       const char *fmt, ...) {
    e->reason = reason;
    e->line = line;
    e->sid = sid;
    snprintf(e->component, sizeof e->component, "%s", component != NULL ? component : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    return false;
}

/* The `invalid_params` reply for `e`, its message prefixed with `prefix` (a path) when given. */
static void scene_error_reply(mpack_writer_t *w, u64 id, const char *prefix, const scene_err *e) {
    char msg[512];
    if (prefix != NULL)
        snprintf(msg, sizeof msg, "%s: %s", prefix, e->msg);
    else
        snprintf(msg, sizeof msg, "%s", e->msg);
    const u32 fields = 1u + (e->line > 0) + (e->sid != 0) + (e->component[0] != '\0');
    mpack_start_map(w, 2);
    mpack_write_cstr(w, "id");
    mpack_write_u64(w, id);
    mpack_write_cstr(w, "error");
    mpack_start_map(w, 3);
    mpack_write_cstr(w, "code");
    mpack_write_i32(w, (int32_t)DE_EP_ERR_INVALID_PARAMS);
    mpack_write_cstr(w, "message");
    mpack_write_cstr(w, msg);
    mpack_write_cstr(w, "data");
    mpack_start_map(w, fields);
    mpack_write_cstr(w, "reason");
    mpack_write_cstr(w, e->reason != NULL ? e->reason : "value");
    if (e->line > 0) {
        mpack_write_cstr(w, "line");
        mpack_write_i32(w, e->line);
    }
    if (e->sid != 0) {
        char hex[20];
        de__ser_format_hex64(e->sid, hex);
        mpack_write_cstr(w, "entity");
        mpack_write_cstr(w, hex);
    }
    if (e->component[0] != '\0') {
        mpack_write_cstr(w, "component");
        mpack_write_cstr(w, e->component);
    }
    mpack_finish_map(w);
    mpack_finish_map(w);
    mpack_finish_map(w);
}

/* A scene ID spelled as a key or a string: "0x" and 1 to 16 hex digits, or decimal digits. 0 if
 * it is neither (0 is no scene ID). */
static u64 sid_from_str(const char *s, size_t len) {
    u64 v = 0;
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return de__ser_parse_hex64(s, len, &v) ? v : 0;
    if (len == 0 || len > 20)
        return 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        const u64 d = (u64)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10)
            return 0;
        v = v * 10 + d;
    }
    return v;
}

static u64 parse_sid(const toml_datum_t *d) {
    if (d->type == TOML_INT64)
        return d->u.int64 > 0 ? (u64)d->u.int64 : 0;
    if (d->type == TOML_STRING)
        return sid_from_str(d->u.str.ptr, (size_t)d->u.str.len);
    return 0;
}

/* The 1-based line in a reflection reader message ("scene: line N: ..."), else `fallback`. */
static int line_of_message(const char *msg, int fallback) {
    int line = 0;
    if (msg != NULL && sscanf(msg, "scene: line %d:", &line) == 1 && line > 0)
        return line;
    return fallback;
}

static const toml_datum_t *tab_get(const toml_datum_t *t, const char *key) {
    if (t->type != TOML_TABLE)
        return NULL;
    for (int32_t i = 0; i < t->u.tab.size; ++i)
        if (strcmp(t->u.tab.key[i], key) == 0)
            return &t->u.tab.value[i];
    return NULL;
}

typedef struct load_map {
    u64 *sids;
    u64 *ents;
    u32 n;
    de__editor *ed;
} load_map;

static u64 map_sid_to_entity(u64 sid, void *user) {
    const load_map *m = (const load_map *)user;
    for (u32 i = 0; i < m->n; ++i)
        if (m->sids[i] == sid)
            return m->ents[i];
    return 0;
}

static u32 map_id_to_asset(u64 id, const char *type, void *user) {
    const load_map *m = (const load_map *)user;
    if (m->ed->e->assets == NULL || id == 0)
        return 0;
    const u32 h = de__asset_handle_of(m->ed->e->assets, (de_asset_id){id}, type);
    if (h == 0)
        de_error_clear();
    return h;
}

static bool is_hierarchy_name(const char *name) {
    return strcmp(name, "DeChildOf") == 0 || strcmp(name, "DeChildren") == 0 ||
           strcmp(name, "DeGlobalTransform") == 0;
}

/* DENG-810: a component a structural override can name. The hierarchy is the engine's own, and
 * the instance link is the editor's. */
static bool comp_name_ok(const char *name) {
    return name != NULL && name[0] != '\0' && strlen(name) < 120 && strchr(name, '.') == NULL &&
           !is_hierarchy_name(name) && strcmp(name, "PrefabInstance") != 0;
}

/* The MessagePack nil: the value of a presence marker, and of an absent component. */
static const char k_nil[1] = {'\xc0'};

static const char *datum_kind(toml_type_t t) {
    switch (t) {
    case TOML_STRING:
        return "a string";
    case TOML_INT64:
        return "an integer";
    case TOML_FP64:
        return "a float";
    case TOML_BOOLEAN:
        return "a boolean";
    case TOML_ARRAY:
        return "an array";
    case TOML_TABLE:
        return "a table";
    default:
        return "a date or time";
    }
}

/* One entity of scene text: its scene ID and its table. */
typedef struct text_ent {
    u64 sid;
    const toml_datum_t *tab;
    int line; /* of its table, 0 if not known */
} text_ent;

static int cmp_text_ent(const void *x, const void *y) {
    const text_ent *a = (const text_ent *)x, *b = (const text_ent *)y;
    return a->sid < b->sid ? -1 : a->sid > b->sid;
}

/* The entities of scene text into `out` (room for `cap`), in scene-ID order. Two layouts:
 * - keyed (what scene.serialize writes, SPEC §9.2): `[entity.0x…]`, one table per entity keyed
 *   by its scene ID; an `id` key in it is refused (the key is the ID);
 * - `[[entity]]` (DENG-215 authored scenes): an array of tables, each with an optional `id`
 *   (a new scene ID when absent). */
static bool text_entities(const toml_datum_t *top, text_ent *out, u32 cap, u32 *count,
                          scene_err *err) {
    *count = 0;
    const toml_datum_t *ents = tab_get(top, "entity");
    if (ents == NULL)
        return true;
    u32 n = 0;
    if (ents->type == TOML_TABLE) {
        for (int32_t k = 0; k < ents->u.tab.size && n < cap; ++k) {
            const char *key = ents->u.tab.key[k];
            const toml_datum_t *v = &ents->u.tab.value[k];
            const u64 sid = sid_from_str(key, strlen(key));
            if (sid == 0)
                return scene_fail(err, "id", v->lineno, 0, NULL,
                                  "entity key \"%.64s\" is no scene ID (\"0x\" and 1 to 16 hex "
                                  "digits, not 0)",
                                  key);
            if (v->type != TOML_TABLE)
                return scene_fail(err, "shape", v->lineno, sid, NULL,
                                  "entity 0x%016llx is %s, not a table", (unsigned long long)sid,
                                  datum_kind(v->type));
            const toml_datum_t *id = tab_get(v, "id");
            if (id != NULL)
                return scene_fail(err, "id", id->lineno, sid, NULL,
                                  "entity 0x%016llx: a keyed entity table has no `id` (its key "
                                  "is the scene ID)",
                                  (unsigned long long)sid);
            out[n++] = (text_ent){sid, v, v->lineno};
        }
    } else if (ents->type == TOML_ARRAY) {
        for (int32_t i = 0; i < ents->u.arr.size && n < cap; ++i) {
            const toml_datum_t *v = &ents->u.arr.elem[i];
            if (v->type != TOML_TABLE)
                return scene_fail(err, "shape", v->lineno, 0, NULL, "entity %d is %s, not a table",
                                  (int)i, datum_kind(v->type));
            const toml_datum_t *id = tab_get(v, "id");
            u64 sid = 0;
            if (id != NULL && (sid = parse_sid(id)) == 0)
                return scene_fail(err, "id", id->lineno, 0, NULL,
                                  "entity %d: `id` is no scene ID", (int)i);
            out[n++] = (text_ent){id != NULL ? sid : de_scene_id_new(), v, v->lineno};
        }
    } else {
        return scene_fail(err, "shape", ents->lineno, 0, NULL,
                          "`entity` is %s; scene text has `[entity.0x…]` tables",
                          datum_kind(ents->type));
    }
    qsort(out, n, sizeof(text_ent), cmp_text_ent);
    for (u32 i = 1; i < n; ++i)
        if (out[i].sid == out[i - 1].sid)
            return scene_fail(err, "duplicate", out[i].line, out[i].sid, NULL,
                              "scene ID 0x%016llx is on two entities",
                              (unsigned long long)out[i].sid);
    *count = n;
    return true;
}

static u32 text_entity_cap(const toml_datum_t *top) {
    const toml_datum_t *ents = tab_get(top, "entity");
    if (ents == NULL)
        return 0;
    if (ents->type == TOML_TABLE)
        return (u32)ents->u.tab.size;
    if (ents->type == TOML_ARRAY)
        return (u32)ents->u.arr.size;
    return 0;
}

static void link_instances(de__editor *ed, const toml_datum_t *top, const load_map *m,
                           const de_ep_arena *scratch);

/* Spawns the entities of scene text into `scene`, in scene-ID order, and restores the prefab
 * instance links of its `[[prefab.nested]]` entries (DENG-799). False with `err` set on a
 * failure; nothing stays spawned then. `a` is the caller's arena: its free part is scratch. */
static bool spawn_text(de__editor *ed, de_ep_arena *a, const char *text, size_t len, u64 scene,
                       scene_err *err) {
    de_world *w = ed->e->world;
    const de_allocator *al = de_heap_allocator();
    memset(err, 0, sizeof *err);
    de__ed_note_asset_paths(text, len);
    toml_result_t tr = de__toml_parse(al, text, (int)len);
    if (!tr.ok) {
        scene_fail(err, "syntax", tr.errline, 0, NULL, "%s", tr.errmsg);
        de__toml_free(tr);
        return false;
    }
    const u32 cap = text_entity_cap(&tr.toptab);
    text_ent *te = (text_ent *)DE_ALLOC(al, sizeof(text_ent) * (cap + 1), 8, DE_MEM_GENERAL);
    load_map m = {NULL, NULL, 0, ed};
    m.sids = (u64 *)DE_ALLOC(al, sizeof(u64) * (cap + 1), 8, DE_MEM_GENERAL);
    m.ents = (u64 *)DE_ALLOC(al, sizeof(u64) * (cap + 1), 8, DE_MEM_GENERAL);
    u32 n = 0;
    bool ok = te != NULL && m.sids != NULL && m.ents != NULL;
    if (!ok)
        scene_fail(err, "value", 0, 0, NULL, "out of memory");
    ok = ok && text_entities(&tr.toptab, te, cap, &n, err);
    /* 1: spawn every entity, so ENTITY fields can point at any of them. */
    for (u32 i = 0; ok && i < n; ++i) {
        m.sids[i] = te[i].sid;
        m.ents[i] = de_entity_spawn(w).bits;
        m.n = i + 1;
        member_add(m.ents[i], scene, m.sids[i]);
    }
    /* 2: components, read from their tables. */
    const de_ser_context ctx = {NULL, map_sid_to_entity, NULL, map_id_to_asset, &m};
    const de_ser_options opt = {DE_SER_TEXT, 0, &ctx};
    for (u32 i = 0; ok && i < n; ++i) {
        const toml_datum_t *ent = te[i].tab;
        for (int32_t k = 0; ok && k < ent->u.tab.size; ++k) {
            const char *key = ent->u.tab.key[k];
            const toml_datum_t *v = &ent->u.tab.value[k];
            if (v->type != TOML_TABLE || is_hierarchy_name(key))
                continue;
            const de_component c = de__ed_component(ed, de_ep_cstr(key));
            de_type_id type;
            u32 size = 0;
            if (c.id == 0 || !de__ed_component_type(ed, c, &type, &size)) {
                DE_WARN(ed->log, "scene: unknown component %s skipped", key);
                continue;
            }
            void *data = de_entity_add(w, (de_entity){m.ents[i]}, c);
            if (size == 0 || data == NULL)
                continue;
            if (de__ser_text_read_table(ed->e->types, type, data, &opt, v, al, "scene") != DE_OK) {
                const de_error *e = de_error_last();
                const char *why = e != NULL ? e->message : "bad value";
                ok = scene_fail(err, "value", line_of_message(why, v->lineno), te[i].sid, key,
                                "entity 0x%016llx, %s: %s", (unsigned long long)te[i].sid, key,
                                why);
                de_error_clear();
            }
        }
    }
    /* 3: the hierarchy. A parent must be an entity of the same text. */
    for (u32 i = 0; ok && i < n; ++i) {
        const toml_datum_t *p = tab_get(te[i].tab, "parent");
        if (p == NULL)
            continue;
        const u64 psid = parse_sid(p);
        const u64 pe = psid != 0 ? map_sid_to_entity(psid, &m) : 0;
        if (pe == 0) {
            ok = scene_fail(err, "parent", p->lineno, te[i].sid, NULL,
                            "entity 0x%016llx: parent is no entity of this scene",
                            (unsigned long long)te[i].sid);
            break;
        }
        if (de_entity_set_parent(w, (de_entity){m.ents[i]}, (de_entity){pe}) != DE_OK) {
            de_error_clear();
            ok = scene_fail(err, "parent", p->lineno, te[i].sid, NULL,
                            "entity 0x%016llx: parent 0x%016llx makes a cycle",
                            (unsigned long long)te[i].sid, (unsigned long long)psid);
        }
    }
    if (ok) {
        /* Scratch past what the caller keeps, 16-byte aligned like the arena's own. */
        const size_t at = (a->used + 15u) & ~(size_t)15u;
        const de_ep_arena scratch = {a->base + at, at < a->size ? a->size - at : 0, 0};
        link_instances(ed, &tr.toptab, &m, &scratch);
        /* DENG-824: a version 1 layer kept its instances inline; they become pages. */
        for (u32 i = 0; ed->foliage != NULL && i < n; ++i) {
            const toml_datum_t *fo = tab_get(te[i].tab, "DeFoliage");
            if (fo == NULL || fo->type != TOML_TABLE ||
                (tab_get(fo, "cells") == NULL && tab_get(fo, "instances") == NULL))
                continue;
            if (!ed->foliage->import_v1(ed, (de_entity){m.ents[i]}, fo))
                DE_WARN(ed->log, "scene: entity 0x%016llx: version 1 foliage not imported",
                        (unsigned long long)te[i].sid);
        }
        if (ed->foliage != NULL)
            ed->foliage->loaded(ed, m.ents, m.n, DE_ENTITY_NULL);
    }
    if (!ok)
        for (u32 i = 0; i < m.n; ++i)
            de_entity_despawn(w, (de_entity){m.ents[i]});
    if (te)
        DE_FREE(al, te, sizeof(text_ent) * (cap + 1));
    if (m.sids)
        DE_FREE(al, m.sids, sizeof(u64) * (cap + 1));
    if (m.ents)
        DE_FREE(al, m.ents, sizeof(u64) * (cap + 1));
    de__toml_free(tr);
    if (!ok)
        members_prune(ed);
    de__ed_world_touch(ed);
    return ok;
}

/* ---------- Writing scene text ---------- */

/* The asset IDs written so far and their paths: a saved scene names assets by path, as authored
 * scenes do (`texture = "flappy/tex/bird"`), not by ID. */
#define SAVE_PATHS 512u

typedef struct save_map {
    de__editor *ed;
    u64 ids[SAVE_PATHS];
    const char *paths[SAVE_PATHS];
    u32 count;
} save_map;

static const char *saved_path(const save_map *m, u64 id) {
    for (u32 i = 0; i < m->count; ++i)
        if (m->ids[i] == id)
            return m->paths[i];
    return NULL;
}

static u64 save_entity_to_sid(u64 bits, void *user) {
    (void)user;
    const ed_member *m = member_find(bits);
    return m != NULL ? m->sid : bits;
}

static u64 save_asset_to_id(u32 handle, const char *type, void *user) {
    (void)type;
    save_map *m = (save_map *)user;
    const char *path = de__ed_asset_path(m->ed, handle);
    if (path == NULL)
        return (u64)handle;
    const u64 id = de_asset_id_from(path).hash;
    if (saved_path(m, id) == NULL && m->count < SAVE_PATHS) {
        m->ids[m->count] = id;
        m->paths[m->count++] = path;
    }
    return id;
}

static const char k_asset_open[] = "{ id = \"0x";

/* True for a line whose value is only empty asset tables (`normal_tex = { id = "0x0...0" }`,
 * `materials = [{ id = "0x0...0" }, ...]`): what the defaults already are. */
static bool only_empty_assets(const char *line, size_t n) {
    static const char zero[] = "{ id = \"0x0000000000000000\" }";
    const size_t zn = sizeof zero - 1;
    const char *eq = memchr(line, '=', n);
    if (eq == NULL)
        return false;
    size_t i = (size_t)(eq - line) + 1, tables = 0;
    while (i < n) {
        if (n - i >= zn && memcmp(line + i, zero, zn) == 0) {
            ++tables;
            i += zn;
        } else if (line[i] == ' ' || line[i] == ',' || line[i] == '[' || line[i] == ']') {
            ++i;
        } else {
            return false;
        }
    }
    return tables > 0;
}

/* One line of a component's text with its asset tables (`{ id = "0x..." }`, `{ id = "0x...", path =
 * "..." }`) as path strings where the path is known; false for a line that only sets asset fields
 * to none, which the defaults already are. */
static bool asset_line(const save_map *m, const char *line, size_t n, de_mem_writer *out) {
    if (only_empty_assets(line, n))
        return false;
    const char *open = k_asset_open;
    const size_t on = sizeof k_asset_open - 1;
    de_writer w = de_writer_from_mem(out);
    size_t at = 0, copied = 0;
    while (at + on + 17 <= n) {
        if (memcmp(line + at, open, on) != 0) {
            ++at;
            continue;
        }
        const char *hex = line + at + on;
        char buf[17];
        memcpy(buf, hex, 16);
        buf[16] = '\0';
        char *end = NULL;
        const u64 id = strtoull(buf, &end, 16);
        const char *close = memchr(hex, '}', n - (size_t)(hex - line));
        if (end != buf + 16 || close == NULL) {
            ++at;
            continue;
        }
        const size_t after = (size_t)(close - line) + 1;
        const char *path = id != 0 ? saved_path(m, id) : NULL;
        if (path != NULL) {
            (void)w.write(w.user, line + copied, at - copied);
            (void)w.write(w.user, "\"", 1);
            (void)w.write(w.user, path, strlen(path));
            (void)w.write(w.user, "\"", 1);
            copied = after;
        }
        at = after;
    }
    (void)w.write(w.user, line + copied, n - copied);
    return true;
}

/* Writes one component's fields as TOML lines, without the serializer's header keys. */
static void put_component(de__editor *ed, de_mem_writer *out, de_type_id type, const void *data,
                          const de_ser_options *opt) {
    de_mem_writer tmp;
    de_mem_writer_init(&tmp, NULL);
    de_writer tw = de_writer_from_mem(&tmp);
    de__ser_out o;
    de__ser_out_init(&o, &tw);
    (void)de__ser_text_write_body(&o, ed->e->types, type, data, opt);
    (void)de__ser_out_flush(&o);
    de_error_clear();
    de_writer ow = de_writer_from_mem(out);
    size_t at = 0;
    while (at < tmp.size) {
        size_t end = at;
        while (end < tmp.size && tmp.data[end] != '\n')
            ++end;
        const size_t n = end - at;
        const save_map *sm = opt->ctx != NULL ? (const save_map *)opt->ctx->user : NULL;
        if (!(n >= 2 && tmp.data[at] == '_' && tmp.data[at + 1] == '_')) {
            if (sm == NULL)
                (void)ow.write(ow.user, tmp.data + at, end < tmp.size ? n + 1 : n);
            else if (asset_line(sm, (const char *)tmp.data + at, n, out) && end < tmp.size)
                (void)ow.write(ow.user, "\n", 1);
        }
        at = end + 1;
    }
    de_mem_writer_free(&tmp);
}

static void putf(de_mem_writer *out, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void putf(de_mem_writer *out, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        de_writer w = de_writer_from_mem(out);
        (void)w.write(w.user, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    }
}

typedef struct save_ent {
    u64 entity, sid;
} save_ent;

static int cmp_save(const void *x, const void *y) {
    const save_ent *a = (const save_ent *)x, *b = (const save_ent *)y;
    return a->sid < b->sid ? -1 : a->sid > b->sid;
}

static void write_links(de__editor *ed, const save_ent *list, u32 n, de_mem_writer *out);

static int cmp_cstr(const void *x, const void *y) {
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

/* The entities of `scene` (or `only`, when given) as scene text into `out` (DENG-570,
 * DIDE-R147): entities in scene-ID order, each `[entity.0x<16 hex>]` with its `parent`, then its
 * serialized components as `[entity.0x<16 hex>.<Type>]` in type-name order, fields in reflection
 * order, a blank line between entities. A whole scene then gets one `[[prefab.nested]]` entry per
 * prefab instance whose root it saves (DENG-799). The same world always gives the same bytes, and
 * moving one entity changes only that entity's transform lines. */
static void write_scene(de__editor *ed, u64 scene, const u64 *only, u32 only_count,
                        de_mem_writer *out) {
    de_world *w = ed->e->world;
    const de_allocator *al = de_heap_allocator();
    u32 total = 0;
    for (u32 ai = 0; ai < w->archetypes.count; ++ai)
        total += (((ecs_archetype *const *)w->archetypes.data)[ai])->count;
    save_ent *list = (save_ent *)DE_ALLOC(al, sizeof(save_ent) * (total + 1), 8, DE_MEM_GENERAL);
    if (list == NULL)
        return;
    u32 n = 0;
    for (u32 ai = 0; ai < w->archetypes.count; ++ai) {
        const ecs_archetype *a = ((ecs_archetype *const *)w->archetypes.data)[ai];
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            for (u32 r = 0; r < ch->count; ++r) {
                const de_entity e = ecs_chunk_entities(ch)[r];
                bool want = false;
                if (only != NULL) {
                    for (u32 k = 0; k < only_count && !want; ++k)
                        want = only[k] == e.bits;
                } else {
                    want = in_scene(ed, e, scene) && !de__ed_views_owns(e.bits);
                }
                /* DENG-824: the pages of a layer that is gone are not saved. */
                if (want && only == NULL && ed->foliage != NULL) {
                    const u64 layer = ed->foliage->page_layer(ed, e.bits);
                    want = layer == 0 || de_entity_alive(w, (de_entity){layer});
                }
                if (!want)
                    continue;
                ed_member *m = member_find(e.bits);
                if (m == NULL) {
                    member_add(e.bits, ED_GAME_SCENE, de_scene_id_new());
                    m = member_find(e.bits);
                }
                list[n++] = (save_ent){e.bits, m != NULL ? m->sid : e.bits};
            }
        }
    }
    qsort(list, n, sizeof(save_ent), cmp_save);
    save_map sm;
    memset(&sm, 0, sizeof sm);
    sm.ed = ed;
    const de_ser_context ctx = {save_entity_to_sid, NULL, save_asset_to_id, NULL, &sm};
    const de_ser_options opt = {DE_SER_TEXT, DE_FIELD_SERIALIZE, &ctx};
    for (u32 i = 0; i < n; ++i) {
        const de_entity e = {list[i].entity};
        if (i > 0)
            putf(out, "\n");
        /* SPEC §9.2: one table per entity keyed by its scene ID, then its components. */
        putf(out, "[entity.0x%016llx]\n", (unsigned long long)list[i].sid);
        const de_entity p = de_entity_parent(w, e);
        if (p.bits != 0) {
            /* Only a parent that is saved too: the text must reload on its own. */
            const ed_member *pm = member_find(p.bits);
            bool saved = false;
            for (u32 k = 0; pm != NULL && k < n && !saved; ++k)
                saved = list[k].entity == p.bits;
            if (saved)
                putf(out, "parent = \"0x%016llx\"\n", (unsigned long long)pm->sid);
        }
        const ecs_record *rec = ecs_entity_record(w, e);
        const ecs_archetype *a = ((ecs_archetype *const *)w->archetypes.data)[rec->archetype];
        const ecs_chunk *ch = ecs_table_chunk(a, rec->chunk);
        const char *names[256];
        u32 idx[256], nn = 0;
        for (u32 k = 0; k < a->component_count && nn < 256; ++k) {
            const ecs_component_info *ci = ecs_component_get(w, a->components[k]);
            if (ci == NULL || ci->name == NULL || is_hierarchy_name(ci->name) ||
                !(ci->flags & DE_COMP_SERIALIZE))
                continue;
            names[nn] = ci->name;
            idx[nn++] = k;
        }
        /* Sorted by type name, as the cooker sorts them. */
        for (u32 x = 1; x < nn; ++x)
            for (u32 y = x; y > 0 && strcmp(names[y - 1], names[y]) > 0; --y) {
                const char *t = names[y];
                names[y] = names[y - 1];
                names[y - 1] = t;
                const u32 ti = idx[y];
                idx[y] = idx[y - 1];
                idx[y - 1] = ti;
            }
        (void)cmp_cstr;
        for (u32 k = 0; k < nn; ++k) {
            const ecs_component_info *ci = ecs_component_get(w, a->components[idx[k]]);
            putf(out, "[entity.0x%016llx.%s]\n", (unsigned long long)list[i].sid, ci->name);
            const u32 col = a->column_of[idx[k]];
            if (col != ECS_INVALID_INDEX && ci->size != 0)
                put_component(
                    ed, out, ci->type,
                    (const u8 *)ecs_chunk_column(a, ch, col) + (size_t)rec->row * ci->size, &opt);
        }
    }
    if (only == NULL)
        write_links(ed, list, n, out);
    DE_FREE(al, list, sizeof(save_ent) * (total + 1));
}

/* ---------- Scene methods ---------- */

static bool no_change_in_play(de__editor *ed, const de_ep_request *req, mpack_writer_t *w) {
    if (ed->mode == DE_EP_WORLD_MODE_EDIT)
        return true;
    de__ed_error(w, req->id, DE_EP_ERR_WRONG_MODE, "scenes can't change during a play session");
    return false;
}

static void despawn_scene(de__editor *ed, u64 scene) {
    members_prune(ed);
    for (u32 i = 0; i < g_scene.member_count; ++i)
        if (g_scene.members[i].scene == scene) {
            de__ed_prefab_instance_drop(g_scene.members[i].entity);
            de_entity_despawn(ed->e->world, (de_entity){g_scene.members[i].entity});
        }
    members_prune(ed);
    de__ed_world_touch(ed);
}

static bool read_file(const char *path, char **text, size_t *len) {
    const de_allocator *al = de_heap_allocator();
    if (strstr(path, ":/") != NULL && path[1] != ':') {
        void *data = NULL;
        u64 size = 0;
        if (de_file_read_all(path, al, &data, &size) != DE_OK) {
            de_error_clear();
            return false;
        }
        *text = (char *)data;
        *len = (size_t)size;
        return true;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? (char *)DE_ALLOC(al, (size_t)n + 1, 8, DE_MEM_GENERAL) : NULL;
    const bool ok = buf != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        if (buf)
            DE_FREE(al, buf, (size_t)n + 1);
        return false;
    }
    buf[n] = '\0';
    *text = buf;
    *len = (size_t)n;
    return true;
}

static void on_list(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    de_ep_scene_list list;
    memset(&list, 0, sizeof list);
    list.scenes.items =
        (de_ep_scene_info *)de__ed_alloc(a, sizeof(de_ep_scene_info) * ED_MAX_SCENES);
    for (u32 i = 0; i < ED_MAX_SCENES && list.scenes.items != NULL; ++i) {
        const ed_scene *s = &g_scene.scenes[i];
        if (s->id == 0)
            continue;
        de_ep_scene_info *o = &list.scenes.items[list.scenes.count++];
        o->scene = s->id;
        o->path = de_ep_cstr(s->path);
        o->active = s->active;
        o->dirty = s->dirty;
    }
    de_ep_write_response_scene_list(w, req->id, &list);
}

static void on_load(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(scene_load, de_ep_scene_load_params, q);
    if (!no_change_in_play(ed, req, w))
        return;
    char *path = de__ed_cstr(a, q.path);
    ed_scene *slot = NULL;
    for (u32 i = 0; i < ED_MAX_SCENES && slot == NULL; ++i)
        if (g_scene.scenes[i].id == 0)
            slot = &g_scene.scenes[i];
    if (path == NULL || slot == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many scenes");
        return;
    }
    if (!q.additive)
        for (u32 i = 0; i < ED_MAX_SCENES; ++i)
            if (g_scene.scenes[i].id != 0 && g_scene.scenes[i].id != ED_GAME_SCENE) {
                despawn_scene(ed, g_scene.scenes[i].id);
                memset(&g_scene.scenes[i], 0, sizeof g_scene.scenes[i]);
            }
    const u64 id = ++g_scene.next_scene;
    char *text = NULL;
    size_t len = 0;
    scene_err err;
    /* A missing file is a new, empty scene (the IDE creates it on save). */
    if (read_file(path, &text, &len)) {
        const bool ok = spawn_text(ed, a, text, len, id, &err);
        DE_FREE(de_heap_allocator(), text, len + 1);
        if (!ok) {
            scene_error_reply(w, req->id, path, &err);
            return;
        }
    }
    memset(slot, 0, sizeof *slot);
    slot->id = id;
    snprintf(slot->path, sizeof slot->path, "%s", path);
    for (u32 i = 0; i < ED_MAX_SCENES; ++i)
        g_scene.scenes[i].active = false;
    slot->active = true;
    de_ep_scene_params r = {id};
    de_ep_write_response_scene_load(w, req->id, &r);
}

static void on_unload(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(scene_unload, de_ep_scene_params, q);
    if (!no_change_in_play(ed, req, w))
        return;
    ed_scene *s = scene_find(q.scene);
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "scene %llu", (unsigned long long)q.scene);
        return;
    }
    despawn_scene(ed, q.scene);
    if (q.scene != ED_GAME_SCENE)
        memset(s, 0, sizeof *s);
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_scene_unload(w, req->id, &e);
}

static void on_serialize(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(scene_serialize, de_ep_scene_params, q);
    if (scene_find(q.scene) == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "scene %llu", (unsigned long long)q.scene);
        return;
    }
    members_prune(ed);
    de_mem_writer out;
    de_mem_writer_init(&out, NULL);
    write_scene(ed, q.scene, NULL, 0, &out);
    de_ep_scene_text t;
    t.text.ptr = out.data != NULL ? (const char *)out.data : "";
    t.text.len = (u32)out.size;
    /* The IDE writes the file, so the scene stays dirty until it says it saved. */
    de_ep_write_response_scene_serialize(w, req->id, &t);
    de_mem_writer_free(&out);
}

static void on_set_active(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(scene_set_active, de_ep_scene_params, q);
    ed_scene *s = scene_find(q.scene);
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "scene %llu", (unsigned long long)q.scene);
        return;
    }
    for (u32 i = 0; i < ED_MAX_SCENES; ++i)
        g_scene.scenes[i].active = false;
    s->active = true;
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_scene_set_active(w, req->id, &e);
}

/* The entities scene.serialize writes for `scene`: its members, and for the game scene the
 * entities in no scene, never the editor's views. NULL (count 0) if there are none or memory ran
 * out; free with DE_FREE(heap, list, sizeof(u64) * (count + 1)). */
static u64 *scene_entities(de__editor *ed, u64 scene, u32 *count) {
    de_world *w = ed->e->world;
    const de_allocator *al = de_heap_allocator();
    u32 total = 0;
    for (u32 ai = 0; ai < w->archetypes.count; ++ai)
        total += (((ecs_archetype *const *)w->archetypes.data)[ai])->count;
    *count = 0;
    u64 *list = (u64 *)DE_ALLOC(al, sizeof(u64) * (total + 1), 8, DE_MEM_GENERAL);
    if (list == NULL)
        return NULL;
    u32 n = 0;
    for (u32 ai = 0; ai < w->archetypes.count; ++ai) {
        const ecs_archetype *a = ((ecs_archetype *const *)w->archetypes.data)[ai];
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            for (u32 r = 0; r < ch->count; ++r) {
                const de_entity e = ecs_chunk_entities(ch)[r];
                if (n < total && in_scene(ed, e, scene) && !de__ed_views_owns(e.bits))
                    list[n++] = e.bits;
            }
        }
    }
    *count = n;
    return list;
}

#define ED_RELOAD_SELECTION 256u

/* DIDE-R147, DIDE-R148: replaces the scene with the text. All or nothing: the new entities are
 * spawned first and the old ones go only when the whole text read, so bad text leaves the scene
 * as it was. The selection is kept by scene ID. */
static void on_reload(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(scene_reload_from_text, de_ep_scene_reload_params, q);
    if (!no_change_in_play(ed, req, w))
        return;
    ed_scene *s = scene_find(q.scene);
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "scene %llu", (unsigned long long)q.scene);
        return;
    }
    members_prune(ed);
    const de_allocator *al = de_heap_allocator();
    u32 old_count = 0;
    u64 *old = scene_entities(ed, q.scene, &old_count);
    /* The selection by scene ID (0: not an entity of this scene, kept as it is). */
    u64 sel[ED_RELOAD_SELECTION], sel_sid[ED_RELOAD_SELECTION];
    u32 sel_count = de__ed_gizmo_selection(sel, ED_RELOAD_SELECTION);
    if (sel_count > ED_RELOAD_SELECTION)
        sel_count = ED_RELOAD_SELECTION;
    for (u32 i = 0; i < sel_count; ++i) {
        const ed_member *m = member_find(sel[i]);
        sel_sid[i] = m != NULL && m->scene == q.scene ? m->sid : 0;
    }
    /* Old and new members share scene IDs until the old entities go: members are found by
     * entity, and the text's references resolve among its own entities. */
    scene_err err;
    if (!spawn_text(ed, a, q.text.ptr, q.text.len, q.scene, &err)) {
        if (old != NULL)
            DE_FREE(al, old, sizeof(u64) * (old_count + 1));
        scene_error_reply(w, req->id, NULL, &err);
        return;
    }
    for (u32 i = 0; i < old_count; ++i) {
        de__ed_prefab_instance_drop(old[i]);
        de_entity_despawn(ed->e->world, (de_entity){old[i]});
    }
    if (old != NULL)
        DE_FREE(al, old, sizeof(u64) * (old_count + 1));
    members_prune(ed);
    de__ed_world_touch(ed);
    /* The selection follows its scene IDs to the new entities; one whose ID is gone drops. */
    u32 kept = 0;
    for (u32 i = 0; i < sel_count; ++i) {
        if (sel_sid[i] == 0) {
            sel[kept++] = sel[i];
            continue;
        }
        for (u32 k = 0; k < g_scene.member_count; ++k)
            if (g_scene.members[k].scene == q.scene && g_scene.members[k].sid == sel_sid[i]) {
                sel[kept++] = g_scene.members[k].entity;
                break;
            }
    }
    if (sel_count != 0)
        de__ed_gizmo_set_selection(sel, kept);
    s->dirty = true;
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_scene_reload_from_text(w, req->id, &e);
    de_ep_scene_params p = {q.scene};
    mpack_writer_t nw;
    const u64 seq = de__ed_note_begin(ed, &nw);
    de_ep_write_notify_scene_dirty(&nw, seq, &p);
    de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
}

/* ---------- Prefabs ---------- */

/* Every entity of the tree under `root`, root first. */
static u32 tree_of(de__editor *ed, de_entity root, u64 *out, u32 cap) {
    u32 n = 0;
    if (cap == 0)
        return 0;
    out[n++] = root.bits;
    for (u32 i = 0; i < n; ++i)
        for (de_entity c = de_entity_children_first(ed->e->world, (de_entity){out[i]});
             c.bits != 0 && n < cap; c = de_entity_children_next(ed->e->world, c))
            out[n++] = c.bits;
    return n;
}

u32 de__ed_scene_tree(de__editor *ed, de_entity root, u64 *out, u32 cap) {
    return tree_of(ed, root, out, cap);
}

/* One more entry of a growing base list; NULL when out of memory. */
static ed_base *base_vec_push(ed_base **base, u32 *cap, u32 *count) {
    const de_allocator *al = de_heap_allocator();
    if (*count == *cap) {
        const u32 ncap = *cap ? *cap * 2 : 64;
        ed_base *g = (ed_base *)DE_ALLOC(al, sizeof(ed_base) * ncap, 8, DE_MEM_GENERAL);
        if (g == NULL)
            return NULL;
        if (*base != NULL) {
            memcpy(g, *base, sizeof(ed_base) * *count);
            DE_FREE(al, *base, sizeof(ed_base) * *cap);
        }
        *base = g;
        *cap = ncap;
    }
    ed_base *b = &(*base)[(*count)++];
    memset(b, 0, sizeof *b);
    return b;
}

/* "Component.field" values of every entity of the tree, as MessagePack, into `inst`, and a
 * "Component" marker (nil) for each component an override can name (DENG-810). `scratch` is the
 * free part of the caller's arena: what the caller keeps there stays. */
static void instance_base(de__editor *ed, ed_instance *inst, const de_ep_arena *scratch) {
    const de_allocator *al = de_heap_allocator();
    u64 ents[1024];
    const u32 n = tree_of(ed, (de_entity){inst->root}, ents, 1024);
    de_mem_writer bytes;
    de_mem_writer_init(&bytes, NULL);
    u32 cap = 64, count = 0;
    ed_base *base = (ed_base *)DE_ALLOC(al, sizeof(ed_base) * cap, 8, DE_MEM_GENERAL);
    for (u32 i = 0; i < n && base != NULL; ++i) {
        de_ep_arena a = *scratch;
        de_ep_entity_row row;
        if (!de__ed_entity_row(ed, &a, (de_entity){ents[i]}, NULL, NULL, &row))
            continue;
        for (u32 c = 0; c < row.components.count && base != NULL; ++c) {
            char name[128];
            snprintf(name, sizeof name, "%.*s", (int)row.components.items[c].key.len,
                     row.components.items[c].key.ptr);
            if (comp_name_ok(name)) {
                ed_base *m = base_vec_push(&base, &cap, &count);
                if (m == NULL)
                    break;
                m->entity = ents[i];
                snprintf(m->path, sizeof m->path, "%s", name);
                m->off = (u32)bytes.size;
                m->len = 1;
                de_writer mw = de_writer_from_mem(&bytes);
                if (mw.write(mw.user, k_nil, 1) != DE_OK) {
                    --count;
                    break;
                }
            }
            mpack_reader_t reader;
            mpack_reader_init_data(&reader, row.components.items[c].value.ptr,
                                   row.components.items[c].value.len);
            const u32 fields = mpack_expect_map(&reader);
            if (mpack_reader_error(&reader) == mpack_ok) {
                for (u32 f = 0; f < fields; ++f) {
                    de_ep_kv_raw field;
                    if (!de__ed_raw_map_entry(&reader, &field))
                        break;
                    ed_base *b = base_vec_push(&base, &cap, &count);
                    if (b == NULL) {
                        mpack_reader_flag_error(&reader, mpack_error_memory);
                        break;
                    }
                    b->entity = ents[i];
                    snprintf(b->path, sizeof b->path, "%.*s.%.*s",
                             (int)row.components.items[c].key.len, row.components.items[c].key.ptr,
                             (int)field.key.len, field.key.ptr);
                    b->off = (u32)bytes.size;
                    b->len = (u32)field.value.len;
                    de_writer bw = de_writer_from_mem(&bytes);
                    if (bw.write(bw.user, field.value.ptr, b->len) != DE_OK) {
                        --count;
                        mpack_reader_flag_error(&reader, mpack_error_memory);
                        break;
                    }
                }
            }
            mpack_done_map(&reader);
            (void)mpack_reader_destroy(&reader);
        }
    }
    inst->base = base;
    inst->base_count = count;
    inst->base_cap = base != NULL ? cap : 0;
    inst->bytes = bytes.data;
    inst->byte_count = bytes.capacity;
    /* The writer's buffer now belongs to the instance. */
}

static ed_instance *instance_of(de__editor *ed, u64 entity) {
    for (u32 i = 0; i < g_scene.instance_count; ++i) {
        ed_instance *inst = &g_scene.instances[i];
        u64 ents[1024];
        const u32 n = tree_of(ed, (de_entity){inst->root}, ents, 1024);
        for (u32 k = 0; k < n; ++k)
            if (ents[k] == entity)
                return inst;
    }
    return NULL;
}

static ed_instance *instance_push(void) {
    if (g_scene.instance_count == g_scene.instance_cap) {
        const de_allocator *al = de_heap_allocator();
        const u32 cap = g_scene.instance_cap ? g_scene.instance_cap * 2 : 16;
        ed_instance *grown =
            (ed_instance *)DE_ALLOC(al, sizeof(ed_instance) * cap, 8, DE_MEM_GENERAL);
        if (grown == NULL)
            return NULL;
        if (g_scene.instances != NULL) {
            memcpy(grown, g_scene.instances, sizeof(ed_instance) * g_scene.instance_count);
            DE_FREE(al, g_scene.instances, sizeof(ed_instance) * g_scene.instance_cap);
        }
        g_scene.instances = grown;
        g_scene.instance_cap = cap;
    }
    ed_instance *inst = &g_scene.instances[g_scene.instance_count++];
    memset(inst, 0, sizeof *inst);
    return inst;
}

static ed_instance *instance_rooted(u64 root) {
    for (u32 i = 0; i < g_scene.instance_count; ++i)
        if (g_scene.instances[i].root == root)
            return &g_scene.instances[i];
    return NULL;
}

/* ---------- Prefab definitions (DENG-571) ----------
 *
 * Each loaded prefab asset keeps its "Component.field" values in prefab-local space: entity
 * fields hold the prefab's own scene ids. A `[prefab]` table in the prefab's source links it to
 * the level below: `base = "<asset>"` (a variant, same ids) and `[[prefab.nested]]` entries
 * (`asset`, `root`, `ids = [[inner, outer], ...]`) for prefabs flattened into it. */

#define ED_LOCAL_CAP 1024u
#define ED_CHAIN_MAX 16u

typedef struct ed_dval {
    u64 sid;
    char path[128];
    u32 off, len;
} ed_dval;

typedef struct ed_nest {
    char asset[256];
    u64 root;
    u64 *inner, *outer; /* one block of 2 * count */
    u32 count;
} ed_nest;

typedef struct ed_def {
    char asset[256];
    char base[256];
    ed_dval *vals;
    u32 val_count, val_cap;
    u8 *bytes;
    size_t used, cap;
    ed_nest *nest;
    u32 nest_count;
    bool ready; /* its asset loaded (DENG-807): the values are the cooked prefab's */
    /* The hash of the source bytes these values stand for, when the editor wrote them itself or
     * read them in a reload. A reload event whose source still has this hash is the echo of an
     * own write: the cooked asset it reports can be older than the values (DENG-813). */
    bool src_known;
    u64 src_hash;
} ed_def;

static struct {
    ed_def **defs;
    u32 count, cap;
    u64 revision;
} g_prefab;

static void instance_local_free(ed_instance *inst) {
    if (inst->local_sid != NULL)
        DE_FREE(de_heap_allocator(), inst->local_sid, sizeof(u64) * ED_LOCAL_CAP * 2);
    inst->local_sid = inst->local_ent = NULL;
    inst->local_count = 0;
}

static bool local_sid_of(const ed_instance *inst, u64 entity, u64 *sid) {
    for (u32 i = 0; i < inst->local_count; ++i)
        if (inst->local_ent[i] == entity) {
            *sid = inst->local_sid[i];
            return true;
        }
    return false;
}

static bool local_ent_of(const ed_instance *inst, u64 sid, u64 *entity) {
    for (u32 i = 0; i < inst->local_count; ++i)
        if (inst->local_sid[i] == sid) {
            *entity = inst->local_ent[i];
            return true;
        }
    return false;
}

static void def_free_inner(ed_def *d) {
    const de_allocator *al = de_heap_allocator();
    if (d->vals != NULL)
        DE_FREE(al, d->vals, sizeof(ed_dval) * d->val_cap);
    if (d->bytes != NULL)
        DE_FREE(al, d->bytes, d->cap);
    for (u32 k = 0; k < d->nest_count; ++k)
        if (d->nest[k].inner != NULL)
            DE_FREE(al, d->nest[k].inner, sizeof(u64) * 2 * d->nest[k].count);
    if (d->nest != NULL)
        DE_FREE(al, d->nest, sizeof(ed_nest) * d->nest_count);
}

static void prefab_defs_free(void) {
    const de_allocator *al = de_heap_allocator();
    for (u32 i = 0; i < g_prefab.count; ++i) {
        ed_def *d = g_prefab.defs[i];
        def_free_inner(d);
        DE_FREE(al, d, sizeof *d);
    }
    if (g_prefab.defs != NULL)
        DE_FREE(al, g_prefab.defs, sizeof(ed_def *) * g_prefab.cap);
    memset(&g_prefab, 0, sizeof g_prefab);
}

static ed_def *def_find(const char *asset) {
    for (u32 i = 0; i < g_prefab.count; ++i)
        if (strcmp(g_prefab.defs[i]->asset, asset) == 0)
            return g_prefab.defs[i];
    return NULL;
}

static ed_dval *def_get(const ed_def *d, u64 sid, const char *path) {
    for (u32 i = 0; i < d->val_count; ++i)
        if (d->vals[i].sid == sid && strcmp(d->vals[i].path, path) == 0)
            return &d->vals[i];
    return NULL;
}

static de_ep_raw dval_raw(const ed_def *d, const ed_dval *v) {
    return (de_ep_raw){(const char *)d->bytes + v->off, v->len};
}

static bool def_has_sid(const ed_def *d, u64 sid) {
    for (u32 i = 0; i < d->val_count; ++i)
        if (d->vals[i].sid == sid)
            return true;
    return false;
}

/* Sets (sid, path) of `d` to `raw`. The new bytes go after the old ones. */
static bool def_put(ed_def *d, u64 sid, const char *path, const void *raw, size_t len) {
    const de_allocator *al = de_heap_allocator();
    if (d->used + len > d->cap) {
        size_t cap = d->cap ? d->cap : 1024;
        while (cap < d->used + len)
            cap *= 2;
        u8 *g = (u8 *)DE_ALLOC(al, cap, 8, DE_MEM_GENERAL);
        if (g == NULL)
            return false;
        if (d->bytes != NULL) {
            memcpy(g, d->bytes, d->used);
            DE_FREE(al, d->bytes, d->cap);
        }
        d->bytes = g;
        d->cap = cap;
    }
    ed_dval *v = def_get(d, sid, path);
    if (v == NULL) {
        if (d->val_count == d->val_cap) {
            const u32 cap = d->val_cap ? d->val_cap * 2 : 32;
            ed_dval *g = (ed_dval *)DE_ALLOC(al, sizeof(ed_dval) * cap, 8, DE_MEM_GENERAL);
            if (g == NULL)
                return false;
            if (d->vals != NULL) {
                memcpy(g, d->vals, sizeof(ed_dval) * d->val_count);
                DE_FREE(al, d->vals, sizeof(ed_dval) * d->val_cap);
            }
            d->vals = g;
            d->val_cap = cap;
        }
        v = &d->vals[d->val_count++];
        memset(v, 0, sizeof *v);
        v->sid = sid;
        snprintf(v->path, sizeof v->path, "%s", path);
    }
    memcpy(d->bytes + d->used, raw, len);
    v->off = (u32)d->used;
    v->len = (u32)len;
    d->used += len;
    return true;
}

typedef struct def_visit {
    de__editor *ed;
    ed_def *d;
} def_visit;

/* Each field is written as the editor writes it, so the bytes compare exactly. DENG-810: each
 * entity also gets a "" marker and each component a "Component" marker (nil), so a definition
 * knows its entities and components, tags included. DENG-811: the "" marker holds the entity's
 * parent id (a u64), or nil for a root. */
static void def_visit_fn(void *user, u64 sid, de_type_id type, const char *component,
                         const void *obj) {
    def_visit *v = (def_visit *)user;
    if (component == NULL) {
        const u64 parent = obj != NULL ? *(const u64 *)obj : 0;
        char buf[16];
        mpack_writer_t mw;
        mpack_writer_init(&mw, buf, sizeof buf);
        mpack_write_u64(&mw, parent);
        const size_t len = mpack_writer_buffer_used(&mw);
        const bool ok = mpack_writer_destroy(&mw) == mpack_ok;
        if (parent != 0 && ok)
            (void)def_put(v->d, sid, "", buf, len);
        else
            (void)def_put(v->d, sid, "", k_nil, 1);
        return;
    }
    if (comp_name_ok(component))
        (void)def_put(v->d, sid, component, k_nil, 1);
    if (obj == NULL)
        return;
    u32 n = 0;
    const de_field_info *fields = de_type_fields(v->ed->e->types, type, &n);
    for (u32 i = 0; i < n && fields != NULL; ++i) {
        char buf[4096];
        mpack_writer_t mw;
        mpack_writer_init(&mw, buf, sizeof buf);
        de__ed_write_field(v->ed, &mw, type, i, &fields[i], (const u8 *)obj + fields[i].offset);
        const size_t len = mpack_writer_buffer_used(&mw);
        if (mpack_writer_destroy(&mw) != mpack_ok)
            continue;
        char path[128];
        snprintf(path, sizeof path, "%s.%s", component, fields[i].name);
        (void)def_put(v->d, sid, path, buf, len);
    }
}

static void def_str(const toml_datum_t *d, char *out, size_t cap) {
    if (d != NULL && d->type == TOML_STRING)
        snprintf(out, cap, "%.*s", (int)d->u.str.len, d->u.str.ptr);
}

/* The `[prefab]` table of the prefab's source, if it has one. False when the source can't be
 * read or parsed. */
static bool def_links(de__editor *ed, ed_def *d) {
    const de_allocator *al = de_heap_allocator();
    void *src = NULL;
    u64 size = 0;
    if (!de__ed_asset_source(ed, d->asset, &src, &size))
        return false;
    toml_result_t tr = de__toml_parse(al, (const char *)src, (int)size);
    const bool parsed = tr.ok;
    const toml_datum_t *pf = tr.ok ? tab_get(&tr.toptab, "prefab") : NULL;
    if (pf != NULL && pf->type == TOML_TABLE) {
        def_str(tab_get(pf, "base"), d->base, sizeof d->base);
        const toml_datum_t *ns = tab_get(pf, "nested");
        const u32 nn = ns != NULL && ns->type == TOML_ARRAY ? (u32)ns->u.arr.size : 0;
        d->nest = nn ? (ed_nest *)DE_ALLOC(al, sizeof(ed_nest) * nn, 8, DE_MEM_GENERAL) : NULL;
        for (u32 i = 0; i < nn && d->nest != NULL; ++i) {
            ed_nest *n = &d->nest[d->nest_count];
            memset(n, 0, sizeof *n);
            const toml_datum_t *t = &ns->u.arr.elem[i];
            if (t->type != TOML_TABLE)
                continue;
            def_str(tab_get(t, "asset"), n->asset, sizeof n->asset);
            const toml_datum_t *root = tab_get(t, "root");
            n->root = root != NULL ? parse_sid(root) : 0;
            const toml_datum_t *ids = tab_get(t, "ids");
            const u32 count = ids != NULL && ids->type == TOML_ARRAY ? (u32)ids->u.arr.size : 0;
            if (n->asset[0] == '\0' || count == 0)
                continue;
            n->inner = (u64 *)DE_ALLOC(al, sizeof(u64) * 2 * count, 8, DE_MEM_GENERAL);
            if (n->inner == NULL)
                continue;
            n->outer = n->inner + count;
            for (u32 k = 0; k < count; ++k) {
                const toml_datum_t *pair = &ids->u.arr.elem[k];
                const bool ok = pair->type == TOML_ARRAY && pair->u.arr.size == 2;
                n->inner[n->count] = ok ? parse_sid(&pair->u.arr.elem[0]) : 0;
                n->outer[n->count] = ok ? parse_sid(&pair->u.arr.elem[1]) : 0;
                n->count += ok && n->inner[n->count] != 0 && n->outer[n->count] != 0;
            }
            ++d->nest_count;
        }
    }
    de__toml_free(tr);
    DE_FREE(al, src, size + 1);
    return parsed;
}

/* Reads `d`'s values and links from its asset (the asset system's current object and the
 * manifest's source). `d` must be empty. */
static void def_fill(de__editor *ed, ed_def *d) {
    de_asset_system *s = ed->e->assets;
    const de_asset_id id = de_asset_id_from(d->asset);
    const de_prefab p = de_asset_prefab(s, id);
    if (DE_HANDLE_VALID(p)) {
        de_load_handle lh = de_asset_load(s, id, 0);
        if (de_asset_wait(s, lh, 10000000000ULL) == DE_LOAD_READY) {
            def_visit v = {ed, d};
            d->ready = true;
            (void)de__prefab_visit(s, ed->e->world, p, def_visit_fn, &v);
        }
        de_asset_release(s, lh);
    }
    de_error_clear();
    (void)def_links(ed, d);
}

/* The definition of `asset`, loading it and the prefabs it links to on first use. */
static ed_def *prefab_def_load(de__editor *ed, const char *asset, u32 depth) {
    ed_def *d = def_find(asset);
    de_asset_system *s = ed->e->assets;
    if (d != NULL || depth > ED_CHAIN_MAX || s == NULL || asset[0] == '\0')
        return d;
    const de_allocator *al = de_heap_allocator();
    if (g_prefab.count == g_prefab.cap) {
        const u32 cap = g_prefab.cap ? g_prefab.cap * 2 : 16;
        ed_def **g = (ed_def **)DE_ALLOC(al, sizeof(ed_def *) * cap, 8, DE_MEM_GENERAL);
        if (g == NULL)
            return NULL;
        if (g_prefab.defs != NULL) {
            memcpy(g, g_prefab.defs, sizeof(ed_def *) * g_prefab.count);
            DE_FREE(al, g_prefab.defs, sizeof(ed_def *) * g_prefab.cap);
        }
        g_prefab.defs = g;
        g_prefab.cap = cap;
    }
    d = (ed_def *)DE_ALLOC(al, sizeof *d, 8, DE_MEM_GENERAL);
    if (d == NULL)
        return NULL;
    memset(d, 0, sizeof *d);
    snprintf(d->asset, sizeof d->asset, "%s", asset);
    g_prefab.defs[g_prefab.count++] = d; /* before the links: a cycle stops here */
    def_fill(ed, d);
    if (d->base[0] != '\0')
        (void)prefab_def_load(ed, d->base, depth + 1);
    for (u32 k = 0; k < d->nest_count; ++k)
        (void)prefab_def_load(ed, d->nest[k].asset, depth + 1);
    return d;
}

/* One level of a field's chain: the definition, the entity's id in it, and the nested link that
 * led to it from the level above (NULL: the level above is a variant of it). */
typedef struct ed_level {
    ed_def *d;
    u64 sid;
    const ed_nest *via;
} ed_level;

/* The level below (d, sid): the variant's base when it has the entity, else the nested prefab
 * the entity came from. */
static bool def_next(const ed_def *d, u64 sid, ed_level *out) {
    if (d->base[0] != '\0') {
        ed_def *b = def_find(d->base);
        if (b != NULL && def_has_sid(b, sid)) {
            *out = (ed_level){b, sid, NULL};
            return true;
        }
    }
    for (u32 k = 0; k < d->nest_count; ++k)
        for (u32 i = 0; i < d->nest[k].count; ++i)
            if (d->nest[k].outer[i] == sid) {
                ed_def *n = def_find(d->nest[k].asset);
                if (n == NULL)
                    return false;
                *out = (ed_level){n, d->nest[k].inner[i], &d->nest[k]};
                return true;
            }
    return false;
}

/* The levels that hold `path` for (d, sid), from the instance's prefab down. */
static u32 chain_of(ed_def *d, u64 sid, const char *path, ed_level *out) {
    if (d == NULL || def_get(d, sid, path) == NULL)
        return 0;
    u32 n = 0;
    out[n++] = (ed_level){d, sid, NULL};
    ed_level next;
    while (n < ED_CHAIN_MAX && def_next(out[n - 1].d, out[n - 1].sid, &next) &&
           def_get(next.d, next.sid, path) != NULL)
        out[n++] = next;
    return n;
}

/* The shape of a field value, so entity ids nested in structs and arrays can be found. `has`:
 * the value holds an entity id somewhere. `owner` and `index` place `f` in its struct type (an
 * array's element needs them); owner 0 is an element or a path's leaf. */
typedef struct ed_kind {
    const de_type_registry *r;
    de_type_id owner;
    u32 index;
    de_field_info f;
    bool has;
} ed_kind;

#define ED_KIND_DEPTH 8u

static bool kind_has_entity(const de_type_registry *r, de_type_id owner, u32 index,
                            const de_field_info *f, u32 depth) {
    if (depth > ED_KIND_DEPTH)
        return false;
    if (f->kind == DE_FK_ENTITY)
        return true;
    if (f->kind == DE_FK_STRUCT && f->attrs.nested_type != NULL) {
        const de_type_id nt = de_type_find(r, f->attrs.nested_type);
        u32 n = 0;
        const de_field_info *fs = nt.id ? de_type_fields(r, nt, &n) : NULL;
        for (u32 i = 0; i < n && fs != NULL; ++i)
            if (kind_has_entity(r, nt, i, &fs[i], depth + 1))
                return true;
        return false;
    }
    if (f->kind == DE_FK_FIXED_ARRAY && owner.id) {
        const de_field_info *el = de_type_array_element(r, owner, index);
        return el != NULL && kind_has_entity(r, (de_type_id){0}, 0, el, depth + 1);
    }
    return false;
}

/* Whether `path` names a field, and the shape of the field's value. */
static bool path_field(de__editor *ed, const char *path, ed_kind *entity) {
    const char *dot = strchr(path, '.');
    if (dot == NULL)
        return false;
    const de_component c = de__ed_component(ed, (de_ep_str){path, (u32)(dot - path)});
    de_type_id type;
    u32 size = 0;
    const de_field_info *f = NULL;
    u32 off = 0;
    if (c.id == 0 || !de__ed_component_type(ed, c, &type, &size) ||
        de_reflect_resolve_path(ed->e->types, type, dot + 1, &f, &off) != DE_OK || f == NULL) {
        de_error_clear();
        return false;
    }
    *entity = (ed_kind){.r = ed->e->types, .f = *f};
    /* A whole top-level field has an index in its component's type. */
    u32 n = 0;
    const de_field_info *fs = de_type_fields(ed->e->types, type, &n);
    for (u32 i = 0; i < n && fs != NULL; ++i)
        if (strcmp(fs[i].name, dot + 1) == 0) {
            entity->owner = type;
            entity->index = i;
        }
    entity->has = kind_has_entity(entity->r, entity->owner, entity->index, f, 0);
    return true;
}

static bool raw_eq(de_ep_raw x, de_ep_raw y) {
    return x.len == y.len && memcmp(x.ptr, y.ptr, x.len) == 0;
}

typedef struct ed_map {
    const u64 *from, *to;
    u32 n;
} ed_map;

/* Copies `n` unchanged. */
static bool node_copy(mpack_writer_t *w, mpack_node_t n) {
    switch (mpack_node_type(n)) {
    case mpack_type_nil: mpack_write_nil(w); return true;
    case mpack_type_bool: mpack_write_bool(w, mpack_node_bool(n)); return true;
    case mpack_type_int: mpack_write_i64(w, mpack_node_i64(n)); return true;
    case mpack_type_uint: mpack_write_u64(w, mpack_node_u64(n)); return true;
    case mpack_type_float: mpack_write_float(w, mpack_node_float(n)); return true;
    case mpack_type_double: mpack_write_double(w, mpack_node_double(n)); return true;
    case mpack_type_str: mpack_write_str(w, mpack_node_str(n), (u32)mpack_node_strlen(n)); return true;
    case mpack_type_bin: mpack_write_bin(w, mpack_node_data(n), (u32)mpack_node_bin_size(n)); return true;
    case mpack_type_array: {
        const u32 len = (u32)mpack_node_array_length(n);
        mpack_start_array(w, len);
        for (u32 i = 0; i < len; ++i)
            if (!node_copy(w, mpack_node_array_at(n, i)))
                return false;
        mpack_finish_array(w);
        return true;
    }
    case mpack_type_map: {
        const u32 len = (u32)mpack_node_map_count(n);
        mpack_start_map(w, len);
        for (u32 i = 0; i < len; ++i)
            if (!node_copy(w, mpack_node_map_key_at(n, i)) ||
                !node_copy(w, mpack_node_map_value_at(n, i)))
                return false;
        mpack_finish_map(w);
        return true;
    }
    default: return false;
    }
}

/* Writes `n`, a value of field `f`, with its entity ids mapped. Mirrors de__ed_write_field. */
static bool node_remap(mpack_writer_t *w, mpack_node_t n, const ed_kind *k, de_type_id owner,
                       u32 index, const de_field_info *f, const ed_map *m, u32 depth) {
    if (depth > ED_KIND_DEPTH || !kind_has_entity(k->r, owner, index, f, 0))
        return node_copy(w, n);
    if (f->kind == DE_FK_ENTITY) {
        if (mpack_node_type(n) != mpack_type_uint && mpack_node_type(n) != mpack_type_int)
            return false;
        const u64 v = mpack_node_u64(n);
        if (v == 0) {
            mpack_write_u64(w, 0);
            return true;
        }
        for (u32 i = 0; i < m->n; ++i)
            if (m->from[i] == v) {
                mpack_write_u64(w, m->to[i]);
                return true;
            }
        return false;
    }
    if (f->kind == DE_FK_STRUCT) {
        const de_type_id nt = de_type_find(k->r, f->attrs.nested_type);
        u32 nf = 0;
        const de_field_info *fs = de_type_fields(k->r, nt, &nf);
        if (mpack_node_type(n) != mpack_type_map || fs == NULL)
            return false;
        const u32 len = (u32)mpack_node_map_count(n);
        mpack_start_map(w, len);
        for (u32 i = 0; i < len; ++i) {
            mpack_node_t key = mpack_node_map_key_at(n, i), val = mpack_node_map_value_at(n, i);
            if (!node_copy(w, key))
                return false;
            u32 j = 0;
            while (j < nf && !(mpack_node_type(key) == mpack_type_str &&
                               mpack_node_strlen(key) == strlen(fs[j].name) &&
                               memcmp(mpack_node_str(key), fs[j].name, strlen(fs[j].name)) == 0))
                ++j;
            if (!(j < nf ? node_remap(w, val, k, nt, j, &fs[j], m, depth + 1) : node_copy(w, val)))
                return false;
        }
        mpack_finish_map(w);
        return true;
    }
    if (f->kind == DE_FK_FIXED_ARRAY) {
        const de_field_info *el = de_type_array_element(k->r, owner, index);
        if (mpack_node_type(n) != mpack_type_array || el == NULL)
            return false;
        const u32 len = (u32)mpack_node_array_length(n);
        mpack_start_array(w, len);
        for (u32 i = 0; i < len; ++i)
            if (!node_remap(w, mpack_node_array_at(n, i), k, (de_type_id){0}, 0, el, m, depth + 1))
                return false;
        mpack_finish_array(w);
        return true;
    }
    return node_copy(w, n);
}

/* `raw` with its entity ids mapped from from[i] to to[i], also those inside struct and array
 * fields. A value without entities, or a NULL `from` (same ids), passes unchanged; 0 (no
 * entity) stays 0. False: an id isn't in `from`. */
static bool remap(de_ep_arena *a, ed_kind entity, de_ep_raw raw, const u64 *from, const u64 *to,
                  u32 n, de_ep_raw *out) {
    *out = raw;
    if (!entity.has || from == NULL)
        return true;
    if (raw.len > 65536)
        return false;
    const size_t cap = (size_t)raw.len * 9 + 16;
    char *b = (char *)de__ed_alloc(a, cap);
    if (b == NULL)
        return false;
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, raw.ptr, raw.len);
    mpack_tree_parse(&tree);
    mpack_writer_t w;
    mpack_writer_init(&w, b, cap);
    const ed_map m = {from, to, n};
    const bool ok = mpack_tree_error(&tree) == mpack_ok &&
                    node_remap(&w, mpack_tree_root(&tree), &entity, entity.owner, entity.index,
                               &entity.f, &m, 0);
    if (!ok)
        mpack_writer_flag_error(&w, mpack_error_data); /* open containers are dropped */
    const size_t used = mpack_writer_buffer_used(&w);
    const bool wok = mpack_writer_destroy(&w) == mpack_ok;
    const bool tok = mpack_tree_destroy(&tree) == mpack_ok;
    if (!ok || !wok || !tok)
        return false;
    out->ptr = b;
    out->len = used;
    return true;
}

/* Level k's value of `path`, in the ids of level k + 1. */
static bool down_one(de_ep_arena *a, ed_kind entity, const ed_level *lv, u32 k, de_ep_raw raw,
                     de_ep_raw *out) {
    const ed_nest *n = lv[k + 1].via;
    return remap(a, entity, raw, n ? n->outer : NULL, n ? n->inner : NULL, n ? n->count : 0, out);
}

/* Whether level k sets `path` itself: its value differs from the level below's (exact bytes). */
static bool level_owns(de_ep_arena *a, ed_kind entity, const ed_level *lv, u32 count, u32 k,
                       const char *path) {
    if (k + 1 >= count)
        return true;
    de_ep_raw mine;
    if (!down_one(a, entity, lv, k, dval_raw(lv[k].d, def_get(lv[k].d, lv[k].sid, path)), &mine))
        return true;
    return !raw_eq(mine, dval_raw(lv[k + 1].d, def_get(lv[k + 1].d, lv[k + 1].sid, path)));
}

/* The instance base value at `b` becomes `raw`. The new bytes go after the old ones. */
static bool base_put(ed_instance *inst, ed_base *b, const void *raw, size_t len) {
    u8 *grown = (u8 *)DE_ALLOC(de_heap_allocator(), inst->byte_count + len, 8, DE_MEM_GENERAL);
    if (grown == NULL)
        return false;
    if (inst->bytes != NULL) {
        memcpy(grown, inst->bytes, inst->byte_count);
        DE_FREE(de_heap_allocator(), inst->bytes, inst->byte_count);
    }
    memcpy(grown + inst->byte_count, raw, len);
    b->off = (u32)inst->byte_count;
    b->len = (u32)len;
    inst->bytes = grown;
    inst->byte_count += len;
    return true;
}

/* ---------- Prefab instance links in scene text (DENG-799, DIDE-R37) ----------
 *
 * A saved scene keeps each prefab instance as a top-level `[[prefab.nested]]` entry, the same
 * shape a prefab source uses for the prefabs nested in it:
 *
 *     [[prefab.nested]]
 *     asset = "props/stack"
 *     root = "0x<scene id of the root>"
 *     ids = [["0x<prefab-local id>", "0x<scene id>"], ...]
 *
 * Entries are in root scene-ID order, pairs in prefab-local order. Only entities the scene saves
 * appear. An unpacked instance has no record, so it writes no entry. On load, an entry that does
 * not resolve is skipped with a warning: the entities still load, only the link is lost. */

static de_ep_raw base_raw(const ed_instance *inst, const ed_base *b);

static void put_toml_str(de_mem_writer *out, const char *str) {
    de_writer w = de_writer_from_mem(out);
    (void)w.write(w.user, "\"", 1);
    for (const char *c = str; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\')
            (void)w.write(w.user, "\\", 1);
        (void)w.write(w.user, c, 1);
    }
    (void)w.write(w.user, "\"", 1);
}

static u64 saved_sid(const save_ent *list, u32 n, u64 entity, bool *found) {
    for (u32 i = 0; i < n; ++i)
        if (list[i].entity == entity) {
            *found = true;
            return list[i].sid;
        }
    *found = false;
    return 0;
}

typedef struct link_pair {
    u64 local, sid;
} link_pair;

static int cmp_link_pair(const void *x, const void *y) {
    const link_pair *a = (const link_pair *)x, *b = (const link_pair *)y;
    if (a->local != b->local)
        return a->local < b->local ? -1 : 1;
    return a->sid < b->sid ? -1 : a->sid > b->sid;
}

static void write_links(de__editor *ed, const save_ent *list, u32 n, de_mem_writer *out) {
    (void)ed;
    const de_allocator *al = de_heap_allocator();
    const u32 count = g_scene.instance_count;
    if (count == 0)
        return;
    save_ent *roots = (save_ent *)DE_ALLOC(al, sizeof(save_ent) * count, 8, DE_MEM_GENERAL);
    link_pair *pairs =
        (link_pair *)DE_ALLOC(al, sizeof(link_pair) * ED_LOCAL_CAP, 8, DE_MEM_GENERAL);
    if (roots == NULL || pairs == NULL) {
        if (roots != NULL)
            DE_FREE(al, roots, sizeof(save_ent) * count);
        if (pairs != NULL)
            DE_FREE(al, pairs, sizeof(link_pair) * ED_LOCAL_CAP);
        return;
    }
    u32 rn = 0;
    for (u32 i = 0; i < count; ++i) {
        bool found = false;
        const u64 sid = saved_sid(list, n, g_scene.instances[i].root, &found);
        if (found)
            roots[rn++] = (save_ent){i, sid}; /* entity holds the instance index here */
    }
    qsort(roots, rn, sizeof(save_ent), cmp_save);
    for (u32 r = 0; r < rn; ++r) {
        const ed_instance *inst = &g_scene.instances[roots[r].entity];
        u32 pn = 0;
        for (u32 k = 0; k < inst->local_count && pn < ED_LOCAL_CAP; ++k) {
            bool found = false;
            const u64 sid = saved_sid(list, n, inst->local_ent[k], &found);
            if (found)
                pairs[pn++] = (link_pair){inst->local_sid[k], sid};
        }
        qsort(pairs, pn, sizeof(link_pair), cmp_link_pair);
        putf(out, "\n[[prefab.nested]]\nasset = ");
        put_toml_str(out, inst->asset);
        putf(out, "\nroot = \"0x%016llx\"\nids = [", (unsigned long long)roots[r].sid);
        for (u32 k = 0; k < pn; ++k)
            putf(out, "%s[\"0x%016llx\", \"0x%016llx\"]", k ? ", " : "",
                 (unsigned long long)pairs[k].local, (unsigned long long)pairs[k].sid);
        putf(out, "]\n");
    }
    DE_FREE(al, roots, sizeof(save_ent) * count);
    DE_FREE(al, pairs, sizeof(link_pair) * ED_LOCAL_CAP);
}

/* The instance's fields as the prefab gives them, where the prefab has them: what a fresh spawn
 * would have recorded. A field the prefab does not have keeps its current value. */
static bool is_nil(de_ep_raw r);
static ed_base *base_find(ed_instance *inst, u64 entity, de_ep_str path);
static bool def_comp_map(de_ep_arena *a, const ed_def *d, u64 sid, const char *comp,
                         de_ep_raw *out);
static bool comp_remap(de__editor *ed, de_ep_arena *a, const char *comp, de_ep_raw map,
                       const u64 *from, const u64 *to, u32 n, de_ep_raw *out);
static bool base_comp_set(ed_instance *inst, u64 entity, const char *comp, de_ep_raw map);

/* The instance base takes the definition's components (DENG-810): one the instance lost
 * (component_removed) comes back into the base from the definition, and one the instance
 * added (component_added) leaves it. */
static void link_components(de__editor *ed, ed_instance *inst, const ed_def *d,
                            const de_ep_arena *scratch) {
    for (u32 j = 0; j < inst->local_count; ++j) {
        const u64 ent = inst->local_ent[j], lsid = inst->local_sid[j];
        if (!def_has_sid(d, lsid))
            continue;
        for (bool again = true; again;) {
            again = false;
            for (u32 i = 0; i < inst->base_count && !again; ++i) {
                const ed_base *b = &inst->base[i];
                if (b->entity != ent || strchr(b->path, '.') != NULL || !comp_name_ok(b->path) ||
                    def_get(d, lsid, b->path) != NULL)
                    continue;
                char comp[128];
                snprintf(comp, sizeof comp, "%s", b->path);
                again = base_comp_set(inst, ent, comp, (de_ep_raw){k_nil, 1});
            }
        }
        for (u32 i = 0; i < d->val_count; ++i) {
            const ed_dval *v = &d->vals[i];
            if (v->sid != lsid || strchr(v->path, '.') != NULL || !comp_name_ok(v->path) ||
                base_find(inst, ent, de_ep_cstr(v->path)) != NULL)
                continue;
            de_ep_arena a = *scratch;
            de_ep_raw map, mine;
            if (def_comp_map(&a, d, lsid, v->path, &map) &&
                comp_remap(ed, &a, v->path, map, inst->local_sid, inst->local_ent,
                           inst->local_count, &mine))
                (void)base_comp_set(inst, ent, v->path, mine);
        }
    }
}

static void link_base(de__editor *ed, ed_instance *inst, const de_ep_arena *scratch) {
    instance_base(ed, inst, scratch);
    const ed_def *d = prefab_def_load(ed, inst->asset, 0);
    if (d == NULL)
        return;
    for (u32 i = 0; i < inst->base_count; ++i) {
        ed_base *b = &inst->base[i];
        u64 lsid = 0;
        if (!local_sid_of(inst, b->entity, &lsid))
            continue;
        const ed_dval *v = def_get(d, lsid, b->path);
        ed_kind kind = {0};
        if (v == NULL || !path_field(ed, b->path, &kind))
            continue;
        de_ep_arena a = *scratch;
        de_ep_raw nv = {NULL, 0};
        if (remap(&a, kind, dval_raw(d, v), inst->local_sid, inst->local_ent, inst->local_count,
                  &nv) &&
            !raw_eq(nv, base_raw(inst, b)))
            (void)base_put(inst, b, nv.ptr, nv.len);
    }
    link_components(ed, inst, d, scratch);
}

static void link_instances(de__editor *ed, const toml_datum_t *top, const load_map *m,
                           const de_ep_arena *scratch) {
    const toml_datum_t *prefab = tab_get(top, "prefab");
    const toml_datum_t *nested = prefab != NULL ? tab_get(prefab, "nested") : NULL;
    if (nested == NULL)
        return;
    if (nested->type != TOML_ARRAY) {
        DE_WARN(ed->log, "scene: prefab.nested is no array of tables; prefab links skipped");
        return;
    }
    for (int32_t i = 0; i < nested->u.arr.size; ++i) {
        const toml_datum_t *e = &nested->u.arr.elem[i];
        const toml_datum_t *asset = tab_get(e, "asset");
        const toml_datum_t *root = tab_get(e, "root");
        const toml_datum_t *ids = tab_get(e, "ids");
        const u64 root_sid = root != NULL ? parse_sid(root) : 0;
        const u64 root_ent = root_sid != 0 ? map_sid_to_entity(root_sid, (void *)m) : 0;
        if (asset == NULL || asset->type != TOML_STRING || asset->u.str.len == 0 ||
            asset->u.str.len >= 256 || ids == NULL || ids->type != TOML_ARRAY || root_ent == 0 ||
            instance_rooted(root_ent) != NULL) {
            DE_WARN(ed->log, "scene: prefab link %d (line %d) does not resolve; skipped", i,
                    e->lineno);
            continue;
        }
        u64 *block = (u64 *)DE_ALLOC(de_heap_allocator(), sizeof(u64) * ED_LOCAL_CAP * 2, 8,
                                     DE_MEM_GENERAL);
        if (block == NULL)
            return;
        u32 ln = 0;
        for (int32_t k = 0; k < ids->u.arr.size && ln < ED_LOCAL_CAP; ++k) {
            const toml_datum_t *pr = &ids->u.arr.elem[k];
            if (pr->type != TOML_ARRAY || pr->u.arr.size != 2)
                continue;
            const u64 local = parse_sid(&pr->u.arr.elem[0]);
            const u64 sid = parse_sid(&pr->u.arr.elem[1]);
            const u64 ent = sid != 0 ? map_sid_to_entity(sid, (void *)m) : 0;
            if (local == 0 || ent == 0)
                continue;
            block[ln] = local;
            block[ED_LOCAL_CAP + ln] = ent;
            ++ln;
        }
        ed_instance *inst = instance_push();
        if (inst == NULL) {
            DE_FREE(de_heap_allocator(), block, sizeof(u64) * ED_LOCAL_CAP * 2);
            return;
        }
        inst->root = root_ent;
        snprintf(inst->asset, sizeof inst->asset, "%s", asset->u.str.ptr);
        /* One block, as a spawn keeps it: the ids, then the entities. */
        inst->local_sid = block;
        inst->local_ent = block + ED_LOCAL_CAP;
        inst->local_count = ln;
        link_base(ed, inst, scratch);
    }
}

bool de__ed_prefab_spawn(de__editor *ed, de_ep_arena *a, const char *asset, u64 scene,
                         de_entity parent, de_entity *root, de_ep_error *code, char *msg,
                         size_t msg_cap) {
    de_asset_system *s = ed->e->assets;
    *root = DE_ENTITY_NULL;
    if (s == NULL || asset == NULL) {
        *code = DE_EP_ERR_UNSUPPORTED;
        snprintf(msg, msg_cap, "no asset system");
        return false;
    }
    if (scene_find(scene) == NULL) {
        *code = DE_EP_ERR_NOT_FOUND;
        snprintf(msg, msg_cap, "scene %llu", (unsigned long long)scene);
        return false;
    }
    const de_asset_id id = de_asset_id_from(asset);
    de_prefab p = de_asset_prefab(s, id);
    if (!DE_HANDLE_VALID(p)) {
        de_error_clear();
        *code = DE_EP_ERR_NOT_FOUND;
        snprintf(msg, msg_cap, "prefab %s", asset);
        return false;
    }
    de_load_handle lh = de_asset_load(s, id, 0);
    if (de_asset_wait(s, lh, 10000000000ULL) != DE_LOAD_READY) {
        de_asset_release(s, lh);
        *code = DE_EP_ERR_NOT_FOUND;
        snprintf(msg, msg_cap, "prefab %s did not load", asset);
        return false;
    }
    u64 *lsid =
        (u64 *)DE_ALLOC(de_heap_allocator(), sizeof(u64) * ED_LOCAL_CAP * 2, 8, DE_MEM_GENERAL);
    de_entity lent[ED_LOCAL_CAP];
    u32 lcount = 0;
    const de_result r =
        lsid != NULL ? de__prefab_instantiate_ids(s, ed->e->world, p, parent, root, lsid, lent,
                                                  ED_LOCAL_CAP, &lcount)
                     : de_prefab_instantiate(s, ed->e->world, p, parent, root);
    de_asset_release(s, lh);
    if (r != DE_OK && lsid != NULL)
        DE_FREE(de_heap_allocator(), lsid, sizeof(u64) * ED_LOCAL_CAP * 2);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        *code = DE_EP_ERR_INVALID_PARAMS;
        snprintf(msg, msg_cap, "%s", e != NULL ? e->message : "instantiate failed");
        de_error_clear();
        return false;
    }
    /* DENG-824: a foliage page is a root of the prefab, beside its layer; the instance root is the
     * first other root. */
    if (ed->foliage != NULL && ed->foliage->page_layer(ed, root->bits) != 0)
        for (u32 i = 0; i < lcount; ++i)
            if (ed->foliage->page_layer(ed, lent[i].bits) == 0 &&
                de_entity_parent(ed->e->world, lent[i]).bits == parent.bits) {
                *root = lent[i];
                break;
            }
    u64 ents[1024];
    const u32 n = tree_of(ed, *root, ents, 1024);
    for (u32 i = 0; i < n; ++i)
        member_add(ents[i], scene, de_scene_id_new());
    if (ed->foliage != NULL && lcount != 0) {
        u64 all[ED_LOCAL_CAP];
        for (u32 i = 0; i < lcount; ++i)
            all[i] = lent[i].bits;
        ed->foliage->loaded(ed, all, lcount, parent);
    }
    /* Remember the instance and its fields as instantiated. */
    ed_instance *inst = instance_push();
    if (inst != NULL) {
        inst->root = root->bits;
        snprintf(inst->asset, sizeof inst->asset, "%s", asset);
        /* Scratch past what the caller keeps, 16-byte aligned like the arena's own. */
        const size_t at = (a->used + 15u) & ~(size_t)15u;
        const de_ep_arena scratch = {a->base + at, at < a->size ? a->size - at : 0, 0};
        instance_base(ed, inst, &scratch);
        if (lsid != NULL) {
            /* One block: the entities, then the ids. */
            u64 *ids = lsid;
            u64 *lents = lsid + ED_LOCAL_CAP;
            for (u32 i = 0; i < lcount; ++i)
                lents[i] = lent[i].bits;
            inst->local_sid = ids;
            inst->local_ent = lents;
            inst->local_count = lcount;
            lsid = NULL;
        }
        (void)prefab_def_load(ed, asset, 0);
    }
    if (lsid != NULL)
        DE_FREE(de_heap_allocator(), lsid, sizeof(u64) * ED_LOCAL_CAP * 2);
    de__ed_world_touch(ed);
    return true;
}

const char *de__ed_prefab_instance_asset(u64 root) {
    const ed_instance *inst = instance_rooted(root);
    return inst != NULL ? inst->asset : NULL;
}

void de__ed_prefab_instance_write_base(u64 root, mpack_writer_t *w) {
    const ed_instance *inst = instance_rooted(root);
    const u32 n = inst != NULL ? inst->base_count : 0;
    mpack_start_array(w, n);
    for (u32 i = 0; i < n; ++i) {
        const ed_base *b = &inst->base[i];
        mpack_start_map(w, 3);
        mpack_write_cstr(w, "entity");
        mpack_write_u64(w, b->entity);
        mpack_write_cstr(w, "path");
        mpack_write_cstr(w, b->path);
        mpack_write_cstr(w, "value");
        mpack_write_object_bytes(w, (const char *)inst->bytes + b->off, b->len);
        mpack_finish_map(w);
    }
    mpack_finish_array(w);
}

void de__ed_prefab_instance_drop(u64 root) {
    ed_instance *inst = instance_rooted(root);
    if (inst == NULL)
        return;
    const de_allocator *al = de_heap_allocator();
    if (inst->base != NULL)
        DE_FREE(al, inst->base, sizeof(ed_base) * inst->base_cap);
    if (inst->bytes != NULL)
        DE_FREE(al, inst->bytes, inst->byte_count);
    instance_local_free(inst);
    const u32 i = (u32)(inst - g_scene.instances);
    memmove(inst, inst + 1, sizeof(ed_instance) * (g_scene.instance_count - i - 1));
    --g_scene.instance_count;
}

void de__ed_prefab_instance_write_local(u64 root, mpack_writer_t *w) {
    const ed_instance *inst = instance_rooted(root);
    const u32 n = inst != NULL ? inst->local_count : 0;
    mpack_start_array(w, n);
    for (u32 i = 0; i < n; ++i) {
        mpack_start_array(w, 2);
        mpack_write_u64(w, inst->local_sid[i]);
        mpack_write_u64(w, inst->local_ent[i]);
        mpack_finish_array(w);
    }
    mpack_finish_array(w);
}

/* The [sid, entity] pairs that de__ed_prefab_instance_write_local wrote, into `inst`. */
static void local_restore(ed_instance *inst, const char *local, size_t len) {
    if (local == NULL || len == 0)
        return;
    u64 *block =
        (u64 *)DE_ALLOC(de_heap_allocator(), sizeof(u64) * ED_LOCAL_CAP * 2, 8, DE_MEM_GENERAL);
    if (block == NULL)
        return;
    mpack_reader_t r;
    mpack_reader_init_data(&r, local, len);
    const u32 n = mpack_expect_array_max(&r, ED_LOCAL_CAP);
    u32 count = 0;
    for (u32 i = 0; i < n && mpack_reader_error(&r) == mpack_ok; ++i) {
        mpack_expect_array_match(&r, 2);
        block[count] = mpack_expect_u64(&r);
        block[ED_LOCAL_CAP + count] = mpack_expect_u64(&r);
        mpack_done_array(&r);
        ++count;
    }
    mpack_done_array(&r);
    if (mpack_reader_destroy(&r) != mpack_ok) {
        DE_FREE(de_heap_allocator(), block, sizeof(u64) * ED_LOCAL_CAP * 2);
        return;
    }
    inst->local_sid = block;
    inst->local_ent = block + ED_LOCAL_CAP;
    inst->local_count = count;
}

bool de__ed_prefab_instance_restore(u64 root, const char *asset, const char *base, size_t len,
                                    const char *local, size_t local_len) {
    const de_allocator *al = de_heap_allocator();
    mpack_reader_t r;
    mpack_reader_init_data(&r, base, len);
    const u32 n = mpack_expect_array_max(&r, 1u << 20);
    ed_base *b = n ? (ed_base *)DE_ALLOC(al, sizeof(ed_base) * n, 8, DE_MEM_GENERAL) : NULL;
    u8 *bytes = len ? (u8 *)DE_ALLOC(al, len, 8, DE_MEM_GENERAL) : NULL;
    size_t used = 0;
    u32 count = 0;
    if ((n && b == NULL) || (len && bytes == NULL))
        mpack_reader_flag_error(&r, mpack_error_memory);
    for (u32 i = 0; i < n && mpack_reader_error(&r) == mpack_ok; ++i) {
        ed_base *e = &b[count];
        memset(e, 0, sizeof *e);
        bool has_value = false;
        const u32 keys = mpack_expect_map(&r);
        for (u32 k = 0; k < keys && mpack_reader_error(&r) == mpack_ok; ++k) {
            char key[16];
            mpack_expect_cstr(&r, key, sizeof key);
            if (strcmp(key, "entity") == 0) {
                e->entity = mpack_expect_u64(&r);
            } else if (strcmp(key, "path") == 0) {
                mpack_expect_cstr(&r, e->path, sizeof e->path);
            } else if (strcmp(key, "value") == 0) {
                const char *begin = r.data;
                mpack_discard(&r);
                if (mpack_reader_error(&r) == mpack_ok && r.data > begin) {
                    e->off = (u32)used;
                    e->len = (u32)(r.data - begin);
                    memcpy(bytes + used, begin, e->len);
                    used += e->len;
                    has_value = true;
                }
            } else {
                mpack_discard(&r);
            }
        }
        mpack_done_map(&r);
        count += has_value;
    }
    mpack_done_array(&r);
    ed_instance *inst = mpack_reader_destroy(&r) == mpack_ok ? instance_push() : NULL;
    if (inst == NULL) {
        if (b != NULL)
            DE_FREE(al, b, sizeof(ed_base) * n);
        if (bytes != NULL)
            DE_FREE(al, bytes, len);
        return false;
    }
    inst->root = root;
    snprintf(inst->asset, sizeof inst->asset, "%s", asset);
    inst->base = b;
    inst->base_count = count;
    inst->base_cap = b != NULL ? n : 0;
    inst->bytes = bytes;
    inst->byte_count = bytes != NULL ? len : 0;
    local_restore(inst, local, local_len);
    return true;
}

static void on_instantiate(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(prefab_instantiate, de_ep_prefab_instantiate_params, q);
    de_entity root;
    de_ep_error code = DE_EP_OK;
    char msg[256];
    if (!de__ed_prefab_spawn(ed, a, de__ed_cstr(a, q.asset), q.scene,
                             q.has_parent ? (de_entity){q.parent} : DE_ENTITY_NULL, &root, &code,
                             msg, sizeof msg)) {
        de__ed_error(w, req->id, code, "%s", msg);
        return;
    }
    de_ep_entity_result_one res = {root.bits};
    de_ep_write_response_prefab_instantiate(w, req->id, &res);
}

/* The current value of `path` on `entity`, as raw MessagePack in the arena. */
static bool current_value(de__editor *ed, de_ep_arena *a, u64 entity, const char *path,
                          de_ep_raw *out) {
    de_ep_str p = de_ep_cstr(path);
    de_ep_list_str fields = {&p, 1};
    de_ep_entity_row row;
    if (!de__ed_entity_row(ed, a, (de_entity){entity}, NULL, &fields, &row) ||
        row.components.count != 1)
        return false;
    *out = row.components.items[0].value;
    return true;
}

static ed_base *base_find(ed_instance *inst, u64 entity, de_ep_str path) {
    for (u32 i = 0; i < inst->base_count; ++i)
        if (inst->base[i].entity == entity && strlen(inst->base[i].path) == path.len &&
            memcmp(inst->base[i].path, path.ptr, path.len) == 0)
            return &inst->base[i];
    return NULL;
}

static de_ep_raw base_raw(const ed_instance *inst, const ed_base *b) {
    return (de_ep_raw){(const char *)inst->bytes + b->off, b->len};
}

/* ---------- Whole components (DENG-810) ----------
 *
 * A definition and an instance base hold a component as a "Component" marker (nil) plus its
 * "Component.field" entries. These helpers read such a component as one map (keys in entry
 * order, which is the type's field order, so the bytes equal the editor's own encoding of the
 * component), and replace or drop it. An absent component reads as nil. */

static bool is_nil(de_ep_raw r) {
    return r.len == 1 && (u8)r.ptr[0] == 0xc0u;
}

/* A whole-component path ("Component"), as opposed to a field path ("Component.field"). */
static bool is_comp_path(const char *path) {
    return path[0] != '\0' && strchr(path, '.') == NULL;
}

/* The field part of `path` when it is a field of `comp`, else NULL. */
static const char *comp_field_of(const char *path, const char *comp) {
    const size_t n = strlen(comp);
    return strncmp(path, comp, n) == 0 && path[n] == '.' ? path + n + 1 : NULL;
}

/* A map of `n` keys and raw values, in the arena. */
static bool map_build(de_ep_arena *a, const de_ep_str *keys, const de_ep_raw *vals, u32 n,
                      de_ep_raw *out) {
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, n);
    for (u32 i = 0; i < n; ++i) {
        mpack_write_str(&mw, keys[i].ptr, keys[i].len);
        mpack_write_object_bytes(&mw, vals[i].ptr, vals[i].len);
    }
    mpack_finish_map(&mw);
    *out = de__ed_raw_end(a, &mw);
    return out->len != 0;
}

/* `comp` of (d, sid) as a map; nil when the definition does not have it. */
static bool def_comp_map(de_ep_arena *a, const ed_def *d, u64 sid, const char *comp,
                         de_ep_raw *out) {
    if (def_get(d, sid, comp) == NULL) {
        *out = (de_ep_raw){k_nil, 1};
        return true;
    }
    u32 n = 0;
    for (u32 i = 0; i < d->val_count; ++i)
        n += d->vals[i].sid == sid && comp_field_of(d->vals[i].path, comp) != NULL;
    de_ep_str *keys = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (n + 1));
    de_ep_raw *vals = (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * (n + 1));
    if (keys == NULL || vals == NULL)
        return false;
    u32 k = 0;
    for (u32 i = 0; i < d->val_count && k < n; ++i) {
        const char *f = d->vals[i].sid == sid ? comp_field_of(d->vals[i].path, comp) : NULL;
        if (f == NULL)
            continue;
        keys[k] = de_ep_cstr(f);
        vals[k++] = dval_raw(d, &d->vals[i]);
    }
    return map_build(a, keys, vals, k, out);
}

/* `comp` of `entity` in the instance base as a map; nil when the base does not have it. */
static bool base_comp_map(de_ep_arena *a, ed_instance *inst, u64 entity, const char *comp,
                          de_ep_raw *out) {
    if (base_find(inst, entity, de_ep_cstr(comp)) == NULL) {
        *out = (de_ep_raw){k_nil, 1};
        return true;
    }
    u32 n = 0;
    for (u32 i = 0; i < inst->base_count; ++i)
        n += inst->base[i].entity == entity && comp_field_of(inst->base[i].path, comp) != NULL;
    de_ep_str *keys = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (n + 1));
    de_ep_raw *vals = (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * (n + 1));
    if (keys == NULL || vals == NULL)
        return false;
    u32 k = 0;
    for (u32 i = 0; i < inst->base_count && k < n; ++i) {
        const ed_base *b = &inst->base[i];
        const char *f = b->entity == entity ? comp_field_of(b->path, comp) : NULL;
        if (f == NULL)
            continue;
        keys[k] = de_ep_cstr(f);
        vals[k++] = base_raw(inst, b);
    }
    return map_build(a, keys, vals, k, out);
}

/* The current value of `comp` on `entity` as a map; nil when the entity does not have it. */
static bool current_comp(de__editor *ed, de_ep_arena *a, u64 entity, const char *comp,
                         de_ep_raw *out) {
    de_ep_str c = de_ep_cstr(comp);
    de_ep_list_str only = {&c, 1};
    de_ep_entity_row row;
    if (!de__ed_entity_row(ed, a, (de_entity){entity}, &only, NULL, &row))
        return false;
    *out = row.components.count == 1 ? row.components.items[0].value : (de_ep_raw){k_nil, 1};
    return true;
}

/* `map` (a value of `comp`, or nil) with the entity ids of its fields mapped `from` -> `to`. */
static bool comp_remap(de__editor *ed, de_ep_arena *a, const char *comp, de_ep_raw map,
                       const u64 *from, const u64 *to, u32 n, de_ep_raw *out) {
    if (is_nil(map)) {
        *out = map;
        return true;
    }
    mpack_reader_t r;
    mpack_reader_init_data(&r, map.ptr, map.len);
    const u32 count = mpack_expect_map_max(&r, 1024);
    de_ep_str *keys = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (count + 1));
    de_ep_raw *vals = (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * (count + 1));
    if (keys == NULL || vals == NULL)
        mpack_reader_flag_error(&r, mpack_error_memory);
    bool ok = true;
    for (u32 i = 0; i < count && ok && mpack_reader_error(&r) == mpack_ok; ++i) {
        de_ep_kv_raw kv;
        if (!de__ed_raw_map_entry(&r, &kv))
            break;
        keys[i] = kv.key;
        vals[i] = kv.value;
        char path[128];
        snprintf(path, sizeof path, "%s.%.*s", comp, (int)kv.key.len, kv.key.ptr);
        ed_kind kind = {0};
        if (path_field(ed, path, &kind) && kind.has)
            ok = remap(a, kind, kv.value, from, to, n, &vals[i]);
    }
    mpack_done_map(&r);
    if (mpack_reader_destroy(&r) != mpack_ok || !ok)
        return false;
    return map_build(a, keys, vals, count, out);
}

/* Level k's value of `comp`, in the ids of level k + 1. */
static bool comp_down_one(de__editor *ed, de_ep_arena *a, const char *comp, const ed_level *lv,
                          u32 k, de_ep_raw raw, de_ep_raw *out) {
    const ed_nest *n = lv[k + 1].via;
    return comp_remap(ed, a, comp, raw, n ? n->outer : NULL, n ? n->inner : NULL,
                      n ? n->count : 0, out);
}

/* (d, sid) has `comp` as `map`, or not at all when `map` is nil. */
static bool def_comp_set(ed_def *d, u64 sid, const char *comp, de_ep_raw map) {
    u32 k = 0;
    for (u32 i = 0; i < d->val_count; ++i) {
        const ed_dval *v = &d->vals[i];
        if (v->sid == sid && (strcmp(v->path, comp) == 0 || comp_field_of(v->path, comp) != NULL))
            continue;
        d->vals[k++] = *v;
    }
    d->val_count = k;
    if (is_nil(map))
        return true;
    bool ok = def_put(d, sid, comp, k_nil, 1);
    mpack_reader_t r;
    mpack_reader_init_data(&r, map.ptr, map.len);
    const u32 count = mpack_expect_map_max(&r, 1024);
    for (u32 i = 0; i < count && ok && mpack_reader_error(&r) == mpack_ok; ++i) {
        de_ep_kv_raw kv;
        if (!de__ed_raw_map_entry(&r, &kv))
            break;
        char path[128];
        snprintf(path, sizeof path, "%s.%.*s", comp, (int)kv.key.len, kv.key.ptr);
        ok = def_put(d, sid, path, kv.value.ptr, kv.value.len);
    }
    mpack_done_map(&r);
    return mpack_reader_destroy(&r) == mpack_ok && ok;
}

/* A new base entry (entity, path) with value `raw`. */
static bool base_append(ed_instance *inst, u64 entity, const char *path, const void *raw,
                        size_t len) {
    ed_base *b = base_vec_push(&inst->base, &inst->base_cap, &inst->base_count);
    if (b == NULL)
        return false;
    b->entity = entity;
    snprintf(b->path, sizeof b->path, "%s", path);
    if (!base_put(inst, b, raw, len)) {
        --inst->base_count;
        return false;
    }
    return true;
}

/* The base of `entity` has `comp` as `map`, or not at all when `map` is nil. */
static bool base_comp_set(ed_instance *inst, u64 entity, const char *comp, de_ep_raw map) {
    u32 k = 0;
    for (u32 i = 0; i < inst->base_count; ++i) {
        const ed_base *b = &inst->base[i];
        if (b->entity == entity &&
            (strcmp(b->path, comp) == 0 || comp_field_of(b->path, comp) != NULL))
            continue;
        inst->base[k++] = *b;
    }
    inst->base_count = k;
    if (is_nil(map))
        return true;
    bool ok = base_append(inst, entity, comp, k_nil, 1);
    mpack_reader_t r;
    mpack_reader_init_data(&r, map.ptr, map.len);
    const u32 count = mpack_expect_map_max(&r, 1024);
    for (u32 i = 0; i < count && ok && mpack_reader_error(&r) == mpack_ok; ++i) {
        de_ep_kv_raw kv;
        if (!de__ed_raw_map_entry(&r, &kv))
            break;
        char path[128];
        snprintf(path, sizeof path, "%s.%.*s", comp, (int)kv.key.len, kv.key.ptr);
        ok = base_append(inst, entity, path, kv.value.ptr, kv.value.len);
    }
    mpack_done_map(&r);
    return mpack_reader_destroy(&r) == mpack_ok && ok;
}

/* ---------- Whole entities (DENG-811) ----------
 *
 * A definition's "" marker of an entity holds its parent's id (nil for a root), so the
 * definition knows its tree. An instance links each prefab entity to a live one through its
 * local pairs; an entity_added is a live entity without a link under one with a link, and an
 * entity_removed is a definition entity whose linked entity is gone. */

#define ED_SUB_MAX 256u

/* The parent id of (d, sid), 0 for a root; false when `d` has no entity `sid`. */
static bool def_parent(const ed_def *d, u64 sid, u64 *parent) {
    const ed_dval *v = def_get(d, sid, "");
    *parent = 0;
    if (v == NULL)
        return false;
    const de_ep_raw raw = dval_raw(d, v);
    if (is_nil(raw))
        return true;
    mpack_reader_t r;
    mpack_reader_init_data(&r, raw.ptr, raw.len);
    const u64 p = mpack_expect_u64(&r);
    if (mpack_reader_destroy(&r) == mpack_ok)
        *parent = p;
    return true;
}

/* The entities of `d` under `sid`, `sid` first and parents before children. `cap` returned
 * means the subtree may be larger. */
static u32 def_subtree(const ed_def *d, u64 sid, u64 *out, u32 cap) {
    u32 n = 0;
    if (cap == 0 || def_get(d, sid, "") == NULL)
        return 0;
    out[n++] = sid;
    for (u32 i = 0; i < n; ++i)
        for (u32 k = 0; k < d->val_count && n < cap; ++k) {
            u64 p = 0;
            const u64 s = d->vals[k].sid;
            if (d->vals[k].path[0] != '\0' || !def_parent(d, s, &p) || p != out[i])
                continue;
            bool seen = false;
            for (u32 j = 0; j < n && !seen; ++j)
                seen = out[j] == s;
            if (!seen)
                out[n++] = s;
        }
    return n;
}

/* The live entity the instance links `sid` to; false when there is none, or it is gone. */
static bool linked_alive(de__editor *ed, const ed_instance *inst, u64 sid, u64 *entity) {
    for (u32 i = 0; i < inst->local_count; ++i)
        if (inst->local_sid[i] == sid &&
            de_entity_alive(ed->e->world, (de_entity){inst->local_ent[i]})) {
            *entity = inst->local_ent[i];
            return true;
        }
    return false;
}

/* (d, sid)'s components, {Comp: fields}, from its "Component" markers. */
static bool def_comps(de_ep_arena *a, const ed_def *d, u64 sid, de_ep_raw *out) {
    u32 n = 0;
    for (u32 i = 0; i < d->val_count; ++i)
        n += d->vals[i].sid == sid && is_comp_path(d->vals[i].path);
    de_ep_str *keys = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (n + 1));
    de_ep_raw *vals = (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * (n + 1));
    if (keys == NULL || vals == NULL)
        return false;
    u32 k = 0;
    for (u32 i = 0; i < d->val_count && k < n; ++i) {
        const ed_dval *v = &d->vals[i];
        if (v->sid != sid || !is_comp_path(v->path))
            continue;
        keys[k] = de_ep_cstr(v->path);
        if (!def_comp_map(a, d, sid, v->path, &vals[k++]))
            return false;
    }
    return map_build(a, keys, vals, k, out);
}

/* `comps` ({Comp: fields}) with the entity ids of its fields mapped `from` -> `to`. */
static bool comps_remap(de__editor *ed, de_ep_arena *a, de_ep_raw comps, const u64 *from,
                        const u64 *to, u32 n, de_ep_raw *out) {
    if (from == NULL) {
        *out = comps;
        return true;
    }
    mpack_reader_t r;
    mpack_reader_init_data(&r, comps.ptr, comps.len);
    const u32 count = mpack_expect_map_max(&r, 1024);
    de_ep_str *keys = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (count + 1));
    de_ep_raw *vals = (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * (count + 1));
    if (keys == NULL || vals == NULL)
        mpack_reader_flag_error(&r, mpack_error_memory);
    bool ok = true;
    for (u32 i = 0; i < count && ok && mpack_reader_error(&r) == mpack_ok; ++i) {
        de_ep_kv_raw kv;
        if (!de__ed_raw_map_entry(&r, &kv))
            break;
        char comp[128];
        if (kv.key.len >= sizeof comp) {
            ok = false;
            break;
        }
        memcpy(comp, kv.key.ptr, kv.key.len);
        comp[kv.key.len] = '\0';
        keys[i] = kv.key;
        ok = comp_remap(ed, a, comp, kv.value, from, to, n, &vals[i]);
    }
    mpack_done_map(&r);
    if (mpack_reader_destroy(&r) != mpack_ok || !ok)
        return false;
    return map_build(a, keys, vals, count, out);
}

/* `comps` as the key-value list a spawn op takes (borrowing `comps`). */
static bool comps_split(de_ep_arena *a, de_ep_raw comps, de_ep_map_raw *out) {
    mpack_reader_t r;
    mpack_reader_init_data(&r, comps.ptr, comps.len);
    const u32 count = mpack_expect_map_max(&r, 1024);
    de_ep_kv_raw *kv = (de_ep_kv_raw *)de__ed_alloc(a, sizeof(de_ep_kv_raw) * (count + 1));
    if (kv == NULL)
        mpack_reader_flag_error(&r, mpack_error_memory);
    for (u32 i = 0; i < count && mpack_reader_error(&r) == mpack_ok; ++i)
        if (!de__ed_raw_map_entry(&r, &kv[i]))
            break;
    mpack_done_map(&r);
    if (mpack_reader_destroy(&r) != mpack_ok)
        return false;
    *out = (de_ep_map_raw){kv, count};
    return true;
}

/* The components of a live entity that an override can name, as {Comp: fields}. */
static bool entity_comps(de__editor *ed, de_ep_arena *a, u64 entity, de_ep_raw *out) {
    de_ep_entity_row row;
    if (!de__ed_entity_row(ed, a, (de_entity){entity}, NULL, NULL, &row))
        return false;
    const u32 n = row.components.count;
    de_ep_str *keys = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (n + 1));
    de_ep_raw *vals = (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * (n + 1));
    if (keys == NULL || vals == NULL)
        return false;
    u32 k = 0;
    for (u32 i = 0; i < n; ++i) {
        const de_ep_kv_raw *kv = &row.components.items[i];
        char name[128];
        if (kv->key.len >= sizeof name)
            continue;
        memcpy(name, kv->key.ptr, kv->key.len);
        name[kv->key.len] = '\0';
        if (!comp_name_ok(name))
            continue;
        keys[k] = kv->key;
        vals[k++] = kv->value;
    }
    return map_build(a, keys, vals, k, out);
}

/* Frees the instance base and builds it again from the live tree and the definition. */
static void instance_rebuild(de__editor *ed, ed_instance *inst, const de_ep_arena *a) {
    const de_allocator *al = de_heap_allocator();
    if (inst->base != NULL)
        DE_FREE(al, inst->base, sizeof(ed_base) * inst->base_cap);
    if (inst->bytes != NULL)
        DE_FREE(al, inst->bytes, inst->byte_count);
    inst->base = NULL;
    inst->bytes = NULL;
    inst->base_count = inst->base_cap = 0;
    inst->byte_count = 0;
    const size_t at = (a->used + 15u) & ~(size_t)15u;
    const de_ep_arena scratch = {a->base + at, at < a->size ? a->size - at : 0, 0};
    link_base(ed, inst, &scratch);
}

/* ---------- Prefab reload (DENG-803) ----------
 *
 * When a prefab asset is reloaded, its definition and the loaded definitions that link to it
 * (variants, nesting, transitively) are read again. Definitions whose values and links come out
 * unchanged stay as they are: that is what a reload of the editor's own prefab.state write looks
 * like, so it neither bumps the revision again nor touches the instances. When any changed, the
 * prefab revision goes up (clients holding the old one get CONFLICT "stale"), and each live
 * instance of a changed prefab takes the new value of every field it did not override; an
 * override stays an override (its entity value is kept, the instance base moves). Entities are
 * not added or removed, and nothing goes on the undo stack. */

#define ED_DEFS_MAX 256u

static bool def_same(const ed_def *x, const ed_def *y) {
    if (strcmp(x->base, y->base) != 0 || x->nest_count != y->nest_count ||
        x->val_count != y->val_count)
        return false;
    for (u32 k = 0; k < x->nest_count; ++k) {
        const ed_nest *a = &x->nest[k], *b = &y->nest[k];
        if (strcmp(a->asset, b->asset) != 0 || a->root != b->root || a->count != b->count ||
            (a->count != 0 && (memcmp(a->inner, b->inner, sizeof(u64) * a->count) != 0 ||
                               memcmp(a->outer, b->outer, sizeof(u64) * a->count) != 0)))
            return false;
    }
    for (u32 i = 0; i < x->val_count; ++i) {
        const ed_dval *w = def_get(y, x->vals[i].sid, x->vals[i].path);
        if (w == NULL || !raw_eq(dval_raw(x, &x->vals[i]), dval_raw(y, w)))
            return false;
    }
    return true;
}

/* `inst` takes the values of its prefab's new definition `d` for the fields it did not override. */
static void instance_refresh(de__editor *ed, ed_instance *inst, const ed_def *d) {
    if (inst->local_count == 0)
        return;
    for (u32 i = 0; i < d->val_count; ++i) {
        const ed_dval *v = &d->vals[i];
        u64 entity = 0;
        if (!local_ent_of(inst, v->sid, &entity))
            continue;
        ed_base *b = base_find(inst, entity, de_ep_cstr(v->path));
        if (b == NULL)
            continue;
        ed_kind kind = {0};
        if (!path_field(ed, v->path, &kind))
            continue;
        de_ep_arena a = {ed->arena, ed->arena_cap, 0};
        de_ep_raw nv;
        if (!remap(&a, kind, dval_raw(d, v), inst->local_sid, inst->local_ent, inst->local_count,
                   &nv) ||
            raw_eq(nv, base_raw(inst, b)))
            continue;
        de_ep_raw cur;
        if (current_value(ed, &a, entity, v->path, &cur) && raw_eq(cur, base_raw(inst, b)))
            (void)de__ed_set_value(ed, &a, entity, v->path, nv);
        (void)base_put(inst, b, nv.ptr, nv.len);
    }
}

/* The hash of `asset`'s source file as it is now. False when it can't be read. */
static bool source_hash(de__editor *ed, const char *asset, u64 *out) {
    void *src = NULL;
    u64 size = 0;
    if (!de__ed_asset_source(ed, asset, &src, &size))
        return false;
    *out = de_hash64(src, (size_t)size, 0);
    DE_FREE(de_heap_allocator(), src, size + 1);
    return true;
}

void de__ed_prefab_reloaded(de__editor *ed, de_asset_id id) {
    if (ed->e->assets == NULL || g_prefab.count == 0)
        return;
    u8 mark[ED_DEFS_MAX];
    memset(mark, 0, sizeof mark);
    const u32 n = g_prefab.count < ED_DEFS_MAX ? g_prefab.count : ED_DEFS_MAX;
    bool any = false;
    for (u32 i = 0; i < n; ++i)
        if (de_asset_id_from(g_prefab.defs[i]->asset).hash == id.hash)
            mark[i] = any = true;
    if (!any)
        return;
    for (bool grew = true; grew;) { /* the dependents, transitively */
        grew = false;
        for (u32 i = 0; i < n; ++i) {
            const ed_def *d = g_prefab.defs[i];
            for (u32 j = 0; j < n && !mark[i]; ++j) {
                if (!mark[j])
                    continue;
                bool dep = d->base[0] != '\0' && strcmp(d->base, g_prefab.defs[j]->asset) == 0;
                for (u32 k = 0; k < d->nest_count && !dep; ++k)
                    dep = strcmp(d->nest[k].asset, g_prefab.defs[j]->asset) == 0;
                if (dep)
                    mark[i] = grew = true;
            }
        }
    }
    u8 changed[ED_DEFS_MAX];
    memset(changed, 0, sizeof changed);
    bool any_changed = false;
    for (u32 i = 0; i < n; ++i) {
        if (!mark[i])
            continue;
        ed_def *d = g_prefab.defs[i];
        /* A reload runs on a snapshot of the file and its event can come late: after the editor
         * wrote the file again (undo, redo) the cooked asset is older than the definition. When
         * the source is the one the definition came from, there is nothing new to read. */
        u64 now = 0;
        const bool have_now = source_hash(ed, d->asset, &now);
        if (have_now && d->src_known && d->src_hash == now)
            continue;
        ed_def fresh;
        memset(&fresh, 0, sizeof fresh);
        snprintf(fresh.asset, sizeof fresh.asset, "%s", d->asset);
        def_fill(ed, &fresh);
        if (def_same(d, &fresh)) {
            d->src_known = have_now;
            d->src_hash = now;
            def_free_inner(&fresh);
            continue;
        }
        fresh.src_known = have_now;
        fresh.src_hash = now;
        def_free_inner(d);
        *d = fresh;
        changed[i] = any_changed = true;
    }
    if (!any_changed)
        return;
    for (u32 i = 0; i < n; ++i) /* a link may name a prefab that was not loaded yet */
        if (changed[i]) {
            ed_def *d = g_prefab.defs[i];
            if (d->base[0] != '\0')
                (void)prefab_def_load(ed, d->base, 1);
            for (u32 k = 0; k < d->nest_count; ++k)
                (void)prefab_def_load(ed, d->nest[k].asset, 1);
        }
    ++g_prefab.revision;
    for (u32 q = 0; q < g_scene.instance_count; ++q) {
        ed_instance *inst = &g_scene.instances[q];
        for (u32 i = 0; i < n; ++i)
            if (changed[i] && strcmp(g_prefab.defs[i]->asset, inst->asset) == 0)
                instance_refresh(ed, inst, g_prefab.defs[i]);
    }
    de__ed_world_touch(ed);
}

/* {id, error: {code, message, data: {reason, revision}}} */
static void prefab_error(mpack_writer_t *w, u64 id, de_ep_error code, const char *reason,
                         const char *fmt, ...) DE_PRINTF_FMT(5, 6);
static void prefab_error(mpack_writer_t *w, u64 id, de_ep_error code, const char *reason,
                         const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    mpack_start_map(w, 2);
    mpack_write_cstr(w, "id");
    mpack_write_u64(w, id);
    mpack_write_cstr(w, "error");
    mpack_start_map(w, 3);
    mpack_write_cstr(w, "code");
    mpack_write_i32(w, (int32_t)code);
    mpack_write_cstr(w, "message");
    mpack_write_cstr(w, msg);
    mpack_write_cstr(w, "data");
    de_ep_write_prefab_error_data(
        w, &(const de_ep_prefab_error_data){de_ep_cstr(reason), g_prefab.revision});
    mpack_finish_map(w);
    mpack_finish_map(w);
}

/* A malformed path is INVALID_PARAMS; one that names no field is NOT_FOUND. */
static bool path_check(de__editor *ed, const de_ep_request *req, mpack_writer_t *w,
                       const char *path, de_ep_str raw, ed_kind *entity) {
    if (path == NULL || raw.len == 0 || raw.len >= sizeof(((ed_base *)0)->path) ||
        strchr(path, '.') == NULL || strlen(path) != raw.len) {
        prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "path", "bad path %.*s",
                     (int)raw.len, raw.ptr);
        return false;
    }
    if (!path_field(ed, path, entity)) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no_field", "no field %s", path);
        return false;
    }
    return true;
}

/* prefab.overrides: the overrides of the entity's subtree in its instance. Protocol 1.18 adds
 * `entity`, `base`, `source`, `chain` and `ids` to each entry, and `asset`, `root` and
 * `revision` to the list; 1.17 readers skip them. */
static void on_overrides(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(prefab_overrides, de_ep_entity_ref, q);
    ed_instance *inst = instance_of(ed, q.entity);
    if (inst == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "not_instance",
                     "%llu is not a prefab instance", (unsigned long long)q.entity);
        return;
    }
    u64 tree[1024];
    const u32 tn = tree_of(ed, (de_entity){q.entity}, tree, 1024);
    ed_def *def = def_find(inst->asset);
    /* The component rows of the subtree's instance entities, for component_added (DENG-810). */
    de_ep_entity_row *ents = tn ? (de_ep_entity_row *)de__ed_alloc(a, sizeof *ents * tn) : NULL;
    u32 added = 0;
    for (u32 k = 0; k < tn && ents != NULL; ++k) {
        u64 sid = 0;
        if (!local_sid_of(inst, tree[k], &sid) ||
            !de__ed_entity_row(ed, a, (de_entity){tree[k]}, NULL, NULL, &ents[k]))
            ents[k] = (de_ep_entity_row){0};
        added += ents[k].components.count;
    }
    const u32 count = inst->base_count + added + tn + (def != NULL ? def->val_count : 0);
    typedef struct pf_row {
        u64 entity;
        const char *path;
        de_ep_raw cur, base;
        bool structural;
        de_ep_override_kind kind;
        ed_level lv[ED_CHAIN_MAX];
        u32 levels, source;
        u64 parent, local; /* entity_added / entity_removed (DENG-811) */
        bool has_local;
    } pf_row;
    pf_row *rows = count ? (pf_row *)de__ed_alloc(a, sizeof(pf_row) * count) : NULL;
    if (count && (rows == NULL || (tn && ents == NULL))) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    u32 n = 0;
    for (u32 i = 0; i < inst->base_count && rows != NULL; ++i) {
        const ed_base *b = &inst->base[i];
        bool in_tree = false;
        for (u32 k = 0; k < tn && !in_tree; ++k)
            in_tree = tree[k] == b->entity;
        u64 bsid = 0;
        if (!in_tree || !local_sid_of(inst, b->entity, &bsid))
            continue;
        if (strchr(b->path, '.') == NULL) {
            /* A component marker: component_removed when the entity no longer has it. */
            de_ep_raw cur, base;
            u64 sid = 0;
            if (!comp_name_ok(b->path) || !current_comp(ed, a, b->entity, b->path, &cur) ||
                !is_nil(cur) || !base_comp_map(a, inst, b->entity, b->path, &base))
                continue;
            pf_row *row = &rows[n++];
            *row = (pf_row){.entity = b->entity, .path = b->path, .cur = cur, .base = base,
                            .structural = true, .kind = DE_EP_OVERRIDE_KIND_COMPONENT_REMOVED};
            row->levels = local_sid_of(inst, b->entity, &sid) ? chain_of(def, sid, "", row->lv) : 0;
            while (row->source < row->levels &&
                   def_get(row->lv[row->source].d, row->lv[row->source].sid, b->path) == NULL)
                ++row->source;
            continue;
        }
        /* The hierarchy follows the entities (DENG-811): adding or removing a child is an
         * entity override, not a change to its parent's DeChildren. */
        char comp[128];
        snprintf(comp, sizeof comp, "%.*s", (int)(strchr(b->path, '.') - b->path), b->path);
        de_ep_raw cur;
        if (is_hierarchy_name(comp) || !current_value(ed, a, b->entity, b->path, &cur) ||
            raw_eq(cur, base_raw(inst, b)))
            continue;
        pf_row *row = &rows[n++];
        *row = (pf_row){.entity = b->entity, .path = b->path, .cur = cur, .base = base_raw(inst, b)};
        u64 sid = 0;
        ed_kind entity = {0};
        row->levels = local_sid_of(inst, b->entity, &sid) && path_field(ed, b->path, &entity)
                          ? chain_of(def, sid, b->path, row->lv)
                          : 0;
        row->source = 0;
        while (row->source < row->levels &&
               !level_owns(a, entity, row->lv, row->levels, row->source, b->path))
            ++row->source;
    }
    /* component_added: an instance entity has a component its base does not. */
    for (u32 k = 0; k < tn && rows != NULL; ++k)
        for (u32 c = 0; c < ents[k].components.count; ++c) {
            const de_ep_kv_raw *kv = &ents[k].components.items[c];
            char name[128];
            if (kv->key.len >= sizeof name)
                continue;
            memcpy(name, kv->key.ptr, kv->key.len);
            name[kv->key.len] = '\0';
            u64 sid = 0;
            if (!comp_name_ok(name) || base_find(inst, tree[k], kv->key) != NULL ||
                !local_sid_of(inst, tree[k], &sid))
                continue;
            char *path = (char *)de__ed_alloc(a, kv->key.len + 1);
            if (path == NULL)
                continue;
            memcpy(path, name, kv->key.len + 1);
            pf_row *row = &rows[n++];
            *row = (pf_row){.entity = tree[k], .path = path, .cur = kv->value,
                            .base = (de_ep_raw){k_nil, 1}, .structural = true,
                            .kind = DE_EP_OVERRIDE_KIND_COMPONENT_ADDED};
            row->levels = chain_of(def, sid, "", row->lv);
        }
    /* entity_added: a live entity without a prefab id under one with an id (DENG-811). Only the
     * topmost is listed; its children come and go with it. */
    for (u32 k = 0; k < tn && rows != NULL; ++k) {
        u64 sid = 0, psid = 0;
        const de_entity p = de_entity_parent(ed->e->world, (de_entity){tree[k]});
        if (local_sid_of(inst, tree[k], &sid) || p.bits == 0 || !local_sid_of(inst, p.bits, &psid))
            continue;
        de_ep_raw comps;
        if (!entity_comps(ed, a, tree[k], &comps))
            continue;
        pf_row *row = &rows[n++];
        *row = (pf_row){.entity = tree[k], .path = "", .cur = comps,
                        .base = (de_ep_raw){k_nil, 1}, .structural = true,
                        .kind = DE_EP_OVERRIDE_KIND_ENTITY_ADDED, .parent = p.bits};
        row->levels = chain_of(def, psid, "", row->lv);
    }
    /* entity_removed: a definition entity whose parent is linked and alive, but it is not. */
    for (u32 k = 0; k < tn && rows != NULL && def != NULL; ++k) {
        u64 psid = 0;
        if (!local_sid_of(inst, tree[k], &psid))
            continue;
        for (u32 i = 0; i < def->val_count; ++i) {
            const ed_dval *v = &def->vals[i];
            u64 parent = 0, alive = 0;
            if (v->path[0] != '\0' || !def_parent(def, v->sid, &parent) || parent != psid ||
                v->sid == psid || linked_alive(ed, inst, v->sid, &alive))
                continue;
            de_ep_raw raw, base;
            if (!def_comps(a, def, v->sid, &raw))
                continue;
            if (!comps_remap(ed, a, raw, inst->local_sid, inst->local_ent, inst->local_count,
                             &base))
                base = raw;
            pf_row *row = &rows[n++];
            *row = (pf_row){.entity = tree[k], .path = "", .cur = (de_ep_raw){k_nil, 1},
                            .base = base, .structural = true,
                            .kind = DE_EP_OVERRIDE_KIND_ENTITY_REMOVED, .parent = tree[k],
                            .local = v->sid, .has_local = true};
            row->levels = chain_of(def, v->sid, "", row->lv);
        }
    }
    de_ep_override *out = n ? (de_ep_override *)de__ed_alloc(a, sizeof(de_ep_override) * n) : NULL;
    if (n && out == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    for (u32 i = 0; i < n; ++i) {
        const pf_row *row = &rows[i];
        de_ep_str *chain =
            row->levels ? (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * row->levels) : NULL;
        u64 *ids = row->levels ? (u64 *)de__ed_alloc(a, sizeof(u64) * row->levels) : NULL;
        if (row->levels && (chain == NULL || ids == NULL)) {
            prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
            return;
        }
        for (u32 k = 0; k < row->levels; ++k) {
            chain[k] = de_ep_cstr(row->lv[k].d->asset);
            ids[k] = row->lv[k].sid;
        }
        out[i] = (de_ep_override){
            .path = de_ep_cstr(row->path),
            .value = row->cur,
            .entity = row->entity,
            .base = row->base,
            .source = de_ep_cstr(row->source < row->levels ? row->lv[row->source].d->asset
                                                           : inst->asset),
            .chain = {chain, row->levels},
            .ids = {ids, row->levels},
            .has_kind = row->structural,
            .kind = row->kind,
            .has_parent = row->parent != 0,
            .parent = row->parent,
            .has_local = row->has_local,
            .local = row->local,
        };
    }
    const de_ep_override_list res = {
        .overrides = {out, n},
        .asset = de_ep_cstr(inst->asset),
        .root = inst->root,
        .revision = g_prefab.revision,
    };
    de_ep_write_response_prefab_overrides(w, req->id, &res);
}

/* ---------- Prefab source files (DENG-798) ----------
 *
 * A prefab.state op that changes definitions also writes their `*.prefab.toml` sources, before
 * anything changes in memory. Its inverse carries each file's old bytes and a hash of the bytes
 * written, so undo and redo restore exact bytes, and refuse ("source_changed") when the file was
 * changed outside the editor in between. Every file is probed for writing first; a failed write
 * puts back the files already written. */

#define ED_FILES_MAX 128u

static struct {
    const char *reason; /* why the last prefab.state failed, for prefab_error */
} g_files;

const char *de__ed_prefab_fail_reason(void) {
    return g_files.reason;
}

/* A planned definition change, and a planned instance base change. */
typedef struct pf_dchg {
    ed_def *d;
    u64 sid;
    de_ep_raw old, val;
    bool follow; /* a dependent that inherits the field (DENG-798) */
} pf_dchg;

typedef struct pf_bchg {
    ed_instance *inst;
    u64 entity;
    de_ep_raw val;
    bool set; /* the instance had the old value: it takes the new one */
} pf_bchg;

#define ED_PLAN_MAX 128u

static bool plan_has(const pf_dchg *c, u32 n, const ed_def *d, u64 sid) {
    for (u32 i = 0; i < n; ++i)
        if (c[i].d == d && c[i].sid == sid)
            return true;
    return false;
}

static u64 instance_scene(u64 root) {
    u64 scene = 0, sid = 0;
    return de__ed_scene_member(root, &scene, &sid) ? scene : ED_GAME_SCENE;
}

static bool reject_state(de__editor *ed, u64 id, mpack_writer_t *w, bool has_revision,
                         u64 revision) {
    if (ed->mode != DE_EP_WORLD_MODE_EDIT) {
        prefab_error(w, id, DE_EP_ERR_WRONG_MODE, "play_mode",
                     "prefabs can't change during a play session");
        return true;
    }
    if (has_revision && revision != g_prefab.revision) {
        prefab_error(w, id, DE_EP_ERR_CONFLICT, "stale",
                     "prefab revision %llu is not the current %llu", (unsigned long long)revision,
                     (unsigned long long)g_prefab.revision);
        return true;
    }
    return false;
}

static bool reject_request(de__editor *ed, const de_ep_request *req, mpack_writer_t *w,
                           const de_ep_override_path_params *q) {
    return reject_state(ed, req->id, w, q->has_revision, q->revision);
}

/* ---------- Dependents the editor has not loaded (DENG-807) ----------
 *
 * The apply plan follows the definitions the editor holds. Before the plan is built, every
 * manifest prefab whose base or nested chain reaches the target is loaded, so a variant or a
 * nesting prefab that was never spawned follows the base, and its source is written too. A
 * prefab that is not loaded yet is read for its links only. One whose source can't be read may
 * be a dependent, so the apply is refused ("dependent_unreadable"); so is a cycle of links among
 * the dependents ("dependent_cycle"). Both are found before anything is written. */

#define ED_DEPS_MAX 4096u

typedef struct pf_node {
    const char *asset;
    const ed_def *d; /* the loaded definition, or &own */
    ed_def own;      /* the links of a prefab that is not loaded yet */
    bool reach, left;
} pf_node;

static u32 node_find(const pf_node *nodes, u32 n, const char *asset) {
    for (u32 i = 0; i < n; ++i)
        if (strcmp(nodes[i].asset, asset) == 0)
            return i;
    return n;
}

/* Whether node `i` links to a node that reaches the target (and, with `live`, is not `left`). */
static bool node_links(const pf_node *nodes, u32 n, u32 i, bool live) {
    const ed_def *d = nodes[i].d;
    for (u32 k = 0; k <= d->nest_count; ++k) {
        const char *to = k < d->nest_count ? d->nest[k].asset : d->base;
        const u32 j = to[0] != '\0' ? node_find(nodes, n, to) : n;
        if (j < n && nodes[j].reach && !(live && nodes[j].left))
            return true;
    }
    return false;
}

static void nodes_free(pf_node *nodes, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (nodes[i].d == &nodes[i].own)
            def_free_inner(&nodes[i].own);
    DE_FREE(de_heap_allocator(), nodes, sizeof(pf_node) * n);
}

/* Loads the definitions of the manifest prefabs whose links reach `target`. False, with the
 * error written, when the apply must not go on. */
static bool load_dependents(de__editor *ed, const de_ep_request *req, mpack_writer_t *w,
                            const char *target) {
    u32 n = 0;
    while (n <= ED_DEPS_MAX && de__ed_asset_nth(ed, "prefab", n) != NULL)
        ++n;
    if (n > ED_DEPS_MAX) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large", "the project has too many prefabs");
        return false;
    }
    if (n == 0)
        return true;
    pf_node *nodes =
        (pf_node *)DE_ALLOC(de_heap_allocator(), sizeof(pf_node) * n, 8, DE_MEM_GENERAL);
    if (nodes == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of memory");
        return false;
    }
    memset(nodes, 0, sizeof(pf_node) * n);
    for (u32 i = 0; i < n; ++i) {
        pf_node *nd = &nodes[i];
        nd->asset = de__ed_asset_nth(ed, "prefab", i);
        nd->d = def_find(nd->asset);
        if (nd->d != NULL)
            continue;
        nd->d = &nd->own;
        snprintf(nd->own.asset, sizeof nd->own.asset, "%s", nd->asset);
        if (!def_links(ed, &nd->own)) {
            const char *src = de__ed_asset_source_path(ed, nd->asset);
            prefab_error(w, req->id, DE_EP_ERR_INTERNAL, "dependent_unreadable",
                         "can't read %s (%s): it may be a variant or nesting prefab of %s",
                         src != NULL ? src : "no source", nd->asset, target);
            nodes_free(nodes, n);
            return false;
        }
    }
    const u32 t = node_find(nodes, n, target);
    if (t == n) {
        nodes_free(nodes, n);
        return true;
    }
    nodes[t].reach = true;
    for (bool grew = true; grew;) {
        grew = false;
        for (u32 i = 0; i < n; ++i)
            if (!nodes[i].reach && node_links(nodes, n, i, false))
                nodes[i].reach = grew = true;
    }
    /* Peel off the dependents that link to no other remaining one; what stays is a cycle. */
    for (bool peeled = true; peeled;) {
        peeled = false;
        for (u32 i = 0; i < n; ++i)
            if (nodes[i].reach && !nodes[i].left && !node_links(nodes, n, i, true))
                nodes[i].left = peeled = true;
    }
    for (u32 i = 0; i < n; ++i)
        if (nodes[i].reach && !nodes[i].left) {
            prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "dependent_cycle",
                         "%s is in a cycle of prefab links that reaches %s", nodes[i].asset,
                         target);
            nodes_free(nodes, n);
            return false;
        }
    for (u32 i = 0; i < n; ++i) {
        if (!nodes[i].reach || i == t)
            continue;
        const ed_def *d = prefab_def_load(ed, nodes[i].asset, 0);
        if (d == NULL || !d->ready) {
            prefab_error(w, req->id, d == NULL ? DE_EP_ERR_BUSY : DE_EP_ERR_INTERNAL,
                         d == NULL ? "memory" : "dependent_unreadable",
                         "can't load %s, a variant or nesting prefab of %s", nodes[i].asset,
                         target);
            nodes_free(nodes, n);
            return false;
        }
    }
    nodes_free(nodes, n);
    return true;
}

/* prefab.applyOverride: the instance's value becomes the value of `target` (default: the
 * instance's prefab) and of every level between. The change flows on to the variants and
 * nesting prefabs that did not set the field themselves, and to every instance that had the old
 * value. All of it is one undoable transaction. */
/* The rest of an apply: the prefab.state op that writes `dc` and `bc` (`path` is the field or
 * the whole component) goes in ops[0], ahead of the instance edits in ops[1..no), and the
 * result lists the prefabs, their files and the instance entities. */
static void apply_finish(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w, const de_ep_override_path_params *q, u64 root,
                         const char *path, const pf_dchg *dc, u32 nd, const pf_bchg *bc, u32 nb,
                         de_ep_edit_op *ops, u32 no) {
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 2);
    mpack_write_cstr(&mw, "defs");
    mpack_start_array(&mw, nd);
    for (u32 i = 0; i < nd; ++i) {
        mpack_start_map(&mw, 5);
        mpack_write_cstr(&mw, "asset");
        mpack_write_cstr(&mw, dc[i].d->asset);
        mpack_write_cstr(&mw, "sid");
        mpack_write_u64(&mw, dc[i].sid);
        mpack_write_cstr(&mw, "path");
        mpack_write_cstr(&mw, path);
        mpack_write_cstr(&mw, "value");
        mpack_write_object_bytes(&mw, dc[i].val.ptr, dc[i].val.len);
        mpack_write_cstr(&mw, "follow");
        mpack_write_bool(&mw, dc[i].follow);
        mpack_finish_map(&mw);
    }
    mpack_finish_array(&mw);
    mpack_write_cstr(&mw, "bases");
    mpack_start_array(&mw, nb);
    for (u32 i = 0; i < nb; ++i) {
        mpack_start_map(&mw, 4);
        mpack_write_cstr(&mw, "root");
        mpack_write_u64(&mw, bc[i].inst->root);
        mpack_write_cstr(&mw, "entity");
        mpack_write_u64(&mw, bc[i].entity);
        mpack_write_cstr(&mw, "path");
        mpack_write_cstr(&mw, path);
        mpack_write_cstr(&mw, "value");
        mpack_write_object_bytes(&mw, bc[i].val.ptr, bc[i].val.len);
        mpack_finish_map(&mw);
    }
    mpack_finish_array(&mw);
    mpack_finish_map(&mw);
    ops[0].op = de_ep_cstr("prefab.state");
    ops[0].has_value = true;
    ops[0].value = de__ed_raw_end(a, &mw);
    if (ops[0].value.len == 0) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    /* The edit reuses the scratch arena: keep what the result reports. */
    const char *names[ED_PLAN_MAX];
    u64 ents[ED_PLAN_MAX];
    for (u32 i = 0; i < nd; ++i)
        names[i] = dc[i].d->asset;
    for (u32 i = 0; i < nb; ++i)
        ents[i] = bc[i].entity;
    de_ep_error code = DE_EP_OK;
    char msg[256];
    g_files.reason = NULL;
    const u32 tx = de__ed_edit_run(ed, a, q->has_tx ? q->tx : 0, instance_scene(root), ops, no, &code, msg, sizeof msg);
    if (tx == 0) {
        prefab_error(w, req->id, code, g_files.reason ? g_files.reason : "edit", "%s", msg);
        return;
    }
    /* `files` (protocol 1.19) is the source path of each entry of `prefabs`, in the same order:
     * the manifest's, as listed in the manifest ("crate.prefab.toml"). */
    de_ep_str pnames[ED_PLAN_MAX], fnames[ED_PLAN_MAX];
    for (u32 i = 0; i < nd; ++i) {
        const char *src = de__ed_asset_source_path(ed, names[i]);
        pnames[i] = de_ep_cstr(names[i]);
        fnames[i] = de_ep_cstr(src != NULL ? src : names[i]);
    }
    const de_ep_apply_result res = {
        .tx = tx,
        .revision = g_prefab.revision,
        .prefabs = {pnames, nd},
        .entities = {ents, nb},
        .files = {fnames, nd},
    };
    de_ep_write_response_prefab_apply_override(w, req->id, &res);
}

/* ---------- Structural overrides: whole components (DENG-810) ---------- */

/* What a component_added or component_removed request names, checked in the protocol's order. */
typedef struct pf_sreq {
    ed_instance *inst;
    char comp[128];
    de_ep_raw cur; /* the instance's map, or nil (removed) */
    ed_level lv[ED_CHAIN_MAX];
    u32 levels;
} pf_sreq;

static bool structural_check(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w, const de_ep_override_path_params *q,
                             pf_sreq *s) {
    memset(s, 0, sizeof *s);
    const bool added = q->kind == DE_EP_OVERRIDE_KIND_COMPONENT_ADDED;
    if (q->path.len >= sizeof s->comp || memchr(q->path.ptr, '\0', q->path.len) != NULL) {
        prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "path", "bad path %.*s",
                     (int)q->path.len, q->path.ptr);
        return false;
    }
    if (q->path.len == 0 || memchr(q->path.ptr, '.', q->path.len) != NULL) {
        prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "kind",
                     "%s takes a component name, not %.*s", de_ep_override_kind_name(q->kind),
                     (int)q->path.len, q->path.ptr);
        return false;
    }
    memcpy(s->comp, q->path.ptr, q->path.len);
    s->comp[q->path.len] = '\0';
    if (de__ed_component(ed, q->path).id == 0) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no_component", "no component type %s",
                     s->comp);
        return false;
    }
    s->inst = instance_of(ed, q->entity);
    if (s->inst == NULL || !current_comp(ed, a, q->entity, s->comp, &s->cur)) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "not_instance",
                     "%llu is not a prefab instance", (unsigned long long)q->entity);
        return false;
    }
    const bool has = !is_nil(s->cur);
    const bool marked = base_find(s->inst, q->entity, q->path) != NULL;
    if (!comp_name_ok(s->comp) || (added ? !has || marked : has || !marked)) {
        prefab_error(w, req->id, DE_EP_ERR_CONFLICT, "not_override", "%s is not %s on %llu",
                     s->comp, added ? "added" : "removed", (unsigned long long)q->entity);
        return false;
    }
    u64 sid = 0;
    s->levels = local_sid_of(s->inst, q->entity, &sid)
                    ? chain_of(def_find(s->inst->asset), sid, "", s->lv)
                    : 0;
    if (s->levels == 0) {
        prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no_definition",
                     "no prefab definition of %llu", (unsigned long long)q->entity);
        return false;
    }
    return true;
}

static void apply_structural(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w, const de_ep_override_path_params *q) {
    pf_sreq s;
    if (!structural_check(ed, req, a, w, q, &s))
        return;
    ed_instance *inst = s.inst;
    const char *comp = s.comp;
    const ed_level *lv = s.lv;
    u32 t = 0;
    if (q->has_target && q->target.len > 0) {
        while (t < s.levels && !de_ep_str_eq(q->target, lv[t].d->asset))
            ++t;
        if (t == s.levels) {
            prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "target",
                         "%.*s is not in the prefab chain of %llu", (int)q->target.len,
                         q->target.ptr, (unsigned long long)q->entity);
            return;
        }
    }
    if (!load_dependents(ed, req, w, lv[t].d->asset))
        return;
    pf_dchg *dc = (pf_dchg *)de__ed_alloc(a, sizeof(pf_dchg) * ED_PLAN_MAX);
    pf_bchg *bc = (pf_bchg *)de__ed_alloc(a, sizeof(pf_bchg) * ED_PLAN_MAX);
    bool *rm = (bool *)de__ed_alloc(a, sizeof(bool) * ED_PLAN_MAX);
    if (dc == NULL || bc == NULL || rm == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    /* The levels down to the target take the instance's component, in each level's own ids;
     * a level that already matches stays as it is. */
    u32 nd = 0, nb = 0;
    de_ep_raw v;
    if (!comp_remap(ed, a, comp, s.cur, inst->local_ent, inst->local_sid, inst->local_count,
                    &v)) {
        prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "outside_prefab",
                     "%s refers to an entity outside the prefab", comp);
        return;
    }
    for (u32 k = 0; k <= t; ++k) {
        if (k > 0 && !comp_down_one(ed, a, comp, lv, k - 1, v, &v)) {
            prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "outside_prefab",
                         "%s refers to an entity outside %s", comp, lv[k].d->asset);
            return;
        }
        de_ep_raw old;
        if (!def_comp_map(a, lv[k].d, lv[k].sid, comp, &old)) {
            prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
            return;
        }
        if (!raw_eq(old, v))
            dc[nd++] = (pf_dchg){lv[k].d, lv[k].sid, old, v, false};
    }
    /* Dependents that inherit the component as it was follow, level by level. */
    for (u32 i = 0; i < nd; ++i) {
        const pf_dchg c = dc[i];
        for (u32 di = 0; di < g_prefab.count; ++di) {
            ed_def *e = g_prefab.defs[di];
            for (u32 vi = 0; vi < e->val_count && e != c.d; ++vi) {
                const ed_dval *ev = &e->vals[vi];
                ed_level nx;
                if (ev->path[0] != '\0' || !def_next(e, ev->sid, &nx) || nx.d != c.d ||
                    nx.sid != c.sid || plan_has(dc, nd, e, ev->sid))
                    continue;
                const ed_nest *via = nx.via;
                de_ep_raw own, mine, next;
                if (!def_comp_map(a, e, ev->sid, comp, &own) ||
                    !comp_remap(ed, a, comp, own, via ? via->outer : NULL,
                                via ? via->inner : NULL, via ? via->count : 0, &mine) ||
                    !raw_eq(mine, c.old) ||
                    !comp_remap(ed, a, comp, c.val, via ? via->inner : NULL,
                                via ? via->outer : NULL, via ? via->count : 0, &next) ||
                    raw_eq(own, next))
                    continue; /* it sets the component itself */
                if (nd == ED_PLAN_MAX) {
                    prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large",
                                 "the change reaches too many prefabs");
                    return;
                }
                dc[nd++] = (pf_dchg){e, ev->sid, own, next, true};
            }
        }
    }
    /* Every instance of a changed prefab: its base, and its component when it had the old one. */
    for (u32 ii = 0; ii < g_scene.instance_count; ++ii) {
        ed_instance *in = &g_scene.instances[ii];
        for (u32 i = 0; i < nd; ++i) {
            u64 e = 0;
            if (strcmp(dc[i].d->asset, in->asset) != 0 || !local_ent_of(in, dc[i].sid, &e))
                continue;
            de_ep_raw val, oldb, cur;
            if (!comp_remap(ed, a, comp, dc[i].val, in->local_sid, in->local_ent,
                            in->local_count, &val) ||
                !base_comp_map(a, in, e, comp, &oldb) || !current_comp(ed, a, e, comp, &cur))
                continue;
            if (nb == ED_PLAN_MAX) {
                prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large",
                             "the change reaches too many instances");
                return;
            }
            rm[nb] = !is_nil(cur);
            bc[nb++] = (pf_bchg){in, e, val, raw_eq(cur, oldb) && !raw_eq(cur, val)};
        }
    }
    u32 sets = 0;
    for (u32 i = 0; i < nb; ++i)
        sets += bc[i].set;
    de_ep_edit_op *ops = (de_ep_edit_op *)de__ed_alloc(a, sizeof(de_ep_edit_op) * (2 * sets + 1));
    if (ops == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    memset(ops, 0, sizeof(de_ep_edit_op) * (2 * sets + 1));
    u32 no = 1;
    for (u32 i = 0; i < nb; ++i) {
        if (!bc[i].set)
            continue;
        if (rm[i]) {
            de_ep_edit_op *o = &ops[no++];
            o->op = de_ep_cstr("remove");
            o->has_entity = true;
            o->entity = bc[i].entity;
            o->has_component = true;
            o->component = de_ep_cstr(comp);
        }
        if (!is_nil(bc[i].val)) {
            de_ep_edit_op *o = &ops[no++];
            o->op = de_ep_cstr("add");
            o->has_entity = true;
            o->entity = bc[i].entity;
            o->has_component = true;
            o->component = de_ep_cstr(comp);
            o->has_value = true;
            o->value = bc[i].val;
        }
    }
    apply_finish(ed, req, a, w, q, inst->root, comp, dc, nd, bc, nb, ops, no);
}

/* Revert puts the prefab's component back (removed) or takes the instance's away (added). */
static void revert_structural(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                              mpack_writer_t *w, const de_ep_override_path_params *q) {
    pf_sreq s;
    if (!structural_check(ed, req, a, w, q, &s))
        return;
    de_ep_edit_op op;
    memset(&op, 0, sizeof op);
    op.has_entity = true;
    op.entity = q->entity;
    op.has_component = true;
    op.component = de_ep_cstr(s.comp);
    if (q->kind == DE_EP_OVERRIDE_KIND_COMPONENT_REMOVED) {
        de_ep_raw base;
        if (!base_comp_map(a, s.inst, q->entity, s.comp, &base)) {
            prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
            return;
        }
        op.op = de_ep_cstr("add");
        op.has_value = true;
        op.value = base;
    } else {
        op.op = de_ep_cstr("remove");
    }
    de_ep_error code = DE_EP_OK;
    char msg[256];
    const u32 tx = de__ed_edit_run(ed, a, q->has_tx ? q->tx : 0, instance_scene(s.inst->root),
                                   &op, 1, &code, msg, sizeof msg);
    if (tx == 0) {
        prefab_error(w, req->id, code, "edit", "%s", msg);
        return;
    }
    const de_ep_revert_result res = {.tx = tx, .revision = g_prefab.revision};
    de_ep_write_response_prefab_revert_override(w, req->id, &res);
}

/* ---------- Structural overrides: whole entities (DENG-811) ---------- */

/* What an entity_added or entity_removed request names, checked in the protocol's order. */
typedef struct pf_ereq {
    ed_instance *inst;
    bool added;
    u64 sid; /* added: the parent's prefab id; removed: the removed entity's (local) */
    ed_level lv[ED_CHAIN_MAX];
    u32 levels;
} pf_ereq;

static bool entity_check(de__editor *ed, const de_ep_request *req, mpack_writer_t *w,
                         const de_ep_override_path_params *q, bool apply, pf_ereq *s) {
    memset(s, 0, sizeof *s);
    s->added = q->kind == DE_EP_OVERRIDE_KIND_ENTITY_ADDED;
    const char *kind = de_ep_override_kind_name(q->kind);
    if (q->path.len != 0 || (!s->added && !q->has_local)) {
        prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "kind", "%s takes an empty path%s",
                     kind, s->added ? "" : " and the removed entity's local id");
        return false;
    }
    s->inst = instance_of(ed, q->entity);
    if (s->inst == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "not_instance",
                     "%llu is not a prefab instance", (unsigned long long)q->entity);
        return false;
    }
    ed_def *d0 = def_find(s->inst->asset);
    u64 sid = 0;
    const bool linked = local_sid_of(s->inst, q->entity, &sid);
    if (s->added) {
        const de_entity p = de_entity_parent(ed->e->world, (de_entity){q->entity});
        if (linked) {
            prefab_error(w, req->id, DE_EP_ERR_CONFLICT, "not_override",
                         "%llu is an entity of the prefab", (unsigned long long)q->entity);
            return false;
        }
        if (p.bits == 0 || !local_sid_of(s->inst, p.bits, &s->sid)) {
            prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no_entity",
                         "the parent of %llu is not an entity of the prefab",
                         (unsigned long long)q->entity);
            return false;
        }
    } else {
        u64 parent = 0, alive = 0;
        if (!linked || d0 == NULL || q->local == sid || !def_parent(d0, q->local, &parent) ||
            parent != sid) {
            prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no_entity",
                         "the prefab has no entity 0x%llx under %llu",
                         (unsigned long long)q->local, (unsigned long long)q->entity);
            return false;
        }
        if (linked_alive(ed, s->inst, q->local, &alive)) {
            prefab_error(w, req->id, DE_EP_ERR_CONFLICT, "not_override",
                         "entity 0x%llx of the prefab is not removed (it is %llu)",
                         (unsigned long long)q->local, (unsigned long long)alive);
            return false;
        }
        s->sid = q->local;
    }
    s->levels = chain_of(d0, s->sid, "", s->lv);
    if (s->levels == 0) {
        prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no_definition",
                     "no prefab definition of %llu", (unsigned long long)q->entity);
        return false;
    }
    if (apply && q->has_target && q->target.len > 0 &&
        !de_ep_str_eq(q->target, s->lv[0].d->asset)) {
        u32 t = 0;
        while (t < s->levels && !de_ep_str_eq(q->target, s->lv[t].d->asset))
            ++t;
        if (t == s->levels)
            prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "target",
                         "%.*s is not in the prefab chain of %llu", (int)q->target.len,
                         q->target.ptr, (unsigned long long)q->entity);
        else
            prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "kind",
                         "%s applies to the instance's prefab (%s) only", kind,
                         s->lv[0].d->asset);
        return false;
    }
    return true;
}

/* prefab.state entries: definition changes, and instance links (sid -> entity; 0 unlinks). */
typedef struct pf_sdef {
    const char *asset;
    u64 sid;
    const char *path;
    de_ep_raw val;
} pf_sdef;

typedef struct pf_slink {
    u64 root, sid, entity;
} pf_slink;

static bool state_op(de_ep_arena *a, const pf_sdef *d, u32 nd, const pf_slink *l, u32 nl,
                     de_ep_edit_op *op) {
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, (nd != 0) + (nl != 0));
    if (nd != 0) {
        mpack_write_cstr(&mw, "defs");
        mpack_start_array(&mw, nd);
        for (u32 i = 0; i < nd; ++i) {
            mpack_start_map(&mw, 5);
            mpack_write_cstr(&mw, "asset");
            mpack_write_cstr(&mw, d[i].asset);
            mpack_write_cstr(&mw, "sid");
            mpack_write_u64(&mw, d[i].sid);
            mpack_write_cstr(&mw, "path");
            mpack_write_cstr(&mw, d[i].path);
            mpack_write_cstr(&mw, "value");
            mpack_write_object_bytes(&mw, d[i].val.ptr, d[i].val.len);
            mpack_write_cstr(&mw, "follow");
            mpack_write_bool(&mw, false);
            mpack_finish_map(&mw);
        }
        mpack_finish_array(&mw);
    }
    if (nl != 0) {
        mpack_write_cstr(&mw, "links");
        mpack_start_array(&mw, nl);
        for (u32 i = 0; i < nl; ++i) {
            mpack_start_map(&mw, 3);
            mpack_write_cstr(&mw, "root");
            mpack_write_u64(&mw, l[i].root);
            mpack_write_cstr(&mw, "sid");
            mpack_write_u64(&mw, l[i].sid);
            mpack_write_cstr(&mw, "entity");
            mpack_write_u64(&mw, l[i].entity);
            mpack_finish_map(&mw);
        }
        mpack_finish_array(&mw);
    }
    mpack_finish_map(&mw);
    memset(op, 0, sizeof *op);
    op->op = de_ep_cstr("prefab.state");
    op->has_value = true;
    op->value = de__ed_raw_end(a, &mw);
    return op->value.len != 0;
}

/* {parent, components}: a new entity block (parent 0: none). */
static bool block_value(de_ep_arena *a, u64 parent, de_ep_raw comps, de_ep_raw *out) {
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 1 + (parent != 0));
    if (parent != 0) {
        mpack_write_cstr(&mw, "parent");
        mpack_write_u64(&mw, parent);
    }
    mpack_write_cstr(&mw, "components");
    mpack_write_object_bytes(&mw, comps.ptr, comps.len);
    mpack_finish_map(&mw);
    *out = de__ed_raw_end(a, &mw);
    return out->len != 0;
}

/* {root, inner, outer}: a new pair of the [[prefab.nested]] entry whose root is `root`. */
static bool nest_value(de_ep_arena *a, u64 root, u64 inner, u64 outer, de_ep_raw *out) {
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 3);
    mpack_write_cstr(&mw, "root");
    mpack_write_u64(&mw, root);
    mpack_write_cstr(&mw, "inner");
    mpack_write_u64(&mw, inner);
    mpack_write_cstr(&mw, "outer");
    mpack_write_u64(&mw, outer);
    mpack_finish_map(&mw);
    *out = de__ed_raw_end(a, &mw);
    return out->len != 0;
}

/* `n` free entity ids, so the ops can name the entities before they exist (a spawn takes an id
 * that is free). */
static void ids_reserve(de__editor *ed, u64 *out, u32 n) {
    for (u32 i = 0; i < n; ++i)
        out[i] = de_entity_spawn(ed->e->world).bits;
    for (u32 i = 0; i < n; ++i)
        de_entity_despawn(ed->e->world, (de_entity){out[i]});
}

static void spawn_op(de_ep_edit_op *o, u64 scene, u64 entity, u64 parent, de_ep_map_raw comps) {
    memset(o, 0, sizeof *o);
    o->op = de_ep_cstr("spawn");
    o->has_scene = true;
    o->scene = scene;
    o->has_entity = true;
    o->entity = entity;
    o->has_parent = parent != 0;
    o->parent = parent;
    o->has_components = true;
    o->components = comps;
}

static void despawn_op(de_ep_edit_op *o, u64 entity) {
    memset(o, 0, sizeof *o);
    o->op = de_ep_cstr("despawn");
    o->has_entity = true;
    o->entity = entity;
}

/* A prefab level an entity change writes: `up` is the level it follows, through `via` (a
 * nesting) or as a variant (NULL). For a removal, `sub` is the entity's subtree there. */
typedef struct pf_elv {
    ed_def *d;
    u64 sid;
    const ed_nest *via;
    u32 up;
    u64 *sub;
    u32 subn;
} pf_elv;

static bool elv_has(const pf_elv *pl, u32 n, const ed_def *d, u64 sid) {
    for (u32 i = 0; i < n; ++i)
        if (pl[i].d == d && pl[i].sid == sid)
            return true;
    return false;
}

/* `id` of the nesting prefab's space in the nested prefab's (outer -> inner). */
static u64 via_inner(const ed_nest *via, u64 id) {
    for (u32 i = 0; via != NULL && i < via->count; ++i)
        if (via->outer[i] == id)
            return via->inner[i];
    return id;
}

/* Whether (e, x) holds the same values as (L, y), seen through `via`. The root's parent is not
 * compared: it differs by design. */
static bool vals_same(de__editor *ed, de_ep_arena *a, const ed_def *e, u64 x, const ed_nest *via,
                      const ed_def *l, u64 y, bool root) {
    u32 ne = 0, nl = 0;
    for (u32 i = 0; i < e->val_count; ++i)
        ne += e->vals[i].sid == x;
    for (u32 i = 0; i < l->val_count; ++i)
        nl += l->vals[i].sid == y;
    if (ne != nl)
        return false;
    for (u32 i = 0; i < e->val_count; ++i) {
        const ed_dval *v = &e->vals[i];
        if (v->sid != x)
            continue;
        const ed_dval *lv = def_get(l, y, v->path);
        if (lv == NULL)
            return false;
        if (v->path[0] == '\0') {
            u64 pe = 0, pl = 0;
            if (!root && (!def_parent(e, x, &pe) || !def_parent(l, y, &pl) ||
                          via_inner(via, pe) != pl))
                return false;
            continue;
        }
        if (is_comp_path(v->path))
            continue;
        ed_kind kind = {0};
        de_ep_raw mine = dval_raw(e, v);
        if (path_field(ed, v->path, &kind) &&
            !remap(a, kind, dval_raw(e, v), via ? via->outer : NULL, via ? via->inner : NULL,
                   via ? via->count : 0, &mine))
            return false;
        if (!raw_eq(mine, dval_raw(l, lv)))
            return false;
    }
    return true;
}

/* The end of an entity apply: runs `ops` and reports the plan's prefabs and their files. */
static void entity_finish(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w, const de_ep_override_path_params *q, u64 root,
                          const pf_elv *pl, u32 np, const u64 *ents, u32 ne,
                          const de_ep_edit_op *ops, u32 no) {
    const char *names[ED_PLAN_MAX];
    u32 nn = 0;
    for (u32 i = 0; i < np; ++i) {
        bool seen = false;
        for (u32 k = 0; k < nn && !seen; ++k)
            seen = names[k] == pl[i].d->asset;
        if (!seen)
            names[nn++] = pl[i].d->asset;
    }
    static u64 kept[ED_LOCAL_CAP];
    const u32 nk = ne < ED_LOCAL_CAP ? ne : ED_LOCAL_CAP;
    memcpy(kept, ents, sizeof(u64) * nk);
    de_ep_error code = DE_EP_OK;
    char msg[256];
    g_files.reason = NULL;
    const u32 tx = de__ed_edit_run(ed, a, q->has_tx ? q->tx : 0, instance_scene(root), ops, no,
                                   &code, msg, sizeof msg);
    if (tx == 0) {
        prefab_error(w, req->id, code, g_files.reason ? g_files.reason : "edit", "%s", msg);
        return;
    }
    de_ep_str pnames[ED_PLAN_MAX], fnames[ED_PLAN_MAX];
    for (u32 i = 0; i < nn; ++i) {
        const char *src = de__ed_asset_source_path(ed, names[i]);
        pnames[i] = de_ep_cstr(names[i]);
        fnames[i] = de_ep_cstr(src != NULL ? src : names[i]);
    }
    const de_ep_apply_result res = {
        .tx = tx,
        .revision = g_prefab.revision,
        .prefabs = {pnames, nn},
        .entities = {kept, nk},
        .files = {fnames, nn},
    };
    de_ep_write_response_prefab_apply_override(w, req->id, &res);
}

#define ED_SPAWN_MAX ED_LOCAL_CAP

static u64 g_count[ED_LOCAL_CAP]; /* scratch for counting a tree */

/* entity_added: the subtree joins the instance's prefab under new ids (past every id of the
 * prefabs written), and every variant and nesting prefab of it; the other instances of those
 * prefabs spawn it too. */
static void apply_entity_added(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                               mpack_writer_t *w, const de_ep_override_path_params *q,
                               const pf_ereq *s) {
    ed_instance *inst = s->inst;
    if (!load_dependents(ed, req, w, s->lv[0].d->asset))
        return;
    u64 tree[ED_SUB_MAX];
    u32 up[ED_SUB_MAX];
    const u32 tn = tree_of(ed, (de_entity){q->entity}, tree, ED_SUB_MAX);
    pf_elv *pl = (pf_elv *)de__ed_alloc(a, sizeof(pf_elv) * ED_PLAN_MAX);
    if (tn == ED_SUB_MAX || pl == NULL) {
        prefab_error(w, req->id, pl == NULL ? DE_EP_ERR_BUSY : DE_EP_ERR_UNSUPPORTED,
                     pl == NULL ? "memory" : "too_large", "the added subtree is too large");
        return;
    }
    for (u32 i = 0; i < tn; ++i) {
        up[i] = 0;
        const u64 p = de_entity_parent(ed->e->world, (de_entity){tree[i]}).bits;
        for (u32 k = 0; k < i; ++k)
            if (tree[k] == p)
                up[i] = k;
    }
    /* The plan: the instance's prefab, then whatever follows a level in it. */
    u32 np = 0;
    pl[np++] = (pf_elv){s->lv[0].d, s->lv[0].sid, NULL, 0, NULL, 0};
    for (u32 i = 0; i < np; ++i)
        for (u32 di = 0; di < g_prefab.count; ++di) {
            ed_def *e = g_prefab.defs[di];
            for (u32 vi = 0; vi < e->val_count && e != pl[i].d; ++vi) {
                const ed_dval *ev = &e->vals[vi];
                ed_level nx;
                if (ev->path[0] != '\0' || !def_next(e, ev->sid, &nx) || nx.d != pl[i].d ||
                    nx.sid != pl[i].sid || elv_has(pl, np, e, ev->sid))
                    continue;
                if (np == ED_PLAN_MAX) {
                    prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large",
                                 "the change reaches too many prefabs");
                    return;
                }
                pl[np++] = (pf_elv){e, ev->sid, nx.via, i, NULL, 0};
            }
        }
    /* New ids, the same in every level: past every id the written prefabs use. */
    u64 top = 0;
    for (u32 i = 0; i < np; ++i) {
        const ed_def *d = pl[i].d;
        for (u32 k = 0; k < d->val_count; ++k)
            top = d->vals[k].sid > top ? d->vals[k].sid : top;
        for (u32 k = 0; k < d->nest_count; ++k)
            for (u32 j = 0; j < d->nest[k].count; ++j) {
                top = d->nest[k].inner[j] > top ? d->nest[k].inner[j] : top;
                top = d->nest[k].outer[j] > top ? d->nest[k].outer[j] : top;
            }
    }
    const u64 base_id = top + 1;
    u64 nid[ED_SUB_MAX];
    for (u32 i = 0; i < tn; ++i)
        nid[i] = base_id + i;
    /* The other instances that spawn the subtree: one per (instance, level) match. */
    u32 nmatch = 0;
    for (u32 ii = 0; ii < g_scene.instance_count; ++ii)
        for (u32 j = 0; j < np && &g_scene.instances[ii] != inst; ++j) {
            u64 pe = 0;
            nmatch += strcmp(g_scene.instances[ii].asset, pl[j].d->asset) == 0 &&
                      linked_alive(ed, &g_scene.instances[ii], pl[j].sid, &pe);
        }
    if ((u64)(nmatch + 1) * tn > ED_SPAWN_MAX) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large",
                     "the change reaches too many instances");
        return;
    }
    const u32 nall = (nmatch + 1) * tn;
    /* Each level's components of each new entity, in the level's ids. */
    const u32 lc = inst->local_count;
    u64 *from = (u64 *)de__ed_alloc(a, sizeof(u64) * (tn + lc + 1));
    u64 *to = (u64 *)de__ed_alloc(a, sizeof(u64) * (tn + lc + 1));
    de_ep_raw *comps = (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * np * tn);
    pf_sdef *defs = (pf_sdef *)de__ed_alloc(a, sizeof(pf_sdef) * np * tn * 2);
    pf_slink *links = (pf_slink *)de__ed_alloc(a, sizeof(pf_slink) * nall);
    de_ep_edit_op *ops = (de_ep_edit_op *)de__ed_alloc(a, sizeof(de_ep_edit_op) * (nall - tn + 2));
    u64 *spawned = (u64 *)de__ed_alloc(a, sizeof(u64) * nall);
    if (from == NULL || to == NULL || comps == NULL || defs == NULL || links == NULL ||
        ops == NULL || spawned == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    for (u32 i = 0; i < tn; ++i) {
        from[i] = tree[i];
        to[i] = nid[i];
    }
    memcpy(from + tn, inst->local_ent, sizeof(u64) * lc);
    memcpy(to + tn, inst->local_sid, sizeof(u64) * lc);
    for (u32 i = 0; i < tn; ++i) {
        de_ep_raw live;
        if (!entity_comps(ed, a, tree[i], &live) ||
            !comps_remap(ed, a, live, from, to, tn + lc, &comps[i])) {
            prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "outside_prefab",
                         "%llu refers to an entity outside the prefab",
                         (unsigned long long)tree[i]);
            return;
        }
    }
    for (u32 j = 1; j < np; ++j) {
        const ed_nest *via = pl[j].via;
        for (u32 i = 0; i < tn; ++i) {
            const de_ep_raw prev = comps[pl[j].up * tn + i];
            if (via == NULL) {
                comps[j * tn + i] = prev;
                continue;
            }
            /* The new ids are the same in every level; the rest map inner -> outer. */
            u64 *f2 = (u64 *)de__ed_alloc(a, sizeof(u64) * (tn + via->count + 1));
            u64 *t2 = (u64 *)de__ed_alloc(a, sizeof(u64) * (tn + via->count + 1));
            if (f2 != NULL && t2 != NULL) {
                memcpy(f2, nid, sizeof(u64) * tn);
                memcpy(t2, nid, sizeof(u64) * tn);
                memcpy(f2 + tn, via->inner, sizeof(u64) * via->count);
                memcpy(t2 + tn, via->outer, sizeof(u64) * via->count);
            }
            if (f2 == NULL || t2 == NULL ||
                !comps_remap(ed, a, prev, f2, t2, tn + via->count, &comps[j * tn + i])) {
                prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "outside_prefab",
                             "%llu refers to an entity outside %s", (unsigned long long)tree[i],
                             pl[j].d->asset);
                return;
            }
        }
    }
    u32 nd = 0;
    for (u32 j = 0; j < np; ++j)
        for (u32 i = 0; i < tn; ++i) {
            de_ep_raw v;
            if (!block_value(a, i == 0 ? pl[j].sid : nid[up[i]], comps[j * tn + i], &v)) {
                prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
                return;
            }
            defs[nd++] = (pf_sdef){pl[j].d->asset, nid[i], "", v};
            if (pl[j].via != NULL) {
                if (!nest_value(a, pl[j].via->root, nid[i], nid[i], &v)) {
                    prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
                    return;
                }
                defs[nd++] = (pf_sdef){pl[j].d->asset, nid[i], "@nest", v};
            }
        }
    /* The instance links its subtree; the other instances spawn theirs. */
    u32 nl = 0, ns = 0;
    for (u32 i = 0; i < tn; ++i) {
        links[nl++] = (pf_slink){inst->root, nid[i], tree[i]};
        spawned[ns++] = tree[i];
    }
    u32 no = 1;
    for (u32 ii = 0; ii < g_scene.instance_count; ++ii) {
        ed_instance *in = &g_scene.instances[ii];
        if (in == inst)
            continue;
        for (u32 j = 0; j < np; ++j) {
            u64 pe = 0;
            if (strcmp(in->asset, pl[j].d->asset) != 0 || !linked_alive(ed, in, pl[j].sid, &pe))
                continue;
            u64 *bits = spawned + ns;
            ids_reserve(ed, bits, tn);
            const u32 il = in->local_count;
            u64 *f2 = (u64 *)de__ed_alloc(a, sizeof(u64) * (tn + il + 1));
            u64 *t2 = (u64 *)de__ed_alloc(a, sizeof(u64) * (tn + il + 1));
            if (f2 == NULL || t2 == NULL) {
                prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
                return;
            }
            memcpy(f2, nid, sizeof(u64) * tn);
            memcpy(t2, bits, sizeof(u64) * tn);
            memcpy(f2 + tn, in->local_sid, sizeof(u64) * il);
            memcpy(t2 + tn, in->local_ent, sizeof(u64) * il);
            for (u32 i = 0; i < tn; ++i) {
                de_ep_raw mine;
                de_ep_map_raw kv;
                if (!comps_remap(ed, a, comps[j * tn + i], f2, t2, tn + il, &mine) ||
                    !comps_split(a, mine, &kv)) {
                    prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "outside_prefab",
                                 "%llu refers to an entity outside the prefab",
                                 (unsigned long long)tree[i]);
                    return;
                }
                spawn_op(&ops[no++], instance_scene(in->root), bits[i],
                         i == 0 ? pe : bits[up[i]], kv);
                links[nl++] = (pf_slink){in->root, nid[i], bits[i]};
            }
            ns += tn;
        }
    }
    if (!state_op(a, defs, nd, NULL, 0, &ops[0]) || !state_op(a, NULL, 0, links, nl, &ops[no])) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    ++no;
    entity_finish(ed, req, a, w, q, inst->root, pl, np, spawned, ns, ops, no);
}

/* entity_removed: the entity's subtree leaves the instance's prefab, and every variant and
 * nesting prefab that holds it unchanged; the instances of those prefabs lose it too. */
static void apply_entity_removed(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                                 mpack_writer_t *w, const de_ep_override_path_params *q,
                                 const pf_ereq *s) {
    ed_instance *inst = s->inst;
    if (!load_dependents(ed, req, w, s->lv[0].d->asset))
        return;
    pf_elv *pl = (pf_elv *)de__ed_alloc(a, sizeof(pf_elv) * ED_PLAN_MAX);
    u64 *sub = (u64 *)de__ed_alloc(a, sizeof(u64) * ED_SUB_MAX);
    if (pl == NULL || sub == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    u32 np = 0;
    const u32 n0 = def_subtree(s->lv[0].d, s->sid, sub, ED_SUB_MAX);
    if (n0 == 0 || n0 == ED_SUB_MAX) {
        prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "too_large",
                     "the removed subtree is too large");
        return;
    }
    pl[np++] = (pf_elv){s->lv[0].d, s->sid, NULL, 0, sub, n0};
    /* Dependents whose copy of the subtree is the same follow. */
    for (u32 i = 0; i < np; ++i)
        for (u32 di = 0; di < g_prefab.count; ++di) {
            ed_def *e = g_prefab.defs[di];
            for (u32 vi = 0; vi < e->val_count && e != pl[i].d; ++vi) {
                const ed_dval *ev = &e->vals[vi];
                ed_level nx;
                if (ev->path[0] != '\0' || !def_next(e, ev->sid, &nx) || nx.d != pl[i].d ||
                    nx.sid != pl[i].sid || elv_has(pl, np, e, ev->sid))
                    continue;
                u64 *es = (u64 *)de__ed_alloc(a, sizeof(u64) * ED_SUB_MAX);
                if (es == NULL) {
                    prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
                    return;
                }
                const u32 en = def_subtree(e, ev->sid, es, ED_SUB_MAX);
                bool same = en == pl[i].subn;
                for (u32 k = 0; k < en && same; ++k) {
                    ed_level xl;
                    bool in_sub = false;
                    if (!def_next(e, es[k], &xl) || xl.d != pl[i].d) {
                        same = false;
                        break;
                    }
                    for (u32 m = 0; m < pl[i].subn && !in_sub; ++m)
                        in_sub = pl[i].sub[m] == xl.sid;
                    same = in_sub && vals_same(ed, a, e, es[k], xl.via, pl[i].d, xl.sid, k == 0);
                }
                if (!same)
                    continue; /* it changed the entity: it keeps it */
                if (np == ED_PLAN_MAX) {
                    prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large",
                                 "the change reaches too many prefabs");
                    return;
                }
                pl[np++] = (pf_elv){e, ev->sid, nx.via, i, es, en};
            }
        }
    u32 total = 0;
    for (u32 j = 0; j < np; ++j)
        total += pl[j].subn;
    /* At most: a link per linked entity of the subtree, and a despawn per entity of the
     * instances it reaches. */
    u32 lcap = 0, gcap = 0;
    for (u32 ii = 0; ii < g_scene.instance_count; ++ii) {
        const ed_instance *in = &g_scene.instances[ii];
        bool hit = false;
        for (u32 j = 0; j < np; ++j)
            if (strcmp(in->asset, pl[j].d->asset) == 0) {
                lcap += pl[j].subn;
                hit = true;
            }
        if (hit)
            gcap += tree_of(ed, (de_entity){in->root}, g_count, ED_LOCAL_CAP);
    }
    if (lcap > ED_SPAWN_MAX || gcap > ED_SPAWN_MAX) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large",
                     "the change reaches too many entities");
        return;
    }
    pf_sdef *defs = (pf_sdef *)de__ed_alloc(a, sizeof(pf_sdef) * total);
    pf_slink *links = lcap ? (pf_slink *)de__ed_alloc(a, sizeof(pf_slink) * lcap) : NULL;
    u64 *gone = gcap ? (u64 *)de__ed_alloc(a, sizeof(u64) * gcap) : NULL;
    de_ep_edit_op *ops = (de_ep_edit_op *)de__ed_alloc(a, sizeof(de_ep_edit_op) * (gcap + 2));
    if (defs == NULL || (lcap && links == NULL) || (gcap && gone == NULL) || ops == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    u32 nd = 0;
    for (u32 j = 0; j < np; ++j)
        for (u32 k = 0; k < pl[j].subn; ++k)
            defs[nd++] = (pf_sdef){pl[j].d->asset, pl[j].sub[k], "", (de_ep_raw){k_nil, 1}};
    /* Every instance of a written prefab unlinks the subtree, and despawns what is alive. */
    u32 nl = 0, ng = 0;
    for (u32 ii = 0; ii < g_scene.instance_count; ++ii) {
        ed_instance *in = &g_scene.instances[ii];
        for (u32 j = 0; j < np; ++j) {
            if (strcmp(in->asset, pl[j].d->asset) != 0)
                continue;
            for (u32 k = 0; k < pl[j].subn; ++k) {
                u64 e = 0;
                if (!local_ent_of(in, pl[j].sub[k], &e))
                    continue;
                if (nl == lcap)
                    continue;
                links[nl++] = (pf_slink){in->root, pl[j].sub[k], 0};
                if (!de_entity_alive(ed->e->world, (de_entity){e}))
                    continue;
                u64 t[ED_SUB_MAX];
                const u32 n = tree_of(ed, (de_entity){e}, t, ED_SUB_MAX);
                for (u32 m = 0; m < n; ++m) {
                    bool seen = false;
                    for (u32 x = 0; x < ng && !seen; ++x)
                        seen = gone[x] == t[m];
                    if (seen)
                        continue;
                    if (ng == gcap)
                        break;
                    gone[ng++] = t[m];
                }
            }
        }
    }
    u32 no = 0;
    if (nl != 0 && !state_op(a, NULL, 0, links, nl, &ops[no++])) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    for (u32 i = ng; i-- > 0;)
        despawn_op(&ops[no++], gone[i]);
    if (!state_op(a, defs, nd, NULL, 0, &ops[no++])) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    entity_finish(ed, req, a, w, q, inst->root, pl, np, gone, ng, ops, no);
}

static void apply_entity(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w, const de_ep_override_path_params *q) {
    pf_ereq s;
    if (!entity_check(ed, req, w, q, true, &s))
        return;
    if (s.added)
        apply_entity_added(ed, req, a, w, q, &s);
    else
        apply_entity_removed(ed, req, a, w, q, &s);
}

/* Revert takes the added subtree away, or spawns the removed one back from the prefab. */
static void revert_entity(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w, const de_ep_override_path_params *q) {
    pf_ereq s;
    if (!entity_check(ed, req, w, q, false, &s))
        return;
    ed_instance *inst = s.inst;
    u64 tree[ED_SUB_MAX];
    const u32 tn = s.added ? tree_of(ed, (de_entity){q->entity}, tree, ED_SUB_MAX)
                           : def_subtree(s.lv[0].d, s.sid, tree, ED_SUB_MAX);
    de_ep_edit_op *ops = (de_ep_edit_op *)de__ed_alloc(a, sizeof(de_ep_edit_op) * (tn + 1));
    if (ops == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    u32 no = 0;
    if (s.added) {
        for (u32 i = tn; i-- > 0;)
            despawn_op(&ops[no++], tree[i]);
    } else {
        ed_def *d0 = s.lv[0].d;
        const u32 sn = tn;
        u64 miss[ED_SUB_MAX], bits[ED_SUB_MAX];
        u32 nm = 0;
        for (u32 i = 0; i < sn; ++i) {
            u64 e = 0;
            if (!linked_alive(ed, inst, tree[i], &e))
                miss[nm++] = tree[i];
        }
        ids_reserve(ed, bits, nm);
        const u32 lc = inst->local_count;
        u64 *from = (u64 *)de__ed_alloc(a, sizeof(u64) * (nm + lc + 1));
        u64 *to = (u64 *)de__ed_alloc(a, sizeof(u64) * (nm + lc + 1));
        pf_slink *links = (pf_slink *)de__ed_alloc(a, sizeof(pf_slink) * (nm + 1));
        if (from == NULL || to == NULL || links == NULL) {
            prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
            return;
        }
        memcpy(from, miss, sizeof(u64) * nm);
        memcpy(to, bits, sizeof(u64) * nm);
        memcpy(from + nm, inst->local_sid, sizeof(u64) * lc);
        memcpy(to + nm, inst->local_ent, sizeof(u64) * lc);
        for (u32 i = 0; i < nm; ++i) {
            u64 psid = 0, parent = q->entity;
            if (miss[i] != s.sid && def_parent(d0, miss[i], &psid)) {
                bool found = false;
                for (u32 k = 0; k < nm && !found; ++k)
                    if (miss[k] == psid) {
                        parent = bits[k];
                        found = true;
                    }
                if (!found)
                    (void)linked_alive(ed, inst, psid, &parent);
            }
            de_ep_raw raw, mine;
            de_ep_map_raw kv;
            if (!def_comps(a, d0, miss[i], &raw) ||
                !comps_remap(ed, a, raw, from, to, nm + lc, &mine) || !comps_split(a, mine, &kv)) {
                prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "outside_prefab",
                             "entity 0x%llx refers to an entity outside the instance",
                             (unsigned long long)miss[i]);
                return;
            }
            spawn_op(&ops[no++], instance_scene(inst->root), bits[i], parent, kv);
            links[i] = (pf_slink){inst->root, miss[i], bits[i]};
        }
        if (!state_op(a, NULL, 0, links, nm, &ops[no++])) {
            prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
            return;
        }
    }
    de_ep_error code = DE_EP_OK;
    char msg[256];
    g_files.reason = NULL;
    const u32 tx = de__ed_edit_run(ed, a, q->has_tx ? q->tx : 0, instance_scene(inst->root), ops,
                                   no, &code, msg, sizeof msg);
    if (tx == 0) {
        prefab_error(w, req->id, code, g_files.reason ? g_files.reason : "edit", "%s", msg);
        return;
    }
    const de_ep_revert_result res = {.tx = tx, .revision = g_prefab.revision};
    de_ep_write_response_prefab_revert_override(w, req->id, &res);
}

static void on_apply(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    de_ep_override_path_params q;
    memset(&q, 0, sizeof q);
    if (!de_ep_read_params_prefab_apply_override(req, a, &q)) {
        prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "params", "bad params");
        return;
    }
    if (reject_request(ed, req, w, &q))
        return;
    if (q.has_kind && (q.kind == DE_EP_OVERRIDE_KIND_ENTITY_ADDED ||
                       q.kind == DE_EP_OVERRIDE_KIND_ENTITY_REMOVED)) {
        apply_entity(ed, req, a, w, &q);
        return;
    }
    if (q.has_kind && q.kind != DE_EP_OVERRIDE_KIND_FIELD) {
        apply_structural(ed, req, a, w, &q);
        return;
    }
    char *path = de__ed_cstr(a, q.path);
    ed_kind entity = {0};
    if (!path_check(ed, req, w, path, q.path, &entity))
        return;
    ed_instance *inst = instance_of(ed, q.entity);
    ed_base *b = inst != NULL ? base_find(inst, q.entity, q.path) : NULL;
    if (b == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, inst ? "no_field" : "not_instance",
                     "no prefab field %s on %llu", path, (unsigned long long)q.entity);
        return;
    }
    de_ep_raw cur;
    if (!current_value(ed, a, q.entity, path, &cur)) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no_field", "no field %s", path);
        return;
    }
    if (raw_eq(cur, base_raw(inst, b))) {
        prefab_error(w, req->id, DE_EP_ERR_CONFLICT, "not_override", "%s is not overridden",
                     path);
        return;
    }
    u64 sid = 0;
    ed_level lv[ED_CHAIN_MAX];
    const u32 levels =
        local_sid_of(inst, q.entity, &sid) ? chain_of(def_find(inst->asset), sid, path, lv) : 0;
    if (levels == 0) {
        prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no_definition",
                     "no prefab definition of %s for %llu", path, (unsigned long long)q.entity);
        return;
    }
    u32 t = 0;
    if (q.has_target && q.target.len > 0) {
        while (t < levels && !de_ep_str_eq(q.target, lv[t].d->asset))
            ++t;
        if (t == levels) {
            prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "target",
                         "%.*s is not in the prefab chain of %s", (int)q.target.len, q.target.ptr, path);
            return;
        }
    }
    if (!load_dependents(ed, req, w, lv[t].d->asset))
        return;
    pf_dchg *dc = (pf_dchg *)de__ed_alloc(a, sizeof(pf_dchg) * ED_PLAN_MAX);
    pf_bchg *bc = (pf_bchg *)de__ed_alloc(a, sizeof(pf_bchg) * ED_PLAN_MAX);
    if (dc == NULL || bc == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    /* The levels down to the target take the value, in each level's own ids. */
    u32 nd = 0, nb = 0;
    de_ep_raw v;
    if (!remap(a, entity, cur, inst->local_ent, inst->local_sid, inst->local_count, &v)) {
        prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "outside_prefab",
                     "%s refers to an entity outside the prefab", path);
        return;
    }
    for (u32 k = 0; k <= t; ++k) {
        if (k > 0 && !down_one(a, entity, lv, k - 1, v, &v)) {
            prefab_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "outside_prefab",
                         "%s refers to an entity outside %s", path, lv[k].d->asset);
            return;
        }
        dc[nd++] = (pf_dchg){lv[k].d, lv[k].sid, dval_raw(lv[k].d, def_get(lv[k].d, lv[k].sid, path)),
                             v, false};
    }
    /* Dependents that did not set the field follow, level by level. */
    for (u32 i = 0; i < nd; ++i) {
        const pf_dchg c = dc[i];
        for (u32 di = 0; di < g_prefab.count; ++di) {
            ed_def *e = g_prefab.defs[di];
            for (u32 vi = 0; vi < e->val_count && e != c.d; ++vi) {
                const ed_dval *ev = &e->vals[vi];
                ed_level nx;
                if (strcmp(ev->path, path) != 0 || !def_next(e, ev->sid, &nx) || nx.d != c.d ||
                    nx.sid != c.sid || plan_has(dc, nd, e, ev->sid))
                    continue;
                const ed_nest *via = nx.via;
                de_ep_raw mine, next;
                if (!remap(a, entity, dval_raw(e, ev), via ? via->outer : NULL,
                           via ? via->inner : NULL, via ? via->count : 0, &mine) ||
                    !raw_eq(mine, c.old) ||
                    !remap(a, entity, c.val, via ? via->inner : NULL, via ? via->outer : NULL,
                           via ? via->count : 0, &next))
                    continue; /* it sets the field itself */
                if (nd == ED_PLAN_MAX) {
                    prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large",
                                 "the change reaches too many prefabs");
                    return;
                }
                dc[nd++] = (pf_dchg){e, ev->sid, dval_raw(e, ev), next, true};
            }
        }
    }
    /* Every instance of a changed prefab: its base, and its value when it had the old one. */
    for (u32 ii = 0; ii < g_scene.instance_count; ++ii) {
        ed_instance *in = &g_scene.instances[ii];
        for (u32 i = 0; i < nd; ++i) {
            u64 e = 0;
            if (strcmp(dc[i].d->asset, in->asset) != 0 || !local_ent_of(in, dc[i].sid, &e))
                continue;
            ed_base *eb = base_find(in, e, q.path);
            de_ep_raw val, now;
            if (eb == NULL || !remap(a, entity, dc[i].val, in->local_sid, in->local_ent,
                                     in->local_count, &val))
                continue;
            if (nb == ED_PLAN_MAX) {
                prefab_error(w, req->id, DE_EP_ERR_BUSY, "too_large",
                             "the change reaches too many instances");
                return;
            }
            const bool same = current_value(ed, a, e, path, &now) && raw_eq(now, base_raw(in, eb));
            bc[nb++] = (pf_bchg){in, e, val, same && !raw_eq(now, val)};
        }
    }
    u32 sets = 0;
    for (u32 i = 0; i < nb; ++i)
        sets += bc[i].set;
    de_ep_edit_op *ops = (de_ep_edit_op *)de__ed_alloc(a, sizeof(de_ep_edit_op) * (sets + 1));
    if (ops == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    memset(ops, 0, sizeof(de_ep_edit_op) * (sets + 1));
    u32 no = 1;
    for (u32 i = 0; i < nb; ++i) {
        if (!bc[i].set)
            continue;
        de_ep_edit_op *o = &ops[no++];
        o->op = de_ep_cstr("set");
        o->has_entity = true;
        o->entity = bc[i].entity;
        o->has_path = true;
        o->path = de_ep_cstr(path);
        o->has_value = true;
        o->value = bc[i].val;
    }
    apply_finish(ed, req, a, w, &q, inst->root, path, dc, nd, bc, nb, ops, no);
}

/* prefab.revertOverride: an undoable `set` back to the instance's base value. */
static void on_revert(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    de_ep_override_path_params q;
    memset(&q, 0, sizeof q);
    if (!de_ep_read_params_prefab_revert_override(req, a, &q)) {
        prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "params", "bad params");
        return;
    }
    if (reject_request(ed, req, w, &q))
        return;
    if (q.has_kind && (q.kind == DE_EP_OVERRIDE_KIND_ENTITY_ADDED ||
                       q.kind == DE_EP_OVERRIDE_KIND_ENTITY_REMOVED)) {
        revert_entity(ed, req, a, w, &q);
        return;
    }
    if (q.has_kind && q.kind != DE_EP_OVERRIDE_KIND_FIELD) {
        revert_structural(ed, req, a, w, &q);
        return;
    }
    char *path = de__ed_cstr(a, q.path);
    ed_kind entity = {0};
    if (!path_check(ed, req, w, path, q.path, &entity))
        return;
    ed_instance *inst = instance_of(ed, q.entity);
    ed_base *b = inst != NULL ? base_find(inst, q.entity, q.path) : NULL;
    de_ep_raw cur;
    if (b == NULL || !current_value(ed, a, q.entity, path, &cur)) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, inst ? "no_field" : "not_instance",
                     "no prefab field %s on %llu", path, (unsigned long long)q.entity);
        return;
    }
    if (raw_eq(cur, base_raw(inst, b))) {
        prefab_error(w, req->id, DE_EP_ERR_CONFLICT, "not_override", "%s is not overridden",
                     path);
        return;
    }
    const de_ep_raw base = base_raw(inst, b);
    char *copy = (char *)de__ed_alloc(a, base.len);
    if (copy == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
        return;
    }
    memcpy(copy, base.ptr, base.len);
    de_ep_edit_op op;
    memset(&op, 0, sizeof op);
    op.op = de_ep_cstr("set");
    op.has_entity = true;
    op.entity = q.entity;
    op.has_path = true;
    op.path = de_ep_cstr(path);
    op.has_value = true;
    op.value = (de_ep_raw){copy, base.len};
    de_ep_error code = DE_EP_OK;
    char msg[256];
    const u32 tx =
        de__ed_edit_run(ed, a, q.has_tx ? q.tx : 0, instance_scene(inst->root), &op, 1, &code, msg, sizeof msg);
    if (tx == 0) {
        prefab_error(w, req->id, code, "edit", "%s", msg);
        return;
    }
    const de_ep_revert_result res = {.tx = tx, .revision = g_prefab.revision};
    de_ep_write_response_prefab_revert_override(w, req->id, &res);
}

/* One `defs` or `bases` entry of a prefab.state value. */
typedef struct pf_entry {
    char asset[256];
    u64 sid, root, entity;
    char path[128];
    de_ep_raw val;
    bool follow;
} pf_entry;

static u32 state_read(mpack_reader_t *r, de_ep_arena *a, pf_entry **out) {
    const u32 n = mpack_expect_array_max(r, ED_PLAN_MAX);
    pf_entry *e = n ? (pf_entry *)de__ed_alloc(a, sizeof(pf_entry) * n) : NULL;
    if (n && e == NULL)
        mpack_reader_flag_error(r, mpack_error_memory);
    for (u32 i = 0; i < n && mpack_reader_error(r) == mpack_ok; ++i) {
        memset(&e[i], 0, sizeof e[i]);
        const u32 keys = mpack_expect_map(r);
        for (u32 k = 0; k < keys && mpack_reader_error(r) == mpack_ok; ++k) {
            char key[16];
            mpack_expect_cstr(r, key, sizeof key);
            if (strcmp(key, "asset") == 0) {
                mpack_expect_cstr(r, e[i].asset, sizeof e[i].asset);
            } else if (strcmp(key, "path") == 0) {
                mpack_expect_cstr(r, e[i].path, sizeof e[i].path);
            } else if (strcmp(key, "sid") == 0) {
                e[i].sid = mpack_expect_u64(r);
            } else if (strcmp(key, "root") == 0) {
                e[i].root = mpack_expect_u64(r);
            } else if (strcmp(key, "entity") == 0) {
                e[i].entity = mpack_expect_u64(r);
            } else if (strcmp(key, "follow") == 0) {
                e[i].follow = mpack_expect_bool(r);
            } else if (strcmp(key, "value") == 0) {
                const char *begin = r->data;
                mpack_discard(r);
                e[i].val = (de_ep_raw){begin, (size_t)(r->data - begin)};
            } else {
                mpack_discard(r);
            }
        }
        mpack_done_map(r);
    }
    mpack_done_array(r);
    *out = e;
    return mpack_reader_error(r) == mpack_ok ? n : 0;
}

/* One `files` entry: the exact bytes to write, and the hash the file must have now. */
typedef struct pf_snap {
    char asset[256];
    de_ep_raw bytes;
    bool has_expect;
    u64 expect;
} pf_snap;

static u32 snaps_read(mpack_reader_t *r, de_ep_arena *a, pf_snap **out) {
    const u32 n = mpack_expect_array_max(r, ED_FILES_MAX);
    pf_snap *e = n ? (pf_snap *)de__ed_alloc(a, sizeof(pf_snap) * n) : NULL;
    if (n && e == NULL)
        mpack_reader_flag_error(r, mpack_error_memory);
    for (u32 i = 0; i < n && mpack_reader_error(r) == mpack_ok; ++i) {
        memset(&e[i], 0, sizeof e[i]);
        const u32 keys = mpack_expect_map(r);
        for (u32 k = 0; k < keys && mpack_reader_error(r) == mpack_ok; ++k) {
            char key[16];
            mpack_expect_cstr(r, key, sizeof key);
            if (strcmp(key, "asset") == 0) {
                mpack_expect_cstr(r, e[i].asset, sizeof e[i].asset);
            } else if (strcmp(key, "bytes") == 0) {
                const u32 len = mpack_expect_bin(r);
                const char *p = len ? mpack_read_bytes_inplace(r, len) : "";
                e[i].bytes = (de_ep_raw){p, len};
                mpack_done_bin(r);
            } else if (strcmp(key, "expect") == 0) {
                e[i].has_expect = true;
                e[i].expect = mpack_expect_u64(r);
            } else {
                mpack_discard(r);
            }
        }
        mpack_done_map(r);
    }
    mpack_done_array(r);
    *out = e;
    return mpack_reader_error(r) == mpack_ok ? n : 0;
}

typedef struct pf_file {
    const char *asset;
    char vpath[1024];
    void *old;
    u64 old_len;
    const char *next;
    size_t next_len;
    char *owned;
    size_t owned_len;
    bool write;
} pf_file;

static void files_release(pf_file *f, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        if (f[i].old != NULL)
            DE_FREE(de_heap_allocator(), f[i].old, f[i].old_len + 1);
        de__ed_prefab_source_free(f[i].owned, f[i].owned_len);
        f[i].old = NULL;
        f[i].owned = NULL;
    }
}

static bool files_fail(de_ep_error *code, char *msg, size_t cap, de_ep_error c, const char *reason,
                       const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
    *code = c;
    g_files.reason = reason;
    return false;
}

/* Reads every source the op touches and works out its new bytes; nothing is written yet. */
static bool files_prepare(de__editor *ed, de_ep_arena *a, const pf_entry *defs, u32 nd,
                          const pf_snap *snaps, u32 ns, bool replay, pf_file *files, u32 *nf,
                          de_ep_error *code, char *msg, size_t cap) {
    *nf = 0;
    const u32 want = replay ? ns : nd;
    for (u32 i = 0; i < want; ++i) {
        const char *asset = replay ? snaps[i].asset : defs[i].asset;
        bool seen = false;
        for (u32 k = 0; k < *nf && !seen; ++k)
            seen = strcmp(files[k].asset, asset) == 0;
        if (seen)
            continue;
        pf_file *f = &files[(*nf)++];
        memset(f, 0, sizeof *f);
        f->asset = asset;
        if (!de__ed_asset_source_vpath(ed, asset, f->vpath, sizeof f->vpath) ||
            de_file_read_all(f->vpath, de_heap_allocator(), &f->old, &f->old_len) != DE_OK) {
            de_error_clear();
            f->old = NULL;
            return files_fail(code, msg, cap, DE_EP_ERR_UNSUPPORTED, "no_source",
                              "prefab %s has no readable source file", asset);
        }
        if (replay) {
            if (snaps[i].has_expect && de_hash64(f->old, (size_t)f->old_len, 0) != snaps[i].expect)
                return files_fail(code, msg, cap, DE_EP_ERR_CONFLICT, "source_changed",
                                  "%s changed outside the editor", f->vpath);
            f->next = snaps[i].bytes.ptr;
            f->next_len = snaps[i].bytes.len;
        } else {
            de__ed_src_edit *edits = (de__ed_src_edit *)de__ed_alloc(a, sizeof(de__ed_src_edit) * nd);
            if (edits == NULL)
                return files_fail(code, msg, cap, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
            u32 ne = 0;
            for (u32 k = 0; k < nd; ++k)
                if (strcmp(defs[k].asset, asset) == 0 && strcmp(defs[k].path, "@unnest") != 0)
                    edits[ne++] = (de__ed_src_edit){defs[k].sid, defs[k].path, defs[k].val.ptr,
                                                    defs[k].val.len, defs[k].follow};
            char why[200];
            const char *reason =
                de__ed_prefab_source_edit(ed->e->types, (const char *)f->old, (size_t)f->old_len,
                                          edits, ne, &f->owned, &f->owned_len, why, sizeof why);
            if (reason != NULL)
                return files_fail(code, msg, cap,
                                  strcmp(reason, "memory") == 0 ? DE_EP_ERR_BUSY
                                                                : DE_EP_ERR_UNSUPPORTED,
                                  reason, "%s: %s", f->vpath, why);
            f->next = f->owned;
            f->next_len = f->owned_len;
        }
        f->write = f->next_len != f->old_len || memcmp(f->next, f->old, f->next_len) != 0;
    }
    /* An atomic replace can rename over a read-only file: ask for write access first. */
    for (u32 i = 0; i < *nf; ++i) {
        de_file *h = NULL;
        if (!files[i].write)
            continue;
        if (de_file_open(files[i].vpath, DE_FILE_APPEND, &h) != DE_OK) {
            de_error_clear();
            return files_fail(code, msg, cap, DE_EP_ERR_INTERNAL, "write", "%s is not writable",
                              files[i].vpath);
        }
        de_file_close(h);
    }
    return true;
}

/* Writes the new bytes; on a failure puts back the files already written. */
static bool files_commit(pf_file *files, u32 nf, de_ep_error *code, char *msg, size_t cap) {
    for (u32 i = 0; i < nf; ++i) {
        if (!files[i].write)
            continue;
        if (de_file_write_all_atomic(files[i].vpath, files[i].next, files[i].next_len) == DE_OK)
            continue;
        de_error_clear();
        for (u32 k = 0; k < i; ++k)
            if (files[k].write &&
                de_file_write_all_atomic(files[k].vpath, files[k].old, files[k].old_len) != DE_OK)
                de_error_clear();
        return files_fail(code, msg, cap, DE_EP_ERR_INTERNAL, "write", "could not write %s",
                          files[i].vpath);
    }
    return true;
}

/* DENG-811 entity entries of prefab.state. Path "" sets an entity's whole block: nil removes
 * it, {parent, components} adds it, {vals: [[path, value], ...]} puts back exactly what was
 * there. "@nest" adds a pair to a [[prefab.nested]] entry; "@unnest" takes it away again. */
/* {root, inner, outer} of a "@nest" value. */
static bool nest_read(de_ep_raw v, u64 *root, u64 *inner, u64 *outer) {
    mpack_reader_t r;
    mpack_reader_init_data(&r, v.ptr, v.len);
    const u32 keys = mpack_expect_map_max(&r, 8);
    *root = *inner = *outer = 0;
    for (u32 k = 0; k < keys && mpack_reader_error(&r) == mpack_ok; ++k) {
        char key[8];
        mpack_expect_cstr(&r, key, sizeof key);
        const u64 x = mpack_expect_u64(&r);
        if (strcmp(key, "root") == 0)
            *root = x;
        else if (strcmp(key, "inner") == 0)
            *inner = x;
        else if (strcmp(key, "outer") == 0)
            *outer = x;
    }
    mpack_done_map(&r);
    return mpack_reader_destroy(&r) == mpack_ok;
}

static ed_nest *nest_of(ed_def *d, u64 root) {
    for (u32 k = 0; k < d->nest_count; ++k)
        if (d->nest[k].root == root)
            return &d->nest[k];
    return NULL;
}

/* Adds (inner, outer) to `n`, or removes the last such pair. */
static bool nest_change(ed_nest *n, u64 inner, u64 outer, bool add) {
    const de_allocator *al = de_heap_allocator();
    u32 drop = n->count;
    if (!add) {
        for (u32 i = n->count; i-- > 0 && drop == n->count;)
            if (n->inner[i] == inner && n->outer[i] == outer)
                drop = i;
        if (drop == n->count)
            return false;
    }
    const u32 count = add ? n->count + 1 : n->count - 1;
    u64 *block = NULL;
    if (count != 0) {
        block = (u64 *)DE_ALLOC(al, sizeof(u64) * 2 * count, 8, DE_MEM_GENERAL);
        if (block == NULL)
            return false;
        u32 k = 0;
        for (u32 i = 0; i < n->count; ++i) {
            if (i == drop)
                continue;
            block[k] = n->inner[i];
            block[count + k] = n->outer[i];
            ++k;
        }
        if (add) {
            block[k] = inner;
            block[count + k] = outer;
        }
    }
    if (n->inner != NULL)
        DE_FREE(al, n->inner, sizeof(u64) * 2 * n->count);
    n->inner = block;
    n->outer = block != NULL ? block + count : NULL;
    n->count = count;
    return true;
}

static bool nest_has(const ed_nest *n, u64 inner, u64 outer) {
    for (u32 i = 0; i < n->count; ++i)
        if (n->inner[i] == inner && n->outer[i] == outer)
            return true;
    return false;
}

/* Every value of (d, sid) as {vals: [[path, value], ...]}, or nil when it has none. */
static bool entity_vals(de_ep_arena *a, const ed_def *d, u64 sid, de_ep_raw *out) {
    if (def_get(d, sid, "") == NULL) {
        *out = (de_ep_raw){k_nil, 1};
        return true;
    }
    u32 n = 0;
    for (u32 i = 0; i < d->val_count; ++i)
        n += d->vals[i].sid == sid;
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 1);
    mpack_write_cstr(&mw, "vals");
    mpack_start_array(&mw, n);
    for (u32 i = 0; i < d->val_count; ++i) {
        const ed_dval *v = &d->vals[i];
        if (v->sid != sid)
            continue;
        const de_ep_raw raw = dval_raw(d, v);
        mpack_start_array(&mw, 2);
        mpack_write_cstr(&mw, v->path);
        mpack_write_object_bytes(&mw, raw.ptr, raw.len);
        mpack_finish_array(&mw);
    }
    mpack_finish_array(&mw);
    mpack_finish_map(&mw);
    *out = de__ed_raw_end(a, &mw);
    return out->len != 0;
}

/* Sets the whole block of (d, sid) from an entity entry's value. */
static bool entity_set(ed_def *d, u64 sid, de_ep_raw val) {
    u32 k = 0;
    for (u32 i = 0; i < d->val_count; ++i)
        if (d->vals[i].sid != sid)
            d->vals[k++] = d->vals[i];
    d->val_count = k;
    if (is_nil(val))
        return true;
    mpack_reader_t r;
    mpack_reader_init_data(&r, val.ptr, val.len);
    const u32 keys = mpack_expect_map_max(&r, 8);
    bool ok = true, marker = false;
    u64 parent = 0;
    for (u32 i = 0; i < keys && ok && mpack_reader_error(&r) == mpack_ok; ++i) {
        char key[16];
        mpack_expect_cstr(&r, key, sizeof key);
        if (strcmp(key, "vals") == 0) {
            const u32 n = mpack_expect_array_max(&r, 65536);
            for (u32 j = 0; j < n && ok && mpack_reader_error(&r) == mpack_ok; ++j) {
                char path[128];
                mpack_expect_array_match(&r, 2);
                mpack_expect_cstr(&r, path, sizeof path);
                const char *begin = r.data;
                mpack_discard(&r);
                if (mpack_reader_error(&r) == mpack_ok)
                    ok = def_put(d, sid, path, begin, (size_t)(r.data - begin));
                mpack_done_array(&r);
            }
            mpack_done_array(&r);
        } else if (strcmp(key, "parent") == 0) {
            parent = mpack_expect_u64(&r);
        } else if (strcmp(key, "components") == 0) {
            if (!marker) {
                char buf[16];
                mpack_writer_t pw;
                mpack_writer_init(&pw, buf, sizeof buf);
                if (parent != 0)
                    mpack_write_u64(&pw, parent);
                else
                    mpack_write_nil(&pw);
                const size_t len = mpack_writer_buffer_used(&pw);
                ok = mpack_writer_destroy(&pw) == mpack_ok && def_put(d, sid, "", buf, len);
                marker = true;
            }
            const u32 n = mpack_expect_map_max(&r, 1024);
            for (u32 j = 0; j < n && ok && mpack_reader_error(&r) == mpack_ok; ++j) {
                de_ep_kv_raw kv;
                if (!de__ed_raw_map_entry(&r, &kv))
                    break;
                char comp[128];
                snprintf(comp, sizeof comp, "%.*s", (int)kv.key.len, kv.key.ptr);
                ok = def_comp_set(d, sid, comp, kv.value);
            }
            mpack_done_map(&r);
        } else {
            mpack_discard(&r);
        }
    }
    mpack_done_map(&r);
    return mpack_reader_destroy(&r) == mpack_ok && ok;
}

/* The instance rooted at `root` links `sid` to `entity` (0: not at all). */
static bool link_set(ed_instance *in, u64 sid, u64 entity) {
    u32 k = 0;
    for (u32 i = 0; i < in->local_count; ++i)
        if (in->local_sid[i] != sid) {
            in->local_sid[k] = in->local_sid[i];
            in->local_ent[k] = in->local_ent[i];
            ++k;
        }
    in->local_count = k;
    if (entity == 0)
        return true;
    if (in->local_sid == NULL) {
        u64 *block = (u64 *)DE_ALLOC(de_heap_allocator(), sizeof(u64) * ED_LOCAL_CAP * 2, 8,
                                     DE_MEM_GENERAL);
        if (block == NULL)
            return false;
        in->local_sid = block;
        in->local_ent = block + ED_LOCAL_CAP;
    }
    if (in->local_count == ED_LOCAL_CAP)
        return false;
    in->local_sid[in->local_count] = sid;
    in->local_ent[in->local_count] = entity;
    ++in->local_count;
    return true;
}

bool de__ed_prefab_state(de__editor *ed, de_ep_arena *a, de_ep_raw value, de_ep_raw *inv,
                         de_ep_error *code, char *msg, size_t msg_cap) {
    pf_entry *defs = NULL, *bases = NULL, *links = NULL;
    pf_snap *snaps = NULL;
    u32 nd = 0, nb = 0, ns = 0, nl = 0;
    bool replay = false;
    g_files.reason = NULL;
    mpack_reader_t r;
    mpack_reader_init_data(&r, value.ptr, value.len);
    const u32 keys = mpack_expect_map(&r);
    for (u32 k = 0; k < keys && mpack_reader_error(&r) == mpack_ok; ++k) {
        char key[16];
        mpack_expect_cstr(&r, key, sizeof key);
        if (strcmp(key, "defs") == 0)
            nd = state_read(&r, a, &defs);
        else if (strcmp(key, "links") == 0)
            nl = state_read(&r, a, &links);
        else if (strcmp(key, "bases") == 0)
            nb = state_read(&r, a, &bases);
        else if (strcmp(key, "files") == 0) {
            replay = true;
            ns = snaps_read(&r, a, &snaps);
        } else
            mpack_discard(&r);
    }
    mpack_done_map(&r);
    if (mpack_reader_destroy(&r) != mpack_ok) {
        *code = DE_EP_ERR_INVALID_PARAMS;
        snprintf(msg, msg_cap, "prefab.state value does not decode");
        return false;
    }
    /* Check everything first, so a failure changes nothing. */
    for (u32 i = 0; i < nd; ++i) {
        ed_def *d = def_find(defs[i].asset);
        bool there = d != NULL;
        if (there && defs[i].path[0] == '\0') {
            there = true;
        } else if (there && defs[i].path[0] == '@') {
            u64 root = 0, inner = 0, outer = 0;
            const ed_nest *n = nest_read(defs[i].val, &root, &inner, &outer) ? nest_of(d, root)
                                                                             : NULL;
            there = n != NULL && (defs[i].path[1] == 'n' || nest_has(n, inner, outer));
        } else if (there) {
            there = is_comp_path(defs[i].path) ? def_has_sid(d, defs[i].sid)
                                               : def_get(d, defs[i].sid, defs[i].path) != NULL;
        }
        if (!there) {
            *code = DE_EP_ERR_NOT_FOUND;
            snprintf(msg, msg_cap, "prefab %s has no %s", defs[i].asset, defs[i].path);
            return false;
        }
    }
    for (u32 i = 0; i < nb; ++i) {
        ed_instance *in = instance_rooted(bases[i].root);
        u64 lsid = 0;
        if (in == NULL || (is_comp_path(bases[i].path)
                               ? !local_sid_of(in, bases[i].entity, &lsid)
                               : base_find(in, bases[i].entity, de_ep_cstr(bases[i].path)) == NULL)) {
            *code = DE_EP_ERR_NOT_FOUND;
            snprintf(msg, msg_cap, "instance %llu has no %s", (unsigned long long)bases[i].root,
                     bases[i].path);
            return false;
        }
    }
    for (u32 i = 0; i < nl; ++i)
        if (instance_rooted(links[i].root) == NULL) {
            *code = DE_EP_ERR_NOT_FOUND;
            snprintf(msg, msg_cap, "no instance %llu", (unsigned long long)links[i].root);
            return false;
        }
    /* The sources: read and edited, then checked for write access. */
    pf_file *files = NULL;
    u32 nf = 0;
    const u32 fcap = replay ? ns : nd;
    if (fcap != 0) {
        files = (pf_file *)de__ed_alloc(a, sizeof(pf_file) * fcap);
        if (files == NULL)
            return files_fail(code, msg, msg_cap, DE_EP_ERR_BUSY, "memory",
                              "out of scratch memory");
    }
    if (!files_prepare(ed, a, defs, nd, snaps, ns, replay, files, &nf, code, msg, msg_cap)) {
        files_release(files, nf);
        return false;
    }
    /* The old values of whole components (DENG-810), before the inverse's writer opens. */
    de_ep_raw *dold = nd ? (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * nd) : NULL;
    de_ep_raw *bold = nb ? (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * nb) : NULL;
    u64 *lold = nl ? (u64 *)de__ed_alloc(a, sizeof(u64) * nl) : NULL;
    bool olds = (nd == 0 || dold != NULL) && (nb == 0 || bold != NULL) && (nl == 0 || lold != NULL);
    for (u32 i = 0; i < nl && olds; ++i) {
        lold[i] = 0;
        (void)local_ent_of(instance_rooted(links[i].root), links[i].sid, &lold[i]);
    }
    for (u32 i = 0; i < nd && olds; ++i) {
        const ed_def *d = def_find(defs[i].asset);
        if (defs[i].path[0] == '\0')
            olds = entity_vals(a, d, defs[i].sid, &dold[i]);
        else if (defs[i].path[0] == '@')
            dold[i] = defs[i].val;
        else if (is_comp_path(defs[i].path))
            olds = def_comp_map(a, d, defs[i].sid, defs[i].path, &dold[i]);
        else
            dold[i] = dval_raw(d, def_get(d, defs[i].sid, defs[i].path));
    }
    for (u32 i = 0; i < nb && olds; ++i) {
        ed_instance *in = instance_rooted(bases[i].root);
        if (is_comp_path(bases[i].path))
            olds = base_comp_map(a, in, bases[i].entity, bases[i].path, &bold[i]);
        else
            bold[i] = base_raw(in, base_find(in, bases[i].entity, de_ep_cstr(bases[i].path)));
    }
    if (!olds) {
        files_release(files, nf);
        return files_fail(code, msg, msg_cap, DE_EP_ERR_BUSY, "memory", "out of scratch memory");
    }
    /* The inverse: the same entries with the old values, and the old bytes of each file. */
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 4);
    mpack_write_cstr(&mw, "files");
    u32 nw = 0;
    for (u32 i = 0; i < nf; ++i)
        nw += files[i].write;
    mpack_start_array(&mw, nw);
    for (u32 i = 0; i < nf; ++i) {
        if (!files[i].write)
            continue;
        mpack_start_map(&mw, 3);
        mpack_write_cstr(&mw, "asset");
        mpack_write_cstr(&mw, files[i].asset);
        mpack_write_cstr(&mw, "bytes");
        mpack_write_bin(&mw, (const char *)files[i].old, (u32)files[i].old_len);
        mpack_write_cstr(&mw, "expect");
        mpack_write_u64(&mw, de_hash64(files[i].next, files[i].next_len, 0));
        mpack_finish_map(&mw);
    }
    mpack_finish_array(&mw);
    mpack_write_cstr(&mw, "defs");
    mpack_start_array(&mw, nd);
    for (u32 i = 0; i < nd; ++i) {
        const de_ep_raw old = dold[i];
        mpack_start_map(&mw, 5);
        mpack_write_cstr(&mw, "asset");
        mpack_write_cstr(&mw, defs[i].asset);
        mpack_write_cstr(&mw, "sid");
        mpack_write_u64(&mw, defs[i].sid);
        mpack_write_cstr(&mw, "path");
        mpack_write_cstr(&mw, strcmp(defs[i].path, "@nest") == 0     ? "@unnest"
                              : strcmp(defs[i].path, "@unnest") == 0 ? "@nest"
                                                                     : defs[i].path);
        mpack_write_cstr(&mw, "value");
        mpack_write_object_bytes(&mw, old.ptr, old.len);
        mpack_write_cstr(&mw, "follow");
        mpack_write_bool(&mw, defs[i].follow);
        mpack_finish_map(&mw);
    }
    mpack_finish_array(&mw);
    mpack_write_cstr(&mw, "bases");
    mpack_start_array(&mw, nb);
    for (u32 i = 0; i < nb; ++i) {
        const de_ep_raw old = bold[i];
        mpack_start_map(&mw, 4);
        mpack_write_cstr(&mw, "root");
        mpack_write_u64(&mw, bases[i].root);
        mpack_write_cstr(&mw, "entity");
        mpack_write_u64(&mw, bases[i].entity);
        mpack_write_cstr(&mw, "path");
        mpack_write_cstr(&mw, bases[i].path);
        mpack_write_cstr(&mw, "value");
        mpack_write_object_bytes(&mw, old.ptr, old.len);
        mpack_finish_map(&mw);
    }
    mpack_finish_array(&mw);
    mpack_write_cstr(&mw, "links");
    mpack_start_array(&mw, nl);
    for (u32 i = 0; i < nl; ++i) {
        mpack_start_map(&mw, 3);
        mpack_write_cstr(&mw, "root");
        mpack_write_u64(&mw, links[i].root);
        mpack_write_cstr(&mw, "sid");
        mpack_write_u64(&mw, links[i].sid);
        mpack_write_cstr(&mw, "entity");
        mpack_write_u64(&mw, lold[i]);
        mpack_finish_map(&mw);
    }
    mpack_finish_array(&mw);
    mpack_finish_map(&mw);
    *inv = de__ed_raw_end(a, &mw);
    if (inv->len == 0) {
        files_release(files, nf);
        return files_fail(code, msg, msg_cap, DE_EP_ERR_BUSY, "too_large",
                          "the prefab sources do not fit in scratch memory");
    }
    if (!files_commit(files, nf, code, msg, msg_cap)) {
        files_release(files, nf);
        return false;
    }
    /* Recook what was written (a no-op with hot reload off). */
    for (u32 i = 0; i < nf; ++i) {
        if (!files[i].write)
            continue;
        if (ed->e->assets != NULL)
            (void)de__asset_reload_queue(ed->e->assets, de_asset_id_from(files[i].asset));
        ed_def *wd = def_find(files[i].asset);
        if (wd != NULL) {
            wd->src_known = true;
            wd->src_hash = de_hash64(files[i].next, files[i].next_len, 0);
        }
    }
    files_release(files, nf);
    bool ok = true;
    for (u32 i = 0; i < nd; ++i) {
        ed_def *d = def_find(defs[i].asset);
        if (defs[i].path[0] == '\0') {
            ok &= entity_set(d, defs[i].sid, defs[i].val);
        } else if (defs[i].path[0] == '@') {
            u64 root = 0, inner = 0, outer = 0;
            ed_nest *n = nest_read(defs[i].val, &root, &inner, &outer) ? nest_of(d, root) : NULL;
            ok &= n != NULL && nest_change(n, inner, outer, defs[i].path[1] == 'n');
        } else {
            ok &= is_comp_path(defs[i].path)
                      ? def_comp_set(d, defs[i].sid, defs[i].path, defs[i].val)
                      : def_put(d, defs[i].sid, defs[i].path, defs[i].val.ptr, defs[i].val.len);
        }
    }
    for (u32 i = 0; i < nb; ++i) {
        ed_instance *in = instance_rooted(bases[i].root);
        ok &= is_comp_path(bases[i].path)
                  ? base_comp_set(in, bases[i].entity, bases[i].path, bases[i].val)
                  : base_put(in, base_find(in, bases[i].entity, de_ep_cstr(bases[i].path)),
                             bases[i].val.ptr, bases[i].val.len);
    }
    for (u32 i = 0; i < nl; ++i)
        ok &= link_set(instance_rooted(links[i].root), links[i].sid, links[i].entity);
    for (u32 i = 0; i < nl; ++i) {
        bool seen = false;
        for (u32 k = 0; k < i && !seen; ++k)
            seen = links[k].root == links[i].root;
        if (!seen)
            instance_rebuild(ed, instance_rooted(links[i].root), a);
    }
    ++g_prefab.revision;
    if (!ok) {
        *code = DE_EP_ERR_BUSY;
        snprintf(msg, msg_cap, "out of memory");
    }
    return ok;
}

/* scene.dirty arrives from edits; keep our flag in step. */
void de__ed_scene_mark_dirty(u64 scene);
/* prefab.unpack (DENG-801): the instance becomes plain entities and its record goes. One undoable
 * transaction; undo puts the record back with its overrides. The prefab itself does not change,
 * so the revision stays. */
static void on_unpack(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    de_ep_prefab_unpack_params q;
    memset(&q, 0, sizeof q);
    if (!de_ep_read_params_prefab_unpack(req, a, &q)) {
        prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "params", "bad params");
        return;
    }
    if (reject_state(ed, req->id, w, q.has_revision, q.revision))
        return;
    /* Only an instance root: an entity inside the instance is not one. */
    if (instance_rooted(q.entity) == NULL) {
        prefab_error(w, req->id, DE_EP_ERR_NOT_FOUND, "not_instance",
                     "%llu is not a prefab instance root", (unsigned long long)q.entity);
        return;
    }
    de_ep_edit_op op;
    memset(&op, 0, sizeof op);
    op.op = de_ep_cstr("prefab.unpack");
    op.has_entity = true;
    op.entity = q.entity;
    de_ep_error code = DE_EP_OK;
    char msg[256];
    const u32 tx = de__ed_edit_run(ed, a, q.has_tx ? q.tx : 0, instance_scene(q.entity), &op, 1,
                                   &code, msg, sizeof msg);
    if (tx == 0) {
        prefab_error(w, req->id, code, "edit", "%s", msg);
        return;
    }
    const de_ep_unpack_result res = {.tx = tx, .revision = g_prefab.revision, .root = q.entity};
    de_ep_write_response_prefab_unpack(w, req->id, &res);
}

void de__ed_scene_mark_dirty(u64 scene) {
    ed_scene *s = scene_find(scene);
    if (s != NULL)
        s->dirty = true;
}

void de__ed_scene_register(de__editor *ed) {
    prefab_defs_free();
    memset(&g_scene, 0, sizeof g_scene);
    ed->scene = (struct de__ed_scene *)&g_scene;
    g_scene.next_scene = ED_GAME_SCENE;
    ed_scene *game = &g_scene.scenes[0];
    game->id = ED_GAME_SCENE;
    snprintf(game->path, sizeof game->path, "%s", ed->h->config.name);
    game->active = true;
    de__ed_on(ed, DE_EP_METHOD_SCENE_LIST, on_list);
    de__ed_on(ed, DE_EP_METHOD_SCENE_LOAD, on_load);
    de__ed_on(ed, DE_EP_METHOD_SCENE_UNLOAD, on_unload);
    de__ed_on(ed, DE_EP_METHOD_SCENE_SERIALIZE, on_serialize);
    de__ed_on(ed, DE_EP_METHOD_SCENE_SET_ACTIVE, on_set_active);
    de__ed_on(ed, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, on_reload);
    de__ed_on(ed, DE_EP_METHOD_PREFAB_INSTANTIATE, on_instantiate);
    de__ed_on(ed, DE_EP_METHOD_PREFAB_OVERRIDES, on_overrides);
    de__ed_on(ed, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, on_apply);
    de__ed_on(ed, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, on_revert);
    de__ed_on(ed, DE_EP_METHOD_PREFAB_UNPACK, on_unpack);
}
