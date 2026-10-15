/*
 * sss.h: screen-space separable subsurface scattering with the Burley profile (SPEC §12.6;
 * DENG-306). Internal to de_render; include as "render/passes/sss/sss.h".
 *
 * Per view and frame, after deferred lighting (the view stage "sss" at (DE__VIEW_SLOT_LIGHTING,
 * DE__SSS_STAGE_ORDER), before the transparent slot), when the view's `de_render_settings.sss` is
 * not DE_QUALITY_OFF. Lighting then wrote each SUBSURFACE pixel's diffuse light only to its
 * diffuse target (lighting.h, the subsurface input; DE_LIGHTING_SSS); this stage blurs it with the
 * Burley profile in two separable passes and adds it back into the HDR lighting target
 * (shaders/sss/sss_separable.slang, engine shader `sss_separable`):
 *
 *   sss.clear    the tile counter to 0                                    counter: WRITE
 *   sss.tiles    8 x 8 tiles holding a G-buffer SUBSURFACE pixel           GB2: READ; tiles,
 *                                                                         counter: WRITE
 *   sss.args     the indirect arguments                                   args: WRITE
 *   sss.blur_h   per SUBSURFACE pixel: the material's amount and mean     G-buffer, depth, vis
 *                free path (vis ID -> visible list -> instance -> material IDs, visible list,
 *                slot), d per channel; the diffuse target blurred along x  scene, diffuse:
 *                                                                         READ; temp, params: WRITE
 *   sss.blur_v   the same pixels: the temp target blurred along y, then   temp, params, diffuse:
 *                lighting += blurred x amount + diffuse x (1 - amount)    READ; lighting: READ,
 *                                                                         WRITE
 *
 * The kernel tables (de__sss_kernel_build): for every odd tap count from DE__SSS_TAPS_MIN to
 * DE__SSS_TAPS_MAX, bins of equal mass of the profile's folded radial distribution in units of
 * d (a centre bin and (taps - 1) / 2 symmetric pairs, each tap at its bin's median), built once
 * when the stage is made and written once into an UPLOAD storage buffer that the kernels read
 * through bindless (it is not a graph resource). A pixel scales them by its largest d and weighs
 * each bin per channel with that channel's exact mass over it (burley_profile.slang), so every
 * channel's weights sum to 1.
 *
 * Threads: create and destroy on the main thread; de__sss_build on the thread that builds the
 * view's graph (the render thread). Memory: the tables and the kernel buffer at create; per frame
 * only transients of the view's graph and pass data on its arena.
 */
#ifndef DE_RENDER_PASSES_SSS_SSS_H
#define DE_RENDER_PASSES_SSS_SSS_H

#include <dengine/de_base.h>
#include <dengine/de_render.h>

#include "render/graph/rg_internal.h"

DE_EXTERN_C_BEGIN

struct de__vis_shared;
struct de__vis_context;
struct de__lighting;
struct de__lighting_context;

/** Taps of a kernel: odd, DE__SSS_TAPS_MIN .. DE__SSS_TAPS_MAX. */
#define DE__SSS_TAPS_MIN 3u
#define DE__SSS_TAPS_MAX 63u
/** Bins of a table of `taps` taps (the centre bin and one per symmetric pair). */
#define DE__SSS_BINS(taps) (((taps) + 1u) / 2u)
/** Every table's bins together (odd tap counts 3..63). */
#define DE__SSS_TABLE_BINS 527u
/** Tile edge (sss_separable.slang DE_SSS_TILE). */
#define DE__SSS_TILE 8u
/** The stage's order in DE__VIEW_SLOT_LIGHTING (after lighting's 0). */
#define DE__SSS_STAGE_ORDER 100
/** SssParams.flags */
enum { DE__SSS_FOLLOW_SURFACE = 1u };

/** One bin of a kernel table (sss_separable.slang reads it as float4), in units of d: the tap's
 * distance from the centre, and the bin [lo, hi) it stands for (hi of the last bin: 1e30). */
typedef struct de__sss_bin {
    f32 pos, lo, hi, reserved;
} de__sss_bin;

