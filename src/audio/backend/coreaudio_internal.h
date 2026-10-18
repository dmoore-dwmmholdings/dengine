#ifndef DE_AUDIO_COREAUDIO_INTERNAL_H
#define DE_AUDIO_COREAUDIO_INTERNAL_H
#include "audio/audio_internal.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
typedef struct de__coreaudio {
    de_allocator alloc;
    de__audio_notify_fn notify;
    void *notify_user;
    de_atomic_u32 closing, listeners_active;
    bool devices_listener, default_listener;
} de__coreaudio;
typedef struct de__coreaudio_stream {
    de__audio_stream base;
    de__coreaudio *backend;
    de__audio_stream_desc desc;
    AudioUnit unit;
    AudioDeviceID device;
    bool alive_listener, initialized, started, changed_buffer;
    UInt32 previous_buffer, requested_buffer;
    de_atomic_u64 callbacks, frames;
} de__coreaudio_stream;
AudioObjectPropertyAddress de__ca_address(AudioObjectPropertySelector selector,
                                          AudioObjectPropertyScope scope);
AudioDeviceID de__ca_default(void);
u32 de__ca_channels(const de_allocator *a, AudioDeviceID device);
AudioDeviceID de__ca_resolve(de__coreaudio *b, const char *uid);
u32 de__ca_enumerate(void *backend, de_audio_device_info *out, u32 max);
bool de__ca_string(AudioDeviceID device, AudioObjectPropertySelector property, char *out,
                   u32 capacity);
OSStatus de__ca_listener(AudioObjectID object, UInt32 count,
                         const AudioObjectPropertyAddress *addresses, void *user);
AudioChannelLayoutTag de__ca_layout(u32 channels);
/* Same callback used by HAL and the hardware-independent RT contract test. */
OSStatus de__ca_render(void *user, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time,
                       UInt32 bus, UInt32 frames, AudioBufferList *buffers);
#endif
