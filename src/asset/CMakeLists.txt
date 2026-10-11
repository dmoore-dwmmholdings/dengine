# src/asset: module `asset` (target de_asset), layer content.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME asset
    LAYER content
    SOURCES asset_stub.c
    DEPS core platform task reflect
    OPTIONAL_DEPS gpu
)
