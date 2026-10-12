/*
 * asset_events.c: asset ECS events and the asset system as a world resource (SPEC §11.3, §7.1;
 * DENG-143).
 *
 * The load code calls de__asset_emit (asset_internal.h) where a slot or a request reaches a final
 * state. Until the system is attached to a world, `emit_fn` is NULL and nothing is produced. The
 * first de_asset_system_update with a world attaches it here: the `de_asset_event` payload type
 * and its event type, the `DeAssetSystemRef` resource, and the `de.asset_events` system in
 * DE_SCHEDULE_FIRST. From then on `emit_fn` queues each event, and that system sends the queue
 * into the world.
 *
 * Why a queue and a First system instead of de_event_send inside de_asset_system_update: the host
 * updates the assets before `First` (so that frame's systems see the new load states), but the
 * ECS event frame step runs at the start of `First`. An event sent before it would belong to the
 * previous event frame and be dropped at the next `First`, so systems could read it in one frame
 * only. Sent from a system of `First`, it is readable in that frame and the next (§7.1). See the
 * Trama page "Decision: Asset events are queued by the update and sent at the start of First".
 *
 * Threads: everything here runs on the main thread (de_asset_system_update, the load API, and
 * the `de.asset_events` system, which is DE_SYSTEM_MAIN_THREAD). The queue grows by doubling and
 * keeps its capacity, so a frame with no event, or no more events than an earlier frame, does not
 * allocate.
 */

#include "asset/asset_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include <stddef.h>
#include <string.h>

#define DE__ASSET_EVENTS_SYSTEM   "de.asset_events"
#define DE__ASSET_EVENTS_QUEUE_0  32u
#define DE__ASSET_EVENT_TYPE_NAME "de_asset_event"
#define DE__ASSET_REF_TYPE_NAME   "DeAssetSystemRef"

typedef struct de__asset_events {
    de_world *world; /**< the attached world; NULL when attaching failed */
    de_event_type type;
    de_resource_id resource;
    de_system system;
    de_asset_event *queue; /**< events not sent yet, in emit order */
    u32 count, capacity;
    bool warned_world; /**< a second world was passed once already */
    bool warned_drop;  /**< an event was dropped (out of memory) once already */
} de__asset_events;

/* ---- The emitter ---------------------------------------------------------------------------- */

static void asset_events_queue(de_asset_system *s, const de_asset_event *ev) {
    de__asset_events *E = s->events;
    if (E == NULL || E->world == NULL)
        return;
    if (E->count == E->capacity) {
        u32 cap = E->capacity != 0 ? E->capacity * 2u : DE__ASSET_EVENTS_QUEUE_0;
        void *p =
            DE_REALLOC(s->alloc, E->queue, sizeof(de_asset_event) * E->capacity,
                       sizeof(de_asset_event) * cap, DE_ALIGNOF(de_asset_event), DE_MEM_ASSET);
        if (p == NULL) {
            if (!E->warned_drop)
                DE_ERROR(s->log, "asset events: out of memory; event kind %d dropped",
                         (int)ev->kind);
            E->warned_drop = true;
            return;
        }
        E->queue = (de_asset_event *)p;
        E->capacity = cap;
    }
    E->queue[E->count++] = *ev;
}

/* `de.asset_events` (First, main thread): sends the queued events, in queue order. */
static void asset_events_system(de_system_ctx *ctx) {
    de_asset_system *s = (de_asset_system *)ctx->user;
    de__asset_events *E = s->events;
    for (u32 i = 0; i < E->count; ++i)
        de_event_send(ctx->world, E->type, &E->queue[i]);
    E->count = 0;
}

/* ---- Attach and detach ---------------------------------------------------------------------- */

static de_result last_error_or(de_result fallback) {
    const de_error *e = de_error_last();
    return e != NULL && e->result != DE_OK ? e->result : fallback;
}

/* Registers the payload and resource types; both registrations keep their ids when they run
 * again (another asset system on the same registry, a hot reload). */
