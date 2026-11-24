# src/world/sky: module `sky` (target de_sky), layer gameplay.
# The physical sky (DENG-206, SPEC §12.13): celestial positions, the CPU atmosphere reference,
# the DeSky system that drives the sun light (de_sky_attach), and, in render builds, the GPU LUTs,
# the sky view stage and the sky as the IBL environment (DENG-838).
de_add_module(
    NAME sky
    LAYER gameplay
    SOURCES sky.c celestial.c atmosphere.c sky_ibl.c sky_internal.h clouds_math.c clouds_internal.h
    DEPS ecs asset
    OPTIONAL_DEPS render
)

if(DE_HAS_RENDER)
  target_sources(de_sky PRIVATE sky_luts.c fog.c fog_volumes.c fog_internal.h clouds.c)
  target_link_libraries(de_sky PRIVATE de_render de_gpu de_ecs)
endif()
