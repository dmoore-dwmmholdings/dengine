#ifndef DE_TEXT_GLYPH_CACHE_INTERNAL_H
#define DE_TEXT_GLYPH_CACHE_INTERNAL_H
#include "font_runtime_internal.h"
#include "atlas_packer.h"
#include <dengine/de_task.h>
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>
#define DE_GLYPH_REQUESTS 8192
#define DE_GLYPH_FONTS    256
#define DE_GLYPH_HASH     16384
typedef struct de__glyph_font {
    de_font handle;
    const de_text_font *identity;
    u8 *source;
    u32 source_size, base_pages, width, height;
    f32 em, range;
    u8 *pixels[8];
    de_atlas_packer *packers[8];
    u32 pages;
    bool warned;
} de__glyph_font;
typedef struct de__glyph_job {
    struct de__glyph_cache *owner;
    u32 font, glyph, status, worker;
    de_result result;
    char thread_name[32];
    de__font_glyph entry;
    u8 *pixels;
} de__glyph_job;
struct de__glyph_cache {
    const de_allocator *alloc;
    de_text_system *text;
    de_task_system *tasks;
    bool own_tasks;
    de_task_counter counter;
    de_mutex mutex;
    de_cvar per_frame, pages_max;
    u32 font_count, count;
    u32 hash[DE_GLYPH_HASH];
    de__glyph_font fonts[DE_GLYPH_FONTS];
    de__glyph_job jobs[DE_GLYPH_REQUESTS];
    de_text_stats stats;
};
de_result de_text__glyph_cache_init(de_text_system *t);
void de_text__glyph_cache_destroy(de_text_system *t);
bool de_text__runtime_glyph(de_text_system *t, de_font font, u32 glyph, de_text_glyph_entry *out);
u32 de_text__page_count(de_text_system *t, de_font font);
const u8 *de_text__page_data(de_text_system *t, de_font font, u32 page);
void de_text__glyph_jobs_wait(de_text_system *t);
#endif
