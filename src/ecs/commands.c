/* commands.c: command buffers, pending entity ids and deterministic apply (DENG-349, SPEC §7.1,
 * §5.7 rule 5).
 *
 * Recording appends {op, entity, id, payload} records to a buffer's page chain. Apply walks the
 * main-thread buffer, then the acquired buffers sorted by (system_order, range_index), each in
 * record order, and runs every command through the internal structural API of DENG-345
 * (ecs_entity_add / ecs_entity_remove / ecs_entity_despawn), ecs_entity_create and
 * de_event_send, so deferred and immediate changes share one code path.
 *
 * Pending ids get their real entity when their spawn applies; later commands and DE_FK_ENTITY
 * payload fields are rewritten through each buffer's `resolved` table. A pre-pass cancels the
 * spawns that a despawn in the same apply names.
 *
 * To add an op (DENG-351): a value in de_command_op, a de_cmd_* recorder, a case in
 * apply_command() and, if it has a payload that owns resources, in destruct_payload(). */
#include "commands_internal.h"
#include "system_internal.h" /* DENG-350: no apply while a schedule runs */

#include "hierarchy_internal.h" /* DENG-351: hierarchy ops, despawn detaches children */

#include <stdint.h>

#if defined(DE_DEBUG) || defined(DE_DEV)
static de_log_category cmd_log_category(void) {
    return de_log_register_category("ecs", DE_LOG_INFO);
}
#define CMD_WARN(...) DE_WARN(cmd_log_category(), __VA_ARGS__)
#else
#define CMD_WARN(...) ((void)0)
#endif

static u32 align_up_u32(u32 v, u32 align) {
    return (v + align - 1u) & ~(align - 1u);
}

/* ---- Buffers and pages ----------------------------------------------------------------------- */

static de_commands *buffer_at(const ecs_cmd_state *st, u32 slot) {
    return ((de_commands *const *)st->buffers.data)[slot];
}

static de_commands *buffer_create(de_world *w, u32 slot) {
    de_commands *c = DE_NEW(w->a, de_commands, DE_MEM_ECS);
    if (c == NULL)
        return NULL;
    memset(c, 0, sizeof *c);
    c->w = w;
    c->slot = slot;
    DE_ARRAY_INIT_TAG(&c->resolved, de_entity, w->a, DE_MEM_ECS);
    return c;
}

static void buffer_free(de_world *w, de_commands *c) {
    ecs_cmd_page *p = c->head;
    while (p != NULL) {
        ecs_cmd_page *next = p->next;
        DE_FREE(w->a, p, (size_t)ECS_CMD_PAGE_HEADER + p->capacity);
        p = next;
    }
    de_array_free(&c->resolved);
    DE_FREE(w->a, c, sizeof *c);
}

/* Where a record with a payload of `psize` bytes at alignment `palign` would go in a page that
 * has `used` bytes in use: the record's start, its payload offset and its total size. */
typedef struct record_fit {
    u32 start, payload_offset, size;
} record_fit;

static bool fit_record(u32 used, u32 capacity, u32 psize, u32 palign, record_fit *out) {
    const u32 start = align_up_u32(used, 8u);
    const u64 payload_at =
        psize ? (u64)align_up_u32(start + (u32)sizeof(ecs_cmd), palign) : start + sizeof(ecs_cmd);
    const u64 end = (payload_at + psize + 7u) & ~(u64)7u;
    if (end > capacity)
        return false;
    out->start = start;
    out->payload_offset = (u32)(payload_at - start);
    out->size = (u32)(end - start);
    return true;
}

