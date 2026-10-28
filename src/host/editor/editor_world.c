/* editor_world.c: world queries, subscriptions and world.delta (DENG-625).
 *
 * Reads the ECS storage directly (ecs_internal.h): archetypes, chunks, rows and their change
 * ticks. An entity's id on the wire is de_entity.bits; its revision is the newest added/changed
 * tick across its columns. */
#include "host/editor/editor_internal.h"

#include <dengine/de_memory.h>

#include "ecs/change_internal.h"
#include "ecs/ecs_internal.h"
#include "ecs/system_internal.h"
#include "host/host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_MAX_SUBS 64u

typedef struct ed_world_sub {
    u32 id;
    u32 topic; /* 0 free, 1 world.delta */
    u32 every; /* frames between deltas */
    u32 since; /* change tick: rows at or after it changed since the last delta */
    bool resync;
    /* A new subscription: its first pass only learns what exists (the IDE has just queried it),
     * so it sends nothing; a resync there would make the IDE reload and subscribe again. */
    bool prime;
    char with[16][64];
    u32 with_count;
    u64 *known; /* sorted entity bits */
    u32 known_count, known_cap;
} ed_world_sub;

typedef struct de__ed_world {
    ed_world_sub subs[ED_MAX_SUBS];
    u32 next_id;
} de__ed_world;

static de__ed_world g_world;

bool de__ed_logs_unsubscribe(u32 id);

/* ---------- Storage access ---------- */

static de_world *W(de__editor *ed) {
    return ed->e->world;
}

static ecs_archetype *arch_at(const de_world *w, u32 i) {
    return ((ecs_archetype *const *)w->archetypes.data)[i];
}

static bool tick_newer(u32 tick, u32 since) {
    return (u32)(tick - since) < 0x80000000u;
}

de_component de__ed_component(de__editor *ed, de_ep_str name) {
    const de_world *w = W(ed);
    for (u32 i = 0; i < w->components.count; ++i) {
        const ecs_component_info *ci = (const ecs_component_info *)de_array_at(&w->components, i);
        if (ci->name != NULL && strlen(ci->name) == name.len &&
            memcmp(ci->name, name.ptr, name.len) == 0)
            return (de_component){i + 1u};
    }
    return (de_component){0};
}

bool de__ed_component_type(de__editor *ed, de_component c, de_type_id *type, u32 *size) {
    const ecs_component_info *ci = ecs_component_get(W(ed), c.id);
    if (ci == NULL)
        return false;
    if (type)
        *type = ci->type;
    if (size)
        *size = ci->size;
    return true;
}

static bool arch_has(const ecs_archetype *a, u32 comp) {
    for (u32 i = 0; i < a->component_count; ++i)
        if (a->components[i] == comp)
            return true;
    return false;
}

static u32 row_rev(const ecs_archetype *a, const ecs_chunk *ch, u32 row) {
    u32 rev = 0;
    for (u32 c = 0; c < a->column_count; ++c) {
        const u32 added = ecs_chunk_added(a, ch, c)[row], changed = ecs_chunk_changed(a, ch, c)[row];
        if (added > rev)
            rev = added;
        if (changed > rev)
            rev = changed;
    }
    return rev;
}

u64 de__ed_entity_rev(de__editor *ed, de_entity e) {
    const de_world *w = W(ed);
    const ecs_record *r = ecs_entity_record(w, e);
    if (r == NULL)
        return 0;
    const ecs_archetype *a = arch_at(w, r->archetype);
    return row_rev(a, ecs_table_chunk(a, r->chunk), r->row) + 1u;
}

/* The column of component `comp` in archetype `a`, or ECS_INVALID_INDEX (absent, or a tag). */
static u32 column_of(const ecs_archetype *a, u32 comp) {
    for (u32 i = 0; i < a->component_count; ++i)
        if (a->components[i] == comp)
            return a->column_of[i];
    return ECS_INVALID_INDEX;
}

static bool name_in(const de_ep_list_str *names, const char *name) {
    const size_t n = strlen(name);
    for (u32 i = 0; i < names->count; ++i)
        if (names->items[i].len == n && memcmp(names->items[i].ptr, name, n) == 0)
            return true;
    return false;
}

/* One component's value (raw) in the arena. */
static de_ep_raw component_raw(de__editor *ed, de_ep_arena *a, const ecs_component_info *ci,
                               const void *data) {
    mpack_writer_t w;
    de__ed_raw_begin(a, &w);
    if (data == NULL || ci->size == 0) {
        mpack_build_map(&w);
        mpack_complete_map(&w);
    } else {
        de__ed_write_object(ed, &w, ci->type, data);
    }
    return de__ed_raw_end(a, &w);
}

