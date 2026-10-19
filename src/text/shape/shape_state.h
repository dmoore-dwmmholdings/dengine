#ifndef DE_TEXT_SHAPE_STATE_H
#define DE_TEXT_SHAPE_STATE_H
#include "shaper_internal.h"
#include "text/fallback_internal.h"
#include <dengine/de_platform.h>
#include <dengine/de_debug.h>
#include <hb-ot.h>
#define DE_SHAPE_STRIPES 16
#define DE_SHAPE_NODES   256
#define DE_SHAPE_BLOCK   65536
/* Fixed blocks from stripe arenas are reused on LRU eviction; no allocation on cache hit. */
typedef struct de__shape_key {
    u32 font, size, spacing, features, direction;
    u64 generations;
    char language[64];
} de__shape_key;
typedef struct de__shape_node {
    u64 hash, tick;
    u32 len, count;
    de__shape_key key;
    u8 *block;
} de__shape_node;
typedef struct de__shape_stripe {
    de_mutex mutex;
    de_arena *arena;
    u64 clock;
    de__shape_node nodes[DE_SHAPE_NODES];
} de__shape_stripe;
typedef struct de__hb_font {
    de_font handle;
    u32 generation;
    const de_text_font *identity;
    hb_font_t *font;
    u32 upem;
} de__hb_font;
struct de__shape_system {
    const de_allocator *alloc;
    de_cvar capacity;
    de_tls_key tls;
    de_mutex font_mutex;
    de__hb_font fonts[256];
    u32 font_count, buffer_count;
    hb_buffer_t *buffers[128];
    de__shape_stripe stripes[DE_SHAPE_STRIPES];
};
u64 de__shape_hash(de_text_system *t, de_str text, const de_text_style *s, de_text_dir dir,
                   de__shape_key *out);
bool de__shape_cache_get(de_text_system *t, u64 hash, const de__shape_key *key, de_str text,
                         const de_allocator *a, de_shaped_run *out);
void de__shape_cache_put(de_text_system *t, u64 hash, const de__shape_key *key, de_str text,
                         const de_shaped_run *run);
#endif
