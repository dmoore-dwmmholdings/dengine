#include "de_test.h"
#include "../../golden/ui/glyph_wait.h"
TEST(ui_golden_wait, cold_render_without_glyph_work_still_completes_all_five_warm_frames) {
    /* Actual lavapipe frame0 trace from40506711571: pump succeeds, pending=0,35.18s. */
    const uint64_t first_frame_ns = UINT64_C(35179638768);
    for (uint32_t frame = 0; frame < 4; ++frame)
        EXPECT_EQ(de_ui_golden_glyph_wait(frame, 0, first_frame_ns), DE_UI_GOLDEN_WAIT);
    EXPECT_EQ(de_ui_golden_glyph_wait(4, 0, first_frame_ns), DE_UI_GOLDEN_READY);
}
TEST(ui_golden_wait, pending_glyph_work_keeps_the_exact_five_second_deadline) {
    for (uint32_t frame = 0; frame <= 4; ++frame) {
        EXPECT_EQ(de_ui_golden_glyph_wait(frame, 1, UINT64_C(5000000000)), DE_UI_GOLDEN_WAIT);
        EXPECT_EQ(de_ui_golden_glyph_wait(frame, 1, UINT64_C(5000000001)), DE_UI_GOLDEN_TIMEOUT);
    }
    EXPECT_EQ(de_ui_golden_glyph_wait(3, 0, 1), DE_UI_GOLDEN_WAIT);
    EXPECT_EQ(de_ui_golden_glyph_wait(4, 1, 1), DE_UI_GOLDEN_WAIT);
    EXPECT_EQ(de_ui_golden_glyph_wait(4, 0, 1), DE_UI_GOLDEN_READY);
}
