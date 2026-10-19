#include "msdf_wrap.h"
#include <msdfgen.h>
#include <cmath>
#include <new>
extern "C" de_result de__msdf_generate(const de__msdf_vertex *v, u32 count, double scale, double ox,
                                       double oy, double range, u32 w, u32 h, float *scratch,
                                       u8 *rgba) {
    if (!v || !count || !scratch || !rgba || !w || !h || scale <= 0 || range <= 0)
        return DE_ERR_INVALID_ARGUMENT;
    try {
        msdfgen::Shape shape;
        msdfgen::Contour *c = nullptr;
        msdfgen::Point2 p;
        for (u32 i = 0; i < count; ++i) {
            const auto &a = v[i];
            msdfgen::Point2 q(a.x, a.y);
            if (a.kind == DE_MSDF_MOVE)
                c = &shape.addContour();
            else if (!c)
                return DE_ERR_PARSE;
            else if (a.kind == DE_MSDF_LINE)
                c->addEdge(msdfgen::EdgeHolder(p, q));
            else if (a.kind == DE_MSDF_QUAD)
                c->addEdge(msdfgen::EdgeHolder(p, msdfgen::Point2(a.cx, a.cy), q));
            else if (a.kind == DE_MSDF_CUBIC)
                c->addEdge(msdfgen::EdgeHolder(p, msdfgen::Point2(a.cx, a.cy),
                                               msdfgen::Point2(a.cx1, a.cy1), q));
            else
                return DE_ERR_PARSE;
            p = q;
        }
        shape.normalize();
        if (!shape.validate())
            return DE_ERR_PARSE;
        msdfgen::edgeColoringSimple(shape, 3.0, 0);
        msdfgen::BitmapSection<float, 4> bitmap(scratch, (int)w, (int)h, msdfgen::Y_UPWARD);
        msdfgen::generateMTSDF(bitmap, shape, msdfgen::Range(range / scale),
                               msdfgen::Vector2(scale), msdfgen::Vector2(ox, oy));
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x)
                for (u32 k = 0; k < 4; ++k) {
                    float n = scratch[4 * (y * w + x) + k];
                    n = n < 0 ? 0 : n > 1 ? 1 : n;
                    rgba[4 * ((h - 1 - y) * w + x) + k] = (u8)std::floor(n * 255.0f + 0.5f);
                }
        return DE_OK;
    } catch (const std::bad_alloc &) {
        return DE_ERR_OUT_OF_MEMORY;
    }
}
