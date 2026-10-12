/* hot_reload.c: the ECS side of game hot reload (DENG-93, SPEC §10.1): system owner tags,
 * component re-layout and the type sync after a game library registered its types again.
 *
 * Everything here is main-thread work between frames. Archetypes are visited in creation order,
 * rows in table order and components in id order, so a re-layout gives the same tables on every
 * machine (SPEC §5.7 rule 5); nothing iterates a hash map. */
#include "hot_reload_internal.h"

#include "commands_internal.h"
#include "ecs_internal.h"
#include "system_internal.h"

/* ---- System owners -------------------------------------------------------------------------- */

void de__world_set_system_owner(de_world *w, u32 owner) {
    if (w != NULL)
        w->system_owner = owner;
}

u32 de__world_system_owner(const de_world *w) {
    return w != NULL ? w->system_owner : 0u;
}

u32 de__world_remove_systems_by_owner(de_world *w, u32 owner) {
    if (w == NULL || w->systems == NULL || owner == DE__SYSTEM_OWNER_ENGINE)
        return 0;
    ECS_ASSERT_STRUCTURAL(w);
    ecs_system_registry *r = w->systems;
    u32 removed = 0;
    /* Slot order; de_system_remove keeps the slot array, so the indices stay valid. */
    for (u32 i = 0; i < r->slots.count; ++i) {
        const ecs_system *s = DE_ARRAY_AT(&r->slots, ecs_system_slot, i).sys;
        if (s != NULL && s->owner == owner) {
            de_system_remove(w, s->handle);
            ++removed;
        }
    }
    return removed;
}

u32 de__world_system_count_by_owner(const de_world *w, u32 owner) {
    if (w == NULL || w->systems == NULL)
        return 0;
    u32 n = 0;
    for (u32 i = 0; i < w->systems->slots.count; ++i) {
        const ecs_system *s = ((const ecs_system_slot *)de_array_at(&w->systems->slots, i))->sys;
        if (s != NULL && s->owner == owner)
            ++n;
    }
    return n;
}

/* ---- Object conversion ---------------------------------------------------------------------- */

static bool same_name(const char *a, const char *b) {
    if (a == NULL || b == NULL)
        return a == b;
    return strcmp(a, b) == 0;
}

static const de_field_info *find_field(const de_type_desc *d, const char *name) {
    for (u32 i = 0; i < d->field_count; ++i) {
        if (strcmp(d->fields[i].name, name) == 0)
            return &d->fields[i];
    }
    return NULL;
}

/* Bytes of one FIXED_ARRAY element (the registry checked that the size divides). */
static u32 array_elem_size(const de_field_info *f) {
    return f->attrs.array_count > 0 ? f->size / f->attrs.array_count : f->size;
}

/* Whether old field `o` can be copied into new field `n` (same name already). */
static bool field_compatible(const de_field_info *o, const de_field_info *n) {
    if (o->kind != n->kind)
        return false;
    switch (n->kind) {
    case DE_FK_STRING:
        return true; /* the buffer may grow or shrink */
    case DE_FK_FIXED_ARRAY:
        return same_name(o->attrs.nested_type, n->attrs.nested_type) &&
               array_elem_size(o) == array_elem_size(n);
    case DE_FK_STRUCT:
    case DE_FK_ENUM:
    case DE_FK_FLAGS:
    case DE_FK_ASSET:
        return same_name(o->attrs.nested_type, n->attrs.nested_type) && o->size == n->size;
    default:
        return o->size == n->size;
    }
}

static void copy_field(const de_field_info *o, const de_field_info *n, const u8 *src, u8 *dst) {
    const u8 *from = src + o->offset;
    u8 *to = dst + n->offset;
    if (n->kind == DE_FK_STRING) {
        const u32 bytes = o->size < n->size ? o->size : n->size;
        memcpy(to, from, bytes);
        to[n->size - 1u] = 0; /* a shorter buffer cuts the text; it stays terminated */
    } else if (n->kind == DE_FK_FIXED_ARRAY) {
        memcpy(to, from, o->size < n->size ? o->size : n->size);
    } else {
        memcpy(to, from, n->size);
    }
}

