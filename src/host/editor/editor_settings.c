/* DENG-696: one detached, cancellable before/after cook; never waits on the frame thread. */
#include "editor_internal.h"
#include "editor_settings_image.h"
#include "editor_settings_value.h"
#include "editor_preview_image.h"
#include "asset/asset_internal.h"
#include "asset/cook_preview_internal.h"
#include "audio/sound_format.h"
#include "render/formats/de_cooked_model.h"
#include "render/formats/de_cooked_mesh.h"
#include "platform/de_atomic.h"
#include <dengine/de_debug.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_BUDGET  ((size_t)256 << 20)
#define SETTINGS_INPUT   ((size_t)256 << 10)
#define SETTINGS_ARENA   ((size_t)1 << 20)
#define SETTINGS_TIMEOUT (30ull * 1000000000ull)
typedef struct settings_job {
    de_allocator allocator;
    de_atomic_u64 live, peak;
    de_atomic_u32 cancelled, done;
    u64 deadline;
    char root[DE_FS_PATH_MAX], id[1024], locale[36];
    char *params;
    size_t param_bytes;
    de_result result;
    char error[512];
    de_ep_asset_preview response;
    u32 triangles[2][8];
    char detail[2][256];
    u8 *png[3];
    size_t png_bytes[3], png_capacity[3];
} settings_job;
static struct {
    settings_job *job;
    de_thread *thread;
    u64 request, revision, accepted_revision, last_peak;
    u32 epoch;
    bool pending;
} g_settings;

