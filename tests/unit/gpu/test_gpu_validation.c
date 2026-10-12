/* test_gpu_validation.c — de_gpu argument checks and their messages (DENG-281). Create descs and
 * submits are checked in every build; command recording only in Debug and Dev. */
#include "de_test.h"

#include <dengine/de_gpu.h>

#include "gpu/gpu_backend.h"

#include <string.h>

typedef struct val_fx {
    de_gpu_device *d;
} val_fx;

static void val_fx_setup(val_fx *f) {
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &f->d), DE_OK);
}
static void val_fx_teardown(val_fx *f) {
    de_gpu_device_destroy(f->d);
}

/* Expects the calling thread's error to be `r` with every needle in its message. */
#define EXPECT_ERROR(r, ...)                                                                       \
    do {                                                                                           \
        const de_error *e_ = de_error_last();                                                      \
        ASSERT_TRUE(e_ != NULL);                                                                   \
        EXPECT_EQ(e_->result, (r));                                                                \
        const char *needles_[] = {__VA_ARGS__};                                                    \
        for (size_t i_ = 0; i_ < sizeof needles_ / sizeof needles_[0]; ++i_)                       \
            EXPECT_TRUE(strstr(e_->message, needles_[i_]) != NULL);                                \
    } while (0)

static const char k_code[4] = {1, 2, 3, 4};
static const de_gpu_shader_blob k_blob = {k_code, sizeof k_code, "main"};

/* ---------- The three acceptance-criteria messages ---------- */

TEST_F(gpu_validation, messages, val_fx) {
    /* push_constant_bytes = 129 */
    de_error_clear();
    de_gpu_pipeline p = de_gpu_pipeline_create(
        f->d,
        &(de_gpu_pipeline_desc){
            .kind = DE_PIPE_COMPUTE, .cs = k_blob, .push_constant_bytes = 129, .name = "blur"});
    EXPECT_FALSE(DE_HANDLE_VALID(p));
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "de_gpu_pipeline_create(\"blur\")",
                 "push_constant_bytes is 129", "the limit is 128 bytes");
    /* 128 is fine. */
    p = de_gpu_pipeline_create(
        f->d,
        &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE, .cs = k_blob, .push_constant_bytes = 128});
    EXPECT_TRUE(DE_HANDLE_VALID(p));

    /* DE_USAGE_DEPTH on a color format */
    de_error_clear();
    de_gpu_texture t =
        de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                                           .width = 16,
                                                           .height = 16,
                                                           .usage = DE_USAGE_DEPTH,
                                                           .name = "zbuf"});
    EXPECT_FALSE(DE_HANDLE_VALID(t));
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "de_gpu_texture_create(\"zbuf\")",
                 "usage has DE_USAGE_DEPTH but DE_FMT_RGBA8_UNORM is a color format");

    /* Mapping a GPU_ONLY buffer */
    de_gpu_buffer b = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE, .name = "particles"});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    de_error_clear();
    EXPECT_TRUE(de_gpu_buffer_map(f->d, b) == NULL);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "de_gpu_buffer_map(\"particles\")",
                 "the buffer is DE_MEM_GPU_ONLY",
                 "only DE_MEM_UPLOAD and DE_MEM_READBACK buffers can be mapped");
    /* Nothing leaked into the pools from the refused creates. */
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_TEXTURE), 0u);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_PIPELINE), 1u);
}

/* ---------- Descs ---------- */

TEST_F(gpu_validation, buffer_descs, val_fx) {
    struct {
        de_gpu_buffer_desc desc;
        const char *needle;
    } cases[] = {
        {{.size = 0, .usage = DE_USAGE_STORAGE}, "size is 0"},
        {{.size = 16}, "usage is 0"},
        {{.size = 16, .usage = 1u << 20}, "unknown bits"},
        {{.size = 16, .usage = DE_USAGE_SAMPLED}, "texture-only flags"},
        {{.size = 16, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_UPLOAD}, "CPU only"},
        {{.size = 16, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_READBACK},
         "take only DE_USAGE_COPY_DST"},
        {{.size = 16, .usage = DE_USAGE_STORAGE, .memory = (de_gpu_memory)7},
         "not a de_gpu_memory value"},
        {{.size = 30, .usage = DE_USAGE_STORAGE, .stride = 12}, "stride 12"},
        {{.size = 30, .usage = DE_USAGE_STORAGE, .stride = 6}, "multiple of 4"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        de_error_clear();
        EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_buffer_create(f->d, &cases[i].desc)));
        EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, cases[i].needle);
    }
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_buffer_create(f->d, NULL)));
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "desc is NULL");
    /* Valid ones, including a readback buffer and a structured buffer. */
    EXPECT_TRUE(DE_HANDLE_VALID(de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 64, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK})));
    EXPECT_TRUE(DE_HANDLE_VALID(de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 48, .usage = DE_USAGE_STORAGE, .stride = 12})));
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_BUFFER), 2u);
}