/* Appends a record header (and room for its payload) to `c`. NULL on out of memory. */
static ecs_cmd *record(de_commands *c, ecs_cmd_op op, de_entity e, u32 id, u32 psize, u32 palign) {
    DE_ASSERT(!c->w->cmd->applying); /* nothing records while an apply runs */
    if (palign == 0)
        palign = 1;
    record_fit fit;
    ecs_cmd_page *p = c->cur;
    if (p == NULL || !fit_record(p->used, p->capacity, psize, palign, &fit)) {
        /* The next page (empty: pages after `cur` are never written before it) if it fits,
         * else a new page inserted after `cur`. */
        ecs_cmd_page *next = p ? p->next : c->head;
        if (next != NULL && fit_record(0, next->capacity, psize, palign, &fit)) {
            p = next;
        } else {
            u32 capacity = ECS_CMD_PAGE_BYTES - ECS_CMD_PAGE_HEADER;
            if (!fit_record(0, capacity, psize, palign, &fit)) {
                const u64 need = sizeof(ecs_cmd) + (u64)palign + psize + 8u;
                if (need > UINT32_MAX - ECS_CMD_PAGE_HEADER) {
                    DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: command payload of %u bytes is too large",
                            psize);
                    return NULL;
                }
                capacity = align_up_u32((u32)need, 8u);
            }
            ecs_cmd_page *np =
                (ecs_cmd_page *)DE_ALLOC(c->w->a, (size_t)ECS_CMD_PAGE_HEADER + capacity,
                                         DE_ECS_MAX_COMPONENT_ALIGN, DE_MEM_ECS);
            if (np == NULL) {
                DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for a command page");
                return NULL;
            }
            np->capacity = capacity;
            np->used = 0;
            if (p == NULL) { /* no page written yet: put it first */
                np->next = c->head;
                c->head = np;
            } else {
                np->next = p->next;
                p->next = np;
            }
            p = np;
            DE_VERIFY(fit_record(0, capacity, psize, palign, &fit));
        }
        c->cur = p;
    }
    ecs_cmd *cmd = (ecs_cmd *)(void *)(ecs_cmd_page_data(p) + fit.start);
    cmd->op = op;
    cmd->pad_ = 0;
    cmd->payload_offset = (u16)fit.payload_offset;
    cmd->size = fit.size;
    cmd->id = id;
    cmd->payload_size = psize;
    cmd->entity = e;
    p->used = fit.start + fit.size;
    c->cmd_count += 1u;
    c->active = true;
    return cmd;
}

static void *cmd_payload(const ecs_cmd *cmd) {
    return cmd->payload_size ? (u8 *)(uintptr_t)cmd + cmd->payload_offset : NULL;
}

/* Calls `fn(cmd, user)` on every record of `c` in record order; stops when it returns false. */
typedef bool (*record_fn)(ecs_cmd *cmd, void *user);
static void for_each_record(const de_commands *c, record_fn fn, void *user) {
    for (ecs_cmd_page *p = c->head; p != NULL; p = p->next) {
        u32 off = 0;
        while (off < p->used) {
            ecs_cmd *cmd = (ecs_cmd *)(void *)(ecs_cmd_page_data(p) + off);
            off += cmd->size;
            if (!fn(cmd, user))
                return;
        }
    }
}

/* Empties `c`, keeping its pages. */
static void buffer_reset(de_commands *c) {
    for (ecs_cmd_page *p = c->head; p != NULL; p = p->next)
        p->used = 0;
    c->cur = NULL;
    c->cmd_count = 0;
    c->spawn_count = 0;
    c->pending_despawns = 0;
    c->active = false;
}

static ecs_event_queue *event_queue(const de_world *w, u32 id) {
    if (id == 0 || id > w->events.count)
        return NULL;
    return (ecs_event_queue *)de_array_at(&w->events, id - 1u);
}

/* Runs the destruct hook of a payload that will not be applied. */
static void destruct_payload(de_world *w, const ecs_cmd *cmd) {
    void *payload = cmd_payload(cmd);
    if (payload == NULL)
        return;
    if (cmd->op == DE_CMD_ADD) {
        const ecs_component_info *info = ecs_component_get(w, cmd->id);
        if (info != NULL && info->hooks.destruct != NULL)
            info->hooks.destruct(payload);
    } else if (cmd->op == DE_CMD_SEND_EVENT) {
        const ecs_event_queue *q = event_queue(w, cmd->id);
        if (q != NULL && q->hooks.destruct != NULL)
            q->hooks.destruct(payload);
    }
}

static bool discard_one(ecs_cmd *cmd, void *user) {
    destruct_payload((de_world *)user, cmd);
    return true;
}

void ecs_cmd_buffer_discard(de_commands *c) {
    if (c == NULL)
        return;
    for_each_record(c, discard_one, c->w);
    buffer_reset(c);
}

/* ---- World state ----------------------------------------------------------------------------- */