/* A "Component.path" field of the entity as a raw value; false when it doesn't resolve. */
static bool field_raw(de__editor *ed, de_ep_arena *a, const ecs_archetype *arch,
                      const ecs_chunk *ch, u32 row, de_ep_str path, de_ep_raw *out) {
    char buf[256];
    if (path.len >= sizeof buf)
        return false;
    memcpy(buf, path.ptr, path.len);
    buf[path.len] = '\0';
    char *dot = strchr(buf, '.');
    const de_ep_str comp_name = {buf, dot != NULL ? (u32)(dot - buf) : path.len};
    const de_component c = de__ed_component(ed, comp_name);
    const ecs_component_info *ci = ecs_component_get(W(ed), c.id);
    if (ci == NULL)
        return false;
    const u32 col = column_of(arch, c.id);
    if (col == ECS_INVALID_INDEX)
        return false;
    const u8 *data = (const u8 *)ecs_chunk_column(arch, ch, col) + (size_t)row * ci->size;
    if (dot == NULL) {
        *out = component_raw(ed, a, ci, data);
        return true;
    }
    const de_field_info *f = NULL;
    u32 off = 0;
    if (de_reflect_resolve_path(ed->e->types, ci->type, dot + 1, &f, &off) != DE_OK) {
        de_error_clear();
        return false;
    }
    mpack_writer_t w;
    de__ed_raw_begin(a, &w);
    de__ed_write_field(ed, &w, (de_type_id){0}, 0, f, data + off);
    *out = de__ed_raw_end(a, &w);
    return true;
}

static bool row_of(de__editor *ed, de_ep_arena *a, const ecs_archetype *arch, const ecs_chunk *ch,
                   u32 row, const de_ep_list_str *components, const de_ep_list_str *fields,
                   de_ep_entity_row *out) {
    const de_world *w = W(ed);
    memset(out, 0, sizeof *out);
    const de_entity e = ecs_chunk_entities(ch)[row];
    out->entity = e.bits;
    out->has_scene = true;
    out->scene = de__ed_scene_of(ed, e);
    out->has_rev = true;
    out->rev = (u64)row_rev(arch, ch, row) + 1u;
    const u32 cap = fields != NULL ? fields->count : arch->component_count;
    de_ep_kv_raw *kv = (de_ep_kv_raw *)de__ed_alloc(a, sizeof(de_ep_kv_raw) * (cap + 1));
    if (kv == NULL)
        return false;
    u32 n = 0;
    if (fields != NULL) {
        for (u32 i = 0; i < fields->count; ++i)
            if (field_raw(ed, a, arch, ch, row, fields->items[i], &kv[n].value))
                kv[n++].key = fields->items[i];
    } else {
        for (u32 i = 0; i < arch->component_count; ++i) {
            const ecs_component_info *ci = ecs_component_get(w, arch->components[i]);
            if (ci == NULL || ci->name == NULL)
                continue;
            if (components != NULL && components->count > 0 && !name_in(components, ci->name))
                continue;
            const u32 col = arch->column_of[i];
            const void *data = col == ECS_INVALID_INDEX
                                   ? NULL
                                   : (const u8 *)ecs_chunk_column(arch, ch, col) + (size_t)row * ci->size;
            kv[n].key = de_ep_cstr(ci->name);
            kv[n++].value = component_raw(ed, a, ci, data);
        }
    }
    out->components.items = kv;
    out->components.count = n;
    return true;
}

bool de__ed_entity_row(de__editor *ed, de_ep_arena *a, de_entity e,
                       const de_ep_list_str *components, const de_ep_list_str *fields,
                       de_ep_entity_row *out) {
    const de_world *w = W(ed);
    const ecs_record *r = ecs_entity_record(w, e);
    if (r == NULL)
        return false;
    const ecs_archetype *arch = arch_at(w, r->archetype);
    return row_of(ed, a, arch, ecs_table_chunk(arch, r->chunk), r->row, components, fields, out);
}

void de__ed_world_touch(de__editor *ed) {
    (void)ecs_change_tick_advance(W(ed));
}

void de__ed_world_resync(de__editor *ed) {
    (void)ed;
    for (u32 i = 0; i < ED_MAX_SUBS; ++i)
        g_world.subs[i].resync = true;
}

/* ---------- world.query ---------- */

