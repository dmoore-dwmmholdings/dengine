# src/anim: module `anim` (target de_anim), layer gameplay.
# anim_stub.c keeps the library non-empty until the animation runtime (S19, S27) adds its code.
# DENG-104: formats/ holds the cooked skeleton and clip formats (header-only, read by the model
# importer in tools/cook/importers/model/ and by the animation code) and the reference clip
# sampler (clip_decode.c).
de_add_module(
    NAME anim
    LAYER gameplay
    SOURCES
        anim_stub.c
        formats/de_cooked_skeleton.h
        formats/de_cooked_clip.h
        formats/clip_decode.c
    DEPS ecs asset
    OPTIONAL_DEPS render
)

# DENG-201 visual playback; the cooked reference decoder retains its arithmetic contract.
target_sources(de_anim PRIVATE anim.c anim_assets.c clip_sample.c pose.c anim_internal.h)
if(MSVC)
    set_source_files_properties(formats/clip_decode.c PROPERTIES COMPILE_OPTIONS "/fp:strict")
else()
    set_source_files_properties(formats/clip_decode.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
