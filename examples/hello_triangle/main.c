/* hello_triangle — first pixels through the public de_gpu API on every backend (DENG-327).
 *
 *   hello_triangle [--backend metal|vulkan|d3d12|default] [--scene triangle|quad] [--frames N]
 *                  [--adapter N] [--offscreen] [--screenshot out.png] [--check]
 *                  [--pack file.dpak] [--assets dir] [--cache-dir dir]
 *
 * Scene `triangle`: one triangle whose vertices the shader pulls from a bindless buffer, with its
 * colour in the push constants. Scene `quad`: a checker texture, uploaded through an UPLOAD buffer
 * and de_gpu_cmd_copy_buffer_to_texture, sampled through bindless texture and sampler indices.
 * Shaders are `shader` assets (assets.toml), cooked in-process in SOURCE mode or read from a pack.
 *
 * Exit status: 0 done, 1 failure (any error logged, or a --check pixel differs), 2 usage,
 * 77 the backend, a Base-tier device or a display is not available here (CTest: skipped).
 */
#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader_glue.h"

#define FRAMES_IN_FLIGHT 2u
#define OFFSCREEN_SIZE   256u /* the --offscreen target; the --check pixels assume it */
#define EXIT_SKIP        77
#define WAIT_NS          (120ull * 1000000000ull)

#if defined(DE_DEBUG) || defined(DE_DEV)
#define HELLO_VALIDATION true /* the backend's API validation (Vulkan layer, D3D12 debug layer) */
#else
#define HELLO_VALIDATION false
#endif

typedef struct options {
    de_gpu_backend backend;
    bool quad, offscreen, check;
    u32 frames;  /* 0 = until the window closes */
    u32 adapter; /* 0 = the best one that meets the Base tier; n = the n-th (de_gpu_device_desc) */
    const char *screenshot, *pack, *assets, *cache_dir;
} options;

typedef struct app {
    options o;
    de_task_system *tasks;
    de_asset_system *assets;
    hello_shaders *shader_types;
    de_load_handle load;
    const char *cache_root; /* "hcache" (--cache-dir) or "cache" */
    bool platform;
    de_window window;
    de_gpu_device *d;
    de_gpu_backend backend;
    de_gpu_swapchain sc;
    de_gpu_texture offscreen;
    de_gpu_format format;
    u32 width, height;
    de_gpu_pipeline pipe;
    de_gpu_buffer positions;
    de_gpu_texture tex;
    de_gpu_sampler smp;
    de_gpu_fence fence;
    u64 frame;
    u32 errors; /* records of level ERROR or above */
} app;

/* Any engine error (validation messages included) makes the run fail. */
static void count_errors(const de_log_record *rec, void *user) {
    if (rec->level >= DE_LOG_ERROR)
        ((app *)user)->errors++;
}

/* Prints the failed call and the error chain; returns 1, or EXIT_SKIP for DE_ERR_UNSUPPORTED. */
static int fail(const char *what) {
    const de_error *e = de_error_last();
    fprintf(stderr, "hello_triangle: %s failed", what);
    for (; e != NULL; e = e->cause)
        fprintf(stderr, ": %s", e->message);
    fprintf(stderr, "\n");
    e = de_error_last();
    return e != NULL && e->result == DE_ERR_UNSUPPORTED ? EXIT_SKIP : 1;
}

static int usage(const char *bad) {
    fprintf(stderr,
            "hello_triangle: bad argument '%s'\nusage: hello_triangle [--backend "
            "metal|vulkan|d3d12|default] [--scene triangle|quad] [--frames N] "
            "[--adapter N] [--offscreen] [--screenshot out.png] [--check] [--pack file.dpak] "
            "[--assets dir] [--cache-dir dir]\n",
            bad);
    return 2;
}

static int parse(int argc, char **argv, options *o) {
    o->assets = HELLO_ASSET_DIR;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--offscreen") == 0) {
            o->offscreen = true;
            continue;
        }
        if (strcmp(a, "--check") == 0) {
            o->check = true;
            continue;
        }
        if (v == NULL)
            return usage(a);
        ++i; /* every other option takes a value */
        if (strcmp(a, "--backend") == 0) {
            static const char *const names[] = {"default", "d3d12", "vulkan", "metal"};
            o->backend = DE_GPU_BACKEND_COUNT_;
            for (u32 b = 0; b < 4u; ++b)
                if (strcmp(v, names[b]) == 0)
                    o->backend = (de_gpu_backend)b; /* the order of de_gpu_backend */
            if (o->backend == DE_GPU_BACKEND_COUNT_)
                return usage(v);
        } else if (strcmp(a, "--scene") == 0) {
            if (strcmp(v, "quad") != 0 && strcmp(v, "triangle") != 0)
                return usage(v);
            o->quad = strcmp(v, "quad") == 0;
        } else if (strcmp(a, "--frames") == 0) {
            o->frames = (u32)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--adapter") == 0) {
            o->adapter = (u32)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--screenshot") == 0) {
            o->screenshot = v;
        } else if (strcmp(a, "--pack") == 0) {
            o->pack = v;
        } else if (strcmp(a, "--assets") == 0) {
            o->assets = v;
        } else if (strcmp(a, "--cache-dir") == 0) {
            o->cache_dir = v;
        } else {
            return usage(a);
        }
    }
    if (o->check && (!o->offscreen || o->screenshot == NULL))
        return usage("--check (needs --offscreen and --screenshot)");
    if (o->screenshot != NULL && o->frames == 0)
        o->frames = 3; /* the screenshot is taken on the last frame */
    return 0;
}

/* ---------- Assets: shaders (SOURCE mode or a pack) and the checker texture ---------- */

static int load_assets(app *a) {
    de_fp_env_init_thread(); /* the main thread runs tasks too (de_task.h) */
    de_task_system_desc td = {.worker_count = 2, .background_worker_count = 1};
    if (de_task_system_create(&td, &a->tasks) != DE_OK)
        return fail("de_task_system_create");
    /* Virtual roots: hello:/ is this directory; the cook cache (.decache) and the pipeline
     * cache go to hcache:/ (--cache-dir) or to the standard cache:/ of the app. */
    if (a->o.cache_dir != NULL) {
        if (de_fs_mount("hcache", a->o.cache_dir, DE_MOUNT_WRITABLE, 0) != DE_OK)
            return fail("mounting --cache-dir");
    } else if (de_fs_init(&(de_fs_desc){.app = "hello_triangle"}) != DE_OK) {
        return fail("de_fs_init");
    }
    a->cache_root = a->o.cache_dir != NULL ? "hcache" : "cache";
    if (de_fs_mount("hello", a->o.assets, 0, 0) != DE_OK)
        return fail("mounting the asset directory");
    char decache[32];
    snprintf(decache, sizeof decache, "%s:/decache", a->cache_root);
    de_asset_system_desc ad = {.tasks = a->tasks, .io_threads = 1, .cache_dir = decache};
    ad.mode = a->o.pack != NULL ? DE_ASSET_MODE_PACKED : DE_ASSET_MODE_SOURCE;
    if (de_asset_system_create(&ad, &a->assets) != DE_OK)
        return fail("de_asset_system_create");
    if (hello_shaders_register(a->assets, &a->shader_types) != DE_OK)
        return fail("registering the shader type");
    if (a->o.pack != NULL) {
        /* PACKED mode: a pack cooked from assets.toml; its directory becomes hpack:/. */
        char dir[DE_FS_PATH_MAX], vpath[DE_FS_PATH_MAX];
        const char *slash = strrchr(a->o.pack, '/'), *bs = strrchr(a->o.pack, '\\');
        slash = bs != NULL && (slash == NULL || bs > slash) ? bs : slash;
        snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - a->o.pack) : 1,
                 slash ? a->o.pack : ".");
        snprintf(vpath, sizeof vpath, "hpack:/%s", slash ? slash + 1 : a->o.pack);
        if (de_fs_mount("hpack", dir, 0, 0) != DE_OK ||
            de_asset_pack_mount(a->assets, vpath, 0) != DE_OK)
            return fail("mounting --pack");
    } else {
        if (!hello_shaders_can_cook()) {
            fprintf(stderr, "hello_triangle: this build cannot cook shaders (Release, Shipping "
                            "or no tools): run with --pack\n");
            return EXIT_SKIP;
        }
        if (de_asset_manifest_load(a->assets, "hello:/assets.toml") != DE_OK)
            return fail("de_asset_manifest_load");
    }
    const de_asset_id ids[3] = {de_asset_id_from("shaders/triangle"),
                                de_asset_id_from("shaders/quad"),
                                de_asset_id_from("textures/checker")};
    a->load = de_asset_load_many(a->assets, ids, 3, 0);
    if (de_asset_wait(a->assets, a->load, WAIT_NS) != DE_LOAD_READY) {
        char msg[512] = "?";
        for (u32 i = 0; i < 3; ++i)
            if (de_asset_last_error(a->assets, ids[i], msg, sizeof msg) == DE_OK)
                fprintf(stderr, "hello_triangle: asset %u: %s\n", i, msg);
        fprintf(stderr, "hello_triangle: the assets did not load\n");
        return 1;
    }
    return 0;
}

