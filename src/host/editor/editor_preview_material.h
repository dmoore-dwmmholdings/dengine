#ifndef DE_EDITOR_PREVIEW_MATERIAL_H
#define DE_EDITOR_PREVIEW_MATERIAL_H
#include "render/loaders/preview_geometry.h"
#include <dengine/de_asset.h>

typedef struct de__ed_material_meshes {
    de_mesh handles[DE__RENDER_PREVIEW_SHAPES];
} de__ed_material_meshes;

/* Main thread, private preview asset system only. Normal GPU-only mesh staging and retirement;
 * one fixed primitive per shape, held until that asset system is destroyed. Busy while uploading.
 * Does not write files, change the manifest, or publish into the game's asset system. */
de_result de__ed_material_mesh(de_asset_system *assets, de__ed_material_meshes *meshes,
                               de__render_preview_shape shape, de_mesh *out);
#endif
