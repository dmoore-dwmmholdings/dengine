#ifndef DE_UI_GOLDEN_GLYPH_WAIT_H
#define DE_UI_GOLDEN_GLYPH_WAIT_H
#include <stdint.h>
typedef enum de_ui_golden_wait_status {
    DE_UI_GOLDEN_WAIT,
    DE_UI_GOLDEN_READY,
    DE_UI_GOLDEN_TIMEOUT
} de_ui_golden_wait_status;
static inline de_ui_golden_wait_status
de_ui_golden_glyph_wait(uint32_t frame_index, uint32_t pending, uint64_t elapsed_ns) {
    if (frame_index >= 4 && pending == 0)
        return DE_UI_GOLDEN_READY;
    /* The watchdog diagnoses outstanding glyph work, not successful cold GPU frames. */
    if (pending != 0 && elapsed_ns > UINT64_C(5000000000))
        return DE_UI_GOLDEN_TIMEOUT;
    return DE_UI_GOLDEN_WAIT;
}
#endif