TEST_F(gpu_validation, texture_descs, val_fx) {
    const u32 S = DE_USAGE_SAMPLED;
    struct {
        de_gpu_texture_desc desc;
        const char *needle;
    } cases[] = {
        {{.width = 4, .usage = S}, "is not a texture format"},
        {{.format = DE_FMT_RGBA8_UNORM, .usage = S}, "width is 0"},
        {{.format = DE_FMT_RGBA8_UNORM, .width = 4}, "usage is 0"},
        {{.format = DE_FMT_RGBA8_UNORM, .width = 4, .usage = DE_USAGE_VERTEX}, "buffer-only"},
        {{.format = DE_FMT_D32_FLOAT, .width = 4, .usage = S}, "needs DE_USAGE_DEPTH"},
        {{.format = DE_FMT_D32_FLOAT, .width = 4, .usage = DE_USAGE_DEPTH | DE_USAGE_STORAGE},
         "cannot be combined"},
        {{.format = DE_FMT_BC7_UNORM, .width = 4, .height = 4, .usage = S | DE_USAGE_STORAGE},
         "block-compressed"},
        {{.format = DE_FMT_BC1_SRGB, .width = 6, .height = 4, .usage = S}, "multiples of 4"},
        {{.format = DE_FMT_RGBA8_SRGB, .width = 4, .usage = DE_USAGE_STORAGE}, "sRGB"},
        {{.format = DE_FMT_RGBA8_UNORM, .width = 32768, .usage = S}, "larger than the 16384"},
        {{.format = DE_FMT_RGBA8_UNORM, .width = 4, .depth_or_layers = 2, .usage = S},
         "use DE_TEX_2D_ARRAY"},
        {{.dim = DE_TEX_CUBE, .format = DE_FMT_RGBA8_UNORM, .width = 8, .height = 4, .usage = S},
         "a cube needs"},
        {{.format = DE_FMT_RGBA8_UNORM, .width = 16, .height = 16, .mip_count = 6, .usage = S},
         "mip_count 6 is more than the 5 levels"},
        {{.format = DE_FMT_RGBA8_UNORM, .width = 16, .sample_count = 3, .usage = S},
         "sample_count 3"},
        {{.format = DE_FMT_RGBA8_UNORM, .width = 16, .sample_count = 4, .mip_count = 2, .usage = S},
         "multisampled textures"},
        {{.dim = DE_TEX_3D, .format = DE_FMT_D32_FLOAT, .width = 4, .usage = DE_USAGE_DEPTH},
         "cannot be a depth target"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        de_error_clear();
        EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_texture_create(f->d, &cases[i].desc)));
        EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, cases[i].needle);
    }
    /* Valid: full chain, 3D, MSAA target, array. */
    EXPECT_TRUE(DE_HANDLE_VALID(de_gpu_texture_create(
        f->d,
        &(de_gpu_texture_desc){
            .format = DE_FMT_RGBA8_UNORM, .width = 16, .height = 16, .mip_count = 5, .usage = S})));
    EXPECT_TRUE(DE_HANDLE_VALID(
        de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.dim = DE_TEX_3D,
                                                           .format = DE_FMT_R16_FLOAT,
                                                           .width = 32,
                                                           .height = 32,
                                                           .depth_or_layers = 64,
                                                           .mip_count = 7,
                                                           .usage = S | DE_USAGE_STORAGE})));
    EXPECT_TRUE(DE_HANDLE_VALID(
        de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                                           .width = 64,
                                                           .height = 64,
                                                           .sample_count = 4,
                                                           .usage = DE_USAGE_RENDER_TARGET})));
    EXPECT_TRUE(DE_HANDLE_VALID(
        de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.dim = DE_TEX_2D_ARRAY,
                                                           .format = DE_FMT_D32_FLOAT,
                                                           .width = 1024,
                                                           .height = 1024,
                                                           .depth_or_layers = 4,
                                                           .usage = DE_USAGE_DEPTH | S})));
}

