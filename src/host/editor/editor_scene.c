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
#include "host/editor/editor_scene_load.h"
#if DE_HAS_RENDER
#include "render/world/render_world.h"
#endif

#include <dengine/de_asset.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/slots_internal.h"
#include "asset/asset_internal.h"
#include "asset/resolve_internal.h"
#include "asset/cook_stage_internal.h"
#include "core/de_toml_internal.h"
#include "platform/vfs_internal.h"
#include "ecs/ecs_internal.h"
#include "host/host_internal.h"
#include "reflect/ser_internal.h"

#include <limits.h>
#include <errno.h>
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
    de_load_handle owned_assets;
    u64 mutation;
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

typedef struct scene_revision {
    u64 entity, revision;
} scene_revision;
typedef struct scene_pending {
    bool active, reload, additive;
    u64 request, target, deadline;
    u32 epoch;
    char path[512];
    char *text;
    size_t length;
    de__scene_assets assets;
    scene_revision *revisions;
    u32 revision_count, revision_cap;
    u64 scene_mutations[ED_MAX_SCENES];
} scene_pending;

typedef struct de__ed_scene {
    ed_scene scenes[ED_MAX_SCENES];
    u64 next_scene;
    ed_member *members;
    u32 member_count, member_cap;
    ed_instance *instances;
    u32 instance_count, instance_cap;
    scene_pending pending;
    de_engine *owner;
    de__editor *attached;
    struct de__ed_scene *next;
} de__ed_scene;

/* State belongs to the public engine lifetime, not the editor connection. Keying by the
 * public pointer also supports standalone editor fixtures without a private-engine downcast. */
static de__ed_scene *scene_states, *active_scene;
#define g_scene (*active_scene)

static const de_allocator *scene_allocator(void) {
    return active_scene && active_scene->owner->allocator ? active_scene->owner->allocator
                                                          : de_heap_allocator();
}

/* ---------- Membership ---------- */

static ed_member *member_find(u64 entity) {
    if (!active_scene)
        return NULL;
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
        const de_allocator *al = scene_allocator();
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
    if (!ed->scene)
        return;
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
    const de__ed_scene *state = (const de__ed_scene *)ed->scene;
    if (state)
        for (u32 i = 0; i < state->member_count; ++i)
            if (state->members[i].entity == e.bits)
                return state->members[i].scene;
    return ED_GAME_SCENE;
}

static ed_scene *scene_find(u64 id) {
    if (!active_scene)
        return NULL;
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
    bool missing_asset;
} load_map;

static u64 map_sid_to_entity(u64 sid, void *user) {
    const load_map *m = (const load_map *)user;
    for (u32 i = 0; i < m->n; ++i)
        if (m->sids[i] == sid)
            return m->ents[i];
    return 0;
}

