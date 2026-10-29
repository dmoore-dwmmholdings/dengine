#ifndef DE_RENDER_PREVIEW_GEOMETRY_H
#define DE_RENDER_PREVIEW_GEOMETRY_H
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

typedef enum de__render_preview_shape {
    DE__RENDER_PREVIEW_SPHERE,
    DE__RENDER_PREVIEW_BOX,
    DE__RENDER_PREVIEW_PLANE,
    DE__RENDER_PREVIEW_SHAPES
} de__render_preview_shape;
#define DE__RENDER_PREVIEW_BYTES_MAX (256u << 10)
typedef struct de__render_preview_blob {
    void *data;
    u64 size;
} de__render_preview_blob;
/* DE_HAS_COOKER only. Fixed geometry through the ordinary model mesh cooker, with a 4 MiB
 * IR scratch budget. Output is at most 256 KiB, allocated from allocator (NULL = heap).
 * Caller frees data with that allocator and exact size. Failure preserves *out. */
de_result de__render_preview_geometry(const de_allocator *allocator, de__render_preview_shape shape,
                                      de__render_preview_blob *out);
#endif