TEST_F(gpu_validation, sampler_pipeline_query_descs, val_fx) {
    de_error_clear();
    EXPECT_FALSE(
        DE_HANDLE_VALID(de_gpu_sampler_create(f->d, &(de_gpu_sampler_desc){.max_aniso = 32.0f})));
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "max_aniso 32 is outside 0..16");
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_gpu_sampler_create(f->d, &(de_gpu_sampler_desc){.u = (de_gpu_address)9})));
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "de_gpu_address");

    struct {
        de_gpu_pipeline_desc desc;
        de_result r;
        const char *needle;
    } cases[] = {
        {{.kind = DE_PIPE_COMPUTE}, DE_ERR_INVALID_ARGUMENT, "needs a cs blob"},
        {{.kind = DE_PIPE_GRAPHICS}, DE_ERR_INVALID_ARGUMENT, "needs a vs blob"},
        {{.kind = DE_PIPE_MESH}, DE_ERR_INVALID_ARGUMENT, "needs an ms blob"},
        {{.kind = DE_PIPE_RAYTRACING, .raygen = k_blob}, DE_ERR_UNSUPPORTED, "S16"},
        {{.kind = DE_PIPE_COMPUTE, .cs = k_blob, .push_constant_bytes = 6},
         DE_ERR_INVALID_ARGUMENT,
         "not a multiple of 4"},
        {{.kind = DE_PIPE_GRAPHICS, .vs = k_blob, .color_count = 9},
         DE_ERR_INVALID_ARGUMENT,
         "color_count 9"},
        {{.kind = DE_PIPE_GRAPHICS,
          .vs = k_blob,
          .color_count = 1,
          .color_formats = {DE_FMT_D32_FLOAT}},
         DE_ERR_INVALID_ARGUMENT,
         "color_formats[0] is DE_FMT_D32_FLOAT"},
        {{.kind = DE_PIPE_GRAPHICS, .vs = k_blob, .depth_format = DE_FMT_RGBA8_UNORM},
         DE_ERR_INVALID_ARGUMENT,
         "not a depth format"},
        {{.kind = DE_PIPE_GRAPHICS, .vs = k_blob, .depth_test = true},
         DE_ERR_INVALID_ARGUMENT,
         "need a depth_format"},
        {{.kind = (de_gpu_pipeline_kind)11}, DE_ERR_INVALID_ARGUMENT, "kind 11"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        de_error_clear();
        EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_pipeline_create(f->d, &cases[i].desc)));
        EXPECT_ERROR(cases[i].r, cases[i].needle);
    }
    EXPECT_TRUE(DE_HANDLE_VALID(
        de_gpu_pipeline_create(f->d, &(de_gpu_pipeline_desc){.kind = DE_PIPE_MESH,
                                                             .ms = k_blob,
                                                             .ps = k_blob,
                                                             .color_count = 1,
                                                             .color_formats = {DE_FMT_RGBA8_SRGB},
                                                             .depth_format = DE_FMT_D32_FLOAT,
                                                             .depth_test = true})));

    EXPECT_FALSE(
        DE_HANDLE_VALID(de_gpu_query_heap_create(f->d, &(de_gpu_query_heap_desc){.name = "q"})));
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "count 0 is outside 1..65536");
}

