# src/world/spline: module `spline` (target de_spline), layer gameplay.
# Spline components (DeSpline, DeSplineMeshes), curve evaluation, arc-length sampling and mesh
# placement (DENG-775: the DIDE editor IPC world.splines, protocol 1.12). The runtime systems that
# render the placed meshes are DENG-583.
de_add_module(
    NAME spline
    LAYER gameplay
    SOURCES spline.c spline.h
    DEPS core reflect ecs asset
)
