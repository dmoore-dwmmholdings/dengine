#ifndef DE_TEST_WINDOW_COCOA_H
#define DE_TEST_WINDOW_COCOA_H
#include <dengine/de_base.h>

typedef struct de_test_window_cocoa_bounds {
    f64 points_width, points_height;
    f64 pixels_width, pixels_height;
} de_test_window_cocoa_bounds;

/* Read the actual AppKit view, independently of SDL's cached pixel dimensions. */
bool de_test_window_cocoa_backing(void *sdl_window, de_test_window_cocoa_bounds *out);
#endif
