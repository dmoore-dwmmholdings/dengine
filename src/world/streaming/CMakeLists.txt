# src/world/streaming: module `streaming` (target de_streaming), layer gameplay.
# DENG-271: world partition. 256 m streaming cells as bundles, streaming sources, cell origins in
# f64 and far-proxy crossfade state. The cell bundles come from `de-cook world`
# (tools/cook/world_cells.c), which shares the header-only cell math in streaming_internal.h.
de_add_module(
    NAME streaming
    LAYER gameplay
    SOURCES
        streaming_internal.h
        cell_grid.c
        far_proxy.c
        world_partition.c
    DEPS core reflect ecs asset
    OPTIONAL_DEPS physics
)
