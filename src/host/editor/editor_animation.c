/* DENG-709: CPU-only cooked clip preview. One retained source set, no world entities. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "host/editor/editor_internal.h"
#include "host/editor/editor_animation_pose.h"
#include "host/host_internal.h"
#include "anim/anim_internal.h"
#include "asset/slots_internal.h"
#include <dengine/de_platform.h>
#include <dengine/de_debug.h>
#include <dengine/de_text.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* The retained source set: model parents, clips and skeletons shared by the clips being
 * previewed. Each entry holds one load reference until it idles out (DE__ED_ANIM_IDLE_NS without
 * a request), the IDE disconnects, or a reload or reimport changes cooked data. Busy when full
 * and nothing is idle, like the DIDE mock (DIDE-308). */
#define DE__ED_ANIM_SOURCES 8u
#define DE__ED_ANIM_IDLE_NS 10000000000ull

typedef struct anim_source {
    de_asset_id id;
    de_load_handle load;
    u64 used;
} anim_source;
static struct {
    anim_source held[DE__ED_ANIM_SOURCES];
    u32 count;
    u64 samples;
} preview;

static void drop(de__editor *ed, u32 i) {
    de_asset_cancel(ed->e->assets, preview.held[i].load);
    de_asset_release(ed->e->assets, preview.held[i].load);
    preview.held[i] = preview.held[--preview.count];
    memset(&preview.held[preview.count], 0, sizeof preview.held[0]);
}
static void reset(de__editor *ed) {
    while (preview.count)
        drop(ed, preview.count - 1);
}
void de__ed_animation_invalidate(de__editor *ed) {
    reset(ed);
}
static void idle(de__editor *ed, u64 now) {
    for (u32 i = preview.count; i-- > 0;)
        if (now - preview.held[i].used >= DE__ED_ANIM_IDLE_NS)
            drop(ed, i);
}
static void tick(de__editor *ed) {
    if (preview.count)
        idle(ed, de_time_now_ns());
}
static bool valid_id(de_ep_str s) {
    return s.ptr && s.len && s.len <= 1024 && !memchr(s.ptr, 0, s.len);
}
static const de__slot *ready(de__editor *ed, const char *name, de_asset_id id, const char *type,
                             u64 now, const de_ep_request *req, mpack_writer_t *w) {
    de_asset_system *assets = ed->e->assets;
    u32 handle = de__asset_handle_of(assets, id, NULL);
    const de__slot *slot = handle ? de__slot_get(assets, handle) : NULL;
    if (!slot) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "%s is not a declared asset", name);
        return NULL;
    }
    if (slot->type_hash != de_hash64_str(type)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s must be %s", name, type);
        return NULL;
    }
    u32 at = preview.count;
    for (u32 i = 0; i < preview.count; ++i)
        if (preview.held[i].id.hash == id.hash)
            at = i;
    if (at == preview.count) {
        if (preview.count == DE__ED_ANIM_SOURCES) {
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "animation source set is full");
            return NULL;
        }
        de_load_handle load = de_asset_load(assets, id, 0);
        if (!load.id) {
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "cannot retain animation source");
            return NULL;
        }
        preview.held[preview.count++] = (anim_source){id, load, now};
    }
    preview.held[at].used = now;
    if (slot->state == DE_LOAD_FAILED || slot->state == DE_LOAD_PARTIAL) {
        char why[256] = {0};
        (void)de_asset_last_error(assets, id, why, sizeof why);
        /* Do not keep a dead reference: the next request retries the load. */
        drop(ed, at);
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "animation source failed: %s", why);
        return NULL;
    }
    if (slot->state != DE_LOAD_READY || !de__slot_object(slot)) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "cooked animation source is loading; retry");
        return NULL;
    }
    return slot;
}
static void sample(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(asset_animation_preview, de_ep_animation_preview_params, q);
    if (!ed->e->anim || !ed->e->assets) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "assets.animationPreview");
        return;
    }
    if (!valid_id(q.id) ||
        (q.has_parent &&
         (!valid_id(q.parent) || q.id.len <= q.parent.len + 1 ||
          memcmp(q.id.ptr, q.parent.ptr, q.parent.len) || q.id.ptr[q.parent.len] != '#')) ||
        !isfinite(q.time_seconds) || q.has_clip_version != q.has_skeleton_version) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "invalid animation ID, parent, time or version pair");
        return;
    }
    char clip_name[1025] = {0}, parent_name[1025] = {0};
    memcpy(clip_name, q.id.ptr, q.id.len);
    if (q.has_parent)
        memcpy(parent_name, q.parent.ptr, q.parent.len);
    const u64 now = de_time_now_ns();
    idle(ed, now);
    /* Cooking the parent declares generated children. Do not start a failed child load first. */
    if (q.has_parent &&
        !ready(ed, parent_name, de_asset_id_from(parent_name), "model", now, req, w))
        return;
    const de__slot *clip_slot =
        ready(ed, clip_name, de_asset_id_from(clip_name), "anim_clip", now, req, w);
    if (!clip_slot)
        return;
    const de__anim_asset *clip = de__slot_object(clip_slot);
    if (clip->kind != 2 || !clip->clip.skeleton_id) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "clip has no skeletal binding");
        return;
    }
    de_asset_id skeleton_id = {clip->clip.skeleton_id};
    const de__slot *skeleton_slot = ready(ed, "clip skeleton", skeleton_id, "skeleton", now, req, w);
    if (!skeleton_slot)
        return;
    const de__anim_asset *skeleton = de__slot_object(skeleton_slot);
    if (skeleton->kind != 1) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "invalid skeleton object");
        return;
    }
    const char *name = de_asset_id_path(ed->e->assets, skeleton_id);
    char fallback[32];
    if (!name) {
        snprintf(fallback, sizeof fallback, "0x%016llx", (unsigned long long)skeleton_id.hash);
        name = fallback;
    }
    de_ep_animation_preview result;
    de_result status =
        de__ed_animation_sample(&clip->clip, &skeleton->skeleton, &q, clip_slot->version,
                                skeleton_slot->version, de_ep_cstr(name), a, &result);
    if (status != DE_OK) {
        de__ed_error(w, req->id,
                     status == DE_ERR_OUT_OF_MEMORY ? DE_EP_ERR_INTERNAL
                     : status == DE_ERR_UNSUPPORTED ? DE_EP_ERR_UNSUPPORTED
                                                    : DE_EP_ERR_INVALID_PARAMS,
                     "invalid or oversized cooked animation (%s)", de_result_str(status));
        return;
    }
    ++preview.samples;
    de_ep_write_response_asset_animation_preview(w, req->id, &result);
}
static de_result stats(int argc, const char *const *argv, void *user) {
    (void)argv;
    (void)user;
    if (argc != 1)
        return DE_ERR_INVALID_ARGUMENT;
    DE_INFO(de_log_register_category("editor.animation", DE_LOG_INFO),
            "held=%u samples=%llu worlds=0", preview.count, (unsigned long long)preview.samples);
    return DE_OK;
}
void de__ed_animation_shutdown(de__editor *ed) {
    reset(ed);
    (void)de_command_unregister("editor.animationStats");
    de_error_clear();
}
void de__ed_animation_register(de__editor *ed) {
    memset(&preview, 0, sizeof preview);
    de__ed_on(ed, DE_EP_METHOD_ASSET_ANIMATION_PREVIEW, sample);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
    if (ed->e->anim && ed->e->assets)
        de__ed_add_capability(ed, "assets.animationPreview");
    (void)de_command_register("editor.animationStats", "Read-only animation preview ownership",
                              stats, NULL);
    de_error_clear();
}