TEST_F(gpu_validation, handles, val_fx) {
    de_gpu_buffer b = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 16, .usage = DE_USAGE_CONSTANT, .name = "cb"});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    de_gpu_buffer_destroy(f->d, b);
    de_error_clear();
    EXPECT_EQ(de_gpu_buffer_bindless(f->d, b), DE_GPU_BINDLESS_INVALID);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "de_gpu_buffer_bindless", "buffer handle 0x", "is stale",
                 "generation 1");
    /* A double destroy is reported, not acted on. */
    de_error_clear();
    de_gpu_buffer_destroy(f->d, b);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "de_gpu_buffer_destroy", "is stale");
    /* The zero handle: destroy ignores it, lookups report it. */
    de_error_clear();
    de_gpu_texture_destroy(f->d, (de_gpu_texture){0});
    EXPECT_TRUE(de_error_last() == NULL);
    EXPECT_EQ(de_gpu_texture_bindless(f->d, (de_gpu_texture){0}, 0, false),
              DE_GPU_BINDLESS_INVALID);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "the texture handle is 0");
    /* A handle of one kind is not a handle of another. */
    de_gpu_sampler s = de_gpu_sampler_create(f->d, &(de_gpu_sampler_desc){0});
    EXPECT_EQ(de_gpu_buffer_bindless(f->d, (de_gpu_buffer){de__gpu_handle_make(9, 1)}),
              DE_GPU_BINDLESS_INVALID);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "is not a buffer handle of this device");
    (void)s;
    /* A buffer without shader-visible usage has no bindless index. */
    de_gpu_buffer staging = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){
            .size = 16, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD, .name = "staging"});
    EXPECT_EQ(de_gpu_buffer_bindless(f->d, staging), DE_GPU_BINDLESS_INVALID);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "\"staging\"", "no bindless index");
    EXPECT_TRUE(de_gpu_buffer_map(f->d, staging) != NULL);
}

/* ---------- Submits (every build) ---------- */

TEST_F(gpu_validation, submits, val_fx) {
    EXPECT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "submit is NULL");
    EXPECT_EQ(de_gpu_queue_submit(f->d, (de_gpu_queue)5, &(de_gpu_submit){0}),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "queue 5");
    /* An empty submit is valid (it still advances the queue timeline). */
    EXPECT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_COPY, &(de_gpu_submit){0}), DE_OK);

    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "pass");
    ASSERT_TRUE(c != NULL);
    de_gpu_cmd *twice[2] = {c, c};
    EXPECT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = twice, .cmd_count = 2}),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "are the same command list");
    de_gpu_fence stale = de_gpu_fence_create(f->d, 0);
    de_gpu_fence_destroy(f->d, stale);
    EXPECT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &c, .cmd_count = 1, .signal = stale}),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "fence handle", "is stale");

    /* An open render pass at submit. */
    de_gpu_texture rt =
        de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                                           .width = 8,
                                                           .height = 8,
                                                           .usage = DE_USAGE_RENDER_TARGET});
    de_gpu_attachment color = {.texture = rt, .clear = true, .store = true};
    de_gpu_cmd_begin_render(c, &color, 1, NULL);
    EXPECT_EQ(
        de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_ERR_INVALID_ARGUMENT);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "open render pass");
    de_gpu_cmd_end_render(c);
    EXPECT_EQ(
        de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
}

/* ---------- Command recording (Debug and Dev) ---------- */

