/* editor_edit.c: transactional edits with inverses, undo and redo (DENG-626).
 *
 * Ops: spawn, despawn, add, remove, set, reparent, instantiate, uninstantiate, prefab.unpack (and its
 * inverse prefab.pack, DENG-801). They run between
 * frames (the service's frame-begin hook), so they use the world's immediate structural calls.
 * Each applied op is kept with its inverse as EditOp MessagePack bytes; undo applies the inverses
 * in reverse, redo the ops again, and each replayed op's own inverse replaces its counterpart, so
 * a redo restores what the undo took away. An undone despawn gets its entity id back: the freed
 * index is taken again at the old generation.
 *
 * Prefabs (DENG-764): `instantiate` (path: the prefab asset; optional parent) spawns a new
 * instance: entities, scene ids and the instance record. Its inverse is `uninstantiate` (entity:
 * the instance root), which removes the whole tree, its scene memberships and the record. The
 * inverse of that is `instantiate` with `value` set to a snapshot of the instance, which brings
 * back the same entity ids, stable scene ids, components and record:
 *   {asset: str, entities: [{id, parent, scene, sid, components: {Name: value}}],
 *    base: [{entity, path, value}]}
 *
 * Sibling order (DENG-778, protocol 1.14, ui.editor): `spawn`, `instantiate` and `reparent` take
 * an optional `index`, the place among the parent's other children (de_entity_set_parent_at: at
 * or past the end appends; ignored without a parent). The inverses of `despawn`, `uninstantiate`
 * and `reparent` carry the old index, so undo puts an entity back where it was.
 *
 * Terrain (DENG-580, protocol 1.11, world.terrain): an op whose name starts with `terrain.` goes to
 * the terrain area (editor_terrain.c, render builds); its inverse is a `terrain.restore`. */
#include "host/editor/editor_internal.h"

#include <dengine/de_memory.h>

#include "ecs/ecs_internal.h"
#include "host/host_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ED_MAX_TX 512u

typedef enum { TX_FREE, TX_OPEN, TX_COMMITTED, TX_UNDONE, TX_CANCELLED } tx_state;

/* Scratch for reverting open transactions when the connection ends. Not the request arena: a
 * drop can happen inside a handler that still uses it. */
#define ED_RESET_SCRATCH (8u << 20)

typedef struct ed_blob {
    u8 *p;
    u32 n;
} ed_blob;

typedef struct ed_pair {
    ed_blob fwd, inv;
} ed_pair;

typedef struct ed_tx {
    u32 id;
    tx_state state;
    bool in_play; /**< begun outside Edit: a return to Edit discards its effects anyway */
    u64 scene;
    ed_pair *pairs;
    u32 count, cap;
    u64 *assigned;
    u32 assigned_count, assigned_cap;
} ed_tx;

typedef struct de__ed_edit {
    ed_tx tx[ED_MAX_TX];
    u32 next_id;
} de__ed_edit;

static de__ed_edit g_edit;

/* An op failure: code and message. */
typedef struct ed_fail {
    de_ep_error code;
    char msg[256];
    const char *reason; /* a structured reason, when the op has one (prefab.state) */
} ed_fail;

static bool fail(ed_fail *f, de_ep_error code, const char *fmt, ...) {
    f->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, sizeof f->msg, fmt, ap);
    va_end(ap);
    return false;
}

/* ---------- Owned bytes ---------- */

static ed_blob blob_of_op(de__editor *ed, const de_ep_edit_op *o) {
    ed_blob b = {0};
    mpack_writer_t w;
    mpack_writer_init(&w, ed->note, ed->note_cap);
    de_ep_write_edit_op(&w, o);
    const size_t n = mpack_writer_buffer_used(&w);
    if (mpack_writer_destroy(&w) != mpack_ok)
        return b;
    b.p = (u8 *)DE_ALLOC(de_heap_allocator(), n, 8, DE_MEM_GENERAL);
    if (b.p == NULL)
        return b;
    memcpy(b.p, ed->note, n);
    b.n = (u32)n;
    return b;
}

static void blob_free(ed_blob *b) {
    if (b->p != NULL)
        DE_FREE(de_heap_allocator(), b->p, b->n);
    b->p = NULL;
    b->n = 0;
}

static bool op_of_blob(const ed_blob *b, de_ep_arena *a, de_ep_edit_op *out) {
    mpack_reader_t r;
    mpack_reader_init_data(&r, (const char *)b->p, b->n);
    memset(out, 0, sizeof *out);
    de_ep_read_edit_op(&r, a, out);
    return mpack_reader_destroy(&r) == mpack_ok;
}

static void tx_free(ed_tx *t) {
    const de_allocator *al = de_heap_allocator();
    for (u32 i = 0; i < t->count; ++i) {
        blob_free(&t->pairs[i].fwd);
        blob_free(&t->pairs[i].inv);
    }
    if (t->pairs != NULL)
        DE_FREE(al, t->pairs, sizeof(ed_pair) * t->cap);
    if (t->assigned != NULL)
        DE_FREE(al, t->assigned, sizeof(u64) * t->assigned_cap);
    memset(t, 0, sizeof *t);
}

static bool tx_push(ed_tx *t, ed_blob fwd, ed_blob inv) {
    const de_allocator *al = de_heap_allocator();
    if (t->count == t->cap) {
        const u32 cap = t->cap ? t->cap * 2 : 16;
        ed_pair *p = (ed_pair *)DE_ALLOC(al, sizeof(ed_pair) * cap, 8, DE_MEM_GENERAL);
        if (p == NULL)
            return false;
        if (t->pairs != NULL) {
            memcpy(p, t->pairs, sizeof(ed_pair) * t->count);
            DE_FREE(al, t->pairs, sizeof(ed_pair) * t->cap);
        }
        t->pairs = p;
        t->cap = cap;
    }
    t->pairs[t->count].fwd = fwd;
    t->pairs[t->count].inv = inv;
    ++t->count;
    return true;
}

static void tx_assign(ed_tx *t, u64 e) {
    const de_allocator *al = de_heap_allocator();
    if (t->assigned_count == t->assigned_cap) {
        const u32 cap = t->assigned_cap ? t->assigned_cap * 2 : 16;
        u64 *p = (u64 *)DE_ALLOC(al, sizeof(u64) * cap, 8, DE_MEM_GENERAL);
        if (p == NULL)
            return;
        if (t->assigned != NULL) {
            memcpy(p, t->assigned, sizeof(u64) * t->assigned_count);
            DE_FREE(al, t->assigned, sizeof(u64) * t->assigned_cap);
        }
        t->assigned = p;
        t->assigned_cap = cap;
    }
    t->assigned[t->assigned_count++] = e;
}

/* The entities spawned by open transactions (DIDE-358): world.delta reports them once their
 * transaction commits, after its edit.applied, and never when it is cancelled. Fills up to `cap`;
 * returns the count. */
u32 de__ed_edit_open_spawns(u64 *out, u32 cap) {
    u32 n = 0;
    for (u32 i = 0; i < ED_MAX_TX; ++i) {
        const ed_tx *t = &g_edit.tx[i];
        if (t->state != TX_OPEN)
            continue;
        for (u32 k = 0; k < t->assigned_count; ++k, ++n)
            if (out != NULL && n < cap)
                out[n] = t->assigned[k];
    }
    return n;
}

