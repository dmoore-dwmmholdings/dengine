# src/gpu/shader: module `gpu/shader` (target de_gpu_shader), layer content. SPEC §11.6, §12.2;
# DENG-289. The runtime `shader` asset type: it loads cooked shaders (shader_format.h, written by
# tools/cook/importers/shader/) behind de_shader handles and hands their blobs to pipeline creation.
#
# Its own module, not part of de_gpu: it reads the asset system (de_asset.h and the slot table)
# while de_asset may use de_gpu (OPTIONAL_DEPS gpu), so inside de_gpu it would close the cycle
# asset -> gpu -> asset. tools/check_layers/layers.toml lists it with allow edges to gpu and
# asset (Trama "Decision: gpu/shader is its own module"). Not built without a GPU (DE_HEADLESS).
de_add_module(
    NAME gpu_shader
    LAYER content
    SOURCES
        shader_format.h
        shader_internal.h
        shader_type.c
    DEPS gpu asset
    OPTIONAL_OPTION DE_HAS_GPU
)