/* Resolves component names to ids; false when one is unknown. */
static bool resolve_names(de__editor *ed, const de_ep_list_str *names, u32 *ids) {
    for (u32 i = 0; i < names->count; ++i) {
        ids[i] = de__ed_component(ed, names->items[i]).id;
        if (ids[i] == 0)
            return false;
    }
    return true;
}

static void on_query(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(world_query, de_ep_query_params, q);
    if (q.limit == 0 || q.limit > DE_EP_LIMIT_QUERY_PAGE_SIZE) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "limit must be 1..%u",
                     DE_EP_LIMIT_QUERY_PAGE_SIZE);
        return;
    }
    u32 with[64], without[64];
    if (q.with.count > 64 || (q.has_without && q.without.count > 64)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "too many components");
        return;
    }
    if (!resolve_names(ed, &q.with, with)) {
        /* An unknown component matches nothing. */
        de_ep_query_result empty;
        memset(&empty, 0, sizeof empty);
        empty.frame = de__ed_frame(ed);
        de_ep_write_response_world_query(w, req->id, &empty);
        return;
    }
    u32 without_count = 0;
    if (q.has_without)
        for (u32 i = 0; i < q.without.count; ++i) {
            const u32 id = de__ed_component(ed, q.without.items[i]).id;
            if (id != 0)
                without[without_count++] = id;
        }
    u64 after = 0;
    if (q.has_cursor && q.cursor.len > 0) {
        char buf[32];
        const u32 n = q.cursor.len < 31 ? q.cursor.len : 31;
        memcpy(buf, q.cursor.ptr, n);
        buf[n] = '\0';
        after = strtoull(buf, NULL, 10);
    }
    const de_world *wd = W(ed);
    de_ep_entity_row *rows = (de_ep_entity_row *)de__ed_alloc(a, sizeof(de_ep_entity_row) * (q.limit + 1));
    if (rows == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "world.query: page too large for scratch memory");
        return;
    }
    /* Pages go in ascending entity-bits order; the cursor is the last entity sent. Rows are
     * collected per archetype, so pick the smallest entities above the cursor in a few passes:
     * gather candidates, then keep the lowest `limit`. */
    u32 n = 0;
    bool more = false;
    u64 floor = after;
    for (;;) {
        /* The smallest matching entity above `floor`. */
        u64 best = 0;
        const ecs_archetype *ba = NULL;
        const ecs_chunk *bc = NULL;
        u32 brow = 0;
        for (u32 ai = 0; ai < wd->archetypes.count; ++ai) {
            const ecs_archetype *arch = arch_at(wd, ai);
            if (arch->count == 0)
                continue;
            bool ok = true;
            for (u32 i = 0; i < q.with.count && ok; ++i)
                ok = arch_has(arch, with[i]);
            for (u32 i = 0; i < without_count && ok; ++i)
                ok = !arch_has(arch, without[i]);
            if (!ok)
                continue;
            for (u32 ci = 0; ci < arch->chunks.count; ++ci) {
                const ecs_chunk *ch = ecs_table_chunk(arch, ci);
                const de_entity *ents = ecs_chunk_entities(ch);
                for (u32 r = 0; r < ch->count; ++r) {
                    const u64 bits = ents[r].bits;
                    if (bits <= floor || (best != 0 && bits >= best) || de__ed_views_owns(bits))
                        continue;
                    if (q.has_changed_since && (u64)row_rev(arch, ch, r) + 1u <= q.changed_since)
                        continue;
                    best = bits;
                    ba = arch;
                    bc = ch;
                    brow = r;
                }
            }
        }
        if (ba == NULL)
            break;
        if (n == q.limit) {
            more = true;
            break;
        }
        if (!row_of(ed, a, ba, bc, brow, NULL, q.has_fields ? &q.fields : NULL, &rows[n]))
            break;
        ++n;
        floor = best;
    }
    de_ep_query_result res;
    memset(&res, 0, sizeof res);
    res.rows.items = rows;
    res.rows.count = n;
    res.frame = de__ed_frame(ed);
    char cursor[32];
    if (more && n > 0) {
        snprintf(cursor, sizeof cursor, "%llu", (unsigned long long)rows[n - 1].entity);
        res.has_cursor = true;
        res.cursor = de_ep_cstr(cursor);
    }
    de_ep_write_response_world_query(w, req->id, &res);
}

