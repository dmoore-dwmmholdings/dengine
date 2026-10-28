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

#include <dengine/de_asset.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/slots_internal.h"
#include "core/de_toml_internal.h"
#include "ecs/ecs_internal.h"
#include "host/host_internal.h"
#include "reflect/ser_internal.h"

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
    u32 base_count;
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

static u64 parse_sid(const toml_datum_t *d) {
    if (d->type == TOML_INT64)
        return (u64)d->u.int64;
    if (d->type == TOML_STRING)
        return strtoull(d->u.s, NULL, 0);
    return 0;
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

/* Spawns the entities of scene text into `scene`. False with `err` set on a failure; nothing
 * stays spawned then. */
static bool spawn_text(de__editor *ed, const char *text, size_t len, u64 scene, char *err,
                       size_t err_cap) {
    de_world *w = ed->e->world;
    const de_allocator *al = de_heap_allocator();
    de__ed_note_asset_paths(text, len);
    toml_result_t tr = de__toml_parse(al, text, (int)len);
    if (!tr.ok) {
        snprintf(err, err_cap, "%s", tr.errmsg);
        de__toml_free(tr);
        return false;
    }
    de_text_doc *doc = NULL;
    if (de_text_doc_parse(al, text, len, "scene", &doc) != DE_OK) {
        const de_error *e = de_error_last();
        snprintf(err, err_cap, "%s", e != NULL ? e->message : "bad scene text");
        de_error_clear();
        de__toml_free(tr);
        return false;
    }
    const toml_datum_t *arr = tab_get(&tr.toptab, "entity");
    const u32 n = arr != NULL && arr->type == TOML_ARRAY ? (u32)arr->u.arr.size : 0u;
    load_map m = {NULL, NULL, n, ed};
    m.sids = (u64 *)DE_ALLOC(al, sizeof(u64) * (n + 1), 8, DE_MEM_GENERAL);
    m.ents = (u64 *)DE_ALLOC(al, sizeof(u64) * (n + 1), 8, DE_MEM_GENERAL);
    bool ok = m.sids != NULL && m.ents != NULL;
    /* 1: spawn every entity, so ENTITY fields can point at any of them. */
    for (u32 i = 0; ok && i < n; ++i) {
        const toml_datum_t *ent = &arr->u.arr.elem[i];
        const toml_datum_t *id = tab_get(ent, "id");
        m.sids[i] = id != NULL ? parse_sid(id) : de_scene_id_new();
        m.ents[i] = de_entity_spawn(w).bits;
        member_add(m.ents[i], scene, m.sids[i]);
    }
    /* 2: components. */
    const de_ser_context ctx = {NULL, map_sid_to_entity, NULL, map_id_to_asset, &m};
    const de_ser_options opt = {DE_SER_TEXT, 0, &ctx};
    for (u32 i = 0; ok && i < n; ++i) {
        const toml_datum_t *ent = &arr->u.arr.elem[i];
        for (int32_t k = 0; k < ent->u.tab.size; ++k) {
            const char *key = ent->u.tab.key[k];
            if (ent->u.tab.value[k].type != TOML_TABLE || is_hierarchy_name(key))
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
            if (de_text_doc_read_member(doc, "entity", i, key, ed->e->types, type, data, &opt) != DE_OK) {
                const de_error *e = de_error_last();
                snprintf(err, err_cap, "entity %u, %s: %s", i, key, e != NULL ? e->message : "bad value");
                de_error_clear();
                ok = false;
                break;
            }
        }
    }
    /* 3: the hierarchy. */
    for (u32 i = 0; ok && i < n; ++i) {
        const toml_datum_t *p = tab_get(&arr->u.arr.elem[i], "parent");
        if (p == NULL)
            continue;
        const u64 pe = map_sid_to_entity(parse_sid(p), &m);
        if (pe != 0)
            (void)de_entity_set_parent(w, (de_entity){m.ents[i]}, (de_entity){pe});
    }
    if (!ok)
        for (u32 i = 0; i < n; ++i)
            if (m.ents != NULL && m.ents[i] != 0)
                de_entity_despawn(w, (de_entity){m.ents[i]});
    if (m.sids)
        DE_FREE(al, m.sids, sizeof(u64) * (n + 1));
    if (m.ents)
        DE_FREE(al, m.ents, sizeof(u64) * (n + 1));
    de_text_doc_destroy(doc);
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

/* The entities of `scene` (or `only`, when given) as scene text into `out`. */
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
        putf(out, "[[entity]]\nid = \"0x%016llx\"\n", (unsigned long long)list[i].sid);
        const de_entity p = de_entity_parent(w, e);
        if (p.bits != 0) {
            const ed_member *pm = member_find(p.bits);
            if (pm != NULL)
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
            putf(out, "[entity.%s]\n", ci->name);
            const u32 col = a->column_of[idx[k]];
            if (col != ECS_INVALID_INDEX && ci->size != 0)
                put_component(ed, out, ci->type,
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
    list.scenes.items = (de_ep_scene_info *)de__ed_alloc(a, sizeof(de_ep_scene_info) * ED_MAX_SCENES);
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
    char err[512] = "";
    /* A missing file is a new, empty scene (the IDE creates it on save). */
    if (read_file(path, &text, &len)) {
        const bool ok = spawn_text(ed, text, len, id, err, sizeof err);
        DE_FREE(de_heap_allocator(), text, len + 1);
        if (!ok) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s: %s", path, err);
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

static void on_reload(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(scene_reload_from_text, de_ep_scene_reload_params, q);
    if (!no_change_in_play(ed, req, w))
        return;
    ed_scene *s = scene_find(q.scene);
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "scene %llu", (unsigned long long)q.scene);
        return;
    }
    /* Check the text parses before the old entities go. */
    toml_result_t tr = de__toml_parse(NULL, q.text.ptr, (int)q.text.len);
    if (!tr.ok) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s", tr.errmsg);
        de__toml_free(tr);
        return;
    }
    de__toml_free(tr);
    despawn_scene(ed, q.scene);
    char err[512] = "";
    if (!spawn_text(ed, q.text.ptr, q.text.len, q.scene, err, sizeof err)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s", err);
        return;
    }
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

/* "Component.field" values of every entity of the tree, as MessagePack, into `inst`. */
static void instance_base(de__editor *ed, ed_instance *inst) {
    const de_allocator *al = de_heap_allocator();
    u64 ents[1024];
    const u32 n = tree_of(ed, (de_entity){inst->root}, ents, 1024);
    de_mem_writer bytes;
    de_mem_writer_init(&bytes, NULL);
    u32 cap = 64, count = 0;
    ed_base *base = (ed_base *)DE_ALLOC(al, sizeof(ed_base) * cap, 8, DE_MEM_GENERAL);
    for (u32 i = 0; i < n && base != NULL; ++i) {
        de_ep_arena a = {ed->arena, ed->arena_cap, 0};
        de_ep_entity_row row;
        if (!de__ed_entity_row(ed, &a, (de_entity){ents[i]}, NULL, NULL, &row))
            continue;
        for (u32 c = 0; c < row.components.count; ++c) {
            mpack_tree_t tree;
            mpack_tree_init_data(&tree, row.components.items[c].value.ptr, row.components.items[c].value.len);
            mpack_tree_parse(&tree);
            mpack_node_t m = mpack_tree_root(&tree);
            if (mpack_tree_error(&tree) == mpack_ok && mpack_node_type(m) == mpack_type_map) {
                for (size_t f = 0; f < mpack_node_map_count(m); ++f) {
                    if (count == cap) {
                        ed_base *g = (ed_base *)DE_ALLOC(al, sizeof(ed_base) * cap * 2, 8, DE_MEM_GENERAL);
                        if (g == NULL)
                            break;
                        memcpy(g, base, sizeof(ed_base) * count);
                        DE_FREE(al, base, sizeof(ed_base) * cap);
                        base = g;
                        cap *= 2;
                    }
                    mpack_node_t k = mpack_node_map_key_at(m, f), v = mpack_node_map_value_at(m, f);
                    ed_base *b = &base[count++];
                    b->entity = ents[i];
                    snprintf(b->path, sizeof b->path, "%.*s.%.*s", (int)row.components.items[c].key.len,
                             row.components.items[c].key.ptr, (int)mpack_node_strlen(k), mpack_node_str(k));
                    b->off = (u32)bytes.size;
                    b->len = (u32)mpack_node_data_len(v);
                    de_writer bw = de_writer_from_mem(&bytes);
                    (void)bw.write(bw.user, mpack_node_data(v), b->len);
                }
            }
            mpack_tree_destroy(&tree);
        }
    }
    inst->base = base;
    inst->base_count = count;
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

static void on_instantiate(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(prefab_instantiate, de_ep_prefab_instantiate_params, q);
    de_asset_system *s = ed->e->assets;
    char *asset = de__ed_cstr(a, q.asset);
    if (s == NULL || asset == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no asset system");
        return;
    }
    if (scene_find(q.scene) == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "scene %llu", (unsigned long long)q.scene);
        return;
    }
    const de_asset_id id = de_asset_id_from(asset);
    de_prefab p = de_asset_prefab(s, id);
    if (!DE_HANDLE_VALID(p)) {
        de_error_clear();
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "prefab %s", asset);
        return;
    }
    de_load_handle lh = de_asset_load(s, id, 0);
    if (de_asset_wait(s, lh, 10000000000ULL) != DE_LOAD_READY) {
        de_asset_release(s, lh);
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "prefab %s did not load", asset);
        return;
    }
    de_entity root = DE_ENTITY_NULL;
    const de_result r = de_prefab_instantiate(s, ed->e->world, p,
                                              q.has_parent ? (de_entity){q.parent} : DE_ENTITY_NULL, &root);
    de_asset_release(s, lh);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s", e != NULL ? e->message : "instantiate failed");
        de_error_clear();
        return;
    }
    u64 ents[1024];
    const u32 n = tree_of(ed, root, ents, 1024);
    for (u32 i = 0; i < n; ++i)
        member_add(ents[i], q.scene, de_scene_id_new());
    /* Remember the instance and its fields as instantiated. */
    if (g_scene.instance_count == g_scene.instance_cap) {
        const de_allocator *al = de_heap_allocator();
        const u32 cap = g_scene.instance_cap ? g_scene.instance_cap * 2 : 16;
        ed_instance *grown = (ed_instance *)DE_ALLOC(al, sizeof(ed_instance) * cap, 8, DE_MEM_GENERAL);
        if (grown != NULL) {
            if (g_scene.instances != NULL) {
                memcpy(grown, g_scene.instances, sizeof(ed_instance) * g_scene.instance_count);
                DE_FREE(al, g_scene.instances, sizeof(ed_instance) * g_scene.instance_cap);
            }
            g_scene.instances = grown;
            g_scene.instance_cap = cap;
        }
    }
    if (g_scene.instance_count < g_scene.instance_cap) {
        ed_instance *inst = &g_scene.instances[g_scene.instance_count++];
        memset(inst, 0, sizeof *inst);
        inst->root = root.bits;
        snprintf(inst->asset, sizeof inst->asset, "%s", asset);
        instance_base(ed, inst);
    }
    de__ed_world_touch(ed);
    de_ep_entity_result_one res = {root.bits};
    de_ep_write_response_prefab_instantiate(w, req->id, &res);
}