/* Converts one object from layout `od` to layout `nd` (see de__world_relayout_component). */
static void convert_object(const de_type_desc *od, const de_type_desc *nd, const void *src,
                           void *dst, u32 *refused) {
    memset(dst, 0, nd->size);
    if (od->field_count == 0 && nd->field_count == 0) {
        memcpy(dst, src, od->size < nd->size ? od->size : nd->size);
    } else {
        for (u32 i = 0; i < nd->field_count; ++i) {
            const de_field_info *n = &nd->fields[i];
            const de_field_info *o = find_field(od, n->name);
            if (o != NULL && field_compatible(o, n))
                copy_field(o, n, (const u8 *)src, (u8 *)dst);
        }
    }
    if (nd->hooks.migrate != NULL &&
        !nd->hooks.migrate(dst, od->version, src, od->fields, od->field_count))
        ++*refused;
}

/* The checks de_component_register / de_resource_insert / de_event_register make. */
static de_result check_desc(const de_world *w, const de_type_desc *d, bool chunk_row,
                            const char *what) {
    const u32 align = d->align ? d->align : 1u;
    if (!d->is_pod && (d->hooks.copy == NULL || d->hooks.destruct == NULL))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "%s '%s': neither POD nor has copy and destruct hooks (SPEC 7.1)", what,
                       d->name);
    if (align > DE_ECS_MAX_COMPONENT_ALIGN)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "%s '%s': alignment %u; the most is %u", what, d->name,
                       align, DE_ECS_MAX_COMPONENT_ALIGN);
    if (d->size == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s '%s': size 0", what, d->name);
    if (chunk_row &&
        sizeof(de_entity) + (u64)d->size + (align - 1u) + 2u * sizeof(u32) > (u64)w->chunk_bytes)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "%s '%s' (%u bytes) does not fit a %u-byte chunk", what,
                       d->name, d->size, w->chunk_bytes);
    return DE_OK;
}

/* ---- Component re-layout -------------------------------------------------------------------- */

/* The new table of one archetype. */
typedef struct relayout_plan {
    ecs_archetype *a;
    u32 column;          /* the re-laid-out component's column */
    u32 rows;            /* new rows per chunk */
    u32 used;            /* new chunk_used */
    ecs_column *columns; /* new column layout (a->column_count entries) */
    de_array chunks;     /* ecs_chunk, the new chunks (blocks from the world's pool) */
} relayout_plan;

static void plans_free(de_world *w, relayout_plan *plans, u32 count, bool free_blocks) {
    if (plans == NULL)
        return;
    for (u32 i = 0; i < count; ++i) {
        relayout_plan *p = &plans[i];
        if (free_blocks) {
            for (u32 k = 0; k < p->chunks.count; ++k)
                de_pool_free(w->chunk_pool, DE_ARRAY_AT(&p->chunks, ecs_chunk, k).data);
        }
        de_array_free(&p->chunks);
        if (p->columns != NULL)
            DE_FREE(w->a, p->columns, (size_t)p->a->column_count * sizeof(ecs_column));
    }
    DE_FREE(w->a, plans, (size_t)count * sizeof(relayout_plan));
}

/* The new layout of archetype `a` with the component infos as they are now (the caller has set
 * the re-laid-out component's size and alignment). 0 rows when no row fits. */
static u32 plan_layout(const de_world *w, ecs_archetype *a, ecs_column *cols, u32 *used) {
    ecs_archetype tmp = *a;
    tmp.columns = cols;
    u64 row_bytes = sizeof(de_entity);
    for (u32 c = 0; c < a->column_count; ++c)
        row_bytes += (u64)cols[c].size + 2u * sizeof(u32);
    u64 rows = w->chunk_bytes / row_bytes;
    while (rows > 0 && ecs_archetype_chunk_layout(w, &tmp, (u32)rows, false) > w->chunk_bytes)
        --rows;
    if (rows == 0)
        return 0;
    *used = (u32)ecs_archetype_chunk_layout(w, &tmp, (u32)rows, true);
    return (u32)rows;
}

