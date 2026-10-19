/* COM objects are created, used and released on their owning MTA threads. The device manager
 * calls only the handshakes; notification and stream threads never reopen themselves. */
#include "audio/backend/wasapi_internal.h"
#include <string.h>
#include <initguid.h>
/* MIDL headers declare these but the xwin SDK's uuid.lib does not define the audio IDs. */
DEFINE_GUID(IID_IAudioClient, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03,
            0xb2);
DEFINE_GUID(IID_IAudioClient3, 0x7ed4ee07, 0x8e67, 0x4cd4, 0x8c, 0x1a, 0x2b, 0x7a, 0x59, 0x87, 0xad,
            0x42);
DEFINE_GUID(IID_IAudioRenderClient, 0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2,
            0x60, 0xe2);
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36,
            0x17, 0xe6);
DEFINE_GUID(IID_IMMNotificationClient, 0x7991eec9, 0x7e89, 0x4d85, 0x83, 0x90, 0x6c, 0x70, 0x3c,
            0xec, 0x60, 0xc0);
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92,
            0x91, 0x69, 0x2e);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa,
            0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00,
            0x38, 0x9b, 0x71);

typedef struct wasapi_stream {
    de__audio_stream base;
    de__wasapi *backend;
    de__audio_stream_desc desc;
    char uid[128];
    HANDLE stop, start, event;
    de_thread *thread;
    de_sem ready;
    HRESULT result;
    bool exclusive, started;
    de__wasapi_format format;
    u32 capacity;
    f32 *scratch, *input;
    de__resampler *resampler;
} wasapi_stream;
static de_atomic_u32 force_exclusive_failure = DE_ATOMIC_INIT(0);
void de__wasapi_test_fail_exclusive(bool fail) {
    de_atomic_store_u32(&force_exclusive_failure, fail, DE_ATOMIC_RELEASE);
}
bool de__wasapi_is_exclusive(const de__audio_stream *stream) {
    return ((const wasapi_stream *)stream)->exclusive;
}
static HRESULT enumerator(IMMDeviceEnumerator **out) {
    return CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator,
                            (void **)out);
}
static i32 notification_thread(void *user) {
    de__wasapi *b = user;
    HRESULT apartment = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *e = NULL;
    b->result = SUCCEEDED(apartment) ? enumerator(&e) : apartment;
    if (SUCCEEDED(b->result))
        b->result = IMMDeviceEnumerator_RegisterEndpointNotificationCallback(e, &b->notification);
    de_sem_post(&b->ready, 1);
    if (SUCCEEDED(b->result)) {
        WaitForSingleObject(b->stop, INFINITE);
        IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(e, &b->notification);
    }
    if (e)
        IMMDeviceEnumerator_Release(e);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return 0;
}
static void destroy(void *backend) {
    de__wasapi *b = backend;
    if (b->thread) {
        SetEvent(b->stop);
        de_thread_join(b->thread);
    }
    if (b->stop)
        CloseHandle(b->stop);
    de_sem_destroy(&b->ready);
    DE_FREE(&b->alloc, b, sizeof *b);
}
static de_result create(const de__audio_backend_desc *desc, void **out) {
    *out = NULL;
    de__wasapi *b = DE_NEW(desc->allocator, de__wasapi, DE_MEM_AUDIO);
    if (!b)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "WASAPI backend");
    memset(b, 0, sizeof *b);
    b->alloc = *desc->allocator;
    b->desc = *desc;
    de__wasapi_notify_init(b);
    de_result r = de_sem_init(&b->ready, 0);
    if (r != DE_OK) {
        DE_FREE(&b->alloc, b, sizeof *b);
        return r;
    }
    b->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    r = b->stop ? de_thread_create(notification_thread, b, "de-wasapi-notify", DE_THREAD_NORMAL, 0,
                                   &b->thread)
                : DE_ERR_IO;
    if (r == DE_OK) {
        de_sem_wait(&b->ready);
        if (FAILED(b->result))
            r = DE_ERR_UNSUPPORTED;
    }
    if (r != DE_OK) {
        destroy(b);
        return DE_FAIL(r, "WASAPI endpoint notifications unavailable");
    }
    *out = b;
    return DE_OK;
}
static bool endpoint_id(IMMDevice *device, char out[128]) {
    LPWSTR wide = NULL;
    if (FAILED(IMMDevice_GetId(device, &wide)))
        return false;
    bool ok = de__wasapi_utf8((const u16 *)wide, out, 128);
    CoTaskMemFree(wide);
    return ok;
}
static u32 enumerate(void *backend, de_audio_device_info *out, u32 max) {
    (void)backend;
    HRESULT apartment = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE)
        return 0;
    IMMDeviceEnumerator *e = NULL;
    IMMDeviceCollection *collection = NULL;
    IMMDevice *def = NULL;
    u32 count = 0;
    char default_id[128] = {0};
    if (FAILED(enumerator(&e)))
        goto done;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eMultimedia, &def))) {
        endpoint_id(def, default_id);
        IMMDevice_Release(def);
    }
    if (FAILED(
            IMMDeviceEnumerator_EnumAudioEndpoints(e, eRender, DEVICE_STATE_ACTIVE, &collection)))
        goto done;
    UINT total = 0;
    IMMDeviceCollection_GetCount(collection, &total);
    for (UINT i = 0; i < total; ++i) {
        IMMDevice *device = NULL;
        IPropertyStore *properties = NULL;
        de_audio_device_info info = {0};
        if (FAILED(IMMDeviceCollection_Item(collection, i, &device)))
            continue;
        if (!endpoint_id(device, info.id)) {
            IMMDevice_Release(device);
            continue;
        }
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &properties))) {
            PROPVARIANT value;
            memset(&value, 0, sizeof value);
            if (SUCCEEDED(IPropertyStore_GetValue(properties, &PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR)
                de__wasapi_utf8((const u16 *)value.pwszVal, info.name, sizeof info.name);
            PropVariantClear(&value);
            IPropertyStore_Release(properties);
        }
        if (!info.name[0])
            memcpy(info.name, info.id, sizeof info.name);
        IAudioClient *client = NULL;
        WAVEFORMATEX *format = NULL;
        if (SUCCEEDED(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                                         (void **)&client))) {
            if (SUCCEEDED(IAudioClient_GetMixFormat(client, &format))) {
                info.channels = format->nChannels;
                CoTaskMemFree(format);
            }
            IAudioClient_Release(client);
        }
        info.is_default = strcmp(info.id, default_id) == 0;
        if (info.channels) {
            if (out && count < max)
                out[count] = info;
            ++count;
        }
        IMMDevice_Release(device);
    }
