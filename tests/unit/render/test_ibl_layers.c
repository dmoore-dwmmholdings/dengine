/* DENG-431: independent IBL layer acceptance. Cases and limits are fixed before the
 * production implementation: per-channel furnace <= 1.02; numeric composition 3e-4
 * absolute + 0.3% relative; inactive layers bitwise identical; independent Charlie
 * tensor quadrature at mu=.015625/r=.07 must remain above 1.4, while the IBL coefficient
 * is bounded consistently for added energy and attenuation. No final RGB clamping.
 * Actual shared evaluate_indirect_split runs with real bound sky/probe/GI/SSR/AO resources.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "render/materials/brdf_luts.h"
#include "render/passes/vis/gbuffer.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "render_d3d12_test.h"

#if defined(DE_ILL_TEST_COOK) && DE_ILL_TEST_COOK
#include "asset/manifest_internal.h"
#include "cook/importers/shader/shader_importer.h"
#include "cook/importers/shader/slang_compile.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_internal.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- GPU fixture ---------- */

enum { K_EVALUATE, K_COUNT };
static const char *const k_entries[K_COUNT] = {"cs_ibl_layers"};

/** Seven float4 inputs of ibl_layers_kernels.slang, uploaded with float4 descriptor stride. */
typedef struct il_case {
    float p0[4]; /* roughness, metallic, reserved, mu_o */
    float p1[4]; /* clearcoat, clearcoat roughness, sheen roughness, reserved */
    float p2[4]; /* sheen colour, specular factor */
    float p3[4]; /* iridescence, iridescence ior, thickness (nm), features */
    float p4[4]; /* coat normal xyz, resource mode bits */
    float p5[4]; /* RGB base colour, material AO */
    float p6[4]; /* decoded quantized G-buffer normal */
} il_case;

#define SK_OUT  32u
#define WAIT_NS ((u64)120 * 1000000000u)

typedef struct il_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de_gpu_backend backend;
    de_gpu_pipeline pipe[K_COUNT];
    de__brdf_luts *luts;
    u32 lut;
    u32 config;
    u32 config_data[16];
    de_gpu_texture textures[16];
    u32 texture_count;
    de_gpu_sampler sampler;
    char why[700];
    /* Buffers of il_run, destroyed at teardown: a buffer freed between runs can come back at the
     * same memory, and Vulkan's synchronization validation then sees two unordered copies into
     * it (the fence waits order them, but not in its model). */
    de_gpu_buffer keep[64];
    u32 keep_count;
} il_fx;

static void il_fx_setup(il_fx *f) {
    memset(f, 0, sizeof *f);
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    de_error_clear();
}

static u32 validation_errors(de_gpu_device *d) {
    switch (de_gpu_get_caps(d)->backend) {
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN: {
        u32 warnings = 0;
        return de__gpu_vk_validation_messages(d, &warnings);
    }
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return de__d3d12_debug_error_count(d);
#endif
    default:
        return 0;
    }
}

static void il_fx_teardown(il_fx *f) {
    if (f->dev != NULL) {
        de_gpu_wait_idle(f->dev);
        for (u32 k = 0; k < K_COUNT; ++k)
            if (DE_HANDLE_VALID(f->pipe[k]))
                de_gpu_pipeline_destroy(f->dev, f->pipe[k]);
        for (u32 k = 0; k < f->keep_count; ++k)
            de_gpu_buffer_destroy(f->dev, f->keep[k]);
        for (u32 k = 0; k < f->texture_count; ++k)
            de_gpu_texture_destroy(f->dev, f->textures[k]);
        if (DE_HANDLE_VALID(f->sampler))
            de_gpu_sampler_destroy(f->dev, f->sampler);
        de__brdf_luts_destroy(f->luts);
        de_gpu_wait_idle(f->dev);
        EXPECT_EQ(validation_errors(f->dev), 0u);
        de_gpu_device_destroy(f->dev);
    }
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
}

static bool backend_built(de_gpu_backend b) {
    switch (b) {
#if DE_HAS_GPU_METAL
    case DE_GPU_METAL:
        return true;
#endif
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN:
        return true;
#endif
#if DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return true;
#endif
    default:
        return false;
    }
}

static de_gpu_barrier buf_barrier(de_gpu_buffer buf, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buf;
    b.before = before;
    b.after = after;
    return b;
}

