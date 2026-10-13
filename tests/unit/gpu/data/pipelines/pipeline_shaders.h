/* pipeline_shaders.h — the shared S10 pipeline tests: shaders, inputs and expected values
 * (DENG-323). The Vulkan tests (tests/unit/gpu/vulkan/test_vk_pipeline.c) use them, and the
 * D3D12 (DENG-324) and Metal (DENG-325) pipeline tests use the same shaders and values, so the
 * three backends are checked against one another.
 *
 * The .slang files next to this header are cooked at test time through the S09 shader importer
 * (de_shader_import, through the cooker) for the target a backend consumes; de_pipe_test_shader
 * hands out one entry point's code and reflection. Builds without the importer (no DE_BUILD_TOOLS,
 * no Slang for the host) report that, and the tests skip.
 *
 * Every render test draws into a DE_PIPE_TEST_SIZE square RGBA8_UNORM target cleared to
 * DE_PIPE_TEST_CLEAR, and reads pixels back (row 0 at the top).
 */
#ifndef DE_TEST_PIPELINE_SHADERS_H
#define DE_TEST_PIPELINE_SHADERS_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

#include "gpu/shader/shader_format.h"

DE_EXTERN_C_BEGIN

/* ---------- Cooking ---------- */

/** One cooked entry point: its code (de_gpu_shader_blob) and the importer's reflection. */
typedef struct de_pipe_test_shader {
    de_gpu_shader_blob blob;
    u32 stage;               /**< de__shader_stage */
    u32 threadgroup[3];      /**< compute, mesh, amplification */
    u32 push_constant_bytes; /**< on this target */
} de_pipe_test_shader;

/**
 * Cooks every shared test shader for `target` (once per process and target). False, with the
 * reason in `why`, when this build has no shader importer or the host cannot cook the target.
 * Not thread-safe; call it from the test thread.
 */
bool de_pipe_test_cook(de__shader_target target, char *why, size_t why_cap);
/** Entry point `entry` of `file` ("triangle.slang") cooked for `target` (after de_pipe_test_cook).
 * False when it was not cooked. The blob stays valid for the whole process. */
bool de_pipe_test_get(de__shader_target target, const char *file, const char *entry,
                      de_pipe_test_shader *out);

/* ---------- Shared inputs and expected values ---------- */

/** Render targets are DE_PIPE_TEST_SIZE x DE_PIPE_TEST_SIZE RGBA8_UNORM (+ D32_FLOAT depth). */
#define DE_PIPE_TEST_SIZE 64u
/** Clear color (0.2, 0.4, 0.6, 1.0): bytes (51, 102, 153, 255). */
#define DE_PIPE_TEST_CLEAR_F  0.2f, 0.4f, 0.6f, 1.0f
#define DE_PIPE_TEST_CLEAR_U8 0x33, 0x66, 0x99, 0xFF

/** One expected pixel: (x, y) from the top-left, RGBA8 bytes. */
typedef struct de_pipe_test_pixel {
    u32 x, y;
    u8 rgba[4];
} de_pipe_test_pixel;

/**
 * triangle.slang (`vs_main`, `ps_main`; 24 bytes of push constants: float4 color, uint
 * positions, uint first) on a GRAPHICS pipeline with DE_CULL_BACK, depth test + write, D32_FLOAT
 * depth cleared to 0 (reverse-Z far). Three draws of 3 vertices from de_pipe_test_triangle_pos:
 *   A  first 0, green (0, 1, 0, 1), z 0.5,  counter-clockwise: the lower-left half;
 *   B  first 3, red   (1, 0, 0, 1), z 0.25, counter-clockwise, same place: behind A (reverse-Z);
 *   C  first 6, blue  (0, 0, 1, 1), z 0.5,  clockwise, the upper-right half: culled.
 * de_pipe_test_triangle_expect: 3 pixels (green, green, clear).
 */
extern const f32 de_pipe_test_triangle_pos[9][4];
extern const f32 de_pipe_test_triangle_color[3][4];
extern const de_pipe_test_pixel de_pipe_test_triangle_expect[3];

/**
 * indirect_args.slang (`cs_main`, 16 bytes: args, count_buf, draws, count) writes
 * DE_PIPE_TEST_INDIRECT_RECORDS draw records (draw i: 3 indices from 3*i) and the count
 * DE_PIPE_TEST_INDIRECT_COUNT. Then the triangle pipeline (green, first 0) draws with
 * de_gpu_cmd_draw_indexed_indirect(max_draws = 3) through u16 indices 0..8 into
 * de_pipe_test_indirect_pos: three counter-clockwise triangles, one per vertical third. Only the
 * first two thirds are green (de_pipe_test_indirect_expect).
 */
#define DE_PIPE_TEST_INDIRECT_RECORDS 3u
#define DE_PIPE_TEST_INDIRECT_COUNT   2u
extern const f32 de_pipe_test_indirect_pos[9][4];
extern const u16 de_pipe_test_indirect_indices[9];
extern const de_pipe_test_pixel de_pipe_test_indirect_expect[3];

/**
 * compute.slang (`cs_main`, 64x1x1, 16 bytes: src, dst, count, seed): dst[i] = src[i] * 3 + seed
 * for i < count, with src[i] = i * 7 + 1. Dispatch ceil(count / 64) groups.
 */
#define DE_PIPE_TEST_COMPUTE_COUNT  1000u
#define DE_PIPE_TEST_COMPUTE_SEED   0x1000u
#define DE_PIPE_TEST_COMPUTE_SRC(i) ((u32)(i) * 7u + 1u)
#define DE_PIPE_TEST_COMPUTE_DST(i) (DE_PIPE_TEST_COMPUTE_SRC(i) * 3u + DE_PIPE_TEST_COMPUTE_SEED)

/**
 * push_limit.slang (`cs_main`, 32x1x1, 128 bytes): one group copies the 32 pushed words into the
 * buffer whose bindless index is word 31; word i (i < 31) is DE_PIPE_TEST_PUSH_WORD(i). The
 * readback equals the pushed words.
 */
#define DE_PIPE_TEST_PUSH_WORD(i) (0xC0DE0000u + (u32)(i) * 0x111u)

/** mesh.slang (`as_main`, `ms_main`, `ps_main`; 24 bytes like triangle.slang): one triangle from
 * de_pipe_test_triangle_pos (first 0, green), so the expected pixels are the triangle test's
 * first two plus the clear color where C would be. */

DE_EXTERN_C_END

#endif