static void on_entity(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(world_entity, de_ep_entity_params, q);
    de_ep_entity_result res;
    memset(&res, 0, sizeof res);
    res.entities.items = (de_ep_entity_row *)de__ed_alloc(a, sizeof(de_ep_entity_row) * (q.ids.count + 1));
    if (res.entities.items == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "world.entity: too many ids");
        return;
    }
    for (u32 i = 0; i < q.ids.count; ++i) {
        const de_entity e = {q.ids.items[i]};
        if (!de__ed_entity_row(ed, a, e, q.has_components ? &q.components : NULL, NULL,
                               &res.entities.items[res.entities.count])) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "entity %llu",
                         (unsigned long long)e.bits);
            return;
        }
        ++res.entities.count;
    }
    de_ep_write_response_world_entity(w, req->id, &res);
}

static void on_archetypes(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    const de_world *wd = W(ed);
    de_ep_archetype_list list;
    memset(&list, 0, sizeof list);
    list.archetypes.items = (de_ep_archetype *)de__ed_alloc(a, sizeof(de_ep_archetype) * (wd->archetypes.count + 1));
    for (u32 ai = 0; ai < wd->archetypes.count && list.archetypes.items != NULL; ++ai) {
        const ecs_archetype *arch = arch_at(wd, ai);
        if (arch->count == 0)
            continue;
        de_ep_archetype *o = &list.archetypes.items[list.archetypes.count++];
        memset(o, 0, sizeof *o);
        o->components.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (arch->component_count + 1));
        for (u32 i = 0; i < arch->component_count && o->components.items != NULL; ++i) {
            const ecs_component_info *ci = ecs_component_get(wd, arch->components[i]);
            if (ci != NULL && ci->name != NULL)
                o->components.items[o->components.count++] = de_ep_cstr(ci->name);
        }
        o->count = arch->count;
        o->has_chunks = true;
        o->chunks = arch->chunks.count;
        o->has_capacity = true;
        o->capacity = arch->chunks.count * arch->rows_per_chunk;
    }
    de_ep_write_response_world_archetypes(w, req->id, &list);
}

/* ---------- Systems ---------- */

static const char *const k_stage[DE_SCHEDULE_COUNT_] = {
    "Startup", "First", "PreUpdate", "FixedUpdate", "Update", "PostUpdate", "Extract", "Last"};

static de_ep_list_str access_names(de__editor *ed, de_ep_arena *a, const ecs_access_item *items,
                                   u32 count, int kind, int want_write) {
    const de_world *wd = W(ed);
    const de_type_registry *r = ed->e->types;
    de_ep_list_str l = {0};
    l.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (count + 1));
    for (u32 i = 0; i < count && l.items != NULL; ++i) {
        if (want_write >= 0 && (int)items[i].write != want_write)
            continue;
        const char *name = NULL;
        if (kind == 0) {
            const ecs_component_info *ci = ecs_component_get(wd, items[i].id);
            name = ci ? ci->name : NULL;
        } else if (kind == 1 && items[i].id >= 1 && items[i].id <= wd->resources.count) {
            const ecs_resource *res = (const ecs_resource *)de_array_at(&wd->resources, items[i].id - 1u);
            const de_type_desc *d = de_type_get(r, res->type);
            name = d ? d->name : NULL;
        } else if (kind == 2 && items[i].id >= 1 && items[i].id <= wd->events.count) {
            const ecs_event_queue *eq = (const ecs_event_queue *)de_array_at(&wd->events, items[i].id - 1u);
            const de_type_desc *d = de_type_get(r, eq->payload);
            name = d ? d->name : NULL;
        }
        if (name != NULL)
            l.items[l.count++] = de_ep_cstr(name);
    }
    return l;
}

