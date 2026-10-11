/* Tests for the tracking allocator, memory tags, per-tag stats and the allocation hooks
 * (DENG-149). Each test registers its own tags, so traffic from other code (the heap reports in
 * Debug/Dev builds) never touches the numbers it checks. Trackers here sit on a TLSF over a
 * static buffer, which does not report, so the tracker itself is the reporting allocator. */
#include "de_test.h"

#include <dengine/de_memory.h>

#include <stdint.h>
#include <string.h>

static DE_ALIGN(16) u8 g_mem[1 << 20];

static de_allocator local_tlsf(void) {
    de_tlsf *t = NULL;
    if (de_tlsf_create(g_mem, sizeof g_mem, &t) != DE_OK)
        return *de_allocator_null();
    return de_tlsf_allocator(t);
}

/* ---- hooks recorder ---------------------------------------------------------------------- */

typedef struct hook_event {
    char kind; /* 'a' or 'f' */
    void *ptr;
    size_t size;
    u32 tag;
} hook_event;

typedef struct hook_log {
    u32 filter_tag;
    hook_event ev[64];
    u32 n;
    u32 other;                   /* events for other tags */
    const de_allocator *reenter; /* allocate from inside the hook */
} hook_log;

static void on_alloc(void *user, void *ptr, size_t size, u32 tag) {
    hook_log *l = (hook_log *)user;
    if (tag != l->filter_tag) {
        l->other++;
        return;
    }
    if (l->n < 64)
        l->ev[l->n++] = (hook_event){'a', ptr, size, tag};
    if (l->reenter != NULL) {
        /* An allocation inside a hook is counted but calls no hook (no recursion). */
        void *p = DE_ALLOC(l->reenter, 8, 8, tag);
        DE_FREE(l->reenter, p, 8);
    }
}

static void on_free(void *user, void *ptr, size_t size, u32 tag) {
    hook_log *l = (hook_log *)user;
    if (tag != l->filter_tag) {
        l->other++;
        return;
    }
    if (l->n < 64)
        l->ev[l->n++] = (hook_event){'f', ptr, size, tag};
}

/* ---- tests ------------------------------------------------------------------------------- */

TEST(tracking, counts) {
    de_allocator base = local_tlsf();
    de_tracking_allocator tr;
    const u32 tag = de_mem_register_tag("test.counts");
    de_allocator a = de_tracking_allocator_init(&tr, &base, tag);

    de_tracking_stats s = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(s.alloc_count, 0u);
    EXPECT_EQ(s.bytes_live, 0u);

    void *p1 = DE_ALLOC(&a, 100, 16, DE_MEM_ECS);
    void *p2 = DE_ALLOC(&a, 50, 16, DE_MEM_ECS);
    ASSERT_TRUE(p1 && p2);
    s = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(s.alloc_count, 2u);
    EXPECT_EQ(s.blocks_live, 2u);
    EXPECT_EQ(s.bytes_live, 150u);
    EXPECT_EQ(s.bytes_peak, 150u);

    p1 = DE_REALLOC(&a, p1, 100, 300, 16, DE_MEM_ECS);
    ASSERT_TRUE(p1 != NULL);
    s = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(s.realloc_count, 1u);
    EXPECT_EQ(s.bytes_live, 350u);
    EXPECT_EQ(s.bytes_peak, 350u);

    void *p3 = DE_REALLOC(&a, NULL, 0, 10, 16, DE_MEM_ECS); /* counts as an alloc */
    ASSERT_TRUE(p3 != NULL);
    DE_FREE(&a, p1, 300);
    DE_FREE(&a, p2, 50);
    DE_FREE(&a, NULL, 0); /* not counted */
    s = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(s.alloc_count, 3u);
    EXPECT_EQ(s.free_count, 2u);
    EXPECT_EQ(s.blocks_live, 1u);
    EXPECT_EQ(s.bytes_live, 10u);
    EXPECT_EQ(s.bytes_peak, 360u);
    EXPECT_EQ(s.fail_count, 0u);
    DE_FREE(&a, p3, 10);
    s = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(s.blocks_live, 0u);
    EXPECT_EQ(s.bytes_live, 0u);

    /* The tracker's tag replaces the caller's: the backing block carries `tag`. */
    const de_mem_stats ecs_before = de_mem_stats_for_tag(DE_MEM_ECS);
    void *p4 = DE_ALLOC(&a, 64, 16, DE_MEM_ECS);
    const de_mem_stats ecs_after = de_mem_stats_for_tag(DE_MEM_ECS);
    EXPECT_EQ(ecs_after.alloc_count, ecs_before.alloc_count);
    EXPECT_EQ(de_mem_stats_for_tag(tag).bytes_live, 64u);
    DE_FREE(&a, p4, 64);

    /* Backing failures are counted and keep the backing's error. */
    de_tracking_allocator tn;
    de_allocator an = de_tracking_allocator_init(&tn, de_allocator_null(), tag);
    de_error_clear();
    EXPECT_TRUE(DE_ALLOC(&an, 8, 8, 0) == NULL);
    EXPECT_TRUE(DE_REALLOC(&an, NULL, 0, 8, 8, 0) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_tracking_allocator_stats(&tn).fail_count, 2u);
    EXPECT_EQ(de_tracking_allocator_stats(&tn).alloc_count, 0u);
}

