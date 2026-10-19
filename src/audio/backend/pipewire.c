#include "audio/audio_internal.h"
#include "audio/backend/linux_dl.h"
#include "core/de_compiler.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* These public PipeWire method macros use GNU expressions. Confine compatibility diagnostics
 * to the four SDK call wrappers; all backend implementation remains pedantic C11. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-statement-expression-from-macro-expansion"
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
static int core_listen(struct pw_core *c, struct spa_hook *h, const struct pw_core_events *e,
                       void *user) {
    return pw_core_add_listener(c, h, e, user);
}
static int core_sync(struct pw_core *c) {
    return pw_core_sync(c, PW_ID_CORE, 0);
}
static int registry_listen(struct pw_registry *r, struct spa_hook *h,
                           const struct pw_registry_events *e, void *user) {
    return pw_registry_add_listener(r, h, e, user);
}
static int metadata_listen(struct pw_metadata *m, struct spa_hook *h,
                           const struct pw_metadata_events *e, void *user) {
    return pw_metadata_add_listener(m, h, e, user);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#define MAX_NODES 128u
typedef struct pw_device {
    u32 id;
    de_audio_device_info info;
} pw_device;
typedef struct pipewire_backend {
    de_allocator alloc;
    de__audio_backend_desc desc;
    de__pw_api api;
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_metadata *metadata;
    struct spa_hook core_listener, registry_listener, metadata_listener;
    pw_device nodes[MAX_NODES];
    u32 count, metadata_id;
    char default_name[128];
    int sync;
    de_atomic_u32 connected, failed;
    bool loop_started, initialized;
} pipewire_backend;
typedef struct pipewire_stream {
    de__audio_stream base;
    pipewire_backend *backend;
    de__audio_stream_desc desc;
    struct pw_stream *stream;
    struct spa_hook listener;
    de_atomic_u32 closing, callbacks;
} pipewire_stream;
static void post(pipewire_backend *b, u32 events) {
    if (b->desc.notify)
        b->desc.notify(b->desc.notify_user, events);
}
static void done(void *user, u32 id, int seq) {
    pipewire_backend *b = user;
    if (id == PW_ID_CORE && seq == b->sync)
        de_atomic_store_u32(&b->connected, 1, DE_ATOMIC_RELEASE);
}
static void error(void *user, u32 id, int seq, int result, const char *message) {
    (void)id;
    (void)seq;
    (void)result;
    (void)message;
    pipewire_backend *b = user;
    de_atomic_store_u32(&b->failed, 1, DE_ATOMIC_RELEASE);
    post(b, DE__AUDIO_EV_STREAM_LOST);
}
static int metadata_property(void *user, u32 subject, const char *key, const char *type,
                             const char *value) {
    (void)subject;
    (void)type;
    pipewire_backend *b = user;
    if (key && strcmp(key, "default.audio.sink") != 0)
        return 0;
    char name[128] = {0};
    /* PipeWire default metadata is JSON {"name":"node.name"}. Node names have no escapes. */
    const char *p = value ? strstr(value, "\"name\"") : NULL;
    if (p && (p = strchr(p + 6, ':')) && (p = strchr(p + 1, '"'))) {
        const char *end = strchr(++p, '"');
        if (end && end - p < (ptrdiff_t)sizeof name && !memchr(p, '\\', (size_t)(end - p)))
            memcpy(name, p, (size_t)(end - p));
    }
    if (strcmp(name, b->default_name)) {
        memcpy(b->default_name, name, sizeof name);
        post(b, DE__AUDIO_EV_DEFAULT_CHANGED);
    }
    return 0;
}
static const struct pw_metadata_events metadata_events = {.version = PW_VERSION_METADATA_EVENTS,
                                                          .property = metadata_property};
static void global(void *user, u32 id, u32 permissions, const char *type, u32 version,
                   const struct spa_dict *props) {
    (void)permissions;
    pipewire_backend *b = user;
    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : NULL;
        if (name && strcmp(name, "default") == 0 && !b->metadata) {
            b->metadata =
                pw_registry_bind(b->registry, id, type,
                                 version < PW_VERSION_METADATA ? version : PW_VERSION_METADATA, 0);
            if (b->metadata) {
                b->metadata_id = id;
                metadata_listen(b->metadata, &b->metadata_listener, &metadata_events, b);
            }
        }
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props || b->count == MAX_NODES)
        return;
    const char *media = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!media || strcmp(media, "Audio/Sink") || !name || strlen(name) >= 128)
        return;
    pw_device *node = &b->nodes[b->count++];
    memset(node, 0, sizeof *node);
    node->id = id;
    snprintf(node->info.id, sizeof node->info.id, "%s", name);
    const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    snprintf(node->info.name, sizeof node->info.name, "%s", description ? description : name);
    const char *channels = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNELS);
    node->info.channels = channels ? (u32)strtoul(channels, NULL, 10) : 2;
    post(b, DE__AUDIO_EV_DEVICES_CHANGED);
}
static void global_remove(void *user, u32 id) {
    pipewire_backend *b = user;
    if (b->metadata && id == b->metadata_id) {
        spa_hook_remove(&b->metadata_listener);
        b->api.pw_proxy_destroy((struct pw_proxy *)b->metadata);
        b->metadata = NULL;
        b->default_name[0] = 0;
        post(b, DE__AUDIO_EV_DEFAULT_CHANGED);
    }
    for (u32 i = 0; i < b->count; ++i)
        if (b->nodes[i].id == id) {
            memmove(b->nodes + i, b->nodes + i + 1, (--b->count - i) * sizeof b->nodes[0]);
            post(b, DE__AUDIO_EV_DEVICES_CHANGED | DE__AUDIO_EV_STREAM_LOST);
            break;
        }
}
static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS, .done = done, .error = error};
static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS, .global = global, .global_remove = global_remove};
static void destroy(void *backend) {
    pipewire_backend *b = backend;
    if (b->loop_started)
        b->api.pw_thread_loop_stop(b->loop);
    if (b->metadata) {
        spa_hook_remove(&b->metadata_listener);
        b->api.pw_proxy_destroy((struct pw_proxy *)b->metadata);
    }
    if (b->registry) {
        spa_hook_remove(&b->registry_listener);
        b->api.pw_proxy_destroy((struct pw_proxy *)b->registry);
    }
    if (b->core) {
        spa_hook_remove(&b->core_listener);
        b->api.pw_core_disconnect(b->core);
    }
    if (b->context)
        b->api.pw_context_destroy(b->context);
    if (b->loop)
        b->api.pw_thread_loop_destroy(b->loop);
    if (b->initialized)
        b->api.pw_deinit();
    de__linux_unload(b->api.library);
    DE_FREE(&b->alloc, b, sizeof *b);
}
static de_result create(const de__audio_backend_desc *desc, void **out) {
    *out = NULL;
    pipewire_backend *b = DE_NEW(desc->allocator, pipewire_backend, DE_MEM_AUDIO);
    if (!b)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PipeWire backend");
    memset(b, 0, sizeof *b);
    b->alloc = *desc->allocator;
    b->desc = *desc;
    de_atomic_init_u32(&b->connected, 0);
    de_atomic_init_u32(&b->failed, 0);
    if (!de__pw_load(&b->api)) {
        destroy(b);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "PipeWire library unavailable");
    }
    b->api.pw_init(NULL, NULL);
    b->initialized = true;
    b->loop = b->api.pw_thread_loop_new("de-pipewire", NULL);
    if (b->loop)
        b->context = b->api.pw_context_new(b->api.pw_thread_loop_get_loop(b->loop), NULL, 0);
    if (b->context)
        b->core = b->api.pw_context_connect(b->context, NULL, 0);
    if (b->core) {
        core_listen(b->core, &b->core_listener, &core_events, b);
        b->registry = pw_core_get_registry(b->core, PW_VERSION_REGISTRY, 0);
        if (b->registry) {
            registry_listen(b->registry, &b->registry_listener, &registry_events, b);
            b->sync = core_sync(b->core);
            b->loop_started = b->api.pw_thread_loop_start(b->loop) >= 0;
        }
    }
    u64 end = de_time_now_ns() + 200000000ull;
    while (b->loop_started && !de_atomic_load_u32(&b->connected, DE_ATOMIC_ACQUIRE) &&
           !de_atomic_load_u32(&b->failed, DE_ATOMIC_ACQUIRE) && de_time_now_ns() < end)
        de_sleep_ns(1000000);
    if (!de_atomic_load_u32(&b->connected, DE_ATOMIC_ACQUIRE) ||
        de_atomic_load_u32(&b->failed, DE_ATOMIC_ACQUIRE)) {
        destroy(b);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "PipeWire daemon did not connect within 200ms");
    }
    *out = b;
    return DE_OK;
}
static u32 enumerate(void *backend, de_audio_device_info *out, u32 max) {
    pipewire_backend *b = backend;
    b->api.pw_thread_loop_lock(b->loop);
    u32 count = b->count;
    for (u32 i = 0; out && i < count && i < max; ++i) {
        out[i] = b->nodes[i].info;
        out[i].is_default = strcmp(out[i].id, b->default_name) == 0;
    }
    b->api.pw_thread_loop_unlock(b->loop);
    return count;
}
static void state_changed(void *user, enum pw_stream_state old, enum pw_stream_state state,
                          const char *message) {
    (void)old;
    (void)message;
    pipewire_stream *s = user;
    if (state == PW_STREAM_STATE_ERROR && !de_atomic_load_u32(&s->closing, DE_ATOMIC_ACQUIRE))
        post(s->backend, DE__AUDIO_EV_STREAM_LOST);
}
static void process(void *user) {
    pipewire_stream *s = user;
    static DE_THREAD_LOCAL bool fp_initialized;
    if (!fp_initialized) {
        de_fp_env_init_thread();
        fp_initialized = true;
    }
    de_atomic_fetch_add_u32(&s->callbacks, 1, DE_ATOMIC_ACQ_REL);
    de__rt_guard_enter();
    struct pw_buffer *buffer = s->backend->api.pw_stream_dequeue_buffer(s->stream);
    if (buffer && buffer->buffer && buffer->buffer->n_datas == 1) {
        struct spa_data *data = &buffer->buffer->datas[0];
        u32 stride = s->desc.channels * sizeof(f32), frames = data->maxsize / stride;
        if (buffer->requested && frames > buffer->requested)
            frames = (u32)buffer->requested;
        if (data->data && data->chunk) {
            if (de_atomic_load_u32(&s->closing, DE_ATOMIC_ACQUIRE))
                memset(data->data, 0, (size_t)frames * stride);
            else
                s->desc.render(s->desc.user, data->data, frames);
            data->chunk->offset = 0;
            data->chunk->stride = (int32_t)stride;
            data->chunk->size = frames * stride;
            buffer->size = frames;
        }
    }
    if (buffer)
        s->backend->api.pw_stream_queue_buffer(s->stream, buffer);
    de__rt_guard_leave();
    de_atomic_fetch_sub_u32(&s->callbacks, 1, DE_ATOMIC_RELEASE);
}
static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS, .state_changed = state_changed, .process = process};
static void stop(de__audio_stream *base) {
    pipewire_stream *s = (pipewire_stream *)base;
    pipewire_backend *b = s->backend;
    de_atomic_store_u32(&s->closing, 1, DE_ATOMIC_RELEASE);
    if (s->stream) {
        b->api.pw_thread_loop_lock(b->loop);
        b->api.pw_stream_set_active(s->stream, false);
        b->api.pw_stream_disconnect(s->stream);
        b->api.pw_thread_loop_unlock(b->loop);
    }
    while (de_atomic_load_u32(&s->callbacks, DE_ATOMIC_ACQUIRE))
        de_sleep_ns(100000);
}
static void close_stream(de__audio_stream *base) {
    pipewire_stream *s = (pipewire_stream *)base;
    pipewire_backend *b = s->backend;
    stop(base);
    b->api.pw_thread_loop_lock(b->loop);
    if (s->stream) {
        spa_hook_remove(&s->listener);
        b->api.pw_stream_destroy(s->stream);
    }
    b->api.pw_thread_loop_unlock(b->loop);
    DE_FREE(&b->alloc, s, sizeof *s);
}
static de_result open_stream(void *backend, const char *id, const de__audio_stream_desc *desc,
                             de__audio_stream **out) {
    *out = NULL;
    pipewire_backend *b = backend;
    if (!desc || !desc->render || !desc->channels || desc->channels > 8 || !desc->sample_rate)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid PipeWire format");
    pipewire_stream *s = DE_NEW(&b->alloc, pipewire_stream, DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PipeWire stream");
    memset(s, 0, sizeof *s);
    s->base.vt = &de__audio_backend_pipewire;
    s->base.backend = b;
    s->backend = b;
    s->desc = *desc;
    de_atomic_init_u32(&s->closing, 0);
    de_atomic_init_u32(&s->callbacks, 0);
    b->api.pw_thread_loop_lock(b->loop);
    struct pw_properties *props =
        b->api.pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                 PW_KEY_MEDIA_ROLE, "Game", NULL);
    char latency[64];
    snprintf(latency, sizeof latency, "%u/%u", desc->block_frames, desc->sample_rate);
    if (props) {
        b->api.pw_properties_set(props, PW_KEY_NODE_LATENCY, latency);
        if (id)
            b->api.pw_properties_set(props, PW_KEY_TARGET_OBJECT, id);
    }
    s->stream = props ? b->api.pw_stream_new(b->core, "DEngine", props) : NULL;
    int error = -ENOMEM;
    if (s->stream) {
        b->api.pw_stream_add_listener(s->stream, &s->listener, &stream_events, s);
        u8 storage[1024];
        struct spa_pod_builder builder = {0};
        spa_pod_builder_init(&builder, storage, sizeof storage);
        struct spa_audio_info_raw format = {0};
        format.format = SPA_AUDIO_FORMAT_F32;
        format.rate = desc->sample_rate;
        format.channels = desc->channels;
        static const enum spa_audio_channel positions[8] = {
            SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
            SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR};
        if (desc->channels == 1)
            format.position[0] = SPA_AUDIO_CHANNEL_MONO;
        else
            for (u32 i = 0; i < desc->channels; ++i)
                format.position[i] = positions[i];
        const struct spa_pod *params[] = {
            spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
        error = b->api.pw_stream_connect(s->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                         PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                             PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE,
                                         params, 1);
    }
    b->api.pw_thread_loop_unlock(b->loop);
    if (error < 0) {
        close_stream(&s->base);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "PipeWire stream connect failed (%d)", error);
    }
    *out = &s->base;
    return DE_OK;
}
static de_result start(de__audio_stream *base) {
    pipewire_stream *s = (pipewire_stream *)base;
    pipewire_backend *b = s->backend;
    b->api.pw_thread_loop_lock(b->loop);
    int error = b->api.pw_stream_set_active(s->stream, true);
    b->api.pw_thread_loop_unlock(b->loop);
    return error < 0 ? DE_FAIL(DE_ERR_IO, "PipeWire stream start failed (%d)", error) : DE_OK;
}
const de__audio_backend_vtbl de__audio_backend_pipewire = {
    "pipewire", DE_AUDIO_PIPEWIRE, create, destroy, enumerate, open_stream, start,
    stop,       close_stream,      NULL};