static void on_systems(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    de_world *wd = W(ed);
    const ecs_system_registry *reg = wd->systems;
    de_ep_system_list list;
    memset(&list, 0, sizeof list);
    if (reg == NULL) {
        de_ep_write_response_world_systems(w, req->id, &list);
        return;
    }
    list.systems.items = (de_ep_system_info *)de__ed_alloc(a, sizeof(de_ep_system_info) * (reg->slots.count + 1));
    for (u32 s = 0; s < DE_SCHEDULE_COUNT_ && list.systems.items != NULL; ++s) {
        const ecs_schedule_state *st = &reg->schedules[s];
        ecs_system *const *sys = (ecs_system *const *)st->systems.data;
        for (u32 i = 0; i < st->systems.count; ++i) {
            const ecs_system *x = sys[i];
            de_ep_system_info *o = &list.systems.items[list.systems.count++];
            memset(o, 0, sizeof *o);
            o->name = de_ep_cstr(x->name != NULL ? x->name : "(unnamed)");
            o->stage = de_ep_cstr(k_stage[s]);
            o->reads = access_names(ed, a, x->access.components, x->access.component_count, 0, 0);
            o->writes = access_names(ed, a, x->access.components, x->access.component_count, 0, 1);
            de_task_node_stats ts;
            memset(&ts, 0, sizeof ts);
            if (de_system_get_stats(wd, x->handle, &ts) == DE_OK && ts.runs > 0)
                o->ms = (double)ts.last_ns * 1e-6;
            else
                o->ms = (double)x->serial_last_ns * 1e-6;
            de_error_clear();
            o->has_enabled = true;
            o->enabled = x->enabled;
            o->has_flags = true;
            o->flags.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * 3);
            if (o->flags.items != NULL) {
                if (x->flags & DE_SYSTEM_MAIN_THREAD)
                    o->flags.items[o->flags.count++] = de_ep_cstr("main_thread");
                if (x->flags & DE_SYSTEM_DETERMINISTIC)
                    o->flags.items[o->flags.count++] = de_ep_cstr("deterministic");
                if (x->flags & DE_SYSTEM_EXCLUSIVE)
                    o->flags.items[o->flags.count++] = de_ep_cstr("exclusive");
            }
            /* The systems before it in the schedule's last-built DAG. */
            o->has_after = true;
            const ecs_sched_edge *edges = (const ecs_sched_edge *)st->edges.data;
            o->after.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (st->edges.count + x->after_count + 1));
            for (u32 k = 0; k < st->edges.count && o->after.items != NULL; ++k)
                if (edges[k].to == i && edges[k].from < st->systems.count && sys[edges[k].from]->name)
                    o->after.items[o->after.count++] = de_ep_cstr(sys[edges[k].from]->name);
            o->has_resource_reads = o->has_resource_writes = true;
            o->resource_reads = access_names(ed, a, x->access.resources, x->access.resource_count, 1, 0);
            o->resource_writes = access_names(ed, a, x->access.resources, x->access.resource_count, 1, 1);
            o->has_event_reads = o->has_event_writes = true;
            o->event_reads = access_names(ed, a, x->access.events, x->access.event_count, 2, 0);
            o->event_writes = access_names(ed, a, x->access.events, x->access.event_count, 2, 1);
        }
    }
    de_ep_write_response_world_systems(w, req->id, &list);
}

static void on_set_system_enabled(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                                  mpack_writer_t *w) {
    DE__ED_PARAMS(world_set_system_enabled, de_ep_system_enable_params, q);
    de_world *wd = W(ed);
    const ecs_system_registry *reg = wd->systems;
    for (u32 i = 0; reg != NULL && i < reg->slots.count; ++i) {
        const ecs_system_slot *slot = (const ecs_system_slot *)de_array_at(&reg->slots, i);
        const ecs_system *x = slot->sys;
        if (x == NULL || x->name == NULL || strlen(x->name) != q.name.len ||
            memcmp(x->name, q.name.ptr, q.name.len) != 0)
            continue;
        de_system_set_enabled(wd, x->handle, q.enabled);
        de_ep_empty e;
        memset(&e, 0, sizeof e);
        de_ep_write_response_world_set_system_enabled(w, req->id, &e);
        return;
    }
    de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no system %.*s", (int)q.name.len, q.name.ptr);
}

/* ---------- Resources, events, checksum ---------- */

static void on_resources(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    const de_world *wd = W(ed);
    de_ep_resource_list list;
    memset(&list, 0, sizeof list);
    list.resources.items = (de_ep_kv_raw *)de__ed_alloc(a, sizeof(de_ep_kv_raw) * (wd->resources.count + 1));
    for (u32 i = 0; i < wd->resources.count && list.resources.items != NULL; ++i) {
        const ecs_resource *res = (const ecs_resource *)de_array_at(&wd->resources, i);
        const de_type_desc *d = de_type_get(ed->e->types, res->type);
        if (d == NULL || res->data == NULL)
            continue;
        mpack_writer_t mw;
        de__ed_raw_begin(a, &mw);
        de__ed_write_object(ed, &mw, res->type, res->data);
        de_ep_kv_raw *kv = &list.resources.items[list.resources.count++];
        kv->key = de_ep_cstr(d->name);
        kv->value = de__ed_raw_end(a, &mw);
    }
    de_ep_write_response_world_resources(w, req->id, &list);
}

