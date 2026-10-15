/*
 * exposure.c: physical camera exposure (SPEC §12.7; DENG-304). A view's exposure comes from its
 * camera's EV100 (DeCamera.exposure_ev100; NaN = automatic, which S18's histogram auto exposure,
 * DENG-160, will provide; until then r.exposure_ev100_default):
 *
 *   exposure = 1 / (1.2 x 2^EV100)
 *
 * (the saturation-based sensitivity of ISO 12232 with K = 12.5 and q = 0.65: L_max = 1.2 x
 * 2^EV100 maps to 1). Passes that write radiance into the HDR targets multiply it by this
 * pre-exposure (the resolve's emission, the deferred lighting), which keeps half floats in range
 * for physical light levels; the output pass then works in display units. Each view keeps this
 * frame's exposure and the previous frame's, which the GPU scene puts in the scene constants
 * (DeSceneConstants.pre_exposure / prev_pre_exposure) for temporal passes to rescale history.
 *
 * The GPU scene's view stage (renderer.c) asks first in a frame, lighting later: the first call of
 * a frame advances the state, later calls of the same frame return the same values.
 */
#include "render/lighting/lighting.h"

#include <math.h>

f32 de__exposure_from_ev100(f32 ev100) {
    if (!isfinite(ev100))
        ev100 = (f32)DE__EXPOSURE_EV100_DEFAULT;
    return 1.0f / (1.2f * exp2f(ev100));
}

f32 de__exposure_update(de__exposure_state *s, de_view view, u64 frame, f32 camera_ev100,
                        f32 fallback_ev100, f32 *prev) {
    if (s->valid && s->view.id == view.id && s->frame == frame) {
        if (prev != NULL)
            *prev = s->previous;
        return s->current;
    }
    const f32 ev = isfinite(camera_ev100)     ? camera_ev100
                   : isfinite(fallback_ev100) ? fallback_ev100
                                              : (f32)DE__EXPOSURE_EV100_DEFAULT;
    const f32 e = de__exposure_from_ev100(ev);
    const bool continues = s->valid && s->view.id == view.id && s->frame + 1u == frame;
    s->previous = continues ? s->current : e;
    s->current = e;
    s->ev100 = ev;
    s->frame = frame;
    s->view = view;
    s->valid = true;
    if (prev != NULL)
        *prev = s->previous;
    return e;
}

f32 de__lighting_view_exposure(de__lighting *lt, u32 slot, de_view view, u64 frame,
                               f32 camera_ev100, f32 *prev) {
    if (lt == NULL || slot >= DE__VIEW_MAX) {
        if (prev != NULL)
            *prev = 1.0f;
        return 1.0f;
    }
    const f32 fallback = DE_HANDLE_VALID(lt->cv_ev100_default)
                             ? (f32)de_cvar_float(lt->cv_ev100_default)
                             : (f32)DE__EXPOSURE_EV100_DEFAULT;
    return de__exposure_update(&lt->exposure[slot], view, frame, camera_ev100, fallback, prev);
}
