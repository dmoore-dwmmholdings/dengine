#include "audio/audio_internal.h"
#include "audio/backend/linux_dl.h"
#include "audio/backend/wasapi_format.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct alsa_backend {
    de_allocator alloc;
    de__audio_backend_desc desc;
    de__alsa_api api;
    u64 next_poll, fingerprint;
} alsa_backend;
typedef struct alsa_stream {
    de__audio_stream base;
    alsa_backend *backend;
    de__audio_stream_desc desc;
    snd_pcm_t *pcm;
    snd_pcm_uframes_t period;
    bool integer;
    struct pollfd *fds;
    u32 fd_count;
    int wake[2];
    f32 *samples;
    i16 *converted;
    de_thread *thread;
    de_atomic_u32 closing, inject_xrun;
} alsa_stream;
static u32 enumerate(void *backend, de_audio_device_info *out, u32 max) {
    alsa_backend *b = backend;
    void **hints = NULL;
    u32 count = 0;
    if (b->api.snd_device_name_hint(-1, "pcm", &hints) < 0)
        return 0;
    for (u32 i = 0; hints && hints[i]; ++i) {
        char *name = b->api.snd_device_name_get_hint(hints[i], "NAME");
        char *io = b->api.snd_device_name_get_hint(hints[i], "IOID");
        char *description = b->api.snd_device_name_get_hint(hints[i], "DESC");
        if (name && strlen(name) < 128 && (!io || strcmp(io, "Input") != 0)) {
            de_audio_device_info d = {0};
            snprintf(d.id, sizeof d.id, "%s", name);
            snprintf(d.name, sizeof d.name, "%s", description ? description : name);
            for (u32 n = 0; d.name[n]; ++n)
                if (d.name[n] == '\n')
                    d.name[n] = ' ';
            d.channels = 2;
            d.is_default = strcmp(name, "default") == 0;
            if (out && count < max)
                out[count] = d;
            ++count;
        }
        free(name);
        free(io);
        free(description);
    }
    if (hints)
        b->api.snd_device_name_free_hint(hints);
    return count;
}
static u64 fingerprint(alsa_backend *b) {
    de_audio_device_info devices[128];
    u32 count = enumerate(b, devices, (u32)(sizeof devices / sizeof devices[0]));
    u64 hash = count;
    for (u32 i = 0; i < count && i < (u32)(sizeof devices / sizeof devices[0]); ++i)
        hash = de_hash64(devices[i].id, strlen(devices[i].id), hash);
    return hash;
}
static void poll_devices(void *backend) {
    alsa_backend *b = backend;
    u64 now = de_time_now_ns();
    if (now < b->next_poll)
        return;
    b->next_poll = now + 2000000000ull;
    u64 hash = fingerprint(b);
    if (hash != b->fingerprint && b->desc.notify)
        b->desc.notify(b->desc.notify_user, DE__AUDIO_EV_DEVICES_CHANGED);
    b->fingerprint = hash;
}
static de_result create(const de__audio_backend_desc *desc, void **out) {
    *out = NULL;
    alsa_backend *b = DE_NEW(desc->allocator, alsa_backend, DE_MEM_AUDIO);
    if (!b)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ALSA backend");
    memset(b, 0, sizeof *b);
    b->alloc = *desc->allocator;
    b->desc = *desc;
    if (!de__alsa_load(&b->api)) {
        DE_FREE(&b->alloc, b, sizeof *b);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "ALSA library unavailable");
    }
    b->fingerprint = fingerprint(b);
    b->next_poll = de_time_now_ns() + 2000000000ull;
    *out = b;
    return DE_OK;
}
static void destroy(void *backend) {
    alsa_backend *b = backend;
    de__linux_unload(b->api.library);
    DE_FREE(&b->alloc, b, sizeof *b);
}
void de__alsa_test_xrun(de__audio_stream *base) {
    alsa_stream *s = (alsa_stream *)base;
    de_atomic_store_u32(&s->inject_xrun, 1, DE_ATOMIC_RELEASE);
}
static bool recover(alsa_stream *s, int error) {
    if (error == -EPIPE || error == -ESTRPIPE) {
        if (s->desc.underrun)
            s->desc.underrun(s->desc.user);
        return s->backend->api.snd_pcm_recover(s->pcm, error, 1) >= 0;
    }
    return false;
}
static i32 render_thread(void *user) {
    alsa_stream *s = user;
    de__alsa_api *api = &s->backend->api;
    u64 deadline = de_time_now_ns();
    while (!de_atomic_load_u32(&s->closing, DE_ATOMIC_ACQUIRE)) {
        int ready = poll(s->fds, s->fd_count + 1, s->fd_count ? 100 : 0);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (s->fds[s->fd_count].revents)
            break;
        if (!ready && s->fd_count)
            continue;
        unsigned short revents = 0;
        if (s->fd_count &&
            api->snd_pcm_poll_descriptors_revents(s->pcm, s->fds, s->fd_count, &revents) < 0)
            break;
        if (s->fd_count && !(revents & (POLLOUT | POLLERR)))
            continue;
        de__rt_guard_enter();
        s->desc.render(s->desc.user, s->samples, (u32)s->period);
        if (s->integer)
            de__wasapi_convert(s->samples, s->converted, (u32)s->period * s->desc.channels,
                               DE__WASAPI_I16);
        snd_pcm_uframes_t done = 0;
        while (done < s->period && !de_atomic_load_u32(&s->closing, DE_ATOMIC_ACQUIRE)) {
            const void *data = s->integer ? (const void *)(s->converted + done * s->desc.channels)
                                          : (const void *)(s->samples + done * s->desc.channels);
            snd_pcm_sframes_t n = de_atomic_exchange_u32(&s->inject_xrun, 0, DE_ATOMIC_ACQ_REL)
                                      ? -EPIPE
                                      : api->snd_pcm_writei(s->pcm, data, s->period - done);
            if (n > 0)
                done += (snd_pcm_uframes_t)n;
            else if (n == -EAGAIN || !n) {
                de__rt_guard_leave();
                poll(s->fds, s->fd_count + 1, 10);
                de__rt_guard_enter();
            } else if (!recover(s, (int)n)) {
                de_atomic_store_u32(&s->closing, 1, DE_ATOMIC_RELEASE);
                if (s->backend->desc.notify)
                    s->backend->desc.notify(s->backend->desc.notify_user, DE__AUDIO_EV_STREAM_LOST);
            }
        }
        de__rt_guard_leave();
        /* Hardware poll normally paces; ALSA null PCM is always writable, so cap it at audio time.
         */
        deadline += (u64)s->period * 1000000000ull / s->desc.sample_rate;
        u64 now = de_time_now_ns();
        if (deadline > now)
            de_sleep_ns(deadline - now);
        else if (now - deadline > 250000000ull)
            deadline = now;
    }
    return 0;
}
static void stop(de__audio_stream *base) {
    alsa_stream *s = (alsa_stream *)base;
    if (s->thread) {
        de_atomic_store_u32(&s->closing, 1, DE_ATOMIC_RELEASE);
        u8 byte = 1;
        (void)write(s->wake[1], &byte, 1);
        de_thread_join(s->thread);
        s->thread = NULL;
    }
    if (s->pcm)
        s->backend->api.snd_pcm_drop(s->pcm);
}
static void close_stream(de__audio_stream *base) {
    alsa_stream *s = (alsa_stream *)base;
    alsa_backend *b = s->backend;
    stop(base);
    if (s->pcm)
        b->api.snd_pcm_close(s->pcm);
    if (s->wake[0] >= 0)
        close(s->wake[0]);
    if (s->wake[1] >= 0)
        close(s->wake[1]);
    if (s->fds)
        DE_FREE(&b->alloc, s->fds, (s->fd_count + 1) * sizeof(struct pollfd));
    if (s->samples)
        DE_FREE(&b->alloc, s->samples, (size_t)s->period * s->desc.channels * sizeof(f32));
    if (s->converted)
        DE_FREE(&b->alloc, s->converted, (size_t)s->period * s->desc.channels * sizeof(i16));
    DE_FREE(&b->alloc, s, sizeof *s);
}
static de_result open_stream(void *backend, const char *id, const de__audio_stream_desc *desc,
                             de__audio_stream **out) {
    *out = NULL;
    alsa_backend *b = backend;
    de__alsa_api *api = &b->api;
    if (!desc || !desc->render || !desc->sample_rate || !desc->channels || desc->channels > 8)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid ALSA stream");
    alsa_stream *s = DE_NEW(&b->alloc, alsa_stream, DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ALSA stream");
    memset(s, 0, sizeof *s);
    s->wake[0] = s->wake[1] = -1;
    s->base.vt = &de__audio_backend_alsa;
    s->base.backend = b;
    s->backend = b;
    s->desc = *desc;
    de_atomic_init_u32(&s->closing, 0);
    de_atomic_init_u32(&s->inject_xrun, 0);
    snd_pcm_hw_params_t *params = NULL;
    int error =
        api->snd_pcm_open(&s->pcm, id ? id : "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (error >= 0)
        error = api->snd_pcm_hw_params_malloc(&params);
    if (error >= 0)
        error = api->snd_pcm_hw_params_any(s->pcm, params);
    if (error >= 0)
        error = api->snd_pcm_hw_params_set_access(s->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (error >= 0) {
        error = api->snd_pcm_hw_params_set_format(s->pcm, params, SND_PCM_FORMAT_FLOAT_LE);
        if (error < 0) {
            error = api->snd_pcm_hw_params_set_format(s->pcm, params, SND_PCM_FORMAT_S16_LE);
            s->integer = error >= 0;
        }
    }
    if (error >= 0)
        error = api->snd_pcm_hw_params_set_channels(s->pcm, params, desc->channels);
    unsigned int rate = desc->sample_rate, periods = 3;
    int direction = 0;
    if (error >= 0)
        error = api->snd_pcm_hw_params_set_rate_near(s->pcm, params, &rate, &direction);
    if (error >= 0 && rate != desc->sample_rate)
        error = -EINVAL;
    s->period = desc->block_frames;
    if (error >= 0)
        error = api->snd_pcm_hw_params_set_period_size_near(s->pcm, params, &s->period, &direction);
    if (error >= 0)
        error = api->snd_pcm_hw_params_set_periods_near(s->pcm, params, &periods, &direction);
    if (error >= 0)
        error = api->snd_pcm_hw_params(s->pcm, params);
    if (params)
        api->snd_pcm_hw_params_free(params);
    if (error >= 0)
        error = api->snd_pcm_prepare(s->pcm);
    if (error >= 0) {
        int count = api->snd_pcm_poll_descriptors_count(s->pcm);
        if (count < 0 || count > 1024 || !s->period || s->period > 65536)
            error = -EINVAL;
        else {
            s->fd_count = (u32)count;
            s->fds = DE_ALLOC(&b->alloc, (s->fd_count + 1) * sizeof(struct pollfd),
                              DE_ALIGNOF(struct pollfd), DE_MEM_AUDIO);
            s->samples = DE_ALLOC(&b->alloc, (size_t)s->period * desc->channels * sizeof(f32), 16,
                                  DE_MEM_AUDIO);
            if (s->integer)
                s->converted = DE_ALLOC(&b->alloc, (size_t)s->period * desc->channels * sizeof(i16),
                                        16, DE_MEM_AUDIO);
            if (!s->fds || !s->samples || (s->integer && !s->converted))
                error = -ENOMEM;
            else if (pipe(s->wake))
                error = -errno;
            else {
                error = api->snd_pcm_poll_descriptors(s->pcm, s->fds, s->fd_count);
                s->fds[s->fd_count] = (struct pollfd){s->wake[0], POLLIN, 0};
            }
        }
    }
    if (error < 0) {
        close_stream(&s->base);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "ALSA stream setup failed (%d)", error);
    }
    *out = &s->base;
    return DE_OK;
}
static de_result start(de__audio_stream *base) {
    alsa_stream *s = (alsa_stream *)base;
    return de_thread_create(render_thread, s, "de-alsa-render", DE_THREAD_REALTIME, 0, &s->thread);
}
const de__audio_backend_vtbl de__audio_backend_alsa = {"alsa",       DE_AUDIO_ALSA, create, destroy,
                                                       enumerate,    open_stream,   start,  stop,
                                                       close_stream, poll_devices};