de_result ecs_commands_init(de_world *w) {
    ecs_cmd_state *st = DE_NEW(w->a, ecs_cmd_state, DE_MEM_ECS);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for command state");
    memset(st, 0, sizeof *st);
    DE_ARRAY_INIT_TAG(&st->buffers, de_commands *, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&st->free_slots, u32, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&st->active, ecs_cmd_active, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&st->component_refs, ecs_cmd_type_refs, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&st->event_refs, ecs_cmd_type_refs, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&st->entity_offsets, u32, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&st->homes, ecs_cmd_active, w->a, DE_MEM_ECS);
    de_mutex_init(&st->lock);
    w->cmd = st;
    /* Slot 0: the main-thread buffer, so de_world_commands never takes the lock. */
    st->main = buffer_create(w, 0);
    if (st->main == NULL || DE_ARRAY_PUSH(&st->buffers, de_commands *, st->main) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for command state");
    return DE_OK;
}

void ecs_commands_destroy(de_world *w) {
    ecs_cmd_state *st = w->cmd;
    if (st == NULL)
        return;
    bool main_listed = false;
    for (u32 i = 0; i < st->buffers.count; ++i) {
        de_commands *c = buffer_at(st, i);
        main_listed |= c == st->main;
        ecs_cmd_buffer_discard(c);
        buffer_free(w, c);
    }
    if (st->main != NULL && !main_listed) { /* init failed between create and push */
        buffer_free(w, st->main);
    }
    de_array_free(&st->buffers);
    de_array_free(&st->free_slots);
    de_array_free(&st->active);
    de_array_free(&st->component_refs);
    de_array_free(&st->event_refs);
    de_array_free(&st->entity_offsets);
    de_array_free(&st->homes);
    de_mutex_destroy(&st->lock);
    DE_FREE(w->a, st, sizeof *st);
    w->cmd = NULL;
}

static u64 active_key(u32 system_order, u32 range_index) {
    return ((u64)system_order << 32) | range_index;
}

/* First position in sorted list `list` (st->active or st->homes) whose key is >= `key`. */
static u32 key_lower_bound(const de_array *list, u64 key) {
    u32 lo = 0, hi = list->count;
    const ecs_cmd_active *a = (const ecs_cmd_active *)list->data;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (a[mid].key < key)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

de_commands *ecs_cmd_buffer_acquire(de_world *w, u32 system_order, u32 range_index) {
    if (w == NULL || w->cmd == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs_cmd_buffer_acquire: world is NULL");
        return NULL;
    }
    ecs_cmd_state *st = w->cmd;
    const u64 key = active_key(system_order, range_index);
    de_mutex_lock(&st->lock);
    DE_ASSERT(!st->applying);
    const u32 pos = key_lower_bound(&st->active, key);
    if (pos < st->active.count && DE_ARRAY_AT(&st->active, ecs_cmd_active, pos).key == key) {
        de_commands *c = buffer_at(st, DE_ARRAY_AT(&st->active, ecs_cmd_active, pos).slot);
        de_mutex_unlock(&st->lock);
        return c;
    }
    /* Room in the sorted list first, so nothing below can fail after a slot is taken. */
    if (de_array_reserve(&st->active, st->active.count + 1u) != DE_OK) {
        de_mutex_unlock(&st->lock);
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for command buffers");
        return NULL;
    }
    /* DENG-379: the key's own slot if it has one (never acquired by another key: a key takes
     * another key's slot only from the spare list, and that removes the old key's home). */
    u32 slot = UINT32_MAX;
    const u32 home = key_lower_bound(&st->homes, key);
    if (home < st->homes.count && DE_ARRAY_AT(&st->homes, ecs_cmd_active, home).key == key) {
        slot = DE_ARRAY_AT(&st->homes, ecs_cmd_active, home).slot;
        DE_ASSERT(!buffer_at(st, slot)->active);
    } else {
        /* No slot yet: a spare one (skipping spares that their own key took back since the
         * last apply), else a new one. */
        u32 spare;
        while (DE_ARRAY_POP(&st->free_slots, u32, &spare)) {
            if (!buffer_at(st, spare)->active) {
                slot = spare;
                break;
            }
        }
        if (slot == UINT32_MAX && st->buffers.count >= ECS_CMD_MAX_SLOTS) {
            /* At the slot limit: any slot not in use, even one the last apply used. */
            for (u32 s = 1; s < st->buffers.count; ++s) {
                if (!buffer_at(st, s)->active) {
                    slot = s;
                    break;
                }
            }
        }
        if (slot != UINT32_MAX) {
            /* Take it over: its old key loses its home. */
            const de_commands *old = buffer_at(st, slot);
            const u32 at =
                key_lower_bound(&st->homes, active_key(old->system_order, old->range_index));
            DE_ASSERT(at < st->homes.count &&
                      DE_ARRAY_AT(&st->homes, ecs_cmd_active, at).slot == slot);
            de_array_remove_ordered(&st->homes, at);
        } else {
            slot = st->buffers.count;
            if (slot >= ECS_CMD_MAX_SLOTS) {
                de_mutex_unlock(&st->lock);
                DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: more than %u command buffers in use",
                        ECS_CMD_MAX_SLOTS - 1u);
                return NULL;
            }
            de_commands *nc = buffer_create(w, slot);
            /* The spare list and the homes can hold every slot, so neither the release at
             * apply nor a later take-over allocates. */
            if (nc == NULL || de_array_reserve(&st->free_slots, slot + 1u) != DE_OK ||
                de_array_reserve(&st->homes, slot) != DE_OK ||
                DE_ARRAY_PUSH(&st->buffers, de_commands *, nc) != DE_OK) {
                if (nc != NULL)
                    buffer_free(w, nc);
                de_mutex_unlock(&st->lock);
                DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for command buffers");
                return NULL;
            }
        }
        const ecs_cmd_active h = {key, slot, 0};
        DE_VERIFY(de_array_insert(&st->homes, key_lower_bound(&st->homes, key), &h) ==
                  DE_OK); /* reserved at slot creation */
    }
    de_commands *c = buffer_at(st, slot);
    c->system_order = system_order;
    c->range_index = range_index;
    c->active = true;
    const ecs_cmd_active entry = {key, slot, 0};
    DE_VERIFY(de_array_insert(&st->active, pos, &entry) == DE_OK); /* reserved above */
    de_mutex_unlock(&st->lock);
    return c;
}

/* ---- Entity references in payloads ----------------------------------------------------------- */

#define REFS_MAX_DEPTH 16u

/* Appends the offsets of every DE_FK_ENTITY in struct type `t` (at byte `base`) to `out`. */
static de_result collect_refs(const de_type_registry *r, de_type_id t, u32 base, de_array *out,
                              u32 depth) {
    if (depth > REFS_MAX_DEPTH)
        return DE_OK; /* registration forbids cycles; this is only a guard */
    u32 n = 0;
    const de_field_info *f = de_type_fields(r, t, &n);
    for (u32 i = 0; i < n; ++i) {
        const u32 off = base + f[i].offset;
        if (f[i].kind == DE_FK_ENTITY) {
            DE_TRY(de_array_push(out, &off));
        } else if (f[i].kind == DE_FK_STRUCT) {
            DE_TRY(collect_refs(r, de_type_find(r, f[i].attrs.nested_type), off, out, depth + 1u));
        } else if (f[i].kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *el = de_type_array_element(r, t, i);
            if (el == NULL)
                continue;
            const u32 count = f[i].attrs.array_count;
            if (el->kind == DE_FK_ENTITY) {
                for (u32 k = 0; k < count; ++k) {
                    const u32 eo = off + k * el->size;
                    DE_TRY(de_array_push(out, &eo));
                }
            } else if (el->kind == DE_FK_STRUCT) {
                const de_type_id nt = de_type_find(r, el->attrs.nested_type);
                for (u32 k = 0; k < count; ++k)
                    DE_TRY(collect_refs(r, nt, off + k * el->size, out, depth + 1u));
            }
        }
    }
    return DE_OK;
}

/* The entity-reference offsets of a command payload type, built on first use (apply only, so
 * single-threaded). NULL with nothing to remap (or out of memory: references stay as they are,
 * with a warning). */
static const ecs_cmd_type_refs *payload_refs(de_world *w, de_array *table, u32 id,
                                             de_type_id type) {
    ecs_cmd_state *st = w->cmd;
    if (id == 0)
        return NULL;
    if (table->count < id && de_array_resize(table, id) != DE_OK) {
        CMD_WARN("ecs: out of memory; entity references in command payloads are not remapped");
        return NULL;
    }
    ecs_cmd_type_refs *refs = (ecs_cmd_type_refs *)de_array_at(table, id - 1u);
    if (!refs->built) {
        const u32 first = st->entity_offsets.count;
        if (collect_refs(w->types, type, 0, &st->entity_offsets, 0) != DE_OK) {
            st->entity_offsets.count = first;
            CMD_WARN("ecs: out of memory; entity references in command payloads are not "
                     "remapped");
            return NULL;
        }
        refs->first = first;
        refs->count = st->entity_offsets.count - first;
        refs->built = true;
    }
    return refs->count ? refs : NULL;
}

/* The `resolved` entry of pending id `e` in the current apply: the real entity, DE_ENTITY_NULL
 * (not spawned yet, or not from this apply) or ECS_CMD_CANCELLED. */
static de_entity pending_lookup(const ecs_cmd_state *st, de_entity e) {
    const u32 slot = ecs_cmd_pending_slot(e);
    const u32 local = ecs_cmd_pending_local(e);
    if (slot >= st->buffers.count)
        return DE_ENTITY_NULL;
    const de_commands *c = buffer_at(st, slot);
    if (c->resolved_epoch != st->epoch || local >= c->resolved.count)
        return DE_ENTITY_NULL;
    return ((const de_entity *)c->resolved.data)[local];
}

static bool is_cancelled(de_entity e) {
    return e.bits == ECS_CMD_CANCELLED.bits;
}

/* Rewrites the pending ids in a payload's entity fields to real entities. */
static void remap_payload(de_world *w, const ecs_cmd_type_refs *refs, u8 *payload) {
    const ecs_cmd_state *st = w->cmd;
    const u32 *offs = (const u32 *)st->entity_offsets.data + refs->first;
    for (u32 i = 0; i < refs->count; ++i) {
        de_entity e;
        memcpy(&e, payload + offs[i], sizeof e);
        if (!de_entity_is_pending(e))
            continue;
        de_entity real = pending_lookup(st, e);
        if (is_cancelled(real)) {
            real = DE_ENTITY_NULL;
        } else if (real.bits == 0) {
            CMD_WARN("ecs: command payload refers to pending entity 0x%016llx, which is not "
                     "spawned at this point of the apply; set to null",
                     (unsigned long long)e.bits);
        }
        memcpy(payload + offs[i], &real, sizeof real);
    }
}

/* ---- Apply ----------------------------------------------------------------------------------- */

/* The real, alive target of a command, or DE_ENTITY_NULL (dropped; warns unless the target is a
 * cancelled spawn). */
static de_entity command_target(de_world *w, const ecs_cmd *cmd) {
    de_entity t = cmd->entity;
    if (de_entity_is_pending(t)) {
        const de_entity real = pending_lookup(w->cmd, t);
        if (is_cancelled(real))
            return DE_ENTITY_NULL; /* its spawn was cancelled: drop quietly */
        t = real;
    }
    if (t.bits == 0 || ecs_entity_record(w, t) == NULL) {
        CMD_WARN("ecs: command %u on entity 0x%016llx, which is not alive at apply; dropped",
                 (unsigned)cmd->op, (unsigned long long)cmd->entity.bits);
        return DE_ENTITY_NULL;
    }
    return t;
}

static void apply_add(de_world *w, ecs_cmd *cmd) {
    const ecs_component_info *info = ecs_component_get(w, cmd->id);
    u8 *payload = (u8 *)cmd_payload(cmd);
    if (payload != NULL) {
        const ecs_cmd_type_refs *refs =
            payload_refs(w, &w->cmd->component_refs, cmd->id, info->type);
        if (refs != NULL)
            remap_payload(w, refs, payload);
    }
    const de_entity t = command_target(w, cmd);
    void *data = NULL;
    if (t.bits == 0 || ecs_entity_add(w, t, cmd->id, &data) != DE_OK) {
        destruct_payload(w, cmd);
        return;
    }
    if (payload != NULL) {
        /* Move the payload in: the column's current value (new and constructed, or the old one)
         * is destructed and overwritten. Components are trivially relocatable. */
        if (info->hooks.destruct != NULL)
            info->hooks.destruct(data);
        memcpy(data, payload, info->size);
    }
}

static void apply_send_event(de_world *w, ecs_cmd *cmd) {
    const ecs_event_queue *q = event_queue(w, cmd->id);
    u8 *payload = (u8 *)cmd_payload(cmd);
    const ecs_cmd_type_refs *refs = payload_refs(w, &w->cmd->event_refs, cmd->id, q->payload);
    if (refs != NULL)
        remap_payload(w, refs, payload);
    de_event_send(w, (de_event_type){cmd->id}, payload); /* copies (hook or memcpy) */
    destruct_payload(w, cmd);
}

/* DENG-351: DE_CMD_SET_PARENT. The payload is the new parent (real or pending, or null). */
static void apply_set_parent(de_world *w, ecs_cmd *cmd) {
    const de_entity child = command_target(w, cmd);
    if (child.bits == 0)
        return;
    de_entity parent;
    memcpy(&parent, cmd_payload(cmd), sizeof parent);
    if (de_entity_is_pending(parent)) {
        parent = pending_lookup(w->cmd, parent);
        if (parent.bits == 0 || is_cancelled(parent)) {
            CMD_WARN("ecs: deferred set_parent of 0x%016llx: the parent is a spawn that is "
                     "cancelled or not applied yet; dropped",
                     (unsigned long long)child.bits);
            return;
        }
    }
    if (w->hier == NULL)
        return; /* recorded only with built-ins (de_cmd_set_parent); a guard */
    if (parent.bits != 0 && ecs_entity_record(w, parent) == NULL) {
        CMD_WARN("ecs: deferred set_parent of 0x%016llx under 0x%016llx, which is not alive at "
                 "apply; dropped",
                 (unsigned long long)child.bits, (unsigned long long)parent.bits);
        return;
    }
    (void)ecs_hier_set_parent(w, child, parent, true); /* a cycle is dropped with a warning */
}

/* Applies one command of buffer `c`. */
static void apply_command(de_world *w, de_commands *c, ecs_cmd *cmd) {
    switch ((de_command_op)cmd->op) {
    case DE_CMD_SPAWN: {
        de_entity *slot = &DE_ARRAY_AT(&c->resolved, de_entity, ecs_cmd_pending_local(cmd->entity));
        if (is_cancelled(*slot))
            return;
        de_entity e = DE_ENTITY_NULL;
        if (ecs_entity_create(w, ecs_archetype_at(w, ECS_EMPTY_ARCHETYPE), &e) != DE_OK)
            CMD_WARN("ecs: deferred spawn failed (out of memory); commands on it are dropped");
        *slot = e;
        return;
    }
    case DE_CMD_DESPAWN: {
        const de_entity t = command_target(w, cmd);
        if (t.bits != 0) {
            ecs_hier_on_despawn(w, t); /* DENG-351: as de_entity_despawn */
            (void)ecs_entity_despawn(w, t);
        }
        return;
    }
    case DE_CMD_ADD:
        apply_add(w, cmd);
        return;
    case DE_CMD_REMOVE: {
        const de_entity t = command_target(w, cmd);
        if (t.bits != 0)
            (void)ecs_entity_remove(w, t, cmd->id);
        return;
    }
    case DE_CMD_SEND_EVENT:
        apply_send_event(w, cmd);
        return;
    /* DENG-351: hierarchy ops. */
    case DE_CMD_SET_PARENT:
        apply_set_parent(w, cmd);
        return;
    case DE_CMD_DESPAWN_RECURSIVE: {
        const de_entity t = command_target(w, cmd);
        if (t.bits != 0)
            ecs_hier_despawn_recursive(w, t);
        return;
    }
    }
    DE_PANIC("ecs: unknown command op %u", (unsigned)cmd->op); /* a corrupted buffer */
}

typedef struct apply_ctx {
    de_world *w;
    de_commands *c;
} apply_ctx;

static bool apply_one(ecs_cmd *cmd, void *user) {
    apply_ctx *ctx = (apply_ctx *)user;
    apply_command(ctx->w, ctx->c, cmd);
    return true;
}

/* Cancel pre-pass: a despawn of a pending id marks that spawn cancelled. */
static bool cancel_one(ecs_cmd *cmd, void *user) {
    if (cmd->op != DE_CMD_DESPAWN || !de_entity_is_pending(cmd->entity))
        return true;
    const ecs_cmd_state *st = (const ecs_cmd_state *)user;
    const u32 slot = ecs_cmd_pending_slot(cmd->entity);
    const u32 local = ecs_cmd_pending_local(cmd->entity);
    if (slot < st->buffers.count) {
        de_commands *c = buffer_at(st, slot);
        if (c->resolved_epoch == st->epoch && local < c->resolved.count)
            ((de_entity *)c->resolved.data)[local] = ECS_CMD_CANCELLED;
    }
    return true;
}

/* The buffer at position `i` of the apply order: the main buffer, then `active` by key. */
static de_commands *apply_order_at(const ecs_cmd_state *st, u32 i) {
    if (i == 0)
        return st->main;
    return buffer_at(st, ((const ecs_cmd_active *)st->active.data)[i - 1u].slot);
}

void ecs_cmd_apply_all(de_world *w) {
    if (w == NULL || w->cmd == NULL)
        return;
    ecs_cmd_state *st = w->cmd;
    DE_ASSERT(!st->applying);
    st->applying = true;
    st->epoch += 1u;
    const u32 n = st->active.count + 1u;

    /* 1. Fresh resolve tables. A buffer whose table cannot grow is dropped whole (its pending
     *    ids could not be resolved). */
    for (u32 i = 0; i < n; ++i) {
        de_commands *c = apply_order_at(st, i);
        if (de_array_resize(&c->resolved, c->spawn_count) != DE_OK) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                    "ecs: out of memory applying commands; %u commands of "
                    "buffer (%u, %u) dropped",
                    c->cmd_count, c->system_order, c->range_index);
            ecs_cmd_buffer_discard(c);
            c->resolved.count = 0;
            c->resolved_epoch = 0;
            continue;
        }
        if (c->resolved.count > 0)
            memset(c->resolved.data, 0, (size_t)c->resolved.count * sizeof(de_entity));
        c->resolved_epoch = st->epoch;
    }
    /* 2. Cancel the spawns of pending ids that are despawned anywhere in this apply. */
    for (u32 i = 0; i < n; ++i) {
        de_commands *c = apply_order_at(st, i);
        if (c->pending_despawns > 0)
            for_each_record(c, cancel_one, st);
    }
    /* 3. Apply, in (main, then system_order, range_index) order, each buffer in record order. */
    for (u32 i = 0; i < n; ++i) {
        apply_ctx ctx = {w, apply_order_at(st, i)};
        for_each_record(ctx.c, apply_one, &ctx);
    }
    /* 4. Release. The spare list for the next acquires is every slot this apply did not use
     *    (DENG-379): the keys of the used slots most likely come back and find them by their
     *    home. Then empty every buffer (pages kept). */
    st->free_slots.count = 0;
    for (u32 slot = st->buffers.count; slot-- > 1u;)
        if (!buffer_at(st, slot)->active)
            DE_VERIFY(de_array_push(&st->free_slots, &slot) == DE_OK); /* reserved at creation */
    st->main->active = false;
    buffer_reset(st->main);
    for (u32 i = 0; i < st->active.count; ++i)
        buffer_reset(buffer_at(st, DE_ARRAY_AT(&st->active, ecs_cmd_active, i).slot));
    st->active.count = 0;
    st->applying = false;
}

