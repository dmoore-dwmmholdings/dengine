#ifndef DE_GOLDEN_COOKED_ASSETS_H
#define DE_GOLDEN_COOKED_ASSETS_H
#include <dengine/de_asset.h>
#if DE_HAS_COOKER
#define DE_GOLDEN_ASSET_MODE DE_ASSET_MODE_SOURCE
#else
#define DE_GOLDEN_ASSET_MODE DE_ASSET_MODE_PACKED
#endif

/* Test-side preparation only. Dev keeps runtime source cooking; Release/Shipping load
 * actual cooked packs. Destroy the asset system before cleaning up its mapped pack. */
de_result de_golden_pack_assets(de_asset_system *assets, const char *asset_root, char path[256]);
de_result de_golden_pack_cleanup(char path[256]);
#endif
