#ifndef DE_HOST_GPU_CAPTURE_H
#define DE_HOST_GPU_CAPTURE_H
#include <dengine/de_base.h>
#if defined(DE_DEV) || defined(DE_DEBUG)
typedef struct de__host_capture {
    bool armed, launch, enabled_metal_env;
    u32 frames;
    char directory[896];
} de__host_capture;
struct de__host;
struct de_input;
bool de__host_capture_pressed(const struct de_input *input);
de_result de__host_capture_prepare(struct de__host *h);
void de__host_capture_poll(struct de__host *h);
void de__host_capture_cleanup(struct de__host *h);
#endif
#endif
