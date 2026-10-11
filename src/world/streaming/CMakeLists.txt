# src/world/streaming: module `streaming` (target de_streaming), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME streaming
    LAYER gameplay
    SOURCES streaming_stub.c
    DEPS ecs asset
    OPTIONAL_DEPS physics
)
