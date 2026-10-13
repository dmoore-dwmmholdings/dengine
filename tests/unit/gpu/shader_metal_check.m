/* shader_metal_check.m: loads a cooked metallib on the host's Metal device for
 * test_shader_cook.c (DENG-289: "metallib loads with newLibraryWithData:"). Built with ARC. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>

/* 1: the library loads and has `entry`; 0: it does not (reason in `err`); -1: no Metal device. */
int de_test_metallib_load(const void *data, size_t size, const char *entry, char *err,
                          size_t cap);

int de_test_metallib_load(const void *data, size_t size, const char *entry, char *err,
                          size_t cap) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil)
            return -1;
        dispatch_data_t d = dispatch_data_create(data, size, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithData:d error:&e];
        if (lib == nil) {
            snprintf(err, cap, "newLibraryWithData: %s",
                     e != nil ? e.localizedDescription.UTF8String : "failed");
            return 0;
        }
        id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:entry]];
        if (fn == nil) {
            snprintf(err, cap, "no function \"%s\" in the library", entry);
            return 0;
        }
        return 1;
    }
}
