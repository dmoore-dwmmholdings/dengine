/* surface_metal.m: the Objective-C part of the Metal surface (macOS, DENG-250). Compiled with
 * -fobjc-arc. Kept to what C cannot do: setting a property of the CAMetalLayer that SDL's Metal
 * view owns. Everything else is in surface_sdl.c. */
#include "platform/sdl/sdl_internal.h"

#import <QuartzCore/CAMetalLayer.h>

void de__platform_sdl_metal_layer_set_scale(void *layer, float scale) {
    /* __bridge: the view owns the layer; no retain or release here. */
    CAMetalLayer *ml = (__bridge CAMetalLayer *)layer;
    if ([ml isKindOfClass:[CAMetalLayer class]])
        ml.contentsScale = (CGFloat)scale;
}
