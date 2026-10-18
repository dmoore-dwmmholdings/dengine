#include "audio/backend/wasapi_internal.h"
#include <stddef.h>
static de__wasapi *owner(IMMNotificationClient *client) {
    return (de__wasapi *)((u8 *)client - offsetof(de__wasapi, notification));
}
void de__wasapi_post(de__wasapi *b, u32 events) {
    if (b->desc.notify)
        b->desc.notify(b->desc.notify_user, events);
}
static HRESULT STDMETHODCALLTYPE query(IMMNotificationClient *c, REFIID iid, void **out) {
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IMMNotificationClient))
        return E_NOINTERFACE;
    *out = c;
    InterlockedIncrement(&owner(c)->refs);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE addref(IMMNotificationClient *c) {
    return (ULONG)InterlockedIncrement(&owner(c)->refs);
}
static ULONG STDMETHODCALLTYPE release(IMMNotificationClient *c) {
    return (ULONG)InterlockedDecrement(&owner(c)->refs); /* embedded, backend owns lifetime */
}
static HRESULT STDMETHODCALLTYPE state(IMMNotificationClient *c, LPCWSTR id, DWORD value) {
    (void)id;
    (void)value;
    de__wasapi_post(owner(c), DE__AUDIO_EV_DEVICES_CHANGED | DE__AUDIO_EV_STREAM_LOST);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE added(IMMNotificationClient *c, LPCWSTR id) {
    (void)id;
    de__wasapi_post(owner(c), DE__AUDIO_EV_DEVICES_CHANGED);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE removed(IMMNotificationClient *c, LPCWSTR id) {
    (void)id;
    de__wasapi_post(owner(c), DE__AUDIO_EV_DEVICES_CHANGED | DE__AUDIO_EV_STREAM_LOST);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE changed(IMMNotificationClient *c, EDataFlow flow, ERole role,
                                         LPCWSTR id) {
    (void)id;
    if (flow == eRender && (role == eConsole || role == eMultimedia))
        de__wasapi_post(owner(c), DE__AUDIO_EV_DEFAULT_CHANGED);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE property(IMMNotificationClient *c, LPCWSTR id,
                                          const PROPERTYKEY key) {
    (void)id;
    (void)key;
    de__wasapi_post(owner(c), DE__AUDIO_EV_DEVICES_CHANGED);
    return S_OK;
}
static IMMNotificationClientVtbl vtbl = {query, addref,  release, state,
                                         added, removed, changed, property};
void de__wasapi_notify_init(de__wasapi *b) {
    b->notification.lpVtbl = &vtbl;
    b->refs = 1;
}
