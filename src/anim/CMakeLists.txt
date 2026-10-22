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
target_sources(de_anim PRIVATE anim.c anim_system.c anim_assets.c anim_reflect.c anim_math.h anim_math_fast.c clip_sample.c sampling.c pose.c pose.h anim_internal.h)
target_sources(de_anim PRIVATE sprite_clip_format.h)
target_sources(de_anim PRIVATE sprite_clip.c sprite_anim.c)
target_sources(de_anim PRIVATE anim_clock.c anim_deterministic.c anim_checksum.c pose_deterministic.c)
target_sources(de_anim PRIVATE blend.c blend.h)
# Shared inline math consumers and the complete fixed evaluation call graph.
de_mark_deterministic_sources(anim.c anim_system.c anim_assets.c anim_reflect.c anim_stub.c
    sampling.c formats/clip_decode.c anim_clock.c anim_deterministic.c anim_checksum.c
    pose_deterministic.c blend.c)
