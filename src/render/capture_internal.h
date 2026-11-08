/* DENG-573: private Dev launch capture, owned by the renderer rather than a host callback. */
#ifndef DE_RENDER_CAPTURE_INTERNAL_H
#define DE_RENDER_CAPTURE_INTERNAL_H
#include <dengine/de_render.h>
#if defined(DE_DEV) || defined(DE_DEBUG)
#include "platform/de_atomic.h"
typedef struct de__render_capture {
    de_atomic_u32 state; /* 0 idle, 1 pending, 2 active; release/acquire publishes the mailbox */
    u32 frames, completed;
    char path[1024];
} de__render_capture;
/* Main thread; copies path. A busy request never replaces or queues behind an existing one. */
de_result de__render_capture_request(de_renderer *r, const char *path, u32 frames);
/* Render thread only. Begin after acquisition; finish counts only a successful submitted frame. */
void de__render_capture_begin(de_renderer *r);
void de__render_capture_frame(de_renderer *r, bool success);
/* Render thread, or destruction after the render thread has joined. Never reports completion. */
void de__render_capture_abort(de_renderer *r);
#endif
#endif