TEST(tracking, fail_after) {
    de_allocator base = local_tlsf();
    de_tracking_allocator tr;
    de_allocator a = de_tracking_allocator_init(&tr, &base, de_mem_register_tag("test.fail"));
    void *p[8];

    de_tracking_allocator_fail_after(&tr, 3);
    for (int i = 0; i < 3; ++i) {
        p[i] = DE_ALLOC(&a, 32, 16, 0);
        ASSERT_TRUE(p[i] != NULL);
    }
    de_error_clear();
    EXPECT_TRUE(DE_ALLOC(&a, 32, 16, 0) == NULL); /* the N+1th */
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(DE_ALLOC(&a, 32, 16, 0) == NULL); /* and every later one */
    /* A realloc is an allocation request too; the old block stays valid. */
    memset(p[0], 0x77, 32);
    EXPECT_TRUE(DE_REALLOC(&a, p[0], 32, 64, 16, 0) == NULL);
    EXPECT_EQ(((u8 *)p[0])[31], 0x77);
    /* Frees always work. */
    DE_FREE(&a, p[2], 32);
    de_tracking_stats s = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(s.alloc_count, 3u);
    EXPECT_EQ(s.fail_count, 3u);
    EXPECT_EQ(s.free_count, 1u);

    /* Re-arming counts from the call; 0 fails the very next request. */
    de_tracking_allocator_fail_after(&tr, 0);
    EXPECT_TRUE(DE_ALLOC(&a, 32, 16, 0) == NULL);
    de_tracking_allocator_fail_after(&tr, 1);
    p[2] = DE_ALLOC(&a, 32, 16, 0);
    EXPECT_TRUE(p[2] != NULL);
    EXPECT_TRUE(DE_ALLOC(&a, 32, 16, 0) == NULL);
    /* Disarmed: no limit. */
    de_tracking_allocator_fail_after(&tr, -1);
    for (int i = 3; i < 8; ++i) {
        p[i] = DE_ALLOC(&a, 32, 16, 0);
        ASSERT_TRUE(p[i] != NULL);
    }
    for (int i = 0; i < 8; ++i)
        DE_FREE(&a, p[i], 32);
    s = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(s.blocks_live, 0u);
    EXPECT_EQ(s.bytes_live, 0u);
}