static ed_tx *tx_find(u32 id) {
    for (u32 i = 0; i < ED_MAX_TX; ++i)
        if (g_edit.tx[i].state != TX_FREE && g_edit.tx[i].id == id)
            return &g_edit.tx[i];
    return NULL;
}

/* ---------- Applying one op ---------- */

static de_world *W(de__editor *ed) {
    return ed->e->world;
}

static bool is_hierarchy(de__editor *ed, de_component c) {
    const de_builtin_component_ids b = de_builtin_components(W(ed));
    return c.id == b.child_of.id || c.id == b.children.id || c.id == b.global_transform.id;
}

/* The entity's components (minus the hierarchy's own) as a raw map, for a despawn's inverse. */
static de_ep_raw components_raw(de__editor *ed, de_ep_arena *a, de_entity e) {
    de_world *w = W(ed);
    const ecs_record *rec = ecs_entity_record(w, e);
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_build_map(&mw);
    if (rec != NULL) {
        const ecs_archetype *arch = ((ecs_archetype *const *)w->archetypes.data)[rec->archetype];
        const ecs_chunk *ch = ecs_table_chunk(arch, rec->chunk);
        for (u32 i = 0; i < arch->component_count; ++i) {
            const de_component c = {arch->components[i]};
            const ecs_component_info *ci = ecs_component_get(w, c.id);
            if (ci == NULL || ci->name == NULL || is_hierarchy(ed, c))
                continue;
            mpack_write_cstr(&mw, ci->name);
            const u32 col = arch->column_of[i];
            if (col == ECS_INVALID_INDEX || ci->size == 0) {
                mpack_build_map(&mw);
                mpack_complete_map(&mw);
            } else {
                de__ed_write_object(ed, &mw, ci->type,
                                    (const u8 *)ecs_chunk_column(arch, ch, col) +
                                        (size_t)rec->row * ci->size);
            }
        }
    }
    mpack_complete_map(&mw);
    return de__ed_raw_end(a, &mw);
}

/* Spawns an entity, at `want`'s id when that index is free (an undone despawn). */
static de_entity spawn_at(de__editor *ed, u64 want) {
    de_world *w = W(ed);
    if (want != 0) {
        const de_entity e = {want};
        const u32 index = de_entity_index(e), gen = de_entity_generation(e);
        u32 *free = (u32 *)w->free_indices.data;
        for (u32 i = 0; i < w->free_indices.count && index < w->records.count && gen != 0; ++i) {
            if (free[i] != index)
                continue;
            ecs_archetype *empty = ((ecs_archetype **)w->archetypes.data)[ECS_EMPTY_ARCHETYPE];
            ecs_record *rec = (ecs_record *)de_array_at(&w->records, index);
            const u32 old_gen = rec->generation;
            rec->generation = gen;
            u32 chunk = 0, row = 0;
            if (ecs_table_push_row(w, empty, e, &chunk, &row) != DE_OK) {
                rec->generation = old_gen;
                de_error_clear();
                break;
            }
            rec->archetype = empty->index;
            rec->chunk = chunk;
            rec->row = row;
            free[i] = free[w->free_indices.count - 1];
            --w->free_indices.count;
            return e;
        }
    }
    return de_entity_spawn(w);
}

/* Sets a component's data from a raw map value (none: zeroed/default). */
static bool add_with(de__editor *ed, de_entity e, de_component c, const de_ep_raw *value,
                     ed_fail *f) {
    de_type_id type;
    u32 size = 0;
    de__ed_component_type(ed, c, &type, &size);
    void *data = de_entity_add(W(ed), e, c);
    if (data == NULL && size != 0)
        return fail(f, DE_EP_ERR_INTERNAL, "could not add the component");
    if (value == NULL || value->len == 0 || size == 0)
        return true;
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, value->ptr, value->len);
    mpack_tree_parse(&tree);
    bool ok = mpack_tree_error(&tree) == mpack_ok &&
              (mpack_node_type(mpack_tree_root(&tree)) == mpack_type_nil ||
               de__ed_read_object(ed, mpack_tree_root(&tree), type, data));
    mpack_tree_destroy(&tree);
    return ok ? true : fail(f, DE_EP_ERR_INVALID_PARAMS, "the value doesn't match the component");
}

static de_entity parent_of(de__editor *ed, de_entity e) {
    return de_entity_parent(W(ed), e);
}
/* Parents `e` to `p`, at `o`'s index when it has one (DENG-778). */
static de_result set_parent_op(de_world *w, de_entity e, de_entity p, const de_ep_edit_op *o) {
    if (o->has_index && p.bits != 0)
        return de_entity_set_parent_at(w, e, p, o->index);
    return de_entity_set_parent(w, e, p);
}
/* Gives the inverse `inv` the place of `e` among its siblings, when it has a parent. */
static void inv_index(de__editor *ed, de_entity e, de_ep_edit_op *inv) {
    const u32 i = de_entity_child_index(W(ed), e);
    if (i != UINT32_MAX) {
        inv->has_index = true;
        inv->index = i;
    }
}

/* ---------- Prefab instances (DENG-764) ---------- */

#define ED_MAX_TREE 1024u

/* The instance under `root` as an `instantiate` value (see the top of the file). */
static bool snapshot_instance(de__editor *ed, de_ep_arena *a, de_entity root, const char *asset,
                              de_ep_raw *out) {
    u64 *ents = (u64 *)de__ed_alloc(a, sizeof(u64) * ED_MAX_TREE);
    de_ep_raw *comps = (de_ep_raw *)de__ed_alloc(a, sizeof(de_ep_raw) * ED_MAX_TREE);
    if (ents == NULL || comps == NULL)
        return false;
    const u32 n = de__ed_scene_tree(ed, root, ents, ED_MAX_TREE);
    if (n >= ED_MAX_TREE)
        return false;
    /* Each raw writer takes the rest of the arena, so the components go first. */
    for (u32 i = 0; i < n; ++i) {
        comps[i] = components_raw(ed, a, (de_entity){ents[i]});
        if (comps[i].ptr == NULL)
            return false;
    }
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 4);
    mpack_write_cstr(&mw, "asset");
    mpack_write_cstr(&mw, asset);
    mpack_write_cstr(&mw, "entities");
    mpack_start_array(&mw, n);
    for (u32 i = 0; i < n; ++i) {
        const de_entity e = {ents[i]};
        u64 scene = 0, sid = 0;
        (void)de__ed_scene_member(e.bits, &scene, &sid);
        mpack_start_map(&mw, 5);
        mpack_write_cstr(&mw, "id");
        mpack_write_u64(&mw, e.bits);
        mpack_write_cstr(&mw, "parent");
        mpack_write_u64(&mw, parent_of(ed, e).bits);
        mpack_write_cstr(&mw, "scene");
        mpack_write_u64(&mw, scene);
        mpack_write_cstr(&mw, "sid");
        mpack_write_u64(&mw, sid);
        mpack_write_cstr(&mw, "components");
        mpack_write_object_bytes(&mw, comps[i].ptr, comps[i].len);
        mpack_finish_map(&mw);
    }
    mpack_finish_array(&mw);
    mpack_write_cstr(&mw, "base");
    de__ed_prefab_instance_write_base(root.bits, &mw);
    mpack_write_cstr(&mw, "local");
    de__ed_prefab_instance_write_local(root.bits, &mw);
    mpack_finish_map(&mw);
    *out = de__ed_raw_end(a, &mw);
    return out->ptr != NULL;
}

