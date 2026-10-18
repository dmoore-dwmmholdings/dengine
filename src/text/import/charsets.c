#include "font_import.h"
#include <string.h>
static void add(u8 *b, u32 l, u32 h) {
    for (u32 c = l; c <= h; ++c)
        b[c >> 3] |= (u8)(1u << (c & 7));
}
de_result de__font_charset(const char *n, u8 *b, u32 bytes) {
    if (!n || !b || bytes < 0x110000 / 8)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "charset buffer");
    if (!strcmp(n, "ascii"))
        add(b, 32, 126);
    else if (!strcmp(n, "latin")) {
        add(b, 32, 126);
        add(b, 160, 255);
    } else if (!strcmp(n, "latin-ext")) {
        add(b, 32, 126);
        add(b, 160, 0x24f);
        add(b, 0x1e00, 0x1eff);
    } else if (!strcmp(n, "greek")) {
        add(b, 0x370, 0x3ff);
        add(b, 0x1f00, 0x1fff);
    } else if (!strcmp(n, "cyrillic")) {
        add(b, 0x400, 0x52f);
        add(b, 0x2de0, 0x2dff);
        add(b, 0xa640, 0xa69f);
    } else if (!strcmp(n, "vietnamese")) {
        add(b, 32, 0x24f);
        add(b, 0x1e00, 0x1eff);
    } else if (!strcmp(n, "kana")) {
        add(b, 0x3040, 0x30ff);
        add(b, 0x31f0, 0x31ff);
        add(b, 0xff65, 0xff9f);
    } else
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "unknown font charset %s", n);
    return DE_OK;
}
