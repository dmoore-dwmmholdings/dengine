#ifndef DE_LAYOUT_TEST_UTIL_H
#define DE_LAYOUT_TEST_UTIL_H
#include "runtime_test_util.h"
#include "text/layout/layout_internal.h"
#include "text/glyph_cache_internal.h"
#include <math.h>
static inline de_text_layout *layout_build(runtime_fx *f, const char *str, f32 width,
                                           de_text_align align, de_text_dir dir, bool snap) {
    de_text_style style = {.font = f->latin, .size_px = 20, .color = {1, 1, 1, 1}};
    de_text_layout_desc d = {.utf8 = {str, strlen(str)},
                             .style = &style,
                             .max_width = width,
                             .align = align,
                             .direction = dir,
                             .pixel_snap = snap};
    de_text_layout *l = NULL;
    ASSERT_EQ(de_text_layout_build(f->t, &d, NULL, &l), DE_OK);
    return l;
}
#endif