/* ---- Public API ------------------------------------------------------------------------------ */

de_entity de_cmd_spawn(de_commands *c) {
    if (c == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_spawn: buffer is NULL");
        return DE_ENTITY_NULL;
    }
    if (c->spawn_count >= DE_CMD_MAX_SPAWNS) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_cmd_spawn: more than %u spawns in one buffer",
                DE_CMD_MAX_SPAWNS);
        return DE_ENTITY_NULL;
    }
    const de_entity e = ecs_cmd_pending_make(c->slot, c->spawn_count);
    if (record(c, DE_CMD_SPAWN, e, 0, 0, 1) == NULL)
        return DE_ENTITY_NULL;
    c->spawn_count += 1u;
    return e;
}

void de_cmd_despawn(de_commands *c, de_entity e) {
    if (c == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_despawn: buffer is NULL");
        return;
    }
    if (record(c, DE_CMD_DESPAWN, e, 0, 0, 1) != NULL && de_entity_is_pending(e))
        c->pending_despawns += 1u;
}

void *de_cmd_add(de_commands *c, de_entity e, de_component comp) {
    if (c == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_add: buffer is NULL");
        return NULL;
    }
    const ecs_component_info *info = ecs_component_get(c->w, comp.id);
    if (info == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_add: component %u is not registered", comp.id);
        return NULL;
    }
    ecs_cmd *cmd = record(c, DE_CMD_ADD, e, comp.id, info->size, info->align);
    if (cmd == NULL)
        return NULL;
    void *payload = cmd_payload(cmd);
    if (payload == NULL)
        return (void *)c->w->tag_placeholder;
    if (info->hooks.construct != NULL)
        info->hooks.construct(payload);
    else
        memset(payload, 0, info->size);
    return payload;
}