typedef struct snap_ent {
    u64 id, parent, scene, sid;
    de_ep_kv_raw *comps;
    u32 comp_count;
} snap_ent;

typedef struct snap {
    char asset[256];
    snap_ent *ents;
    u32 count;
    const char *base;
    size_t base_len;
    const char *local; /* [entity, prefab-local id] pairs (DENG-571) */
    size_t local_len;
} snap;

static bool snapshot_read(de_ep_arena *a, de_ep_raw value, snap *out) {
    memset(out, 0, sizeof *out);
    mpack_reader_t r;
    mpack_reader_init_data(&r, value.ptr, value.len);
    const u32 keys = mpack_expect_map_max(&r, 16);
    for (u32 k = 0; k < keys && mpack_reader_error(&r) == mpack_ok; ++k) {
        char key[16];
        mpack_expect_cstr(&r, key, sizeof key);
        if (strcmp(key, "asset") == 0) {
            mpack_expect_cstr(&r, out->asset, sizeof out->asset);
        } else if (strcmp(key, "entities") == 0 && out->ents == NULL) {
            const u32 n = mpack_expect_array_max(&r, ED_MAX_TREE);
            out->ents = (snap_ent *)de__ed_alloc(a, sizeof(snap_ent) * (n + 1));
            if (out->ents == NULL)
                mpack_reader_flag_error(&r, mpack_error_memory);
            for (u32 i = 0; i < n && mpack_reader_error(&r) == mpack_ok; ++i) {
                snap_ent *e = &out->ents[out->count++];
                memset(e, 0, sizeof *e);
                const u32 fields = mpack_expect_map_max(&r, 16);
                for (u32 f = 0; f < fields && mpack_reader_error(&r) == mpack_ok; ++f) {
                    char name[16];
                    mpack_expect_cstr(&r, name, sizeof name);
                    if (strcmp(name, "id") == 0) {
                        e->id = mpack_expect_u64(&r);
                    } else if (strcmp(name, "parent") == 0) {
                        e->parent = mpack_expect_u64(&r);
                    } else if (strcmp(name, "scene") == 0) {
                        e->scene = mpack_expect_u64(&r);
                    } else if (strcmp(name, "sid") == 0) {
                        e->sid = mpack_expect_u64(&r);
                    } else if (strcmp(name, "components") == 0 && e->comps == NULL) {
                        const u32 m = mpack_expect_map_max(&r, 4096);
                        e->comps = (de_ep_kv_raw *)de__ed_alloc(a, sizeof(de_ep_kv_raw) * (m + 1));
                        if (e->comps == NULL)
                            mpack_reader_flag_error(&r, mpack_error_memory);
                        for (u32 c = 0; c < m && mpack_reader_error(&r) == mpack_ok; ++c)
                            if (de__ed_raw_map_entry(&r, &e->comps[e->comp_count]))
                                ++e->comp_count;
                        mpack_done_map(&r);
                    } else {
                        mpack_discard(&r);
                    }
                }
                mpack_done_map(&r);
            }
            mpack_done_array(&r);
        } else if (strcmp(key, "base") == 0 || strcmp(key, "local") == 0) {
            const bool base = key[0] == 'b';
            const char *begin = r.data;
            mpack_discard(&r);
            if (mpack_reader_error(&r) == mpack_ok) {
                *(base ? &out->base : &out->local) = begin;
                *(base ? &out->base_len : &out->local_len) = (size_t)(r.data - begin);
            }
        } else {
            mpack_discard(&r);
        }
    }
    mpack_done_map(&r);
    return mpack_reader_destroy(&r) == mpack_ok && out->count > 0 && out->asset[0] != '\0';
}

/* Takes the first `n` entities of a restore back out. */
static void restore_undo(de__editor *ed, const snap *s, u32 n) {
    for (u32 i = n; i > 0; --i) {
        de__ed_scene_member_drop(s->ents[i - 1].id);
        if (de_entity_alive(W(ed), (de_entity){s->ents[i - 1].id}))
            de_entity_despawn(W(ed), (de_entity){s->ents[i - 1].id});
    }
}

/* `instantiate` with a snapshot: the same ids, scene ids, components and instance record. */
static bool restore_instance(de__editor *ed, de_ep_arena *a, const de_ep_edit_op *o,
                             de_entity parent, de_entity *root, ed_fail *f) {
    de_world *w = W(ed);
    snap s;
    if (!snapshot_read(a, o->value, &s))
        return fail(f, DE_EP_ERR_INVALID_PARAMS, "instantiate value is not an instance snapshot");
    if (o->has_entity && o->entity != s.ents[0].id)
        return fail(f, DE_EP_ERR_INVALID_PARAMS, "entity is not the snapshot's root");
    /* Check everything first: all or nothing. */
    for (u32 i = 0; i < s.count; ++i) {
        if (s.ents[i].id == 0 || de_entity_alive(w, (de_entity){s.ents[i].id}))
            return fail(f, DE_EP_ERR_CONFLICT, "entity %llu exists",
                        (unsigned long long)s.ents[i].id);
        for (u32 c = 0; c < s.ents[i].comp_count; ++c)
            if (de__ed_component(ed, s.ents[i].comps[c].key).id == 0)
                return fail(f, DE_EP_ERR_INVALID_PARAMS, "no component type %.*s",
                            (int)s.ents[i].comps[c].key.len, s.ents[i].comps[c].key.ptr);
    }
    for (u32 i = 0; i < s.count; ++i) {
        const de_entity e = spawn_at(ed, s.ents[i].id);
        if (e.bits != s.ents[i].id) {
            if (de_entity_alive(w, e))
                de_entity_despawn(w, e);
            restore_undo(ed, &s, i);
            return fail(f, DE_EP_ERR_CONFLICT, "entity id %llu is taken",
                        (unsigned long long)s.ents[i].id);
        }
    }
    for (u32 i = 0; i < s.count; ++i) {
        const de_entity e = {s.ents[i].id};
        for (u32 c = 0; c < s.ents[i].comp_count; ++c) {
            const de_component comp = de__ed_component(ed, s.ents[i].comps[c].key);
            if (!add_with(ed, e, comp, &s.ents[i].comps[c].value, f)) {
                restore_undo(ed, &s, s.count);
                return false;
            }
        }
        const de_entity p = i == 0 ? parent : (de_entity){s.ents[i].parent};
        if (p.bits != 0 && de_entity_set_parent(w, e, p) != DE_OK) {
            de_error_clear();
            restore_undo(ed, &s, s.count);
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "parent %llu", (unsigned long long)p.bits);
        }
    }
    for (u32 i = 0; i < s.count; ++i) {
        const de_entity e = {s.ents[i].id};
        if (s.ents[i].sid != 0)
            (void)de__ed_scene_member_set(e.bits, s.ents[i].scene ? s.ents[i].scene : o->scene,
                                          s.ents[i].sid);
        else
            de__ed_scene_adopt(ed, e, o->scene, i == 0 ? parent : (de_entity){s.ents[i].parent});
    }
    de__ed_prefab_instance_drop(s.ents[0].id); /* a stale record of an old root */
    static const char no_base[] = "\x90";
    if (!de__ed_prefab_instance_restore(s.ents[0].id, s.asset, s.base ? s.base : no_base,
                                        s.base ? s.base_len : 1, s.local, s.local_len)) {
        restore_undo(ed, &s, s.count);
        return fail(f, DE_EP_ERR_INTERNAL, "could not restore the instance record");
    }
    de__ed_world_touch(ed);
    *root = (de_entity){s.ents[0].id};
    return true;
}