static void *budget_alloc(void *user, size_t size, size_t align, u32 tag) {
    settings_job *job = user;
    const u64 live = de_atomic_load_u64(&job->live, DE_ATOMIC_RELAXED);
    if (de_atomic_load_u32(&job->cancelled, DE_ATOMIC_ACQUIRE) ||
        de_time_now_ns() >= job->deadline) {
        (void)DE_FAIL(DE_ERR_CANCELLED, "settings preview cancelled or timed out");
        return NULL;
    }
    if (size > SETTINGS_BUDGET || live > SETTINGS_BUDGET - size) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                      "settings preview exceeds its 256 MiB allocation budget");
        return NULL;
    }
    void *p = DE_ALLOC(de_heap_allocator(), size, align, tag);
    if (p) {
        de_atomic_store_u64(&job->live, live + size, DE_ATOMIC_RELEASE);
        if (live + size > de_atomic_load_u64(&job->peak, DE_ATOMIC_RELAXED))
            de_atomic_store_u64(&job->peak, live + size, DE_ATOMIC_RELEASE);
    }
    return p;
}
static void budget_free(void *user, void *ptr, size_t size) {
    if (!ptr)
        return;
    settings_job *job = user;
    DE_FREE(de_heap_allocator(), ptr, size);
    const u64 live = de_atomic_load_u64(&job->live, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&job->live, live - size, DE_ATOMIC_RELEASE);
}
static void *budget_realloc(void *user, void *ptr, size_t old_size, size_t size, size_t align,
                            u32 tag) {
    void *p = budget_alloc(user, size, align, tag);
    if (p && ptr) {
        memcpy(p, ptr, old_size < size ? old_size : size);
        budget_free(user, ptr, old_size);
    }
    return p;
}
static void job_free(settings_job *job) {
    if (!job)
        return;
    for (u32 i = 0; i < 3; ++i)
        if (job->png[i])
            budget_free(job, job->png[i], job->png_capacity[i]);
    DE_FREE(de_heap_allocator(), job->params, job->param_bytes);
    DE_FREE(de_heap_allocator(), job, sizeof *job);
}
static de_result make_png(settings_job *job, u32 index, u32 width, u32 height, const u8 *pixels) {
    const size_t size = de__ed_preview_png_size(width, height);
    job->png[index] = DE_ALLOC(&job->allocator, size, 16, DE_MEM_ASSET);
    if (!job->png[index])
        return DE_ERR_OUT_OF_MEMORY;
    job->png_capacity[index] = size;
    return de__ed_preview_png(width, height, pixels, job->png[index], size, &job->png_bytes[index]);
}
static de_result texture_result(settings_job *job, const de__cook_preview *preview) {
    u8 probe[4];
    u32 width[2], height[2];
    const char *format[2];
    const de__cook_preview_side *sides[] = {&preview->before, &preview->after};
    for (u32 i = 0; i < 2; ++i)
        DE_TRY(de__ed_settings_pixels(sides[i]->data, sides[i]->size, 1, 1, probe, &width[i],
                                      &height[i], &format[i]));
    const u32 longest = width[0] > height[0] ? width[0] : height[0];
    u32 w = longest > 256 ? (u32)((u64)width[0] * 256 / longest) : width[0];
    u32 h = longest > 256 ? (u32)((u64)height[0] * 256 / longest) : height[0];
    if (!w)
        w = 1;
    if (!h)
        h = 1;
    const size_t size = (size_t)w * h * 4;
    u8 *pixels = DE_ALLOC(&job->allocator, size * 3, 16, DE_MEM_ASSET);
    if (!pixels)
        return DE_ERR_OUT_OF_MEMORY;
    de_result result = DE_OK;
    for (u32 i = 0; i < 2 && result == DE_OK; ++i) {
        result = de__ed_settings_pixels(sides[i]->data, sides[i]->size, w, h, pixels + size * i,
                                        &width[i], &height[i], &format[i]);
        snprintf(job->detail[i], sizeof job->detail[i], "%s; %ux%u; mip0/layer0; diff x8",
                 format[i], width[i], height[i]);
    }
    for (size_t i = 0; result == DE_OK && i < size; i += 4) {
        u32 alpha =
            (u32)(pixels[i + 3] > pixels[size + i + 3] ? pixels[i + 3] - pixels[size + i + 3]
                                                       : pixels[size + i + 3] - pixels[i + 3]);
        for (u32 c = 0; c < 3; ++c) {
            u32 d =
                (u32)(pixels[i + c] > pixels[size + i + c] ? pixels[i + c] - pixels[size + i + c]
                                                           : pixels[size + i + c] - pixels[i + c]);
            if (alpha > d)
                d = alpha;
            d *= 8;
            pixels[size * 2 + i + c] = (u8)(d > 255 ? 255 : d);
        }
        pixels[size * 2 + i + 3] = 255;
    }
    for (u32 i = 0; i < 3 && result == DE_OK; ++i)
        result = make_png(job, i, w, h, pixels + size * i);
    DE_FREE(&job->allocator, pixels, size * 3);
    return result;
}
static de_result model_result(settings_job *job, const de__cook_preview_side *side, u32 index) {
    de_cooked_model_view model;
    const char *why = NULL;
    if (!de_cooked_model_parse(side->data, side->size, &model, &why) || model.mesh_count > 1024)
        return DE_ERR_PARSE;
    u32 lod_count = 0, meshes = 0;
    for (u32 i = 0; i < side->child_count; ++i) {
        de__cook_preview_child child;
        if (!de__asset_cook_preview_child(side, i, &child))
            return DE_ERR_PARSE;
        if (strcmp(child.type, "mesh"))
            continue;
        de_cooked_mesh_view mesh;
        if (!de_cooked_mesh_parse(child.data, child.size, &mesh, &why) || mesh.lod_count > 8)
            return DE_ERR_PARSE;
        if (mesh.lod_count > lod_count)
            lod_count = mesh.lod_count;
        ++meshes;
        for (u32 l = 0; l < 8; ++l) {
            u32 at = l < mesh.lod_count ? l : mesh.lod_count - 1;
            u32 n = de_cooked_mesh_get_lod(&mesh, at).triangle_count;
            if (n > UINT32_MAX - job->triangles[index][l])
                return DE_ERR_UNSUPPORTED;
            job->triangles[index][l] += n;
        }
    }
    if (meshes != model.mesh_count)
        return DE_ERR_PARSE;
    de_ep_asset_preview_side *wire = index ? &job->response.after : &job->response.before;
    wire->has_triangles = true;
    wire->triangles = (de_ep_list_u32){job->triangles[index], lod_count};
    snprintf(job->detail[index], sizeof job->detail[index],
             "%u meshes; summed triangles; short LOD chains clamp", meshes);
    return DE_OK;
}
static de_result summaries(settings_job *job, const de__cook_preview *preview, const char *type) {
    const de__cook_preview_side *sides[] = {&preview->before, &preview->after};
    if (!strcmp(type, "texture"))
        DE_TRY(texture_result(job, preview));
    for (u32 i = 0; i < 2; ++i) {
        de_ep_asset_preview_side *wire = i ? &job->response.after : &job->response.before;
        if (sides[i]->total_bytes > (512u << 20))
            return DE_ERR_UNSUPPORTED;
        wire->bytes = sides[i]->total_bytes;
        if (!strcmp(type, "model"))
            DE_TRY(model_result(job, sides[i], i));
        else if (!strcmp(type, "sound")) {
            de__sound_view sound;
            DE_TRY(de__sound_parse(sides[i]->data, sides[i]->size, &sound));
            const char *codecs[] = {"", "PCM16", "PCM float32", "ADPCM", "Vorbis", "Opus"};
            snprintf(job->detail[i], sizeof job->detail[i], "%s; %u Hz; %u channels; %s",
                     codecs[sound.codec], sound.rate, sound.channels,
                     sound.flags & DE__DSND_STREAM ? "streamed" : "resident");
        }
        wire->has_detail = true;
        wire->detail = de_ep_cstr(job->detail[i]);
    }
    return DE_OK;
}
static i32 cook(void *user) {
    settings_job *job = user;
    de_fp_env_init_thread();
    de_crash_thread_init();
    de_type_registry *types = NULL;
    de_world *world = NULL;
    de_asset_system *assets = NULL;
    de__cook_preview preview = {0};
    de__manifest_kv *settings = NULL;
    u8 *storage = DE_ALLOC(&job->allocator, SETTINGS_ARENA, 16, DE_MEM_ASSET);
    de_result result = storage ? DE_OK : DE_ERR_OUT_OF_MEMORY;
    de_ep_arena arena = {storage, SETTINGS_ARENA, 0};
    de_ep_asset_preview_params params = {0};
    if (result == DE_OK) {
        de_ep_request req = {.params = {job->params, job->param_bytes}};
        if (!de_ep_read_params_asset_preview_settings(&req, &arena, &params))
            result = DE_ERR_INVALID_ARGUMENT;
    }
    if (result == DE_OK)
        result = de__ed_settings_values(&params.settings, &arena, &settings);
    de_cook_platform platform = DE_COOK_PLATFORM_HOST;
    if (params.has_platform)
        platform = de_ep_str_eq(params.platform, "windows") ? DE_COOK_PLATFORM_WINDOWS
                   : de_ep_str_eq(params.platform, "macos") ? DE_COOK_PLATFORM_MACOS
                                                            : DE_COOK_PLATFORM_LINUX;
    if (result == DE_OK)
        result = de_type_registry_create(&job->allocator, &types);
    if (result == DE_OK)
        result =
            de_world_create(&(de_world_desc){.allocator = &job->allocator, .types = types}, &world);
    if (result == DE_OK)
        result = de_render_register_components(world);
    if (result == DE_OK)
        result = de_asset_system_create(&(de_asset_system_desc){.allocator = &job->allocator,
                                                                .types = types,
                                                                .locale = job->locale,
                                                                .staging_ring_bytes = 1u << 20},
                                        &assets);
    if (result == DE_OK)
        result = de_asset_manifest_load(assets, job->root);
    if (result == DE_OK)
        result = de__asset_cook_preview(assets, job->id, settings, params.settings.count, platform,
                                        &preview);
    if (result == DE_OK) {
        const de__manifest_asset *entry = de__manifest_find_asset(assets->manifest, job->id);
        result = entry ? summaries(job, &preview, entry->type) : DE_ERR_NOT_FOUND;
    }
    if (result == DE_OK)
        result = de__asset_cook_preview_validate(&preview);
    const de_error *error = de_error_last();
    snprintf(job->error, sizeof job->error, "%s",
             error && error->result == result ? error->message : de_result_str(result));
    de__asset_cook_preview_free(&preview);
    de_asset_system_destroy(assets);
    de_world_destroy(world);
    de_type_registry_destroy(types);
    if (storage)
        DE_FREE(&job->allocator, storage, SETTINGS_ARENA);
    job->result = result;
    de_atomic_store_u32(&job->done, 1, DE_ATOMIC_RELEASE);
    return 0;
}
static void reset(de__editor *ed) {
    (void)ed;
    g_settings.pending = false;
    if (g_settings.job)
        de_atomic_store_u32(&g_settings.job->cancelled, 1, DE_ATOMIC_RELEASE);
}
void de__ed_settings_invalidate(void) {
    ++g_settings.revision;
}
static de_ep_error error_code(de_result result) {
    return result == DE_ERR_INVALID_ARGUMENT                     ? DE_EP_ERR_INVALID_PARAMS
           : result == DE_ERR_NOT_FOUND                          ? DE_EP_ERR_NOT_FOUND
           : result == DE_ERR_UNSUPPORTED                        ? DE_EP_ERR_UNSUPPORTED
           : result == DE_ERR_BUSY || result == DE_ERR_CANCELLED ? DE_EP_ERR_BUSY
                                                                 : DE_EP_ERR_INTERNAL;
}
void de__ed_settings_pump(de__editor *ed) {
    settings_job *job = g_settings.job;
    if (!job)
        return;
    if (!de_atomic_load_u32(&job->done, DE_ATOMIC_ACQUIRE)) {
        if (g_settings.pending && de_time_now_ns() >= job->deadline) {
            const u64 request = g_settings.request;
            reset(ed);
            if (ed->connected && ed->authed) {
                mpack_writer_t w;
                de__ed_respond_begin(ed, &w);
                de__ed_error(&w, request, DE_EP_ERR_INTERNAL, "settings preview timed out");
                (void)de__ed_send(ed, &w, DE_EP_KIND_RESPONSE);
            }
        }
        return;
    }
    (void)de_thread_join(g_settings.thread);
    g_settings.thread = NULL;
    const bool respond =
        g_settings.pending && g_settings.epoch == ed->epoch && ed->connected && ed->authed;
    const u64 request = g_settings.request;
    if (g_settings.accepted_revision != g_settings.revision) {
        job->result = DE_ERR_BUSY;
        snprintf(job->error, sizeof job->error, "source changed; retry");
    }
    g_settings.job = NULL;
    g_settings.pending = false;
    g_settings.last_peak = de_atomic_load_u64(&job->peak, DE_ATOMIC_ACQUIRE);
    if (respond) {
        if (job->result == DE_OK) {
            for (u32 i = 0; i < 3 && ed->connected; ++i)
                if (job->png[i]) {
                    u64 blob = de__ed_send_blob(ed, job->png[i], job->png_bytes[i]);
                    if (i == 0) {
                        job->response.before.has_blob_id = blob != 0;
                        job->response.before.blob_id = blob;
                    } else if (i == 1) {
                        job->response.after.has_blob_id = blob != 0;
                        job->response.after.blob_id = blob;
                    } else {
                        job->response.has_diff_blob_id = blob != 0;
                        job->response.diff_blob_id = blob;
                    }
                }
            if (ed->connected) {
                mpack_writer_t w;
                de__ed_respond_begin(ed, &w);
                de_ep_write_response_asset_preview_settings(&w, request, &job->response);
                (void)de__ed_send(ed, &w, DE_EP_KIND_RESPONSE);
            }
        } else {
            mpack_writer_t w;
            de__ed_respond_begin(ed, &w);
            de__ed_error(&w, request, error_code(job->result), "settings preview: %s", job->error);
            (void)de__ed_send(ed, &w, DE_EP_KIND_RESPONSE);
        }
    }
    job_free(job);
}
static void on_preview(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    DE__ED_PARAMS(asset_preview_settings, de_ep_asset_preview_params, q);
#if !DE_HAS_COOKER
    (void)q;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "settings previews require the Dev cooker");
    return;
