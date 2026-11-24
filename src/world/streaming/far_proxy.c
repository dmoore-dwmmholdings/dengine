/*
 * DENG-271: far-proxy state per cell. A proxy stands in for a cell that is not loaded. When the
 * cell loads, the proxy fades out over `fade_seconds` (alpha 1 -> 0) while the cell fades in
 * (1 - alpha); the renderer dithers both. When the cell unloads, the proxy is back at once (the
 * cell is gone, so there is nothing to fade from). This file owns the timing; `de-cook world`
 * bakes the proxy meshes (tools/cook/world_proxy.c, DENG-830) and world_partition.c draws them
 * with the render module's dithered DeRenderFade.
 */
#include "streaming_internal.h"

#include <dengine/de_asset.h>

void de__far_proxy_reset(de__far_proxy *fp) {
    fp->alpha = 1.0f;
    fp->active = true;
}

void de__far_proxy_step(de__far_proxy *fp, bool cell_loaded, f32 dt, f32 fade) {
    if (!cell_loaded) {
        de__far_proxy_reset(fp);
        return;
    }
    if (!fp->active)
        return;
    if (!(fade > 0.0f) || !(dt >= 0.0f)) {
        fp->alpha = 0.0f;
    } else {
        fp->alpha -= dt / fade;
        if (fp->alpha < 0.0f)
            fp->alpha = 0.0f;
    }
    if (fp->alpha <= 0.0f)
        fp->active = false;
}