/* `uninstantiate`: the whole tree, its scene memberships and the instance record go. */
static bool remove_instance(de__editor *ed, de_ep_arena *a, de_entity root, de_ep_edit_op *inv,
                            ed_fail *f) {
    const char *asset = de__ed_prefab_instance_asset(root.bits);
    if (asset == NULL)
        return fail(f, DE_EP_ERR_INVALID_PARAMS, "%llu is not a prefab instance root",
                    (unsigned long long)root.bits);
    char *copy = de__ed_cstr(a, de_ep_cstr(asset));
    de_ep_raw value;
    if (copy == NULL || !snapshot_instance(ed, a, root, copy, &value))
        return fail(f, DE_EP_ERR_INTERNAL, "could not capture the instance");
    u64 *ents = (u64 *)de__ed_alloc(a, sizeof(u64) * ED_MAX_TREE);
    if (ents == NULL)
        return fail(f, DE_EP_ERR_BUSY, "out of scratch memory");
    inv->op = de_ep_cstr("instantiate");
    inv->has_scene = true;
    inv->scene = de__ed_scene_of(ed, root);
    inv->has_entity = true;
    inv->entity = root.bits;
    inv->has_path = true;
    inv->path = de_ep_cstr(copy);
    inv->has_value = true;
    inv->value = value;
    const de_entity p = parent_of(ed, root);
    if (p.bits != 0) {
        inv->has_parent = true;
        inv->parent = p.bits;
        inv_index(ed, root, inv);
    }
    /* Children first, so no despawn leaves an orphan behind. */
    const u32 n = de__ed_scene_tree(ed, root, ents, ED_MAX_TREE);
    for (u32 i = n; i > 0; --i) {
        de__ed_scene_member_drop(ents[i - 1]);
        de_entity_despawn(W(ed), (de_entity){ents[i - 1]});
    }
    de__ed_prefab_instance_drop(root.bits);
    de__ed_world_touch(ed);
    return true;
}

/* `prefab.unpack` (DENG-801): the instance record goes; the entities stay, now plain. Its inverse
 * `prefab.pack` carries the record exactly (asset, base values, prefab-local ids), so undo puts
 * the instance, and with it every override, back. Overrides are not stored: they are the
 * difference between the live values and the base values. */
static bool unpack_instance(de__editor *ed, de_ep_arena *a, de_entity root, de_ep_edit_op *inv,
                            ed_fail *f) {
    const char *asset = de__ed_prefab_instance_asset(root.bits);
    if (asset == NULL)
        return fail(f, DE_EP_ERR_INVALID_PARAMS, "%llu is not a prefab instance root",
                    (unsigned long long)root.bits);
    char *copy = de__ed_cstr(a, de_ep_cstr(asset));
    if (copy == NULL)
        return fail(f, DE_EP_ERR_BUSY, "out of scratch memory");
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 3);
    mpack_write_cstr(&mw, "asset");
    mpack_write_cstr(&mw, copy);
    mpack_write_cstr(&mw, "base");
    de__ed_prefab_instance_write_base(root.bits, &mw);
    mpack_write_cstr(&mw, "local");
    de__ed_prefab_instance_write_local(root.bits, &mw);
    mpack_finish_map(&mw);
    const de_ep_raw value = de__ed_raw_end(a, &mw);
    if (value.ptr == NULL)
        return fail(f, DE_EP_ERR_INTERNAL, "could not capture the instance record");
    inv->op = de_ep_cstr("prefab.pack");
    inv->has_entity = true;
    inv->entity = root.bits;
    inv->has_value = true;
    inv->value = value;
    de__ed_prefab_instance_drop(root.bits);
    de__ed_world_touch(ed);
    return true;
}

/* `prefab.pack`: puts the instance record that `prefab.unpack` captured back on `root`. */
static bool pack_instance(de__editor *ed, de_ep_raw value, de_entity root, de_ep_edit_op *inv,
                          ed_fail *f) {
    if (de__ed_prefab_instance_asset(root.bits) != NULL)
        return fail(f, DE_EP_ERR_CONFLICT, "%llu is already a prefab instance",
                    (unsigned long long)root.bits);
    char asset[256] = "";
    const char *base = NULL, *local = NULL;
    size_t base_len = 0, local_len = 0;
    mpack_reader_t r;
    mpack_reader_init_data(&r, value.ptr, value.len);
    const u32 keys = mpack_expect_map_max(&r, 8);
    for (u32 k = 0; k < keys && mpack_reader_error(&r) == mpack_ok; ++k) {
        char key[16];
        mpack_expect_cstr(&r, key, sizeof key);
        if (strcmp(key, "asset") == 0) {
            mpack_expect_cstr(&r, asset, sizeof asset);
        } else if (strcmp(key, "base") == 0 || strcmp(key, "local") == 0) {
            const bool is_base = key[0] == 'b';
            const char *begin = r.data;
            mpack_discard(&r);
            if (mpack_reader_error(&r) == mpack_ok) {
                *(is_base ? &base : &local) = begin;
                *(is_base ? &base_len : &local_len) = (size_t)(r.data - begin);
            }
        } else {
            mpack_discard(&r);
        }
    }
    mpack_done_map(&r);
    if (mpack_reader_destroy(&r) != mpack_ok || asset[0] == '\0' || base == NULL)
        return fail(f, DE_EP_ERR_INVALID_PARAMS, "prefab.pack value is not an instance record");
    if (!de__ed_prefab_instance_restore(root.bits, asset, base, base_len, local, local_len))
        return fail(f, DE_EP_ERR_INTERNAL, "could not restore the instance record");
    inv->op = de_ep_cstr("prefab.unpack");
    inv->has_entity = true;
    inv->entity = root.bits;
    de__ed_world_touch(ed);
    return true;
}