void de_cmd_remove(de_commands *c, de_entity e, de_component comp) {
    if (c == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_remove: buffer is NULL");
        return;
    }
    if (ecs_component_get(c->w, comp.id) == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_remove: component %u is not registered", comp.id);
        return;
    }
    (void)record(c, DE_CMD_REMOVE, e, comp.id, 0, 1);
}

void de_cmd_send_event(de_commands *c, de_event_type t, const void *payload) {
    if (c == NULL || payload == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_send_event: buffer and payload are required");
        return;
    }
    const ecs_event_queue *q = event_queue(c->w, t.id);
    if (q == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_send_event: event type %u is not registered",
                t.id);
        return;
    }
    ecs_cmd *cmd = record(c, DE_CMD_SEND_EVENT, DE_ENTITY_NULL, t.id, q->size, q->align);
    if (cmd == NULL)
        return;
    void *dst = cmd_payload(cmd);
    if (q->hooks.copy != NULL)
        q->hooks.copy(dst, payload);
    else
        memcpy(dst, payload, q->size);
}

/* DENG-351: hierarchy commands. */
void de_cmd_set_parent(de_commands *c, de_entity child, de_entity parent) {
    if (c == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_set_parent: buffer is NULL");
        return;
    }
    if (c->w->hier == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_set_parent: the built-in components are not "
                                         "registered; call de_builtin_components() at setup");
        return;
    }
    ecs_cmd *cmd =
        record(c, DE_CMD_SET_PARENT, child, 0, (u32)sizeof(de_entity), (u32)DE_ALIGNOF(de_entity));
    if (cmd != NULL)
        memcpy(cmd_payload(cmd), &parent, sizeof parent);
}

