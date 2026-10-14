/* golden.h — the scene API of the golden-image harness `de-golden` (DENG-329, SPEC §19.2).
 *
 * A scene is one function that records GPU work through de_gpu into an offscreen target. The
 * harness creates the device for --backend, the target (scenes.toml: size, format), one command
 * list on the graphics queue, calls the scene, then reads the target back and compares it with
 * tests/golden/refs/<backend>/<tier>/<scene>.png using LDR-FLIP (flip/flip.h) and the scene's
 * thresholds. docs/testing/golden.md describes the whole flow; this header is what a scene file
 * needs.
 *
 * Adding a scene:
 *   1. In a file under tests/golden/scenes/ (listed in tests/golden/CMakeLists.txt):
 *          DE_GOLDEN_SCENE(my_scene) {
 *              de_golden_begin_render(ctx, (const float[4]){0, 0, 0, 1}, (de_gpu_texture){0});
 *              ...record into ctx->cmd...
 *              de_golden_end_render(ctx);
 *          }
 *   2. A [scene.my_scene] table in tests/golden/scenes.toml (thresholds, backends, tiers, caps).
 *      CMake reads the table names, so each scene gets one CTest per backend.
 *   3. Run `de-golden --backend <b> --scene my_scene --update-refs` on a machine of each backend
 *      (never on CI) and commit the PNGs under tests/golden/refs/.
 *
 * Rules for a scene:
 *   - Fixed settings only: no time, no randomness without a fixed seed, no dependence on the
 *     machine except through the backend. The same scene must give the same image on every run.
 *   - Record into ctx->cmd only; the harness submits it once, after the scene returns, and waits.
 *     Helpers below record their uploads into the same list, before the scene's own commands, so
 *     call them before you draw.
 *   - The target starts in DE_ACCESS_NONE with undefined contents. Move it with
 *     de_golden_begin_render() / de_golden_target_barrier(); the harness copies it from whatever
 *     access ctx->target_access names when the scene returns.
 *   - Everything made through the helpers is destroyed by the harness after the GPU is done.
 *     Objects a scene creates itself with de_gpu_* it must destroy itself (deferred destruction
 *     makes that safe right after recording).
 *   - Buffers that shaders read must be GPU_ONLY (de_golden_storage_buffer), not UPLOAD memory:
 *     shaders access de_buffer<T> as a read-write structured buffer, which UPLOAD memory cannot
 *     back on D3D12 (DENG-327).
 *   - On a problem, call de_golden_fail() or de_golden_skip() and return. The helpers already
 *     fail the scene when they cannot do their work.
 *
 * Test code: it includes engine internals (the shader cooker) through golden.c only.
 */
#ifndef DE_GOLDEN_H
#define DE_GOLDEN_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

DE_EXTERN_C_BEGIN

typedef struct de_golden_run de_golden_run; /* harness-private state of one scene run */

/** What a scene gets. Fields are set by the harness; a scene changes only `target_access`
 * (through the helpers, or by hand after its own target barriers). */
typedef struct de_golden_ctx {
    de_gpu_device *device;
    const de_gpu_caps *caps;
    de_gpu_backend backend;
    const char *tier;      /**< "base" or "rt": the tier this run renders and compares at */
    de_gpu_cmd *cmd;       /**< graphics queue, recording; submitted by the harness */
    de_gpu_texture target; /**< RENDER_TARGET | COPY_SRC | COPY_DST | SAMPLED */
    de_gpu_format format;  /**< DE_FMT_RGBA8_UNORM or DE_FMT_RGBA16_FLOAT (scenes.toml) */
    u32 width, height;     /**< scenes.toml `size`, default 512 x 512 */
    u32 target_access;     /**< de_gpu_access the target is in now */
    de_golden_run *run;
} de_golden_ctx;

typedef void (*de_golden_scene_fn)(de_golden_ctx *ctx);

/** Registers a scene (DE_GOLDEN_SCENE does it before main). Names are lower_snake_case and
 * unique; each needs a [scene.<name>] table in scenes.toml. */
void de_golden_register(const char *name, de_golden_scene_fn fn);

/* Runs `fn` before main(): the CRT initialiser section on MSVC / clang-cl (kept alive with
 * /include), the constructor attribute elsewhere. The same technique as tests/unit/de_test.h. */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#if defined(_WIN64)
