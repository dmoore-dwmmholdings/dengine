#ifndef DE_ASEPRITE_ATLAS_PACK_H
#define DE_ASEPRITE_ATLAS_PACK_H
#include "aseprite_render.h"

typedef struct de_aseprite_atlas_frame {
    u32 x, y, width, height; /* content rectangle, excluding the one-pixel extrusion */
    u32 trim_x, trim_y, trim_width, trim_height; /* source canvas bounds; zero extent = empty */
    u16 ms;
} de_aseprite_atlas_frame;
typedef struct de_aseprite_atlas_set {
    de_aseprite_string name; /* borrowed layer name; empty for the flattened set */
    u32 layer, first_frame;  /* layer = UINT32_MAX for the flattened primary clip */
} de_aseprite_atlas_set;
typedef struct de_aseprite_atlas {
    u32 width, height, canvas_width, canvas_height;
    u32 set_count, frames_per_set, frame_count;
    de_aseprite_atlas_set *sets;
    de_aseprite_atlas_frame *frames;
    u8 *rgba;
    /* Private ownership; source remains alive while set names are used. */
    de_allocator allocator;
    size_t metadata_bytes, rgba_bytes;
} de_aseprite_atlas;
typedef struct de_aseprite_atlas_desc {
    bool keep_layers; /* additionally pack each visible image layer in source layer order */
    u32 max_size;     /* zero = 4096; at most 16384, including extrusion */
    size_t max_bytes; /* zero = 256 MiB; all live atlas allocations, including temporary work */
    de_aseprite_warning_fn warning;
    void *warning_user;
} de_aseprite_atlas_desc;

/* Two rendering passes share one canvas: measure trim bounds, then fill the chosen layout.
 * Deterministic sorted skyline packing, no rotation. Empty frames occupy a transparent 1x1
 * content tile. Every tile includes one extruded texel on every side, including corners.
 * On failure *out is NULL, all atlas allocations are freed, and why is a static diagnostic.
 * A size/budget limit returns UNSUPPORTED; parent allocator failure returns OUT_OF_MEMORY. */
de_result de_aseprite_atlas_build(const de_aseprite_doc *doc, const de_allocator *allocator,
                                  const de_aseprite_atlas_desc *desc, de_aseprite_atlas **out,
                                  const char **why);
void de_aseprite_atlas_free(de_aseprite_atlas *atlas);

/* Existing DTEX writer, uncompressed RGBA8 sRGB and one mip for independent atlas regions.
 * pixel_art selects nearest sampling. Caller frees *data with allocator and *bytes. */
de_result de_aseprite_atlas_encode(const de_aseprite_atlas *atlas, const de_allocator *allocator,
                                   bool pixel_art, void **data, u64 *bytes);
#endif
