/* simd_sse.c: x86-64 SSE4.2 fast-math kernels (DENG-242, SPEC §5.4).
 *
 * Compiled with -msse4.2 (src/core/CMakeLists.txt); MSVC needs no flag for these intrinsics. The
 * dispatcher selects this table only when cpuid reports SSE4.2. There is no FMA at this level, so
 * v4_madd is a multiply then an add. On other architectures (or without the flag) the file
 * compiles to nothing and de__math_table_sse42() returns NULL. */
#include "core/math/math_simd.h"

/* MSVC cl has no __SSE4_2__ macro and needs no flag; every other compiler must have been given
 * -msse4.2, or the table is left out (a universal macOS build's x86_64 slice, for example). */
#if (defined(__x86_64__) || defined(_M_X64)) &&                                                    \
    (defined(__SSE4_2__) || (defined(_MSC_VER) && !defined(__clang__)))

#include <smmintrin.h> /* SSE4.1: blendv */

typedef __m128 v4;

#define DE__SIMD_FN(name) de__##name##_sse42

#define v4_load(p)                _mm_loadu_ps(p)
#define v4_store(p, v)            _mm_storeu_ps((p), (v))
#define v4_set1(x)                _mm_set1_ps(x)
#define v4_add(a, b)              _mm_add_ps((a), (b))
#define v4_sub(a, b)              _mm_sub_ps((a), (b))
#define v4_mul(a, b)              _mm_mul_ps((a), (b))
#define v4_div(a, b)              _mm_div_ps((a), (b))
#define v4_madd(a, b, c)          _mm_add_ps(_mm_mul_ps((a), (b)), (c))
#define v4_abs(v)                 _mm_andnot_ps(_mm_set1_ps(-0.0f), (v))
#define v4_cmpge(a, b)            _mm_cmpge_ps((a), (b))
#define v4_cmplt(a, b)            _mm_cmplt_ps((a), (b))
#define v4_cmpgt(a, b)            _mm_cmpgt_ps((a), (b))
#define v4_or(a, b)               _mm_or_ps((a), (b))
#define v4_select(m, a, b)        _mm_blendv_ps((b), (a), (m))
#define v4_movemask(m)            _mm_movemask_ps(m)
#define v4_shuf(a, b, i, j, k, l) _mm_shuffle_ps((a), (b), _MM_SHUFFLE(l, k, j, i))

#include "core/math/simd_kernels.h"

static const de__math_table de__table_sse42 = DE__SIMD_TABLE_INIT(DE_MATH_ISA_SSE42);

const de__math_table *de__math_table_sse42(void) {
    return &de__table_sse42;
}

#else

const de__math_table *de__math_table_sse42(void) {
    return NULL;
}

#endif