/* ---------- GPU objects ---------- */

static int make_pipeline(app *a) {
    const char *asset = a->o.quad ? "shaders/quad" : "shaders/triangle";
    de_shader sh = de_asset_shader(a->assets, de_asset_id_from(asset));
    de_gpu_pipeline_desc pd = {.kind = DE_PIPE_GRAPHICS,
                               .color_formats = {a->format},
                               .color_count = 1,
                               .cull = DE_CULL_NONE,
                               .name = asset};
    u32 vs_bytes = 0, ps_bytes = 0;
    if (hello_shader_get(a->assets, sh, a->backend, "vs_main", &pd.vs, &vs_bytes) != DE_OK ||
        hello_shader_get(a->assets, sh, a->backend, "ps_main", &pd.ps, &ps_bytes) != DE_OK) {
        /* DE_ERR_NOT_FOUND: not cooked for this backend's target (DXIL without DXC). */
        bool missing = de_error_last() != NULL && de_error_last()->result == DE_ERR_NOT_FOUND;
        int rc = fail("getting the shader code for this backend");
        return missing ? EXIT_SKIP : rc;
    }
    pd.push_constant_bytes = vs_bytes > ps_bytes ? vs_bytes : ps_bytes;
    a->pipe = de_gpu_pipeline_create(a->d, &pd);
    return DE_HANDLE_VALID(a->pipe) ? 0 : fail("de_gpu_pipeline_create");
}

static void submit_and_signal(app *a, de_gpu_cmd *c) {
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = a->fence, .signal_value = ++a->frame};
    if (de_gpu_queue_submit(a->d, DE_QUEUE_GRAPHICS, &s) != DE_OK)
        (void)fail("de_gpu_queue_submit");
}

/* Scene 1: three clip-space positions the vertex shader reads by bindless index. Shaders read
 * de_buffer<T> as a read-write structured buffer (shaders/common/bindless.slang), so the buffer is
 * GPU_ONLY STORAGE in DE_ACCESS_SHADER_WRITE (a UAV on D3D12), filled by a copy from UPLOAD memory.
 */
