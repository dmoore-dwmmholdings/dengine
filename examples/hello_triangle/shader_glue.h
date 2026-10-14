/* shader_glue.h — hello_triangle's access to cooked shaders (DENG-327).
 *
 * The engine has no public API yet that registers the `shader` asset type or hands out a
 * shader's code for de_gpu_pipeline_create: both are internal (src/gpu/shader/shader_internal.h),
 * and the engine registers the type nowhere (DENG-409). shader_glue.c wraps those internals behind
 * this header, which uses public types only, so main.c stays on the public API. When the engine
 * gets a public shader API, this file and shader_glue.c go away.
 */
#ifndef HELLO_SHADER_GLUE_H
#define HELLO_SHADER_GLUE_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

typedef struct hello_shaders hello_shaders;

/** Whether this build can cook `.slang` sources in SOURCE mode (Debug or Dev, with the tools). */
bool hello_shaders_can_cook(void);

/**
 * Registers the `shader` asset type with `assets`: with the Slang importer when this build can
 * cook, else for cooked data only (packs). Destroy the result after the asset system.
 */
de_result hello_shaders_register(de_asset_system *assets, hello_shaders **out);
void hello_shaders_destroy(hello_shaders *h);

/**
 * The code of entry point `entry` of the READY shader `sh` for `backend`, and the push-constant
 * bytes it uses (`*push_bytes`). DE_ERR_NOT_FOUND when the shader was not cooked for the backend's
 * target (for example DXIL without DXC). The blob stays valid while the asset is loaded.
 */
de_result hello_shader_get(de_asset_system *assets, de_shader sh, de_gpu_backend backend,
                           const char *entry, de_gpu_shader_blob *out, u32 *push_bytes);

#endif