/* Records `record` into one list on the compute queue, submits and waits. */
typedef void (*il_record_fn)(de_gpu_cmd *c, void *user);
static void il_submit(il_fx *f, il_record_fn record, void *user) {
    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "layers test");
    ASSERT_TRUE(c != NULL);
    record(c, user);
    if (de_error_last() != NULL)
        printf("recording: %s\n", de_error_last()->message);
    EXPECT_TRUE(de_error_last() == NULL);
    const de_gpu_fence fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS, &s), DE_OK);
    /* The first dispatch includes the driver's compile of the kernel (DENG-428): a long bound. */
    ASSERT_EQ(de_gpu_fence_wait(f->dev, fence, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(f->dev, fence);
}

#if defined(DE_ILL_TEST_COOK) && DE_ILL_TEST_COOK

/* "illsrc:/": data/ over the engine's shaders/. */
static bool mount_sources(void) {
    static int mounted = -1;
    if (mounted < 0)
        mounted = de_fs_mount("illsrc", DE_ILL_TEST_SHADER_DIR, 0, 0) == DE_OK &&
                  de_fs_mount("illsrc", DE_ILL_TEST_DATA_DIR, 0, 1) == DE_OK;
    return mounted == 1;
}

static const char *target_name(de__shader_target t) {
    switch (t) {
    case DE__SHADER_TARGET_SPIRV:
        return "spirv";
    case DE__SHADER_TARGET_DXIL:
        return "dxil";
    case DE__SHADER_TARGET_METALLIB:
        return "metallib";
    default:
        return "none";
    }
}

static bool find_entry(const de__shader_view *v, u32 target, const char *name,
                       de_gpu_shader_blob *out, de__shader_record *rec_out) {
    for (u32 e = 0; e < v->header.entry_count; ++e) {
        const char *n = NULL;
        (void)de__shader_view_entry(v, e, &n);
        if (strcmp(n, name) != 0)
            continue;
        const i32 ri = de__shader_view_find(v, target, 0, e);
        if (ri < 0)
            return false;
        const de__shader_record rec = de__shader_view_record(v, (u32)ri);
        out->code = v->base + rec.code_offset;
        out->size = rec.code_size;
        out->entry = n;
        *rec_out = rec;
        return true;
    }
    return false;
}

/* standard_kernels.slang's entry points for the fixture's backend; false with f->why. */
static bool il_pipelines(il_fx *f) {
    const de__shader_target target = de__shader_target_for_backend(f->backend);
    char msg[200];
    if (!de__slang_available(msg, sizeof msg)) {
        snprintf(f->why, sizeof f->why, "no Slang compiler: %.150s", msg);
        return false;
    }
    if (!mount_sources()) {
        snprintf(f->why, sizeof f->why, "cannot mount the shader sources");
        return false;
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
    const char *targets[1] = {target_name(target)};
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "targets";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = targets;
    kv.value.u.arr.count = 1;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "ibl_layers_kernels";
    a.type = "shader";
    a.source = "ibl_layers_kernels.slang";
    a.file = "illsrc:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    de_result r = DE_OK;
    if (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK)
        r = DE_ERR_INTERNAL;
    if (r == DE_OK) {
        de_cook_input in = {.asset = &a, .asset_root = "illsrc:/", .source = NULL};
        r = de_cooker_cook(c, &in, &res);
    }
    de__shader_view view;
    bool ok = r == DE_OK && de__shader_view_init(res.data, res.size, &view) == DE_OK;
    for (u32 k = 0; ok && k < K_COUNT; ++k) {
        de_gpu_shader_blob cs = {0};
        de__shader_record rec;
        ok = find_entry(&view, (u32)target, k_entries[k], &cs, &rec);
        if (ok)
            f->pipe[k] = de_gpu_pipeline_create(
                f->dev,
                &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                        .cs = cs,
                                        .push_constant_bytes = 32,
                                        .threadgroup_size = {rec.threadgroup[0], rec.threadgroup[1],
                                                             rec.threadgroup[2]},
                                        .name = k_entries[k]});
        ok = ok && DE_HANDLE_VALID(f->pipe[k]);
    }
    if (!ok)
        snprintf(f->why, sizeof f->why, "ibl_layers_kernels.slang did not cook or link: %.600s",
                 de_error_last() ? de_error_last()->message : "?");
    de_cook_result_free(&res);
    de_cooker_destroy(c);
    return ok;
}

#endif

/* Opens backend `b`: the device, the kernels and the energy LUT (generated and waited for). */
static void il_open(il_fx *f, de_gpu_backend b, bool kernels) {
    if (!backend_built(b)) {
        snprintf(f->why, sizeof f->why, "the %s backend is not in this build",
                 de_gpu_backend_name(b));
        SKIP(f->why);
    }
    f->backend = b;
#if defined(__APPLE__)
    if (b == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc desc = {.backend = b,
                               .debug_layer = true,
                               .gpu_validation = b == DE_GPU_VULKAN,
                               .pipeline_cache_dir = ""};
    de_result r = de_gpu_device_create(&desc, &f->dev);
    if (r == DE_ERR_UNSUPPORTED && b == DE_GPU_VULKAN) {
        desc.adapter = 1;
        r = de_gpu_device_create(&desc, &f->dev);
    }
    if (r != DE_OK) {
        f->dev = NULL;
        snprintf(f->why, sizeof f->why, "no usable %s device: %.400s", de_gpu_backend_name(b),
                 de_error_last() ? de_error_last()->message : "?");
        SKIP(f->why);
    }
    const de_gpu_caps *caps = de_gpu_get_caps(f->dev);
    printf("device: \"%s\" (%s)\n", caps->device_name, de_gpu_backend_name(b));
    render_d3d12_report(f->dev);
    if (!caps->base_tier_ok) {
        snprintf(f->why, sizeof f->why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(f->why);
    }
#if defined(DE_ILL_TEST_COOK) && DE_ILL_TEST_COOK
    if (kernels && !il_pipelines(f))
        SKIP(f->why);
#else
    if (kernels)
        SKIP("this build has no shader importer (DE_BUILD_TOOLS off or no Slang)");
#endif
    ASSERT_EQ(de__brdf_luts_create(f->dev, &f->alloc, &f->luts), DE_OK);
    f->lut = de__brdf_luts_bindless(f->luts);
    if (f->lut == DE_GPU_BINDLESS_INVALID)
        SKIP("no brdf_lut_gen code for this backend in this build");
    u64 v = 0;
    const de_gpu_fence fence = de__brdf_luts_fence(f->luts, &v);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, fence, v, WAIT_NS), DE_OK);
}

/* ---------- Running a kernel ---------- */

typedef struct run_ctx {
    il_fx *f;
    de_gpu_pipeline pipe;
    de_gpu_buffer in, out, rb;
    u64 out_bytes;
    u32 params[8];
    u32 threads;
} run_ctx;

static void run_record(de_gpu_cmd *c, void *user) {
    run_ctx *x = (run_ctx *)user;
    de_gpu_barrier bars[2] = {buf_barrier(x->in, DE_ACCESS_NONE, DE_ACCESS_SHADER_READ),
                              buf_barrier(x->out, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE)};
    de_gpu_cmd_barriers(c, bars, 2);
    de_gpu_cmd_set_pipeline(c, x->pipe);
    /* At most 65535 groups per dispatch (Vulkan's guaranteed maxComputeWorkGroupCount). */
    const u32 chunk = 65535u * 64u;
    for (u32 first = 0; first < x->threads; first += chunk) {
        x->params[6] = first;
        const u32 n = x->threads - first < chunk ? x->threads - first : chunk;
        de_gpu_cmd_push_constants(c, x->params, sizeof x->params);
        de_gpu_cmd_dispatch(c, (n + 63u) / 64u, 1, 1);
    }
    bars[0] = buf_barrier(x->out, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, bars, 1);
    de_gpu_cmd_copy_buffer(c, x->rb, 0, x->out, 0, x->out_bytes);
}

/* Runs kernel `k` over `count` cases with `per_case` threads each (and `grid_x` for cs_quad);
 * returns `out_floats` floats (malloc; the caller frees). */
static float *il_run(il_fx *f, u32 k, const il_case *cases, u32 count, u32 per_case, u32 grid_x,
                     u32 out_floats) {
    run_ctx x;
    memset(&x, 0, sizeof x);
    x.f = f;
    x.pipe = f->pipe[k];
    x.threads = count * per_case;
    x.out_bytes = (u64)out_floats * sizeof(float);
    const u64 in_bytes = (u64)count * sizeof(il_case);
    x.in = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = in_bytes,
                                                              .usage = DE_USAGE_STORAGE,
                                                              .memory = DE_MEM_UPLOAD,
                                                              .stride = 16,
                                                              .name = "layers cases"});
    x.out = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = x.out_bytes,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .stride = 16,
                                      .name = "layers out"});
    x.rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = x.out_bytes,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK,
                                                              .name = "layers readback"});
    float *res = (float *)calloc(out_floats, sizeof(float));
    if (!DE_HANDLE_VALID(x.in) || !DE_HANDLE_VALID(x.out) || !DE_HANDLE_VALID(x.rb) ||
        res == NULL) {
        free(res);
        return NULL;
    }
    memcpy(de_gpu_buffer_map(f->dev, x.in), cases, (size_t)in_bytes);
    x.params[0] = de_gpu_buffer_bindless(f->dev, x.in);
    x.params[1] = de_gpu_buffer_bindless(f->dev, x.out);
    x.params[2] = count;
    x.params[3] = f->lut;
    x.params[4] = f->config;
    x.params[5] = grid_x;
    il_submit(f, run_record, &x);
    memcpy(res, de_gpu_buffer_map(f->dev, x.rb), (size_t)x.out_bytes);
    if (f->keep_count + 3u <= sizeof f->keep / sizeof f->keep[0]) {
        f->keep[f->keep_count++] = x.in;
        f->keep[f->keep_count++] = x.out;
        f->keep[f->keep_count++] = x.rb;
    } else {
        de_gpu_buffer_destroy(f->dev, x.in);
        de_gpu_buffer_destroy(f->dev, x.out);
        de_gpu_buffer_destroy(f->dev, x.rb);
    }
    return res;
}

