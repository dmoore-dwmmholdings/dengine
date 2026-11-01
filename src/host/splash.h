/* DENG-703: common startup branding; no game assets, UI or renderer instance required. */
#ifndef DE_HOST_SPLASH_H
#define DE_HOST_SPLASH_H
#include <dengine/de_base.h>
#include <dengine/de_platform.h>
struct de__host;
typedef struct de__splash_sample {
    f32 alpha, reveal, settle, text_alpha, background;
    bool done;
} de__splash_sample;
#define DE__SPLASH_DURATION_NS 3000000000ull
/** Pure monotonic timeline; still mode holds one static mark for 650ms. */
de__splash_sample de__splash_at(u64 elapsed_ns, bool still);
/** Policy shared by the dynamic development host and generated static game hosts. */
bool de__splash_enabled(const struct de__host *host);
/** Best effort: logs/unwinds presentation failures, never prevents a game starting.
 * A platform close asks the host to quit before invoking game init. */
void de__host_splash(struct de__host *host);
/** A close on the splash window is honored even if a caller owns another window. */
bool de__splash_quit_event(const de_app_event *event, de_window window);
#if DE_HAS_RENDER
#include <dengine/de_gpu.h>
/** Internal render seams also let the native pixel fixture render the exact startup
 * shader into its own offscreen target, without capturing the desktop. */
typedef struct de__splash_gpu {
    de_gpu_pipeline pipeline;
    de_gpu_texture atlas;
    de_gpu_sampler sampler;
    u32 image_index, sampler_index;
} de__splash_gpu;
de_result de__splash_create(de_gpu_device *device, const de_allocator *allocator,
                            de__splash_gpu *out);
void de__splash_destroy(de_gpu_device *device, de__splash_gpu *resources);
void de__splash_record(de_gpu_cmd *cmd, de_gpu_texture target, const de__splash_gpu *resources,
                       u32 width, u32 height, de__splash_sample sample);
#endif
#endif
