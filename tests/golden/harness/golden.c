/* golden.c — scene registry, scene helpers, shader cooking and the render + readback of one scene
 * (DENG-329). See golden.h: the scene API, and at its end what main.c calls.
 *
 * Shaders: tests/golden/shaders/<file> is cooked in-process through the S09 shader importer (the
 * same path as the S10 pipeline tests, tests/unit/gpu/data/pipelines/pipeline_shaders.c), once per
 * file and target, without a cook cache. That needs engine internals (the cooker, the shader
 * format); the public engine has no shader API yet (DENG-409). Builds without the importer
 * (no DE_BUILD_TOOLS, no Slang) skip every scene that needs a shader.
 */
#include "golden.h"

#include <dengine/de_platform.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if DE_GOLDEN_COOK
#include <dengine/de_asset.h>

#include "asset/manifest_internal.h"
#include "cook/importers/shader/shader_importer.h"
#include "cook/importers/shader/slang_compile.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_internal.h"
#endif

#define WAIT_NS        (120ull * 1000000000ull)
#define MAX_SCENES     256u
#define MAX_TRACKED    64u
#define MAX_SHADERS    64u
#define READBACK_ALIGN 256u

/* ---------- Registry ---------- */

static de_golden_scene_entry g_scenes[MAX_SCENES];
static u32 g_scene_count;
static bool g_scene_overflow;

void de_golden_register(const char *name, de_golden_scene_fn fn) {
    if (g_scene_count == MAX_SCENES) {
        g_scene_overflow = true;
        return;
    }
    /* Kept sorted by name (insertion; registration order across files is unspecified). */
    u32 i = g_scene_count++;
    while (i > 0 && strcmp(g_scenes[i - 1].name, name) > 0) {
        g_scenes[i] = g_scenes[i - 1];
        --i;
    }
    g_scenes[i].name = name;
    g_scenes[i].fn = fn;
}

u32 de_golden_scene_count(void) {
    return g_scene_count;
}

const de_golden_scene_entry *de_golden_scene_at(u32 i) {
    return i < g_scene_count ? &g_scenes[i] : NULL;
}

const de_golden_scene_entry *de_golden_scene_find(const char *name) {
    for (u32 i = 0; i < g_scene_count; ++i)
        if (strcmp(g_scenes[i].name, name) == 0)
            return &g_scenes[i];
    return NULL;
}

bool de_golden_registry_ok(char *msg, size_t cap) {
    if (g_scene_overflow) {
        snprintf(msg, cap, "more than %u scenes registered", MAX_SCENES);
        return false;
    }
    for (u32 i = 1; i < g_scene_count; ++i) {
        if (strcmp(g_scenes[i - 1].name, g_scenes[i].name) == 0) {
            snprintf(msg, cap, "scene '%s' is registered twice", g_scenes[i].name);
            return false;
        }
    }
    return true;
}

/* ---------- One run ---------- */

struct de_golden_run {
    de_golden_outcome outcome; /* DE_GOLDEN_RAN until a fail or skip */
    char msg[512];
    bool rendering;
    de_gpu_buffer buffers[MAX_TRACKED];
    de_gpu_texture textures[MAX_TRACKED];
    de_gpu_sampler samplers[MAX_TRACKED];
    de_gpu_pipeline pipelines[MAX_TRACKED];
    u32 buffer_count, texture_count, sampler_count, pipeline_count;
};

static void set_outcome(de_golden_ctx *ctx, de_golden_outcome o, const char *fmt, va_list ap) {
    de_golden_run *r = ctx->run;
    if (r->outcome != DE_GOLDEN_RAN)
        return; /* the first reason wins */
    r->outcome = o;
    vsnprintf(r->msg, sizeof r->msg, fmt, ap);
}

void de_golden_fail(de_golden_ctx *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_outcome(ctx, DE_GOLDEN_FAILED, fmt, ap);
    va_end(ap);
}

void de_golden_skip(de_golden_ctx *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_outcome(ctx, DE_GOLDEN_SKIPPED, fmt, ap);
    va_end(ap);
}

bool de_golden_stopped(const de_golden_ctx *ctx) {
    return ctx->run->outcome != DE_GOLDEN_RAN;
}

/* The error chain of the last failure, for messages. */
static const char *error_chain(char *buf, size_t cap) {
    buf[0] = '\0';
    size_t n = 0;
    for (const de_error *e = de_error_last(); e != NULL && n + 1 < cap; e = e->cause) {
        int k = snprintf(buf + n, cap - n, "%s%s", n ? ": " : "", e->message);
        if (k < 0)
            break;
        n += (size_t)k;
    }
    return buf[0] ? buf : "(no error context)";
}

