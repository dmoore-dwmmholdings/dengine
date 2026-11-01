#ifndef DE_RENDER_UPSCALE_UPSCALER_H
#define DE_RENDER_UPSCALE_UPSCALER_H
#include <dengine/de_render.h>
typedef struct de__upscale_size {
    u32 input_width, input_height, output_width, output_height;
} de__upscale_size;
/** Backend-owned images exposed for graph declarations. Opaque native histories leave these
 * zero. Handles remain valid until resize/destroy; previous writes must finish before reuse. */
typedef struct de__upscale_history {
    de_gpu_texture color[2], depth[2];
} de__upscale_history;
typedef struct de__upscale_inputs {
    de_gpu_texture color, depth, motion;
    de_gpu_texture exposure, reactive; /**< Optional 1x1 exposure and output-space reactive mask. */
    de_gpu_texture history_color, history_depth;
    de__upscale_size size;
    de_vec2 jitter;     /**< Shared pixel offset; positive Y down. */
    de_mat4 projection; /**< Reverse-Z projection, for depth linearization. */
    f32 pre_exposure, previous_pre_exposure;
    f32 delta_seconds, sharpen;
    bool reset, linear_depth;
} de__upscale_inputs;
typedef struct de__upscale_outputs {
    de_gpu_texture color, history_color, history_depth;
} de__upscale_outputs;
/** Init/resize may allocate; steady dispatch records commands only. The graph declares every
 * nonzero shared history/input/output and supplies barriers. A failed submission must not advance
 * history. Resize retains old resources on failure. Destroy after CPU recording is idle. */
typedef struct de__upscaler_backend {
    const char *name;
    de_upscaler kind;
    de_result (*init)(de_renderer *renderer, void **state);
    de_result (*resize)(void *state, de__upscale_size size, de__upscale_history *history);
    void (*dispatch)(void *state, de_gpu_cmd *cmd, const de__upscale_inputs *inputs,
                     const de__upscale_outputs *outputs);
    void (*destroy)(void *state);
} de__upscaler_backend;
/** Vendor availability bits: 1u << DE_UPSCALE_*. TAA is the universal fallback. AUTO prefers
 * MetalFX on Metal, FSR on Vulkan/D3D12; unsupported explicit vendor requests fall back to TAA. */
de_upscaler de__upscaler_select(de_upscaler request, de_gpu_backend backend, u32 available);
/** Negative texture bias while upscaling only; extra bias is finite, clamped to 0..4. */
f32 de__upscaler_mip_bias(de__upscale_size size, f32 extra_bias);
const de__upscaler_backend *de__taa_backend(void);

struct de__view_frame;
typedef struct de__upscalers de__upscalers;
bool de__upscaler_policy(const de_render_settings *settings, const de_view_display *display,
                         bool pixel_art, bool capture);
de_result de__upscalers_create(de_renderer *renderer);
/** Register a supported vendor implementation before rendering starts. The descriptor outlives
 * the renderer. Registration makes it eligible for AUTO; unsupported devices are not registered. */
de_result de__upscalers_register(de_renderer *renderer, const de__upscaler_backend *backend);
void de__upscalers_destroy(de_renderer *renderer);
void de__upscalers_forget(de_renderer *renderer, de_view view);
/** Before visibility/GPU-scene build: choose/reset and apply the shared raster jitter.
 * Backend contexts/history are initialized lazily only when scene HDR reaches the post stage. */
void de__upscalers_prepare(struct de__view_frame *frame);
/** Publish ping-pong state only after the graph successfully submits. */
void de__upscalers_complete(de_renderer *renderer, de_view view, u64 frame, bool submitted);
#endif