TEST(tracking, hooks_called) {
    de_allocator base = local_tlsf();
    de_tracking_allocator tr;
    const u32 tag = de_mem_register_tag("test.hooks");
    de_allocator a = de_tracking_allocator_init(&tr, &base, tag);

    hook_log log;
    memset(&log, 0, sizeof log);
    log.filter_tag = tag;
    const de_mem_hooks hooks = {on_alloc, on_free, &log};
    const de_mem_hooks *prev = de_mem_set_hooks(&hooks);
    EXPECT_TRUE(prev == NULL);

    void *p = DE_ALLOC(&a, 40, 16, 0);
    void *q = DE_REALLOC(&a, p, 40, 400, 16, 0);
    DE_FREE(&a, q, 400);
    ASSERT_EQ(log.n, 4u);
    EXPECT_EQ(log.ev[0].kind, 'a');
    EXPECT_EQ(log.ev[0].ptr, p);
    EXPECT_EQ(log.ev[0].size, 40u);
    EXPECT_EQ(log.ev[0].tag, tag);
    EXPECT_EQ(log.ev[1].kind, 'f'); /* realloc: free of the old block first */
    EXPECT_EQ(log.ev[1].ptr, p);
    EXPECT_EQ(log.ev[1].size, 40u);
    EXPECT_EQ(log.ev[2].kind, 'a');
    EXPECT_EQ(log.ev[2].ptr, q);
    EXPECT_EQ(log.ev[2].size, 400u);
    EXPECT_EQ(log.ev[3].kind, 'f');
    EXPECT_EQ(log.ev[3].ptr, q);

    /* A failed realloc reports the old block back as allocated. */
    log.n = 0;
    p = DE_ALLOC(&a, 16, 16, 0);
    de_tracking_allocator_fail_after(&tr, 0);
    EXPECT_TRUE(DE_REALLOC(&a, p, 16, 32, 16, 0) == NULL); /* injected: reports nothing */
    EXPECT_EQ(log.n, 1u);
    de_tracking_allocator_fail_after(&tr, -1);
    EXPECT_TRUE(DE_REALLOC(&a, p, 16, (size_t)1 << 30, 16, 0) == NULL); /* backing fails */
    ASSERT_EQ(log.n, 3u);
    EXPECT_EQ(log.ev[1].kind, 'f');
    EXPECT_EQ(log.ev[2].kind, 'a');
    EXPECT_EQ(log.ev[2].ptr, p);
    EXPECT_EQ(de_mem_stats_for_tag(tag).bytes_live, 16u);

    /* A hook that allocates does not recurse; its allocation is still counted. */
    log.n = 0;
    log.reenter = &a;
    const u64 before = de_mem_stats_for_tag(tag).alloc_count;
    void *r = DE_ALLOC(&a, 24, 16, 0);
    EXPECT_EQ(log.n, 1u);
    EXPECT_EQ(de_mem_stats_for_tag(tag).alloc_count, before + 2);
    log.reenter = NULL;
    DE_FREE(&a, r, 24);
    DE_FREE(&a, p, 16);

    /* Chaining: a second installer gets the first hooks back and restores them on uninstall. */
    hook_log log2;
    memset(&log2, 0, sizeof log2);
    log2.filter_tag = tag;
    const de_mem_hooks hooks2 = {on_alloc, NULL, &log2};
    const de_mem_hooks *prev2 = de_mem_set_hooks(&hooks2);
    EXPECT_EQ(prev2, &hooks);
    log.n = 0;
    p = DE_ALLOC(&a, 8, 8, 0);
    DE_FREE(&a, p, 8); /* hooks2 has no on_free */
    EXPECT_EQ(log2.n, 1u);
    EXPECT_EQ(log.n, 0u);
    EXPECT_EQ(de_mem_set_hooks(prev2), &hooks2);
    p = DE_ALLOC(&a, 8, 8, 0);
    DE_FREE(&a, p, 8);
    EXPECT_EQ(log.n, 2u);
    EXPECT_EQ(de_mem_set_hooks(NULL), &hooks);

    /* Uninstalled: nothing is called. */
    log.n = 0;
    p = DE_ALLOC(&a, 8, 8, 0);
    DE_FREE(&a, p, 8);
    EXPECT_EQ(log.n, 0u);
}