done:
    if (collection)
        IMMDeviceCollection_Release(collection);
    if (e)
        IMMDeviceEnumerator_Release(e);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return count;
}
void de__wasapi_wave(const de__wasapi_format *f, WAVEFORMATEXTENSIBLE *w) {
    memset(w, 0, sizeof *w);
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = (WORD)f->channels;
    w->Format.nSamplesPerSec = f->rate;
    w->Format.wBitsPerSample = f->sample == DE__WASAPI_F32   ? 32
                               : f->sample == DE__WASAPI_I24 ? 24
                                                             : 16;
    w->Format.nBlockAlign = (WORD)(w->Format.nChannels * (w->Format.wBitsPerSample / 8));
    w->Format.nAvgBytesPerSec = w->Format.nSamplesPerSec * w->Format.nBlockAlign;
    w->Format.cbSize = sizeof *w - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = w->Format.wBitsPerSample;
    w->dwChannelMask = f->mask;
    w->SubFormat =
        f->sample == DE__WASAPI_F32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}
static HRESULT activate(IMMDevice *device, IAudioClient **client) {
    return IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)client);
}
static HRESULT exclusive(wasapi_stream *s, IMMDevice *device, IAudioClient **out) {
    if (de_atomic_load_u32(&force_exclusive_failure, DE_ATOMIC_ACQUIRE))
        return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    IAudioClient *probe = NULL;
    WAVEFORMATEX *mix = NULL;
    HRESULT hr = activate(device, &probe);
    if (FAILED(hr))
        return hr;
    hr = IAudioClient_GetMixFormat(probe, &mix);
    if (FAILED(hr)) {
        IAudioClient_Release(probe);
        return hr;
    }
    u32 rate = mix->nSamplesPerSec;
    CoTaskMemFree(mix);
    IAudioClient_Release(probe);
    de__wasapi_format candidates[3];
    de__wasapi_candidates(rate, s->desc.channels, candidates);
    for (u32 i = 0; i < 3; ++i) {
        IAudioClient *client = NULL;
        WAVEFORMATEXTENSIBLE wave;
        de__wasapi_wave(&candidates[i], &wave);
        hr = activate(device, &client);
        if (FAILED(hr))
            continue;
        hr =
            IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE, &wave.Format, NULL);
        if (hr == S_OK) {
            REFERENCE_TIME period =
                (REFERENCE_TIME)((10000000ull * s->desc.block_frames + s->desc.sample_rate - 1) /
                                 s->desc.sample_rate);
            hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                         &wave.Format, NULL);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 frames = 0;
                if (SUCCEEDED(IAudioClient_GetBufferSize(client, &frames))) {
                    IAudioClient_Release(client);
                    client = NULL;
                    hr = activate(device, &client);
                    period = (REFERENCE_TIME)((10000000ull * frames + rate - 1) / rate);
                    if (SUCCEEDED(hr))
                        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                                     period, &wave.Format, NULL);
                }
            }
            if (SUCCEEDED(hr)) {
                s->format = candidates[i];
                s->exclusive = true;
                *out = client;
                return hr;
            }
        }
        if (client)
            IAudioClient_Release(client);
    }
    return FAILED(hr) ? hr : AUDCLNT_E_UNSUPPORTED_FORMAT;
}
static HRESULT shared(wasapi_stream *s, IMMDevice *device, IAudioClient **out) {
    de__wasapi_format f = {s->desc.sample_rate, s->desc.channels, de__wasapi_mask(s->desc.channels),
                           DE__WASAPI_F32};
    WAVEFORMATEXTENSIBLE wave;
    de__wasapi_wave(&f, &wave);
    IAudioClient *client = NULL;
    IAudioClient3 *client3 = NULL;
    HRESULT hr = activate(device, &client);
    if (FAILED(hr))
        return hr;
    if (SUCCEEDED(IAudioClient_QueryInterface(client, &IID_IAudioClient3, (void **)&client3))) {
        UINT32 def = 0, fundamental = 0, minimum = 0, maximum = 0;
        hr = IAudioClient3_GetSharedModeEnginePeriod(client3, &wave.Format, &def, &fundamental,
                                                     &minimum, &maximum);
        if (SUCCEEDED(hr))
            hr = IAudioClient3_InitializeSharedAudioStream(
                client3, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimum, &wave.Format, NULL);
        IAudioClient3_Release(client3);
        if (SUCCEEDED(hr)) {
            s->format = f;
            *out = client;
            return hr;
        }
    }
    IAudioClient_Release(client);
    client = NULL;
    hr = activate(device, &client);
    if (SUCCEEDED(hr))
        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                         AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     0, 0, &wave.Format, NULL);
    if (FAILED(hr)) {
        if (client)
            IAudioClient_Release(client);
        return hr;
    }
    s->format = f;
    *out = client;
    return hr;
}
static void render(wasapi_stream *s, f32 *out, u32 frames) {
    if (!s->resampler) {
        s->desc.render(s->desc.user, out, frames);
        return;
    }
    u32 done = 0;
    while (done < frames) {
        u32 n =
            de__resample_read(s->resampler, out + (size_t)done * s->desc.channels, frames - done);
        done += n;
        if (done == frames)
            break;
        s->desc.render(s->desc.user, s->input, 1024);
        de__resample_push(s->resampler, s->input, 1024);
    }
}
static i32 render_thread(void *user) {
    wasapi_stream *s = user;
    HRESULT apartment = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *e = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *renderer = NULL;
    HANDLE mmcss = NULL;
    DWORD task = 0;
    HRESULT hr = SUCCEEDED(apartment) ? enumerator(&e) : apartment;
    if (SUCCEEDED(hr)) {
        if (s->uid[0]) {
            WCHAR wide[128];
            int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s->uid, -1, wide, 128);
            hr = n ? IMMDeviceEnumerator_GetDevice(e, wide, &device) : E_INVALIDARG;
        } else
            hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eMultimedia, &device);
    }
    de_cvar_desc cv = {0};
    cv.name = "audio.wasapi_exclusive";
    cv.help = "Prefer WASAPI exclusive output; fall back to shared on failure";
    bool want_exclusive = de_cvar_bool(de_cvar_register(&cv));
    if (SUCCEEDED(hr) && want_exclusive) {
        hr = exclusive(s, device, &client);
        if (FAILED(hr))
            DE_WARN(de_log_register_category("audio", DE_LOG_INFO),
                    "WASAPI exclusive failed (%08lx); using shared mode", (unsigned long)hr);
    }
    if (device && !client)
        hr = shared(s, device, &client);
    if (SUCCEEDED(hr))
        hr = IAudioClient_SetEventHandle(client, s->event);
    if (SUCCEEDED(hr))
        hr = IAudioClient_GetBufferSize(client, &s->capacity);
    if (SUCCEEDED(hr))
        hr = IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&renderer);
    if (SUCCEEDED(hr)) {
        s->scratch = DE_ALLOC(&s->backend->alloc, (size_t)s->capacity * s->desc.channels * 4, 16,
                              DE_MEM_AUDIO);
        if (!s->scratch)
            hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr) && s->format.rate != s->desc.sample_rate) {
        s->input =
            DE_ALLOC(&s->backend->alloc, 1024 * s->desc.channels * sizeof(f32), 16, DE_MEM_AUDIO);
        if (!s->input ||
            de__resample_create(&s->backend->alloc, s->desc.channels,
                                (f64)s->desc.sample_rate / s->format.rate, &s->resampler) != DE_OK)
            hr = E_OUTOFMEMORY;
    }
    s->result = hr;
    de_sem_post(&s->ready, 1);
    if (FAILED(hr))
        goto done;
    HANDLE before[] = {s->stop, s->start};
    if (WaitForMultipleObjects(2, before, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
        goto done;
    /* Prime endpoint with silence before the first event, without consuming mixer samples. */
    BYTE *bytes = NULL;
    hr = IAudioRenderClient_GetBuffer(renderer, s->capacity, &bytes);
    if (SUCCEEDED(hr))
        hr = IAudioRenderClient_ReleaseBuffer(renderer, s->capacity, AUDCLNT_BUFFERFLAGS_SILENT);
    if (SUCCEEDED(hr))
        hr = IAudioClient_Start(client);
    s->result = hr;
    s->started = SUCCEEDED(hr);
    de_sem_post(&s->ready, 1);
    if (FAILED(hr))
        goto done;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    HANDLE events[] = {s->stop, s->event};
    for (;;) {
        DWORD event = WaitForMultipleObjects(2, events, FALSE, 500);
        if (event == WAIT_OBJECT_0)
            break;
        if (event == WAIT_TIMEOUT)
            continue;
        if (event != WAIT_OBJECT_0 + 1) {
            hr = E_FAIL;
            break;
        }
        de__rt_guard_enter();
        UINT32 padding = 0;
        hr = s->exclusive ? S_OK : IAudioClient_GetCurrentPadding(client, &padding);
        if (SUCCEEDED(hr) && padding <= s->capacity) {
            u32 frames = s->capacity - padding;
            if (frames) {
                hr = IAudioRenderClient_GetBuffer(renderer, frames, &bytes);
                if (SUCCEEDED(hr)) {
                    render(s, s->scratch, frames);
                    de__wasapi_convert(s->scratch, bytes, frames * s->desc.channels,
                                       s->format.sample);
                    hr = IAudioRenderClient_ReleaseBuffer(renderer, frames, 0);
                }
            }
        }
        de__rt_guard_leave();
        if (FAILED(hr)) {
            de__wasapi_post(s->backend, DE__AUDIO_EV_STREAM_LOST);
            break;
        }
    }
done:
    if (client && s->started)
        IAudioClient_Stop(client);
    if (mmcss)
        AvRevertMmThreadCharacteristics(mmcss);
    if (renderer)
        IAudioRenderClient_Release(renderer);
    if (client)
        IAudioClient_Release(client);
    if (device)
        IMMDevice_Release(device);
    if (e)
        IMMDeviceEnumerator_Release(e);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return 0;
}
static void stop(de__audio_stream *base) {
    wasapi_stream *s = (wasapi_stream *)base;
    if (s->thread) {
        SetEvent(s->stop);
        de_thread_join(s->thread);
        s->thread = NULL;
    }
}
static void close_stream(de__audio_stream *base) {
    wasapi_stream *s = (wasapi_stream *)base;
    stop(base);
    if (s->stop)
        CloseHandle(s->stop);
    if (s->start)
        CloseHandle(s->start);
    if (s->event)
        CloseHandle(s->event);
    de_sem_destroy(&s->ready);
    de__resample_destroy(s->resampler);
    if (s->scratch)
        DE_FREE(&s->backend->alloc, s->scratch, (size_t)s->capacity * s->desc.channels * 4);
    if (s->input)
        DE_FREE(&s->backend->alloc, s->input, 1024 * s->desc.channels * sizeof(f32));
    DE_FREE(&s->backend->alloc, s, sizeof *s);
}
static de_result open_stream(void *backend, const char *uid, const de__audio_stream_desc *desc,
                             de__audio_stream **out) {
    *out = NULL;
    de__wasapi *b = backend;
    if (!desc || !desc->render || !de__wasapi_mask(desc->channels) || (uid && strlen(uid) >= 128))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid WASAPI stream");
    wasapi_stream *s = DE_NEW(&b->alloc, wasapi_stream, DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "WASAPI stream");
    memset(s, 0, sizeof *s);
    s->base.vt = &de__audio_backend_wasapi;
    s->base.backend = b;
    s->backend = b;
    s->desc = *desc;
    if (uid)
        memcpy(s->uid, uid, strlen(uid) + 1);
    de_result r = de_sem_init(&s->ready, 0);
    if (r != DE_OK) {
        DE_FREE(&b->alloc, s, sizeof *s);
        return r;
    }
    s->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->start = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    r = s->stop && s->start && s->event ? de_thread_create(render_thread, s, "de-wasapi-render",
                                                           DE_THREAD_REALTIME, 0, &s->thread)
                                        : DE_ERR_IO;
    if (r == DE_OK) {
        de_sem_wait(&s->ready);
        if (FAILED(s->result))
            r = DE_ERR_UNSUPPORTED;
    }
    if (r != DE_OK) {
        close_stream(&s->base);
        return DE_FAIL(r, "WASAPI stream initialization failed");
    }
    *out = &s->base;
    return DE_OK;
}
static de_result start(de__audio_stream *base) {
    wasapi_stream *s = (wasapi_stream *)base;
    SetEvent(s->start);
    de_sem_wait(&s->ready);
    return SUCCEEDED(s->result)
               ? DE_OK
               : DE_FAIL(DE_ERR_IO, "WASAPI start failed (%08lx)", (unsigned long)s->result);
}
const de__audio_backend_vtbl de__audio_backend_wasapi = {
    "wasapi",    DE_AUDIO_WASAPI, create, destroy,      enumerate,
    open_stream, start,           stop,   close_stream, NULL};
