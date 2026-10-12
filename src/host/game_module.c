/*
 * game_module.c: attaching the game's de_game_api (SPEC §10.1). Owner of the bodies: DENG-92
 * (library loading, de_add_game()); DENG-93 adds the hot-reload swap.
 *
 * DENG-90 stub: the statically linked game (de_host_desc.static_api) only.
 */
#include "host/host_internal.h"

de_result de__game_api_check(const de_game_api *api) {
    if (api == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "game API table is NULL");
    if (api->api_version != DE_GAME_API_VERSION)
        return DE_FAIL(DE_ERR_VERSION_MISMATCH,
                       "game '%s' was built for game API version %u, this engine has version %u",
                       api->name != NULL ? api->name : "(unnamed)", (unsigned)api->api_version,
                       (unsigned)DE_GAME_API_VERSION);
    return DE_OK;
}

de_result de__game_attach(de__host *h, const de_host_desc *d) {
    if (d->static_api == NULL) {
        if (d->game_library != NULL)
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "game library %s: loading a game library is not supported yet",
                           d->game_library);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "no game: de_host_desc.static_api is NULL");
    }
    DE_TRY(de__game_api_check(d->static_api));
    h->api = d->static_api;
    return DE_OK;
}

void de__game_detach(de__host *h) {
    h->api = NULL;
}