/* Inputs are exact powers of two where possible; every face and mip has a distinct code. */
static const double il_faces[6][3] = {{2, .25, .5}, {.25, 2, .5}, {.5, .25, 2},
                                      {1, 2, .25},  {.5, 1, 2},   {2, .5, 1}};
static const double il_mips[3] = {1, .5, .25};
static const double il_gi[3] = {.25, .5, .75};
static const double IL_PI = 3.14159265358979323846;
static u16 il_half(float v) {
    u32 x;
    memcpy(&x, &v, 4);
    u32 sign = (x >> 16) & 0x8000u, m = (x & 0x7fffffu) + 0x1000u;
    i32 e = (i32)((x >> 23) & 255u) - 112;
    if (e <= 0)
        return (u16)sign;
    if (m & 0x800000u) {
        ++e;
        m = 0;
    }
    return (u16)(sign | ((u32)e << 10) | (m >> 13));
}
static double il_unhalf(u16 h) {
    return ldexp(1.0 + (double)(h & 1023u) / 1024.0, (int)((h >> 10) & 31u) - 15);
}
static de_gpu_buffer il_upload(il_fx *f, const void *p, u64 size, u32 stride) {
    ASSERT_TRUE(f->keep_count < 64);
    de_gpu_buffer b = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = size,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .memory = DE_MEM_UPLOAD,
                                      .stride = stride,
                                      .name = "IBL acceptance input"});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    memcpy(de_gpu_buffer_map(f->dev, b), p, (size_t)size);
    f->keep[f->keep_count++] = b;
    return b;
}
typedef struct il_tex_upload {
    de_gpu_texture t;
    de_gpu_buffer src;
    u32 mips, pitch[3];
    u64 offset[3];
    bool storage;
} il_tex_upload;
static void il_tex_record(de_gpu_cmd *cmd, void *user) {
    il_tex_upload *u = user;
    de_gpu_barrier bars[2] = {
        {.texture = u->t, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST},
        {.buffer = u->src, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_SRC}};
    de_gpu_cmd_barriers(cmd, bars, 2);
    for (u32 m = 0; m < u->mips; ++m)
        de_gpu_cmd_copy_buffer_to_texture(cmd, u->t, m, 0, u->src, u->offset[m], u->pitch[m]);
    bars[0] = (de_gpu_barrier){.texture = u->t,
                               .before = DE_ACCESS_COPY_DST,
                               .after = DE_ACCESS_SHADER_READ |
                                        (u->storage ? DE_ACCESS_SHADER_WRITE : 0)};
    de_gpu_cmd_barriers(cmd, bars, 1);
}
static de_gpu_texture il_texture(il_fx *f, u32 w, u32 h, u32 mips, u32 kind) {
    bool storage = kind == 1 || kind == 2;
    de_gpu_texture t = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                       .width = w,
                                       .height = h,
                                       .mip_count = mips,
                                       .usage = DE_USAGE_COPY_DST | DE_USAGE_SAMPLED |
                                                (storage ? DE_USAGE_STORAGE : 0),
                                       .name = "IBL acceptance texture"});
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    f->textures[f->texture_count++] = t;
    il_tex_upload u = {.t = t, .mips = mips, .storage = storage};
    u64 bytes = 0;
    for (u32 m = 0; m < mips; ++m) {
        u.pitch[m] = ((w >> m) * 8u + 255u) & ~255u;
        u.offset[m] = bytes;
        bytes = (bytes + (u64)u.pitch[m] * (h >> m) + 511u) & ~(u64)511u;
    }
    u8 *p = calloc((size_t)bytes, 1);
    ASSERT_TRUE(p != NULL);
    for (u32 m = 0; m < mips; ++m)
        for (u32 y = 0; y < (h >> m); ++y)
            for (u32 x = 0; x < (w >> m); ++x) {
                u16 *q = (u16 *)(p + u.offset[m] + (u64)y * u.pitch[m] + x * 8u);
                for (u32 c = 0; c < 4; ++c) {
                    double v = 1;
                    if (kind == 0)
                        v = c < 3 ? il_faces[y / (4u >> m)][c] * il_mips[m] : 1;
                    else if (kind == 1)
                        v = c < 3 ? IL_PI * il_gi[c] : 1;
                    else if (kind == 2)
                        v = c < 2 ? 1 : 0;
                    else if (kind == 3)
                        v = .5;
                    else if (kind == 4)
                        v = c < 3 ? .25 * (c + 1) : .5;
                    else if (kind == 5)
                        v = c < 3 ? .125 * (c + 1) : .5;
                    else if (kind == 6)
                        v = c < 3 ? .25 * (c + 1) : 1;
                    else if (kind == 7)
                        v = c < 3 ? .125 * (c + 1) : 1;
                    q[c] = v == 0 ? 0 : il_half((float)v);
                }
            }
    u.src = il_upload(f, p, bytes, 0);
    free(p);
    il_submit(f, il_tex_record, &u);
    return t;
}
static void il_buffer_reads(de_gpu_cmd *cmd, void *user) {
    il_fx *f = user;
    for (u32 i = 0; i < f->keep_count; ++i) {
        de_gpu_barrier b = buf_barrier(f->keep[i], DE_ACCESS_NONE, DE_ACCESS_SHADER_READ);
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
}
static void il_resources(il_fx *f) {
    de_gpu_texture env = il_texture(f, 4, 24, 3, 0), irr = il_texture(f, 80, 10, 1, 1);
    de_gpu_texture dist = il_texture(f, 144, 18, 1, 2), ao = il_texture(f, 1, 1, 1, 3);
    de_gpu_texture ssr = il_texture(f, 1, 1, 1, 4), gi_override = il_texture(f, 1, 1, 1, 5);
    de_gpu_texture full_ssr = il_texture(f, 1, 1, 1, 6);
    de_gpu_texture full_ssgi = il_texture(f, 1, 1, 1, 7);
    f->sampler = de_gpu_sampler_create(f->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                      .mag = DE_FILTER_LINEAR,
                                                                      .mip = DE_FILTER_LINEAR,
                                                                      .u = DE_ADDR_CLAMP,
                                                                      .v = DE_ADDR_CLAMP,
                                                                      .w = DE_ADDR_CLAMP});
    ASSERT_TRUE(DE_HANDLE_VALID(f->sampler));
    float states[32] = {0};
    de_gpu_buffer state = il_upload(f, states, sizeof states, 16);
    union {
        float f[72];
        u32 u[72];
    } gi = {0};
    gi.u[24] = 1;
    gi.f[36] = gi.f[41] = gi.f[46] = 1;
    gi.f[48] = gi.f[49] = gi.f[50] = 1;
    gi.f[52] = gi.f[53] = gi.f[54] = 1;
    gi.f[59] = 1;
    gi.u[60] = gi.u[61] = gi.u[62] = 2;
    gi.u[63] = 8;
    gi.u[64] = de_gpu_texture_bindless(f->dev, irr, 0, true);
    gi.u[65] = de_gpu_texture_bindless(f->dev, dist, 0, true);
    gi.u[66] = de_gpu_buffer_bindless(f->dev, state);
    gi.f[68] = 1;
    gi.f[70] = 100;
    gi.f[71] = 1;
    de_gpu_buffer volumes = il_upload(f, &gi, sizeof gi, 144);
    union {
        float f[20];
        u32 u[20];
    } probe = {0};
    probe.f[4] = probe.f[5] = probe.f[6] = 100;
    probe.f[7] = 2;
    probe.f[11] = 1;
    probe.f[13] = 4;
    probe.u[18] = de_gpu_texture_bindless(f->dev, env, 0, false);
    probe.u[19] = 3;
    de_gpu_buffer records = il_upload(f, &probe, sizeof probe, 80);
    u32 list[65] = {1};
    de_gpu_buffer probes = il_upload(f, list, sizeof list, 0);
    u32 config[16] = {de_gpu_texture_bindless(f->dev, env, 0, false),
                      de_gpu_sampler_bindless(f->dev, f->sampler),
                      de_gpu_buffer_bindless(f->dev, records),
                      de_gpu_buffer_bindless(f->dev, probes),
                      de_gpu_buffer_bindless(f->dev, volumes),
                      de_gpu_texture_bindless(f->dev, ao, 0, false),
                      de_gpu_texture_bindless(f->dev, ssr, 0, false),
                      de_gpu_texture_bindless(f->dev, gi_override, 0, false)};
    config[12] = de_gpu_texture_bindless(f->dev, full_ssgi, 0, false);
    config[8] = de_gpu_texture_bindless(f->dev, full_ssr, 0, false);
    memcpy(f->config_data, config, sizeof config);
    f->config = de_gpu_buffer_bindless(f->dev, il_upload(f, config, sizeof config, 16));
    il_submit(f, il_buffer_reads, f);
}

