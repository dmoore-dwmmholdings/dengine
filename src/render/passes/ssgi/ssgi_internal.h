#ifndef DE_RENDER_SSGI_INTERNAL_H
#define DE_RENDER_SSGI_INTERNAL_H
#include "render/passes/ssr/ssr_internal.h"
DE_EXTERN_C_BEGIN
typedef struct de__ssgi de__ssgi;
typedef struct de__ssgi_info {
    u64 frame;
    u32 width, height, trace_width, trace_height, dispatches;
    bool history_valid;
    f32 previous_exposure, current_exposure;
    de_gpu_texture output, temporal, moments;
    de_gpu_texture rays; /* Borrowed transient conditional incident radiance/confidence. */
} de__ssgi_info;
/** Borrows renderer-owned final-depth/HDR history from SSR's common infrastructure. */
de_result de__ssgi_create(de_renderer *r, de__ssgi **out);
void de__ssgi_destroy(de__ssgi *s);
void de__ssgi_view_complete(de__ssgi *s, de_view view, u64 frame, bool success);
bool de__ssgi_view_info(const de__ssgi *s, de_view view, de__ssgi_info *out);
DE_EXTERN_C_END
#endif
