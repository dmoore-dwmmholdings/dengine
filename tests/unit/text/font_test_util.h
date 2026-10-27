#ifndef FONT_TEST_UTIL_H
#define FONT_TEST_UTIL_H
#include "de_test.h"
#include <dengine/de_platform.h>
#include <dengine/de_memory.h>
#include <dengine/de_text.h>
#include "cook/lib/de_cooker.h"
#include "text/import/font_import.h"
#include <stdio.h>
#include <string.h>
static inline void font_mount(void) {
    static bool m;
    if (!m) {
        ASSERT_EQ(de_fs_mount("fonts", DE_FONT_DATA, DE_MOUNT_READ_ONLY, 0), DE_OK);
        m = true;
    }
}
static inline de_result font_cook_with_verify(const char *source, const de__manifest_kv *s, u32 ns,
                                              de_cook_result *out, de_cook_verify verify) {
    font_mount();
    de_cooker *c = NULL;
    de_result r = de_cooker_create(&(de_cooker_desc){.verify_determinism = verify}, &c);
    if (r != DE_OK)
        return r;
    de_asset_type_desc td = de_font_import_type_desc();
    r = de_cooker_register_type(c, &td);
    if (r == DE_OK) {
        de__manifest_asset a = {
            .id = "ui/font", .type = "font", .source = source, .settings = s, .setting_count = ns};
        r = de_cooker_cook(c, &(de_cook_input){.asset = &a, .asset_root = "fonts:/"}, out);
    }
    de_cooker_destroy(c);
    return r;
}
static inline de_result font_cook(const char *source, const de__manifest_kv *s, u32 ns,
                                  de_cook_result *out) {
    return font_cook_with_verify(source, s, ns, out, DE_COOK_VERIFY_ON);
}
#endif