/* Frozen case builders; flags are the established Standard feature bits. */
static il_case il_base(float r, float metal, float mu) {
    il_case c = {0};
    c.p0[0] = r;
    c.p0[1] = metal;
    c.p0[3] = mu;
    c.p1[2] = .5f;
    c.p2[3] = 1;
    c.p3[1] = 1.3f;
    c.p4[2] = 1;
    c.p5[0] = c.p5[1] = c.p5[2] = c.p5[3] = 1;
    return c;
}
static void il_coat(il_case *c, float r) {
    c->p1[0] = 1;
    c->p1[1] = r;
    c->p3[3] = (float)((u32)c->p3[3] | 1u);
}
static void il_sheen(il_case *c, float r, bool colored) {
    c->p1[2] = r;
    c->p2[0] = colored ? .2f : 1;
    c->p2[1] = colored ? .7f : 1;
    c->p2[2] = colored ? .9f : 1;
    c->p3[3] = (float)((u32)c->p3[3] | 2u);
}
static void il_film(il_case *c, float factor, float thickness) {
    c->p3[0] = factor;
    c->p3[2] = thickness;
    c->p3[3] = (float)((u32)c->p3[3] | 8u);
}

/* Independent tensor quadrature, unlike the LUT generator's Hammersley estimator. */
static double il_charlie_fit(double x, double a) {
    double t = (1 - a) * (1 - a);
    return (21.5473 + 3.7772 * t) / (1 + (3.82987 - .50552 * t) * pow(x, .19823 - .03022 * t)) +
           (-1.97760 + .70367 * t) * x - 4.32054 - .53913 * t;
}
static double il_charlie_lambda(double mu, double a) {
    return mu < .5 ? exp(il_charlie_fit(mu, a))
                   : exp(2 * il_charlie_fit(.5, a) - il_charlie_fit(1 - mu, a));
}
static double il_charlie_integral(double mu, double r, u32 nz, u32 np) {
    double e = 0, a = r * r, ox = sqrt(1 - mu * mu), lo = il_charlie_lambda(mu, a);
    for (u32 z = 0; z < nz; ++z) {
        double iz = (z + .5) / nz, st = sqrt(1 - iz * iz);
        double vis = fmin(1, 1 / (4 * mu * iz * (1 + lo + il_charlie_lambda(iz, a))));
        for (u32 p = 0; p < np; ++p) {
            double phi = 2 * IL_PI * (p + .5) / np, hx = ox + st * cos(phi), hy = st * sin(phi),
                   hz = mu + iz;
            double h2 = hz * hz / (hx * hx + hy * hy + hz * hz);
            e += (2 + 1 / a) * pow(fmax(1 - h2, 0), .5 / a) / (2 * IL_PI) * vis * iz;
        }
    }
    return e * 2 * IL_PI / (nz * np);
}
TEST(ibl_layers, raw_charlie_extreme_reference) {
    double coarse = il_charlie_integral(.015625, .07, 256, 512);
    double fine = il_charlie_integral(.015625, .07, 512, 1024);
    printf("raw Charlie E: coarse %.9f refined %.9f; IBL directional energy normalizes to 1\n",
           coarse, fine);
    EXPECT_TRUE(fine > 1.4 && fine < 1.46);
    EXPECT_TRUE(fabs(fine - coarse) < .001);
}

/* CPU table reference, independently sampled/interpolated and composed in double precision. */
static float il_lut[32 * 64 * 4];
static bool il_lut_ready;
static double il_clamp(double x, double a, double b) {
    return fmin(fmax(x, a), b);
}
static void il_lookup(double mu, double r, bool sheen, double out[4]) {
    if (!il_lut_ready) {
        de__brdf_lut_reference(4096, il_lut);
        il_lut_ready = true;
    }
    double x = il_clamp(mu * 32 - .5, 0, 31);
    double y = il_clamp((sheen ? (il_clamp(r, .07, 1) - .07) / .93 : r) * 31, 0, 31);
    u32 ix = (u32)x, iy = (u32)y, jx = ix < 31 ? ix + 1 : ix, jy = iy < 31 ? iy + 1 : iy;
    double fx = x - ix, fy = y - iy;
    u32 row = sheen ? 32 : 0;
    for (u32 c = 0; c < 4; ++c) {
        double a = il_lut[((row + iy) * 32 + ix) * 4 + c],
               b = il_lut[((row + iy) * 32 + jx) * 4 + c];
        double d = il_lut[((row + jy) * 32 + ix) * 4 + c],
               e = il_lut[((row + jy) * 32 + jx) * 4 + c];
        out[c] = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
    }
}
static double il_energy(const double t[4], double f0, double f90) {
    double avg = f0 + (f90 - f0) / 21, ea = t[2] + t[3];
    return f0 * t[0] + f90 * t[1] + avg * avg * ea / (1 - avg * (1 - ea)) * (1 - t[0] - t[1]);
}
/* Double precision Fourier/Airy reference at mu_o; the separate normal-incidence spectral
 * acceptance of test_standard_layers remains unchanged. No shader helper is used as oracle. */