void de_cmd_despawn_recursive(de_commands *c, de_entity e) {
    if (c == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cmd_despawn_recursive: buffer is NULL");
        return;
    }
    (void)record(c, DE_CMD_DESPAWN_RECURSIVE, e, 0, 0, 1);
}

de_commands *de_world_commands(de_world *w) {
    if (w == NULL || w->cmd == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_commands: world is NULL");
        return NULL;
    }
    return w->cmd->main;
}

void de_world_apply_commands(de_world *w) {
    if (w == NULL)
        return;
    ECS_ASSERT_STRUCTURAL(w);
    /* DENG-350: not even inside an exclusive system (which may use the immediate API): the
     * schedule's buffers are acquired for the whole run and applied at its sync point. */
    DE_ASSERT(w->systems == NULL || !w->systems->running);
    ecs_cmd_apply_all(w);
}

de_entity de_commands_resolve(const de_world *w, de_entity pending) {
    if (!de_entity_is_pending(pending))
        return pending;
    if (w == NULL || w->cmd == NULL)
        return DE_ENTITY_NULL;
    const de_entity real = pending_lookup(w->cmd, pending);
    return is_cancelled(real) ? DE_ENTITY_NULL : real;
}

typedef struct iterate_ctx {
    de_command_visit_fn fn;
    void *user;
} iterate_ctx;

static bool iterate_one(ecs_cmd *cmd, void *user) {
    const iterate_ctx *ctx = (const iterate_ctx *)user;
    de_command view;
    memset(&view, 0, sizeof view);
    view.op = (de_command_op)cmd->op;
    view.entity = cmd->entity;
    view.payload = cmd_payload(cmd);
    view.payload_size = cmd->payload_size;
    if (cmd->op == DE_CMD_ADD || cmd->op == DE_CMD_REMOVE)
        view.component.id = cmd->id;
    else if (cmd->op == DE_CMD_SEND_EVENT)
        view.event.id = cmd->id;
    return ctx->fn(&view, ctx->user);
}

void de_commands_iterate(const de_commands *c, de_command_visit_fn fn, void *user) {
    if (c == NULL || fn == NULL)
        return;
    iterate_ctx ctx = {fn, user};
    for_each_record(c, iterate_one, &ctx);
}

u32 de_commands_count(const de_commands *c) {
    return c ? c->cmd_count : 0u;
}