static de_result asset_events_types(de_type_registry *types, de_type_id *payload, de_type_id *ref) {
    static const de_field_info k_fields[] = {
        DE_FIELD(de_asset_event, kind, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(de_asset_event, handle, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(de_asset_event, id, DE_FK_U64, DE_FIELD_DEFAULT),
        DE_FIELD(de_asset_event, state, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(de_asset_event, error, DE_FK_I32, DE_FIELD_DEFAULT),
    };
    DE_STATIC_ASSERT(sizeof(de_asset_event_kind) == 4 && sizeof(de_load_state) == 4 &&
                         sizeof(de_result) == 4,
                     "de_asset_event's enum fields are reflected as i32");
    DE_STATIC_ASSERT(sizeof(de_load_handle) == 4 && sizeof(de_asset_id) == 8,
                     "de_asset_event's handle and id are reflected as u32 and u64");
    *payload = de_type_register(types, &DE_TYPE_DESC(de_asset_event, 1, k_fields));
    if (!DE_HANDLE_VALID(*payload))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL),
                            "registering " DE__ASSET_EVENT_TYPE_NAME);

    /* A pointer has no field kind, so the resource type has no fields: it is never serialized
     * or snapshotted, only looked up (like de_input_resource and de_audio_resource). */
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE__ASSET_REF_TYPE_NAME;
    d.size = (u32)sizeof(DeAssetSystemRef);
    d.align = (u32)DE_ALIGNOF(DeAssetSystemRef);
    d.version = 1;
    d.is_pod = true;
    *ref = de_type_register(types, &d);
    if (!DE_HANDLE_VALID(*ref))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL), "registering " DE__ASSET_REF_TYPE_NAME);
    return DE_OK;
}

static de_result asset_events_setup(de_asset_system *s, de__asset_events *E, de_world *w) {
    de_type_registry *types = de_world_types(w);
    if (types == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "the world has no type registry");
    de_type_id payload = {0}, ref_type = {0};
    DE_TRY(asset_events_types(types, &payload, &ref_type));

    /* One asset system per world: the system name is fixed. */
    de_resource_id found = de_resource_find(w, ref_type);
    if (DE_HANDLE_VALID(found)) {
        const DeAssetSystemRef *cur = (const DeAssetSystemRef *)de_resource_get(w, found);
        if (cur != NULL && cur->ptr != NULL && cur->ptr != s)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS, "the world has another asset system");
    }

    E->type = de_event_register(w, payload);
    if (!DE_HANDLE_VALID(E->type))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL), "registering the event type");

    DeAssetSystemRef ref = {s};
    E->resource = de_resource_insert(w, ref_type, &ref);
    if (!DE_HANDLE_VALID(E->resource))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL), "inserting " DE__ASSET_REF_TYPE_NAME);

    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = DE__ASSET_EVENTS_SYSTEM;
    sd.schedule = DE_SCHEDULE_FIRST;
    sd.fn = asset_events_system;
    sd.user = s;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    sd.writes_events = &E->type;
    sd.writes_event_count = 1;
    E->system = de_system_add(w, &sd);
    if (!DE_HANDLE_VALID(E->system)) {
        de_resource_remove(w, E->resource);
        E->resource = (de_resource_id){0};
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL), "adding " DE__ASSET_EVENTS_SYSTEM);
    }
    return DE_OK;
}

void de__asset_events_attach(de_asset_system *s, de_world *world) {
    if (s == NULL || world == NULL)
        return;
    de__asset_events *E = s->events;
    if (E != NULL) {
        if (E->world != world && E->world != NULL && !E->warned_world) {
            DE_WARN(s->log, "de_asset_system_update: the asset system is attached to another "
                            "world; its events stay there");
            E->warned_world = true;
        }
        return;
    }
    E = DE_NEW(s->alloc, de__asset_events, DE_MEM_ASSET);
    if (E == NULL) {
        DE_ERROR(s->log, "asset events: out of memory attaching to the world (retried next "
                         "update)");
        return;
    }
    memset(E, 0, sizeof *E);
    s->events = E;
    de_result r = asset_events_setup(s, E, world);
    if (r != DE_OK) {
        /* Kept with world NULL, so the attach is not retried (and logged) every frame. */
        const de_error *err = de_error_last();
        DE_ERROR(s->log, "asset events: cannot attach to the world: %s",
                 err != NULL ? err->message : de_result_str(r));
        de_error_clear();
        return;
    }
    E->world = world;
    s->emit_fn = asset_events_queue;
}

void de__asset_events_detach(de_asset_system *s) {
    if (s == NULL)
        return;
    de__asset_events *E = s->events;
    if (s->emit_fn == asset_events_queue)
        s->emit_fn = NULL;
    if (E == NULL)
        return;
    if (E->world != NULL) {
        de_system_remove(E->world, E->system);
        de_resource_remove(E->world, E->resource);
    }
    if (E->queue != NULL)
        DE_FREE(s->alloc, E->queue, sizeof(de_asset_event) * E->capacity);
    DE_FREE(s->alloc, E, sizeof *E);
    s->events = NULL;
}

/* ---- Public accessors ----------------------------------------------------------------------- */

de_event_type de_asset_event_type(const de_asset_system *s) {
    const de__asset_events *E = s != NULL ? s->events : NULL;
    return E != NULL && E->world != NULL ? E->type : (de_event_type){0};
}

de_resource_id de_asset_resource(const de_asset_system *s) {
    const de__asset_events *E = s != NULL ? s->events : NULL;
    return E != NULL && E->world != NULL ? E->resource : (de_resource_id){0};
}