static int make_triangle(app *a) {
    static const f32 pos[3][4] = {
        {-0.6f, -0.6f, 0.5f, 1.0f}, {0.6f, -0.6f, 0.5f, 1.0f}, {0.0f, 0.6f, 0.5f, 1.0f}};
    de_gpu_buffer up =
        de_gpu_buffer_create(a->d, &(de_gpu_buffer_desc){.size = sizeof pos,
                                                         .usage = DE_USAGE_COPY_SRC,
                                                         .memory = DE_MEM_UPLOAD,
                                                         .name = "positions upload"});
    a->positions = de_gpu_buffer_create(
        a->d, &(de_gpu_buffer_desc){.size = sizeof pos,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                    .stride = 16,
                                    .name = "triangle positions"});
    void *p = de_gpu_buffer_map(a->d, up);
    de_gpu_cmd *c = de_gpu_cmd_begin(a->d, DE_QUEUE_GRAPHICS, "positions upload");
    if (p == NULL || !DE_HANDLE_VALID(a->positions) || c == NULL)
        return fail("creating the position buffer");
    memcpy(p, pos, sizeof pos);
    de_gpu_barrier b = {
        .buffer = a->positions, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer(c, a->positions, 0, up, 0, sizeof pos);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_WRITE;
    de_gpu_cmd_barriers(c, &b, 1);
    submit_and_signal(a, c);         /* the frames come later on the same queue */
    de_gpu_buffer_destroy(a->d, up); /* released once the copy is done */
    return 0;
}

/* Scene 2: decode the PNG blob, copy it through an UPLOAD buffer into a sampled texture. */
static int make_quad(app *a) {
    u64 size = 0;
    de_blob blob = de_asset_blob(a->assets, de_asset_id_from("textures/checker"));
    const void *png = de_blob_data(a->assets, blob, &size);
    int w = 0, h = 0, comp = 0;
    stbi_uc *px = png ? stbi_load_from_memory(png, (int)size, &w, &h, &comp, 4) : NULL;
    if (px == NULL) {
        fprintf(stderr, "hello_triangle: cannot decode textures/checker\n");
        return 1;
    }
    const u32 pitch = ((u32)w * 4u + 255u) & ~255u; /* row pitch: a multiple of 256 */
    de_gpu_buffer up = de_gpu_buffer_create(a->d, &(de_gpu_buffer_desc){.size = (u64)pitch * (u32)h,
                                                                        .usage = DE_USAGE_COPY_SRC,
                                                                        .memory = DE_MEM_UPLOAD,
                                                                        .name = "checker upload"});
    u8 *dst = (u8 *)de_gpu_buffer_map(a->d, up);
    for (int y = 0; dst != NULL && y < h; ++y)
        memcpy(dst + (size_t)y * pitch, px + (size_t)y * (size_t)w * 4u, (size_t)w * 4u);
    stbi_image_free(px);
    /* An sRGB texture on an sRGB swapchain shows the PNG's colours; the offscreen target is
     * UNORM, so the bytes pass through unchanged and the --check pixels are exact. */
    bool srgb = a->format == DE_FMT_BGRA8_SRGB || a->format == DE_FMT_RGBA8_SRGB;
    a->tex = de_gpu_texture_create(
        a->d, &(de_gpu_texture_desc){.format = srgb ? DE_FMT_RGBA8_SRGB : DE_FMT_RGBA8_UNORM,
                                     .width = (u32)w,
                                     .height = (u32)h,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                     .name = "checker"});
    a->smp = de_gpu_sampler_create(
        a->d, &(de_gpu_sampler_desc){.u = DE_ADDR_CLAMP, .v = DE_ADDR_CLAMP, .w = DE_ADDR_CLAMP});
    de_gpu_cmd *c = de_gpu_cmd_begin(a->d, DE_QUEUE_GRAPHICS, "checker upload");
    if (dst == NULL || !DE_HANDLE_VALID(a->tex) || !DE_HANDLE_VALID(a->smp) || c == NULL)
        return fail("creating the checker texture");
    de_gpu_barrier b = {.texture = a->tex, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(c, a->tex, 0, 0, up, 0, pitch);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &b, 1);
    submit_and_signal(a, c);         /* the frames come later on the same queue */
    de_gpu_buffer_destroy(a->d, up); /* released once the copy is done */
    return 0;
}

/* ---------- Frames ---------- */

static void record_scene(app *a, de_gpu_cmd *c, de_gpu_texture target) {
    de_gpu_attachment color = {
        .texture = target, .clear = true, .clear_color = {0.2f, 0.4f, 0.6f, 1.0f}, .store = true};
    de_gpu_cmd_begin_render(c, &color, 1, NULL);
    de_gpu_cmd_set_pipeline(c, a->pipe);
    if (a->o.quad) {
        const struct {
            u32 tex, smp;
            f32 scale;
        } pc = {de_gpu_texture_bindless(a->d, a->tex, 0, false),
                de_gpu_sampler_bindless(a->d, a->smp), 0.75f};
        de_gpu_cmd_push_constants(c, &pc, sizeof pc);
        de_gpu_cmd_draw(c, 6, 1, 0, 0);
    } else {
        const struct {
            f32 color[4];
            u32 positions;
        } pc = {{1.0f, 0.8f, 0.2f, 1.0f}, de_gpu_buffer_bindless(a->d, a->positions)};
        de_gpu_cmd_push_constants(c, &pc, sizeof pc);
        de_gpu_cmd_draw(c, 3, 1, 0, 0);
    }
    de_gpu_cmd_end_render(c);
}

/* The --check pixels of the 256x256 offscreen target: (x, y) from the top-left, RGBA8. */
typedef struct pixel {
    u32 x, y;
    u8 rgba[4];
} pixel;
static const pixel k_triangle_px[4] = {{128, 128, {255, 204, 51, 255}},
                                       {128, 200, {255, 204, 51, 255}},
                                       {10, 10, {51, 102, 153, 255}},
                                       {245, 245, {51, 102, 153, 255}}};
static const pixel k_quad_px[4] = {{44, 44, {0xFF, 0x00, 0x00, 0xFF}},   /* cell (0, 0): red */
                                   {68, 44, {0x20, 0x30, 0x60, 0xFF}},   /* cell (1, 0) */
                                   {212, 212, {0xF0, 0xA0, 0x20, 0xFF}}, /* cell (7, 7) */
                                   {10, 10, {51, 102, 153, 255}}};       /* clear colour */

/* Writes the read-back image as PNG (RGBA) and checks the pixels. Returns the failures. */
static u32 screenshot(app *a, u8 *rows, u32 pitch) {
    const bool bgra = a->format == DE_FMT_BGRA8_SRGB || a->format == DE_FMT_BGRA8_UNORM;
    for (u32 y = 0; bgra && y < a->height; ++y)
        for (u32 x = 0; x < a->width; ++x) {
            u8 *p = rows + (size_t)y * pitch + (size_t)x * 4u, t = p[0];
            p[0] = p[2];
            p[2] = t;
        }
    u32 bad = 0;
    if (!stbi_write_png(a->o.screenshot, (int)a->width, (int)a->height, 4, rows, (int)pitch)) {
        fprintf(stderr, "hello_triangle: cannot write %s\n", a->o.screenshot);
        bad++;
    }
    const pixel *px = a->o.quad ? k_quad_px : k_triangle_px;
    for (u32 i = 0; a->o.check && i < 4u; ++i) {
        const u8 *p = rows + (size_t)px[i].y * pitch + (size_t)px[i].x * 4u;
        bool ok = memcmp(p, px[i].rgba, 4) == 0;
        bad += ok ? 0u : 1u;
        printf("pixel (%u, %u): %u %u %u %u%s\n", px[i].x, px[i].y, p[0], p[1], p[2], p[3],
               ok ? "" : "  MISMATCH");
    }
    return bad;
}

/* Renders one frame. `last`: also copy the image to a READBACK buffer and write the screenshot. */
static int frame(app *a, bool last) {
    /* At most FRAMES_IN_FLIGHT frames queued: wait for the one before those. */
    if (a->frame >= FRAMES_IN_FLIGHT &&
        de_gpu_fence_wait(a->d, a->fence, a->frame + 1u - FRAMES_IN_FLIGHT, WAIT_NS) != DE_OK)
        return fail("de_gpu_fence_wait");
    de_gpu_texture target = a->offscreen;
    if (!a->o.offscreen) {
        de_result r = de_gpu_swapchain_acquire(a->d, a->sc, &target);
        if (r == DE_ERR_BUSY) /* minimised, or the size changed: try again next frame */
            return 0;
        if (r != DE_OK)
            return fail("de_gpu_swapchain_acquire");
    }
    const bool shot = last && a->o.screenshot != NULL;
    const u32 pitch = (a->width * 4u + 255u) & ~255u;
    de_gpu_buffer rb = {0};
    if (shot)
        rb = de_gpu_buffer_create(a->d, &(de_gpu_buffer_desc){.size = (u64)pitch * a->height,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK,
                                                              .name = "screenshot"});
    de_gpu_cmd *c = de_gpu_cmd_begin(a->d, DE_QUEUE_GRAPHICS, "frame");
    if (c == NULL || (shot && !DE_HANDLE_VALID(rb)))
        return fail("starting the frame");
    de_gpu_barrier b = {.texture = target,
                        .before = DE_ACCESS_NONE /* cleared anyway */,
                        .after = DE_ACCESS_RENDER_TARGET};
    de_gpu_cmd_barriers(c, &b, 1);
    record_scene(a, c, target);
    b.before = DE_ACCESS_RENDER_TARGET;
    if (shot) {
        b.after = DE_ACCESS_COPY_SRC;
        de_gpu_cmd_barriers(c, &b, 1);
        de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, pitch, target, 0, 0);
        b.before = DE_ACCESS_COPY_SRC;
    }
    if (!a->o.offscreen) {
        b.after = DE_ACCESS_PRESENT;
        de_gpu_cmd_barriers(c, &b, 1);
    }
    submit_and_signal(a, c);
    if (!a->o.offscreen && de_gpu_swapchain_present(a->d, a->sc) != DE_OK)
        return fail("de_gpu_swapchain_present");
    if (!shot)
        return 0;
    if (de_gpu_fence_wait(a->d, a->fence, a->frame, WAIT_NS) != DE_OK)
        return fail("waiting for the screenshot");
    u32 bad = screenshot(a, (u8 *)de_gpu_buffer_map(a->d, rb), pitch);
    de_gpu_buffer_destroy(a->d, rb);
    return bad == 0 ? 0 : 1;
}