/* The current value of `path` on `entity`, as raw MessagePack in the arena. */
static bool current_value(de__editor *ed, de_ep_arena *a, u64 entity, const char *path, de_ep_raw *out) {
    de_ep_str p = de_ep_cstr(path);
    de_ep_list_str fields = {&p, 1};
    de_ep_entity_row row;
    if (!de__ed_entity_row(ed, a, (de_entity){entity}, NULL, &fields, &row) || row.components.count != 1)
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
    list.overrides.items = (de_ep_override *)de__ed_alloc(a, sizeof(de_ep_override) * (inst->base_count + 1));
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
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no prefab field %.*s", (int)q.path.len, q.path.ptr);
        return;
    }
    if (apply) {
        /* The instance's value becomes the prefab's: it is no longer an override. */
        de_ep_raw cur;
        if (current_value(ed, a, q.entity, b->path, &cur)) {
            /* The new bytes go after the old ones. */
            u8 *grown = (u8 *)DE_ALLOC(de_heap_allocator(), inst->byte_count + cur.len, 8, DE_MEM_GENERAL);
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
    const de_component c = dot ? de__ed_component(ed, (de_ep_str){path, (u32)(dot - path)}) : (de_component){0};
    de_type_id type;
    u32 size = 0;
    u8 *data = c.id && de__ed_component_type(ed, c, &type, &size)
                   ? (u8 *)de_entity_get(ed->e->world, (de_entity){q.entity}, c)
                   : NULL;
    const de_field_info *f = NULL;
    u32 off = 0;
    if (data == NULL || de_reflect_resolve_path(ed->e->types, type, dot + 1, &f, &off) != DE_OK) {
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
                    de__ed_read_field(ed, mpack_tree_root(&tree), index < fc ? type : (de_type_id){0},
                                      index, f, data + off);
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