TEST(tracking, tag_stats) {
    de_allocator base = local_tlsf();
    const u32 t1 = de_mem_register_tag("test.stats.a");
    const u32 t2 = de_mem_register_tag("test.stats.b");
    ASSERT_NE(t1, t2);
    de_tracking_allocator ta, tb;
    de_allocator a = de_tracking_allocator_init(&ta, &base, t1);
    de_allocator b = de_tracking_allocator_init(&tb, &base, t2);

    void *a1 = DE_ALLOC(&a, 1000, 16, 0);
    void *a2 = DE_ALLOC(&a, 500, 16, 0);
    void *b1 = DE_ALLOC(&b, 64, 16, 0);
    DE_FREE(&a, a1, 1000);
    void *a3 = DE_ALLOC(&a, 200, 16, 0);
    a2 = DE_REALLOC(&a, a2, 500, 2000, 16, 0); /* one free and one alloc */
    ASSERT_TRUE(a2 && a3 && b1);

    de_mem_stats s = de_mem_stats_for_tag(t1);
    EXPECT_EQ(s.alloc_count, 4u);
    EXPECT_EQ(s.free_count, 2u);
    EXPECT_EQ(s.bytes_live, 2200u);
    EXPECT_EQ(s.bytes_peak, 2200u); /* 1500, 500, 700, 200 (realloc free), 2200 */
    s = de_mem_stats_for_tag(t2);
    EXPECT_EQ(s.alloc_count, 1u);
    EXPECT_EQ(s.free_count, 0u);
    EXPECT_EQ(s.bytes_live, 64u);
    EXPECT_EQ(s.bytes_peak, 64u);

    DE_FREE(&a, a2, 2000);
    DE_FREE(&a, a3, 200);
    DE_FREE(&b, b1, 64);
    s = de_mem_stats_for_tag(t1);
    EXPECT_EQ(s.alloc_count, 4u);
    EXPECT_EQ(s.free_count, 4u);
    EXPECT_EQ(s.bytes_live, 0u);
    EXPECT_EQ(s.bytes_peak, 2200u); /* the peak stays */
    EXPECT_EQ(de_mem_stats_for_tag(t2).bytes_live, 0u);

    /* Unknown tags give zeros; out-of-range report tags count as general. */
    s = de_mem_stats_for_tag(DE_MEM_TAG_MAX + 5);
    EXPECT_EQ(s.alloc_count, 0u);
}

TEST(tracking, no_double_report) {
    /* Tracker on tracker: only the lower one reports, under the upper one's tag. */
    de_allocator base = local_tlsf();
    const u32 lower_tag = de_mem_register_tag("test.lower");
    const u32 upper_tag = de_mem_register_tag("test.upper");
    de_tracking_allocator lo, up;
    de_allocator l = de_tracking_allocator_init(&lo, &base, lower_tag);
    de_allocator u = de_tracking_allocator_init(&up, &l, upper_tag);
    void *p = DE_ALLOC(&u, 128, 16, 0);
    ASSERT_TRUE(p != NULL);
    /* The lower tracker forces its own tag on what it passes down and reports with it. */
    EXPECT_EQ(de_mem_stats_for_tag(lower_tag).alloc_count, 1u);
    EXPECT_EQ(de_mem_stats_for_tag(upper_tag).alloc_count, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&up).alloc_count, 1u);
    EXPECT_EQ(de_tracking_allocator_stats(&lo).alloc_count, 1u);
    DE_FREE(&u, p, 128);
    EXPECT_EQ(de_mem_stats_for_tag(lower_tag).free_count, 1u);

    /* Tracker on the heap: the heap reports in DE_MEM_TRACKING builds, the tracker otherwise.
     * Either way the block is counted once, under the tracker's tag. */
    const u32 heap_tag = de_mem_register_tag("test.heap");
    de_tracking_allocator th;
    de_allocator h = de_tracking_allocator_init(&th, NULL, heap_tag);
    void *q = DE_ALLOC(&h, 4000, 16, DE_MEM_ECS);
    ASSERT_TRUE(q != NULL);
    de_mem_stats s = de_mem_stats_for_tag(heap_tag);
    EXPECT_EQ(s.alloc_count, 1u);
    EXPECT_EQ(s.bytes_live, 4000u);
    q = DE_REALLOC(&h, q, 4000, 100, 16, DE_MEM_ECS);
    ASSERT_TRUE(q != NULL);
    s = de_mem_stats_for_tag(heap_tag);
    EXPECT_EQ(s.alloc_count, 2u);
    EXPECT_EQ(s.free_count, 1u);
    EXPECT_EQ(s.bytes_live, 100u);
    DE_FREE(&h, q, 100);
    s = de_mem_stats_for_tag(heap_tag);
    EXPECT_EQ(s.free_count, 2u);
    EXPECT_EQ(s.bytes_live, 0u);
    EXPECT_EQ(s.bytes_peak, 4000u);
}

