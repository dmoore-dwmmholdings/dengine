/* Sky Hop: original native D-Engine arcade demo. Design: crisp / friendly / readable.
 * Cream sky, deep ink, teal pipes and gold bird; bespoke 5x7 labels. One scene, one flap action.
 * READY -> PLAYING -> DEAD; P freezes and resumes. Canvas letterboxes to any window size.
 * Renderer/asset bootstrap follows examples/hello_triangle using only public engine APIs. */
#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_input.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_shader.h>
#include <dengine/de_task.h>
#include <stb_image_write.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#define FRAMES_IN_FLIGHT 2u
#define MAX_RECTS        8192u
#define WAIT_NS          (120ull * 1000000000ull)
#define EXIT_SKIP        77
#define HELLO_VALIDATION true
typedef struct options {
    de_gpu_backend backend;
    bool offscreen, check, smoke, self_check, quad;
    u32 frames, adapter;
    const char *screenshot, *pack, *assets, *cache_dir;
} options;
typedef struct rect {
    float bounds[4], color[4];
} rect;
typedef struct app {
    options o;
    de_task_system *tasks;
    de_asset_system *assets;
    de_load_handle load;
    const char *cache_root;
    bool platform;
    de_window window;
    de_input *input;
    de_gpu_device *d;
    de_gpu_backend backend;
    de_gpu_swapchain sc;
    de_gpu_texture offscreen;
    de_gpu_format format;
    u32 width, height;
    de_gpu_pipeline pipe;
    de_gpu_buffer rectangles[FRAMES_IN_FLIGHT];
    rect *maps[FRAMES_IN_FLIGHT];
    rect *drawing;
    u32 count;
    de_gpu_fence fence;
    u64 frame;
    u32 errors;
    game game;
} app;
static void count_errors(const de_log_record *r, void *user) {
    if (r->level >= DE_LOG_ERROR)
        ((app *)user)->errors++;
}
static int fail(const char *what) {
    fprintf(stderr, "sky_hop: %s", what);
    for (const de_error *e = de_error_last(); e; e = e->cause)
        fprintf(stderr, ": %s", e->message);
    fputc('\n', stderr);
    return 1;
}
static int parse(int argc, char **argv, options *o) {
    o->assets = HELLO_ASSET_DIR;
    o->backend = DE_GPU_METAL;
    for (int i = 1; i < argc; ++i) {
        const char *s = argv[i];
        if (!strcmp(s, "--self-check"))
            o->self_check = true;
        else if (!strcmp(s, "--offscreen"))
            o->offscreen = true;
        else if (!strcmp(s, "--smoke"))
            o->smoke = true;
        else if (!strcmp(s, "--check"))
            o->check = true;
        else if (i + 1 < argc && !strcmp(s, "--frames"))
            o->frames = (u32)strtoul(argv[++i], NULL, 10);
        else if (i + 1 < argc && !strcmp(s, "--screenshot"))
            o->screenshot = argv[++i];
        else if (i + 1 < argc && !strcmp(s, "--cache-dir"))
            o->cache_dir = argv[++i];
        else {
            fprintf(stderr, "Sky Hop: unknown option %s\n", s);
            return 2;
        }
    }
    if (o->screenshot && !o->frames)
        o->frames = 3;
    if (o->smoke && !o->frames)
        o->frames = 360;
    return 0;
}
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
    } else if (de_fs_init(&(de_fs_desc){.app = "sky_hop"}) != DE_OK) {
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
    /* The `shader` type (de_shader.h): in Debug and Dev builds with the tools it cooks .slang
     * sources in SOURCE mode through the in-process cooker. */
    if (de_shader_register_type(a->assets, NULL) != DE_OK)
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
#if DE_HAS_COOKER /* Debug and Dev builds with the tools: the in-process cooker */
        if (de_asset_manifest_load(a->assets, "hello:/assets.toml") != DE_OK)
            return fail("de_asset_manifest_load");
#else
        fprintf(stderr, "sky_hop: this build cannot cook shaders (Release, Shipping "
                        "or no tools): run with --pack\n");
        return EXIT_SKIP;
#endif
    }
    const de_asset_id ids[1] = {de_asset_id_from("shaders/sky_hop")};
    a->load = de_asset_load_many(a->assets, ids, 1, 0);
    if (de_asset_wait(a->assets, a->load, WAIT_NS) != DE_LOAD_READY) {
        char msg[512] = "?";
        for (u32 i = 0; i < 1; ++i)
            if (de_asset_last_error(a->assets, ids[i], msg, sizeof msg) == DE_OK)
                fprintf(stderr, "sky_hop: asset %u: %s\n", i, msg);
        fprintf(stderr, "sky_hop: the assets did not load\n");
        return 1;
    }
    return 0;
}

