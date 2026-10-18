/* test_command_queue.c: the lock-free command path (DENG-308). Many producer threads, overflow
 * accounting, and a queue that never blocks. Label `tsan`: CI runs it under ThreadSanitizer. */
#include "audio_test_util.h"

#define PRODUCERS    8u
#define PER_PRODUCER 20000u

typedef struct apply_log {
    u64 next[PRODUCERS]; /* audio thread: next expected sequence per producer */
    u64 applied;
    u64 out_of_order;
} apply_log;

static void count_fn(de_audio *a, void *user, u64 arg, u64 sample) {
    (void)a;
    (void)sample;
    apply_log *l = (apply_log *)user;
    const u32 p = (u32)(arg >> 32);
    const u64 seq = arg & 0xFFFFFFFFu;
    /* Per producer, accepted commands arrive in push order (sequence numbers only grow; the
     * dropped ones leave gaps). */
    if (seq < l->next[p])
        ++l->out_of_order;
    l->next[p] = seq + 1;
    ++l->applied;
}

typedef struct producer {
    de_audio *a;
    apply_log *alog;
    u32 id;
    u32 count;
    de_sem *go;
    de_atomic_u32 *done;
    u64 accepted;
    u64 dropped;
} producer;

static i32 producer_main(void *user) {
    producer *p = (producer *)user;
    de_sem_wait(p->go); /* all producers start together */
    for (u32 i = 0; i < p->count; ++i) {
        const u64 arg = ((u64)p->id << 32) | i;
        if (de__audio_post_fn(p->a, count_fn, p->alog, arg, 0))
            ++p->accepted;
        else
            ++p->dropped;
    }
    de_atomic_fetch_add_u32(p->done, 1, DE_ATOMIC_RELEASE);
    return 0;
}

static void run_producers(de_audio *a, apply_log *alog, producer *p, u32 n, u32 count, bool pump) {
    de_sem go;
    ASSERT_EQ(de_sem_init(&go, 0), DE_OK);
    de_atomic_u32 done;
    de_atomic_init_u32(&done, 0);
    de_thread *th[PRODUCERS];
    for (u32 i = 0; i < n; ++i) {
        memset(&p[i], 0, sizeof p[i]);
        p[i].a = a;
        p[i].alog = alog;
        p[i].id = i;
        p[i].count = count;
        p[i].go = &go;
        p[i].done = &done;
        ASSERT_EQ(de_thread_create(producer_main, &p[i], "producer", DE_THREAD_NORMAL, 0, &th[i]),
                  DE_OK);
    }
    de_sem_post(&go, n);
    /* This thread is the audio thread meanwhile: it renders blocks, draining the queue. */
    while (pump && de_atomic_load_u32(&done, DE_ATOMIC_ACQUIRE) < n)
        de__audio_null_pump(a, 256, NULL);
    for (u32 i = 0; i < n; ++i)
        de_thread_join(th[i]);
    de_sem_destroy(&go);
}

TEST(audio_cmdq, mpsc_8_producers_all_applied) {
    de_audio *a = test_audio_manual(16, 1024);
    static apply_log alog;
    memset(&alog, 0, sizeof alog);
    static producer p[PRODUCERS];
    run_producers(a, &alog, p, PRODUCERS, PER_PRODUCER, true);
    u64 accepted = 0, dropped = 0;
    for (u32 i = 0; i < PRODUCERS; ++i) {
        EXPECT_EQ(p[i].accepted + p[i].dropped, (u64)PER_PRODUCER);
        accepted += p[i].accepted;
        dropped += p[i].dropped;
    }
    /* Drain what is left: one block takes up to a queue's worth. */
    de__audio_null_pump(a, 512, NULL);
    EXPECT_EQ(alog.applied, accepted);
    EXPECT_EQ(alog.out_of_order, 0u);
    EXPECT_EQ((u64)de_audio_get_stats(a).commands_dropped, dropped);
    EXPECT_TRUE(accepted > 0u);
    de_audio_destroy(a);
}

TEST(audio_cmdq, overflow_counted_never_blocks) {
    de_audio *a = test_audio_manual(16, 64);
    static apply_log alog;
    memset(&alog, 0, sizeof alog);
    /* Nobody drains: exactly the capacity is accepted, the rest is dropped and counted. */
    u32 ok = 0;
    for (u32 i = 0; i < 1000; ++i)
        ok += de__audio_post_fn(a, count_fn, &alog, i, 0) ? 1u : 0u;
    EXPECT_EQ(ok, 64u);
    EXPECT_EQ(de_audio_get_stats(a).commands_dropped, 936u);
    de__audio_null_pump(a, 256, NULL);
    EXPECT_EQ(alog.applied, 64u);

    /* 8 threads against a full queue with no consumer: every call returns, all counted. */
    memset(&alog, 0, sizeof alog);
    for (u32 i = 0; i < 64; ++i)
        ASSERT_TRUE(de__audio_post_fn(a, count_fn, &alog, i, 0));
    static producer p[PRODUCERS];
    run_producers(a, &alog, p, PRODUCERS, 5000, false);
    u64 accepted = 0;
    for (u32 i = 0; i < PRODUCERS; ++i)
        accepted += p[i].accepted;
    EXPECT_EQ(accepted, 0u);
    EXPECT_EQ(de_audio_get_stats(a).commands_dropped, 936u + PRODUCERS * 5000u);
    de__audio_null_pump(a, 256, NULL);
    EXPECT_EQ(alog.applied, 64u);
    de_audio_destroy(a);
}

TEST(audio_cmdq, play_on_full_queue_gives_slot_back) {
    de_audio *a = test_audio_manual(2, 4);
    static apply_log alog;
    memset(&alog, 0, sizeof alog);
    for (u32 i = 0; i < 4; ++i)
        ASSERT_TRUE(de__audio_post_fn(a, count_fn, &alog, i, 0));
    test_source s1, s2, s3;
    test_src_init(&s1, TSRC_CONST, 0.5f, 0, 1);
    test_src_init(&s2, TSRC_CONST, 0.5f, 0, 1);
    test_src_init(&s3, TSRC_CONST, 0.5f, 0, 1);
    de_voice v1 = test_play(a, &s1, 0, false);
    EXPECT_FALSE(DE_HANDLE_VALID(v1));
    EXPECT_EQ(de_error_last()->result, DE_ERR_BUSY);
    EXPECT_EQ(test_src_closes(&s1), 1u); /* the caller side closed it */
    de__audio_null_pump(a, 256, NULL);
    /* Both slots are still free. */
    de_voice v2 = test_play(a, &s2, 0, false);
    de_voice v3 = test_play(a, &s3, 0, false);
    EXPECT_TRUE(DE_HANDLE_VALID(v2));
    EXPECT_TRUE(DE_HANDLE_VALID(v3));
    f32 cap[256 * 2];
    de__audio_null_pump(a, 256, cap);
    EXPECT_FLOAT_EQ(cap[0], 0.70710678118f, 0.0);
    de_audio_destroy(a);
    EXPECT_EQ(test_src_closes(&s2), 1u);
    EXPECT_EQ(test_src_closes(&s3), 1u);
}

/* Producers that play and stop voices while the audio thread mixes: handles, slots and source
 * hand-offs across threads (the TSan part). */
typedef struct churner {
    de_audio *a;
    test_source src[16];
    de_sem *go;
    u64 plays;
} churner;

static i32 churner_main(void *user) {
    churner *c = (churner *)user;
    de_sem_wait(c->go);
    de_voice v[16];
    memset(v, 0, sizeof v);
    for (u32 rnd = 0; rnd < 400; ++rnd) {
        for (u32 i = 0; i < 16; ++i) {
            if (DE_HANDLE_VALID(v[i])) {
                de_audio_stop(c->a, v[i], 0.0f);
                v[i].id = 0;
            } else if (c->src[i].base.vt == NULL || test_src_closes(&c->src[i]) > 0) {
                test_src_init(&c->src[i], TSRC_CONST, 0.01f, (rnd & 1) ? 100 : 0, 1);
                v[i] = test_play(c->a, &c->src[i], 0, false);
                if (DE_HANDLE_VALID(v[i]))
                    ++c->plays;
                else
                    c->src[i].base.vt = NULL;
            }
        }
    }
    for (u32 i = 0; i < 16; ++i)
        if (DE_HANDLE_VALID(v[i]))
            de_audio_stop(c->a, v[i], 0.0f);
    return 0;
}

TEST(audio_cmdq, concurrent_play_stop_churn) {
    de_audio *a = test_audio_realtime(64, 256);
    static churner c[4];
    de_sem go;
    ASSERT_EQ(de_sem_init(&go, 0), DE_OK);
    de_thread *th[4];
    for (u32 i = 0; i < 4; ++i) {
        memset(&c[i], 0, sizeof c[i]);
        c[i].a = a;
        c[i].go = &go;
        ASSERT_EQ(de_thread_create(churner_main, &c[i], "churn", DE_THREAD_NORMAL, 0, &th[i]),
                  DE_OK);
    }
    de_sem_post(&go, 4);
    for (u32 i = 0; i < 4; ++i)
        de_thread_join(th[i]);
    de_sem_destroy(&go);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_TRUE(c[i].plays > 0u);
    de_audio_destroy(a);
    /* Every source that was handed to the system was closed exactly once. */
    for (u32 i = 0; i < 4; ++i)
        for (u32 k = 0; k < 16; ++k)
            if (c[i].src[k].base.vt != NULL)
                EXPECT_EQ(test_src_closes(&c[i].src[k]), 1u);
}