static void il_film_rgb(const double f0[3], double mu, double n2, double thickness, double out[3]) {
    double cos2 = sqrt(1 - (1 - mu * mu) / (n2 * n2));
    double first = (n2 - 1) / (n2 + 1);
    first *= first;
    double r12 = first + (1 - first) * pow(1 - mu, 5), trans = 1 - r12;
    double rs[3], product[3], cm[3], phase[3];
    for (u32 c = 0; c < 3; ++c) {
        double root = sqrt(il_clamp(f0[c], 0, .9999)), n3 = (1 + root) / (1 - root);
        double last = (n3 - n2) / (n3 + n2);
        last *= last;
        double r23 = last + (1 - last) * pow(1 - cos2, 5);
        product[c] = il_clamp(r12 * r23, .00001, .9999);
        rs[c] = trans * trans * r23 / (1 - product[c]);
        cm[c] = rs[c] - trans;
        phase[c] = IL_PI + (n3 < n2 ? IL_PI : 0);
        out[c] = r12 + rs[c];
    }
    static const double val[3] = {5.4856e-13, 4.4201e-13, 5.2481e-13};
    static const double pos[3] = {1.6810e6, 1.7953e6, 2.2084e6};
    static const double var[3] = {4.3278e9, 9.3046e9, 6.6121e9};
    for (u32 m = 1; m <= 2; ++m) {
        double p = 2 * IL_PI * (2 * n2 * thickness * cos2 * m) * 1e-9, xyz[3];
        for (u32 c = 0; c < 3; ++c)
            xyz[c] = val[c] * sqrt(2 * IL_PI * var[c]) * cos(pos[c] * p + m * phase[c]) *
                     exp(-p * p * var[c]);
        xyz[0] += 9.7470e-14 * sqrt(2 * IL_PI * 4.5282e9) * cos(2.2399e6 * p + m * phase[0]) *
                  exp(-p * p * 4.5282e9);
        for (u32 c = 0; c < 3; ++c)
            xyz[c] /= 1.0685e-7;
        double rgb[3] = {3.2404542 * xyz[0] - 1.5371385 * xyz[1] - .4985314 * xyz[2],
                         -.9692660 * xyz[0] + 1.8760108 * xyz[1] + .0415560 * xyz[2],
                         .0556434 * xyz[0] - .2040259 * xyz[1] + 1.0572252 * xyz[2]};
        for (u32 c = 0; c < 3; ++c) {
            cm[c] *= sqrt(product[c]);
            out[c] += 2 * cm[c] * rgb[c];
        }
    }
    for (u32 c = 0; c < 3; ++c)
        out[c] = il_clamp(out[c], 0, 1);
}
static double il_occlusion(double mu, double ao, double rough) {
    return il_clamp(pow(fmax(mu + ao, 0), exp2(-16 * rough - 1)) - 1 + ao, 0, 1);
}
static void il_direction(const double n[3], double mu, double rough, double dir[3]) {
    double v[3] = {sqrt(1 - mu * mu), 0, mu}, nv = n[0] * v[0] + n[2] * v[2];
    double a = rough * rough, t = (1 - a) * (sqrt(1 - a) + a), length = 0;
    for (u32 c = 0; c < 3; ++c) {
        double refl = 2 * nv * n[c] - v[c];
        dir[c] = n[c] * (1 - t) + refl * t;
        length += dir[c] * dir[c];
    }
    length = sqrt(length);
    for (u32 c = 0; c < 3; ++c)
        dir[c] /= length;
}
static void il_radiance(const double n[3], double mu, double rough, u32 mode, double out[3]) {
    double dir[3];
    il_direction(n, mu, rough, dir);
    u32 axis = fabs(dir[0]) >= fabs(dir[1]) && fabs(dir[0]) >= fabs(dir[2]) ? 0
               : fabs(dir[1]) >= fabs(dir[2])                               ? 1
                                                                            : 2;
    u32 face = axis * 2 + (dir[axis] < 0 ? 1u : 0u);
    double lod = il_clamp(rough, 0, 1) * 2;
    u32 lo = (u32)lod, hi = lo < 2 ? lo + 1 : lo;
    double frac = lod - lo;
    for (u32 c = 0; c < 3; ++c)
        out[c] = (mode & 3u) ? il_faces[face][c] * (il_mips[lo] * (1 - frac) + il_mips[hi] * frac) *
                                   ((mode & 2u) ? 2 : 1)
                             : 1;
}
/* Known face texels and mip scales independently constrain each texture sample. A 1/32
 * LOD envelope covers filtering precision without assuming an Apple GPU quantizer. Integer
 * mips are exact; fractional samples must also remain a convex blend of adjacent mip texels.
 * This envelope applies only to the sampler stage; material/furnace limits stay unchanged. */
