#include "../world_panel/world_fixture.h"
DE_GOLDEN_SCENE(ui_pixel_four) {
#if DE_HAS_UI && DE_HAS_RENDER && defined(DE_GOLDEN_COOK)
    ui_scene s = {0};
    de_result result = setup(&s, ctx, true);
    if (result == DE_OK) {
        node(&s, s.root, 16, 16, 448, 238, surface, 1, 0);
        label(&s, s.root, 32, 32, 400, 24, "ABBAABBA", 8);
        label(&s, s.root, 32, 72, 400, 40, "ABBA", 16);
        label(&s, s.root, 32, 128, 400, 56, "ABBA", 24);
        for (u32 i = 0; i < 12; ++i)
            node(&s, s.root, 32 + 16 * (f32)i, 208, 8, 16, i % 2 ? ink : accent, 0, 0);
        result = render(&s, 1);
    }
    u8 *pixels = NULL;
    if (result == DE_OK) {
        pixels = malloc((size_t)s.width * s.height * 4);
        if (!pixels)
            result = DE_ERR_OUT_OF_MEMORY;
    }
    if (result == DE_OK)
        result = capture(&s, pixels);
    if (result == DE_OK) {
        u32 mismatches = 0;
        for (u32 y = 0; y < s.height; y += 4)
            for (u32 x = 0; x < s.width; x += 4)
                for (u32 dy = 0; dy < 4; ++dy)
                    for (u32 dx = 0; dx < 4; ++dx)
                        mismatches += memcmp(pixels + ((u64)(y + dy) * s.width + x + dx) * 4,
                                             pixels + ((u64)y * s.width + x) * 4, 4) != 0;
        if (mismatches)
            de_golden_fail(ctx, "Pixel UI4x contains%u unequal block samples", mismatches);
        output(&s, pixels);
    } else
        de_golden_fail(ctx, "Pixel UI: %s", de_result_str(result));
    free(pixels);
    cleanup(&s);
#else
    de_golden_skip(ctx, "Pixel UI requires UI/render/cooker");
#endif
}