/* Moves every row of plan `p`'s archetype into the new chunks, then installs them. */
static void plan_commit(de_world *w, relayout_plan *p, const de_type_desc *od,
                        const de_type_desc *nd, u32 *refused) {
    ecs_archetype *a = p->a;
    const u32 old_rows = a->rows_per_chunk;
    for (u32 i = 0; i < a->count; ++i) {
        const ecs_chunk *oc = ecs_table_chunk(a, i / old_rows);
        const u32 orow = i % old_rows;
        const u32 nci = i / p->rows;
        const ecs_chunk *nc = &DE_ARRAY_AT(&p->chunks, ecs_chunk, nci);
        const u32 nrow = i % p->rows;

        const de_entity e = ecs_chunk_entities(oc)[orow];
        ecs_chunk_entities(nc)[nrow] = e;
        ecs_record *rec = (ecs_record *)de_array_at(&w->records, de_entity_index(e));
        rec->chunk = nci;
        rec->row = nrow;

        for (u32 c = 0; c < a->column_count; ++c) {
            const ecs_column *ocol = &a->columns[c];
            const ecs_column *ncol = &p->columns[c];
            const u8 *src = oc->data + ocol->data_offset + (size_t)orow * ocol->size;
            u8 *dst = nc->data + ncol->data_offset + (size_t)nrow * ncol->size;
            if (c == p->column)
                convert_object(od, nd, src, dst, refused);
            else
                memcpy(dst, src, ocol->size);
            ((u32 *)(void *)(nc->data + ncol->added_offset))[nrow] =
                ((const u32 *)(const void *)(oc->data + ocol->added_offset))[orow];
            ((u32 *)(void *)(nc->data + ncol->changed_offset))[nrow] =
                ((const u32 *)(const void *)(oc->data + ocol->changed_offset))[orow];
        }
    }
    for (u32 k = 0; k < a->chunks.count; ++k)
        de_pool_free(w->chunk_pool, ecs_table_chunk(a, k)->data);
    de_array_free(&a->chunks);
    a->chunks = p->chunks; /* the plan's array moves into the archetype */
    de_array_init(&p->chunks, w->a, sizeof(ecs_chunk), DE_ALIGNOF(ecs_chunk), DE_MEM_ECS);
    memcpy(a->columns, p->columns, (size_t)a->column_count * sizeof(ecs_column));
    a->rows_per_chunk = p->rows;
    a->chunk_used = p->used;
}

/* The DENG-349 entity-offset cache of component `c` was built from the old layout. */
static void drop_command_refs(de_world *w, u32 c) {
    if (w->cmd != NULL && w->cmd->component_refs.count >= c)
        DE_ARRAY_AT(&w->cmd->component_refs, ecs_cmd_type_refs, c - 1u).built = false;
}