static u32 map_id_to_asset(u64 id, const char *type, void *user) {
    load_map *m = (load_map *)user;
    if (id == 0)
        return 0;
    bool owned = false;
    for (u32 i = 0; i < g_scene.pending.assets.count; ++i)
        if (g_scene.pending.assets.refs[i].id.hash == id)
            owned = true;
    const u32 h = owned && m->ed->e->assets
                      ? de__asset_handle_of(m->ed->e->assets, (de_asset_id){id}, type)
                      : 0;
    if (!h || !de__asset_object(m->ed->e->assets, h))
        m->missing_asset = true;
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
    const de_allocator *al = scene_allocator();
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
    load_map m = {NULL, NULL, n, ed, false};
    m.sids = (u64 *)DE_ALLOC(al, sizeof(u64) * (n + 1), 8, DE_MEM_GENERAL);
    m.ents = (u64 *)DE_ALLOC(al, sizeof(u64) * (n + 1), 8, DE_MEM_GENERAL);
    bool ok = m.sids != NULL && m.ents != NULL;
    if (m.ents)
        memset(m.ents, 0, sizeof(u64) * (n + 1));
    /* 1: spawn every entity, so ENTITY fields can point at any of them. */
    for (u32 i = 0; ok && i < n; ++i) {
        const toml_datum_t *ent = &arr->u.arr.elem[i];
        if (ent->type != TOML_TABLE) {
            snprintf(err, err_cap, "entity %u is not a table", i);
            ok = false;
            break;
        }
        const toml_datum_t *id = tab_get(ent, "id");
        m.sids[i] = id != NULL ? parse_sid(id) : de_scene_id_new();
        for (u32 k = 0; k < i; ++k)
            if (m.sids[k] == m.sids[i])
                ok = false;
        if (!m.sids[i] || !ok) {
            snprintf(err, err_cap, "entity %u has a zero or duplicate scene ID", i);
            ok = false;
            break;
        }
        m.ents[i] = de_entity_spawn(w).bits;
        ok = m.ents[i] != 0 && member_add(m.ents[i], scene, m.sids[i]);
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
            if (size == 0)
                continue;
            if (data == NULL) {
                snprintf(err, err_cap, "entity %u: could not add %s", i, key);
                ok = false;
                break;
            }
            if (de_text_doc_read_member(doc, "entity", i, key, ed->e->types, type, data, &opt) !=
                    DE_OK ||
                m.missing_asset) {
                const de_error *e = de_error_last();
                /* Keep the entity/component prefix, then bound the diagnostic to what fits. */
                const int prefix_length = snprintf(err, err_cap, "entity %u, %s: ", i, key);
                if (prefix_length >= 0 && (size_t)prefix_length < err_cap) {
                    const size_t remaining = err_cap - (size_t)prefix_length;
                    const int precision =
                        remaining - 1 > (size_t)INT_MAX ? INT_MAX : (int)(remaining - 1);
                    snprintf(err + prefix_length, remaining, "%.*s", precision,
                             e != NULL ? e->message : "bad value");
                }
                de_error_clear();
                ok = false;
                break;
            }
        }
    }
    /* Validate the parent graph before calling ECS: its cycle contract asserts in Dev. */
    u32 *parents = NULL;
    u8 *colors = NULL;
    if (ok && n) {
        parents = DE_ALLOC(al, sizeof(u32) * n, 8, DE_MEM_GENERAL);
        colors = DE_ALLOC(al, n, 1, DE_MEM_GENERAL);
        ok = parents && colors;
        if (colors)
            memset(colors, 0, n);
        for (u32 i = 0; ok && i < n; ++i) {
            parents[i] = n;
            const toml_datum_t *parent = tab_get(&arr->u.arr.elem[i], "parent");
            if (!parent)
                continue;
            const u64 sid = parse_sid(parent);
            for (u32 j = 0; j < n; ++j)
                if (m.sids[j] == sid)
                    parents[i] = j;
            if (parents[i] == n) {
                snprintf(err, err_cap, "entity %u: missing parent", i);
                ok = false;
            }
        }
        for (u32 i = 0; ok && i < n; ++i) {
            u32 at = i;
            while (at < n && colors[at] == 0) {
                colors[at] = 1;
                at = parents[at];
            }
            if (at < n && colors[at] == 1) {
                snprintf(err, err_cap, "scene hierarchy has a cycle");
                ok = false;
            }
            at = i;
            while (at < n && colors[at] == 1) {
                colors[at] = 2;
                at = parents[at];
            }
        }
    }
    if (parents)
        DE_FREE(al, parents, sizeof(u32) * n);
    if (colors)
        DE_FREE(al, colors, n);
    /* 3: the hierarchy. */
    for (u32 i = 0; ok && i < n; ++i) {
        const toml_datum_t *p = tab_get(&arr->u.arr.elem[i], "parent");
        if (p == NULL)
            continue;
        const u64 pe = map_sid_to_entity(parse_sid(p), &m);
        if (pe == 0 || de_entity_set_parent(w, (de_entity){m.ents[i]}, (de_entity){pe}) != DE_OK) {
            snprintf(err, err_cap, "entity %u: missing or invalid parent", i);
            ok = false;
        }
    }
    if (!ok && err[0] == 0)
        snprintf(err, err_cap, "scene entity allocation failed");
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
typedef struct save_path {
    u64 id;
    char *path;
    size_t bytes;
} save_path;
typedef struct save_map {
    de__editor *ed;
    save_path *paths;
    u32 count, cap;
    bool failed;
} save_map;

static const char *saved_path(const save_map *m, u64 id) {
    for (u32 i = 0; i < m->count; ++i)
        if (m->paths[i].id == id)
            return m->paths[i].path;
    return NULL;
}

static void save_map_free(save_map *m) {
    const de_allocator *a = scene_allocator();
    for (u32 i = 0; i < m->count; ++i)
        DE_FREE(a, m->paths[i].path, m->paths[i].bytes);
    if (m->paths)
        DE_FREE(a, m->paths, sizeof(save_path) * m->cap);
}

static u64 save_entity_to_sid(u64 bits, void *user) {
    (void)user;
    const ed_member *m = member_find(bits);
    return m != NULL ? m->sid : bits;
}

