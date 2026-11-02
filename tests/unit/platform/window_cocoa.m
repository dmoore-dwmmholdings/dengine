#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include "window_cocoa.h"

bool de_test_window_cocoa_backing(void *sdl_window, de_test_window_cocoa_bounds *out) {
    @autoreleasepool {
        SDL_PropertiesID properties = SDL_GetWindowProperties(sdl_window);
        NSWindow *window = (__bridge NSWindow *)SDL_GetPointerProperty(
            properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
        if (![window isKindOfClass:[NSWindow class]] || !window.contentView)
            return false;
        NSView *view = window.contentView;
        NSRect points = view.bounds;
        NSRect pixels = [view convertRectToBacking:points];
        *out = (de_test_window_cocoa_bounds){points.size.width, points.size.height,
                                             pixels.size.width, pixels.size.height};
        return points.size.width > 0 && points.size.height > 0 && pixels.size.width > 0 &&
               pixels.size.height > 0;
    }
}