#endif
    if (!ed->e->assets || !ed->e->assets->manifest ||
        ed->e->assets->desc.mode == DE_ASSET_MODE_PACKED) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "settings previews require source assets");
        return;
    }
    if (!q.id.len || q.id.len >= 1024 || memchr(q.id.ptr, 0, q.id.len) || q.settings.count > 256 ||
        req->params.len > SETTINGS_INPUT ||
        (q.has_platform && !de_ep_str_eq(q.platform, "windows") &&
         !de_ep_str_eq(q.platform, "macos") && !de_ep_str_eq(q.platform, "linux"))) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "invalid bounded settings preview parameters");
        return;
    }
    if (g_settings.job) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "one settings preview worker is active");
        return;
    }
    char id[1024];
    memcpy(id, q.id.ptr, q.id.len);
    id[q.id.len] = 0;
    const de__manifest_asset *entry = de__manifest_find_asset(ed->e->assets->manifest, id);
    if (!entry) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "asset not found");
        return;
    }
    if (strcmp(entry->type, "texture") && strcmp(entry->type, "model") &&
        strcmp(entry->type, "sound")) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no importer preview for this type");
        return;
    }
    settings_job *job = DE_NEW(de_heap_allocator(), settings_job, DE_MEM_ASSET);
    if (!job) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "settings preview allocation failed");
        return;
    }
    memset(job, 0, sizeof *job);
    job->deadline = de_time_now_ns() + SETTINGS_TIMEOUT;
    job->allocator = (de_allocator){budget_alloc, budget_realloc, budget_free, job};
    snprintf(job->root, sizeof job->root, "%s", ed->e->assets->manifest->root);
    snprintf(job->id, sizeof job->id, "%s", id);
    snprintf(job->locale, sizeof job->locale, "%s", ed->e->assets->locale_buf);
    job->param_bytes = req->params.len;
    job->params = DE_ALLOC(de_heap_allocator(), job->param_bytes, 16, DE_MEM_ASSET);
    if (!job->params) {
        job_free(job);
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "settings preview input allocation failed");
        return;
    }
    memcpy(job->params, req->params.ptr, job->param_bytes);
    de_thread *thread = NULL;
    de_result result =
        de_thread_create(cook, job, "de-settings-preview", DE_THREAD_NORMAL, 4u << 20, &thread);
    if (result != DE_OK) {
        job_free(job);
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "settings preview worker failed");
        return;
    }
    g_settings.job = job;
    g_settings.thread = thread;
    g_settings.request = req->id;
    g_settings.epoch = ed->epoch;
    g_settings.accepted_revision = g_settings.revision;
    g_settings.pending = true;
    de__ed_defer(ed);
}
static de_result stats(int argc, const char *const *argv, void *user) {
    (void)argv;
    (void)user;
    if (argc != 1)
        return DE_ERR_INVALID_ARGUMENT;
    settings_job *job = g_settings.job;
    const u64 live = job ? de_atomic_load_u64(&job->live, DE_ATOMIC_ACQUIRE) : 0;
    const u64 peak = job ? de_atomic_load_u64(&job->peak, DE_ATOMIC_ACQUIRE) : g_settings.last_peak;
    DE_INFO(de_log_register_category("editor.settings", DE_LOG_INFO),
            "workers=%u pending=%u live_bytes=%llu peak_bytes=%llu", job ? 1u : 0u,
            g_settings.pending ? 1u : 0u, (unsigned long long)live, (unsigned long long)peak);
    return DE_OK;
}
void de__ed_settings_register(de__editor *ed) {
    memset(&g_settings, 0, sizeof g_settings);
    de__ed_on(ed, DE_EP_METHOD_ASSET_PREVIEW_SETTINGS, on_preview);
    de__ed_on_reset(ed, reset);
    (void)de_command_register("editor.settingsPreviewStats",
                              "Report settings preview worker allocation ownership", stats, NULL);
    de_error_clear();
}
void de__ed_settings_shutdown(de__editor *ed) {
    reset(ed);
    if (g_settings.thread) {
        (void)de_thread_join(g_settings.thread);
        g_settings.thread = NULL;
    }
    job_free(g_settings.job);
    g_settings.job = NULL;
    (void)de_command_unregister("editor.settingsPreviewStats");
    de_error_clear();
}
