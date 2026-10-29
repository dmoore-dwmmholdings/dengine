#ifndef DE_EDITOR_PREVIEW_IMAGE_H
#define DE_EDITOR_PREVIEW_IMAGE_H
#include <dengine/de_base.h>

/* Caller-owned bounded PNG encoding. No heap, compression cache, or image history.
 * Width/height must be 1..1024. Failure leaves the destination and written size untouched. */
size_t de__ed_preview_png_size(u32 width, u32 height);
de_result de__ed_preview_png(u32 width, u32 height, const u8 *rgba, void *png, size_t capacity,
                             size_t *written);
/* Convert the renderer's row-pitched RGBA16F colour to opaque sRGB RGBA8. */
void de__ed_preview_rgba(u32 width, u32 height, const void *half, u32 pitch, u8 *rgba);
#endif
