/* editor_edit.c: transactional edits with inverses, undo and redo (DENG-626).
 *
 * Ops: spawn, despawn, add, remove, set, reparent. They run between frames (the service's
 * frame-begin hook), so they use the world's immediate structural calls. Each applied op is kept
 * with its inverse as EditOp MessagePack bytes; undo applies the inverses in reverse, redo the
 * ops again. An undone despawn gets its entity id back: the freed index is taken again at the
 * old generation. */
#include "host/editor/editor_internal.h"

#include <dengine/de_memory.h>

#include "ecs/ecs_internal.h"
#include "host/host_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ED_MAX_TX 512u

typedef enum { TX_FREE, TX_OPEN, TX_COMMITTED, TX_UNDONE, TX_CANCELLED } tx_state;

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
                                    (const u8 *)ecs_chunk_column(arch, ch, col) + (size_t)rec->row * ci->size);
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
            (void)de_entity_set_parent(w, ne, (de_entity){o->parent});
        de__ed_scene_adopt(ed, ne, o->has_scene ? o->scene : 0,
                           (de_entity){o->has_parent ? o->parent : 0});
        *spawned = ne.bits;
        inv->op = de_ep_cstr("despawn");
        inv->has_entity = true;
        inv->entity = ne.bits;
        return true;
    }

    if (!de_entity_alive(w, e))
        return fail(f, DE_EP_ERR_NOT_FOUND, "entity %llu", (unsigned long long)e.bits);

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
        }
        /* The components as a map, split into kv pairs for the EditOp. */
        const de_ep_raw all = components_raw(ed, a, e);
        mpack_tree_t tree;
        mpack_tree_init_data(&tree, all.ptr, all.len);
        mpack_tree_parse(&tree);
        mpack_node_t root = mpack_tree_root(&tree);
        if (mpack_tree_error(&tree) == mpack_ok && mpack_node_type(root) == mpack_type_map) {
            const u32 n = (u32)mpack_node_map_count(root);
            de_ep_kv_raw *kv = (de_ep_kv_raw *)de__ed_alloc(a, sizeof(de_ep_kv_raw) * (n + 1));
            for (u32 i = 0; i < n && kv != NULL; ++i) {
                mpack_node_t k = mpack_node_map_key_at(root, i);
                mpack_node_t v = mpack_node_map_value_at(root, i);
                kv[i].key = (de_ep_str){mpack_node_str(k), (u32)mpack_node_strlen(k)};
                kv[i].value = (de_ep_raw){mpack_node_data(v), mpack_node_data_len(v)};
            }
            inv->has_components = kv != NULL;
            inv->components.items = kv;
            inv->components.count = kv ? n : 0;
        }
        /* The tree points into `all`, which lives in the arena; the kv pairs stay valid. */
        mpack_tree_destroy(&tree);
        de_entity_despawn(w, e);
        return true;
    }

    if (de_ep_str_eq(o->op, "add") || de_ep_str_eq(o->op, "remove")) {
        if (!o->has_component)
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "%.*s needs component", (int)o->op.len, o->op.ptr);
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
        const de_component c = de__ed_component(ed, (de_ep_str){path, dot ? (u32)(dot - path) : o->path.len});
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
        if (dot != NULL && de_reflect_resolve_path(ed->e->types, type, dot + 1, &field, &off) != DE_OK) {
            de_error_clear();
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "no field %s", path);
        }
        if (field != NULL && (field->flags & DE_FIELD_READONLY))
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "%s is read-only", path);
        /* The old value, for the inverse. */
        mpack_writer_t mw;
        de__ed_raw_begin(a, &mw);
        if (field != NULL)
            de__ed_write_field(ed, &mw, (de_type_id){0}, 0, field, data + off);
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
            /* A fixed array's element needs its owner; whole arrays go through the struct. */
            u32 fc = 0, index = 0;
            const de_field_info *fields = de_type_fields(ed->e->types, type, &fc);
            for (; index < fc && &fields[index] != field; ++index) {
            }
            ok = de__ed_read_field(ed, mpack_tree_root(&tree), index < fc ? type : (de_type_id){0},
                                   index, field, copy + off);
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
                    return fail(f, DE_EP_ERR_INVALID_PARAMS, "an entity can't be moved under its own child");
        }
        const de_entity old = parent_of(ed, e);
        /* Keep the world transform: the new local one is the old world one seen from the new
         * parent (default true). */
        const de_builtin_component_ids b = de_builtin_components(w);
        const bool keep = !o->has_keep_world || o->keep_world;
        DeTransform *local = (DeTransform *)de_entity_get(w, e, b.transform);
        const DeGlobalTransform *gw = (const DeGlobalTransform *)de_entity_get_const(w, e, b.global_transform);
        if (keep && local != NULL && gw != NULL) {
            const DeGlobalTransform *pg = p.bits ? (const DeGlobalTransform *)de_entity_get_const(w, p, b.global_transform) : NULL;
            if (p.bits == 0) {
                *local = *gw;
            } else if (pg != NULL) {
                const de_transform pinv = de_transform_inverse(pg);
                *local = de_transform_mul(&pinv, gw);
            }
            de_entity_mark_changed(w, e, b.transform);
        }
        if (de_entity_set_parent(w, e, p) != DE_OK) {
            de_error_clear();
            return fail(f, DE_EP_ERR_INVALID_PARAMS, "reparent refused");
        }
        inv->op = de_ep_cstr("reparent");
        inv->has_entity = true;
        inv->entity = e.bits;
        if (old.bits != 0) {
            inv->has_parent = true;
            inv->parent = old.bits;
        }
        inv->has_keep_world = true;
        inv->keep_world = keep;
        return true;
    }

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