static int make_pipeline(app *a) {
    const char *asset = "shaders/sky_hop";
    de_shader sh = de_asset_shader(a->assets, de_asset_id_from(asset));
    de_gpu_pipeline_desc pd = {.kind = DE_PIPE_GRAPHICS,
                               .color_formats = {a->format},
                               .color_count = 1,
                               .cull = DE_CULL_NONE,
                               .name = asset};
    de_shader_reflection vs, ps;
    if (de_shader_get_code(a->assets, sh, a->backend, 0, "vs_main", &pd.vs, &vs) != DE_OK ||
        de_shader_get_code(a->assets, sh, a->backend, 0, "ps_main", &pd.ps, &ps) != DE_OK) {
        /* DE_ERR_NOT_FOUND: not cooked for this backend's target (DXIL without DXC). */
        bool missing = de_error_last() != NULL && de_error_last()->result == DE_ERR_NOT_FOUND;
        int rc = fail("getting the shader code for this backend");
        return missing ? EXIT_SKIP : rc;
    }
    pd.push_constant_bytes = vs.push_constant_bytes > ps.push_constant_bytes
                                 ? vs.push_constant_bytes
                                 : ps.push_constant_bytes;
    a->pipe = de_gpu_pipeline_create(a->d, &pd);
    return DE_HANDLE_VALID(a->pipe) ? 0 : fail("de_gpu_pipeline_create");
}

static void submit_and_signal(app *a, de_gpu_cmd *c) {
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = a->fence, .signal_value = ++a->frame};
    if (de_gpu_queue_submit(a->d, DE_QUEUE_GRAPHICS, &s) != DE_OK)
        (void)fail("submit");
}
static int make_rectangles(app *a) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        a->rectangles[i] = de_gpu_buffer_create(
            a->d, &(de_gpu_buffer_desc){.size = MAX_RECTS * sizeof(rect),
                                        .stride = sizeof(rect),
                                        .usage = DE_USAGE_STORAGE,
                                        .memory = DE_MEM_UPLOAD,
                                        .name = "Sky Hop retained rectangles"});
        a->maps[i] = de_gpu_buffer_map(a->d, a->rectangles[i]);
        if (!a->maps[i])
            return fail("rectangle buffer");
    }
    return 0;
}
static const u32 INK = 0x233f43, SKY = 0xdbe9df, CREAM = 0xf6edce, TEAL = 0x438d7b,
                 TEAL_LIGHT = 0x78b39a, GOLD = 0xe9b34d;
