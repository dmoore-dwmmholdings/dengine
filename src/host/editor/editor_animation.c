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

static struct {
    char clip[1025], parent[1025];
    de_asset_id ids[3];
    de_load_handle loads[3];
    u32 count;
    u64 last_request, samples;
} preview;

static void reset(de__editor *ed) {
    for (u32 i = 0; i < preview.count; ++i) {
        de_asset_cancel(ed->e->assets, preview.loads[i]);
        de_asset_release(ed->e->assets, preview.loads[i]);
    }
    memset(&preview, 0, sizeof preview);
}
void de__ed_animation_invalidate(de__editor *ed) {
    reset(ed);
}
static void tick(de__editor *ed) {
    if (preview.count && de_time_now_ns() - preview.last_request >= 5000000000ull)
        reset(ed);
}
static bool valid_id(de_ep_str s) {
    return s.ptr && s.len && s.len <= 1024 && !memchr(s.ptr, 0, s.len);
}
static const de__slot *ready(de__editor *ed, const char *name, de_asset_id id, const char *type,
                             const de_ep_request *req, mpack_writer_t *w) {
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
    bool held = false;
    for (u32 i = 0; i < preview.count; ++i)
        held |= preview.ids[i].hash == id.hash;
    if (!held) {
        if (preview.count == 3) {
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "animation source set is full");
            return NULL;
        }
        de_load_handle load = de_asset_load(assets, id, 0);
        if (!load.id) {
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "cannot retain animation source");
            return NULL;
        }
        preview.ids[preview.count] = id;
        preview.loads[preview.count++] = load;
    }
    if (slot->state == DE_LOAD_FAILED || slot->state == DE_LOAD_PARTIAL) {
        char why[256] = {0};
        (void)de_asset_last_error(assets, id, why, sizeof why);
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
    if (!de_ep_str_eq(q.id, preview.clip) ||
        (q.has_parent ? !de_ep_str_eq(q.parent, preview.parent) : preview.parent[0] != 0)) {
        reset(ed);
        memcpy(preview.clip, q.id.ptr, q.id.len);
        if (q.has_parent)
            memcpy(preview.parent, q.parent.ptr, q.parent.len);
    }
    preview.last_request = de_time_now_ns();
    /* Cooking the parent declares generated children. Do not start a failed child load first. */
    if (q.has_parent &&
        !ready(ed, preview.parent, de_asset_id_from(preview.parent), "model", req, w))
        return;
    const de__slot *clip_slot =
        ready(ed, preview.clip, de_asset_id_from(preview.clip), "anim_clip", req, w);
    if (!clip_slot)
        return;
    const de__anim_asset *clip = de__slot_object(clip_slot);
    if (clip->kind != 2 || !clip->clip.skeleton_id) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "clip has no skeletal binding");
        return;
    }
    de_asset_id skeleton_id = {clip->clip.skeleton_id};
    const de__slot *skeleton_slot = ready(ed, "clip skeleton", skeleton_id, "skeleton", req, w);
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