static u64 save_asset_to_id(u32 handle, const char *type, void *user) {
    save_map *m = user;
    de_asset_system *assets = m->ed->e->assets;
    const de__slot *slot = assets ? de__slot_get(assets, handle) : NULL;
    if (!slot) {
        if (handle)
            m->failed = true;
        return 0;
    }
    if (type && slot->type_hash != de_hash64_str(type)) {
        m->failed = true;
        return 0;
    }
    const u64 id = slot->id.hash;
    if (saved_path(m, id))
        return id;
    const char *root = assets->resolved ? de__resolved_path(assets->resolved, slot->id) : NULL;
    size_t bytes = root ? strlen(root) + 1 : 0;
    if (!root) {
        const de_result lookup = de__cook_stage_subasset_path(assets, slot->id, NULL, 0, &bytes);
        if (lookup == DE_ERR_NOT_FOUND)
            return id; /* Legitimately nameless packed assets retain their canonical numeric ID. */
        if (lookup != DE_OK) {
            m->failed = true;
            return id;
        }
    }
    const size_t allocation_bytes = bytes;
    const de_allocator *a = scene_allocator();
    char *copy = DE_ALLOC(a, allocation_bytes, 1, DE_MEM_GENERAL);
    if (!copy) {
        m->failed = true;
        return id;
    }
    if (root)
        memcpy(copy, root, bytes);
    else if (de__cook_stage_subasset_path(assets, slot->id, copy, allocation_bytes, &bytes) !=
             DE_OK) {
        DE_FREE(a, copy, allocation_bytes);
        m->failed =
            true; /* Never turn a lost generated name into a silently cold-unloadable save. */
        return id;
    }
    if (m->count == m->cap) {
        u32 cap = m->cap ? m->cap * 2 : 32;
        save_path *grown =
            cap > m->cap ? DE_ALLOC(a, sizeof(save_path) * cap, 8, DE_MEM_GENERAL) : NULL;
        if (!grown) {
            DE_FREE(a, copy, allocation_bytes);
            m->failed = true;
            return id;
        }
        if (m->paths) {
            memcpy(grown, m->paths, sizeof(save_path) * m->count);
            DE_FREE(a, m->paths, sizeof(save_path) * m->cap);
        }
        m->paths = grown;
        m->cap = cap;
    }
    m->paths[m->count++] = (save_path){id, copy, allocation_bytes};
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

static void save_write(save_map *m, de_mem_writer *out, const void *data, size_t n) {
    de_writer w = de_writer_from_mem(out);
    if (!m->failed && w.write(w.user, data, n) != DE_OK)
        m->failed = true;
}
static void save_quoted(save_map *m, de_mem_writer *out, const char *path) {
    save_write(m, out, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        if (*p == '\\' || *p == '"')
            save_write(m, out, "\\", 1);
        if (*p < 32 || *p == 127) {
            char escape[7];
            snprintf(escape, sizeof escape, "\\u%04x", (unsigned)*p);
            save_write(m, out, escape, 6);
        } else
            save_write(m, out, p, 1);
    }
    save_write(m, out, "\"", 1);
}

/* One line of a component's text with its asset tables (`{ id = "0x..." }`, `{ id = "0x...", path =
 * "..." }`) as path strings where the path is known; false for a line that only sets asset fields
 * to none, which the defaults already are. */
static bool asset_line(save_map *m, const char *line, size_t n, de_mem_writer *out) {
    if (only_empty_assets(line, n))
        return false;
    const char *open = k_asset_open;
    const size_t on = sizeof k_asset_open - 1;
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
            save_write(m, out, line + copied, at - copied);
            save_quoted(m, out, path);
            copied = after;
        }
        at = after;
    }
    save_write(m, out, line + copied, n - copied);
    return true;
}

/* Writes one component's fields as TOML lines, without the serializer's header keys. */
static void put_component(de__editor *ed, de_mem_writer *out, de_type_id type, const void *data,
                          const de_ser_options *opt) {
    de_mem_writer tmp;
    de_mem_writer_init(&tmp, scene_allocator());
    de_writer tw = de_writer_from_mem(&tmp);
    de__ser_out o;
    de__ser_out_init(&o, &tw);
    save_map *sm = opt->ctx != NULL ? opt->ctx->user : NULL;
    const de_result serialized = de__ser_text_write_body(&o, ed->e->types, type, data, opt);
    const de_result flushed = de__ser_out_flush(&o);
    if (serialized != DE_OK || flushed != DE_OK) {
        if (sm)
            sm->failed = true;
        de_mem_writer_free(&tmp);
        return;
    }
    de_writer ow = de_writer_from_mem(out);
    size_t at = 0;
    while (at < tmp.size) {
        size_t end = at;
        while (end < tmp.size && tmp.data[end] != '\n')
            ++end;
        const size_t n = end - at;
        if (!(n >= 2 && tmp.data[at] == '_' && tmp.data[at + 1] == '_')) {
            if (sm == NULL)
                (void)ow.write(ow.user, tmp.data + at, end < tmp.size ? n + 1 : n);
            else if (asset_line(sm, (const char *)tmp.data + at, n, out) && end < tmp.size)
                save_write(sm, out, "\n", 1);
        }
        at = end + 1;
    }
    de_mem_writer_free(&tmp);
}

