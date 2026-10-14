/* shader_glue.c — the only hello_triangle file that uses engine internals (DENG-327). See
 * shader_glue.h. It is listed as an exception in tools/check_layers/layers.toml
 * ([[example_internal]]); every other file under examples/ includes only public headers.
 *
 * It also holds the D3D12 Agility SDK exports, which must be in the executable (DENG-285).
 */
#include "shader_glue.h"

#include "gpu/shader/shader_internal.h"

#if defined(DE_HAS_COOKER) && defined(HELLO_HAS_SHADER_IMPORTER)
#include "cook/importers/shader/shader_importer.h"
#define HELLO_CAN_COOK 1
#else
#define HELLO_CAN_COOK 0
#endif

#if defined(_WIN32) && defined(HELLO_D3D12_AGILITY)
#include "gpu/d3d12/d3d12_agility_exports.h"
#endif

struct hello_shaders {
    de__shader_types *types;
};

bool hello_shaders_can_cook(void) {
    return HELLO_CAN_COOK != 0;
}

de_result hello_shaders_register(de_asset_system *assets, hello_shaders **out) {
    static hello_shaders h; /* one per process is all the example needs */
    de__shader_type_desc d = {0};
#if HELLO_CAN_COOK
    d.import = de_shader_import;
    d.importer_version = de_shader_importer_version();
#endif
    DE_TRY(de__shader_type_register(assets, &d, &h.types));
    *out = &h;
    return DE_OK;
}

void hello_shaders_destroy(hello_shaders *h) {
    if (h != NULL) {
        de__shader_types_destroy(h->types);
        h->types = NULL;
    }
}

de_result hello_shader_get(de_asset_system *assets, de_shader sh, de_gpu_backend backend,
                           const char *entry, de_gpu_shader_blob *out, u32 *push_bytes) {
    de__shader_reflection refl;
    DE_TRY(de__shader_blob(assets, sh, backend, 0, entry, out, &refl));
    *push_bytes = refl.push_constant_bytes;
    return DE_OK;
}
