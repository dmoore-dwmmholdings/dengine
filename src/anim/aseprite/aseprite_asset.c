#include "anim/anim_internal.h"
#include "asset/asset_internal.h"
#include "asset/slots_internal.h"

/* An Aseprite's primary object and its layer sub-assets share the sprite loader and
 * handle family. Texture sub-assets retain the renderer's ordinary texture loader. */
de_result de__aseprite_register_asset(de_anim_system *s) {
    if (de__asset_type_find(s->assets, "aseprite").id)
        return DE_OK;
    de_asset_type_desc descriptor = {.name = "aseprite"};
    (void)de__asset_builtin_importer("aseprite", &descriptor);
    descriptor.load = de__sprite_asset_load;
    descriptor.unload = de__sprite_asset_unload;
    descriptor.user = (void *)s->alloc;
    if (!de_asset_type_register(s->assets, &descriptor).id)
        return DE_FAIL(DE_ERR_INTERNAL, "Aseprite asset registration");
    return DE_OK;
}
