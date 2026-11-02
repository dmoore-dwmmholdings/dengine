#include "de_test.h"
#include "de_fuzz.h"

#include <dengine/de_memory.h>
#include <string.h>

static void repeated_stylesheet(const char *source) {
    const size_t length = strlen(source);
    /* Initialize process-wide names and registries before measuring one repeated input. */
    for (u32 i = 0; i < 4; ++i)
        ASSERT_EQ(LLVMFuzzerTestOneInput((const uint8_t *)source, length), 0);
    de_tlsf_info before, after;
    ASSERT_EQ(de_heap_check(&before), DE_OK);
    const de_mem_stats general = de_mem_stats_for_tag(DE_MEM_GENERAL);
    const de_mem_stats ui = de_mem_stats_for_tag(DE_MEM_UI);
    for (u32 i = 0; i < 64; ++i)
        ASSERT_EQ(LLVMFuzzerTestOneInput((const uint8_t *)source, length), 0);
    ASSERT_EQ(de_heap_check(&after), DE_OK);
    EXPECT_EQ(after.used_bytes, before.used_bytes);
    EXPECT_EQ(after.used_blocks, before.used_blocks);
    EXPECT_EQ(de_mem_stats_for_tag(DE_MEM_GENERAL).bytes_live, general.bytes_live);
    EXPECT_EQ(de_mem_stats_for_tag(DE_MEM_UI).bytes_live, ui.bytes_live);
}

TEST(ui_stylesheet_fuzz_ownership, empty_input_releases_builtin_and_custom_sheets) {
    repeated_stylesheet("#\n#");
}
TEST(ui_stylesheet_fuzz_ownership, matching_rule_releases_both_compiled_sheets) {
    repeated_stylesheet("[style.button]\nopacity=0.5\n");
}
