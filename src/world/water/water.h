/*
 * water.h: the buoyancy component and the CPU water model (DENG-776; the DIDE editor IPC
 * world.water, protocol 1.13).
 *
 * A water body is an entity with DeWater (include/dengine/de_render.h): an ocean, a river or a
 * lake. DeBuoyancy on a physics body holds probe spheres in the body's local space.
 *
 * The model here is the one the DIDE mock engine uses (mock-engine/src/water.rs), so the IDE and
 * the engine agree on every number:
 * - Significant wave height: Pierson-Moskowitz 0.21 U^2 / g, scaled 0.9 (Phillips), 1.3 (JONSWAP)
 *   or 1 (Pierson-Moskowitz); 0 for a river or a lake.
 * - Ocean surface: four waves along and around the wind, from the peak wavenumber of a fully
 *   developed sea, g / (1.14 U)^2, with a second harmonic scaled by choppiness, moving with game
 *   time.
 * - Lake: flat at the entity's height + surface_height inside extent (x, z), centred on the
 *   entity and axis-aligned. River: inside width / 2 of its spline curve (across the ground), at
 *   the curve's height + surface_height.
 * - Flow map: along the curve, flow_speed * (1 - (d / (width / 2))^2) inside the river, 0 outside.
 * - Probe force: rho g V s, rho 1000 kg/m^3, g 9.81 m/s^2, V the probe sphere's volume and s its
 *   submerged share, (depth + r) / 2r clamped to 0..1.
 *
 * It is the reference until the runtime water lands: the FFT ocean (DENG-268), rivers and lakes
 * (DENG-269) and the deterministic CPU heights and buoyancy system (DENG-270) replace the
 * evaluation, and the editor service keeps calling these functions. Not deterministic (libm).
 *
 * Include as "world/water/water.h". Internal to the engine.
 */
#ifndef DE_WORLD_WATER_H
#define DE_WORLD_WATER_H

#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_render.h>

#include "world/spline/spline.h"

DE_EXTERN_C_BEGIN

#define DE_WATER_COMPONENT     "DeWater"
#define DE_BUOYANCY_COMPONENT  "DeBuoyancy"
#define DE_WATER_MAX_PROBES    8u
#define DE_WATER_MAX_SURFACE   64u  /* water.surface samples per side */
#define DE_WATER_MAX_FLOW      128u /* water.flowMap texels along the longer side */
#define DE_WATER_GRAVITY       9.81f
#define DE_WATER_DENSITY       1000.0f

/** Buoyancy probes of a physics body (DIDE world.water). `count` of `probes` are used; each is a
 * sphere of `radius` m at an offset in the body's local space. */
typedef struct DeBuoyancy {
    u32 count;
    de_vec3 probes[DE_WATER_MAX_PROBES];
    f32 radius;
} DeBuoyancy;

/** Registers DeBuoyancy in `w` (reusing a type already registered with the same layout). DeWater
 * and its enums are render components (de_render_register_components). */
de_result de__water_register(de_world *w, de_component *buoyancy);

/** A water body, in world space. */
typedef struct de_water_body {
    de_water_kind kind;
    de_wave_spectrum spectrum;
    de_vec2 wind;
    f32 choppiness;
    f32 level;      /* world y of the surface: origin y + surface_height */
    de_vec3 origin; /* the entity's world position */
    de_vec3 extent;
    f32 width, flow_speed;
    const de_spline_poly *river; /* a river's curve in world space; NULL: none */
} de_water_body;

/** Decodes `c` at world position `origin`. `river` (may be NULL) is the curve of its spline. */
void de__water_body(const DeWater *c, de_vec3 origin, const de_spline_poly *river,
                    de_water_body *out);
/** Significant wave height (m); 0 for a river or a lake. */
f32 de__water_wave_height(const de_water_body *b);
/** The surface's world y over (x, z) at game time `t` s. False where the body is not (outside a
 * lake, off a river, or a river without a curve). */
bool de__water_surface_at(const de_water_body *b, f32 x, f32 z, f32 t, f32 *y);

typedef struct de_water_flow {
    f32 origin[2], size[2]; /* world (x, z) of the first texel corner; metres along x and z */
    u32 width, height;      /* texels */
    f32 max_speed;
} de_water_flow;
/** A river's flow map at `resolution` (2..DE_WATER_MAX_FLOW) texels along the longer side.
 * `flow` takes 2 floats per texel, rows along +z (2 * DE_WATER_MAX_FLOW^2 floats). False when
 * `b` is not a river with a curve. */
bool de__water_flow_map(const de_water_body *b, u32 resolution, f32 *flow, de_water_flow *out);

/** The upward force (N) on a probe sphere of `radius` at `depth` m below the surface. */
f32 de__water_probe_force(f32 radius, f32 depth);

DE_EXTERN_C_END

#endif /* DE_WORLD_WATER_H */
