#include "font_msdf_diagnostic.h"
#include "core/de_compiler.h"
#include <dengine/de_platform.h>
#include <msdfgen.h>
#include <cmath>
#include <cstdio>
#include <new>
#include <vector>
#include <core/edge-selectors.h>
#include <core/contour-combiners.h>
#include <core/ShapeDistanceFinder.h>

/* This private source inclusion changes compilation context and adds clock/log overhead.
 * Phase totals localize the existing work; they do not qualify its original timing. */
namespace {
enum Phase { SHAPE, COLOR, GENERATE, SIGN, OTHER, PHASE_COUNT };
const char *names[] = {"shape", "color", "generateMTSDF", "sign", "other"};
enum KernelPhase { BEFORE_CORRECTION, CORRECTION, KERNEL_PHASE_COUNT };
const char *kernel_names[] = {"generate_before_correction", "error_correction"};
struct Trace {
    u32 cook, calls, completed[PHASE_COUNT];
    u64 totals[PHASE_COUNT], current[PHASE_COUNT], shape_start;
    u32 kernel_completed[KERNEL_PHASE_COUNT];
    u64 kernel_totals[KERNEL_PHASE_COUNT], kernel_start;
    bool report;
};
DE_THREAD_LOCAL Trace trace;

u64 begin(Phase phase) {
    if (trace.report) {
        std::fprintf(stderr, "[font-msdf] cook=%u call=%u phase=%s begin\n", trace.cook,
                     trace.calls, names[phase]);
        std::fflush(stderr);
    }
    return de_time_now_ns();
}
void end(Phase phase, u64 elapsed) {
    trace.current[phase] += elapsed;
    trace.totals[phase] += elapsed;
    ++trace.completed[phase];
    if (trace.report) {
        std::fprintf(stderr,
                     "[font-msdf] cook=%u call=%u phase=%s end elapsed_ns=%llu total_ns=%llu\n",
                     trace.cook, trace.calls, names[phase], (unsigned long long)elapsed,
                     (unsigned long long)trace.totals[phase]);
        std::fflush(stderr);
    }
}
u64 kernel_begin(KernelPhase phase) {
    if (trace.report) {
        std::fprintf(stderr, "[font-msdf] cook=%u call=%u phase=%s begin\n", trace.cook,
                     trace.calls, kernel_names[phase]);
        std::fflush(stderr);
    }
    return de_time_now_ns();
}
void kernel_end(KernelPhase phase, u64 elapsed) {
    trace.kernel_totals[phase] += elapsed;
    ++trace.kernel_completed[phase];
    if (trace.report) {
        std::fprintf(stderr,
                     "[font-msdf] cook=%u call=%u phase=%s end elapsed_ns=%llu total_ns=%llu\n",
                     trace.cook, trace.calls, kernel_names[phase], (unsigned long long)elapsed,
                     (unsigned long long)trace.kernel_totals[phase]);
        std::fflush(stderr);
    }
}
} // namespace

/* Every correction overload in the included generator TU forwards to its original
 * before redirects are active. Nested intervals never enter the outer phase sum. */
namespace msdfgen {
static void de_test_error_correction(const BitmapSection<float, 3> &output, const Shape &shape,
                                     const SDFTransformation &transformation,
                                     const MSDFGeneratorConfig &config) {
    kernel_end(BEFORE_CORRECTION, de_time_now_ns() - trace.kernel_start);
    u64 start = kernel_begin(CORRECTION);
    msdfErrorCorrection(output, shape, transformation, config);
    kernel_end(CORRECTION, de_time_now_ns() - start);
}
static void de_test_error_correction(const BitmapSection<float, 4> &output, const Shape &shape,
                                     const SDFTransformation &transformation,
                                     const MSDFGeneratorConfig &config) {
    kernel_end(BEFORE_CORRECTION, de_time_now_ns() - trace.kernel_start);
    u64 start = kernel_begin(CORRECTION);
    msdfErrorCorrection(output, shape, transformation, config);
    kernel_end(CORRECTION, de_time_now_ns() - start);
}
static void de_test_error_correction(const BitmapSection<float, 3> &output, const Shape &shape,
                                     const Projection &projection, Range range,
                                     const MSDFGeneratorConfig &config) {
    kernel_end(BEFORE_CORRECTION, de_time_now_ns() - trace.kernel_start);
    u64 start = kernel_begin(CORRECTION);
    msdfErrorCorrection(output, shape, projection, range, config);
    kernel_end(CORRECTION, de_time_now_ns() - start);
}
static void de_test_error_correction(const BitmapSection<float, 4> &output, const Shape &shape,
                                     const Projection &projection, Range range,
                                     const MSDFGeneratorConfig &config) {
    kernel_end(BEFORE_CORRECTION, de_time_now_ns() - trace.kernel_start);
    u64 start = kernel_begin(CORRECTION);
    msdfErrorCorrection(output, shape, projection, range, config);
    kernel_end(CORRECTION, de_time_now_ns() - start);
}
} // namespace msdfgen

/* Preincluded dependencies keep redirects out of public/header-defined types. Prefix
 * every generator family and source-defined template to avoid duplicate definitions. */
