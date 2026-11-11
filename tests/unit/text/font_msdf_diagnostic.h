#ifndef DE_TEST_FONT_MSDF_DIAGNOSTIC_H
#define DE_TEST_FONT_MSDF_DIAGNOSTIC_H
#include "text/msdf/msdf_wrap.h"
DE_EXTERN_C_BEGIN
void de_test_msdf_trace_reset(u32 cook);
void de_test_msdf_trace_report(void);
bool de_test_msdf_kernel_split_complete(void);
de_result de_test_msdf_generate(const de__msdf_vertex *vertices, u32 count, double scale, double ox,
                                double oy, double range, u32 width, u32 height, float *scratch,
                                u8 *rgba);
DE_EXTERN_C_END
#endif