static bool putf(de_mem_writer *out, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static bool putf(de_mem_writer *out, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf)
        return false;
    de_writer w = de_writer_from_mem(out);
    return w.write(w.user, buf, (size_t)n) == DE_OK;
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
static bool write_scene(de__editor *ed, u64 scene, const u64 *only, u32 only_count,
                        de_mem_writer *out) {
    de_world *w = ed->e->world;
    const de_allocator *al = scene_allocator();
    u32 total = 0;
    for (u32 ai = 0; ai < w->archetypes.count; ++ai)
        total += (((ecs_archetype *const *)w->archetypes.data)[ai])->count;
    save_ent *list = (save_ent *)DE_ALLOC(al, sizeof(save_ent) * (total + 1), 8, DE_MEM_GENERAL);
    if (list == NULL)
        return false;
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
            sm.failed |= !putf(out, "\n");
        sm.failed |=
            !putf(out, "[[entity]]\nid = \"0x%016llx\"\n", (unsigned long long)list[i].sid);
        const de_entity p = de_entity_parent(w, e);
        if (p.bits != 0) {
            const ed_member *pm = member_find(p.bits);
            if (pm != NULL)
                sm.failed |= !putf(out, "parent = \"0x%016llx\"\n", (unsigned long long)pm->sid);
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
            sm.failed |= !putf(out, "[entity.%s]\n", ci->name);
            const u32 col = a->column_of[idx[k]];
            if (col != ECS_INVALID_INDEX && ci->size != 0)
                put_component(
                    ed, out, ci->type,
                    (const u8 *)ecs_chunk_column(a, ch, col) + (size_t)rec->row * ci->size, &opt);
        }
    }
    DE_FREE(al, list, sizeof(save_ent) * (total + 1));
    const bool ok = !sm.failed;
    save_map_free(&sm);
    return ok;
}

/* ---------- Scene methods ---------- */

static bool no_change_in_play(de__editor *ed, const de_ep_request *req, mpack_writer_t *w) {
    if (ed->mode == DE_EP_WORLD_MODE_EDIT)
        return true;
    de__ed_error(w, req->id, DE_EP_ERR_WRONG_MODE, "scenes can't change during a play session");
    return false;
}

static void instances_prune(de__editor *ed) {
    const de_allocator *a = scene_allocator();
    u32 kept = 0;
    for (u32 i = 0; i < g_scene.instance_count; ++i) {
        ed_instance *instance = &g_scene.instances[i];
        if (de_entity_alive(ed->e->world, (de_entity){instance->root})) {
            g_scene.instances[kept++] = *instance;
        } else {
            if (instance->base)
                DE_FREE(a, instance->base, sizeof(ed_base) * instance->base_cap);
            if (instance->bytes)
                DE_FREE(a, instance->bytes, instance->byte_count);
        }
    }
    g_scene.instance_count = kept;
}

static void despawn_scene(de__editor *ed, u64 scene) {
    members_prune(ed);
    for (u32 i = 0; i < g_scene.member_count; ++i)
        if (g_scene.members[i].scene == scene)
            de_entity_despawn(ed->e->world, (de_entity){g_scene.members[i].entity});
    members_prune(ed);
    instances_prune(ed);
    de__ed_world_touch(ed);
}

static de_result read_file(const char *path, char **text, size_t *len) {
    const de_allocator *al = scene_allocator();
    if (strstr(path, ":/") != NULL && path[1] != ':') {
        de_file *file = NULL;
        DE_TRY(de_file_open(path, DE_FILE_READ, &file));
        const u64 size = de_file_size(file);
        if (size > DE_EP_MAX_FRAME || size > INT_MAX) {
            de_file_close(file);
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "scene file exceeds protocol ceiling");
        }
        char *data = DE_ALLOC(al, (size_t)size + 1, 16, DE_MEM_GENERAL);
        if (!data) {
            de_file_close(file);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene file allocation failed");
        }
        u64 got = 0;
        const de_result read = de_file_read(file, data, 0, size, &got);
        de_file_close(file);
        if (read != DE_OK || got != size) {
            DE_FREE(al, data, (size_t)size + 1);
            return DE_FAIL(read != DE_OK ? read : DE_ERR_IO, "scene file read failed");
        }
        data[size] = 0;
        *text = data;
        *len = (size_t)size;
        return DE_OK;
    }
    FILE *file = fopen(path, "rb");
    if (!file)
        return DE_FAIL(errno == ENOENT || errno == ENOTDIR ? DE_ERR_NOT_FOUND : DE_ERR_IO,
                       "scene file could not be opened");
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return DE_FAIL(DE_ERR_IO, "scene file seek failed");
    }
    const long size = ftell(file);
    if (size < 0 || (u64)size > DE_EP_MAX_FRAME || size > INT_MAX || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "scene file size is invalid");
    }
    char *data = DE_ALLOC(al, (size_t)size + 1, 1, DE_MEM_GENERAL);
    const bool ok = data && fread(data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!ok) {
        if (data)
            DE_FREE(al, data, (size_t)size + 1);
        return DE_FAIL(DE_ERR_IO, "scene file could not be read");
    }
    data[size] = 0;
    *text = data;
    *len = (size_t)size;
    return DE_OK;
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

