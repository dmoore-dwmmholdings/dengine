#ifndef DE_HOST_EDITOR_SCENE_LOAD_H
#define DE_HOST_EDITOR_SCENE_LOAD_H
#include "host/editor/editor_internal.h"
#include <dengine/de_asset.h>
/* One transaction owns discovery and exact-reference requests. Only exact is transferred to
 * the committed scene; discovery never keeps unrelated model siblings resident afterwards. */
typedef struct de__scene_ref {
    de_asset_id id, parent;
    char *type;
    size_t type_bytes;
} de__scene_ref;
typedef struct de__scene_assets {
    de_asset_system *assets;
    const de_allocator *allocator;
    de__scene_ref *refs;
    u32 count, cap;
    de_load_handle discovery, exact;
    bool children_started;
} de__scene_assets;
de_result de__scene_assets_begin(de__editor *ed, const char *text, size_t len,
                                 de__scene_assets *out);
/* BUSY while pending; READY and exact typed object validation are required for DE_OK. */
de_result de__scene_assets_poll(de__scene_assets *p);
void de__scene_assets_drop(de__scene_assets *p);
#endif