#define DE__GOLDEN_SYM_PREFIX ""
#else
#define DE__GOLDEN_SYM_PREFIX "_"
#endif
#define DE__GOLDEN_CONSTRUCTOR(fn)                                                                 \
    static void fn(void);                                                                          \
    __declspec(allocate(".CRT$XCU")) void (*fn##_ptr)(void) = fn;                                  \
    __pragma(comment(linker, "/include:" DE__GOLDEN_SYM_PREFIX #fn "_ptr")) static void fn(void)
#else
#define DE__GOLDEN_CONSTRUCTOR(fn)                                                                 \
    static void fn(void) __attribute__((constructor));                                             \
    static void fn(void)
#endif

/** Defines and registers scene `name`; the body follows, with `de_golden_ctx *ctx` in scope. */
#define DE_GOLDEN_SCENE(name)                                                                      \
    static void de__golden_scene_##name(de_golden_ctx *ctx);                                       \
    DE__GOLDEN_CONSTRUCTOR(de__golden_register_##name) {                                           \
        de_golden_register(#name, de__golden_scene_##name);                                        \
    }                                                                                              \
    static void de__golden_scene_##name(de_golden_ctx *ctx)

/* ---------- Outcome ---------- */

/** Fails the scene with a message (printf format). Recording may go on; the image is still read
 * back and written, but the run fails. */
void de_golden_fail(de_golden_ctx *ctx, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
/** Skips the scene on this device (printf format), for example a missing feature that scenes.toml
 * `requires` cannot name. */
void de_golden_skip(de_golden_ctx *ctx, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
/** True once the scene failed or was skipped: return without recording more. */
bool de_golden_stopped(const de_golden_ctx *ctx);

/* ---------- Shaders (tests/golden/shaders/, cooked for the backend at first use) ---------- */

/** One cooked entry point. */
typedef struct de_golden_shader {
    de_gpu_shader_blob blob;
    u32 push_constant_bytes; /**< on this backend's target */
    u32 threadgroup[3];      /**< compute, mesh and amplification entry points */
} de_golden_shader;

/**
 * Entry point `entry` of tests/golden/shaders/`file`, cooked (Slang) for the backend's target.
 * The first call per file cooks it; the blob stays valid for the process. On failure the scene
 * fails, or skips when the build or host cannot cook the target at all (no Slang, no DXC).
 */
bool de_golden_get_shader(de_golden_ctx *ctx, const char *file, const char *entry,
                          de_golden_shader *out);

/** Creates a pipeline (destroyed by the harness). An invalid handle fails the scene with the
 * error chain; DE_ERR_UNSUPPORTED skips it. */
de_gpu_pipeline de_golden_pipeline(de_golden_ctx *ctx, const de_gpu_pipeline_desc *desc);

/* ---------- Resources (destroyed by the harness after the GPU is done) ---------- */

/** A GPU_ONLY buffer with DE_USAGE_STORAGE | DE_USAGE_COPY_DST | `extra_usage`, holding `data`
 * (NULL: undefined contents) through a copy recorded into ctx->cmd, left in
 * DE_ACCESS_SHADER_WRITE so shaders can read and write it. */
de_gpu_buffer de_golden_storage_buffer(de_golden_ctx *ctx, const void *data, u64 size, u32 stride,
                                       u32 extra_usage, const char *name);
/** An UPLOAD buffer with `usage` holding `data`: for index buffers and copy sources, never for
 * buffers that shaders read. */
de_gpu_buffer de_golden_upload_buffer(de_golden_ctx *ctx, const void *data, u64 size, u32 usage,
                                      const char *name);
/** A width x height RGBA8_UNORM texture with SAMPLED usage holding `rgba` (tightly packed rows),
 * uploaded through ctx->cmd and left in DE_ACCESS_SHADER_READ. */
de_gpu_texture de_golden_texture_rgba8(de_golden_ctx *ctx, u32 width, u32 height, const u8 *rgba,
                                       const char *name);
/** A D32_FLOAT depth texture of the target's size, in DE_ACCESS_DEPTH_WRITE. */
de_gpu_texture de_golden_depth_target(de_golden_ctx *ctx);
de_gpu_sampler de_golden_sampler(de_golden_ctx *ctx, const de_gpu_sampler_desc *desc);

/* ---------- Target ---------- */

/** Moves the target to access `after` (from ctx->target_access) and records it there. */
void de_golden_target_barrier(de_golden_ctx *ctx, u32 after);
/** Moves the target to RENDER_TARGET and begins a render pass on it, cleared to `clear` (RGBA),
 * with `depth` (from de_golden_depth_target, cleared to 0: reverse-Z far) when it is valid. */
void de_golden_begin_render(de_golden_ctx *ctx, const float clear[4], de_gpu_texture depth);
void de_golden_end_render(de_golden_ctx *ctx);

/* ---------- Harness side (main.c) ---------- */

typedef struct de_golden_scene_entry {
    const char *name;
    de_golden_scene_fn fn;
} de_golden_scene_entry;

/** The registered scenes, sorted by name. */
u32 de_golden_scene_count(void);
const de_golden_scene_entry *de_golden_scene_at(u32 i);
const de_golden_scene_entry *de_golden_scene_find(const char *name);
/** False (reason in `msg`) when a name is registered twice or there are too many scenes. */
bool de_golden_registry_ok(char *msg, size_t cap);

typedef enum de_golden_outcome {
    DE_GOLDEN_RAN,     /**< rendered and read back */
    DE_GOLDEN_FAILED,  /**< de_golden_fail, a helper failed, or the GPU work failed */
    DE_GOLDEN_SKIPPED, /**< de_golden_skip, or a feature or shader target this host lacks */
} de_golden_outcome;

/** A read-back image: RGBA8, rows tightly packed, row 0 at the top; RGBA16F targets tone-mapped
 * (ACES fit, then the sRGB curve). */
typedef struct de_golden_image {
    u8 *rgba;
    u32 width, height;
} de_golden_image;

typedef struct de_golden_render_desc {
    de_gpu_device *device;
    const char *scene_name;
    de_golden_scene_fn fn;
    const char *tier;
    de_gpu_format format; /**< DE_FMT_RGBA8_UNORM or DE_FMT_RGBA16_FLOAT */
    u32 width, height;
} de_golden_render_desc;

/**
 * Runs one scene: target, command list, the scene, readback, then waits for the GPU and destroys
 * what the run made. `*out` is filled when the outcome is DE_GOLDEN_RAN, and also for
 * DE_GOLDEN_FAILED when the image could still be read back (so it can be written for a look);
 * free it with de_golden_image_free. `msg` gets the reason of a fail or skip.
 */
de_golden_outcome de_golden_render(const de_golden_render_desc *desc, de_golden_image *out,
                                   char *msg, size_t msg_cap);
void de_golden_image_free(de_golden_image *img);
/** Frees the cooked shaders. Call once, after the last device is destroyed. */
void de_golden_shutdown(void);

DE_EXTERN_C_END

#endif