/* One pending transaction: all buffers and requests are detached before response I/O. */
static void pending_clear(void) {
    scene_pending *p = &g_scene.pending;
    de__scene_assets_drop(&p->assets);
    if (p->text)
        DE_FREE(scene_allocator(), p->text, p->length + 1);
    if (p->revisions)
        DE_FREE(scene_allocator(), p->revisions, sizeof(scene_revision) * p->revision_cap);
    memset(p, 0, sizeof *p);
}
static bool affected(de__editor *ed, const scene_pending *p, de_entity e) {
    /* Match serialization ownership: editor cameras are never scene content. */
    if (de__ed_views_owns(e.bits))
        return false;
    const u64 scene = de__ed_scene_of(ed, e);
    return p->reload ? scene == p->target : !p->additive && scene != ED_GAME_SCENE;
}
static int revision_compare(const void *a, const void *b) {
    const u64 x = ((const scene_revision *)a)->entity, y = ((const scene_revision *)b)->entity;
    return (x > y) - (x < y);
}
static bool snapshot(de__editor *ed, scene_pending *p, bool check) {
    de_world *world = ed->e->world;
    u64 total = 1;
    for (u32 ai = 0; ai < world->archetypes.count; ++ai)
        total += ((ecs_archetype *const *)world->archetypes.data)[ai]->count;
    if (total > UINT32_MAX || total > SIZE_MAX / sizeof(scene_revision))
        return false;
    scene_revision *items =
        DE_ALLOC(scene_allocator(), sizeof(*items) * (size_t)total, 8, DE_MEM_GENERAL);
    if (!items)
        return false;
    u32 count = 0;
    for (u32 ai = 0; ai < world->archetypes.count; ++ai) {
        const ecs_archetype *arch = ((ecs_archetype *const *)world->archetypes.data)[ai];
        for (u32 ci = 0; ci < arch->chunks.count; ++ci) {
            const ecs_chunk *chunk = ecs_table_chunk(arch, ci);
            for (u32 row = 0; row < chunk->count; ++row) {
                de_entity e = ecs_chunk_entities(chunk)[row];
                if (affected(ed, p, e))
                    items[count++] = (scene_revision){e.bits, de__ed_entity_rev(ed, e)};
            }
        }
    }
    qsort(items, count, sizeof(*items), revision_compare);
    if (!check) {
        for (u32 i = 0; i < ED_MAX_SCENES; ++i)
            p->scene_mutations[i] = g_scene.scenes[i].mutation;
        p->revisions = items;
        p->revision_count = count;
        p->revision_cap = (u32)total;
        return true;
    }
    bool same = count == p->revision_count;
    for (u32 i = 0; same && i < ED_MAX_SCENES; ++i)
        if (p->reload ? g_scene.scenes[i].id == p->target
                      : !p->additive && g_scene.scenes[i].id != ED_GAME_SCENE)
            same = p->scene_mutations[i] == g_scene.scenes[i].mutation;
    for (u32 i = 0; same && i < count; ++i)
        same = items[i].entity == p->revisions[i].entity &&
               items[i].revision == p->revisions[i].revision;
    DE_FREE(scene_allocator(), items, sizeof(*items) * (size_t)total);
    return same;
}
static void scene_quiesce(de__editor *ed) {
#if DE_HAS_RENDER
    de_gpu_device *device = NULL;
    if (ed->e->renderer) {
        de__render_world_wait_idle(ed->e->renderer);
        device = de_renderer_device(ed->e->renderer);
        de_gpu_wait_idle(device);
    }
    if (ed->e->gpu && ed->e->gpu != device)
        de_gpu_wait_idle(ed->e->gpu);
#else
    (void)ed;
#endif
}
static void scene_release(de__editor *ed, ed_scene *scene) {
    if (ed->e->assets)
        de_asset_release(ed->e->assets, scene->owned_assets);
    scene->owned_assets.id = 0;
}
static bool scene_commit(de__editor *ed, char *error, size_t capacity, u64 *result) {
    scene_pending *p = &g_scene.pending;
    ed_scene *slot = p->reload ? scene_find(p->target) : NULL;
    if (!p->reload) {
        for (u32 i = 0; i < ED_MAX_SCENES && !slot; ++i)
            if (!g_scene.scenes[i].id)
                slot = &g_scene.scenes[i];
        for (u32 i = 0; !p->additive && i < ED_MAX_SCENES && !slot; ++i)
            if (g_scene.scenes[i].id != ED_GAME_SCENE)
                slot = &g_scene.scenes[i];
    }
    if (!slot || !snapshot(ed, p, true)) {
        snprintf(error, capacity,
                 "scene changed during load or transaction snapshot unavailable; retry");
        return false;
    }
    scene_quiesce(ed);
    if (de_time_now_ns() >= p->deadline) {
        snprintf(error, capacity, "scene load deadline expired; previous scene retained");
        return false;
    }
    const u64 staged = ++g_scene.next_scene;
    if (!spawn_text(ed, p->text, p->length, staged, error, capacity))
        return false;
    if (de_time_now_ns() >= p->deadline) {
        despawn_scene(ed, staged);
        snprintf(error, capacity, "scene load deadline expired; previous scene retained");
        return false;
    }
    const u64 final_id = p->reload ? p->target : staged;
    if (p->reload) {
        /* Only this validated reload target, excluding editor-owned views. Despawn is
         * nonrecursive: children outside the target become roots, not collateral deletions. */
        for (u32 i = 0; i < p->revision_count; ++i) {
            const de_entity entity = {p->revisions[i].entity};
            if (affected(ed, p, entity))
                de_entity_despawn(ed->e->world, entity);
        }
        members_prune(ed);
        instances_prune(ed);
        de__ed_world_touch(ed);
        scene_release(ed, slot);
    } else if (!p->additive) {
        for (u32 i = 0; i < ED_MAX_SCENES; ++i)
            if (g_scene.scenes[i].id && g_scene.scenes[i].id != ED_GAME_SCENE) {
                despawn_scene(ed, g_scene.scenes[i].id);
                scene_release(ed, &g_scene.scenes[i]);
                memset(&g_scene.scenes[i], 0, sizeof g_scene.scenes[i]);
            }
    }
    for (u32 i = 0; i < g_scene.member_count; ++i)
        if (g_scene.members[i].scene == staged)
            g_scene.members[i].scene = final_id;
    if (!p->reload) {
        memset(slot, 0, sizeof *slot);
        slot->id = final_id;
        memcpy(slot->path, p->path, sizeof slot->path);
        for (u32 i = 0; i < ED_MAX_SCENES; ++i)
            g_scene.scenes[i].active = false;
        slot->active = true;
    } else
        slot->dirty = true;
    slot->owned_assets = p->assets.exact;
    p->assets.exact.id = 0;
    *result = final_id;
    return true;
}