TEST_F(gpu_validation, commands, val_fx) {
#if !DE__GPU_VALIDATE_COMMANDS
    SKIP("command validation is compiled in Debug and Dev builds only");
#else
    de_gpu_pipeline cs = de_gpu_pipeline_create(
        f->d, &(de_gpu_pipeline_desc){
                  .kind = DE_PIPE_COMPUTE, .cs = k_blob, .push_constant_bytes = 16, .name = "cs"});
    de_gpu_buffer args = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 12, .usage = DE_USAGE_INDIRECT, .name = "args"});
    de_gpu_buffer src = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = 32768,
                                                                         .usage = DE_USAGE_COPY_SRC,
                                                                         .memory = DE_MEM_UPLOAD,
                                                                         .name = "src"});
    de_gpu_buffer dst = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 1024, .usage = DE_USAGE_COPY_DST, .name = "dst"});
    de_gpu_texture tex = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                     .width = 64,
                                     .height = 64,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                     .name = "tex"});
    ASSERT_TRUE(DE_HANDLE_VALID(cs) && DE_HANDLE_VALID(args) && DE_HANDLE_VALID(src) &&
                DE_HANDLE_VALID(dst) && DE_HANDLE_VALID(tex));

    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "frame");
    ASSERT_TRUE(c != NULL);
    u8 data[64] = {0};

    de_error_clear();
    de_gpu_cmd_push_constants(c, data, 16);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "de_gpu_cmd_push_constants(\"frame\")",
                 "no pipeline is bound");
    de_gpu_cmd_dispatch(c, 1, 1, 1);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "no compute pipeline is bound");

    de_gpu_cmd_set_pipeline(c, cs);
    de_error_clear();
    de_gpu_cmd_push_constants(c, data, 16);
    de_gpu_cmd_dispatch(c, 8, 8, 1);
    de_gpu_cmd_dispatch_indirect(c, args, 0);
    EXPECT_TRUE(de_error_last() == NULL);
    de_gpu_cmd_push_constants(c, data, 20);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "20 bytes", "push_constant_bytes = 16");
    de_gpu_cmd_dispatch_indirect(c, args, 4);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "args range [4, +12)", "\"args\" (12 bytes)");
    de_gpu_cmd_dispatch_indirect(c, src, 0);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "\"src\" lacks DE_USAGE_INDIRECT");

    /* Render passes. */
    de_gpu_attachment color = {.texture = tex};
    de_gpu_cmd_begin_render(c, &color, 1, NULL);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "(\"tex\") lacks DE_USAGE_RENDER_TARGET");
    de_gpu_cmd_end_render(c);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "no render pass is open");
    de_gpu_cmd_viewport(c, 0, 0, 64, 64);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "no render pass is open");

    /* Copies. */
    de_error_clear();
    de_gpu_cmd_copy_buffer(c, dst, 0, src, 0, 1024);
    EXPECT_TRUE(de_error_last() == NULL);
    de_gpu_cmd_copy_buffer(c, dst, 512, src, 0, 1024);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "run past");
    de_gpu_cmd_copy_buffer(c, src, 0, dst, 0, 16);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "dst \"src\" lacks DE_USAGE_COPY_DST");
    de_error_clear();
    de_gpu_cmd_copy_buffer_to_texture(c, tex, 0, 0, src, 0, 256);
    EXPECT_TRUE(de_error_last() == NULL);
    de_gpu_cmd_copy_buffer_to_texture(c, tex, 0, 0, src, 0, 200);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "row_pitch 200 must be a multiple of 256");
    de_gpu_cmd_copy_buffer_to_texture(c, tex, 0, 0, src, 16896, 256); /* 64 rows x 256 */
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "needs 16384 bytes at 16896");
    de_gpu_cmd_copy_buffer_to_texture(c, tex, 0, 0, src, 100, 256);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "src_off 100 is not a multiple of 512");
    de_gpu_cmd_copy_buffer_to_texture(c, tex, 1, 0, src, 0, 256);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "mip 1, but \"tex\" has 1");

    /* Barriers. */
    de_gpu_barrier bar = {.buffer = dst, .texture = tex};
    de_gpu_cmd_barriers(c, &bar, 1);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "sets 2 of texture/buffer/accel");

    /* Markers must balance before submit. */
    de_gpu_cmd_marker_pop(c);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "no marker_push to pop");
    de_gpu_cmd_marker_push(c, "scope", 0xff00ffu);
    EXPECT_EQ(
        de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_ERR_INVALID_ARGUMENT);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "open marker_push");
    de_gpu_cmd_marker_pop(c);
    ASSERT_EQ(
        de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    /* Recording into a submitted list. */
    de_gpu_cmd_dispatch(c, 1, 1, 1);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "is not recording");

    /* Pipelines on the wrong queue. */
    de_gpu_cmd *copy = de_gpu_cmd_begin(f->d, DE_QUEUE_COPY, "upload");
    de_gpu_cmd_set_pipeline(copy, cs);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "copy queue");
    de_gpu_cmd_begin_render(copy, &color, 1, NULL);
    EXPECT_ERROR(DE_ERR_INVALID_ARGUMENT, "need the graphics queue");
    EXPECT_EQ(
        de_gpu_queue_submit(f->d, DE_QUEUE_COPY, &(de_gpu_submit){.cmds = &copy, .cmd_count = 1}),
        DE_OK);
#endif
}
