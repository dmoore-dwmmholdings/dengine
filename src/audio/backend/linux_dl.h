#ifndef DE_AUDIO_LINUX_DL_H
#define DE_AUDIO_LINUX_DL_H
#include "audio/backend.h"
typedef struct de__linux_symbol {
    const char *name;
    void *slot;
    size_t bytes;
} de__linux_symbol;
bool de__linux_load(const char *path, const de__linux_symbol *symbols, u32 count, void **handle);
void de__linux_unload(void *handle);
void de__linux_test_missing(u32 mask); /*1 PipeWire,2 ALSA; only changes future loads */
#if defined(__linux__) && DE_AUDIO_HAS_ALSA
#include <alsa/asoundlib.h>
#define DE__ALSA_SYMBOLS(X)                                                                        \
    X(snd_pcm_open)                                                                                \
    X(snd_pcm_close)                                                                               \
    X(snd_pcm_hw_params_malloc)                                                                    \
    X(snd_pcm_hw_params_free)                                                                      \
    X(snd_pcm_hw_params_any) X(snd_pcm_hw_params_set_access) X(snd_pcm_hw_params_set_format)       \
        X(snd_pcm_hw_params_set_channels) X(snd_pcm_hw_params_set_rate_near)                       \
            X(snd_pcm_hw_params_set_period_size_near) X(snd_pcm_hw_params_set_periods_near)        \
                X(snd_pcm_hw_params) X(snd_pcm_hw_params_get_period_size) X(snd_pcm_prepare)       \
                    X(snd_pcm_drop) X(snd_pcm_poll_descriptors_count) X(snd_pcm_poll_descriptors)  \
                        X(snd_pcm_poll_descriptors_revents) X(snd_pcm_writei) X(snd_pcm_recover)   \
                            X(snd_device_name_hint) X(snd_device_name_get_hint)                    \
                                X(snd_device_name_free_hint)
#define DE__DL_FIELD(name) __typeof__(&name) name;
typedef struct de__alsa_api {
    DE__ALSA_SYMBOLS(DE__DL_FIELD) void *library;
} de__alsa_api;
#undef DE__DL_FIELD
bool de__alsa_load(de__alsa_api *api);
void de__alsa_test_xrun(de__audio_stream *stream);
#endif
#if defined(__linux__) && DE_AUDIO_HAS_PIPEWIRE
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#define DE__PW_SYMBOLS(X)                                                                          \
    X(pw_init)                                                                                     \
    X(pw_deinit)                                                                                   \
    X(pw_thread_loop_new)                                                                          \
    X(pw_thread_loop_destroy)                                                                      \
    X(pw_thread_loop_get_loop) X(pw_thread_loop_start) X(pw_thread_loop_stop)                      \
        X(pw_thread_loop_lock) X(pw_thread_loop_unlock) X(pw_context_new) X(pw_context_destroy)    \
            X(pw_context_connect) X(pw_core_disconnect) X(pw_proxy_destroy) X(pw_properties_new)   \
                X(pw_properties_set) X(pw_stream_new) X(pw_stream_destroy)                         \
                    X(pw_stream_add_listener) X(pw_stream_connect) X(pw_stream_disconnect)         \
                        X(pw_stream_set_active) X(pw_stream_dequeue_buffer)                        \
                            X(pw_stream_queue_buffer)

#define DE__DL_FIELD(name) __typeof__(&name) name;
typedef struct de__pw_api {
    DE__PW_SYMBOLS(DE__DL_FIELD) void *library;
} de__pw_api;
#undef DE__DL_FIELD
bool de__pw_load(de__pw_api *api);
#endif
#endif