/* Private nonblocking transaction seam, also used by service-path tests without a transport. */
bool de__ed_scene_poll(de__editor *ed, mpack_writer_t *writer) {
    scene_pending *p = &g_scene.pending;
    if (!p->active)
        return false;
    if (p->epoch != ed->epoch) {
        pending_clear();
        return false;
    }
    if (de_time_now_ns() >= p->deadline) {
        const u64 request = p->request;
        pending_clear();
        de__ed_error(writer, request, DE_EP_ERR_INVALID_PARAMS,
                     "scene load deadline expired; previous scene retained");
        return true;
    }
    de_result r =
        ed->mode == DE_EP_WORLD_MODE_EDIT ? de__scene_assets_poll(&p->assets) : DE_ERR_BUSY;
    if (ed->mode == DE_EP_WORLD_MODE_EDIT && r == DE_ERR_BUSY)
        return false;
    char error[512] = "scene mode changed during load";
    if (r != DE_OK && ed->mode == DE_EP_WORLD_MODE_EDIT) {
        const de_error *e = de_error_last();
        snprintf(error, sizeof error, "%s", e ? e->message : "scene asset loading failed");
    }
    u64 scene = 0;
    const bool ok = r == DE_OK && scene_commit(ed, error, sizeof error, &scene);
    const u64 request = p->request;
    const bool reload = p->reload;
    pending_clear();
    if (!ok)
        de__ed_error(writer, request, DE_EP_ERR_INVALID_PARAMS, "%s", error);
    else if (reload) {
        de_ep_empty empty = {0};
        de_ep_write_response_scene_reload_from_text(writer, request, &empty);
        /* Pending ownership is already detached: send failure may reset synchronously. */
        de_ep_scene_params dirty = {scene};
        mpack_writer_t note;
        const u64 seq = de__ed_note_begin(ed, &note);
        de_ep_write_notify_scene_dirty(&note, seq, &dirty);
        (void)de__ed_send(ed, &note, DE_EP_KIND_NOTIFICATION);
    } else {
        de_ep_scene_params response = {scene};
        de_ep_write_response_scene_load(writer, request, &response);
    }
    return true;
}
static void scene_tick(de__editor *ed) {
    if (!g_scene.pending.active)
        return;
    if (!ed->connected || !ed->authed) {
        pending_clear();
        return;
    }
    mpack_writer_t writer;
    de__ed_respond_begin(ed, &writer);
    if (de__ed_scene_poll(ed, &writer))
        (void)de__ed_send(ed, &writer, DE_EP_KIND_RESPONSE);
    else
        (void)mpack_writer_destroy(&writer);
}
static void scene_reset(de__editor *ed) {
    if (!ed->scene)
        return;
    de__ed_scene *saved = active_scene;
    active_scene = (de__ed_scene *)ed->scene;
    pending_clear();
    active_scene = saved;
}
static void scene_begin(de__editor *ed, const de_ep_request *request, mpack_writer_t *writer,
                        const char *text, size_t length, const char *path, u64 target,
                        bool additive) {
    if (g_scene.pending.active) {
        de__ed_error(writer, request->id, DE_EP_ERR_BUSY, "another scene transaction is pending");
        return;
    }
    if (length > DE_EP_MAX_FRAME || length > INT_MAX || (path && strlen(path) >= 512)) {
        de__ed_error(writer, request->id, DE_EP_ERR_INVALID_PARAMS, "scene input is too large");
        return;
    }
    scene_pending *p = &g_scene.pending;
    p->request = request->id;
    /* Existing DIDE scene requests use a 3-second client timeout. Bound engine work too:
     * no indefinite late commit after a client stops waiting. This is measured on receipt,
     * not a transport acknowledgement guarantee; expensive cold cooks can fail and retry. */
    p->deadline = de_time_now_ns() + 3000000000ULL;
    p->epoch = ed->epoch;
    p->target = target;
    p->reload = target != 0;
    p->additive = additive;
    if (path)
        memcpy(p->path, path, strlen(path) + 1);
    p->text = DE_ALLOC(scene_allocator(), length + 1, 1, DE_MEM_GENERAL);
    p->length = length;
    if (!p->text || !snapshot(ed, p, false)) {
        pending_clear();
        de__ed_error(writer, request->id, DE_EP_ERR_INTERNAL,
                     "scene transaction allocation failed");
        return;
    }
    memcpy(p->text, text, length);
    p->text[length] = 0;
    const de_result r = de__scene_assets_begin(ed, p->text, length, &p->assets);
    if (r != DE_OK) {
        char error[512];
        const de_error *e = de_error_last();
        snprintf(error, sizeof error, "%s", e ? e->message : "scene preflight failed");
        pending_clear();
        de__ed_error(writer, request->id, DE_EP_ERR_INVALID_PARAMS, "%s", error);
        return;
    }
    p->active = true;
    if (!de__ed_scene_poll(ed, writer))
        de__ed_defer(ed);
}

