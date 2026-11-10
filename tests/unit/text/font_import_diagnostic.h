#ifndef DE_TEST_FONT_IMPORT_DIAGNOSTIC_H
#define DE_TEST_FONT_IMPORT_DIAGNOSTIC_H
#include "core/de_compiler.h"
#include "text/import/import_internal.h"
#include "text/msdf/msdf_wrap.h"

/* Only the explicit determinism case uses this source-included importer. Its compilation
 * context and logging overhead differ: these markers localize work, not qualify timing. */
static DE_THREAD_LOCAL struct {
    u32 cook, calls;
    u64 msdf_ns;
} font_diag;

static de_result font_diag_pack(de_asset_import_ctx *ctx, de__font_build *build) {
    fprintf(stderr, "[font-import] cook=%u phase=pack begin glyphs=%u cmap=%u\n", font_diag.cook,
            build->count, build->cmap_count);
    fflush(stderr);
    u64 start = de_time_now_ns();
    de_result result = de__font_pack(ctx, build);
    fprintf(stderr, "[font-import] cook=%u phase=pack end result=%d elapsed_ns=%llu pages=%u\n",
            font_diag.cook, (int)result, (unsigned long long)(de_time_now_ns() - start),
            build->pages);
    fflush(stderr);
    return result;
}
static de_result font_diag_msdf(const de__msdf_vertex *vertices, u32 count, double scale, double ox,
                                double oy, double range, u32 width, u32 height, float *scratch,
                                u8 *rgba) {
    u32 ordinal = ++font_diag.calls;
    bool report = ordinal == 1 || ordinal % 64 == 0;
    if (report) {
        fprintf(stderr, "[font-import] cook=%u phase=msdf begin call=%u vertices=%u size=%ux%u\n",
                font_diag.cook, ordinal, count, width, height);
        fflush(stderr);
    }
    u64 start = de_time_now_ns();
    de_result result =
        de__msdf_generate(vertices, count, scale, ox, oy, range, width, height, scratch, rgba);
    u64 elapsed = de_time_now_ns() - start;
    font_diag.msdf_ns += elapsed;
    if (report) {
        fprintf(stderr,
                "[font-import] cook=%u phase=msdf end call=%u result=%d elapsed_ns=%llu "
                "total_ns=%llu\n",
                font_diag.cook, ordinal, (int)result, (unsigned long long)elapsed,
                (unsigned long long)font_diag.msdf_ns);
        fflush(stderr);
    }
    return result;
}
static de_result font_diag_write(de_asset_import_ctx *ctx, const de__font_build *build) {
    fprintf(stderr, "[font-import] cook=%u phase=msdf complete calls=%u total_ns=%llu\n",
            font_diag.cook, font_diag.calls, (unsigned long long)font_diag.msdf_ns);
    fprintf(stderr, "[font-import] cook=%u phase=write begin\n", font_diag.cook);
    fflush(stderr);
    u64 start = de_time_now_ns();
    de_result result = de__font_write(ctx, build);
    fprintf(stderr, "[font-import] cook=%u phase=write end result=%d elapsed_ns=%llu\n",
            font_diag.cook, (int)result, (unsigned long long)(de_time_now_ns() - start));
    fflush(stderr);
    return result;
}

/* Forward through the actual importer in the same order, with its original inputs/results. */
#define de_font_import           de_test_font_import
#define de_font_import_type_desc de_test_font_import_type_desc
#define de__font_pack            font_diag_pack
#define de__msdf_generate        font_diag_msdf
#define de__font_write           font_diag_write
#include "text/import/font_import.c"
#undef de__font_write
#undef de__msdf_generate
#undef de__font_pack
#undef de_font_import_type_desc
#undef de_font_import

static de_result font_diag_import(de_asset_import_ctx *ctx) {
    font_diag.calls = 0;
    font_diag.msdf_ns = 0;
    fprintf(stderr, "[font-import] cook=%u phase=import begin\n", font_diag.cook);
    fflush(stderr);
    u64 start = de_time_now_ns();
    de_result result = de_test_font_import(ctx);
    fprintf(stderr, "[font-import] cook=%u phase=import end result=%d elapsed_ns=%llu\n",
            font_diag.cook, (int)result, (unsigned long long)(de_time_now_ns() - start));
    fflush(stderr);
    return result;
}
static de_result font_cook_diagnostic(u32 index, const char *source,
                                      const de__manifest_kv *settings, u32 count,
                                      de_cook_result *out, de_cook_verify verify) {
    font_diag.cook = index;
    de_asset_type_desc type = de_test_font_import_type_desc();
    type.import = font_diag_import;
    return font_cook_with_type(source, settings, count, out, verify, &type);
}
#endif