/* Validates and plans (and with `apply`, performs) the re-layout of component `c`. */
static de_result relayout(de_world *w, u32 c, de_type_id new_type, bool apply, u32 *refused) {
    ecs_component_info *info = (ecs_component_info *)de_array_at(&w->components, c - 1u);
    const de_type_desc *nd = de_type_get(w->types, new_type);
    if (nd == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "ecs: re-layout of component '%s': type id 0x%x is not a struct type",
                       info->name, new_type.id);
    const bool retype = new_type.id != info->type.id;
    const bool rename = strcmp(nd->name, info->name) != 0;
    if (retype) {
        const u64 th = de_hash64(&new_type.id, sizeof new_type.id, 0);
        const u64 nh = de_hash64_str(nd->name);
        if (DE_HASHMAP_GET(&w->component_by_type, u32, th, &new_type.id) != NULL ||
            (rename && DE_HASHMAP_GET(&w->component_by_name, u32, nh, &nh) != NULL))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "ecs: re-layout of component '%s': type '%s' is a component already",
                           info->name, nd->name);
    }
    const u64 new_hash = de_type_schema_hash(w->types, new_type);
    const bool tag = (info->flags & DE_COMP_TAG) != 0;
    if (!tag)
        DE_TRY(check_desc(w, nd, true, "ecs: component"));

    /* Plan every archetype with a column of `c`, with the new size and alignment in place. */
    const ecs_component_info saved = *info;
    relayout_plan *plans = NULL;
    u32 plan_count = 0;
    de_result r = DE_OK;
    if (!tag) {
        info->size = nd->size;
        info->align = nd->align ? nd->align : 1u;
        for (u32 i = 0; i < ecs_archetype_count(w); ++i) {
            if (ecs_archetype_column(ecs_archetype_at(w, i), c) != ECS_INVALID_INDEX)
                ++plan_count;
        }
        if (plan_count > 0) {
            plans = (relayout_plan *)DE_ALLOC(w->a, (size_t)plan_count * sizeof(relayout_plan),
                                              DE_ALIGNOF(relayout_plan), DE_MEM_ECS);
            if (plans == NULL) {
                *info = saved;
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: re-layout of '%s': out of memory",
                               saved.name);
            }
            memset(plans, 0, (size_t)plan_count * sizeof(relayout_plan));
            for (u32 k = 0; k < plan_count; ++k)
                de_array_init(&plans[k].chunks, w->a, sizeof(ecs_chunk), DE_ALIGNOF(ecs_chunk),
                              DE_MEM_ECS);
        }
        u32 k = 0;
        for (u32 i = 0; i < ecs_archetype_count(w) && r == DE_OK; ++i) {
            ecs_archetype *a = ecs_archetype_at(w, i);
            const u32 col = ecs_archetype_column(a, c);
            if (col == ECS_INVALID_INDEX)
                continue;
            relayout_plan *p = &plans[k++];
            p->a = a;
            p->column = col;
            p->columns = (ecs_column *)DE_ALLOC(w->a, (size_t)a->column_count * sizeof(ecs_column),
                                                DE_ALIGNOF(ecs_column), DE_MEM_ECS);
            if (p->columns == NULL) {
                r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: re-layout of '%s': out of memory",
                            saved.name);
                break;
            }
            memcpy(p->columns, a->columns, (size_t)a->column_count * sizeof(ecs_column));
            p->columns[col].size = nd->size;
            p->rows = plan_layout(w, a, p->columns, &p->used);
            if (p->rows == 0) {
                r = DE_FAIL(DE_ERR_UNSUPPORTED,
                            "ecs: re-layout of '%s': one row of archetype %u (%u components) "
                            "does not fit a %u-byte chunk",
                            saved.name, a->index, a->component_count, w->chunk_bytes);
                break;
            }
            if (!apply || a->count == 0)
                continue;
            const u32 chunk_count = (a->count + p->rows - 1u) / p->rows;
            if (de_array_reserve(&p->chunks, chunk_count) != DE_OK) {
                r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: re-layout of '%s': out of memory",
                            saved.name);
                break;
            }
            for (u32 n = 0; n < chunk_count; ++n) {
                ecs_chunk ch;
                memset(&ch, 0, sizeof ch);
                ch.data = (u8 *)de_pool_alloc(w->chunk_pool);
                if (ch.data == NULL) {
                    r = DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                                "ecs: re-layout of '%s': out of memory for a table chunk",
                                saved.name);
                    break;
                }
                const u32 left = a->count - n * p->rows;
                ch.count = left < p->rows ? left : p->rows;
                (void)de_array_push(&p->chunks, &ch); /* reserved above */
            }
        }
    }
    if (r == DE_OK && apply && retype) {
        /* New lookup keys first, so a failure here still changes nothing. */
        const u64 th = de_hash64(&new_type.id, sizeof new_type.id, 0);
        const u64 nh = de_hash64_str(nd->name);
        if (de_hashmap_put(&w->component_by_type, th, &new_type.id, &c) != DE_OK) {
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: re-layout of '%s': out of memory", saved.name);
        } else if (rename && de_hashmap_put(&w->component_by_name, nh, &nh, &c) != DE_OK) {
            de_hashmap_remove(&w->component_by_type, th, &new_type.id, NULL);
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: re-layout of '%s': out of memory", saved.name);
        }
    }
    if (r != DE_OK || !apply) {
        plans_free(w, plans, plan_count, true);
        *info = saved;
        return r;
    }

    u32 refused_here = 0;
    for (u32 k = 0; k < plan_count; ++k)
        plan_commit(w, &plans[k], saved.layout, nd, &refused_here);
    plans_free(w, plans, plan_count, false);

    if (retype) {
        const u64 th = de_hash64(&saved.type.id, sizeof saved.type.id, 0);
        de_hashmap_remove(&w->component_by_type, th, &saved.type.id, NULL);
        if (rename) {
            const u64 nh = de_hash64_str(saved.name);
            de_hashmap_remove(&w->component_by_name, nh, &nh, NULL);
        }
    }
    info->type = new_type;
    info->name = nd->name;
    info->layout = nd;
    info->layout_hash = new_hash;
    info->is_pod = nd->is_pod;
    if (!tag)
        info->hooks = nd->hooks; /* size and align were set for the plan */
    ecs_checksum_destroy(w);     /* rebuilt on the next de_world_checksum */
    drop_command_refs(w, c);
    if (refused != NULL)
        *refused += refused_here;
    return DE_OK;
}

