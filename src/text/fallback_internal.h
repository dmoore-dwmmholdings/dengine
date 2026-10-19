#ifndef DE_TEXT_FALLBACK_INTERNAL_H
#define DE_TEXT_FALLBACK_INTERNAL_H
#include "font_runtime_internal.h"
#define DE_TEXT_MAX_CHAINS 64
#define DE_TEXT_CACHE      512
typedef struct de__text_cp_cache {
    u32 cp, glyph, font, epoch;
    bool found;
} de__text_cp_cache;
typedef struct de__text_chain {
    de_font primary, fonts[8];
    u32 count;
    de__text_cp_cache cache[DE_TEXT_CACHE];
} de__text_chain;
typedef struct de__text_watch {
    de_font handle;
    de_text_font *object;
    u32 generation;
} de__text_watch;
struct de__text_runtime {
    atomic_flag lock;
    u32 epoch, chain_count, watch_count;
    de__text_chain chains[DE_TEXT_MAX_CHAINS];
    de__text_watch watch[256];
};
void de__text_watch_font(de_text_system *t, de_font f);
static inline void de__text_lock(struct de__text_runtime *r) {
    while (atomic_flag_test_and_set_explicit(&r->lock, memory_order_acquire)) {
    }
}
static inline void de__text_unlock(struct de__text_runtime *r) {
    atomic_flag_clear_explicit(&r->lock, memory_order_release);
}
#endif
