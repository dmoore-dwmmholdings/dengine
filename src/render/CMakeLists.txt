# src/render: module `render` (target de_render), layer content. SPEC §12.
# Sources are listed one per line; S12 tasks append theirs (DENG-174 renderer + settings,
# DENG-175 render world, DENG-176 views, DENG-177 GPU scene, DENG-178 components).
# graph/: the render graph (DENG-171 builder; DENG-172 compiler and DENG-173 recorder append).
de_add_module(
    NAME render
    LAYER content
    SOURCES
        graph/rg_internal.h
        graph/rg_builder.c
        graph/rg_debug.c
    DEPS gpu asset ecs task
)