/* ---------- Main ---------- */

static int run(app *a) {
    int rc = load_assets(a);
    if (rc != 0)
        return rc;
    a->backend = a->o.backend == DE_GPU_DEFAULT ? de_gpu_platform_default_backend() : a->o.backend;
    if (!a->o.offscreen) {
        /* The window is set up for the backend's surface kind before the device exists. */
        de_result r = de_platform_init(&(de_platform_desc){.app_name = "hello_triangle"});
        if (r != DE_OK)
            return fail("de_platform_init (no display? try --offscreen)");
        a->platform = true;
        de_window_desc wd = {.title = "hello_triangle",
                             .width = 800,
                             .height = 600,
                             .display_index = -1,
                             .resizable = true,
                             .high_dpi = true};
        wd.surface = a->backend == DE_GPU_VULKAN  ? DE_WINDOW_SURFACE_VULKAN
                     : a->backend == DE_GPU_METAL ? DE_WINDOW_SURFACE_METAL
                                                  : DE_WINDOW_SURFACE_NONE;
        if (de_window_create(&wd, &a->window) != DE_OK)
            return fail("de_window_create");
    }
    char pipecache[32];
    snprintf(pipecache, sizeof pipecache, "%s:/pipelines", a->cache_root);
    de_gpu_device_desc dd = {.backend = a->backend,
                             .adapter = a->o.adapter,
                             .debug_layer = HELLO_VALIDATION,
                             .pipeline_cache_dir = pipecache};
    if (de_gpu_device_create(&dd, &a->d) != DE_OK)
        return fail("de_gpu_device_create");
    const de_gpu_caps *caps = de_gpu_get_caps(a->d);
    printf("hello_triangle: %s on %s (%s)\n", a->o.quad ? "quad" : "triangle",
           de_gpu_backend_name(caps->backend), caps->device_name);
    /* Like the renderer, refuse a device below the Base tier (SPEC §3.3): bindless needs it. */
    de_gpu_tier_report tier;
    de_gpu_evaluate_tier(caps, &tier);
    if (!tier.base_ok) {
        fprintf(stderr, "hello_triangle: %s is below the Base tier: %s\n", caps->device_name,
                tier.base_reason_count > 0 ? tier.base_reasons[0] : "?");
        return EXIT_SKIP;
    }
    a->fence = de_gpu_fence_create(a->d, 0);
    if (a->o.offscreen) {
        a->format = DE_FMT_RGBA8_UNORM;
        a->width = a->height = OFFSCREEN_SIZE;
        a->offscreen = de_gpu_texture_create(
            a->d, &(de_gpu_texture_desc){.format = a->format,
                                         .width = a->width,
                                         .height = a->height,
                                         .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                         .name = "offscreen target"});
    } else {
        a->format = DE_FMT_BGRA8_SRGB; /* what every backend's swapchain picks today */
        de_window_size_pixels(a->window, &a->width, &a->height);
        a->sc = de_gpu_swapchain_create(a->d, &(de_gpu_swapchain_desc){.window = a->window});
    }
    if (!DE_HANDLE_VALID(a->fence) || !(DE_HANDLE_VALID(a->offscreen) || DE_HANDLE_VALID(a->sc)))
        return fail("creating the render target");
    if ((rc = make_pipeline(a)) != 0 || (rc = a->o.quad ? make_quad(a) : make_triangle(a)) != 0)
        return rc;

    bool quit = false;
    for (u32 n = 0; !quit && (a->o.frames == 0 || n < a->o.frames); ++n) {
        de_app_event ev[16];
        u32 count = a->platform ? de_platform_pump(ev, 16) : 0u;
        for (u32 i = 0; i < count; ++i) {
            if (ev[i].kind == DE_EV_QUIT || ev[i].kind == DE_EV_WINDOW_CLOSE)
                quit = true;
            if (ev[i].kind == DE_EV_WINDOW_RESIZED && ev[i].a > 0 && ev[i].b > 0) {
                a->width = ev[i].a;
                a->height = ev[i].b;
                if (de_gpu_swapchain_resize(a->d, a->sc, a->width, a->height) != DE_OK)
                    return fail("de_gpu_swapchain_resize");
            }
        }
        if ((rc = frame(a, a->o.frames != 0 && n + 1u == a->o.frames)) != 0)
            return rc;
    }
    return 0;
}

