#include "audio/backend/coreaudio_internal.h"
#include <string.h>
AudioObjectPropertyAddress de__ca_address(AudioObjectPropertySelector selector,
                                          AudioObjectPropertyScope scope) {
    return (AudioObjectPropertyAddress){selector, scope, kAudioObjectPropertyElementMain};
}
AudioDeviceID de__ca_default(void) {
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    AudioObjectPropertyAddress p =
        de__ca_address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &p, 0, NULL, &size, &device) != noErr)
        return kAudioObjectUnknown;
    return device;
}
bool de__ca_string(AudioDeviceID device, AudioObjectPropertySelector property, char *out,
                   u32 capacity) {
    CFStringRef value = NULL;
    UInt32 size = sizeof value;
    AudioObjectPropertyAddress p = de__ca_address(property, kAudioObjectPropertyScopeGlobal);
    if (!capacity)
        return false;
    out[0] = 0;
    if (AudioObjectGetPropertyData(device, &p, 0, NULL, &size, &value) != noErr || !value)
        return false;
    bool ok = CFStringGetCString(value, out, capacity, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok;
}
u32 de__ca_channels(const de_allocator *a, AudioDeviceID device) {
    AudioObjectPropertyAddress p =
        de__ca_address(kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &p, 0, NULL, &size) != noErr ||
        size < sizeof(AudioBufferList))
        return 0;
    AudioBufferList *list = DE_ALLOC(a, size, DE_ALIGNOF(AudioBufferList), DE_MEM_AUDIO);
    if (!list)
        return 0;
    u32 count = 0;
    if (AudioObjectGetPropertyData(device, &p, 0, NULL, &size, list) == noErr)
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            count += list->mBuffers[i].mNumberChannels;
    DE_FREE(a, list, size);
    return count;
}
static AudioDeviceID *devices(de__coreaudio *b, UInt32 *bytes) {
    AudioObjectPropertyAddress p =
        de__ca_address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    *bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &p, 0, NULL, bytes) != noErr ||
        !*bytes)
        return NULL;
    UInt32 allocated = *bytes;
    AudioDeviceID *ids = DE_ALLOC(&b->alloc, allocated, DE_ALIGNOF(AudioDeviceID), DE_MEM_AUDIO);
    if (!ids)
        return NULL;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &p, 0, NULL, bytes, ids) != noErr) {
        DE_FREE(&b->alloc, ids, allocated);
        *bytes = 0;
        return NULL;
    }
    /* Preserve allocation size even if the list shrank between the two queries. */
    for (UInt32 i = *bytes / sizeof *ids; i < allocated / sizeof *ids; ++i)
        ids[i] = kAudioObjectUnknown;
    *bytes = allocated;
    return ids;
}
u32 de__ca_enumerate(void *backend, de_audio_device_info *out, u32 max) {
    de__coreaudio *b = backend;
    UInt32 bytes;
    AudioDeviceID *ids = devices(b, &bytes);
    AudioDeviceID def = de__ca_default();
    u32 count = 0;
    for (UInt32 i = 0; ids && i < bytes / sizeof *ids; ++i) {
        de_audio_device_info d = {0};
        if (!ids[i] || !(d.channels = de__ca_channels(&b->alloc, ids[i])) ||
            !de__ca_string(ids[i], kAudioDevicePropertyDeviceUID, d.id, sizeof d.id))
            continue;
        if (!de__ca_string(ids[i], kAudioObjectPropertyName, d.name, sizeof d.name))
            memcpy(d.name, d.id, sizeof d.name);
        d.is_default = ids[i] == def;
        if (out && count < max)
            out[count] = d;
        ++count;
    }
    if (ids)
        DE_FREE(&b->alloc, ids, bytes);
    return count;
}
AudioDeviceID de__ca_resolve(de__coreaudio *b, const char *uid) {
    if (!uid)
        return de__ca_default();
    UInt32 bytes;
    AudioDeviceID *ids = devices(b, &bytes), found = kAudioObjectUnknown;
    for (UInt32 i = 0; ids && i < bytes / sizeof *ids; ++i) {
        char name[128];
        if (ids[i] && de__ca_string(ids[i], kAudioDevicePropertyDeviceUID, name, sizeof name) &&
            strcmp(uid, name) == 0 && de__ca_channels(&b->alloc, ids[i])) {
            found = ids[i];
            break;
        }
    }
    if (ids)
        DE_FREE(&b->alloc, ids, bytes);
    return found;
}
OSStatus de__ca_listener(AudioObjectID object, UInt32 count,
                         const AudioObjectPropertyAddress *addresses, void *user) {
    (void)object;
    de__coreaudio *b = user;
    de_atomic_fetch_add_u32(&b->listeners_active, 1, DE_ATOMIC_ACQ_REL);
    if (!de_atomic_load_u32(&b->closing, DE_ATOMIC_ACQUIRE)) {
        u32 events = 0;
        for (UInt32 i = 0; i < count; ++i) {
            if (addresses[i].mSelector == kAudioHardwarePropertyDefaultOutputDevice)
                events |= DE__AUDIO_EV_DEFAULT_CHANGED;
            else if (addresses[i].mSelector == kAudioDevicePropertyDeviceIsAlive)
                events |= DE__AUDIO_EV_STREAM_LOST | DE__AUDIO_EV_DEVICES_CHANGED;
            else if (addresses[i].mSelector == kAudioHardwarePropertyDevices)
                events |= DE__AUDIO_EV_DEVICES_CHANGED;
        }
        if (events && b->notify)
            b->notify(b->notify_user, events);
    }
    de_atomic_fetch_sub_u32(&b->listeners_active, 1, DE_ATOMIC_RELEASE);
    return noErr;
}