/* ---- The profile (burley_profile.slang's functions, in double precision) ---- */

/** The searchlight scaling factor s(A) = 1.85 - A + 7 |A - 0.8|^3 (A clamped to [0, 1]). */
f64 de__burley_scale(f64 albedo);
/** d = mean free path / s(albedo). */
f64 de__burley_d(f64 mfp, f64 albedo);
/** R(r) = (e^(-r/d) + e^(-r/(3d))) / (8 pi d r). */
f64 de__burley_profile(f64 r, f64 d);
/** The distribution of the distance: 1 - e^(-r/d)/4 - 3 e^(-r/(3d))/4 (1 for d <= 0). */
f64 de__burley_cdf(f64 r, f64 d);
/** The radius (for d = 1) where the distribution reaches u in [0, 1). */
f64 de__burley_inverse_unit(f64 u);

/* ---- Kernels ---- */

/** The bins of the table of `taps` (odd, clamped to the range) into `out`
 * (DE__SSS_BINS(taps) entries). Pure; deterministic. */
void de__sss_kernel_build(u32 taps, de__sss_bin *out);
/** The first bin of the table of `taps` in the table of every table (de__sss_kernel_all). */
u32 de__sss_kernel_first(u32 taps);
/** Every table, odd tap counts from 3 to 63, DE__SSS_TABLE_BINS bins (built once, static). */
const de__sss_bin *de__sss_kernel_all(void);
/**
 * The weights of the table `bins` (`count` bins) for channel shapes `d` (metres) at reference
 * d_ref (the largest of them): `w[k][c]` is bin k's mass under d[c] (each tap of a pair takes
 * half). They are what sss_separable.slang computes per pixel.
 */
void de__sss_kernel_weights(const de__sss_bin *bins, u32 count, const f64 d[3], f64 (*w)[3]);
/** The taps for quality `q` (0 for DE_QUALITY_OFF): `samples` when it is > 0 (made odd and
 * clamped), else 11 (low), 17 (medium), 25 (high, ultra). */
u32 de__sss_taps(de_quality q, i64 samples);

/* ---- The stage ---- */

typedef struct de__sss de__sss;

/** What one view's SSS added this frame (graph resources of its graph). */
typedef struct de__sss_context {
    u32 taps;             /**< the kernel's taps */
    de_rg_resource tiles; /**< raw u32 tile entries */
    de_rg_resource counter, args;
    de_rg_resource temp;   /**< RGBA16F: the x blur */
    de_rg_resource params; /**< RGBA16F: d rgb (metres), amount */
} de__sss_context;

/** The stage's state without a renderer (tests): loads the kernels and uploads the tables. */
de_result de__sss_create_ex(struct de__vis_shared *vis, de__sss **out);
/** The same, and registers the view stage "sss" (views.c; after lighting). */
de_result de__sss_create(de_renderer *r, struct de__vis_shared *vis, struct de__lighting *lt,
                         de__sss **out);
void de__sss_destroy(de__sss *ss);
/** False when the build has no code for the backend (the stage then adds nothing). */
bool de__sss_enabled(const de__sss *ss);

/**
 * Adds the SSS passes of view context `vc` over what lighting published in `lc` (its diffuse
 * target and lighting target) at quality `q`. Adds nothing (and `*out` NULL) for
 * DE_QUALITY_OFF, without a diffuse target, a G-buffer or the kernels. Render thread.
 */
de_result de__sss_build(de__sss *ss, de_rg *g, struct de__vis_context *vc,
                        const struct de__lighting_context *lc, de_quality q, de__sss_context **out);

/** The `public static const uint` values of the SSS shaders, for the layout test. */
typedef struct de__sss_const {
    const char *name;
    u32 value;
} de__sss_const;
const de__sss_const *de__sss_layout_consts(u32 *count);
/** sizeof SssParams (sss_separable.slang). */
u32 de__sss_params_bytes(void);

DE_EXTERN_C_END

#endif /* DE_RENDER_PASSES_SSS_SSS_H */
