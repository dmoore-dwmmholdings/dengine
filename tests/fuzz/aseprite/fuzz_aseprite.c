#include "de_fuzz.h"
#include "cook/importers/aseprite/aseprite_parse.h"
#include "cook/importers/aseprite/aseprite_render.h"
#include <stdlib.h>

/* Exact malloc blocks expose decoder overreads to ASan; the engine's arena heap would
 * otherwise hide accesses that stay inside a larger reserved heap page. */
static void *fuzz_alloc(void *user, size_t bytes, size_t align, u32 tag) {
    (void)user;
    (void)tag;
    return align <= 16 ? malloc(bytes) : NULL;
}
static void fuzz_free(void *user, void *ptr, size_t bytes) {
    (void)user;
    (void)bytes;
    free(ptr);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 1024u * 1024u)
        return 0;
    de_aseprite_doc *doc = NULL;
    const de_allocator allocator = {.alloc = fuzz_alloc, .free = fuzz_free};
    de_result r = de_aseprite_parse(data, size, &allocator, 8u * 1024u * 1024u, &doc, NULL);
    if (r == DE_OK) {
        if (!doc || !doc->frames || doc->allocated > doc->max_bytes)
            abort();
        for (u32 i = 0; i < doc->cel_count; ++i) {
            const de_aseprite_cel *c = &doc->cels[i];
            if (c->layer >= doc->layer_count || c->frame >= doc->frame_count)
                abort();
            if (doc->layers[c->layer].type == 0 && (!c->pixels || !c->width || !c->height))
                abort();
        }
        if ((u32)doc->width * doc->height <= 4096u && doc->layer_count <= 256u &&
            doc->frame_count <= 8u) {
            de_aseprite_renderer *renderer = NULL;
            de_result rendered = de_aseprite_renderer_create(doc, &allocator, 256u * 1024u, NULL,
                                                             NULL, &renderer, NULL);
            if (rendered == DE_OK) {
                size_t stride = (size_t)doc->width * 4u, bytes = stride * doc->height;
                u8 *pixels = malloc(bytes);
                if (pixels) {
                    for (u32 frame = 0; frame < doc->frame_count; ++frame)
                        if (de_aseprite_render_frame(renderer, frame, UINT32_MAX, pixels, stride,
                                                     bytes, NULL) != DE_OK)
                            abort();
                    for (u32 layer = 0; layer < doc->layer_count; ++layer)
                        if (doc->layers[layer].type == 0) {
                            if (de_aseprite_render_frame(renderer, 0, layer, pixels, stride, bytes,
                                                         NULL) != DE_OK)
                                abort();
                            break;
                        }
                    free(pixels);
                }
                de_aseprite_renderer_destroy(renderer);
            } else if (renderer)
                abort();
        }
    } else if (doc)
        abort();
    de_aseprite_free(doc);
    return 0;
}