TEST(tracking, heap_reports_in_tracking_builds) {
    const u32 tag = de_mem_register_tag("test.heap.direct");
    const de_allocator *h = de_heap_allocator();
    void *p = DE_ALLOC(h, 300, 16, tag);
    void *big = DE_ALLOC(h, DE_HEAP_LARGE_THRESHOLD + 1, 16, tag);
    ASSERT_TRUE(p && big);
    const de_mem_stats s = de_mem_stats_for_tag(tag);
#if DE_MEM_TRACKING
    EXPECT_EQ(s.alloc_count, 2u);
    EXPECT_EQ(s.bytes_live, 300u + DE_HEAP_LARGE_THRESHOLD + 1);
#else
    EXPECT_EQ(s.alloc_count, 0u);
#endif
    DE_FREE(h, p, 300);
    DE_FREE(h, big, DE_HEAP_LARGE_THRESHOLD + 1);
    const de_mem_stats e = de_mem_stats_for_tag(tag);
    EXPECT_EQ(e.bytes_live, 0u);
#if DE_MEM_TRACKING
    EXPECT_EQ(e.free_count, 2u); /* the free found the tag in the block headers */
#endif
}

/* Last in the file: it fills the process-wide tag table. */
TEST(tracking, tag_registry) {
    EXPECT_EQ(de_mem_register_tag("general"), (u32)DE_MEM_GENERAL);
    EXPECT_EQ(de_mem_register_tag("physics"), (u32)DE_MEM_PHYSICS);
    EXPECT_EQ(de_mem_register_tag(NULL), (u32)DE_MEM_GENERAL);
    EXPECT_EQ(de_mem_register_tag(""), (u32)DE_MEM_GENERAL);
    EXPECT_STREQ(de_mem_tag_name(DE_MEM_SCRATCH), "scratch");
    EXPECT_GE(de_mem_tag_count(), (u32)DE_MEM_TAG_BUILTIN_COUNT_);
    EXPECT_TRUE(de_mem_tag_name(DE_MEM_TAG_MAX) == NULL);

    const u32 t = de_mem_register_tag("test.registry");
    EXPECT_GE(t, (u32)DE_MEM_TAG_BUILTIN_COUNT_);
    EXPECT_EQ(de_mem_register_tag("test.registry"), t);
    EXPECT_STREQ(de_mem_tag_name(t), "test.registry");

    /* Names are cut to 31 bytes, and the long name finds its cut copy. */
    const char *longname = "a-very-long-memory-tag-name-that-goes-on-and-on";
    const u32 tl = de_mem_register_tag(longname);
    EXPECT_EQ(strlen(de_mem_tag_name(tl)), 31u);
    EXPECT_EQ(de_mem_register_tag(longname), tl);

    char name[32];
    u32 last = 0;
    for (u32 i = de_mem_tag_count(); i < DE_MEM_TAG_MAX; ++i) {
        name[0] = 'x';
        name[1] = (char)('a' + i / 26);
        name[2] = (char)('a' + i % 26);
        name[3] = '\0';
        last = de_mem_register_tag(name);
        EXPECT_EQ(last, i);
    }
    EXPECT_EQ(de_mem_tag_count(), (u32)DE_MEM_TAG_MAX);
    de_error_clear();
    EXPECT_EQ(de_mem_register_tag("one-too-many"), (u32)DE_MEM_GENERAL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_mem_register_tag("test.registry"), t); /* lookups still work */
    (void)last;
}