#define DistancePixelConversion  de_test_DistancePixelConversion
#define generateDistanceField    de_test_generateDistanceField
#define generateSDF              de_test_generateSDF
#define generatePSDF             de_test_generatePSDF
#define generateMSDF             de_test_generateMSDF
#define generateMTSDF            de_test_generateMTSDF
#define generatePseudoSDF        de_test_generatePseudoSDF
#define generateSDF_legacy       de_test_generateSDF_legacy
#define generatePSDF_legacy      de_test_generatePSDF_legacy
#define generateMSDF_legacy      de_test_generateMSDF_legacy
#define generateMTSDF_legacy     de_test_generateMTSDF_legacy
#define generatePseudoSDF_legacy de_test_generatePseudoSDF_legacy
#define msdfErrorCorrection      de_test_error_correction
/* The existing dependency include root also works for private fault-control copies. */
#include <core/msdfgen.cpp>
#undef msdfErrorCorrection
#undef generatePseudoSDF_legacy
#undef generateMTSDF_legacy
#undef generateMSDF_legacy
#undef generatePSDF_legacy
#undef generateSDF_legacy
#undef generatePseudoSDF
#undef generateMTSDF
#undef generateMSDF
#undef generatePSDF
#undef generateSDF
#undef generateDistanceField
#undef DistancePixelConversion

/* Define the forwarding overloads before the production-wrapper redirects. Color/sign
 * call the originals; generation passes unchanged arguments to the private source copy. */
namespace msdfgen {
static void de_test_edge_coloring(Shape &shape, double angle, unsigned long long seed = 0) {
    end(SHAPE, de_time_now_ns() - trace.shape_start);
    u64 start = begin(COLOR);
    edgeColoringSimple(shape, angle, seed);
    end(COLOR, de_time_now_ns() - start);
}
static void
de_test_generate_mtsdf(const BitmapSection<float, 4> &output, const Shape &shape, Range range,
                       const Vector2 &scale, const Vector2 &translate,
                       const ErrorCorrectionConfig &correction = ErrorCorrectionConfig(),
                       bool overlap = true) {
    u64 start = begin(GENERATE);
    /* Includes distance generation plus API/setup overhead before the correction call. */
    trace.kernel_start = kernel_begin(BEFORE_CORRECTION);
    de_test_generateMTSDF(output, shape, range, scale, translate, correction, overlap);
    end(GENERATE, de_time_now_ns() - start);
}
static void de_test_sign_correction(BitmapSection<float, 4> output, const Shape &shape,
                                    const Projection &projection, float zero = .5f,
                                    FillRule fill = FILL_NONZERO) {
    u64 start = begin(SIGN);
    distanceSignCorrection(output, shape, projection, zero, fill);
    end(SIGN, de_time_now_ns() - start);
}
} // namespace msdfgen

#define de__msdf_generate      de_test_msdf_generate_impl
#define edgeColoringSimple     de_test_edge_coloring
#define generateMTSDF          de_test_generate_mtsdf
#define distanceSignCorrection de_test_sign_correction
#include "text/msdf/msdf_wrap.cpp"
#undef distanceSignCorrection
#undef generateMTSDF
#undef edgeColoringSimple
#undef de__msdf_generate

extern "C" void de_test_msdf_trace_reset(u32 cook) {
    trace = {};
    trace.cook = cook;
}
extern "C" void de_test_msdf_trace_report(void) {
    std::fprintf(stderr,
                 "[font-msdf] cook=%u phase=summary calls=%u shape_calls=%u color_calls=%u "
                 "generate_calls=%u sign_calls=%u shape_ns=%llu color_ns=%llu generate_ns=%llu "
                 "sign_ns=%llu other_ns=%llu generate_before_correction_calls=%u "
                 "error_correction_calls=%u generate_before_correction_ns=%llu "
                 "error_correction_ns=%llu\n",
                 trace.cook, trace.calls, trace.completed[SHAPE], trace.completed[COLOR],
                 trace.completed[GENERATE], trace.completed[SIGN],
                 (unsigned long long)trace.totals[SHAPE], (unsigned long long)trace.totals[COLOR],
                 (unsigned long long)trace.totals[GENERATE], (unsigned long long)trace.totals[SIGN],
                 (unsigned long long)trace.totals[OTHER], trace.kernel_completed[BEFORE_CORRECTION],
                 trace.kernel_completed[CORRECTION],
                 (unsigned long long)trace.kernel_totals[BEFORE_CORRECTION],
                 (unsigned long long)trace.kernel_totals[CORRECTION]);
    std::fflush(stderr);
}
extern "C" bool de_test_msdf_kernel_split_complete(void) {
    return trace.calls && trace.kernel_completed[BEFORE_CORRECTION] == trace.calls &&
           trace.kernel_completed[CORRECTION] == trace.calls;
}
extern "C" de_result de_test_msdf_generate(const de__msdf_vertex *vertices, u32 count, double scale,
                                           double ox, double oy, double range, u32 width,
                                           u32 height, float *scratch, u8 *rgba) {
    ++trace.calls;
    trace.report = trace.calls == 1 || trace.calls % 64 == 0;
    for (u64 &elapsed : trace.current)
        elapsed = 0;
    u64 start = de_time_now_ns();
    trace.shape_start = begin(SHAPE);
    de_result result = de_test_msdf_generate_impl(vertices, count, scale, ox, oy, range, width,
                                                  height, scratch, rgba);
    u64 total = de_time_now_ns() - start;
    u64 measured =
        trace.current[SHAPE] + trace.current[COLOR] + trace.current[GENERATE] + trace.current[SIGN];
    /* Residual includes conversion, cleanup, gaps and diagnostics. An early failure may
     * also leave unfinished phase work here; its begin/end markers identify that limit. */
    end(OTHER, total >= measured ? total - measured : 0);
    return result;
}