static bool apply_op(de__editor *ed, de_ep_arena *a, const de_ep_edit_op *o, de_ep_edit_op *inv,
                     u64 *spawned, ed_fail *f) {
    de_world *w = W(ed);
    memset(inv, 0, sizeof *inv);
    *spawned = 0;
    const de_entity e = {o->has_entity ? o->entity : 0};

    if (de_ep_str_eq(o->op, "spawn")) {
        if (o->has_parent && o->parent != 0 && !de_entity_alive(w, (de_entity){o->parent}))
            return fail(f, DE_EP_ERR_NOT_FOUND, "parent %llu", (unsigned long long)o->parent);
        if (o->has_entity && de_entity_alive(w, e))
            return fail(f, DE_EP_ERR_CONFLICT, "entity %llu exists", (unsigned long long)e.bits);
        /* Check every component first: all or nothing. */
        for (u32 i = 0; o->has_components && i < o->components.count; ++i)
            if (de__ed_component(ed, o->components.items[i].key).id == 0)
                return fail(f, DE_EP_ERR_INVALID_PARAMS, "no component type %.*s",
                            (int)o->components.items[i].key.len, o->components.items[i].key.ptr);
        const de_entity ne = spawn_at(ed, o->has_entity ? o->entity : 0);
        if (!de_entity_alive(w, ne))
            return fail(f, DE_EP_ERR_INTERNAL, "spawn failed");
        for (u32 i = 0; o->has_components && i < o->components.count; ++i) {
            const de_component c = de__ed_component(ed, o->components.items[i].key);
            if (!add_with(ed, ne, c, &o->components.items[i].value, f)) {
                de_entity_despawn(w, ne);
                return false;
            }
        }
        if (o->has_parent && o->parent != 0)
            (void)set_parent_op(w, ne, (de_entity){o->parent}, o);
        de__ed_scene_adopt(ed, ne, o->has_scene ? o->scene : 0,
                           (de_entity){o->has_parent ? o->parent : 0});
        *spawned = ne.bits;
        inv->op = de_ep_cstr("despawn");
        inv->has_entity = true;
        inv->entity = ne.bits;
        return true;
    }

    if (de_ep_str_eq(o->op, "instantiate")) {
        if (!o->has_path || o->path.len == 0)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "instantiate needs path (the prefab)");
        const de_entity parent = {o->has_parent ? o->parent : 0};
        if (parent.bits != 0 && !de_entity_alive(w, parent))
            return fail(f, DE_EP_ERR_NOT_FOUND, "parent %llu", (unsigned long long)parent.bits);
        de_entity root = DE_ENTITY_NULL;
        if (o->has_value) {
            if (!restore_instance(ed, a, o, parent, &root, f))
                return false;
        } else {
            de_ep_error code = DE_EP_OK;
            char msg[256];
            if (!de__ed_prefab_spawn(ed, a, de__ed_cstr(a, o->path), o->has_scene ? o->scene : 0,
                                     parent, &root, &code, msg, sizeof msg))
                return fail(f, code, "%s", msg);
        }
        /* The instance root goes to its place among the parent's children (DENG-778). */
        if (o->has_index && parent.bits != 0 &&
            de_entity_set_parent_at(w, root, parent, o->index) != DE_OK)
            de_error_clear(); /* already parented; it stays last */
        *spawned = root.bits;
        inv->op = de_ep_cstr("uninstantiate");
        inv->has_entity = true;
        inv->entity = root.bits;
        return true;
    }

    /* DENG-571: prefab definition and instance base values, from a prefab apply. */
    if (de_ep_str_eq(o->op, "prefab.state")) {
        if (!o->has_value)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "prefab.state needs value");
        de_ep_raw old;
        if (!de__ed_prefab_state(ed, a, o->value, &old, &f->code, f->msg, sizeof f->msg)) {
            f->reason = de__ed_prefab_fail_reason();
            return false;
        }
        inv->op = o->op;
        inv->has_value = true;
        inv->value = old;
        return true;
    }

    if (!de_entity_alive(w, e))
        return fail(f, DE_EP_ERR_NOT_FOUND, "entity %llu", (unsigned long long)e.bits);

    if (de_ep_str_eq(o->op, "uninstantiate"))
        return remove_instance(ed, a, e, inv, f);

    if (de_ep_str_eq(o->op, "prefab.unpack"))
        return unpack_instance(ed, a, e, inv, f);

    if (de_ep_str_eq(o->op, "prefab.pack")) {
        if (!o->has_value)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "prefab.pack needs value");
        return pack_instance(ed, o->value, e, inv, f);
    }

    if (de_ep_str_eq(o->op, "despawn")) {
        inv->op = de_ep_cstr("spawn");
        inv->has_scene = true;
        inv->scene = de__ed_scene_of(ed, e);
        inv->has_entity = true;
        inv->entity = e.bits;
        const de_entity p = parent_of(ed, e);
        if (p.bits != 0) {
            inv->has_parent = true;
            inv->parent = p.bits;
            inv_index(ed, e, inv);
        }
        /* The components as a map, split into kv pairs for the EditOp. */
        const de_ep_raw all = components_raw(ed, a, e);
        mpack_reader_t reader;
        mpack_reader_init_data(&reader, all.ptr, all.len);
        const u32 n = mpack_expect_map(&reader);
        de_ep_kv_raw *kv = (de_ep_kv_raw *)de__ed_alloc(a, sizeof(de_ep_kv_raw) * (n + 1));
        if (kv == NULL)
            mpack_reader_flag_error(&reader, mpack_error_memory);
        for (u32 i = 0; i < n && mpack_reader_error(&reader) == mpack_ok; ++i) {
            if (!de__ed_raw_map_entry(&reader, &kv[i]))
                break;
        }
        mpack_done_map(&reader);
        if (mpack_reader_destroy(&reader) != mpack_ok)
            return fail(f, DE_EP_ERR_INTERNAL, "could not capture despawned components");
        inv->has_components = true;
        inv->components.items = kv;
        inv->components.count = n;
        /* Both keys and complete values borrow `all`, which stays in the arena. */
        de_entity_despawn(w, e);
        return true;
    }

    if (de_ep_str_eq(o->op, "add") || de_ep_str_eq(o->op, "remove")) {
        if (!o->has_component)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "%.*s needs component", (int)o->op.len,
                        o->op.ptr);
        const de_component c = de__ed_component(ed, o->component);
        if (c.id == 0)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "no component type %.*s",
                        (int)o->component.len, o->component.ptr);
        const bool has = de_entity_has(w, e, c);
        if (de_ep_str_eq(o->op, "add")) {
            if (has)
                return fail(f, DE_EP_ERR_CONFLICT, "%llu has %.*s", (unsigned long long)e.bits,
                            (int)o->component.len, o->component.ptr);
            if (!add_with(ed, e, c, o->has_value ? &o->value : NULL, f)) {
                de_entity_remove(w, e, c);
                return false;
            }
            inv->op = de_ep_cstr("remove");
        } else {
            if (!has)
                return fail(f, DE_EP_ERR_NOT_FOUND, "%llu has no %.*s", (unsigned long long)e.bits,
                            (int)o->component.len, o->component.ptr);
            de_ep_list_str only = {(de_ep_str *)&o->component, 1};
            de_ep_entity_row row;
            if (de__ed_entity_row(ed, a, e, &only, NULL, &row) && row.components.count == 1) {
                inv->has_value = true;
                inv->value = row.components.items[0].value;
            }
            de_entity_remove(w, e, c);
            inv->op = de_ep_cstr("add");
        }
        inv->has_entity = true;
        inv->entity = e.bits;
        inv->has_component = true;
        inv->component = o->component;
        return true;
    }

    if (de_ep_str_eq(o->op, "set")) {
        if (!o->has_path || !o->has_value)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "set needs path and value");
        if (o->has_base_rev && o->base_rev != de__ed_entity_rev(ed, e))
            return fail(f, DE_EP_ERR_CONFLICT, "entity %llu changed since revision %llu",
                        (unsigned long long)e.bits, (unsigned long long)o->base_rev);
        char path[256];
        if (o->path.len >= sizeof path)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "path too long");
        memcpy(path, o->path.ptr, o->path.len);
        path[o->path.len] = '\0';
        char *dot = strchr(path, '.');
        const de_component c =
            de__ed_component(ed, (de_ep_str){path, dot ? (u32)(dot - path) : o->path.len});
        de_type_id type;
        u32 size = 0;
        if (c.id == 0 || !de__ed_component_type(ed, c, &type, &size) || !de_entity_has(w, e, c))
            return fail(f, DE_EP_ERR_NOT_FOUND, "%llu has no component for %s",
                        (unsigned long long)e.bits, path);
        u8 *data = (u8 *)de_entity_get(w, e, c);
        if (data == NULL || size == 0)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "%s has no data", path);
        const de_field_info *field = NULL;
        u32 off = 0;
        if (dot != NULL &&
            de_reflect_resolve_path(ed->e->types, type, dot + 1, &field, &off) != DE_OK) {
            de_error_clear();
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "no field %s", path);
        }
        if (field != NULL && (field->flags & DE_FIELD_READONLY))
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "%s is read-only", path);
        /* A fixed array's element needs its owner and field index, for the old value and for the
         * new one alike (DENG-793); whole arrays go through the struct. */
        de_type_id owner = {0};
        u32 index = 0;
        if (field != NULL) {
            u32 fc = 0;
            const de_field_info *fields = de_type_fields(ed->e->types, type, &fc);
            for (; index < fc && &fields[index] != field; ++index) {
            }
            if (index < fc)
                owner = type;
        }
        /* The old value, for the inverse. */
        mpack_writer_t mw;
        de__ed_raw_begin(a, &mw);
        if (field != NULL)
            de__ed_write_field(ed, &mw, owner, index, field, data + off);
        else
            de__ed_write_object(ed, &mw, type, data);
        const de_ep_raw old = de__ed_raw_end(a, &mw);
        /* Decode into a copy, so a bad value changes nothing. */
        u8 *copy = (u8 *)de__ed_alloc(a, size);
        if (copy == NULL)
            return fail(f, DE_EP_ERR_BUSY, "out of scratch memory");
        memcpy(copy, data, size);
        mpack_tree_t tree;
        mpack_tree_init_data(&tree, o->value.ptr, o->value.len);
        mpack_tree_parse(&tree);
        bool ok = mpack_tree_error(&tree) == mpack_ok;
        if (ok && field != NULL) {
            ok = de__ed_read_field(ed, mpack_tree_root(&tree), owner, index, field, copy + off);
        } else if (ok) {
            ok = de__ed_read_object(ed, mpack_tree_root(&tree), type, copy);
        }
        mpack_tree_destroy(&tree);
        if (!ok)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "the value doesn't fit %s", path);
        memcpy(data, copy, size);
        de_entity_mark_changed(w, e, c);
        inv->op = de_ep_cstr("set");
        inv->has_entity = true;
        inv->entity = e.bits;
        inv->has_path = true;
        inv->path = o->path;
        inv->has_value = true;
        inv->value = old;
        return true;
    }

    if (de_ep_str_eq(o->op, "reparent")) {
        const de_entity p = {o->has_parent ? o->parent : 0};
        if (p.bits != 0) {
            if (!de_entity_alive(w, p))
                return fail(f, DE_EP_ERR_NOT_FOUND, "parent %llu", (unsigned long long)p.bits);
            if (p.bits == e.bits)
                return fail(f, DE_EP_ERR_INVALID_PARAMS, "an entity can't be its own parent");
            for (de_entity x = p; x.bits != 0; x = parent_of(ed, x))
                if (x.bits == e.bits)
                    return fail(f, DE_EP_ERR_INVALID_PARAMS,
                                "an entity can't be moved under its own child");
        }
        const de_entity old = parent_of(ed, e);
        const u32 old_index = de_entity_child_index(w, e);
        /* Keep the world transform: the new local one is the old world one seen from the new
         * parent (default true). */
        const de_builtin_component_ids b = de_builtin_components(w);
        const bool keep = !o->has_keep_world || o->keep_world;
        DeTransform *local = (DeTransform *)de_entity_get(w, e, b.transform);
        const DeGlobalTransform *gw =
            (const DeGlobalTransform *)de_entity_get_const(w, e, b.global_transform);
        if (keep && local != NULL && gw != NULL) {
            const DeGlobalTransform *pg =
                p.bits ? (const DeGlobalTransform *)de_entity_get_const(w, p, b.global_transform)
                       : NULL;
            if (p.bits == 0) {
                *local = *gw;
            } else if (pg != NULL) {
                const de_transform pinv = de_transform_inverse(pg);
                *local = de_transform_mul(&pinv, gw);
            }
            de_entity_mark_changed(w, e, b.transform);
        }
        if (set_parent_op(w, e, p, o) != DE_OK) {
            de_error_clear();
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "reparent refused");
        }
        inv->op = de_ep_cstr("reparent");
        inv->has_entity = true;
        inv->entity = e.bits;
        if (old.bits != 0) {
            inv->has_parent = true;
            inv->parent = old.bits;
            inv->has_index = old_index != UINT32_MAX;
            inv->index = old_index;
        }
        inv->has_keep_world = true;
        inv->keep_world = keep;
        return true;
    }

    /* DENG-580: the world.terrain ops, in render builds; DENG-777: spline.carve, a terrain op. */
    if (ed->terrain_op != NULL && ((o->op.len > 8 && memcmp(o->op.ptr, "terrain.", 8) == 0) ||
                                   de_ep_str_eq(o->op, "spline.carve")))
        return ed->terrain_op(ed, a, o, inv, &f->code, f->msg, sizeof f->msg);

    return fail(f, DE_EP_ERR_INVALID_PARAMS, "unknown op %.*s", (int)o->op.len, o->op.ptr);
}