de_result de__world_relayout_component(de_world *w, de_component c, de_type_id new_type,
                                       u32 *migrate_refused) {
    if (migrate_refused != NULL)
        *migrate_refused = 0;
    if (w == NULL || ecs_component_get(w, c.id) == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__world_relayout_component: no world, or component %u is not registered",
                       c.id);
    ECS_ASSERT_STRUCTURAL(w);
    return relayout(w, c.id, new_type, true, migrate_refused);
}

/* ---- Type sync ------------------------------------------------------------------------------ */

/* Re-lays out the value of resource `res` (NULL data: only the description changes). */
static de_result resource_relayout(de_world *w, ecs_resource *res, const de_type_desc *nd,
                                   u32 *refused) {
    const u32 align = nd->align ? nd->align : 1u;
    if (res->data != NULL) {
        void *data = DE_ALLOC(w->a, nd->size, align, DE_MEM_ECS);
        if (data == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: re-layout of resource '%s': out of memory",
                           nd->name);
        convert_object(res->layout, nd, res->data, data, refused);
        DE_FREE(w->a, res->data, res->size);
        res->data = data;
    }
    res->size = nd->size;
    res->align = align;
    return DE_OK;
}

/* Re-lays out both payload buffers of event queue `q`. */
static de_result event_relayout(de_world *w, ecs_event_queue *q, const de_type_desc *nd,
                                u32 *refused) {
    const u32 align = nd->align ? nd->align : 1u;
    de_array next[2];
    for (u32 b = 0; b < 2; ++b) {
        de_array_init(&next[b], w->a, nd->size, align, DE_MEM_ECS);
        if (de_array_resize(&next[b], q->buf[b].count) != DE_OK) {
            de_array_free(&next[0]);
            de_array_free(&next[1]);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: re-layout of event '%s': out of memory",
                           nd->name);
        }
    }
    for (u32 b = 0; b < 2; ++b) {
        for (u32 i = 0; i < q->buf[b].count; ++i)
            convert_object(q->layout, nd, de_array_at(&q->buf[b], i), de_array_at(&next[b], i),
                           refused);
        de_array_free(&q->buf[b]);
        q->buf[b] = next[b];
    }
    q->size = nd->size;
    q->align = align;
    return DE_OK;
}

