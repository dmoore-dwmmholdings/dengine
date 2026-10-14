/* flip.h — LDR-FLIP, the perceptual image difference of the golden-image harness (DENG-329).
 *
 * A C11 implementation of LDR-FLIP (Andersson et al., "FLIP: A Difference Evaluator for
 * Alternating Images", HPG 2020), written from the published algorithm and checked against
 * NVIDIA's official implementation (BSD-3-Clause; not vendored): tests/golden/flip/data/ holds
 * image pairs and the error values that the official `flip-evaluator` gave for them, and
 * test_flip.c requires the same mean error within 1e-3.
 *
 * The algorithm, per pixel of a reference and a test image (sRGB, 8 bits per channel):
 *   - Color: both images go to the opponent space YyCxCz and are filtered with the contrast
 *     sensitivity functions (one Gaussian for Yy and Cx, a sum of two for Cz), back to linear RGB
 *     clamped to [0, 1], to CIELAB with the Hunt adjustment (a, b scaled by 0.01 L). The color
 *     error is the HyAB distance, raised to 0.7 and mapped to [0, 1] with the green-blue distance
 *     as the maximum (95 % of the range for the first 40 % of it).
 *   - Features: edges (first derivative of a Gaussian) and points (second derivative) of the
 *     luminance of both images; the feature difference is the larger of the two magnitude
 *     differences over sqrt(2), raised to 0.5.
 *   - FLIP = color error ^ (1 - feature difference), in [0, 1]. 0 means no visible difference.
 * The filters are sized in degrees of visual angle, so the result depends on the pixels per
 * degree (PPD) of the assumed viewing setup.
 *
 * Image borders are clamped (edge pixels repeat), as in the official implementation. The filters
 * are separable and run in two passes; every intermediate is f32. No engine dependencies: plain C
 * with malloc, so the test and the harness can use it alike. Each call allocates its own scratch,
 * so concurrent calls are safe.
 */
#ifndef DE_GOLDEN_FLIP_H
#define DE_GOLDEN_FLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Pixels per degree of a display `width_px` pixels and `width_m` metres wide, seen from
 * `distance_m` metres: distance * (width_px / width_m) * pi / 180. */
float de_flip_ppd(float distance_m, float width_px, float width_m);

/** The default viewing setup of FLIP: a 0.7 m wide 4K (3840 px) monitor at 0.7 m, about 67.02
 * pixels per degree. */
#define DE_FLIP_DEFAULT_DISTANCE_M 0.7f
#define DE_FLIP_DEFAULT_WIDTH_PX   3840.0f
#define DE_FLIP_DEFAULT_WIDTH_M    0.7f
/** de_flip_ppd() of the default viewing setup. */
float de_flip_default_ppd(void);

/** An sRGB image, 8 bits per channel, `channels` (3 or 4; a fourth is ignored) per pixel, rows
 * `pitch` bytes apart (0 = width * channels). Row 0 is the top. */
typedef struct de_flip_image {
    const uint8_t *pixels;
    uint32_t width, height;
    uint32_t channels;
    size_t pitch;
} de_flip_image;

/**
 * Computes the LDR-FLIP error of `test` against `ref` (same size) into `errors`, width * height
 * floats in [0, 1], row 0 first. `ppd` <= 0 means de_flip_default_ppd(). Returns false when the
 * sizes or channel counts differ or are invalid, or when the scratch memory cannot be allocated.
 */
bool de_flip_ldr(const de_flip_image *ref, const de_flip_image *test, float ppd, float *errors);

/** Summary of an error map. */
typedef struct de_flip_stats {
    double mean; /**< arithmetic mean, the "mean FLIP" of the official tool */
    float p99;   /**< 99th percentile: the value that 99 % of the pixels do not exceed */
    float max;
} de_flip_stats;

/**
 * Statistics of `count` errors. The percentile is the nearest-rank one on a sorted copy (index
 * ceil(0.99 * count) - 1). Returns false when the copy cannot be allocated or count is 0.
 */
bool de_flip_stats_compute(const float *errors, size_t count, de_flip_stats *out);

/** Writes the error map as an RGBA8 heat map (`rgba`: width * height * 4 bytes, opaque) with the
 * magma color map that the official tool uses: black is no difference, light yellow is 1. */
void de_flip_heatmap(const float *errors, uint32_t width, uint32_t height, uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif
