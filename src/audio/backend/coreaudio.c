/* Core Audio owns callback threads. Only ring copies and atomic counters run there. */
#include "audio/backend/coreaudio_internal.h"
#include "core/de_compiler.h"
#include <string.h>
AudioChannelLayoutTag de__ca_layout(u32 channels) {
    switch (channels) {
    case 1:
        return kAudioChannelLayoutTag_Mono;
    case 2:
        return kAudioChannelLayoutTag_Stereo;
    case 6:
        return kAudioChannelLayoutTag_MPEG_5_1_A;
    case 8:
        return kAudioChannelLayoutTag_MPEG_7_1_C;
    default:
        return kAudioChannelLayoutTag_Unknown | channels;
    }
}
OSStatus de__ca_render(void *user, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time,
                       UInt32 bus, UInt32 frames, AudioBufferList *buffers) {
    (void)flags;
    (void)time;
    (void)bus;
    de__coreaudio_stream *s = user;
    /* This is OS-owned, so establish the engine FP environment on each callback thread. */
    static DE_THREAD_LOCAL bool initialized;
    if (!initialized) {
        de_fp_env_init_thread();
        initialized = true;
    }
    de__rt_guard_enter();
    if (buffers && buffers->mNumberBuffers == 1 && buffers->mBuffers[0].mData &&
        buffers->mBuffers[0].mNumberChannels == s->desc.channels &&
        (u64)frames * s->desc.channels * sizeof(f32) <= buffers->mBuffers[0].mDataByteSize) {
        s->desc.render(s->desc.user, buffers->mBuffers[0].mData, frames);
        de_atomic_fetch_add_u64(&s->frames, frames, DE_ATOMIC_RELAXED);
    } else if (buffers) {
        for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
            if (buffers->mBuffers[i].mData)
                memset(buffers->mBuffers[i].mData, 0, buffers->mBuffers[i].mDataByteSize);
    }
    de_atomic_fetch_add_u64(&s->callbacks, 1, DE_ATOMIC_RELAXED);
    de__rt_guard_leave();
    return noErr;
}
static de_result create(const de__audio_backend_desc *d, void **out) {
    *out = NULL;
    de__coreaudio *b = DE_NEW(d->allocator, de__coreaudio, DE_MEM_AUDIO);
    if (!b)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Core Audio backend");
    memset(b, 0, sizeof *b);
    b->alloc = *d->allocator;
    b->notify = d->notify;
    b->notify_user = d->notify_user;
    de_atomic_init_u32(&b->closing, 0);
    de_atomic_init_u32(&b->listeners_active, 0);
    AudioObjectPropertyAddress p =
        de__ca_address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    OSStatus status =
        AudioObjectAddPropertyListener(kAudioObjectSystemObject, &p, de__ca_listener, b);
    if (status == noErr) {
        b->devices_listener = true;
        p.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        status = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &p, de__ca_listener, b);
        b->default_listener = status == noErr;
    }
    if (status != noErr) {
        if (b->devices_listener) {
            p.mSelector = kAudioHardwarePropertyDevices;
            AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &p, de__ca_listener, b);
        }
        DE_FREE(&b->alloc, b, sizeof *b);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "Core Audio listeners unavailable (%d)", (int)status);
    }
    *out = b;
    return DE_OK;
}
static void destroy(void *backend) {
    de__coreaudio *b = backend;
    de_atomic_store_u32(&b->closing, 1, DE_ATOMIC_RELEASE);
    AudioObjectPropertyAddress p =
        de__ca_address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    if (b->devices_listener)
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &p, de__ca_listener, b);
    p.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
    if (b->default_listener)
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &p, de__ca_listener, b);
    while (de_atomic_load_u32(&b->listeners_active, DE_ATOMIC_ACQUIRE))
        de_sleep_ns(100000);
    DE_FREE(&b->alloc, b, sizeof *b);
}
static void stop(de__audio_stream *base) {
    de__coreaudio_stream *s = (de__coreaudio_stream *)base;
    if (s->started) {
        AudioOutputUnitStop(s->unit);
        s->started = false;
    }
}
static void close_stream(de__audio_stream *base) {
    de__coreaudio_stream *s = (de__coreaudio_stream *)base;
    stop(base);
    AudioObjectPropertyAddress p =
        de__ca_address(kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal);
    if (s->alive_listener)
        AudioObjectRemovePropertyListener(s->device, &p, de__ca_listener, s->backend);
    if (s->initialized)
        AudioUnitUninitialize(s->unit);
    if (s->unit)
        AudioComponentInstanceDispose(s->unit);
    if (s->changed_buffer) {
        p.mSelector = kAudioDevicePropertyBufferFrameSize;
        UInt32 size = sizeof(UInt32), current = 0;
        if (AudioObjectGetPropertyData(s->device, &p, 0, NULL, &size, &current) == noErr &&
            current == s->requested_buffer)
            AudioObjectSetPropertyData(s->device, &p, 0, NULL, size, &s->previous_buffer);
    }
    DE_FREE(&s->backend->alloc, s, sizeof *s);
}
static de_result open_stream(void *backend, const char *uid, const de__audio_stream_desc *desc,
                             de__audio_stream **out) {
    *out = NULL;
    de__coreaudio *b = backend;
    AudioDeviceID device = de__ca_resolve(b, uid);
    if (!device)
        return DE_FAIL(DE_ERR_NOT_FOUND, "Core Audio output device unavailable");
    if (!desc || !desc->render || !desc->sample_rate || !desc->channels ||
        desc->channels > de__ca_channels(&b->alloc, device))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "Core Audio output channel layout unavailable");
    de__coreaudio_stream *s = DE_NEW(&b->alloc, de__coreaudio_stream, DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Core Audio stream");
    memset(s, 0, sizeof *s);
    s->base.vt = &de__audio_backend_coreaudio;
    s->base.backend = b;
    s->backend = b;
    s->desc = *desc;
    s->device = device;
    de_atomic_init_u64(&s->callbacks, 0);
    de_atomic_init_u64(&s->frames, 0);
    AudioComponentDescription component = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                           kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &component);
    OSStatus status =
        c ? AudioComponentInstanceNew(c, &s->unit) : kAudioUnitErr_FailedInitialization;
    UInt32 enabled = 1, disabled = 0;
    if (status == noErr)
        status = AudioUnitSetProperty(s->unit, kAudioOutputUnitProperty_EnableIO,
                                      kAudioUnitScope_Output, 0, &enabled, sizeof enabled);
    if (status == noErr)
        status = AudioUnitSetProperty(s->unit, kAudioOutputUnitProperty_EnableIO,
                                      kAudioUnitScope_Input, 1, &disabled, sizeof disabled);
    if (status == noErr)
        status = AudioUnitSetProperty(s->unit, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, 0, &device, sizeof device);
    AudioObjectPropertyAddress p =
        de__ca_address(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    UInt32 size = sizeof(UInt32);
    Boolean settable = false;
    if (status == noErr && AudioObjectIsPropertySettable(device, &p, &settable) == noErr &&
        settable &&
        AudioObjectGetPropertyData(device, &p, 0, NULL, &size, &s->previous_buffer) == noErr) {
        UInt32 requested = desc->block_frames;
        AudioValueRange range = {0};
        AudioObjectPropertyAddress range_p = de__ca_address(
            kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal);
        size = sizeof range;
        if (AudioObjectGetPropertyData(device, &range_p, 0, NULL, &size, &range) == noErr) {
            if (requested < range.mMinimum)
                requested = (UInt32)range.mMinimum;
            if (requested > range.mMaximum)
                requested = (UInt32)range.mMaximum;
        }
        s->requested_buffer = requested;
        s->changed_buffer =
            requested != s->previous_buffer &&
            AudioObjectSetPropertyData(device, &p, 0, NULL, sizeof requested, &requested) == noErr;
    }
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = desc->sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = format.mBytesPerFrame = desc->channels * sizeof(f32);
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = desc->channels;
    format.mBitsPerChannel = 32;
    if (status == noErr)
        status = AudioUnitSetProperty(s->unit, kAudioUnitProperty_StreamFormat,
                                      kAudioUnitScope_Input, 0, &format, sizeof format);
    AudioChannelLayout layout = {0};
    layout.mChannelLayoutTag = de__ca_layout(desc->channels);
    if (status == noErr)
        status = AudioUnitSetProperty(s->unit, kAudioUnitProperty_AudioChannelLayout,
                                      kAudioUnitScope_Input, 0, &layout,
                                      offsetof(AudioChannelLayout, mChannelDescriptions));
    AURenderCallbackStruct callback = {de__ca_render, s};
    if (status == noErr)
        status = AudioUnitSetProperty(s->unit, kAudioUnitProperty_SetRenderCallback,
                                      kAudioUnitScope_Input, 0, &callback, sizeof callback);
    if (status == noErr) {
        status = AudioUnitInitialize(s->unit);
        s->initialized = status == noErr;
    }
    p.mSelector = kAudioDevicePropertyDeviceIsAlive;
    if (status == noErr) {
        status = AudioObjectAddPropertyListener(device, &p, de__ca_listener, b);
        s->alive_listener = status == noErr;
    }
    if (status != noErr) {
        close_stream(&s->base);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "Core Audio stream setup failed (%d)", (int)status);
    }
    *out = &s->base;
    return DE_OK;
}
static de_result start(de__audio_stream *base) {
    de__coreaudio_stream *s = (de__coreaudio_stream *)base;
    OSStatus status = AudioOutputUnitStart(s->unit);
    s->started = status == noErr;
    return status == noErr ? DE_OK
                           : DE_FAIL(DE_ERR_IO, "Core Audio start failed (%d)", (int)status);
}
const de__audio_backend_vtbl de__audio_backend_coreaudio = {
    "coreaudio", DE_AUDIO_COREAUDIO, create, destroy, de__ca_enumerate, open_stream, start,
    stop,        close_stream,       NULL};