de_result de__world_sync_types(de_world *w, de__world_sync_stats *out) {
    de__world_sync_stats st;
    memset(&st, 0, sizeof st);
    if (out != NULL)
        *out = st;
    if (w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__world_sync_types: world is NULL");
    ECS_ASSERT_STRUCTURAL(w);

    /* 1. Check every changed description; nothing changes before all pass. */
    for (u32 c = 1; c <= w->components.count; ++c) {
        const ecs_component_info *info = ecs_component_get(w, c);
        const de_type_desc *d = de_type_get(w->types, info->type);
        if (d == NULL || d == info->layout ||
            de_type_schema_hash(w->types, info->type) == info->layout_hash)
            continue;
        DE_TRY(relayout(w, c, info->type, false, NULL));
    }
    for (u32 i = 0; i < w->resources.count; ++i) {
        const ecs_resource *res = (const ecs_resource *)de_array_at(&w->resources, i);
        const de_type_desc *d = de_type_get(w->types, res->type);
        if (d != NULL && d != res->layout &&
            de_type_schema_hash(w->types, res->type) != res->layout_hash)
            DE_TRY(check_desc(w, d, false, "ecs: resource"));
    }
    for (u32 i = 0; i < w->events.count; ++i) {
        const ecs_event_queue *q = (const ecs_event_queue *)de_array_at(&w->events, i);
        const de_type_desc *d = de_type_get(w->types, q->payload);
        if (d != NULL && d != q->layout &&
            de_type_schema_hash(w->types, q->payload) != q->layout_hash)
            DE_TRY(check_desc(w, d, false, "ecs: event"));
    }

    /* 2. Apply: hooks only for the same layout, a re-layout for another one. */
    de_result r = DE_OK;
    for (u32 c = 1; c <= w->components.count && r == DE_OK; ++c) {
        ecs_component_info *info = (ecs_component_info *)de_array_at(&w->components, c - 1u);
        const de_type_desc *d = de_type_get(w->types, info->type);
        if (d == NULL || d == info->layout)
            continue;
        if (de_type_schema_hash(w->types, info->type) == info->layout_hash) {
            info->layout = d;
            info->name = d->name;
            info->is_pod = d->is_pod;
            if ((info->flags & DE_COMP_TAG) == 0)
                info->hooks = d->hooks;
            ++st.refreshed;
            continue;
        }
        r = relayout(w, c, info->type, true, &st.migrate_refused);
        if (r == DE_OK)
            ++st.relaid_out;
    }
    for (u32 i = 0; i < w->resources.count && r == DE_OK; ++i) {
        ecs_resource *res = (ecs_resource *)de_array_at(&w->resources, i);
        const de_type_desc *d = de_type_get(w->types, res->type);
        if (d == NULL || d == res->layout)
            continue;
        const u64 h = de_type_schema_hash(w->types, res->type);
        if (h != res->layout_hash) {
            r = resource_relayout(w, res, d, &st.migrate_refused);
            if (r != DE_OK)
                break;
            ++st.resources;
        } else {
            ++st.refreshed;
        }
        res->layout = d;
        res->layout_hash = h;
        res->is_pod = d->is_pod;
        res->hooks = d->hooks;
    }
    for (u32 i = 0; i < w->events.count && r == DE_OK; ++i) {
        ecs_event_queue *q = (ecs_event_queue *)de_array_at(&w->events, i);
        const de_type_desc *d = de_type_get(w->types, q->payload);
        if (d == NULL || d == q->layout)
            continue;
        const u64 h = de_type_schema_hash(w->types, q->payload);
        if (h != q->layout_hash) {
            r = event_relayout(w, q, d, &st.migrate_refused);
            if (r != DE_OK)
                break;
            ++st.events;
            if (w->cmd != NULL && w->cmd->event_refs.count > i)
                DE_ARRAY_AT(&w->cmd->event_refs, ecs_cmd_type_refs, i).built = false;
        } else {
            ++st.refreshed;
        }
        q->layout = d;
        q->layout_hash = h;
        q->is_pod = d->is_pod;
        q->hooks = d->hooks;
    }
    if (out != NULL)
        *out = st;
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de__world_sync_types: the world is partly synced");
    return DE_OK;
}
