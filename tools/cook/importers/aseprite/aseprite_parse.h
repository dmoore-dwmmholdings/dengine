/* Bounded, tool-only Aseprite source decoder. Strings and raw pixels borrow the source;
 * keep it alive until de_aseprite_free. Compressed pixels and metadata use the explicit
 * allocator. No cooked representation is introduced here: the importer emits DSCP. */
#ifndef DE_ASEPRITE_PARSE_H
#define DE_ASEPRITE_PARSE_H
#include <dengine/de_memory.h>

typedef struct de_aseprite_string {
    const char *data;
    u32 size;
} de_aseprite_string;
typedef struct de_aseprite_user_data {
    de_aseprite_string text;
    u8 color[4];
    bool has_color;
} de_aseprite_user_data;
typedef struct de_aseprite_layer {
    de_aseprite_string name;
    de_aseprite_user_data user;
    u32 parent; /* UINT32_MAX for a top-level layer */
    u16 flags, type, depth, blend;
    u8 opacity;
} de_aseprite_layer;
typedef struct de_aseprite_cel {
    de_aseprite_user_data user;
    const u8 *pixels; /* native source depth, including resolved linked cels */
    u32 frame;
    i16 x, y, z;
    u16 layer, type, width, height, linked_frame;
    u8 opacity;
} de_aseprite_cel;
typedef struct de_aseprite_frame {
    u32 first_cel, cel_count, offset, bytes, chunks;
    u16 ms;
    u8 palette[256][4];
    bool has_new_palette;
} de_aseprite_frame;
typedef struct de_aseprite_tag {
    de_aseprite_string name;
    de_aseprite_user_data user;
    u16 from, to, repeats;
    u8 direction;
} de_aseprite_tag;
typedef struct de_aseprite_slice_key {
    u32 frame, width, height, center_width, center_height;
    i32 x, y, center_x, center_y, pivot_x, pivot_y;
} de_aseprite_slice_key;
typedef struct de_aseprite_slice {
    de_aseprite_string name;
    de_aseprite_user_data user;
    de_aseprite_slice_key *keys;
    u32 key_count, flags;
} de_aseprite_slice;
typedef struct de_aseprite_doc {
    de_aseprite_layer *layers;
    de_aseprite_cel *cels;
    de_aseprite_frame *frames;
    de_aseprite_tag *tags;
    de_aseprite_slice *slices;
    de_aseprite_user_data user;
    u32 layer_count, cel_count, tag_count, slice_count, flags;
    u16 width, height, depth, frame_count;
    u8 transparent_index;
    bool has_new_palette, has_tilemaps;
    /* Private allocation state. max_bytes includes metadata and inflate buffers. */
    de_allocator allocator;
    void *allocations;
    size_t allocated, max_bytes;
    de_result result;
    const char *why;
} de_aseprite_doc;

/** Zero max_bytes selects a 256 MiB aggregate allocation/canvas budget. Input is bounded
 * independently to 256 MiB. On failure *out is NULL and all partial allocations are freed.
 * why, when supplied, receives a static diagnostic string. */
de_result de_aseprite_parse(const void *source, u64 bytes, const de_allocator *allocator,
                            size_t max_bytes, de_aseprite_doc **out, const char **why);
void de_aseprite_free(de_aseprite_doc *doc);
#endif
