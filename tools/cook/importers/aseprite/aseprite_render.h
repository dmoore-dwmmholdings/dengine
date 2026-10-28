#ifndef DE_ASEPRITE_RENDER_H
#define DE_ASEPRITE_RENDER_H
#include "aseprite_parse.h"

typedef struct de_aseprite_renderer de_aseprite_renderer;
typedef void (*de_aseprite_warning_fn)(void *user, de_aseprite_string layer, const char *message);

/* Source/doc stay alive until destroy. One bounded allocation holds the render plan
 * and one scanline per group depth; rendering frames performs no allocator calls.
 * Warnings name each visible unsupported layer once at creation. max_bytes (zero =
 * 256 MiB) bounds renderer storage, excluding the caller-owned output and doc. */
de_result de_aseprite_renderer_create(const de_aseprite_doc *doc, const de_allocator *allocator,
                                      size_t max_bytes, de_aseprite_warning_fn warning, void *user,
                                      de_aseprite_renderer **out, const char **why);
void de_aseprite_renderer_destroy(de_aseprite_renderer *renderer);

/* UINT32_MAX flattens all visible image layers. An image-layer index renders only that
 * layer, retaining ancestor visibility and group opacity (the layers="keep" frame set).
 * Output is straight RGBA8, top to bottom; scanline padding is untouched. Hidden layers
 * render transparent. Invalid arguments leave output untouched. */
de_result de_aseprite_render_frame(de_aseprite_renderer *renderer, u32 frame, u32 layer, void *rgba,
                                   size_t stride, size_t bytes, const char **why);
bool de_aseprite_layer_visible(const de_aseprite_renderer *renderer, u32 layer);
#endif
