/* de_services.h — Steamworks (optional), mods. SPEC.md §21.1, §21.3.
 * The Steamworks SDK is not vendored; build with -DDE_STEAMWORKS_SDK=<path>. */
#ifndef DE_SERVICES_H
#define DE_SERVICES_H
#include "de_base.h"
#include "de_asset.h"
DE_EXTERN_C_BEGIN

/* ===================== Steamworks ===================== */
typedef struct de_steam de_steam;

DE_API de_result de_steam_init(u32 app_id, de_steam **out); /* DE_ERR_UNSUPPORTED if not built in */
DE_API void de_steam_shutdown(de_steam *s);
DE_API void de_steam_run_callbacks(de_steam *s); /* once per frame */
DE_API bool de_steam_available(const de_steam *s);
DE_API bool de_steam_is_deck(const de_steam *s);

DE_API de_result de_steam_unlock_achievement(de_steam *s, const char *api_name);
DE_API de_result de_steam_set_stat_i32(de_steam *s, const char *name, i32 value);
DE_API de_result de_steam_store_stats(de_steam *s);
DE_API de_result de_steam_cloud_write(de_steam *s, const char *file, const void *data, u32 bytes);
DE_API de_result de_steam_cloud_read(de_steam *s, const char *file, const de_allocator *a,
                                     void **data, u32 *bytes);
DE_API void de_steam_set_rich_presence(de_steam *s, const char *key, const char *value);
DE_API u64 de_steam_local_user_id(const de_steam *s);

/* Lobbies / matchmaking; Steam Networking Sockets is selected via DE_TRANSPORT_STEAM in de_net.h.
 */
typedef struct de_lobby {
    u64 id;
} de_lobby;
DE_API de_result de_steam_lobby_create(de_steam *s, u32 max_members, bool is_public);
DE_API de_result de_steam_lobby_join(de_steam *s, de_lobby lobby);
DE_API de_result de_steam_lobby_set_data(de_steam *s, de_lobby lobby, const char *key,
                                         const char *value);

/* ===================== Mods ===================== */
typedef struct de_mod_info {
    char id[64], name[128], version[32], author[64];
    i32 load_order;
    bool enabled, has_code; /* code mods require the scripting layer (SPEC Q5) */
    char path[260];
} de_mod_info;

typedef struct de_mod_conflict {
    de_asset_id asset;
    char winner[64];
    char loser[64];
} de_mod_conflict;

/* Scans "user:/mods" (+ Steam Workshop if available) for mod.toml manifests. */
DE_API u32 de_mods_scan(de_asset_system *s, de_mod_info *out, u32 max);
DE_API de_result de_mods_set_enabled(de_asset_system *s, const char *mod_id, bool enabled);
DE_API de_result de_mods_set_order(de_asset_system *s, const char *const *mod_ids, u32 count);
/* Mounts enabled mods' packs above the base game in load order. */
DE_API de_result de_mods_apply(de_asset_system *s);
DE_API u32 de_mods_conflicts(de_asset_system *s, de_mod_conflict *out, u32 max);

DE_EXTERN_C_END
#endif
