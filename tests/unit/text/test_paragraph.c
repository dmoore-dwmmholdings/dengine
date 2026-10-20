#include "layout_test_util.h"
TEST_F(paragraph, wrapping_alignment_and_justification, runtime_fx) {
    de_text_layout *a = layout_build(f, "The quick", 0, DE_ALIGN_START, DE_DIR_LTR, false);
    de_text_layout *b = layout_build(f, "brown fox", 0, DE_ALIGN_START, DE_DIR_LTR, false);
    f32 width = (a->size.x + b->size.x) * .5f;
    printf("phrase widths Thequick=%.3f brownfox=%.3f wrap=%.3f\n", a->size.x, b->size.x, width);
    de_text_layout_free(a);
    de_text_layout_free(b);
    de_text_layout *l =
        layout_build(f, "The quick brown fox", width, DE_ALIGN_START, DE_DIR_LTR, false);
    EXPECT_EQ(l->line_count, 3u);
    EXPECT_GT(l->quad_count, 10u);
    EXPECT_TRUE(l->size.x <= width + .001f);
    de_text_layout_free(l);
    a = layout_build(f, "aa aa", 0, DE_ALIGN_START, DE_DIR_LTR, false);
    width = a->size.x + 3;
    de_text_layout_free(a);
    l = layout_build(f, "aa aa aa aa aa aa", width, DE_ALIGN_JUSTIFY, DE_DIR_LTR, false);
    EXPECT_EQ(l->line_count, 3u);
    for (u32 line = 0; line < 2; ++line) {
        f32 right = 0;
        for (u32 i = 0; i < l->cluster_count; ++i) {
            de__layout_cluster *c = &l->clusters[i];
            if (c->line == line && l->utf8[c->start] != ' ' && c->x1 > right)
                right = c->x1;
        }
        EXPECT_FLOAT_EQ(right, width, .001f);
    }
    de_text_layout_free(l);
    l = layout_build(f, "abc", 100, DE_ALIGN_CENTER, DE_DIR_LTR, false);
    EXPECT_TRUE(l->clusters[0].x0 > 20);
    de_text_layout_free(l);
    l = layout_build(f, "abc", 100, DE_ALIGN_END, DE_DIR_LTR, false);
    EXPECT_FLOAT_EQ(l->clusters[l->cluster_count - 1].x1, 100, .001f);
    de_text_layout_free(l);
}
TEST_F(paragraph, hard_breaks_spacing_snap_and_invalid_utf8, runtime_fx) {
    de_text_style style = {.font = f->latin,
                           .size_px = 17.3f,
                           .letter_spacing = 1.2f,
                           .line_height = 1.5f,
                           .color = {1, 1, 1, 1}};
    de_text_layout_desc d = {.utf8 = DE_STR("abc\r\ndef\n"), .style = &style, .pixel_snap = true};
    de_text_layout *l;
    ASSERT_EQ(de_text_layout_build(f->t, &d, NULL, &l), DE_OK);
    EXPECT_EQ(l->line_count, 3u);
    for (u32 i = 0; i < l->quad_count; ++i) {
        de_vec4 p = l->quads[i].pos;
        EXPECT_FLOAT_EQ(p.x, roundf(p.x), 0);
        EXPECT_FLOAT_EQ(p.y, roundf(p.y), 0);
        EXPECT_FLOAT_EQ(p.z, roundf(p.z), 0);
        EXPECT_FLOAT_EQ(p.w, roundf(p.w), 0);
    }
    de_text_layout_free(l);
    const u8 bad[] = {'a', 0xff, 0xc0, 0x80, 'b'};
    d.utf8 = (de_str){(const char *)bad, sizeof bad};
    d.pixel_snap = false;
    ASSERT_EQ(de_text_layout_build(f->t, &d, NULL, &l), DE_OK);
    EXPECT_TRUE(l->size.x > 0);
    EXPECT_GT(l->cluster_count, 2u);
    de_text_layout_free(l);
}
TEST_F(paragraph, bidi_record_separators_and_invalid_utf8_do_not_overrun_levels, runtime_fx) {
    de_text_style style = {.font = f->latin, .size_px = 20, .color = {1, 1, 1, 1}};
    for (u32 separator = 0x1c; separator <= 0x1e; ++separator) {
        const u8 bytes[] = {'a', (u8)separator, 'b', (u8)separator, 0xff};
        for (u32 direction = 0; direction < 3; ++direction) {
            de_text_layout_desc desc = {.utf8 = {(const char *)bytes, sizeof bytes},
                                        .style = &style,
                                        .direction = (de_text_dir)direction};
            de_text_layout *l = NULL;
            ASSERT_EQ(de_text_layout_build(f->t, &desc, NULL, &l), DE_OK);
            EXPECT_EQ(l->line_count, 3u);
            EXPECT_TRUE(isfinite(l->size.x));
            EXPECT_TRUE(de_text_layout_hit_test(l, (de_vec2){100, 100}) <= sizeof bytes);
            de_text_layout_free(l);
        }
    }
    /* Exact text payload of the original libFuzzer crash. */
    const u8 bytes[] = {0x1e, 'a', 0xff, 'a', 0xff};
    de_text_layout_desc desc = {
        .utf8 = {(const char *)bytes, sizeof bytes}, .style = &style, .direction = DE_DIR_RTL};
    de_text_layout *l = NULL;
    ASSERT_EQ(de_text_layout_build(f->t, &desc, NULL, &l), DE_OK);
    EXPECT_EQ(l->line_count, 2u);
    de_text_layout_free(l);
}
TEST_F(paragraph, emergency_breaks_keep_grapheme_and_ligature_clusters, runtime_fx) {
    de_text_layout *l = layout_build(f,
                                     "a\xcc\x81"
                                     "bcdefghijklmnop",
                                     1, DE_ALIGN_START, DE_DIR_LTR, false);
    ASSERT_GT(l->line_count, 2u);
    for (u32 i = 0; i < l->cluster_count; ++i)
        if (l->clusters[i].start == 0)
            EXPECT_EQ(l->clusters[i].end, 3u);
    de_text_layout_free(l);
}
TEST_F(paragraph, immutable_cache_refresh_without_reflow, runtime_fx) {
    /* CJK U+4E00 is present in the fixture's outline source, not its cooked charset. */
    de_text_layout *before = layout_build(f,
                                          "A\xe4\xb8\x80"
                                          "B",
                                          0, DE_ALIGN_START, DE_DIR_LTR, false);
    EXPECT_EQ(before->quad_count, 2u);
    u32 count = before->cluster_count;
    de_vec2 size = before->size;
    de_text_layout *hit = layout_build(f,
                                       "A\xe4\xb8\x80"
                                       "B",
                                       0, DE_ALIGN_START, DE_DIR_LTR, false);
    EXPECT_TRUE(before == hit);
    de_text_layout_free(hit);
    de_text__glyph_jobs_wait(f->t);
    de_text_update(f->t);
    de_text_layout *after = layout_build(f,
                                         "A\xe4\xb8\x80"
                                         "B",
                                         0, DE_ALIGN_START, DE_DIR_LTR, false);
    EXPECT_TRUE(before != after);
    EXPECT_EQ(before->quad_count, 2u);
    EXPECT_EQ(after->quad_count, 3u);
    EXPECT_EQ(after->cluster_count, count);
    EXPECT_FLOAT_EQ(after->size.x, size.x, 0);
    EXPECT_FLOAT_EQ(after->size.y, size.y, 0);
    ASSERT_MEMEQ(before->clusters, after->clusters, count * sizeof *before->clusters);
    EXPECT_EQ(after->quads[1].font.id, f->cjk.id);
    de_text_layout_free(before);
    de_text_layout_free(after);
}
TEST_F(paragraph, cache_key_inputs_and_performance, runtime_fx) {
    char text[1601];
    u32 n = 0;
    u32 characters = 0;
    while (characters < 1000) {
        const char *word = "abc \xe4\xb8\x96\xe7\x95\x8c ";
        size_t len = strlen(word);
        memcpy(text + n, word, len);
        n += (u32)len;
        characters += 7;
    }
    text[n] = 0;
    de_text_style style = {.font = f->latin, .size_px = 20, .color = {1, 1, 1, 1}};
    de_text_layout_desc d = {.utf8 = {text, n}, .style = &style, .max_width = 500};
    de_text_layout *l;
    u64 start = de_time_now_ns();
    ASSERT_EQ(de_text_layout_build(f->t, &d, NULL, &l), DE_OK);
    u64 cold = de_time_now_ns() - start;
    de_text_layout *cached;
    start = de_time_now_ns();
    ASSERT_EQ(de_text_layout_build(f->t, &d, NULL, &cached), DE_OK);
    u64 hot = de_time_now_ns() - start;
    EXPECT_TRUE(l == cached);
    printf("1001-character mixed paragraph cold%.3fms cached%.3fus\n", (double)cold / 1e6,
           (double)hot / 1e3);
#if defined(DE_DEV)
    EXPECT_LE(cold, 500000u);
    EXPECT_LE(hot, 2000u);
#endif
    de_text_layout *steady;
    start = de_time_now_ns();
    ASSERT_EQ(de_text_layout_build(f->t, &d, NULL, &steady), DE_OK);
    u64 stable = de_time_now_ns() - start;
    EXPECT_TRUE(steady == cached);
    printf("stable cached %.3fus\n", (double)stable / 1e3);
#if defined(DE_DEV)
    EXPECT_LE(stable, 2000u);
#endif
    de_text_layout_free(steady);
    de_text_layout_free(cached);
    style.color.x = .5f;
    ASSERT_EQ(de_text_layout_build(f->t, &d, NULL, &cached), DE_OK);
    EXPECT_TRUE(l != cached);
    de_text_layout_free(cached);
    style.color.x = 1;
    de_text_set_fallbacks(f->t, f->latin, NULL, 0);
    ASSERT_EQ(de_text_layout_build(f->t, &d, NULL, &cached), DE_OK);
    EXPECT_TRUE(l != cached);
    de_text_layout_free(cached);
    de_text_layout_free(l);
}
typedef struct layout_threads {
    de_text_system *text;
    de_font font;
} layout_threads;
static i32 layout_thread(void *arg) {
    layout_threads *ctx = arg;
    de_text_style style = {.font = ctx->font, .size_px = 20, .color = {1, 1, 1, 1}};
    de_text_layout_desc desc = {
        .utf8 = DE_STR("Concurrent immutable layout"), .style = &style, .max_width = 150};
    for (u32 i = 0; i < 500; ++i) {
        de_text_layout *l = NULL;
        if (de_text_layout_build(ctx->text, &desc, NULL, &l) != DE_OK)
            return 1;
        if (de_text_layout_size(l).x <= 0) {
            de_text_layout_free(l);
            return 2;
        }
        de_text_layout_free(l);
    }
    return 0;
}
TEST_F(paragraph, concurrent_worker_layouts, runtime_fx) {
    layout_threads ctx = {f->t, f->latin};
    de_thread *threads[4];
    for (u32 i = 0; i < 4; ++i)
        ASSERT_EQ(
            de_thread_create(layout_thread, &ctx, "layout test", DE_THREAD_NORMAL, 0, &threads[i]),
            DE_OK);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_EQ(de_thread_join(threads[i]), 0);
}

TEST_F(paragraph, caller_arena_can_be_destroyed_after_layout_release, runtime_fx) {
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = 1u << 20}, &arena), DE_OK);
    de_allocator allocator = de_arena_allocator(arena);
    de_text_style style = {.font = f->latin, .size_px = 20, .color = {1, 1, 1, 1}};
    de_text_layout_desc desc = {.utf8 = DE_STR("Caller-owned layout storage"), .style = &style};
    de_text_layout *layout = NULL;
    ASSERT_EQ(de_text_layout_build(f->t, &desc, &allocator, &layout), DE_OK);
    EXPECT_TRUE(de_text_layout_size(layout).x > 0);
    de_text_layout_free(layout);
    de_arena_reset(arena);
    de_arena_destroy(arena);
    /* Neither eviction nor text-system destruction may retain the caller's arena. */
    de__text_layout_destroy(f->t);
    ASSERT_EQ(de__text_layout_init(f->t), DE_OK);
    ASSERT_EQ(de_text_layout_build(f->t, &desc, NULL, &layout), DE_OK);
    EXPECT_TRUE(de_text_layout_size(layout).x > 0);
    de_text_layout_free(layout);
}
