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
    const de_result r = de_prefab_instantiate(s, ed->e->world, p, parent, root);
    de_asset_release(s, lh);
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
    }
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
    const u32 i = (u32)(inst - g_scene.instances);
    memmove(inst, inst + 1, sizeof(ed_instance) * (g_scene.instance_count - i - 1));
    --g_scene.instance_count;
}

bool de__ed_prefab_instance_restore(u64 root, const char *asset, const char *base, size_t len) {
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

static void on_overrides(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(prefab_overrides, de_ep_entity_ref, q);
    const ed_instance *inst = instance_of(ed, q.entity);
    if (inst == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "%llu is not a prefab instance",
                     (unsigned long long)q.entity);
        return;
    }
    de_ep_override_list list;
    memset(&list, 0, sizeof list);
    list.overrides.items =
        (de_ep_override *)de__ed_alloc(a, sizeof(de_ep_override) * (inst->base_count + 1));
    for (u32 i = 0; i < inst->base_count && list.overrides.items != NULL; ++i) {
        const ed_base *b = &inst->base[i];
        if (b->entity != q.entity)
            continue;
        de_ep_raw cur;
        if (!current_value(ed, a, b->entity, b->path, &cur))
            continue;
        if (cur.len == b->len && memcmp(cur.ptr, inst->bytes + b->off, b->len) == 0)
            continue;
        de_ep_override *o = &list.overrides.items[list.overrides.count++];
        o->path = de_ep_cstr(b->path);
        o->value = cur;
    }
    de_ep_write_response_prefab_overrides(w, req->id, &list);
}

static ed_base *base_find(ed_instance *inst, u64 entity, de_ep_str path) {
    for (u32 i = 0; i < inst->base_count; ++i)
        if (inst->base[i].entity == entity && strlen(inst->base[i].path) == path.len &&
            memcmp(inst->base[i].path, path.ptr, path.len) == 0)
            return &inst->base[i];
    return NULL;
}

static void apply_or_revert(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                            mpack_writer_t *w, bool apply) {
    de_ep_override_path_params q;
    memset(&q, 0, sizeof q);
    if (!(apply ? de_ep_read_params_prefab_apply_override(req, a, &q)
                : de_ep_read_params_prefab_revert_override(req, a, &q))) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "bad params");
        return;
    }
    ed_instance *inst = instance_of(ed, q.entity);
    ed_base *b = inst != NULL ? base_find(inst, q.entity, q.path) : NULL;
    if (b == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no prefab field %.*s", (int)q.path.len,
                     q.path.ptr);
        return;
    }
    if (apply) {
        /* The instance's value becomes the prefab's: it is no longer an override. */
        de_ep_raw cur;
        if (current_value(ed, a, q.entity, b->path, &cur)) {
            /* The new bytes go after the old ones. */
            u8 *grown =
                (u8 *)DE_ALLOC(de_heap_allocator(), inst->byte_count + cur.len, 8, DE_MEM_GENERAL);
            if (grown != NULL) {
                if (inst->bytes != NULL) {
                    memcpy(grown, inst->bytes, inst->byte_count);
                    DE_FREE(de_heap_allocator(), inst->bytes, inst->byte_count);
                }
                memcpy(grown + inst->byte_count, cur.ptr, cur.len);
                b->off = (u32)inst->byte_count;
                b->len = (u32)cur.len;
                inst->bytes = grown;
                inst->byte_count += cur.len;
            }
        }
        de_ep_empty e;
        memset(&e, 0, sizeof e);
        de_ep_write_response_prefab_apply_override(w, req->id, &e);
        return;
    }
    /* Revert: set the field back to the prefab's value. */
    char *path = de__ed_cstr(a, q.path);
    char *dot = path != NULL ? strchr(path, '.') : NULL;
    const de_component c =
        dot ? de__ed_component(ed, (de_ep_str){path, (u32)(dot - path)}) : (de_component){0};
    de_type_id type;
    u32 size = 0;
    u8 *data = NULL;
    const de_field_info *f = NULL;
    u32 off = 0;
    if (c.id == 0 || !de__ed_component_type(ed, c, &type, &size) ||
        (data = (u8 *)de_entity_get(ed->e->world, (de_entity){q.entity}, c)) == NULL ||
        de_reflect_resolve_path(ed->e->types, type, dot + 1, &f, &off) != DE_OK) {
        de_error_clear();
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no field %s", path ? path : "");
        return;
    }
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, (const char *)inst->bytes + b->off, b->len);
    mpack_tree_parse(&tree);
    u32 fc = 0, index = 0;
    const de_field_info *fields = de_type_fields(ed->e->types, type, &fc);
    while (index < fc && &fields[index] != f)
        ++index;
    const bool ok = mpack_tree_error(&tree) == mpack_ok &&
                    de__ed_read_field(ed, mpack_tree_root(&tree),
                                      index < fc ? type : (de_type_id){0}, index, f, data + off);
    mpack_tree_destroy(&tree);
    if (!ok) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "could not restore %s", path);
        return;
    }
    de_entity_mark_changed(ed->e->world, (de_entity){q.entity}, c);
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_prefab_revert_override(w, req->id, &e);
}

static void on_apply(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    apply_or_revert(ed, req, a, w, true);
}

static void on_revert(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    apply_or_revert(ed, req, a, w, false);
}

/* scene.dirty arrives from edits; keep our flag in step. */
void de__ed_scene_mark_dirty(u64 scene);
void de__ed_scene_mark_dirty(u64 scene) {
    ed_scene *s = scene_find(scene);
    if (s != NULL)
        s->dirty = true;
}

void de__ed_scene_register(de__editor *ed) {
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
}