/* Fails the scene for a failed engine call; DE_ERR_UNSUPPORTED skips it instead. */
static void fail_call(de_golden_ctx *ctx, const char *what) {
    char chain[400];
    const de_error *e = de_error_last();
    if (e != NULL && e->result == DE_ERR_UNSUPPORTED)
        de_golden_skip(ctx, "%s: unsupported: %s", what, error_chain(chain, sizeof chain));
    else
        de_golden_fail(ctx, "%s failed: %s", what, error_chain(chain, sizeof chain));
}

#define TRACK(ctx, kind, h)                                                                        \
    do {                                                                                           \
        de_golden_run *r_ = (ctx)->run;                                                            \
        if (r_->kind##_count < MAX_TRACKED)                                                        \
            r_->kind##s[r_->kind##_count++] = (h);                                                 \
        else                                                                                       \
            de_golden_fail((ctx), "more than %u " #kind "s in one scene", MAX_TRACKED);            \
    } while (0)

/* ---------- Shaders ---------- */

typedef struct cooked_file {
    char file[64];
    u32 target; /* de__shader_target */
    bool ok;
    bool skip; /* !ok because the host cannot cook the target at all */
    char why[300];
#if DE_GOLDEN_COOK
    de_cook_result result;
    de__shader_view view;
#endif
} cooked_file;

static cooked_file g_cooked[MAX_SHADERS];
static u32 g_cooked_count;

#if DE_GOLDEN_COOK

static de__slang_target slang_target(de__shader_target t) {
    return t == DE__SHADER_TARGET_DXIL       ? DE__SLANG_DXIL
           : t == DE__SHADER_TARGET_METALLIB ? DE__SLANG_METALLIB
                                             : DE__SLANG_SPIRV;
}

static const char *target_name(de__shader_target t) {
    return t == DE__SHADER_TARGET_DXIL       ? "dxil"
           : t == DE__SHADER_TARGET_METALLIB ? "metallib"
                                             : "spirv";
}

/* Cooks `file` for `target` into `cf`. Returns false (reason in cf->why) on failure; `*skip`
 * says the host cannot cook the target at all. */
static bool cook_file(cooked_file *cf, de__shader_target target, bool *skip) {
    char msg[200];
    *skip = false;
    if (!de__slang_available(msg, sizeof msg)) {
        snprintf(cf->why, sizeof cf->why, "no Slang compiler for this host: %.200s", msg);
        *skip = true;
        return false;
    }
    if (!de__slang_target_supported(slang_target(target))) {
        snprintf(cf->why, sizeof cf->why,
                 "this host cannot cook %s shaders (DXC or the Metal "
                 "toolchain is missing)",
                 target_name(target));
        *skip = true;
        return false;
    }
    static bool mounted;
    if (!mounted) {
        if (de_fs_mount("goldenshd", DE_GOLDEN_SHADER_DIR, 0, 0) != DE_OK) {
            snprintf(cf->why, sizeof cf->why, "cannot mount %.200s", DE_GOLDEN_SHADER_DIR);
            return false;
        }
        mounted = true;
    }
    de_cooker *c = NULL;
    de_cooker_desc cd = {.verify_determinism = DE_COOK_VERIFY_OFF};
    static const char *const exts[] = {".slang"};
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "shader";
    t.extensions = exts;
    t.extension_count = 1;
    t.importer_version = de_shader_importer_version();
    t.import = de_shader_import;
    char chain[300];
    if (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK) {
        snprintf(cf->why, sizeof cf->why, "cannot create the cooker: %s",
                 error_chain(chain, sizeof chain));
        de_cooker_destroy(c);
        return false;
    }
    const char *targets[1] = {target_name(target)};
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "targets";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = targets;
    kv.value.u.arr.count = 1;
    char id[96];
    snprintf(id, sizeof id, "golden/%s", cf->file);
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "shader";
    a.source = cf->file;
    a.file = "goldenshd:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_input in = {.asset = &a, .asset_root = "goldenshd:/", .source = NULL};
    de_result r = de_cooker_cook(c, &in, &cf->result);
    if (r == DE_OK)
        r = de__shader_view_init(cf->result.data, cf->result.size, &cf->view);
    if (r != DE_OK)
        snprintf(cf->why, sizeof cf->why, "cooking %s for %s failed: %s", cf->file, targets[0],
                 error_chain(chain, sizeof chain));
    de_cooker_destroy(c);
    return r == DE_OK;
}

static bool find_entry(const cooked_file *cf, const char *entry, de_golden_shader *out) {
    const de__shader_view *v = &cf->view;
    for (u32 e = 0; e < v->header.entry_count; ++e) {
        const char *name = NULL;
        (void)de__shader_view_entry(v, e, &name);
        if (name == NULL || strcmp(name, entry) != 0)
            continue;
        i32 ri = de__shader_view_find(v, cf->target, 0, e);
        if (ri < 0)
            return false;
        const de__shader_record rec = de__shader_view_record(v, (u32)ri);
        out->blob.code = v->base + rec.code_offset;
        out->blob.size = rec.code_size;
        out->blob.entry = name;
        out->push_constant_bytes = rec.push_constant_bytes;
        memcpy(out->threadgroup, rec.threadgroup, sizeof out->threadgroup);
        return true;
    }
    return false;
}

bool de_golden_get_shader(de_golden_ctx *ctx, const char *file, const char *entry,
                          de_golden_shader *out) {
    memset(out, 0, sizeof *out);
    if (de_golden_stopped(ctx))
        return false;
    const de__shader_target target = de__shader_target_for_backend(ctx->backend);
    cooked_file *cf = NULL;
    for (u32 i = 0; i < g_cooked_count && cf == NULL; ++i)
        if (g_cooked[i].target == (u32)target && strcmp(g_cooked[i].file, file) == 0)
            cf = &g_cooked[i];
    if (cf == NULL) {
        if (g_cooked_count == MAX_SHADERS || strlen(file) >= sizeof g_cooked[0].file) {
            de_golden_fail(ctx, "too many shader files, or the name '%s' is too long", file);
            return false;
        }
        cf = &g_cooked[g_cooked_count++];
        snprintf(cf->file, sizeof cf->file, "%s", file);
        cf->target = (u32)target;
        cf->ok = cook_file(cf, target, &cf->skip);
    }
    if (!cf->ok) {
        if (cf->skip)
            de_golden_skip(ctx, "%s", cf->why);
        else
            de_golden_fail(ctx, "%s", cf->why);
        return false;
    }
    if (!find_entry(cf, entry, out)) {
        de_golden_fail(ctx, "%s has no entry point '%s' for this backend", file, entry);
        return false;
    }
    return true;
}

static void free_shaders(void) {
    for (u32 i = 0; i < g_cooked_count; ++i)
        de_cook_result_free(&g_cooked[i].result);
    g_cooked_count = 0;
}

#else /* !DE_GOLDEN_COOK */

bool de_golden_get_shader(de_golden_ctx *ctx, const char *file, const char *entry,
                          de_golden_shader *out) {
    (void)entry;
    memset(out, 0, sizeof *out);
    if (!de_golden_stopped(ctx))
        de_golden_skip(ctx,
                       "this build cannot cook %s: no shader importer (DE_BUILD_TOOLS off or "
                       "no Slang)",
                       file);
    return false;
}

static void free_shaders(void) {
    (void)g_cooked;
    (void)g_cooked_count;
}

#endif

de_gpu_pipeline de_golden_pipeline(de_golden_ctx *ctx, const de_gpu_pipeline_desc *desc) {
    if (de_golden_stopped(ctx))
        return (de_gpu_pipeline){0};
    de_error_clear();
    de_gpu_pipeline p = de_gpu_pipeline_create(ctx->device, desc);
    if (!DE_HANDLE_VALID(p)) {
        fail_call(ctx, "de_gpu_pipeline_create");
        return p;
    }
    TRACK(ctx, pipeline, p);
    return p;
}

/* ---------- Resources ---------- */

static de_gpu_barrier buffer_barrier(de_gpu_buffer b, u32 before, u32 after) {
    de_gpu_barrier x;
    memset(&x, 0, sizeof x);
    x.buffer = b;
    x.before = before;
    x.after = after;
    return x;
}

static de_gpu_barrier texture_barrier(de_gpu_texture t, u32 before, u32 after) {
    de_gpu_barrier x;
    memset(&x, 0, sizeof x);
    x.texture = t;
    x.before = before;
    x.after = after;
    return x;
}

de_gpu_buffer de_golden_upload_buffer(de_golden_ctx *ctx, const void *data, u64 size, u32 usage,
                                      const char *name) {
    if (de_golden_stopped(ctx))
        return (de_gpu_buffer){0};
    de_error_clear();
    de_gpu_buffer b = de_gpu_buffer_create(
        ctx->device,
        &(de_gpu_buffer_desc){.size = size, .usage = usage, .memory = DE_MEM_UPLOAD, .name = name});
    void *p = DE_HANDLE_VALID(b) ? de_gpu_buffer_map(ctx->device, b) : NULL;
    if (p == NULL) {
        fail_call(ctx, "creating an UPLOAD buffer");
        if (DE_HANDLE_VALID(b))
            de_gpu_buffer_destroy(ctx->device, b);
        return (de_gpu_buffer){0};
    }
    if (data != NULL)
        memcpy(p, data, (size_t)size);
    TRACK(ctx, buffer, b);
    return b;
}

de_gpu_buffer de_golden_storage_buffer(de_golden_ctx *ctx, const void *data, u64 size, u32 stride,
                                       u32 extra_usage, const char *name) {
    if (de_golden_stopped(ctx))
        return (de_gpu_buffer){0};
    de_gpu_buffer staging = {0};
    if (data != NULL) {
        staging = de_golden_upload_buffer(ctx, data, size, DE_USAGE_COPY_SRC, "golden staging");
        if (!DE_HANDLE_VALID(staging))
            return (de_gpu_buffer){0};
    }
    de_error_clear();
    de_gpu_buffer b = de_gpu_buffer_create(
        ctx->device,
        &(de_gpu_buffer_desc){.size = size,
                              .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST | extra_usage,
                              .stride = stride,
                              .name = name});
    if (!DE_HANDLE_VALID(b)) {
        fail_call(ctx, "creating a storage buffer");
        return b;
    }
    TRACK(ctx, buffer, b);
    if (data != NULL) {
        de_gpu_barrier x = buffer_barrier(b, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
        de_gpu_cmd_barriers(ctx->cmd, &x, 1);
        de_gpu_cmd_copy_buffer(ctx->cmd, b, 0, staging, 0, size);
        x = buffer_barrier(b, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_WRITE);
        de_gpu_cmd_barriers(ctx->cmd, &x, 1);
    } else {
        de_gpu_barrier x = buffer_barrier(b, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
        de_gpu_cmd_barriers(ctx->cmd, &x, 1);
    }
    return b;
}

de_gpu_texture de_golden_texture_rgba8(de_golden_ctx *ctx, u32 width, u32 height, const u8 *rgba,
                                       const char *name) {
    if (de_golden_stopped(ctx))
        return (de_gpu_texture){0};
    const u32 pitch = (width * 4u + READBACK_ALIGN - 1u) & ~(READBACK_ALIGN - 1u);
    de_gpu_buffer up = de_golden_upload_buffer(ctx, NULL, (u64)pitch * height, DE_USAGE_COPY_SRC,
                                               "golden texture upload");
    if (!DE_HANDLE_VALID(up))
        return (de_gpu_texture){0};
    u8 *dst = (u8 *)de_gpu_buffer_map(ctx->device, up);
    for (u32 y = 0; y < height; ++y)
        memcpy(dst + (size_t)y * pitch, rgba + (size_t)y * width * 4u, (size_t)width * 4u);
    de_error_clear();
    de_gpu_texture t = de_gpu_texture_create(
        ctx->device, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                            .width = width,
                                            .height = height,
                                            .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                            .name = name});
    if (!DE_HANDLE_VALID(t)) {
        fail_call(ctx, "creating a texture");
        return t;
    }
    TRACK(ctx, texture, t);
    de_gpu_barrier x = texture_barrier(t, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(ctx->cmd, &x, 1);
    de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, t, 0, 0, up, 0, pitch);
    x = texture_barrier(t, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
    de_gpu_cmd_barriers(ctx->cmd, &x, 1);
    return t;
}

de_gpu_texture de_golden_depth_target(de_golden_ctx *ctx) {
    if (de_golden_stopped(ctx))
        return (de_gpu_texture){0};
    de_error_clear();
    de_gpu_texture t =
        de_gpu_texture_create(ctx->device, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT,
                                                                  .width = ctx->width,
                                                                  .height = ctx->height,
                                                                  .usage = DE_USAGE_DEPTH,
                                                                  .name = "golden depth"});
    if (!DE_HANDLE_VALID(t)) {
        fail_call(ctx, "creating the depth target");
        return t;
    }
    TRACK(ctx, texture, t);
    de_gpu_barrier x = texture_barrier(t, DE_ACCESS_NONE, DE_ACCESS_DEPTH_WRITE);
    de_gpu_cmd_barriers(ctx->cmd, &x, 1);
    return t;
}

de_gpu_sampler de_golden_sampler(de_golden_ctx *ctx, const de_gpu_sampler_desc *desc) {
    if (de_golden_stopped(ctx))
        return (de_gpu_sampler){0};
    de_error_clear();
    de_gpu_sampler s = de_gpu_sampler_create(ctx->device, desc);
    if (!DE_HANDLE_VALID(s)) {
        fail_call(ctx, "creating a sampler");
        return s;
    }
    TRACK(ctx, sampler, s);
    return s;
}

/* ---------- Target ---------- */

void de_golden_target_barrier(de_golden_ctx *ctx, u32 after) {
    de_gpu_barrier x = texture_barrier(ctx->target, ctx->target_access, after);
    de_gpu_cmd_barriers(ctx->cmd, &x, 1);
    ctx->target_access = after;
}

void de_golden_begin_render(de_golden_ctx *ctx, const float clear[4], de_gpu_texture depth) {
    if (ctx->run->rendering) {
        de_golden_fail(ctx, "de_golden_begin_render inside a render pass");
        return;
    }
    if (ctx->target_access != DE_ACCESS_RENDER_TARGET)
        de_golden_target_barrier(ctx, DE_ACCESS_RENDER_TARGET);
    de_gpu_attachment color;
    memset(&color, 0, sizeof color);
    color.texture = ctx->target;
    color.clear = true;
    memcpy(color.clear_color, clear, sizeof color.clear_color);
    color.store = true;
    de_gpu_attachment d;
    memset(&d, 0, sizeof d);
    d.texture = depth;
    d.clear = true;
    d.clear_depth = 0.0f; /* reverse-Z: 0 is far */
    d.store = false;
    de_gpu_cmd_begin_render(ctx->cmd, &color, 1, DE_HANDLE_VALID(depth) ? &d : NULL);
    ctx->run->rendering = true;
}

void de_golden_end_render(de_golden_ctx *ctx) {
    if (!ctx->run->rendering)
        return;
    de_gpu_cmd_end_render(ctx->cmd);
    ctx->run->rendering = false;
}

/* ---------- Render one scene ---------- */

/* IEEE half to float (no FP16 hardware assumptions; exact for every value). */
static float half_to_float(u16 h) {
    const u32 sign = (u32)(h >> 15) & 1u, exp = (u32)(h >> 10) & 0x1Fu, man = (u32)h & 0x3FFu;
    float v;
    if (exp == 0)
        v = ldexpf((float)man, -24); /* subnormal (or zero) */
    else if (exp == 31)
        v = man ? NAN : INFINITY;
    else
        v = ldexpf((float)(man | 0x400u), (int)exp - 25);
    return sign ? -v : v;
}

/* RGBA16F preview: ACES filmic fit (Narkowicz 2015) per channel, then the sRGB curve. */
static u8 tonemap_to_u8(float x) {
    if (!(x > 0.0f))
        return 0; /* also NaN */
    const float t = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
    const float c = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (u8)(s * 255.0f + 0.5f);
}

static u8 alpha_to_u8(float a) {
    const float c = !(a > 0.0f) ? 0.0f : a > 1.0f ? 1.0f : a;
    return (u8)(c * 255.0f + 0.5f);
}

static void destroy_tracked(de_gpu_device *d, de_golden_run *r) {
    for (u32 i = 0; i < r->pipeline_count; ++i)
        de_gpu_pipeline_destroy(d, r->pipelines[i]);
    for (u32 i = 0; i < r->buffer_count; ++i)
        de_gpu_buffer_destroy(d, r->buffers[i]);
    for (u32 i = 0; i < r->texture_count; ++i)
        de_gpu_texture_destroy(d, r->textures[i]);
    for (u32 i = 0; i < r->sampler_count; ++i)
        de_gpu_sampler_destroy(d, r->samplers[i]);
}

de_golden_outcome de_golden_render(const de_golden_render_desc *desc, de_golden_image *out,
                                   char *msg, size_t msg_cap) {
    memset(out, 0, sizeof *out);
    msg[0] = '\0';
    static de_golden_run run; /* one at a time; large, so not on the stack */
    memset(&run, 0, sizeof run);
    de_golden_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.device = desc->device;
    ctx.caps = de_gpu_get_caps(desc->device);
    ctx.backend = ctx.caps->backend;
    ctx.tier = desc->tier;
    ctx.format = desc->format;
    ctx.width = desc->width;
    ctx.height = desc->height;
    ctx.target_access = DE_ACCESS_NONE;
    ctx.run = &run;
    de_gpu_device *d = desc->device;
    const u32 bpp = desc->format == DE_FMT_RGBA16_FLOAT ? 8u : 4u;
    const u32 pitch = (desc->width * bpp + READBACK_ALIGN - 1u) & ~(READBACK_ALIGN - 1u);

    de_error_clear();
    ctx.target = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = desc->format,
                                  .width = desc->width,
                                  .height = desc->height,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC |
                                           DE_USAGE_COPY_DST | DE_USAGE_SAMPLED,
                                  .name = "golden target"});
    de_gpu_buffer rb =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = (u64)pitch * desc->height,
                                                      .usage = DE_USAGE_COPY_DST,
                                                      .memory = DE_MEM_READBACK,
                                                      .name = "golden readback"});
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ctx.cmd = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, desc->scene_name);
    if (!DE_HANDLE_VALID(ctx.target) || !DE_HANDLE_VALID(rb) || !DE_HANDLE_VALID(fence) ||
        ctx.cmd == NULL) {
        char chain[400];
        snprintf(msg, msg_cap,
                 "creating the target, readback buffer, fence or command list "
                 "failed: %s",
                 error_chain(chain, sizeof chain));
        de_gpu_texture_destroy(d, ctx.target);
        de_gpu_buffer_destroy(d, rb);
        de_gpu_fence_destroy(d, fence);
        return DE_GOLDEN_FAILED; /* a begun list is recycled by the device at destroy */
    }

    desc->fn(&ctx);

    /* Whatever happened, the list is closed, the target copied and the list submitted once. */
    de_golden_end_render(&ctx);
    de_golden_target_barrier(&ctx, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_copy_texture_to_buffer(ctx.cmd, rb, 0, pitch, ctx.target, 0, 0);
    de_gpu_submit s = {.cmds = &ctx.cmd, .cmd_count = 1, .signal = fence, .signal_value = 1};
    de_golden_outcome outcome = run.outcome;
    snprintf(msg, msg_cap, "%s", run.msg);
    if (de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s) != DE_OK ||
        de_gpu_fence_wait(d, fence, 1, WAIT_NS) != DE_OK) {
        char chain[400];
        if (outcome == DE_GOLDEN_RAN)
            snprintf(msg, msg_cap, "submitting or waiting for the scene failed: %s",
                     error_chain(chain, sizeof chain));
        outcome = outcome == DE_GOLDEN_SKIPPED ? outcome : DE_GOLDEN_FAILED;
    } else if (outcome != DE_GOLDEN_SKIPPED) {
        const u8 *src = (const u8 *)de_gpu_buffer_map(d, rb);
        out->rgba = (u8 *)malloc((size_t)desc->width * desc->height * 4u);
        if (src == NULL || out->rgba == NULL) {
            snprintf(msg, msg_cap, "mapping the readback buffer failed");
            outcome = DE_GOLDEN_FAILED;
        } else {
            out->width = desc->width;
            out->height = desc->height;
            for (u32 y = 0; y < desc->height; ++y) {
                const u8 *row = src + (size_t)y * pitch;
                u8 *dst = out->rgba + (size_t)y * desc->width * 4u;
                if (bpp == 4u) {
                    memcpy(dst, row, (size_t)desc->width * 4u);
                    continue;
                }
                for (u32 x = 0; x < desc->width; ++x) {
                    u16 h[4];
                    memcpy(h, row + (size_t)x * 8u, sizeof h);
                    for (int c = 0; c < 3; ++c)
                        dst[x * 4u + (u32)c] = tonemap_to_u8(half_to_float(h[c]));
                    dst[x * 4u + 3u] = alpha_to_u8(half_to_float(h[3]));
                }
            }
        }
    }
    destroy_tracked(d, &run);
    de_gpu_texture_destroy(d, ctx.target);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_fence_destroy(d, fence);
    de_gpu_wait_idle(d);
    return outcome;
}

void de_golden_image_free(de_golden_image *img) {
    free(img->rgba);
    memset(img, 0, sizeof *img);
}

void de_golden_shutdown(void) {
    free_shaders();
}
