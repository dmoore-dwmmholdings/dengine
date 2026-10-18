#ifndef DE_AUDIO_WASAPI_INTERNAL_H
#define DE_AUDIO_WASAPI_INTERNAL_H
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "audio/audio_internal.h"
#include <dengine/de_debug.h>
#include "audio/backend/wasapi_format.h"
#include "audio/dsp/resample.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
typedef struct de__wasapi {
    de_allocator alloc;
    de__audio_backend_desc desc;
    IMMNotificationClient notification;
    LONG refs;
    de_thread *thread;
    HANDLE stop;
    de_sem ready;
    HRESULT result;
} de__wasapi;
void de__wasapi_notify_init(de__wasapi *b);
void de__wasapi_post(de__wasapi *b, u32 events);
void de__wasapi_test_fail_exclusive(bool fail);
bool de__wasapi_is_exclusive(const de__audio_stream *stream);
void de__wasapi_wave(const de__wasapi_format *format, WAVEFORMATEXTENSIBLE *wave);
#endif
