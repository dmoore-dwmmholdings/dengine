/*
 * status.c: request aggregation, de_asset_status / _status_of / _status_list and de_asset_wait
 * (SPEC §11.3; DENG-141). See load_internal.h.
 *
 * A request's aggregate is cached on the request and recomputed (O(assets in the request)) when
 * it is created and by every de__load_update while it is not final, so de_asset_status is O(1)
 * and never allocates: games call it every frame.
 */

#include "asset/load_internal.h"

#include "asset/asset_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "ecs/in_schedule_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <string.h>

/* How long de_asset_wait sleeps between polls when it has nothing to help with. */
#define DE__WAIT_POLL_NS 100000u /* 0.1 ms */

/* Writes the message of entry `i`'s failure into `out`. */
static void entry_failure_msg(de_asset_system *s, const de__load_entry *e, char *out, size_t size) {
    const char *path = de__resolved_path(s->resolved, e->id);
    char name[96];
    if (path != NULL)
        snprintf(name, sizeof name, "\"%.80s\"", path);
    else
        snprintf(name, sizeof name, "%016llx", (unsigned long long)e->id.hash);
    if (e->slot == 0) {
        if (e->error == DE_ERR_NOT_FOUND)
            snprintf(out, size, "asset %s is not in the asset manifest", name);
        else
            snprintf(out, size, "asset %s: %s", name, de_result_str(e->error));
        return;
    }
    const de__load_slot *ls = de__load_slot_find(s, e->slot);
    if (ls != NULL && ls->error_msg != NULL) {
        snprintf(out, size, "%s", ls->error_msg);
        return;
    }
    const de__slot *sl = de__slot_get(s, e->slot);
    snprintf(out, size, "asset %s failed to load: %s", name,
             de_result_str(sl != NULL ? sl->error : DE_ERR_INTERNAL));
}

void de__request_finish(de_asset_system *s, de__request *r, de_load_state state) {
    DE_ASSERT(r->state < DE_LOAD_READY && state >= DE_LOAD_READY);
    r->state = state;
    r->progress = 1.0f;
    /* Nothing waits on the slots for this request any more; slots nobody else waits on get
     * their backend work cancelled (STOP_ON_ERROR, de_asset_cancel, release). */
    const bool events = (r->flags & DE_LOAD_NO_EVENTS) == 0;
    for (u32 i = 0; i < r->count; ++i) {
        if (r->entries[i].slot == 0)
            continue;
        if (events) { /* DENG-143: this request no longer asks for the slot's events */
            de__load_slot *ls = de__load_slot_find(s, r->entries[i].slot);
            if (ls != NULL && ls->interest_events != 0)
                ls->interest_events--;
        }
        de__load_slot_unwant(s, r->entries[i].slot, (de__load_priority)r->priority);
    }
    /* DENG-143: DE_ASSET_EV_REQUEST_DONE, unless the request asked for no events or is being
     * released. The handle is rebuilt from the request's index and generation. */
    if (events && !r->releasing && s->emit_fn != NULL && s->load != NULL) {
        u32 index = (u32)(r - s->load->requests);
        de_asset_event ev = {DE_ASSET_EV_REQUEST_DONE,
                             {de__slot_handle_make(index, r->gen)},
                             {0},
                             state,
                             r->first_error};
        de__asset_emit(s, &ev);
    }
}

void de__request_refresh(de_asset_system *s, de__request *r) {
    if (r->state >= DE_LOAD_READY)
        return;
    u32 ready = 0, failed = 0, first = DE__LOAD_NONE;
    u64 bytes_total = 0, bytes_done = 0;
    bool started = false;
    for (u32 i = 0; i < r->count; ++i) {
        const de__load_entry *e = &r->entries[i];
        de__slot *sl = e->slot != 0 ? de__slot_get(s, e->slot) : NULL;
        if (sl == NULL) {
            failed++;
            if (first == DE__LOAD_NONE)
                first = i;
            continue;
        }
        de__load_slot *ls = de__load_slot_find(s, e->slot);
        u64 total = 0, done = 0;
        if (ls != NULL) {
            total = de_atomic_load_u64(&ls->bytes_total, DE_ATOMIC_RELAXED);
            done = de_atomic_load_u64(&ls->bytes_done, DE_ATOMIC_RELAXED);
            if (done > total)
                total = done;
        }
        switch (sl->state) {
        case DE_LOAD_READY: {
            u64 w = total != 0 ? total : sl->bytes;
            bytes_total += w;
            bytes_done += w;
            ready++;
            started = true;
            break;
        }
        case DE_LOAD_FAILED:
            bytes_total += total;
            bytes_done += total;
            failed++;
            if (first == DE__LOAD_NONE)
                first = i;
            started = true;
            break;
        default:
            if (sl->state == DE_LOAD_PENDING && ls != NULL && ls->in_flight &&
                de_atomic_load_u32(&ls->started, DE_ATOMIC_ACQUIRE) != 0)
                sl->state = DE_LOAD_LOADING;
            if (sl->state == DE_LOAD_LOADING)
                started = true;
            bytes_total += total;
            bytes_done += done;
            break;
        }
    }
    r->ready = ready;
    r->failed = failed;
    r->bytes_total = bytes_total;
    r->bytes_done = bytes_done;
    if (first != r->first_failed) {
        /* The first failure in request order (an earlier asset may fail later in time). */
        r->first_failed = first;
        if (first != DE__LOAD_NONE) {
            const de__load_entry *e = &r->entries[first];
            const de__slot *sl = e->slot != 0 ? de__slot_get(s, e->slot) : NULL;
            r->first_error = e->slot == 0 ? e->error : sl != NULL ? sl->error : DE_ERR_INTERNAL;
            entry_failure_msg(s, e, r->first_error_msg, sizeof r->first_error_msg);
        }
    }

    if (ready + failed == r->count) {
        de__request_finish(s, r,
                           failed == 0  ? DE_LOAD_READY
                           : ready != 0 ? DE_LOAD_PARTIAL
                                        : DE_LOAD_FAILED);
        return;
    }
    if ((r->flags & DE_LOAD_STOP_ON_ERROR) != 0 && failed != 0) {
        de__request_finish(s, r, ready != 0 ? DE_LOAD_PARTIAL : DE_LOAD_FAILED);
        return;
    }
    r->state = (started || ready + failed != 0) ? DE_LOAD_LOADING : DE_LOAD_PENDING;
    /* Byte-weighted while any size is known, else by finished assets. Monotonic: bytes_total
     * may grow, the reported progress never goes back. Below 1.0 until final. */
    f64 p = bytes_total != 0 ? (f64)bytes_done / (f64)bytes_total
                             : (f64)(ready + failed) / (f64)r->count;
    f32 pf = (f32)p;
    if (pf > 0.999f)
        pf = 0.999f;
    if (pf > r->progress)
        r->progress = pf;
}