/* ---------- Notifications ---------- */

void de__ed_scene_mark_dirty(u64 scene);

static void send_dirty(de__editor *ed, u64 scene) {
    if (ed->mode != DE_EP_WORLD_MODE_EDIT)
        return; /* play-world edits are thrown away at Stop */
    de__ed_scene_mark_dirty(scene);
    de_ep_scene_params p = {scene};
    mpack_writer_t w;
    const u64 seq = de__ed_note_begin(ed, &w);
    de_ep_write_notify_scene_dirty(&w, seq, &p);
    de__ed_send(ed, &w, DE_EP_KIND_NOTIFICATION);
}

static void send_applied(de__editor *ed, const ed_tx *t) {
    de_ep_arena a = {ed->arena, ed->arena_cap, 0};
    de_ep_edit_applied d;
    memset(&d, 0, sizeof d);
    d.tx = t->id;
    d.inverse.items = (de_ep_edit_op *)de__ed_alloc(&a, sizeof(de_ep_edit_op) * (t->count + 1));
    for (u32 i = 0; i < t->count && d.inverse.items != NULL; ++i)
        if (op_of_blob(&t->pairs[t->count - 1 - i].inv, &a, &d.inverse.items[d.inverse.count]))
            ++d.inverse.count;
    d.assigned_ids.items = t->assigned;
    d.assigned_ids.count = t->assigned_count;
    mpack_writer_t w;
    const u64 seq = de__ed_note_begin(ed, &w);
    de_ep_write_notify_edit_applied(&w, seq, &d);
    de__ed_send(ed, &w, DE_EP_KIND_NOTIFICATION);
}

