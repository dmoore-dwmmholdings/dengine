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
    char path[128]; /* "Component.field" */
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

/* Spawns the entities of scene text into `scene`, in scene-ID order. False with `err` set on a
 * failure; nothing stays spawned then. */
static bool spawn_text(de__editor *ed, const char *text, size_t len, u64 scene, scene_err *err) {
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

static int cmp_cstr(const void *x, const void *y) {
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

/* The entities of `scene` (or `only`, when given) as scene text into `out` (DENG-570,
 * DIDE-R147): entities in scene-ID order, each `[entity.0x<16 hex>]` with its `parent`, then its
 * serialized components as `[entity.0x<16 hex>.<Type>]` in type-name order, fields in reflection
 * order, a blank line between entities. The same world always gives the same bytes, and moving
 * one entity changes only that entity's transform lines. */
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
        if (g_scene.members[i].scene == scene)
            de_entity_despawn(ed->e->world, (de_entity){g_scene.members[i].entity});
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
        const bool ok = spawn_text(ed, text, len, id, &err);
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
    if (!spawn_text(ed, q.text.ptr, q.text.len, q.scene, &err)) {
        if (old != NULL)
            DE_FREE(al, old, sizeof(u64) * (old_count + 1));
        scene_error_reply(w, req->id, NULL, &err);
        return;
    }
    for (u32 i = 0; i < old_count; ++i)
        de_entity_despawn(ed->e->world, (de_entity){old[i]});
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

/* "Component.field" values of every entity of the tree, as MessagePack, into `inst`. `scratch`
 * is the free part of the caller's arena: what the caller keeps there stays. */
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
        for (u32 c = 0; c < row.components.count; ++c) {
            mpack_reader_t reader;
            mpack_reader_init_data(&reader, row.components.items[c].value.ptr,
                                   row.components.items[c].value.len);
            const u32 fields = mpack_expect_map(&reader);
            if (mpack_reader_error(&reader) == mpack_ok) {
                for (u32 f = 0; f < fields; ++f) {
                    de_ep_kv_raw field;
                    if (!de__ed_raw_map_entry(&reader, &field))
                        break;
                    if (count == cap) {
                        ed_base *g =
                            (ed_base *)DE_ALLOC(al, sizeof(ed_base) * cap * 2, 8, DE_MEM_GENERAL);
                        if (g == NULL) {
                            mpack_reader_flag_error(&reader, mpack_error_memory);
                            break;
                        }
                        memcpy(g, base, sizeof(ed_base) * count);
                        DE_FREE(al, base, sizeof(ed_base) * cap);
                        base = g;
                        cap *= 2;
                    }
                    ed_base *b = &base[count++];
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

static void prefab_defs_free(void) {
    const de_allocator *al = de_heap_allocator();
    for (u32 i = 0; i < g_prefab.count; ++i) {
        ed_def *d = g_prefab.defs[i];
        if (d->vals != NULL)
            DE_FREE(al, d->vals, sizeof(ed_dval) * d->val_cap);
        if (d->bytes != NULL)
            DE_FREE(al, d->bytes, d->cap);
        for (u32 k = 0; k < d->nest_count; ++k)
            if (d->nest[k].inner != NULL)
                DE_FREE(al, d->nest[k].inner, sizeof(u64) * 2 * d->nest[k].count);
        if (d->nest != NULL)
            DE_FREE(al, d->nest, sizeof(ed_nest) * d->nest_count);
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

/* Each field is written as the editor writes it, so the bytes compare exactly. */
static void def_visit_fn(void *user, u64 sid, de_type_id type, const char *component,
                         const void *obj) {
    def_visit *v = (def_visit *)user;
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

/* The `[prefab]` table of the prefab's source, if it has one. */
static void def_links(de__editor *ed, ed_def *d) {
    const de_allocator *al = de_heap_allocator();
    void *src = NULL;
    u64 size = 0;
    if (!de__ed_asset_source(ed, d->asset, &src, &size))
        return;
    toml_result_t tr = de__toml_parse(al, (const char *)src, (int)size);
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
    const de_asset_id id = de_asset_id_from(asset);
    const de_prefab p = de_asset_prefab(s, id);
    if (DE_HANDLE_VALID(p)) {
        de_load_handle lh = de_asset_load(s, id, 0);
        if (de_asset_wait(s, lh, 10000000000ULL) == DE_LOAD_READY) {
            def_visit v = {ed, d};
            (void)de__prefab_visit(s, ed->e->world, p, def_visit_fn, &v);
        }
        de_asset_release(s, lh);
    }
    de_error_clear();
    def_links(ed, d);
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
    u64 ents[1024];
    const u32 n = tree_of(ed, *root, ents, 1024);
    for (u32 i = 0; i < n; ++i)
        member_add(ents[i], scene, de_scene_id_new());
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
    const u32 count = inst->base_count;
    typedef struct pf_row {
        const ed_base *b;
        de_ep_raw cur;
        ed_level lv[ED_CHAIN_MAX];
        u32 levels, source;
    } pf_row;
    pf_row *rows = count ? (pf_row *)de__ed_alloc(a, sizeof(pf_row) * count) : NULL;
    u32 n = 0;
    for (u32 i = 0; i < inst->base_count && rows != NULL; ++i) {
        const ed_base *b = &inst->base[i];
        bool in_tree = false;
        for (u32 k = 0; k < tn && !in_tree; ++k)
            in_tree = tree[k] == b->entity;
        de_ep_raw cur;
        if (!in_tree || !current_value(ed, a, b->entity, b->path, &cur) ||
            raw_eq(cur, base_raw(inst, b)))
            continue;
        pf_row *row = &rows[n++];
        row->b = b;
        row->cur = cur;
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
            .path = de_ep_cstr(row->b->path),
            .value = row->cur,
            .entity = row->b->entity,
            .base = base_raw(inst, row->b),
            .source = de_ep_cstr(row->source < row->levels ? row->lv[row->source].d->asset
                                                           : inst->asset),
            .chain = {chain, row->levels},
            .ids = {ids, row->levels},
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

/* prefab.applyOverride: the instance's value becomes the value of `target` (default: the
 * instance's prefab) and of every level between. The change flows on to the variants and
 * nesting prefabs that did not set the field themselves, and to every instance that had the old
 * value. All of it is one undoable transaction. */
static void on_apply(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    de_ep_override_path_params q;
    memset(&q, 0, sizeof q);
    if (!de_ep_read_params_prefab_apply_override(req, a, &q)) {
        prefab_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "params", "bad params");
        return;
    }
    if (reject_request(ed, req, w, &q))
        return;
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
    const u64 root = inst->root;
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
    const u32 tx = de__ed_edit_run(ed, a, q.has_tx ? q.tx : 0, instance_scene(root), ops, no, &code, msg, sizeof msg);
    if (tx == 0) {
        prefab_error(w, req->id, code, g_files.reason ? g_files.reason : "edit", "%s", msg);
        return;
    }
    /* `files` (protocol 1.19) is the source path of each entry of `prefabs`, in the same order:
     * the manifest's, relative to the asset root ("prefabs/crate.prefab.toml"). */
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
                if (strcmp(defs[k].asset, asset) == 0)
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

bool de__ed_prefab_state(de__editor *ed, de_ep_arena *a, de_ep_raw value, de_ep_raw *inv,
                         de_ep_error *code, char *msg, size_t msg_cap) {
    pf_entry *defs = NULL, *bases = NULL;
    pf_snap *snaps = NULL;
    u32 nd = 0, nb = 0, ns = 0;
    bool replay = false;
    g_files.reason = NULL;
    mpack_reader_t r;
    mpack_reader_init_data(&r, value.ptr, value.len);
    const u32 keys = mpack_expect_map(&r);
    for (u32 k = 0; k < keys && mpack_reader_error(&r) == mpack_ok; ++k) {
        char key[8];
        mpack_expect_cstr(&r, key, sizeof key);
        if (strcmp(key, "defs") == 0)
            nd = state_read(&r, a, &defs);
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
        const ed_def *d = def_find(defs[i].asset);
        if (d == NULL || def_get(d, defs[i].sid, defs[i].path) == NULL) {
            *code = DE_EP_ERR_NOT_FOUND;
            snprintf(msg, msg_cap, "prefab %s has no %s", defs[i].asset, defs[i].path);
            return false;
        }
    }
    for (u32 i = 0; i < nb; ++i) {
        ed_instance *in = instance_rooted(bases[i].root);
        if (in == NULL || base_find(in, bases[i].entity, de_ep_cstr(bases[i].path)) == NULL) {
            *code = DE_EP_ERR_NOT_FOUND;
            snprintf(msg, msg_cap, "instance %llu has no %s", (unsigned long long)bases[i].root,
                     bases[i].path);
            return false;
        }
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
    /* The inverse: the same entries with the old values, and the old bytes of each file. */
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 3);
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
        const ed_def *d = def_find(defs[i].asset);
        const de_ep_raw old = dval_raw(d, def_get(d, defs[i].sid, defs[i].path));
        mpack_start_map(&mw, 5);
        mpack_write_cstr(&mw, "asset");
        mpack_write_cstr(&mw, defs[i].asset);
        mpack_write_cstr(&mw, "sid");
        mpack_write_u64(&mw, defs[i].sid);
        mpack_write_cstr(&mw, "path");
        mpack_write_cstr(&mw, defs[i].path);
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
        ed_instance *in = instance_rooted(bases[i].root);
        const de_ep_raw old =
            base_raw(in, base_find(in, bases[i].entity, de_ep_cstr(bases[i].path)));
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
    }
    files_release(files, nf);
    bool ok = true;
    for (u32 i = 0; i < nd; ++i)
        ok &= def_put(def_find(defs[i].asset), defs[i].sid, defs[i].path, defs[i].val.ptr,
                      defs[i].val.len);
    for (u32 i = 0; i < nb; ++i) {
        ed_instance *in = instance_rooted(bases[i].root);
        ok &= base_put(in, base_find(in, bases[i].entity, de_ep_cstr(bases[i].path)),
                       bases[i].val.ptr, bases[i].val.len);
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