static void on_begin(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(edit_begin, de_ep_edit_begin_params, q);
    u32 open = 0;
    ed_tx *slot = NULL;
    for (u32 i = 0; i < ED_MAX_TX; ++i) {
        open += g_edit.tx[i].state == TX_OPEN;
        if (slot == NULL && g_edit.tx[i].state == TX_FREE)
            slot = &g_edit.tx[i];
    }
    if (open >= DE_EP_LIMIT_OPEN_TRANSACTIONS) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many open transactions");
        return;
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
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "no free transaction");
            return;
        }
        tx_free(slot);
    }
    slot->id = ++g_edit.next_id;
    slot->state = TX_OPEN;
    slot->scene = q.scene;
    de_ep_tx_result r = {slot->id};
    de_ep_write_response_edit_begin(w, req->id, &r);
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
    const u32 first = t->count, first_assigned = t->assigned_count;
    for (u32 i = 0; i < q.ops.count; ++i) {
        de_ep_edit_op inv, applied = q.ops.items[i];
        u64 spawned = 0;
        ed_fail f = {DE_EP_OK, ""};
        if (!applied.has_scene) {
            applied.has_scene = true;
            applied.scene = t->scene;
        }
        if (!apply_op(ed, a, &applied, &inv, &spawned, &f)) {
            /* All or nothing: undo what this call applied. */
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
            de__ed_error(w, req->id, f.code, "%s", f.msg);
            return;
        }
        if (spawned != 0) {
            applied.has_entity = true;
            applied.entity = spawned;
            tx_assign(t, spawned);
        }
        tx_push(t, blob_of_op(ed, &applied), blob_of_op(ed, &inv));
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

/* Applies a transaction's inverses (undo) or ops (redo); false with `f` set on a failure. */
static bool replay(de__editor *ed, de_ep_arena *a, ed_tx *t, bool undo, ed_fail *f) {
    for (u32 i = 0; i < t->count; ++i) {
        const ed_pair *p = &t->pairs[undo ? t->count - 1 - i : i];
        de_ep_edit_op o, ignored;
        u64 spawned = 0;
        if (!op_of_blob(undo ? &p->inv : &p->fwd, a, &o))
            return fail(f, DE_EP_ERR_INTERNAL, "a stored op did not decode");
        if (!apply_op(ed, a, &o, &ignored, &spawned, f))
            return false;
    }
    return true;
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
    ed_fail f;
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
    if (!(undo ? de_ep_read_params_edit_undo(req, a, &q) : de_ep_read_params_edit_redo(req, a, &q))) {
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
    ed_fail f = {DE_EP_OK, ""};
    if (!replay(ed, a, t, undo, &f)) {
        de__ed_error(w, req->id, f.code, "%s", f.msg);
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

/* A new connection has a new epoch: older transactions are gone. */
static void reset(de__editor *ed) {
    (void)ed;
    for (u32 i = 0; i < ED_MAX_TX; ++i)
        if (g_edit.tx[i].state != TX_FREE)
            tx_free(&g_edit.tx[i]);
}

/** Drops every transaction (a snapshot restore or Stop made their inverses meaningless). */
void de__ed_edit_forget(de__editor *ed);
void de__ed_edit_forget(de__editor *ed) {
    reset(ed);
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
