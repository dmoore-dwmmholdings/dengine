#include "audio/backend/linux_dl.h"
#include "platform/de_atomic.h"
#include <dlfcn.h>
#include <string.h>
static de_atomic_u32 missing = DE_ATOMIC_INIT(0);
void de__linux_test_missing(u32 mask) {
    de_atomic_store_u32(&missing, mask, DE_ATOMIC_RELEASE);
}
bool de__linux_load(const char *path, const de__linux_symbol *symbols, u32 count, void **handle) {
    *handle = NULL;
    for (u32 i = 0; i < count; ++i)
        memset(symbols[i].slot, 0, symbols[i].bytes);
    u32 mask = de_atomic_load_u32(&missing, DE_ATOMIC_ACQUIRE);
    if ((mask & 1 && strstr(path, "pipewire")) || (mask & 2 && strstr(path, "asound")))
        return false;
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return false;
    for (u32 i = 0; i < count; ++i) {
        void *address = dlsym(lib, symbols[i].name);
        if (!address || symbols[i].bytes != sizeof address) {
            for (u32 n = 0; n < count; ++n)
                memset(symbols[n].slot, 0, symbols[n].bytes);
            dlclose(lib);
            return false;
        }
        memcpy(symbols[i].slot, &address, sizeof address);
    }
    *handle = lib;
    return true;
}
void de__linux_unload(void *handle) {
    if (handle)
        dlclose(handle);
}
#define DE__DL_BIND(name) {#name, &api->name, sizeof api->name},
#if defined(__linux__) && DE_AUDIO_HAS_ALSA
bool de__alsa_load(de__alsa_api *api) {
    memset(api, 0, sizeof *api);
    const de__linux_symbol symbols[] = {DE__ALSA_SYMBOLS(DE__DL_BIND)};
    return de__linux_load("libasound.so.2", symbols, (u32)(sizeof symbols / sizeof symbols[0]),
                          &api->library);
}
#endif
#if defined(__linux__) && DE_AUDIO_HAS_PIPEWIRE
bool de__pw_load(de__pw_api *api) {
    memset(api, 0, sizeof *api);
    const de__linux_symbol symbols[] = {DE__PW_SYMBOLS(DE__DL_BIND)};
    return de__linux_load("libpipewire-0.3.so.0", symbols,
                          (u32)(sizeof symbols / sizeof symbols[0]), &api->library);
}
#endif
#undef DE__DL_BIND