static void shutdown(app *a) {
    if (a->d != NULL) {
        de_gpu_wait_idle(a->d);
        de_gpu_pipeline_destroy(a->d, a->pipe);
        de_gpu_buffer_destroy(a->d, a->positions);
        de_gpu_texture_destroy(a->d, a->tex);
        de_gpu_sampler_destroy(a->d, a->smp);
        de_gpu_texture_destroy(a->d, a->offscreen);
        if (DE_HANDLE_VALID(a->sc))
            de_gpu_swapchain_destroy(a->d, a->sc);
        de_gpu_fence_destroy(a->d, a->fence);
        de_gpu_device_destroy(a->d);
    }
    if (DE_HANDLE_VALID(a->window))
        de_window_destroy(a->window);
    if (a->platform)
        de_platform_shutdown();
    if (a->assets != NULL) {
        de_asset_release(a->assets, a->load);
        de_asset_system_destroy(a->assets);
    }
    hello_shaders_destroy(a->shader_types);
    if (a->tasks != NULL)
        de_task_system_destroy(a->tasks);
    de_fs_shutdown();
}

int main(int argc, char **argv) {
    static app a; /* zeroed */
    int rc = parse(argc, argv, &a.o);
    if (rc != 0)
        return rc;
    (void)de_log_init(NULL);
    de_log_add_sink(count_errors, &a);
    rc = run(&a);
    shutdown(&a);
    de_log_flush(); /* every error logged so far has reached count_errors */
    if (rc == 0 && a.errors != 0) {
        fprintf(stderr, "hello_triangle: %u error(s) were logged\n", a.errors);
        rc = 1;
    }
    de_log_shutdown();
    if (rc == 0)
        printf("hello_triangle: done\n");
    return rc;
}