static void on_load(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(scene_load, de_ep_scene_load_params, q);
    if (!no_change_in_play(ed, req, w))
        return;
    char *path = de__ed_cstr(a, q.path);
    if (!path) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "scene path allocation failed");
        return;
    }
    if (g_scene.pending.active) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "another scene transaction is pending");
        return;
    }
    char *text = NULL;
    size_t length = 0;
    const de_result read = read_file(path, &text, &length);
    if (read != DE_OK && read != DE_ERR_NOT_FOUND) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "scene file could not be read");
        return;
    }
    scene_begin(ed, req, w, text ? text : "", length, path, 0, q.additive);
    if (text)
        DE_FREE(scene_allocator(), text, length + 1);
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
    if (g_scene.pending.active) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "another scene transaction is pending");
        return;
    }
    scene_quiesce(ed);
    despawn_scene(ed, q.scene);
    scene_release(ed, s);
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
    de_mem_writer_init(&out, scene_allocator());
    if (!write_scene(ed, q.scene, NULL, 0, &out)) {
        de_mem_writer_free(&out);
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "scene asset identity serialization failed");
        return;
    }
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
    scene_begin(ed, req, w, q.text.ptr, q.text.len, NULL, q.scene, false);
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
    const de_allocator *al = scene_allocator();
    u64 ents[1024];
    const u32 n = tree_of(ed, (de_entity){inst->root}, ents, 1024);
    de_mem_writer bytes;
    de_mem_writer_init(&bytes, scene_allocator());
    u32 cap = 64, count = 0;
    ed_base *base = (ed_base *)DE_ALLOC(al, sizeof(ed_base) * cap, 8, DE_MEM_GENERAL);
    for (u32 i = 0; i < n && base != NULL; ++i) {
        de_ep_arena a = {ed->arena, ed->arena_cap, 0};
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
    inst->base_cap = cap;
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
    const de_result r = de_prefab_instantiate(
        s, ed->e->world, p, q.has_parent ? (de_entity){q.parent} : DE_ENTITY_NULL, &root);
    de_asset_release(s, lh);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s",
                     e != NULL ? e->message : "instantiate failed");
        de_error_clear();
        return;
    }
    u64 ents[1024];
    const u32 n = tree_of(ed, root, ents, 1024);
    for (u32 i = 0; i < n; ++i)
        member_add(ents[i], q.scene, de_scene_id_new());
    /* Remember the instance and its fields as instantiated. */
    if (g_scene.instance_count == g_scene.instance_cap) {
        const de_allocator *al = scene_allocator();
        const u32 cap = g_scene.instance_cap ? g_scene.instance_cap * 2 : 16;
        ed_instance *grown =
            (ed_instance *)DE_ALLOC(al, sizeof(ed_instance) * cap, 8, DE_MEM_GENERAL);
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
                (u8 *)DE_ALLOC(scene_allocator(), inst->byte_count + cur.len, 8, DE_MEM_GENERAL);
            if (grown != NULL) {
                if (inst->bytes != NULL) {
                    memcpy(grown, inst->bytes, inst->byte_count);
                    DE_FREE(scene_allocator(), inst->bytes, inst->byte_count);
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
    if (s != NULL) {
        s->dirty = true;
        ++s->mutation;
    }
}

bool de__ed_scene_attached(void) {
    return active_scene && active_scene->attached;
}

de_result de__ed_scene_register(de__editor *ed) {
    /* Editor helpers have one active binding. Reject a second simultaneous attachment rather
     * than route one engine's handlers into another engine's retained scene state. */
    if (active_scene && active_scene->attached && active_scene->attached != ed)
        return DE_FAIL(DE_ERR_BUSY, "only one attached scene editor service is supported");
    de__ed_scene *state = scene_states;
    while (state && state->owner != ed->e)
        state = state->next;
    if (!state) {
        state = DE_NEW(ed->e->allocator ? ed->e->allocator : de_heap_allocator(), de__ed_scene,
                       DE_MEM_GENERAL);
        if (!state)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene service state allocation");
        memset(state, 0, sizeof *state);
        state->owner = ed->e;
        state->next = scene_states;
        scene_states = state;
        state->next_scene = ED_GAME_SCENE;
        ed_scene *game = &state->scenes[0];
        game->id = ED_GAME_SCENE;
        snprintf(game->path, sizeof game->path, "%s", ed->h->config.name);
        game->active = true;
    }
    active_scene = state;
    state->attached = ed;
    ed->scene = (struct de__ed_scene *)state;
    de__ed_on_tick(ed, scene_tick);
    de__ed_on_reset(ed, scene_reset);
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
    return DE_OK;
}

void de__ed_scene_detach(de__editor *ed) {
    scene_reset(ed);
    if (ed->scene)
        ((de__ed_scene *)ed->scene)->attached = NULL;
    ed->scene = NULL;
}

/* Called after game shutdown/render-thread quiescence, immediately before assets are destroyed.
 * Does not despawn game entities or reorder renderer/uploader destruction. */
void de__ed_scene_engine_shutdown(de_engine *engine) {
    de__ed_scene **link = &scene_states;
    while (*link && (*link)->owner != engine)
        link = &(*link)->next;
    if (!*link)
        return;
    de__ed_scene *state = *link, *saved = active_scene;
    active_scene = state;
#if DE_HAS_RENDER
    if (engine->renderer)
        de_gpu_wait_idle(de_renderer_device(engine->renderer));
#endif
#if DE_HAS_GPU
    if (engine->gpu)
        de_gpu_wait_idle(engine->gpu);
#endif
    pending_clear();
    for (u32 i = 0; i < ED_MAX_SCENES; ++i)
        if (engine->assets)
            de_asset_release(engine->assets, state->scenes[i].owned_assets);
    const de_allocator *a = scene_allocator();
    for (u32 i = 0; i < state->instance_count; ++i) {
        ed_instance *instance = &state->instances[i];
        if (instance->base)
            DE_FREE(a, instance->base, sizeof(ed_base) * instance->base_cap);
        if (instance->bytes)
            DE_FREE(a, instance->bytes, instance->byte_count);
    }
    if (state->instances)
        DE_FREE(a, state->instances, sizeof(ed_instance) * state->instance_cap);
    if (state->members)
        DE_FREE(a, state->members, sizeof(ed_member) * state->member_cap);
    *link = state->next;
    if (state->attached)
        state->attached->scene = NULL;
    active_scene = saved == state ? NULL : saved;
    DE_FREE(a, state, sizeof *state);
}
