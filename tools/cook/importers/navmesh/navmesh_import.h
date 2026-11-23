/*
 * navmesh_import.h: the cooker's built-in `navmesh` importer (DENG-76). A `.navmesh` source is
 * TOML: build settings, an optional region and the walkable geometry (boxes and triangle meshes);
 * src/gameplay/nav/nav_cook.c documents the format. The importer builds every tile with
 * Recast / Detour and writes a DNAV container (src/gameplay/nav/nav_build.h), which the runtime
 * `navmesh` asset type loads. Deterministic: the same source cooks to the same bytes.
 */
#ifndef DE_COOK_IMPORTERS_NAVMESH_IMPORT_H
#define DE_COOK_IMPORTERS_NAVMESH_IMPORT_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

#define DE_NAVMESH_IMPORTER_VERSION 1u

de_result de_navmesh_import(de_asset_import_ctx *ctx);
de_asset_type_desc de_navmesh_import_type_desc(void);

DE_EXTERN_C_END

#endif /* DE_COOK_IMPORTERS_NAVMESH_IMPORT_H */