static void il_sampler_check(const double n[3], double mu, double rough, const float *sample) {
    double expected[3];
    il_radiance(n, mu, rough, 1, expected);
    double lod = il_clamp(rough, 0, 1) * 2;
    u32 lo = (u32)lod, hi = lo < 2 ? lo + 1 : lo;
    double fraction = lod - lo, first_weight = 0;
    for (u32 c = 0; c < 3; ++c) {
        double face = expected[c] / (il_mips[lo] * (1 - fraction) + il_mips[hi] * fraction);
        double lower = face * il_mips[hi], upper = face * il_mips[lo];
        double tolerance = fraction == 0 ? 0 : (upper - lower) / 32 + 3e-4;
        EXPECT_TRUE(isfinite(sample[c]));
        EXPECT_TRUE(sample[c] >= lower - 3e-4 && sample[c] <= upper + 3e-4);
        EXPECT_TRUE(fabs(sample[c] - expected[c]) <= tolerance);
        if (upper > lower) {
            double weight = (upper - sample[c]) / (upper - lower);
            if (c == 0)
                first_weight = weight;
            else
                EXPECT_TRUE(fabs(weight - first_weight) <= .001);
        }
    }
}
static void il_expected(const il_case *q, double diffuse[3], double specular[3], double producer[3],
                        double producer_diffuse[3], const float *sampled) {
    double view_mu = q->p0[3];
    double mu = q->p6[0] * sqrt(1 - view_mu * view_mu) + q->p6[2] * view_mu;
    double r = q->p0[0], metal = q->p0[1], material = q->p5[3];
    u32 mode = (u32)q->p4[3], flags = (u32)q->p3[3];
    bool film = (flags & 8u) && q->p3[0] > 0 && q->p3[2] > 0;
    double ao = (mode & 8u) ? fmin(material, .5) : (mode & 64u) ? material * .5 : material;
    double energy[4], f0[3], dielectric[3], film_base[3], film_dielectric[3];
    il_lookup(mu, r, false, energy);
    for (u32 c = 0; c < 3; ++c) {
        dielectric[c] = .04 * q->p2[3];
        f0[c] = dielectric[c] * (1 - metal) + q->p5[c] * metal;
    }
    if (film) {
        il_film_rgb(f0, mu, q->p3[1], q->p3[2], film_base);
        il_film_rgb(dielectric, mu, q->p3[1], q->p3[2], film_dielectric);
    }
    double normal[3] = {q->p6[0], q->p6[1], q->p6[2]}, radiance[3];
    il_radiance(normal, view_mu, r, mode, radiance);
    if ((mode & 3u) == 1u)
        for (u32 c = 0; c < 3; ++c)
            radiance[c] = sampled[c];
    double keep = 1, sheen_e = 0, coat_e = 0, coat_n[3], coat_mu = 0, coat_light[3] = {0},
           sheen_light[3] = {0};
    if ((flags & 2u) && fmax(q->p2[0], fmax(q->p2[1], q->p2[2])) > 0) {
        double t[4];
        il_lookup(mu, q->p1[2], true, t);
        sheen_e = il_clamp(t[0], 0, 1);
        keep = il_clamp(1 - fmax(q->p2[0], fmax(q->p2[1], q->p2[2])) * sheen_e, 0, 1);
        il_radiance(normal, view_mu, q->p1[2], mode, sheen_light);
        if ((mode & 3u) == 1u)
            for (u32 c = 0; c < 3; ++c)
                sheen_light[c] = sampled[8 + c];
    }
    double coat_keep = 1;
    if ((flags & 1u) && q->p1[0] > 0) {
        double length = sqrt(q->p4[0] * q->p4[0] + q->p4[1] * q->p4[1] + q->p4[2] * q->p4[2]);
        for (u32 c = 0; c < 3; ++c)
            coat_n[c] = length > 0 ? q->p4[c] / length : normal[c];
        coat_mu = coat_n[0] * sqrt(1 - view_mu * view_mu) + coat_n[2] * view_mu;
        if (coat_mu > 0) {
            double t[4];
            il_lookup(coat_mu, q->p1[1], false, t);
            coat_e = q->p1[0] * (.04 * t[0] + t[1]);
            coat_keep = il_clamp(1 - coat_e, 0, 1);
            il_radiance(coat_n, view_mu, q->p1[1], mode, coat_light);
            if ((mode & 3u) == 1u)
                for (u32 c = 0; c < 3; ++c)
                    coat_light[c] = sampled[4 + c];
        }
    }
    keep *= coat_keep;
    for (u32 c = 0; c < 3; ++c) {
        double base_f0 = f0[c], df0 = dielectric[c];
        if (film) {
            base_f0 += (film_base[c] - base_f0) * q->p3[0];
            df0 += (film_dielectric[c] - df0) * q->p3[0];
        }
        double f90 = q->p2[3] * (1 - metal) + metal;
        double e = il_energy(energy, base_f0, f90), ed = il_energy(energy, df0, q->p2[3]);
        producer[c] = il_energy(energy, f0[c], f90) * il_occlusion(mu, material, r);
        producer_diffuse[c] =
            q->p5[c] * (1 - metal) * (1 - il_energy(energy, dielectric[c], q->p2[3]));
        double rho = q->p5[c] * (1 - metal), dao = ao;
        if (mode & 8u)
            dao = il_clamp(fmax(ao, (((2.0404 * rho - .3324) * ao + (-4.7951 * rho + .6417)) * ao +
                                     2.7552 * rho + .6903) *
                                        ao),
                           0, 1);
        diffuse[c] = rho * (1 - ed) * ((mode & 1u) ? 0 : 1) * dao * keep;
        if (mode & 4u)
            diffuse[c] =
                rho * (1 - ed) * il_unhalf(il_half((float)(IL_PI * il_gi[c]))) / IL_PI * dao * keep;
        if (mode & 32u) {
            double target = .125 * (c + 1) * dao;
            if (!(mode & 256u) && (film || keep != 1)) {
                double baseline = rho * (1 - il_energy(energy, dielectric[c], q->p2[3]));
                target = baseline > 1e-8 ? target * rho * (1 - ed) * keep / baseline : diffuse[c];
            }
            double confidence = (mode & 512u) ? 1 : .5;
            diffuse[c] = diffuse[c] * (1 - confidence) + target * confidence;
        }
        double base = radiance[c] * e * il_occlusion(mu, ao, r) * keep;
        if (mode & 16u) {
            double override = .25 * (c + 1), old = il_occlusion(mu, material, r);
            if (mode & 8u)
                override *= old > 0 ? il_occlusion(mu, ao, r) / old : 0;
            if (film && !(mode & 256u)) {
                double baseline = producer[c];
                if (baseline > 1e-8)
                    override *= e * il_occlusion(mu, material, r) / baseline;
                else
                    override = radiance[c] * e * il_occlusion(mu, ao, r);
            }
            double confidence = (mode & 128u) ? 1 : .5;
            base = base * (1 - confidence) + override * keep * confidence;
        }
        specular[c] =
            base +
            q->p2[c] * sheen_e * sheen_light[c] * il_occlusion(mu, ao, q->p1[2]) * coat_keep +
            coat_light[c] * coat_e * il_occlusion(coat_mu, ao, q->p1[1]);
        if ((mode & 272u) == 272u) {
            double override = .25 * (c + 1), old = il_occlusion(mu, material, r);
            if (mode & 8u)
                override *= old > 0 ? il_occlusion(mu, ao, r) / old : 0;
            double original =
                radiance[c] * e * il_occlusion(mu, ao, r) * keep +
                q->p2[c] * sheen_e * sheen_light[c] * il_occlusion(mu, ao, q->p1[2]) * coat_keep +
                coat_light[c] * coat_e * il_occlusion(coat_mu, ao, q->p1[1]);
            double confidence = (mode & 128u) ? 1 : .5;
            specular[c] = original * (1 - confidence) + override * confidence;
        }
    }
}
static void il_quantized_cases(il_fx *f, il_case *cases, u32 count) {
    u32 *words[3];
    for (u32 t = 0; t < 3; ++t) {
        words[t] = calloc(count, 4);
        ASSERT_TRUE(words[t] != NULL);
    }
    for (u32 i = 0; i < count; ++i) {
        il_case *c = &cases[i];
        u32 packed = 0;
        for (u32 ch = 0; ch < 3; ++ch) {
            u32 v = (u32)floorf(de__gb_srgb_encode(c->p5[ch]) * 255 + .5f);
            packed |= v << (8 * ch);
            c->p5[ch] = de__gb_srgb_decode((float)v / 255);
        }
        u32 ao = (u32)floorf(c->p5[3] * 255 + .5f);
        c->p5[3] = (float)ao / 255;
        words[0][i] = packed | (ao << 24);
        const float normal[3] = {0, 0, 1};
        words[1][i] = de__gb1_pack(normal, c->p0[0]);
        de__gb1_unpack(words[1][i], c->p6, &c->p0[0]);
        u32 metal = (u32)floorf(c->p0[1] * 255 + .5f), spec = (u32)floorf(c->p2[3] * 255 + .5f);
        c->p0[1] = (float)metal / 255;
        c->p2[3] = (float)spec / 255;
        words[2][i] = metal | (spec << 24);
    }
    for (u32 t = 0; t < 3; ++t) {
        de_gpu_texture texture = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.format = de__gb_format(t),
                                           .width = count,
                                           .height = 1,
                                           .usage = DE_USAGE_COPY_DST | DE_USAGE_SAMPLED,
                                           .name = "IBL actual G-buffer"});
        ASSERT_TRUE(DE_HANDLE_VALID(texture));
        f->textures[f->texture_count++] = texture;
        u32 pitch = (count * 4 + 255) & ~255u;
        u8 *data = calloc(pitch, 1);
        ASSERT_TRUE(data != NULL);
        memcpy(data, words[t], count * 4);
        free(words[t]);
        il_tex_upload upload = {
            .t = texture, .src = il_upload(f, data, pitch, 0), .mips = 1, .pitch = {pitch}};
        free(data);
        il_submit(f, il_tex_record, &upload);
        f->config_data[9 + t] = de_gpu_texture_bindless(f->dev, texture, 0, false);
    }
    f->config =
        de_gpu_buffer_bindless(f->dev, il_upload(f, f->config_data, sizeof f->config_data, 16));
    il_submit(f, il_buffer_reads, f);
}
static void il_compare(il_fx *f, de_gpu_backend backend, il_case *cases, u32 count, bool furnace) {
    il_open(f, backend, true);
    il_resources(f);
    il_quantized_cases(f, cases, count);
    float *out = il_run(f, K_EVALUATE, cases, count, 1, 0, count * SK_OUT);
    ASSERT_TRUE(out != NULL);
    u32 bad = 0;
    double worst = 0, maximum = 0;
    for (u32 i = 0; i < count; ++i) {
        double d[3], s[3], producer[3], producer_diffuse[3];
        il_expected(&cases[i], d, s, producer, producer_diffuse, out + i * SK_OUT + 12);
        if (((u32)cases[i].p4[3] & 3u) == 1u) {
            double n[3] = {cases[i].p6[0], cases[i].p6[1], cases[i].p6[2]};
            il_sampler_check(n, cases[i].p0[3], cases[i].p0[0], out + i * SK_OUT + 12);
            il_sampler_check(n, cases[i].p0[3], fmax(cases[i].p1[2], .07), out + i * SK_OUT + 20);
            double cn[3] = {cases[i].p4[0], cases[i].p4[1], cases[i].p4[2]};
            double length = sqrt(cn[0] * cn[0] + cn[1] * cn[1] + cn[2] * cn[2]);
            for (u32 c = 0; c < 3; ++c)
                cn[c] = length > 0 ? cn[c] / length : n[c];
            il_sampler_check(cn, cases[i].p0[3], cases[i].p1[1], out + i * SK_OUT + 16);
        }
        for (u32 c = 0; c < 3; ++c) {
            double gd = out[i * SK_OUT + c], gs = out[i * SK_OUT + 4 + c],
                   gp = out[i * SK_OUT + 8 + c], gpd = out[i * SK_OUT + 28 + c];
            double error = fmax(fabs(gd - d[c]), fmax(fabs(gs - s[c]), fabs(gp - producer[c])));
            worst = fmax(worst, error);
            maximum = fmax(maximum, gd + gs);
            bool wrong =
                !isfinite(gd) || !isfinite(gs) || !isfinite(gp) || !isfinite(gpd) ||
                fabs(gpd - producer_diffuse[c]) > 3e-4 + .003 * fabs(producer_diffuse[c]) ||
                fabs(gd - d[c]) > 3e-4 + .003 * fabs(d[c]) ||
                fabs(gs - s[c]) > 3e-4 + .003 * fabs(s[c]) ||
                fabs(gp - producer[c]) > 3e-4 + .003 * fabs(producer[c]);
            if (wrong && bad < 8)
                printf(
                    "case %u channel %u: diffuse %.9g/%.9g specular %.9g/%.9g producer %.9g/%.9g\n",
                    i, c, gd, d[c], gs, s[c], gp, producer[c]);
            bad += wrong;
            if (furnace)
                EXPECT_TRUE(gd + gs <= 1.02);
        }
    }
    printf("IBL layers %u cases: max error %.9g maximum furnace %.9g failures %u\n", count, worst,
           maximum, bad);
    free(out);
    EXPECT_EQ(bad, 0u);
}
static void il_sampler_stage(il_fx *f, de_gpu_backend b) {
    il_open(f, b, true);
    il_resources(f);
    const float rough[] = {.07f,
                           .2f,
                           .3f,
                           .5f,
                           .65f,
                           .85f,
                           1,
                           (8.f - .001f) / 128,
                           8.f / 128,
                           (8.f + .001f) / 128,
                           (25.f - .001f) / 128,
                           25.f / 128,
                           (25.f + .001f) / 128,
                           (83.f - .001f) / 128,
                           83.f / 128,
                           (83.f + .001f) / 128};
    enum { count = sizeof rough / sizeof rough[0] };
    il_case cases[count];
    for (u32 i = 0; i < count; ++i) {
        cases[i] = il_base(.65f, 0, .6f);
        il_sheen(&cases[i], rough[i], true);
        cases[i].p4[3] = 1;
    }
    il_quantized_cases(f, cases, count);
    float *out = il_run(f, K_EVALUATE, cases, count, 1, 0, count * SK_OUT);
    ASSERT_TRUE(out != NULL);
    for (u32 i = 0; i < count; ++i) {
        double n[3] = {cases[i].p6[0], cases[i].p6[1], cases[i].p6[2]}, expected[3];
        il_radiance(n, cases[i].p0[3], rough[i], 1u, expected);
        printf("sampler r %.9g: raw %.9g %.9g %.9g predicted %.9g %.9g %.9g\n", rough[i],
               out[i * SK_OUT + 20], out[i * SK_OUT + 21], out[i * SK_OUT + 22], expected[0],
               expected[1], expected[2]);
        il_sampler_check(n, cases[i].p0[3], rough[i], out + i * SK_OUT + 20);
    }
    free(out);
}
static void il_furnace(il_fx *f, de_gpu_backend b, u32 layer) {
    static const float mu[4] = {.015625f, .2f, .6f, 1}, rough[3] = {.2f, .6f, 1};
    il_case cases[144];
    u32 n = 0;
    for (u32 k = 0; k < 2; ++k)
        for (u32 i = 0; i < 4; ++i)
            for (u32 j = 0; j < 3; ++j)
                for (u32 r = 0; r < 3; ++r) {
                    il_case c = il_base(rough[j], (float)k, mu[i]);
                    if (layer & 1u)
                        il_coat(&c, rough[r]);
                    if (layer & 2u)
                        il_sheen(&c, r == 0 ? .07f : rough[r], layer == 3);
                    if (layer & 8u)
                        il_film(&c, 1, 350);
                    cases[n++] = c;
                }
    il_compare(f, b, cases, n, true);
}
static void il_film_materials(il_fx *f, de_gpu_backend b) {
    il_case cases[24];
    const float rough[] = {.2f, .6f, 1};
    const float mu[] = {.015625f, .2f, .6f, 1};
    u32 n = 0;
    for (u32 metal = 0; metal < 2; ++metal)
        for (u32 r = 0; r < 3; ++r)
            for (u32 v = 0; v < 4; ++v) {
                il_case c = il_base(rough[r], (float)metal, mu[v]);
                if (metal) {
                    c.p5[0] = .9f;
                    c.p5[1] = .3f;
                    c.p5[2] = .05f;
                }
                il_film(&c, 1, 350);
                cases[n++] = c;
            }
    il_compare(f, b, cases, n, true);
}
static void il_composition(il_fx *f, de_gpu_backend b, u32 mode) {
    il_case cases[14];
    for (u32 i = 0; i < 10; ++i) {
        cases[i] = il_base(.65f, .0f, .6f);
        cases[i].p4[3] = (float)mode;
        cases[i].p5[0] = .8f;
        cases[i].p5[1] = .3f;
        cases[i].p5[2] = .1f;
        cases[i].p5[3] = .7f;
        if (i & 1u)
            il_coat(&cases[i], .2f);
        if (i & 2u)
            il_sheen(&cases[i], .85f, true);
        if (i & 4u)
            il_film(&cases[i], .8f, 350);
        if (i == 8 || i == 9) {
            cases[i].p5[0] = cases[i].p5[1] = cases[i].p5[2] = 0;
            il_coat(&cases[i], .1f);
            il_sheen(&cases[i], .3f, true);
            cases[i].p4[0] = .8660254f;
            cases[i].p4[2] = .5f;
        }
    }
    u32 count = 10;
    if (mode & 48u) {
        cases[10] = il_base(0, 1, 1);
        cases[10].p5[0] = cases[10].p5[1] = cases[10].p5[2] = 0;
        cases[10].p4[3] = (float)(mode | 128u | 512u);
        il_film(&cases[10], 1, 350);
        il_coat(&cases[10], .2f);
        il_sheen(&cases[10], .4f, true);
        cases[11] = il_base(.65f, 1, .6f);
        cases[11].p5[3] = 0;
        cases[11].p4[3] = (float)(mode | 128u | 512u);
        il_film(&cases[11], 1, 350);
        il_coat(&cases[11], .2f);
        il_sheen(&cases[11], .4f, true);
        count = 12;
    }
    if (mode & 48u) {
        cases[12] = il_base(0, 1, 1);
        cases[12].p5[0] = 0;
        cases[12].p5[1] = .3f;
        cases[12].p5[2] = .8f;
        cases[12].p4[3] = (float)(mode | 128u | 512u);
        il_coat(&cases[12], .2f);
        il_sheen(&cases[12], .4f, true);
        il_film(&cases[12], 1, 350);
        cases[13] = il_base(.65f, 0, .6f);
        cases[13].p5[0] = 0;
        cases[13].p5[1] = .3f;
        cases[13].p5[2] = .8f;
        cases[13].p5[3] = .7f;
        cases[13].p4[3] = (float)(mode | 128u | 512u);
        il_coat(&cases[13], .2f);
        il_sheen(&cases[13], .4f, true);
        il_film(&cases[13], 1, 350);
        count = 14;
    }
    il_compare(f, b, cases, count, false);
}
static void il_identity(il_fx *f, de_gpu_backend b) {
    il_open(f, b, true);
    il_resources(f);
    il_case cases[6];
    for (u32 i = 0; i < 6; ++i) {
        cases[i] = il_base(.65f, .5f, .6f);
        cases[i].p4[3] = 3;
    }
    il_coat(&cases[1], .2f);
    cases[1].p1[0] = 0;
    il_sheen(&cases[2], .8f, true);
    cases[2].p2[0] = cases[2].p2[1] = cases[2].p2[2] = 0;
    il_film(&cases[3], 0, 350);
    il_film(&cases[4], 1, 0);
    il_coat(&cases[5], .2f);
    il_sheen(&cases[5], .8f, true);
    il_film(&cases[5], 1, 350);
    cases[5].p3[3] = 0;
    il_quantized_cases(f, cases, 6);
    float *out = il_run(f, K_EVALUATE, cases, 6, 1, 0, 6 * SK_OUT);
    ASSERT_TRUE(out != NULL);
    for (u32 i = 1; i < 6; ++i)
        EXPECT_EQ(memcmp(out, out + i * SK_OUT, 12 * sizeof(float)), 0);
    free(out);
}
#define IL_GPU_TESTS(suffix, backend)                                                              \
    TEST_F(ibl_layers, film_material_response_##suffix, il_fx) {                                   \
        il_film_materials(f, backend);                                                             \
    }                                                                                              \
    TEST_F(ibl_layers, sampler_stage_##suffix, il_fx) {                                            \
        il_sampler_stage(f, backend);                                                              \
    }                                                                                              \
    TEST_F(ibl_layers, coat_furnace_##suffix, il_fx) {                                             \
        il_furnace(f, backend, 1);                                                                 \
    }                                                                                              \
    TEST_F(ibl_layers, sheen_furnace_##suffix, il_fx) {                                            \
        il_furnace(f, backend, 2);                                                                 \
    }                                                                                              \
    TEST_F(ibl_layers, colored_stack_furnace_##suffix, il_fx) {                                    \
        il_furnace(f, backend, 3);                                                                 \
    }                                                                                              \
    TEST_F(ibl_layers, film_stack_furnace_##suffix, il_fx) {                                       \
        il_furnace(f, backend, 11);                                                                \
    }                                                                                              \
    TEST_F(ibl_layers, sky_direction_roughness_##suffix, il_fx) {                                  \
        il_composition(f, backend, 1);                                                             \
    }                                                                                              \
    TEST_F(ibl_layers, probe_direction_roughness_##suffix, il_fx) {                                \
        il_composition(f, backend, 2);                                                             \
    }                                                                                              \
    TEST_F(ibl_layers, gi_diffuse_response_##suffix, il_fx) {                                      \
        il_composition(f, backend, 4);                                                             \
    }                                                                                              \
    TEST_F(ibl_layers, ssr_layer_preservation_##suffix, il_fx) {                                   \
        il_composition(f, backend, 16);                                                            \
    }                                                                                              \
    TEST_F(ibl_layers, gtao_ssr_once_##suffix, il_fx) {                                            \
        il_composition(f, backend, 24);                                                            \
    }                                                                                              \
    TEST_F(ibl_layers, gtao_gi_diffuse_##suffix, il_fx) {                                          \
        il_composition(f, backend, 12);                                                            \
    }                                                                                              \
    TEST_F(ibl_layers, legacy_ao_ssr_##suffix, il_fx) {                                            \
        il_composition(f, backend, 80);                                                            \
    }                                                                                              \
    TEST_F(ibl_layers, ssr_full_confidence_##suffix, il_fx) {                                      \
        il_composition(f, backend, 144);                                                           \
    }                                                                                              \
    TEST_F(ibl_layers, legacy_outgoing_specular_##suffix, il_fx) {                                 \
        il_composition(f, backend, 272);                                                           \
    }                                                                                              \
    TEST_F(ibl_layers, ssgi_layer_response_##suffix, il_fx) {                                      \
        il_composition(f, backend, 32);                                                            \
    }                                                                                              \
    TEST_F(ibl_layers, ssgi_full_confidence_##suffix, il_fx) {                                     \
        il_composition(f, backend, 544);                                                           \
    }                                                                                              \
    TEST_F(ibl_layers, gtao_ssgi_once_##suffix, il_fx) {                                           \
        il_composition(f, backend, 40);                                                            \
    }                                                                                              \
    TEST_F(ibl_layers, gtao_ssgi_full_confidence_##suffix, il_fx) {                                \
        il_composition(f, backend, 552);                                                           \
    }                                                                                              \
    TEST_F(ibl_layers, legacy_outgoing_diffuse_##suffix, il_fx) {                                  \
        il_composition(f, backend, 288);                                                           \
    }                                                                                              \
    TEST_F(ibl_layers, inactive_identity_##suffix, il_fx) {                                        \
        il_identity(f, backend);                                                                   \
    }
IL_GPU_TESTS(metal, DE_GPU_METAL)
IL_GPU_TESTS(vulkan, DE_GPU_VULKAN)
IL_GPU_TESTS(d3d12, DE_GPU_D3D12)
