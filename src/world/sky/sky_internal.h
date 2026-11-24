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
/** The aerial-perspective volume (DENG-840): N x N screen cells x N depth slices, as a 2D atlas
 * N * N wide and N high (slice s at x in [s N, s N + N)). */
#define DE__SKY_AERIAL_N        32u
/** The distance the volume covers, km. Farther surfaces take the last slice. */
#define DE__SKY_AERIAL_RANGE_KM 32.0f

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

/** The aerial-perspective depth mapping, as sky_common.slang: slice coordinate w (0..1) to the
 * distance from the camera (km), and back (clamped to 0..1). */
f64 de__sky_aerial_distance(f64 w, f64 range_km);
f64 de__sky_aerial_slice(f64 dist_km, f64 range_km);
/** Transmittance from radius r (km, on +Z) along the unit direction `dir` (+Z up) over `dist_km`,
 * in double precision (midpoint rule, `steps` segments, the medium clamped to the ground as the
 * shader): the reference for the aerial-perspective volume's alpha. */
void de__sky_aerial_transmittance_cpu(f64 r, const f64 dir[3], f64 dist_km, f64 mie_scale,
                                      u32 steps, f64 out_rgb[3]);

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
/** The LUTs' linear clamp sampler (bindless). */
u32 de__sky_luts_sampler_bindless(const de__sky_luts *l);
/** The fence and value of the last generation. */
de_gpu_fence de__sky_luts_fence(const de__sky_luts *l, u64 *value);

/** The push constants of the aerial-perspective volume (shaders/sky/aerial_perspective.slang,
 * cs_aerial; 96 bytes). de__sky_luts_aerial_record fills idx[1..3]. */
typedef struct de__sky_aerial_params {
    u32 idx[4];    /**< target (storage), transmittance, multiscatter, clamp sampler */
    f32 ray00[4];  /**< camera ray at the top-left corner (world, unnormalized); w: radius km */
    f32 ray_du[4]; /**< its change across the width; w: range km */
    f32 ray_dv[4]; /**< its change down the height; w: mie scale */
    f32 sun[4];    /**< direction, weight (illuminance x pre-exposure) */
    f32 moon[4];   /**< direction, weight */
} de__sky_aerial_params;
/** Whether this backend has the aerial-perspective pipelines. */
bool de__sky_luts_aerial_ready(const de__sky_luts *l);
/** Records the volume's build into `cmd` for `p` (its idx[0]: the bindless storage index of a
 * DE__SKY_AERIAL_N^2 x DE__SKY_AERIAL_N float4 texture). The LUTs must be generated. */
void de__sky_luts_aerial_record(const de__sky_luts *l, de_gpu_cmd *cmd, de__sky_aerial_params *p);
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

/* ---------- The sky as the IBL environment (sky_ibl.c; DENG-838) ---------- */
/** The sky environment's face size, mips (128 .. 1) and the mip its SH9 is projected from. */
#define DE__SKY_IBL_SIZE   128u
#define DE__SKY_IBL_MIPS   8u
#define DE__SKY_IBL_SH_MIP 3u
/** The sun's turn (degrees) that regenerates the environment. */
#define DE__SKY_IBL_SUN_DEG 0.5f
/** The viewer's altitude change (km) that regenerates it. */
#define DE__SKY_IBL_ALT_KM 0.1f

/** Projects a face strip (`n` x 6 `n` RGBA16F texels, rows `pitch_texels` apart; the face order
 * and directions of de_cooked_env.h) of radiance onto the SH9 irradiance E_k = A_l L_k
 * (de_cooked_env.h's basis and A_l). Pure. */
void de__sky_ibl_project_sh(const u16 *texels, u32 n, u32 pitch_texels, f32 out[9][3]);
/** True when the environment generated for (`last`, `last_r_km`) is stale for (`now`, `r_km`):
 * the sun or the moon turned more than DE__SKY_IBL_SUN_DEG, the moon's or the sun's weight, the
 * mie scale or the albedo changed, or the altitude moved more than DE__SKY_IBL_ALT_KM. Pure. */
bool de__sky_ibl_stale(const de__sky_snapshot *last, f32 last_r_km, const de__sky_snapshot *now,
                       f32 r_km);

#if DE_HAS_RENDER
struct de__ibl_sky;
typedef struct de__sky_ibl de__sky_ibl;
/** The generator's pipelines and its persistent textures and buffers on `dev` (every allocation is
 * here: generating allocates nothing). No shader code for this backend: DE_OK with a generator
 * that de__sky_ibl_ready() reports off. */
de_result de__sky_ibl_create(de_gpu_device *dev, const de_allocator *alloc, de__sky_ibl **out);
/** Releases it (the textures' destruction waits for the GPU). Clear the lighting's sky first. */
void de__sky_ibl_destroy(de__sky_ibl *ibl);
bool de__sky_ibl_ready(const de__sky_ibl *ibl);
/** Whether a generation is in flight. */
bool de__sky_ibl_pending(const de__sky_ibl *ibl);
/** True when no environment was generated yet or the last one is stale for (`sn`, `r_km`). */
bool de__sky_ibl_wants(const de__sky_ibl *ibl, const de__sky_snapshot *sn, f32 r_km);
/** Records and submits a generation for `sn` at radius `r_km` on the graphics queue: the sky into
 * the cube's mip 0, the probe downsample and GGX prefilter (S14), the copy into the back strip
 * and the readback of the SH mip. The LUTs must be generated (earlier on the queue). Refused
 * (DE_ERR_BUSY) while one is in flight. */
de_result de__sky_ibl_generate(de__sky_ibl *ibl, const de__sky_luts *luts,
                               const de__sky_snapshot *sn, f32 r_km);
/** Completes a finished generation: projects its SH9 and makes its strip the current one. `wait`:
 * block until it finishes (tests). True when a new environment became current in this call. */
bool de__sky_ibl_poll(de__sky_ibl *ibl, bool wait);
/** The current environment (valid false before the first generation completes). */
void de__sky_ibl_current(const de__sky_ibl *ibl, struct de__ibl_sky *out);
#endif

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
