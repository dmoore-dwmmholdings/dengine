#ifndef DE_SCENE_IMPORT_H
#define DE_SCENE_IMPORT_H
#include <dengine/de_asset.h>
/* Descriptors require an asset system with desc.types as their user context. */
de_asset_type_desc de_scene_import_type_desc(void);
de_asset_type_desc de_prefab_import_type_desc(void);
#endif