static void on_events(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(world_events, de_ep_events_params, q);
    const de_world *wd = W(ed);
    de_ep_event_list list;
    memset(&list, 0, sizeof list);
    u32 cap = 0;
    for (u32 i = 0; i < wd->events.count; ++i) {
        const ecs_event_queue *eq = (const ecs_event_queue *)de_array_at(&wd->events, i);
        cap += eq->buf[0].count + eq->buf[1].count;
    }
    list.events.items = (de_ep_event_record *)de__ed_alloc(a, sizeof(de_ep_event_record) * (cap + 1));
    const u64 frame = de__ed_frame(ed);
    for (u32 i = 0; i < wd->events.count && list.events.items != NULL; ++i) {
        const ecs_event_queue *eq = (const ecs_event_queue *)de_array_at(&wd->events, i);
        const de_type_desc *d = de_type_get(ed->e->types, eq->payload);
        if (d == NULL || (q.types.count > 0 && !name_in(&q.types, d->name)))
            continue;
        /* The older buffer is last frame's, the other this frame's. */
        for (u32 b = 0; b < 2; ++b) {
            const u32 bi = b == 0 ? eq->older : 1u - eq->older;
            const u64 at = b == 0 && frame > 0 ? frame - 1 : frame;
            if (q.frames > 0 && frame - at >= q.frames)
                continue;
            const de_array *buf = &eq->buf[bi];
            for (u32 k = 0; k < buf->count; ++k) {
                mpack_writer_t mw;
                de__ed_raw_begin(a, &mw);
                de__ed_write_object(ed, &mw, eq->payload, (const u8 *)buf->data + (size_t)k * eq->size);
                de_ep_event_record *o = &list.events.items[list.events.count++];
                o->frame = at;
                o->type = de_ep_cstr(d->name);
                o->value = de__ed_raw_end(a, &mw);
            }
        }
    }
    de_ep_write_response_world_events(w, req->id, &list);
}

static void on_checksum(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                        mpack_writer_t *w) {
    (void)a;
    de_ep_checksum c;
    c.frame = de__ed_frame(ed);
    c.checksum = de_world_checksum(W(ed), DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE | DE_COMP_REPLICATE);
    de_ep_write_response_world_checksum(w, req->id, &c);
}

/* ---------- Subscriptions and world.delta ---------- */

static void sub_free(ed_world_sub *s) {
    if (s->known != NULL)
        DE_FREE(de_heap_allocator(), s->known, sizeof(u64) * s->known_cap);
    memset(s, 0, sizeof *s);
}

static void on_subscribe(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(subscribe, de_ep_subscribe_params, q);
    if (!de_ep_str_eq(q.topic, DE_EP_TOPIC_WORLD_DELTA)) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "subscribe to %.*s", (int)q.topic.len,
                     q.topic.ptr);
        return;
    }
    ed_world_sub *s = NULL;
    for (u32 i = 0; i < ED_MAX_SUBS && s == NULL; ++i)
        if (g_world.subs[i].topic == 0)
            s = &g_world.subs[i];
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many subscriptions");
        return;
    }
    memset(s, 0, sizeof *s);
    s->topic = 1;
    s->id = ++g_world.next_id;
    s->every = q.has_rate_hz && q.rate_hz > 0 ? (u32)(60.0f / q.rate_hz) : 1u;
    if (s->every == 0)
        s->every = 1;
    s->prime = true;
    if (q.has_filter && q.filter.len > 0) {
        mpack_tree_t tree;
        mpack_tree_init_data(&tree, q.filter.ptr, q.filter.len);
        mpack_tree_parse(&tree);
        mpack_node_t root = mpack_tree_root(&tree);
        if (mpack_tree_error(&tree) == mpack_ok && mpack_node_type(root) == mpack_type_map) {
            mpack_node_t with = mpack_node_map_cstr_optional(root, "with");
            if (mpack_node_type(with) == mpack_type_array)
                for (size_t i = 0; i < mpack_node_array_length(with) && s->with_count < 16; ++i) {
                    mpack_node_t c = mpack_node_array_at(with, i);
                    if (mpack_node_type(c) == mpack_type_str && mpack_node_strlen(c) < 64) {
                        memcpy(s->with[s->with_count], mpack_node_str(c), mpack_node_strlen(c));
                        s->with[s->with_count][mpack_node_strlen(c)] = '\0';
                        ++s->with_count;
                    }
                }
        }
        mpack_tree_destroy(&tree);
    }
    de_ep_subscribe_result r = {s->id};
    de_ep_write_response_subscribe(w, req->id, &r);
}

static void on_unsubscribe(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(unsubscribe, de_ep_unsubscribe_params, q);
    for (u32 i = 0; i < ED_MAX_SUBS; ++i)
        if (g_world.subs[i].topic != 0 && g_world.subs[i].id == q.subscription) {
            sub_free(&g_world.subs[i]);
            de_ep_empty e;
            memset(&e, 0, sizeof e);
            de_ep_write_response_unsubscribe(w, req->id, &e);
            return;
        }
    /* Log and stats subscriptions live in editor_logs.c. */
    if (de__ed_logs_unsubscribe(q.subscription)) {
        de_ep_empty e;
        memset(&e, 0, sizeof e);
        de_ep_write_response_unsubscribe(w, req->id, &e);
        return;
    }
    de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "subscription %u", q.subscription);
}