/* ---------- Methods ---------- */

/* A new open transaction of `scene`; NULL with `f` set (busy) when none is free. */
static ed_tx *tx_open(de__editor *ed, u64 scene, ed_fail *f) {
    u32 open = 0;
    ed_tx *slot = NULL;
    for (u32 i = 0; i < ED_MAX_TX; ++i) {
        open += g_edit.tx[i].state == TX_OPEN;
        if (slot == NULL && g_edit.tx[i].state == TX_FREE)
            slot = &g_edit.tx[i];
    }
    if (open >= DE_EP_LIMIT_OPEN_TRANSACTIONS) {
        (void)fail(f, DE_EP_ERR_BUSY, "too many open transactions");
        return NULL;
    }
    if (slot == NULL) {
        /* Recycle the oldest finished transaction (its undo history goes). */
        u32 oldest = 0xFFFFFFFFu;
        for (u32 i = 0; i < ED_MAX_TX; ++i)
            if (g_edit.tx[i].state != TX_OPEN && g_edit.tx[i].id < oldest) {
                oldest = g_edit.tx[i].id;
                slot = &g_edit.tx[i];
            }
        if (slot == NULL) {
            (void)fail(f, DE_EP_ERR_BUSY, "no free transaction");
            return NULL;
        }
        tx_free(slot);
    }
    slot->id = ++g_edit.next_id;
    slot->state = TX_OPEN;
    slot->in_play = ed->mode != DE_EP_WORLD_MODE_EDIT;
    slot->scene = scene;
    return slot;
}

static void on_begin(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(edit_begin, de_ep_edit_begin_params, q);
    ed_fail f = {DE_EP_OK, "", NULL};
    const ed_tx *slot = tx_open(ed, q.scene, &f);
    if (slot == NULL) {
        de__ed_error(w, req->id, f.code, "%s", f.msg);
        return;
    }
    de_ep_tx_result r = {slot->id};
    de_ep_write_response_edit_begin(w, req->id, &r);
}

/* Applies `ops` to open transaction `t`, all or nothing: on a failure what this call applied is
 * undone and `f` is set. */
static bool tx_apply(de__editor *ed, de_ep_arena *a, ed_tx *t, const de_ep_edit_op *ops, u32 n,
                     ed_fail *f) {
    const u32 first = t->count, first_assigned = t->assigned_count;
    for (u32 i = 0; i < n; ++i) {
        de_ep_edit_op inv, applied = ops[i];
        u64 spawned = 0;
        if (!applied.has_scene) {
            applied.has_scene = true;
            applied.scene = t->scene;
        }
        if (!apply_op(ed, a, &applied, &inv, &spawned, f)) {
            for (u32 k = t->count; k > first; --k) {
                de_ep_edit_op undo, ignored;
                u64 s2 = 0;
                ed_fail f2;
                if (op_of_blob(&t->pairs[k - 1].inv, a, &undo))
                    (void)apply_op(ed, a, &undo, &ignored, &s2, &f2);
                blob_free(&t->pairs[k - 1].fwd);
                blob_free(&t->pairs[k - 1].inv);
            }
            t->count = first;
            t->assigned_count = first_assigned;
            return false;
        }
        if (spawned != 0) {
            applied.has_entity = true;
            applied.entity = spawned;
            tx_assign(t, spawned);
        }
        tx_push(t, blob_of_op(ed, &applied), blob_of_op(ed, &inv));
    }
    return true;
}

static void on_apply(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(edit_apply, de_ep_edit_apply_params, q);
    ed_tx *t = tx_find(q.tx);
    if (t == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "transaction %u", q.tx);
        return;
    }
    if (t->state != TX_OPEN) {
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "transaction %u isn't open", q.tx);
        return;
    }
    ed_fail f = {DE_EP_OK, "", NULL};
    if (!tx_apply(ed, a, t, q.ops.items, q.ops.count, &f)) {
        de__ed_error(w, req->id, f.code, "%s", f.msg);
        return;
    }
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_edit_apply(w, req->id, &e);
}

static void on_commit(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(edit_commit, de_ep_tx_params, q);
    ed_tx *t = tx_find(q.tx);
    if (t == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "transaction %u", q.tx);
        return;
    }
    if (t->state != TX_OPEN) {
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "transaction %u isn't open", q.tx);
        return;
    }
    t->state = TX_COMMITTED;
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_edit_commit(w, req->id, &e);
    send_applied(ed, t);
    send_dirty(ed, t->scene);
}

u32 de__ed_edit_run(de__editor *ed, de_ep_arena *a, u32 tx, u64 scene, const de_ep_edit_op *ops,
                    u32 count, de_ep_error *code, char *msg, size_t msg_cap) {
    ed_fail f = {DE_EP_OK, "", NULL};
    ed_tx *t = NULL;
    if (tx != 0) {
        t = tx_find(tx);
        if (t == NULL)
            (void)fail(&f, DE_EP_ERR_NOT_FOUND, "transaction %u", tx);
        else if (t->state != TX_OPEN)
            (void)fail(&f, DE_EP_ERR_CONFLICT, "transaction %u isn't open", tx);
        if (f.code != DE_EP_OK)
            t = NULL;
    } else {
        t = tx_open(ed, scene, &f);
    }
    if (t != NULL && !tx_apply(ed, a, t, ops, count, &f)) {
        if (tx == 0)
            tx_free(t); /* nothing in it: the failed call undid its own ops */
        t = NULL;
    }
    if (t == NULL) {
        *code = f.code;
        snprintf(msg, msg_cap, "%s", f.msg);
        return 0;
    }
    const u32 id = t->id;
    if (tx == 0) {
        t->state = TX_COMMITTED;
        send_applied(ed, t);
        send_dirty(ed, t->scene);
    }
    return id;
}

/* Applies a transaction's inverses (undo) or ops (redo); false with `f` set on a failure. Each
 * replayed op's inverse replaces the other side of its pair: an undone `instantiate` is redone
 * from the snapshot its undo took, so it gets the same ids back. */