static void box(app *a, float x, float y, float w, float h, u32 color) {
    if (w <= 0 || h <= 0)
        return;
    if (a->count >= MAX_RECTS) {
        ++a->errors;
        return;
    }
    rect *r = &a->drawing[a->count++];
    *r = (rect){
        {x, y, w, h},
        {((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, 1}};
}
static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static const u8 glyphs[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {31, 4, 4, 4, 4, 4, 31},
    {7, 2, 2, 2, 18, 18, 12},     {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},   {14, 16, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};
static void text(app *a, const char *s, float x, float y, float size, u32 color) {
    for (; *s; ++s, x += size * 6) {
        const char *g = strchr(letters, *s);
        if (!g)
            continue;
        for (u32 row = 0; row < 7; ++row)
            for (u32 col = 0; col < 5; ++col)
                if (glyphs[g - letters][row] & (1u << (4 - col)))
                    box(a, x + col * size, y + row * size, size, size, color);
    }
}
static void centered(app *a, const char *s, float y, float size, u32 color) {
    text(a, s, (320 - (float)strlen(s) * size * 6 + size) * .5f, y, size, color);
}
static void bird(app *a, float y) {
    float wing = a->game.mode == PLAYING && a->game.velocity < 0 ? -2 : 3;
    box(a, 77, y - 8, 23, 16, INK);
    box(a, 81, y - 11, 15, 22, INK);
    box(a, 79, y - 6, 20, 12, GOLD);
    box(a, 83, y - 9, 11, 18, GOLD);
    box(a, 91, y - 7, 7, 7, CREAM);
    box(a, 95, y - 5, 3, 4, INK);
    box(a, 98, y + 1, 8, 5, INK);
    box(a, 99, y + 2, 6, 2, GOLD);
    box(a, 76, y + wing, 13, 7, INK);
    box(a, 78, y + wing + 1, 10, 4, 0xd4933a);
}
static void scene(app *a) {
    game *g = &a->game;
    box(a, 0, 0, 320, 400, SKY);
    for (u32 i = 0; i < 5; ++i) {
        float x = fmodf(72 * i - g->time * 4 + 400, 400) - 40;
        float y = 85 + (i % 3) * 56;
        box(a, x, y, 44, 6, CREAM);
        box(a, x + 8, y - 5, 24, 5, CREAM);
    }
    for (u32 i = 0; i < PIPE_COUNT; ++i) {
        pipe_pair *p = &g->pipes[i];
        float top = p->gap - GAP * .5f, bottom = p->gap + GAP * .5f;
        box(a, p->x, 48, PIPE_W, top - 48, INK);
        box(a, p->x + 3, 48, PIPE_W - 6, top - 48, TEAL);
        box(a, p->x + 7, 48, 5, top - 48, TEAL_LIGHT);
        box(a, p->x - 4, top - 12, PIPE_W + 8, 12, INK);
        box(a, p->x - 1, top - 9, PIPE_W + 2, 6, TEAL);
        box(a, p->x, bottom, PIPE_W, FLOOR - bottom, INK);
        box(a, p->x + 3, bottom, PIPE_W - 6, FLOOR - bottom, TEAL);
        box(a, p->x + 7, bottom + 3, 5, FLOOR - bottom - 3, TEAL_LIGHT);
        box(a, p->x - 4, bottom, PIPE_W + 8, 12, INK);
        box(a, p->x - 1, bottom + 3, PIPE_W + 2, 6, TEAL);
    }
    box(a, 0, FLOOR, 320, 50, CREAM);
    box(a, 0, FLOOR, 320, 3, INK);
    for (int i = 0; i < 24; ++i)
        box(a, i * 16 - fmodf(g->time * 22, 16), FLOOR + 7, 8, 3, TEAL_LIGHT);
    box(a, 0, 0, 320, 48, CREAM);
    box(a, 0, 46, 320, 2, INK);
    text(a, "SKY HOP", 14, 16, 2, INK);
    char score[32];
    snprintf(score, sizeof score, "%03u", g->score);
    text(a, score, 252, 12, 3, INK);
    bird(a, g->y + (g->mode == READY ? sinf(g->time * 3) * 4 : 0));
    if (g->mode == READY) {
        centered(a, "FIND YOUR", 90, 2, INK);
        centered(a, "RHYTHM", 111, 4, INK);
        centered(a, "SPACE OR CLICK TO FLAP", 244, 1.5f, INK);
        centered(a, "SLIP THROUGH THE GAPS", 265, 1, INK);
    }
    if (g->mode == DEAD || g->mode == PAUSED) {
        box(a, 24, 126, 272, 130, CREAM);
        box(a, 24, 126, 272, 3, INK);
        box(a, 24, 253, 272, 3, INK);
        centered(a, g->mode == DEAD ? "NICE FLIGHT" : "PAUSED", 146, 3, INK);
        if (g->mode == DEAD) {
            snprintf(score, sizeof score, "SCORE %u   BEST %u", g->score, g->best);
            centered(a, score, 183, 1.5f, INK);
            centered(a, "SPACE OR CLICK TO RESTART", 222, 1.25f, INK);
        } else {
            centered(a, "P TO RESUME", 205, 2, INK);
        }
    }
    centered(a, "P PAUSE    R RESTART    ESC QUIT", 373, 1, INK);
}
static void record_scene(app *a, de_gpu_cmd *c, de_gpu_texture target) {
    u32 slot = (u32)(a->frame % FRAMES_IN_FLIGHT);
    a->drawing = a->maps[slot];
    a->count = 0;
    scene(a);
    de_gpu_barrier b = {
        .buffer = a->rectangles[slot], .before = DE_ACCESS_NONE, .after = DE_ACCESS_SHADER_READ};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_attachment color = {
        .texture = target, .clear = true, .clear_color = {.017f, .05f, .055f, 1}, .store = true};
    de_gpu_cmd_begin_render(c, &color, 1, NULL);
    de_gpu_cmd_set_pipeline(c, a->pipe);
    struct {
        u32 rectangles;
        f32 width, height;
        u32 pad;
    } pc = {de_gpu_buffer_bindless(a->d, a->rectangles[slot]), (f32)a->width, (f32)a->height, 0};
    de_gpu_cmd_push_constants(c, &pc, sizeof pc);
    de_gpu_cmd_draw(c, 6, a->count, 0, 0);
    de_gpu_cmd_end_render(c);
}
static u32 screenshot(app *a, u8 *rows, u32 pitch) {
    bool bgra = a->format == DE_FMT_BGRA8_SRGB || a->format == DE_FMT_BGRA8_UNORM;
    for (u32 y = 0; bgra && y < a->height; ++y)
        for (u32 x = 0; x < a->width; ++x) {
            u8 *p = rows + y * pitch + x * 4, t = p[0];
            p[0] = p[2];
            p[2] = t;
        }
    if (!stbi_write_png(a->o.screenshot, (int)a->width, (int)a->height, 4, rows, (int)pitch))
        return 1;
    if (a->o.check) {
        u32 gold = 0, ink = 0;
        for (u32 y = 0; y < a->height; ++y)
            for (u32 x = 0; x < a->width; ++x) {
                u8 *p = rows + y * pitch + x * 4;
                gold += p[0] > 200 && p[1] > 120 && p[1] < 210 && p[2] < 120;
                ink += p[0] < 65 && p[1] < 95 && p[2] < 100;
            }
        if (gold < 50 || ink < 500) {
            fprintf(stderr, "Sky Hop screenshot check failed: gold%u ink%u\n", gold, ink);
            return 1;
        }
        printf("Sky Hop actual render: %u gold pixels, %u ink pixels\n", gold, ink);
    }
    return 0;
}

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

static int run(app *a) {
    int rc = load_assets(a);
    if (rc != 0)
        return rc;
    a->backend = a->o.backend == DE_GPU_DEFAULT ? de_gpu_platform_default_backend() : a->o.backend;
    if (!a->o.offscreen) {
        /* The window is set up for the backend's surface kind before the device exists. */
        de_result r = de_platform_init(&(de_platform_desc){.app_name = "Sky Hop"});
        if (r != DE_OK)
            return fail("de_platform_init (no display? try --offscreen)");
        a->platform = true;
        de_window_desc wd = {.title = "Sky Hop",
                             .width = 640,
                             .height = 800,
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
    printf("Sky Hop: %s on %s (%s)\n", "native arcade", de_gpu_backend_name(caps->backend),
           caps->device_name);
    /* Like the renderer, refuse a device below the Base tier (SPEC §3.3): bindless needs it. */
    de_gpu_tier_report tier;
    de_gpu_evaluate_tier(caps, &tier);
    if (!tier.base_ok) {
        fprintf(stderr, "Sky Hop: %s is below the Base tier: %s\n", caps->device_name,
                tier.base_reason_count > 0 ? tier.base_reasons[0] : "?");
        return EXIT_SKIP;
    }
    a->fence = de_gpu_fence_create(a->d, 0);
    if (a->o.offscreen) {
        a->format = DE_FMT_RGBA8_SRGB;
        a->width = 640;
        a->height = 800;
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
    if ((rc = make_pipeline(a)) != 0 || (rc = make_rectangles(a)) != 0)
        return rc;

    if (de_input_create(&(de_input_desc){.window = a->window}, &a->input) != DE_OK)
        return fail("input");
    game_reset(&a->game);
    u64 previous = de_time_now_ns();
    double accumulator = 0;
    bool quit = false;
    for (u32 n = 0; !quit && (a->o.frames == 0 || n < a->o.frames); ++n) {
        de_app_event ev[16];
        u32 count = a->platform ? de_platform_pump(ev, 16) : 0u;
        for (u32 i = 0; i < count; ++i) {
            de_input_app_event(a->input, &ev[i]);
            if (ev[i].kind == DE_EV_QUIT || ev[i].kind == DE_EV_WINDOW_CLOSE)
                quit = true;
            if (ev[i].kind == DE_EV_WINDOW_RESIZED && ev[i].a > 0 && ev[i].b > 0) {
                a->width = ev[i].a;
                a->height = ev[i].b;
                if (de_gpu_swapchain_resize(a->d, a->sc, a->width, a->height) != DE_OK)
                    return fail("de_gpu_swapchain_resize");
            }
        }
        de_input_begin_frame(a->input);
        bool flap =
            de_key_pressed(a->input, DE_KEY_SPACE) || de_mouse_pressed(a->input, DE_MOUSE_LEFT);
        if (de_key_pressed(a->input, DE_KEY_ESCAPE))
            quit = true;
        if (de_key_pressed(a->input, DE_KEY_R)) {
            game_reset(&a->game);
            game_flap(&a->game);
        }
        if (de_key_pressed(a->input, DE_KEY_P))
            game_pause(&a->game);
        if (flap)
            game_flap(&a->game);
        u64 now = de_time_now_ns();
        double elapsed = (double)(now - previous) * 1e-9;
        previous = now;
        if (a->o.smoke) {
            elapsed = 1.0 / 60;
            if (a->game.mode == READY)
                game_flap(&a->game);
            float target = 200;
            for (u32 i = 0; i < PIPE_COUNT; ++i)
                if (a->game.pipes[i].x + PIPE_W > BIRD_X) {
                    target = a->game.pipes[i].gap;
                    break;
                }
            if (a->game.mode == PLAYING && a->game.y > target + 8 && a->game.velocity > -40)
                game_flap(&a->game);
        }
        if (elapsed > .1)
            elapsed = .1;
        accumulator += elapsed;
        while (accumulator >= STEP) {
            game_tick(&a->game, STEP);
            accumulator -= STEP;
        }
        if ((rc = frame(a, a->o.frames != 0 && n + 1u == a->o.frames)) != 0)
            return rc;
        if (a->platform)
            de_sleep_until_ns(now + 16666667ull);
    }
    return 0;
}

static void shutdown(app *a) {
    de_input_destroy(a->input);
    if (a->d != NULL) {
        de_gpu_wait_idle(a->d);
        de_gpu_pipeline_destroy(a->d, a->pipe);
        for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
            de_gpu_buffer_destroy(a->d, a->rectangles[i]);
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
    if (a->tasks != NULL)
        de_task_system_destroy(a->tasks);
    de_fs_shutdown();
}

int main(int argc, char **argv) {
    static app a; /* zeroed */
    int rc = parse(argc, argv, &a.o);
    if (rc != 0)
        return rc;
    if (a.o.self_check)
        return game_self_check();
    (void)de_log_init(NULL);
    de_log_add_sink(count_errors, &a);
    rc = run(&a);
    shutdown(&a);
    de_log_flush(); /* every error logged so far has reached count_errors */
    if (rc == 0 && a.errors != 0) {
        fprintf(stderr, "sky_hop: %u error(s) were logged\n", a.errors);
        rc = 1;
    }
    de_log_shutdown();
    if (rc == 0)
        printf("sky_hop: done\n");
    return rc;
}