static bool known_has(const ed_world_sub *s, u64 bits) {
    u32 lo = 0, hi = s->known_count;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (s->known[mid] < bits)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < s->known_count && s->known[lo] == bits;
}

static int cmp_u64(const void *x, const void *y) {
    const u64 a = *(const u64 *)x, b = *(const u64 *)y;
    return a < b ? -1 : a > b;
}

static void sub_delta(de__editor *ed, ed_world_sub *s, u32 since) {
    const de_world *wd = W(ed);
    const bool prime = s->prime;
    s->prime = false;
    if (prime)
        s->resync = true; /* gather as a resync does, then send nothing */
    u32 with[16];
    u32 with_count = 0;
    for (u32 i = 0; i < s->with_count; ++i) {
        const u32 id = de__ed_component(ed, de_ep_cstr(s->with[i])).id;
        if (id == 0)
            return; /* nothing can match */
        with[with_count++] = id;
    }
    de_ep_arena a = {ed->arena, ed->arena_cap, 0};
    /* The matching entities now. */
    u32 total = 0;
    for (u32 ai = 0; ai < wd->archetypes.count; ++ai)
        total += arch_at(wd, ai)->count;
    u64 *now_set = (u64 *)de__ed_alloc(&a, sizeof(u64) * (total + 1));
    de_ep_entity_row *spawned = (de_ep_entity_row *)de__ed_alloc(&a, sizeof(de_ep_entity_row) * (total + 1));
    de_ep_field_change *changed = (de_ep_field_change *)de__ed_alloc(&a, sizeof(de_ep_field_change) * (total * 4 + 1));
    if (now_set == NULL || spawned == NULL || changed == NULL)
        return;
    u32 n_now = 0, n_sp = 0, n_ch = 0;
    for (u32 ai = 0; ai < wd->archetypes.count; ++ai) {
        const ecs_archetype *arch = arch_at(wd, ai);
        if (arch->count == 0)
            continue;
        bool ok = true;
        for (u32 i = 0; i < with_count && ok; ++i)
            ok = arch_has(arch, with[i]);
        if (!ok)
            continue;
        for (u32 ci = 0; ci < arch->chunks.count; ++ci) {
            const ecs_chunk *ch = ecs_table_chunk(arch, ci);
            for (u32 r = 0; r < ch->count; ++r) {
                const u64 bits = ecs_chunk_entities(ch)[r].bits;
                if (de__ed_views_owns(bits))
                    continue;
                now_set[n_now++] = bits;
                if (s->resync || !known_has(s, bits)) {
                    if (row_of(ed, &a, arch, ch, r, NULL, NULL, &spawned[n_sp]))
                        ++n_sp;
                    continue;
                }
                /* Known: the components whose columns changed since the last delta. */
                for (u32 i = 0; i < arch->component_count && n_ch < total * 4; ++i) {
                    const u32 col = arch->column_of[i];
                    if (col == ECS_INVALID_INDEX)
                        continue;
                    const u32 t1 = ecs_chunk_changed(arch, ch, col)[r];
                    const u32 t0 = ecs_chunk_added(arch, ch, col)[r];
                    if (!tick_newer(t1, since) && !tick_newer(t0, since))
                        continue;
                    const ecs_component_info *cinfo = ecs_component_get(wd, arch->components[i]);
                    if (cinfo == NULL || cinfo->name == NULL)
                        continue;
                    const u8 *data = (const u8 *)ecs_chunk_column(arch, ch, col) + (size_t)r * cinfo->size;
                    u32 fc = 0;
                    const de_field_info *fields = de_type_fields(ed->e->types, cinfo->type, &fc);
                    de_ep_kv_raw *kv = (de_ep_kv_raw *)de__ed_alloc(&a, sizeof(de_ep_kv_raw) * (fc + 1));
                    if (kv == NULL)
                        break;
                    for (u32 f = 0; f < fc; ++f) {
                        mpack_writer_t mw;
                        de__ed_raw_begin(&a, &mw);
                        de__ed_write_field(ed, &mw, cinfo->type, f, &fields[f], data + fields[f].offset);
                        kv[f].key = de_ep_cstr(fields[f].name);
                        kv[f].value = de__ed_raw_end(&a, &mw);
                    }
                    de_ep_field_change *o = &changed[n_ch++];
                    o->entity = bits;
                    o->component = de_ep_cstr(cinfo->name);
                    o->fields.items = kv;
                    o->fields.count = fc;
                }
            }
        }
    }
    qsort(now_set, n_now, sizeof(u64), cmp_u64);
    /* Gone since the last delta. */
    u64 *despawned = (u64 *)de__ed_alloc(&a, sizeof(u64) * (s->known_count + 1));
    u32 n_de = 0;
    if (!s->resync && despawned != NULL) {
        u32 j = 0;
        for (u32 i = 0; i < s->known_count; ++i) {
            while (j < n_now && now_set[j] < s->known[i])
                ++j;
            if (j >= n_now || now_set[j] != s->known[i])
                despawned[n_de++] = s->known[i];
        }
    }
    const bool resync = s->resync;
    s->resync = false;
    /* Keep the matching set for next time. */
    if (n_now > s->known_cap) {
        const de_allocator *al = de_heap_allocator();
        u64 *grown = (u64 *)DE_ALLOC(al, sizeof(u64) * (n_now + 64), 8, DE_MEM_GENERAL);
        if (grown == NULL)
            return;
        if (s->known != NULL)
            DE_FREE(al, s->known, sizeof(u64) * s->known_cap);
        s->known = grown;
        s->known_cap = n_now + 64;
    }
    memcpy(s->known, now_set, sizeof(u64) * n_now);
    s->known_count = n_now;
    if (prime || (!resync && n_sp == 0 && n_de == 0 && n_ch == 0))
        return;
    de_ep_world_delta d;
    memset(&d, 0, sizeof d);
    d.subscription = s->id;
    if (resync) {
        d.has_resync = true;
        d.resync = true;
    }
    d.spawned.items = spawned;
    d.spawned.count = n_sp;
    d.despawned.items = despawned;
    d.despawned.count = n_de;
    d.changed.items = changed;
    d.changed.count = n_ch;
    mpack_writer_t w;
    const u64 seq = de__ed_note_begin(ed, &w);
    de_ep_write_notify_world_delta(&w, seq, &d);
    if (mpack_writer_error(&w) == mpack_error_too_big) {
        mpack_writer_destroy(&w);
        s->resync = true; /* too much at once: the next delta starts over */
        return;
    }
    de__ed_send(ed, &w, DE_EP_KIND_NOTIFICATION);
}