static bool replay(de__editor *ed, de_ep_arena *a, ed_tx *t, bool undo, ed_fail *f) {
    const de_allocator *al = de_heap_allocator();
    ed_blob *fresh = NULL;
    if (t->count != 0) {
        fresh = (ed_blob *)DE_ALLOC(al, sizeof(ed_blob) * t->count, 8, DE_MEM_GENERAL);
        if (fresh == NULL)
            return fail(f, DE_EP_ERR_BUSY, "out of memory");
        memset(fresh, 0, sizeof(ed_blob) * t->count);
    }
    bool ok = true;
    u32 done = 0;
    for (u32 i = 0; i < t->count && ok; ++i, done += ok) {
        const u32 k = undo ? t->count - 1 - i : i;
        const ed_pair *p = &t->pairs[k];
        de_ep_edit_op o, inv;
        u64 spawned = 0;
        if (!op_of_blob(undo ? &p->inv : &p->fwd, a, &o))
            ok = fail(f, DE_EP_ERR_INTERNAL, "a stored op did not decode");
        else if (!apply_op(ed, a, &o, &inv, &spawned, f))
            ok = false;
        else
            fresh[k] = blob_of_op(ed, &inv);
    }
    /* A failed op leaves the ones before it applied: put them back, newest first, so the world
     * (and any prefab source file) is as it was before the replay. */
    for (u32 j = ok ? 0 : done; j-- > 0;) {
        const u32 k = undo ? t->count - 1 - j : j;
        de_ep_edit_op o, inv;
        u64 spawned = 0;
        ed_fail ignored = {DE_EP_OK, "", NULL};
        if (fresh[k].p != NULL && op_of_blob(&fresh[k], a, &o))
            (void)apply_op(ed, a, &o, &inv, &spawned, &ignored);
    }
    for (u32 k = 0; k < t->count; ++k) {
        ed_blob *side = undo ? &t->pairs[k].fwd : &t->pairs[k].inv;
        if (ok && fresh[k].p != NULL) {
            blob_free(side);
            *side = fresh[k];
        } else {
            blob_free(&fresh[k]);
        }
    }
    if (fresh != NULL)
        DE_FREE(al, fresh, sizeof(ed_blob) * t->count);
    return ok;
}

static void on_cancel(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(edit_cancel, de_ep_tx_params, q);
    ed_tx *t = tx_find(q.tx);
    if (t == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "transaction %u", q.tx);
        return;
    }
    if (t->state != TX_OPEN) {
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "transaction %u isn't open", q.tx);
        return;
    }
    ed_fail f = {DE_EP_OK, "", NULL};
    (void)replay(ed, a, t, true, &f);
    tx_free(t);
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_edit_cancel(w, req->id, &e);
}

static void undo_redo(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w,
                      bool undo) {
    de_ep_tx_params q;
    memset(&q, 0, sizeof q);
    if (!(undo ? de_ep_read_params_edit_undo(req, a, &q)
               : de_ep_read_params_edit_redo(req, a, &q))) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "bad params");
        return;
    }
    ed_tx *t = tx_find(q.tx);
    if (t == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "transaction %u", q.tx);
        return;
    }
    if (t->state != (undo ? TX_COMMITTED : TX_UNDONE)) {
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "transaction %u can't be %s", q.tx,
                     undo ? "undone" : "redone");
        return;
    }
    ed_fail f = {DE_EP_OK, "", NULL};
    if (!replay(ed, a, t, undo, &f)) {
        if (f.reason != NULL && f.code != DE_EP_ERR_BUSY) {
            /* {id, error: {code, message, data: {reason}}} */
            mpack_start_map(w, 2);
            mpack_write_cstr(w, "id");
            mpack_write_u64(w, req->id);
            mpack_write_cstr(w, "error");
            mpack_start_map(w, 3);
            mpack_write_cstr(w, "code");
            mpack_write_i32(w, (int32_t)f.code);
            mpack_write_cstr(w, "message");
            mpack_write_cstr(w, f.msg);
            mpack_write_cstr(w, "data");
            mpack_start_map(w, 1);
            mpack_write_cstr(w, "reason");
            mpack_write_cstr(w, f.reason);
            mpack_finish_map(w);
            mpack_finish_map(w);
            mpack_finish_map(w);
        } else {
            de__ed_error(w, req->id, f.code, "%s", f.msg);
        }
        return;
    }
    t->state = undo ? TX_UNDONE : TX_COMMITTED;
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    if (undo)
        de_ep_write_response_edit_undo(w, req->id, &e);
    else
        de_ep_write_response_edit_redo(w, req->id, &e);
    send_dirty(ed, t->scene);
}

static void on_undo(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    undo_redo(ed, req, a, w, true);
}

static void on_redo(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    undo_redo(ed, req, a, w, false);
}

static void drop_all(void) {
    for (u32 i = 0; i < ED_MAX_TX; ++i)
        if (g_edit.tx[i].state != TX_FREE)
            tx_free(&g_edit.tx[i]);
}

/* The connection ended. IPC: the engine "cancels every open transaction", so each open one begun
 * in Edit is reverted as edit.cancel does, newest first. The session's reset ran before this one
 * (it registers first) and already put the pre-play world back, which discards the effects of
 * transactions begun in Play. Then every transaction goes: a new connection has a new epoch. */
static void reset(de__editor *ed) {
    const de_allocator *al = de_heap_allocator();
    unsigned char *scratch = NULL;
    for (;;) {
        ed_tx *t = NULL;
        for (u32 i = 0; i < ED_MAX_TX; ++i) {
            ed_tx *c = &g_edit.tx[i];
            if (c->state == TX_OPEN && !c->in_play && c->count != 0 && (t == NULL || c->id > t->id))
                t = c;
        }
        if (t == NULL)
            break;
        if (scratch == NULL)
            scratch = (unsigned char *)DE_ALLOC(al, ED_RESET_SCRATCH, 8, DE_MEM_GENERAL);
        if (scratch != NULL) {
            de_ep_arena a = {scratch, ED_RESET_SCRATCH, 0};
            ed_fail f = {DE_EP_OK, "", NULL};
            if (!replay(ed, &a, t, true, &f))
                DE_WARN(ed->log, "editor: could not cancel transaction %u at disconnect: %s", t->id,
                        f.msg);
        }
        tx_free(t);
    }
    if (scratch != NULL)
        DE_FREE(al, scratch, ED_RESET_SCRATCH);
    drop_all();
}

/** Drops every transaction without reverting it (a snapshot restore or Stop made their inverses
 * meaningless). */
void de__ed_edit_forget(de__editor *ed);
void de__ed_edit_forget(de__editor *ed) {
    (void)ed;
    drop_all();
}

void de__ed_edit_register(de__editor *ed) {
    ed->edit = (struct de__ed_edit *)&g_edit;
    de__ed_on(ed, DE_EP_METHOD_EDIT_BEGIN, on_begin);
    de__ed_on(ed, DE_EP_METHOD_EDIT_APPLY, on_apply);
    de__ed_on(ed, DE_EP_METHOD_EDIT_COMMIT, on_commit);
    de__ed_on(ed, DE_EP_METHOD_EDIT_CANCEL, on_cancel);
    de__ed_on(ed, DE_EP_METHOD_EDIT_UNDO, on_undo);
    de__ed_on(ed, DE_EP_METHOD_EDIT_REDO, on_redo);
    de__ed_on_reset(ed, reset);
}
