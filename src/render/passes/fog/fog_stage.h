/*
 * fog_stage.h: the renderer's froxel fog (DENG-861, follow-up of DENG-849). The renderer owns the
 * de__fog and registers the view stage "fog" (DE__VIEW_SLOT_TRANSPARENT, order DE__FOG_STAGE_ORDER)
 * once, when it is created, so a world with a DeFogVolume gets fog without de_sky_attach.
 *
 * The sky's stage ("sky", order -100, sky_luts.c) runs first and, when it draws the sky, adds the
 * fog itself through de__fog_stage_add with the aerial perspective behind it (the same de__fog,
 * the same history). The "fog" stage then finds lc->fog set and does nothing. When the sky does
 * not draw (no sky attached, an HDRI environment lights the view, the LUTs are not ready), the
 * "fog" stage adds the fog alone, with no aerial perspective.
 */
#ifndef DE_RENDER_PASSES_FOG_FOG_STAGE_H
#define DE_RENDER_PASSES_FOG_FOG_STAGE_H

#include "render/passes/fog/fog_internal.h"

#if DE_HAS_RENDER

DE_EXTERN_C_BEGIN

struct de_renderer;
struct de__view_frame;
struct de__vis_context;
struct de__lighting_context;

/** The "fog" stage sits after the sky stage (-100) and before the transparents' draw. */
#define DE__FOG_STAGE_ORDER (-99)

typedef struct de__fog_stage de__fog_stage;

/** Creates the fog and registers the "fog" stage on `r`. A fog without pipelines (a backend with
 * no shader code) is not an error: the stage then adds nothing. Main thread. */
de_result de__fog_stage_create(struct de_renderer *r, de__fog_stage **out);
/** Unregisters the stage and frees the fog. The caller waited for the GPU. NULL is ignored. */
void de__fog_stage_destroy(struct de_renderer *r, de__fog_stage *fs);

/** The aerial perspective behind the fog (all zero: none). */
typedef struct de__fog_aerial {
    de_rg_resource texture;
    u32 sampler;
    f32 range_km;
} de__fog_aerial;

/** The renderer's fog (the sky's stage asks for its readiness and max distance); NULL: none. */
de__fog *de__fog_stage_fog(const de__fog_stage *fs);

/** Adds the fog passes of one view and publishes the volume in `lc` (lc->fog, fog_header).
 * Nothing when volumetrics are off, no volume has density, or the fog is not ready. `pre` is the
 * view's pre-exposure. Render thread, from a view stage. */
void de__fog_stage_add(de__fog_stage *fs, const struct de__view_frame *vf,
                       struct de__vis_context *vc, struct de__lighting_context *lc, f32 pre,
                       const de__fog_aerial *aerial);

DE_EXTERN_C_END

#endif /* DE_HAS_RENDER */
#endif /* DE_RENDER_PASSES_FOG_FOG_STAGE_H */
