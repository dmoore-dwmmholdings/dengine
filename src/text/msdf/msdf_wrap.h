#ifndef DE_MSDF_WRAP_H
#define DE_MSDF_WRAP_H
#include <dengine/de_base.h>
DE_EXTERN_C_BEGIN
/* Coordinates use font units, upward Y. A move begins a closed contour. */
typedef struct de__msdf_vertex {
    int kind;
    double x, y, cx, cy, cx1, cy1;
} de__msdf_vertex;
enum { DE_MSDF_MOVE = 1, DE_MSDF_LINE = 2, DE_MSDF_QUAD = 3, DE_MSDF_CUBIC = 4 };
/* Output is top-down RGBA8 MTSDF; scratch floats supplied by the caller. */
de_result de__msdf_generate(const de__msdf_vertex *v, u32 count, double scale, double origin_x,
                            double origin_y, double range, u32 width, u32 height, float *scratch,
                            u8 *rgba);
DE_EXTERN_C_END
#endif
