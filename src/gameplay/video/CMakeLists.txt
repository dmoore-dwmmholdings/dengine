# src/gameplay/video: module `video` (target de_video), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME video
    LAYER gameplay
    SOURCES video_stub.c
    DEPS asset
    OPTIONAL_DEPS render audio
    OPTIONAL_OPTION DE_WITH_VIDEO
)
