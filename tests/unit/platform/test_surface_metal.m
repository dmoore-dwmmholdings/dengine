/* test_surface_metal.m: Objective-C checks for test_surface.c (macOS, DENG-250). ARC. */
#import <QuartzCore/CAMetalLayer.h>

#include <stdbool.h>

bool test_surface_is_metal_layer(void *layer);
double test_surface_layer_scale(void *layer);
void test_surface_layer_drawable(void *layer, double *width, double *height);

bool test_surface_is_metal_layer(void *layer) {
    id obj = (__bridge id)layer;
    return [obj isKindOfClass:[CAMetalLayer class]];
}

double test_surface_layer_scale(void *layer) {
    return (double)((__bridge CAMetalLayer *)layer).contentsScale;
}

void test_surface_layer_drawable(void *layer, double *width, double *height) {
    CGSize size = ((__bridge CAMetalLayer *)layer).drawableSize;
    *width = (double)size.width;
    *height = (double)size.height;
}