/* ---- Status -------------------------------------------------------------------------------- */

de_load_state de_asset_status(de_asset_system *s, de_load_handle h, de_load_info *info) {
    const de__request *r = de__request_get(s, h);
    if (info != NULL) {
        memset(info, 0, sizeof *info);
        if (r != NULL) {
            info->assets_total = r->count;
            info->assets_ready = r->ready;
            info->assets_failed = r->failed;
            info->bytes_total = r->bytes_total;
            info->bytes_done = r->bytes_done;
            info->progress = r->progress;
            if (r->first_failed != DE__LOAD_NONE)
                info->first_failed = r->entries[r->first_failed].id;
            info->first_error = r->first_error;
            memcpy(info->first_error_msg, r->first_error_msg, sizeof info->first_error_msg);
        }
    }
    return r != NULL ? r->state : DE_LOAD_INVALID;
}

de_load_state de_asset_status_of(de_asset_system *s, de_asset_id id) {
    u32 h = de__slot_find(s, id);
    const de__slot *sl = h != 0 ? de__slot_get(s, h) : NULL;
    return sl != NULL ? sl->state : DE_LOAD_INVALID;
}

u32 de_asset_status_list(de_asset_system *s, de_load_handle h, de_asset_status_entry *out,
                         u32 max) {
    const de__request *r = de__request_get(s, h);
    if (r == NULL)
        return 0;
    u32 n = out != NULL ? (max < r->count ? max : r->count) : 0u;
    for (u32 i = 0; i < n; ++i) {
        const de__load_entry *e = &r->entries[i];
        const de__slot *sl = e->slot != 0 ? de__slot_get(s, e->slot) : NULL;
        de_asset_status_entry *o = &out[i];
        o->id = e->id;
        o->state = sl != NULL ? sl->state : DE_LOAD_FAILED;
        o->bytes = 0;
        if (sl != NULL) {
            const de__load_slot *ls = de__load_slot_find(s, e->slot);
            o->bytes = sl->state == DE_LOAD_READY ? sl->bytes
                       : ls != NULL ? de_atomic_load_u64(&ls->bytes_done, DE_ATOMIC_RELAXED)
                                    : 0u;
        }
    }
    return r->count;
}

/* ---- Wait ---------------------------------------------------------------------------------- */

de_load_state de_asset_wait(de_asset_system *s, de_load_handle h, u64 timeout_ns) {
    /* A blocking wait inside a frame-loop system stalls the frame (and with a task system can
     * starve the very workers the loads run on): Debug/Dev assert. */
#if defined(DE_DEBUG) || defined(DE_DEV)
    if (de__ecs_in_schedule())
        DE_PANIC("de_asset_wait called from inside an ECS system (load 0x%08x); poll "
                 "de_asset_status instead",
                 h.id);
#endif
    u64 start = de_time_now_ns();
    u64 deadline = timeout_ns > UINT64_MAX - start ? UINT64_MAX : start + timeout_ns;
    for (;;) {
        de__load_update(s);
        const de__request *r = de__request_get(s, h);
        if (r == NULL)
            return DE_LOAD_INVALID;
        if (r->state >= DE_LOAD_READY)
            return r->state;
        u64 now = de_time_now_ns();
        if (now >= deadline) {
            (void)DE_FAIL(DE_ERR_TIMEOUT,
                          "de_asset_wait: load 0x%08x not finished after %llu ns (%u of %u "
                          "assets done)",
                          h.id, (unsigned long long)timeout_ns, r->ready + r->failed, r->count);
            return r->state;
        }
        /* Help: main-thread tasks (main-thread loaders) when this is the task system's main
         * thread; otherwise sleep briefly while the backend's threads work. */
        u32 ran = 0;
        if (s->tasks != NULL && de_task_worker_index() == 0)
            ran = de_task_pump_main(s->tasks, 0);
        if (ran == 0) {
            u64 left = deadline - now;
            de_sleep_ns(left < DE__WAIT_POLL_NS ? left : DE__WAIT_POLL_NS);
        }
    }
}
