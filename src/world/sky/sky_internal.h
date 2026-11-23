/*
 * sky_internal.h: the physical sky (DENG-206, SPEC §12.13): celestial positions, the CPU
 * atmosphere reference and the GPU LUTs. The atmosphere constants mirror
 * shaders/sky/sky_common.slang; keep both in step.
 */
#ifndef DE_SKY_INTERNAL_H
#define DE_SKY_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_math.h>
#if DE_HAS_RENDER
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#endif

DE_EXTERN_C_BEGIN

/* ---------- Atmosphere (sky_common.slang) ---------- */
#define DE__SKY_BOTTOM          6360.0 /* ground radius, km */
#define DE__SKY_TOP             6460.0 /* top of the atmosphere, km */
#define DE__SKY_RAYLEIGH_R      5.802e-3
#define DE__SKY_RAYLEIGH_G      13.558e-3
#define DE__SKY_RAYLEIGH_B      33.1e-3
#define DE__SKY_RAYLEIGH_H      8.0
#define DE__SKY_MIE_SCAT        3.996e-3
#define DE__SKY_MIE_EXT         4.440e-3
#define DE__SKY_MIE_H           1.2
#define DE__SKY_OZONE_R         0.650e-3
#define DE__SKY_OZONE_G         1.881e-3
#define DE__SKY_OZONE_B         0.085e-3
#define DE__SKY_OZONE_CENTER    25.0
#define DE__SKY_OZONE_HALF      15.0
#define DE__SKY_TRANSMITTANCE_W 256u
#define DE__SKY_TRANSMITTANCE_H 64u
#define DE__SKY_MULTISCATTER_N  32u
#define DE__SKY_VIEW_W          192u
#define DE__SKY_VIEW_H          108u

/** Solar illuminance at the top of the atmosphere used by the sky, in lux (DE_SKY_SOLAR_ILLUMINANCE). */
#define DE__SKY_SOLAR_ILLUMINANCE 120000.0f
/** The full moon's illuminance at the top of the atmosphere, in lux. */
#define DE__SKY_MOON_FULL_ILLUMINANCE 0.25f
/** The sun's and the moon's mean angular radius, in radians. */
#define DE__SKY_SUN_RADIUS  0.00467f
#define DE__SKY_MOON_RADIUS 0.0045f
/** The moon's geometric albedo used by the disk. */
#define DE__SKY_MOON_ALBEDO 0.072f

/** The Mie density scale of a turbidity (1 = clear: no aerosols; 2 = the reference). */
f64 de__sky_mie_scale(f64 turbidity);

/** Transmittance from radius r (km) along mu (cosine from zenith) to the top of the atmosphere, in
 * double precision (midpoint rule, `steps` segments): the reference for the GPU LUT. */
void de__sky_transmittance_cpu(f64 r, f64 mu, f64 mie_scale, u32 steps, f64 out_rgb[3]);
/** The transmittance LUT's (r, mu) at uv (texel centres at (i + 0.5) / size), as the shader. */
void de__sky_transmittance_rmu(f64 u, f64 v, f64 *r, f64 *mu);

/* ---------- Celestial (celestial.c) ---------- */

/** Julian day of a UTC calendar date (Gregorian), `hours` UTC since 0 h. */
f64 de__sky_julian_day(i32 year, i32 month, i32 day, f64 hours);

/** A body's horizontal position: altitude (above the horizon) and azimuth (from north, toward
 * east), both in degrees. */
typedef struct de__sky_horizontal {
    f64 altitude_deg;
    f64 azimuth_deg;
} de__sky_horizontal;

/** The sun (NOAA's algorithm, < 0.01 degrees from 1900 to 2100), geometric (no refraction). */
de__sky_horizontal de__sky_sun(f64 jd, f64 latitude_deg, f64 longitude_deg);
/** The sun's equation of time at `jd`, in minutes (apparent minus mean solar time). */
f64 de__sky_equation_of_time(f64 jd);

typedef struct de__sky_moon_state {
    de__sky_horizontal pos;      /**< topocentric, geometric */
    f64 illuminated_fraction;    /**< 0 new .. 1 full */
    f64 phase;                   /**< 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter */
    f64 distance_km;
} de__sky_moon_state;
/** The moon (Meeus ch. 47, the main periodic terms: about 0.1 degrees). `phase_override` >= 0
 * puts the moon at that synodic phase on the ecliptic instead of its real place. */
de__sky_moon_state de__sky_moon(f64 jd, f64 latitude_deg, f64 longitude_deg, f64 phase_override);

/** The Julian day of DeSky's time: `day_of_year` (1 = 1 January 2026) at local apparent solar
 * time `hours` on longitude 0. */
f64 de__sky_local_solar_jd(f64 day_of_year, f64 hours);

/** A unit direction (x east, y north, z up) from a horizontal position. */
de_vec3 de__sky_horizontal_dir(de__sky_horizontal h);

/* ---------- GPU LUTs (sky_luts.c; render builds) ---------- */
#if DE_HAS_RENDER
typedef struct de__sky_luts de__sky_luts;
/** The transmittance and multi-scattering LUTs' state on `dev`: pipelines and textures. No
 * shader code for this backend: DE_OK with LUTs that de__sky_luts_ready() reports off. */
de_result de__sky_luts_create(de_gpu_device *dev, const de_allocator *alloc, de__sky_luts **out);
void de__sky_luts_destroy(de__sky_luts *l);
bool de__sky_luts_ready(const de__sky_luts *l);
/** Records and submits the generation of both LUTs for (mie_scale, albedo) on the graphics
 * queue, when they differ from the last generation. Later work on that queue sees the result. */
de_result de__sky_luts_update(de__sky_luts *l, f32 mie_scale, f32 albedo);
de_gpu_texture de__sky_luts_transmittance(const de__sky_luts *l);
de_gpu_texture de__sky_luts_multiscatter(const de__sky_luts *l);
/** Bindless indices: transmittance, multi-scattering (sampled). */
u32 de__sky_luts_transmittance_bindless(const de__sky_luts *l);
u32 de__sky_luts_multiscatter_bindless(const de__sky_luts *l);
/** The fence and value of the last generation. */
de_gpu_fence de__sky_luts_fence(const de__sky_luts *l, u64 *value);
#endif

/* ---------- The sky's per-frame inputs (sky.c -> sky_luts.c) ---------- */

/** What the render side draws, from the game thread's last update (copied under a mutex). */
typedef struct de__sky_snapshot {
    de_vec3 sun_dir, moon_dir; /**< toward the body; x east, y north, z up */
    f32 sun_weight;            /**< sun illuminance at the top of the atmosphere, lux */
    f32 moon_weight;           /**< moon illuminance at the top of the atmosphere, lux */
    f32 mie_scale, albedo;
    bool valid;
} de__sky_snapshot;

struct de_sky;
/** The latest snapshot of `s` (render thread). false before the first update. */
bool de__sky_snapshot_get(struct de_sky *s, de__sky_snapshot *out);

#if DE_HAS_RENDER
struct de_renderer;
typedef struct de__sky_render de__sky_render;
/** Registers the sky stage on `r` (DE__VIEW_SLOT_TRANSPARENT, before the transparents). */
de_result de__sky_render_create(struct de_renderer *r, struct de_sky *s, const de_allocator *alloc,
                                de__sky_render **out);
void de__sky_render_destroy(de__sky_render *sr);
#endif

DE_EXTERN_C_END

#endif /* DE_SKY_INTERNAL_H */