static void tick(de__editor *ed) {
    bool any = false;
    for (u32 i = 0; i < ED_MAX_SUBS; ++i)
        any |= g_world.subs[i].topic == 1;
    if (!any)
        return;
    /* Rows stamped from `since` up to `now` changed since the last deltas; writes from here on
     * get later ticks. */
    const u32 now = ecs_change_tick_advance(W(ed));
    const u64 frame = de__ed_frame(ed);
    for (u32 i = 0; i < ED_MAX_SUBS && ed->connected; ++i) {
        ed_world_sub *s = &g_world.subs[i];
        if (s->topic != 1 || (!s->resync && !s->prime && frame % s->every != 0))
            continue;
        sub_delta(ed, s, s->since);
        s->since = now;
    }
}

static void reset(de__editor *ed) {
    (void)ed;
    for (u32 i = 0; i < ED_MAX_SUBS; ++i)
        if (g_world.subs[i].topic != 0)
            sub_free(&g_world.subs[i]);
}

void de__ed_world_register(de__editor *ed) {
    ed->world = (struct de__ed_world *)&g_world;
    de__ed_on(ed, DE_EP_METHOD_WORLD_QUERY, on_query);
    de__ed_on(ed, DE_EP_METHOD_WORLD_ENTITY, on_entity);
    de__ed_on(ed, DE_EP_METHOD_WORLD_ARCHETYPES, on_archetypes);
    de__ed_on(ed, DE_EP_METHOD_WORLD_SYSTEMS, on_systems);
    de__ed_on(ed, DE_EP_METHOD_WORLD_SET_SYSTEM_ENABLED, on_set_system_enabled);
    de__ed_on(ed, DE_EP_METHOD_WORLD_RESOURCES, on_resources);
    de__ed_on(ed, DE_EP_METHOD_WORLD_EVENTS, on_events);
    de__ed_on(ed, DE_EP_METHOD_WORLD_CHECKSUM, on_checksum);
    de__ed_on(ed, DE_EP_METHOD_SUBSCRIBE, on_subscribe);
    de__ed_on(ed, DE_EP_METHOD_UNSUBSCRIBE, on_unsubscribe);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
}
