# src/world/water: module `water` (target de_water), layer gameplay.
# The DeBuoyancy component and the CPU water model (wave height, surface heights, river flow map,
# probe forces) behind the DIDE editor IPC world.water, protocol 1.13 (DENG-776). DeWater is a
# render component. The runtime FFT ocean, rivers and buoyancy are DENG-268, DENG-269, DENG-270.
de_add_module(
    NAME water
    LAYER gameplay
    SOURCES water.c water.h
    DEPS core reflect ecs asset spline
    OPTIONAL_DEPS render physics
)
