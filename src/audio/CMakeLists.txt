# src/audio: module `audio` (target de_audio), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME audio
    LAYER gameplay
    SOURCES audio_stub.c
    DEPS platform asset ecs
    OPTIONAL_OPTION DE_WITH_AUDIO
)
