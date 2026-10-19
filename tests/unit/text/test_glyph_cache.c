#include "runtime_test_util.h"
#include "text/glyph_cache_internal.h"
static u32 unbaked(runtime_fx *f, u32 *glyphs, u32 max) {
    de_text_font *font = de_text__font_get(f->t, f->cjk);
    u32 count = 0;
    for (u32 cp = 0x4e00; cp < 0x4e00 + 550 && count < max; ++cp) {
        u32 gid = de_text__codepoint(font, cp);
        if (!gid)
            continue;
        bool baked = false;
        for (u32 j = 0; j < font->count; ++j)
            if (font->glyphs[j].index == gid)
                baked = true;
        if (!baked)
            glyphs[count++] = gid;
    }
    return count;
}
TEST_F(glyph_cache, cjk500_dedupe_and_frame_budget, runtime_fx) {
    u32 glyphs[500];
    ASSERT_EQ(unbaked(f, glyphs, 500), 500u);
    for (u32 i = 0; i < 500; ++i) {
        de_text_glyph_entry entry;
        EXPECT_FALSE(de_text__glyph(f->t, f->cjk, glyphs[i], &entry));
        EXPECT_TRUE(entry.missing);
        EXPECT_FALSE(de_text__glyph(f->t, f->cjk, glyphs[i], &entry));
    }
    de_text__glyph_jobs_wait(f->t);
    EXPECT_EQ(de_text_get_stats(f->t).background_jobs, 500u);
#if defined(__APPLE__)
    for (u32 i = 0; i < 500; ++i)
        EXPECT_TRUE(strncmp(f->t->glyph_cache->jobs[i].thread_name, "de-bg-", 6) == 0);
#endif
    u32 frames = 0;
    while (de_text_get_stats(f->t).runtime_pending && frames < 10) {
        de_text_stats before = de_text_get_stats(f->t);
        de_text_update(f->t);
        de_text_stats after = de_text_get_stats(f->t);
        EXPECT_LE(after.runtime_generated - before.runtime_generated, 64u);
        ++frames;
    }
    de_text_stats stats = de_text_get_stats(f->t);
    EXPECT_EQ(stats.runtime_generated, 500u);
    EXPECT_EQ(stats.runtime_pending, 0u);
    EXPECT_LE(frames, 10u);
    for (u32 i = 0; i < 500; ++i) {
        de_text_glyph_entry entry;
        ASSERT_TRUE(de_text__glyph(f->t, f->cjk, glyphs[i], &entry));
        EXPECT_FALSE(entry.missing);
    }
    printf("500 CJK generated in %u integration frames\n", frames);
}
TEST_F(glyph_cache, cook_runtime_byte_equal, runtime_fx) {
    de_text_font *font = de_text__font_get(f->t, f->cjk);
    u32 glyph = de_text__codepoint(font, 0x6f22);
    de__font_glyph *old = NULL;
    for (u32 i = 0; i < font->count; ++i)
        if (font->glyphs[i].index == glyph)
            old = &font->glyphs[i];
    ASSERT_TRUE(old != NULL);
    de_text__glyph_miss(f->t, f->cjk, glyph);
    de_text__glyph_jobs_wait(f->t);
    u32 generation = font->generation;
    de_text_update(f->t);
    EXPECT_GT(font->generation, generation);
    de_text_glyph_entry now;
    ASSERT_TRUE(de_text__glyph(f->t, f->cjk, glyph, &now));
    ASSERT_EQ(old->w, now.w);
    ASSERT_EQ(old->h, now.h);
    const u8 *page = de_text__page_data(f->t, f->cjk, now.page);
    ASSERT_TRUE(page != NULL);
    for (u32 y = 0; y < now.h; ++y)
        ASSERT_MEMEQ(font->pixels[old->page] + 4 * ((u64)(old->y + y) * font->width + old->x),
                     page + 4 * ((u64)(now.y + y) * font->width + now.x), now.w * 4);
}
TEST_F(glyph_cache, saturated_dirty_list_preserves_first_runtime_page_update, runtime_fx) {
    de_text_font *font = de_text__font_get(f->t, f->cjk);
    u32 glyph;
    ASSERT_EQ(unbaked(f, &glyph, 1), 1u);
    /* Simulate deferred uploads filling the list on already existing baked pages. */
    font->dirty_count = (u32)(sizeof font->dirty / sizeof *font->dirty);
    for (u32 i = 0; i < font->dirty_count; ++i)
        font->dirty[i] = (de_text_dirty_rect){i % font->pages, 0, 0, 1, 1};
    de_text__glyph_miss(f->t, f->cjk, glyph);
    de_text__glyph_jobs_wait(f->t);
    de_text_update(f->t);
    de_text_glyph_entry entry;
    ASSERT_TRUE(de_text__glyph(f->t, f->cjk, glyph, &entry));
    ASSERT_FALSE(entry.missing);
    ASSERT_GE(entry.page, font->pages);
    EXPECT_LE(font->dirty_count, (u32)(sizeof font->dirty / sizeof *font->dirty));
    for (u32 page = 0; page <= entry.page; ++page) {
        bool covered = false;
        for (u32 i = 0; i < font->dirty_count; ++i) {
            const de_text_dirty_rect *r = &font->dirty[i];
            if (r->page != page)
                continue;
            if (page < font->pages)
                covered |= r->x == 0 && r->y == 0 && r->w >= 1 && r->h >= 1;
            else
                covered |= r->x <= entry.x && r->y <= entry.y && r->x + r->w >= entry.x + entry.w &&
                           r->y + r->h >= entry.y + entry.h;
        }
        EXPECT_TRUE(covered);
    }
}
TEST(glyph_cache, all_baked_and_runtime_pages_remain_dirty_at_capacity) {
    de_text_font font = {.width = 128, .height = 64};
    for (u32 page = 0; page < DE_FONT_PAGES_MAX; ++page)
        de_text__font_dirty(&font, (de_text_dirty_rect){page, 1, 2, 3, 4});
    /* Repeated writes after saturation must retain coverage of every other page. */
    for (u32 repeat = 0; repeat < 2; ++repeat)
        for (u32 page = 0; page < DE_FONT_PAGES_MAX; ++page)
            de_text__font_dirty(&font, (de_text_dirty_rect){page, 20, 30, 5, 6});
    ASSERT_EQ(font.dirty_count, (u32)DE_FONT_PAGES_MAX);
    for (u32 page = 0; page < DE_FONT_PAGES_MAX; ++page) {
        bool covered = false;
        for (u32 i = 0; i < font.dirty_count; ++i) {
            const de_text_dirty_rect *r = &font.dirty[i];
            covered |= r->page == page && r->x == 0 && r->y == 0 && r->w == font.width &&
                       r->h == font.height;
        }
        EXPECT_TRUE(covered);
    }
}
TEST_F(glyph_cache, page_cap_warns_once, runtime_fx) {
    ASSERT_EQ(de_cvar_set(f->t->glyph_cache->pages_max, "0"), DE_OK);
    u32 ids[2];
    ASSERT_EQ(unbaked(f, ids, 2), 2u);
    for (u32 i = 0; i < 2; ++i)
        de_text__glyph_miss(f->t, f->cjk, ids[i]);
    de_text__glyph_jobs_wait(f->t);
    de_text_update(f->t);
    EXPECT_EQ(de_text_get_stats(f->t).cap_warnings, 1u);
    EXPECT_EQ(de_text_get_stats(f->t).runtime_pages, 0u);
    de_text_glyph_entry entry;
    EXPECT_TRUE(de_text__glyph(f->t, f->cjk, ids[0], &entry));
    EXPECT_FALSE(entry.missing);
    ASSERT_EQ(de_cvar_set(f->t->glyph_cache->pages_max, "8"), DE_OK);
}
typedef struct glyph_requests {
    de_text_system *t;
    de_font font;
    u32 ids[50];
} glyph_requests;
static i32 request_thread(void *user) {
    glyph_requests *r = user;
    for (u32 j = 0; j < 10; ++j)
        for (u32 i = 0; i < 50; ++i) {
            de_text_glyph_entry entry;
            (void)de_text__glyph(r->t, r->font, r->ids[i], &entry);
        }
    return 0;
}
TEST_F(glyph_cache, concurrent_request_dedupe, runtime_fx) {
    glyph_requests requests = {.t = f->t, .font = f->cjk};
    ASSERT_EQ(unbaked(f, requests.ids, 50), 50u);
    de_thread *threads[4];
    for (u32 i = 0; i < 4; ++i)
        ASSERT_EQ(de_thread_create(request_thread, &requests, "glyph requester", DE_THREAD_NORMAL,
                                   0, &threads[i]),
                  DE_OK);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_EQ(de_thread_join(threads[i]), 0);
    de_text__glyph_jobs_wait(f->t);
    de_text_update(f->t);
    EXPECT_EQ(de_text_get_stats(f->t).background_jobs, 50u);
    EXPECT_EQ(de_text_get_stats(f->t).runtime_generated, 50u);
}
TEST_F(glyph_cache, bitmap_does_not_enqueue, runtime_fx) {
    de_asset_id id = de_asset_id_from("font/bitmap");
    de_font font = de_asset_font(f->assets, id);
    de_load_handle h = de_asset_load(f->assets, id, 0);
    ASSERT_EQ(de_asset_wait(f->assets, h, 60ULL * 1000000000), (int)DE_LOAD_READY);
    de_text_glyph_entry entry;
    EXPECT_FALSE(de_text__glyph(f->t, font, 999, &entry));
    de_text__glyph_miss(f->t, font, 999);
    EXPECT_EQ(de_text_get_stats(f->t).runtime_pending, 0u);
    de_asset_release(f->assets, h);
}

typedef struct glyph_oom_context {
    runtime_fx *fixture;
    u32 glyph;
    u64 before_background, before_generated, pending, warnings, failures, bytes_live;
    u64 after_background, after_generated, after_pending;
    bool inline_pixels;
} glyph_oom_context;
static i32 external_glyph_oom(void *user) {
    glyph_oom_context *c = user;
    de_text__glyph_miss(c->fixture->t, c->fixture->cjk, c->glyph);
    return 0;
}
static i32 glyph_oom_owner(void *user) {
    glyph_oom_context *c = user;
    de_fp_env_init_thread();
    de_tracking_allocator tracking;
    de_allocator a = de_tracking_allocator_init(&tracking, NULL, DE_MEM_GENERAL);
    de_task_system *tasks = NULL;
    de_result r = de_task_system_create(
        &(de_task_system_desc){.allocator = &a, .worker_count = 1, .background_worker_count = 1},
        &tasks);
    if (r != DE_OK)
        return 1;
    struct de__glyph_cache *cache = c->fixture->t->glyph_cache;
    de_task_system *original = cache->tasks;
    cache->tasks = tasks;
    /* A fresh external slot has no records; only its allocator fails, not glyph memory. */
    de_tracking_allocator_fail_after(&tracking, 0);
    de_thread *external = NULL;
    i32 result = de_thread_create(external_glyph_oom, c, "glyph-external", DE_THREAD_NORMAL, 0,
                                  &external) == DE_OK
                     ? de_thread_join(external)
                     : 2;
    c->inline_pixels = cache->jobs[0].pixels != NULL;
    /* Repeated frame-owner retries must also defer while record allocation still fails. */
    de_text_update(c->fixture->t);
    de_text_update(c->fixture->t);
    de_text_stats before = de_text_get_stats(c->fixture->t);
    c->before_background = before.background_jobs;
    c->before_generated = before.runtime_generated;
    c->pending = before.runtime_pending;
    c->warnings = before.cap_warnings;
    c->failures = de_tracking_allocator_stats(&tracking).fail_count;
    de_tracking_allocator_fail_after(&tracking, -1);
    for (u32 i = 0; i < 4 && de_text_get_stats(c->fixture->t).runtime_pending; ++i) {
        de_text_update(c->fixture->t);
        de_text__glyph_jobs_wait(c->fixture->t);
    }
    de_text_update(c->fixture->t);
    de_text_stats after = de_text_get_stats(c->fixture->t);
    c->after_background = after.background_jobs;
    c->after_generated = after.runtime_generated;
    c->after_pending = after.runtime_pending;
    cache->tasks = original;
    de_task_system_destroy(tasks);
    c->bytes_live = de_tracking_allocator_stats(&tracking).bytes_live;
    return result;
}
TEST_F(glyph_cache, task_record_oom_on_external_thread_defers_msdf_and_recovers, runtime_fx) {
    glyph_oom_context context = {.fixture = f};
    ASSERT_EQ(unbaked(f, &context.glyph, 1), 1u);
    de_thread *owner = NULL;
    ASSERT_EQ(
        de_thread_create(glyph_oom_owner, &context, "glyph-owner", DE_THREAD_NORMAL, 0, &owner),
        DE_OK);
    EXPECT_EQ(de_thread_join(owner), 0);
    EXPECT_FALSE(context.inline_pixels);
    EXPECT_EQ(context.before_background, 0u);
    EXPECT_EQ(context.before_generated, 0u);
    EXPECT_EQ(context.pending, 1u);
    EXPECT_EQ(context.warnings, 0u);
    EXPECT_TRUE(context.failures >= 3);
    EXPECT_EQ(context.after_background, 1u);
    EXPECT_EQ(context.after_generated, 1u);
    EXPECT_EQ(context.after_pending, 0u);
    EXPECT_EQ(context.bytes_live, 0u);
    de_text_glyph_entry entry;
    EXPECT_TRUE(de_text__glyph(f->t, f->cjk, context.glyph, &entry));
    EXPECT_FALSE(entry.missing);
}
