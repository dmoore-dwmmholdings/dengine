/* test_mixer.c: mixing, stats, create/destroy, ECS attach, and the real-time guard with the alloc
 * guard: no allocation and no lock on the audio thread (DENG-308). */
#include "audio_test_util.h"
#include "de_alloc_guard.h"

#include <dengine/de_reflect.h>

#include <math.h>
#include <stdlib.h>

#define CH 2u

TEST(audio_mixer, silence) {
    de_audio *a = test_audio_manual(8, 0);
    f32 cap[2560 * CH];
    for (u32 i = 0; i < 2560 * CH; ++i)
        cap[i] = 1.0f;
    de__audio_null_pump(a, 2560, cap);
    for (u32 i = 0; i < 2560 * CH; ++i)
        if (cap[i] != 0.0f) {
            EXPECT_EQ(i, (u32)UINT32_MAX);
            break;
        }
    de_audio_stats s = de_audio_get_stats(a);
    EXPECT_EQ(s.voices_real, 0u);
    EXPECT_EQ(s.voices_virtual, 0u);
    EXPECT_EQ(s.underruns, 0u);
    EXPECT_EQ(s.commands_dropped, 0u);
    EXPECT_EQ(de_audio_clock(a), 2560u);
    EXPECT_EQ(test_frames_mixed(a), 2560u);
    de_audio_destroy(a);
}

TEST(audio_mixer, single_source_then_end) {
    de_audio *a = test_audio_manual(8, 0);
    test_source s;
    test_src_init(&s, TSRC_CONST, 0.25f, 1000, 1);
    de_voice v = test_play(a, &s, 0, false);
    ASSERT_TRUE(DE_HANDLE_VALID(v));
    f32 cap[1024 * CH];
    de__audio_null_pump(a, 512, cap);
    EXPECT_EQ(de_audio_get_stats(a).voices_real, 1u);
    de__audio_null_pump(a, 512, cap + 512 * CH);
    for (u32 i = 0; i < 1024; ++i) {
        const f32 want = i < 1000 ? 0.25f : 0.0f;
        if (cap[i * CH] != want || cap[i * CH + 1] != want) {
            EXPECT_EQ(i, (u32)UINT32_MAX);
            break;
        }
    }
    EXPECT_FALSE(de_audio_playing(a, v));
    EXPECT_EQ(de_audio_get_stats(a).voices_real, 0u);
    EXPECT_EQ(test_src_wait_closed(a, &s), 1u);
    de_audio_destroy(a);
    EXPECT_EQ(test_src_closes(&s), 1u);
}

TEST(audio_mixer, channel_mapping) {
    de_audio *a = test_audio_manual(8, 0);
    test_source mono, stereo, quad;
    test_src_init(&mono, TSRC_CONST, 0.5f, 256, 1);     /* L = R = 0.5 */
    test_src_init(&stereo, TSRC_CONST, 0.25f, 256, 2);  /* L 0.25, R 0.5 */
    test_src_init(&quad, TSRC_CONST, 0.03125f, 256, 4); /* first two: 0.03125, 0.0625 */
    test_play(a, &mono, 0, false);
    test_play(a, &stereo, 0, false);
    test_play(a, &quad, 0, false);
    f32 cap[256 * CH];
    de__audio_null_pump(a, 256, cap);
    for (u32 i = 0; i < 256; ++i) {
        EXPECT_FLOAT_EQ(cap[i * CH], 0.5 + 0.25 + 0.03125, 0.0);
        EXPECT_FLOAT_EQ(cap[i * CH + 1], 0.5 + 0.5 + 0.0625, 0.0);
    }
    /* Invalid sources are refused and closed. */
    test_source bad;
    test_src_init(&bad, TSRC_CONST, 1.0f, 0, 9);
    EXPECT_FALSE(DE_HANDLE_VALID(test_play(a, &bad, 0, false)));
    EXPECT_EQ(test_src_closes(&bad), 1u);
    de_audio_destroy(a);
}

TEST(audio_mixer, many_voices_sum) {
    de_audio *a = test_audio_manual(128, 0);
    static test_source src[64];
    for (u32 i = 0; i < 64; ++i) {
        test_src_init(&src[i], TSRC_CONST, 0.0078125f, 0, 1); /* 1/128: exact sums */
        ASSERT_TRUE(DE_HANDLE_VALID(test_play(a, &src[i], 0, false)));
    }
    f32 cap[512 * CH];
    de__audio_null_pump(a, 512, cap);
    EXPECT_FLOAT_EQ(cap[0], 0.5, 0.0);
    EXPECT_FLOAT_EQ(cap[511 * CH + 1], 0.5, 0.0);
    EXPECT_EQ(de_audio_get_stats(a).voices_real, 64u);
    de_audio_destroy(a);
    for (u32 i = 0; i < 64; ++i)
        EXPECT_EQ(test_src_closes(&src[i]), 1u);
}

TEST(audio_mixer, slots_exhausted_and_reused) {
    de_audio *a = test_audio_manual(4, 0);
    test_source s[6];
    de_voice v[6];
    for (u32 i = 0; i < 4; ++i) {
        test_src_init(&s[i], TSRC_CONST, 0.1f, 0, 1);
        v[i] = test_play(a, &s[i], 0, false);
        ASSERT_TRUE(DE_HANDLE_VALID(v[i]));
    }
    test_src_init(&s[4], TSRC_CONST, 0.1f, 0, 1);
    v[4] = test_play(a, &s[4], 0, false);
    EXPECT_FALSE(DE_HANDLE_VALID(v[4]));
    EXPECT_EQ(de_error_last()->result, DE_ERR_BUSY);
    EXPECT_EQ(test_src_closes(&s[4]), 1u); /* refused sources are closed */

    de_audio_stop(a, v[1], 0.0f);
    de__audio_null_pump(a, 256, NULL);
    EXPECT_FALSE(de_audio_playing(a, v[1]));
    /* The control thread may be collecting it right now; once closed, the slot is free. */
    EXPECT_EQ(test_src_wait_closed(a, &s[1]), 1u);
    /* The slot comes back (play collects ended voices itself) with a new generation. */
    test_src_init(&s[5], TSRC_CONST, 0.1f, 0, 1);
    v[5] = test_play(a, &s[5], 0, false);
    ASSERT_TRUE(DE_HANDLE_VALID(v[5]));
    EXPECT_EQ(de__voice_slot(v[5]), de__voice_slot(v[1]));
    EXPECT_NE(v[5].id, v[1].id);
    EXPECT_FALSE(de_audio_playing(a, v[1])); /* stale stays stale */
    EXPECT_TRUE(de_audio_playing(a, v[5]));
    de_audio_stop(a, v[1], 0.0f); /* stale: ignored, v[5] keeps playing */
    de__audio_null_pump(a, 256, NULL);
    EXPECT_TRUE(de_audio_playing(a, v[5]));
    EXPECT_FALSE(de_audio_playing(a, (de_voice){0}));
    EXPECT_FALSE(de_audio_playing(a, (de_voice){0xFFFFFFFFu}));
    de_audio_destroy(a);
    for (u32 i = 0; i < 6; ++i)
        EXPECT_EQ(test_src_closes(&s[i]), 1u);
}

TEST(audio_mixer, destroy_closes_every_source_once) {
    de_audio *a = test_audio_manual(16, 0);
    test_source s[5];
    for (u32 i = 0; i < 5; ++i)
        test_src_init(&s[i], TSRC_CONST, 0.1f, i == 2 ? 10 : 0, 1);
    test_play(a, &s[0], 0, false);      /* playing */
    test_play(a, &s[1], 100000, false); /* scheduled (pending) */
    test_play(a, &s[2], 0, false);      /* ends in the first block, not collected */
    de__audio_null_pump(a, 256, NULL);
    test_play(a, &s[3], 0, false); /* still in the command queue */
    de_voice v4 = test_play(a, &s[4], 0, false);
    de_audio_stop(a, v4, 0.0f); /* stopped before it started */
    de_audio_destroy(a);
    for (u32 i = 0; i < 5; ++i)
        EXPECT_EQ(test_src_closes(&s[i]), 1u);
}

TEST(audio_mixer, create_settings) {
    de_audio *a = NULL;
    de_audio_desc d;
    memset(&d, 0, sizeof d);
    /* Defaults, headless: the OS backend is not in this build yet (or falls back to null). */
    ASSERT_EQ(de_audio_create(&d, &a), DE_OK);
    EXPECT_EQ(a->sample_rate, 48000u);
    EXPECT_EQ(a->block_frames, 256u);
    EXPECT_EQ(a->voice_count, 128u);
    EXPECT_EQ(a->command_capacity, 4096u);
    EXPECT_EQ(a->ring.blocks, 3u);
    EXPECT_TRUE(de_audio_list_devices(a, NULL, 0) >= 1u);
    de_audio_destroy(a);

    d.backend = DE_AUDIO_NULL;
    d.sample_rate = 44100;
    d.block_frames = 64;
    d.command_capacity = 1000; /* rounded up */
    ASSERT_EQ(de_audio_create(&d, &a), DE_OK);
    EXPECT_EQ(a->command_capacity, 1024u);
    de_audio_destroy(a);

    memset(&d, 0, sizeof d);
    d.sample_rate = 1000;
    EXPECT_EQ(de_audio_create(&d, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(a == NULL);
    memset(&d, 0, sizeof d);
    d.block_frames = 8;
    EXPECT_EQ(de_audio_create(&d, &a), DE_ERR_INVALID_ARGUMENT);
    memset(&d, 0, sizeof d);
    d.max_voices = 5000;
    EXPECT_EQ(de_audio_create(&d, &a), DE_ERR_INVALID_ARGUMENT);
    memset(&d, 0, sizeof d);
    d.backend = (de_audio_backend)99;
    EXPECT_EQ(de_audio_create(&d, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_audio_create(NULL, &a), DE_ERR_INVALID_ARGUMENT);
    /* A backend that is never built for this OS. */
    memset(&d, 0, sizeof d);
#if defined(_WIN32)
    d.backend = DE_AUDIO_COREAUDIO;
#else
    d.backend = DE_AUDIO_WASAPI;
#endif
    EXPECT_EQ(de_audio_create(&d, &a), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(a == NULL);
    de_audio_destroy(NULL);
}

TEST(audio_mixer, create_out_of_memory_no_leak) {
    de_tracking_allocator t;
    de_allocator al = de_tracking_allocator_init(&t, NULL, DE_MEM_AUDIO);
    de_audio_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &al;
    d.backend = DE_AUDIO_NULL;
    bool created = false;
    for (i64 n = 0; n < 64 && !created; ++n) {
        de_tracking_allocator_fail_after(&t, n);
        de_audio *a = NULL;
        de_result r = de_audio_create(&d, &a);
        if (r == DE_OK) {
            created = true;
            de_tracking_allocator_fail_after(&t, -1);
            de_audio_destroy(a);
        } else {
            EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
            EXPECT_TRUE(a == NULL);
        }
        de_tracking_stats st = de_tracking_allocator_stats(&t);
        EXPECT_EQ(st.blocks_live, 0u);
        EXPECT_EQ(st.bytes_live, 0u);
    }
    EXPECT_TRUE(created);
}

TEST(audio_mixer, stats_dsp_load_and_underruns) {
    de_audio *a = test_audio_realtime(16, 0);
    test_source s;
    test_src_init(&s, TSRC_CONST, 0.1f, 0, 1);
    test_play(a, &s, 0, false);
    ASSERT_TRUE(test_wait(a, test_pred_mixed_at_least, 48000 / 4, 5000));
    de_audio_stats st = de_audio_get_stats(a);
    EXPECT_TRUE(st.dsp_load > 0.0f);
    EXPECT_TRUE(isfinite(st.dsp_load));
    EXPECT_EQ(st.voices_real, 1u);

    /* Stall the mixer: the device keeps reading, finds the ring empty and counts underruns. */
    const u32 before = de_audio_get_stats(a).underruns;
    de_atomic_store_u32(&a->test_stall, 1, DE_ATOMIC_RELAXED);
    de_sleep_ns(100000000ull);
    EXPECT_TRUE(de_audio_get_stats(a).underruns > before);
    const u64 mixed = test_frames_mixed(a);
    de_atomic_store_u32(&a->test_stall, 0, DE_ATOMIC_RELAXED);
    /* The mixer resumes. */
    EXPECT_TRUE(test_wait(a, test_pred_mixed_at_least, mixed + 4800, 5000));
    de_audio_destroy(a);
}

/* ---- ECS attach ------------------------------------------------------------------------------ */

TEST(audio_mixer, attach_inserts_resource) {
    de_type_registry *types = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = types;
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&wd, &w), DE_OK);
    EXPECT_TRUE(de_world_types(w) == types);
    de_audio *a = test_audio_manual(4, 0);
    ASSERT_EQ(de_audio_attach(a, w), DE_OK);
    de_type_id t = de_type_find(types, "de_audio_resource");
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    de_resource_id id = de_resource_find(w, t);
    ASSERT_TRUE(DE_HANDLE_VALID(id));
    const de_audio_resource *res = (const de_audio_resource *)de_resource_get(w, id);
    ASSERT_TRUE(res != NULL);
    EXPECT_TRUE(res->audio == a);
    /* Attaching again (another system) replaces the value, same id. */
    de_audio *b = test_audio_manual(4, 0);
    ASSERT_EQ(de_audio_attach(b, w), DE_OK);
    EXPECT_EQ(de_resource_find(w, t).id, id.id);
    EXPECT_TRUE(((const de_audio_resource *)de_resource_get(w, id))->audio == b);
    EXPECT_EQ(de_audio_attach(NULL, w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_audio_attach(a, NULL), DE_ERR_INVALID_ARGUMENT);
    de_audio_destroy(b);
    de_audio_destroy(a);
    de_world_destroy(w);
    de_type_registry_destroy(types);
}

/* ---- Real-time guard ------------------------------------------------------------------------- */

static void alloc_on_audio_thread(de_audio *a, void *user, u64 arg, u64 sample) {
    (void)a;
    (void)user;
    (void)arg;
    (void)sample;
    const de_allocator *h = de_heap_allocator();
    void *p = DE_ALLOC(h, 64, 16, DE_MEM_AUDIO);
    DE_FREE(h, p, 64);
}

static void lock_on_audio_thread(de_audio *a, void *user, u64 arg, u64 sample) {
    (void)a;
    (void)arg;
    (void)sample;
    de_mutex *m = (de_mutex *)user;
    de_mutex_lock(m);
    de_mutex_unlock(m);
}

TEST(audio_mixer, rt_guard_detects_alloc_and_lock) {
    if (!de__rt_guard_enabled())
        SKIP("the real-time guard is compiled in Debug and Dev builds only");
    de_audio *a = test_audio_manual(4, 0);
    de_mutex m;
    de_mutex_init(&m);
    de__rt_guard_set_fatal(false);
    const u64 h0 = de__rt_guard_hits();
    /* Outside the audio thread nothing counts. */
    alloc_on_audio_thread(a, NULL, 0, 0);
    lock_on_audio_thread(a, &m, 0, 0);
    EXPECT_EQ(de__rt_guard_hits(), h0);
    de__audio_post_fn(a, alloc_on_audio_thread, NULL, 0, 0);
    de__audio_null_pump(a, 256, NULL);
    const u64 h1 = de__rt_guard_hits();
    de__audio_post_fn(a, lock_on_audio_thread, &m, 0, 0);
    de__audio_null_pump(a, 256, NULL);
    const u64 h2 = de__rt_guard_hits();
    de__rt_guard_set_fatal(true);
    /* The heap block: its alloc and its free (and the heap's own lock, if it takes one). */
    EXPECT_TRUE(h1 - h0 >= 2u);
    EXPECT_EQ(h2 - h1, 1u); /* the lock */
    EXPECT_FALSE(de__rt_guard_active());
    de_mutex_destroy(&m);
    de_audio_destroy(a);
}

/* Constant command traffic from producer threads, one burst per tick. Sources come from a
 * per-producer pool and are reused only after the audio system closed them. */
#define TRAFFIC_POOL 8u
#define TRAFFIC_OPS  12u

typedef struct traffic {
    de_audio *a;
    de_sem tick;
    de_sem done; /* posted after each burst: the main thread keeps the producers in step */
    de_atomic_u32 quit;
    de_atomic_u32 ready;
    de_atomic_u64 ops;
    test_source pool[TRAFFIC_POOL];
    de_voice voice[TRAFFIC_POOL];
    u32 seed;
} traffic;

static void nop_fn(de_audio *a, void *user, u64 arg, u64 sample) {
    (void)a;
    (void)user;
    (void)arg;
    (void)sample;
}

static void traffic_burst(traffic *t) {
    for (u32 k = 0; k < TRAFFIC_OPS; ++k) {
        t->seed = t->seed * 1664525u + 1013904223u;
        const u32 i = (t->seed >> 8) % TRAFFIC_POOL;
        test_source *s = &t->pool[i];
        if (DE_HANDLE_VALID(t->voice[i])) {
            de_audio_stop(t->a, t->voice[i], 0.0f);
            t->voice[i].id = 0;
        } else if (test_src_closes(s) > 0 || s->base.vt == NULL) {
            /* Free (never used, or closed by the audio system): play it again. */
            test_src_init(s, TSRC_CONST, 0.001f, (t->seed & 1) != 0 ? 0 : 300, 1);
            t->voice[i] = test_play(t->a, s, 0, (t->seed & 2) != 0);
            if (!DE_HANDLE_VALID(t->voice[i]))
                s->base.vt = NULL; /* refused and closed: free again */
        }
        de__audio_post_fn(t->a, nop_fn, NULL, k, 0);
        de_atomic_fetch_add_u64(&t->ops, 1, DE_ATOMIC_RELAXED);
    }
}

static i32 traffic_main(void *user) {
    traffic *t = (traffic *)user;
    traffic_burst(t); /* warm up: first use of everything on this thread */
    de_atomic_fetch_add_u32(&t->ready, 1, DE_ATOMIC_RELEASE);
    for (;;) {
        de_sem_wait(&t->tick);
        if (de_atomic_load_u32(&t->quit, DE_ATOMIC_ACQUIRE) != 0)
            break;
        traffic_burst(t);
        de_sem_post(&t->done, 1);
    }
    return 0;
}

static void traffic_start(traffic *t, de_audio *a, u32 seed, de_thread **th) {
    memset(t, 0, sizeof *t);
    t->a = a;
    t->seed = seed;
    ASSERT_EQ(de_sem_init(&t->tick, 0), DE_OK);
    ASSERT_EQ(de_sem_init(&t->done, 0), DE_OK);
    de_atomic_init_u32(&t->quit, 0);
    de_atomic_init_u32(&t->ready, 0);
    de_atomic_init_u64(&t->ops, 0);
    ASSERT_EQ(de_thread_create(traffic_main, t, "traffic", DE_THREAD_NORMAL, 0, th), DE_OK);
}

static void traffic_stop(traffic *t, de_thread *th) {
    de_atomic_store_u32(&t->quit, 1, DE_ATOMIC_RELEASE);
    de_sem_post(&t->tick, 1);
    de_thread_join(th);
    de_sem_destroy(&t->tick);
    de_sem_destroy(&t->done);
}

static bool traffic_ready(traffic *t, u32 n) {
    const u64 end = de_time_now_ns() + 5000000000ull;
    for (u32 i = 0; i < n; ++i)
        while (de_atomic_load_u32(&t[i].ready, DE_ATOMIC_ACQUIRE) == 0) {
            if (de_time_now_ns() > end)
                return false;
            de_sleep_ns(2000000ull);
        }
    return true;
}

TEST(audio_mixer, no_alloc_no_lock_10s_64_voices) {
    de_audio *a = test_audio_manual(128, 0);
    static test_source bed[64];
    for (u32 i = 0; i < 64; ++i) {
        test_src_init(&bed[i], TSRC_RAMP, 1e-6f, 4800 + i * 7, 1);
        ASSERT_TRUE(DE_HANDLE_VALID(test_play(a, &bed[i], 0, true)));
    }
    static traffic tr[2];
    de_thread *th[2];
    traffic_start(&tr[0], a, 1, &th[0]);
    traffic_start(&tr[1], a, 2, &th[1]);
    ASSERT_TRUE(traffic_ready(tr, 2));
    de__audio_null_pump(a, 4096, NULL); /* warm up this thread as the audio thread */

    const u64 hits0 = de__rt_guard_hits();
    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    de_alloc_guard_begin(&g);
    /* 10 s of audio: 1875 blocks of 256 frames. Both producers push a burst while each block
     * renders; the next block waits for both bursts, so the queue never overflows. */
    for (u32 b = 0; b < 1875; ++b) {
        de_sem_post(&tr[0].tick, 1);
        de_sem_post(&tr[1].tick, 1);
        de__audio_null_pump(a, 256, NULL);
        de_sem_wait(&tr[0].done);
        de_sem_wait(&tr[1].done);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de__rt_guard_hits(), hits0);

    traffic_stop(&tr[0], th[0]);
    traffic_stop(&tr[1], th[1]);
    EXPECT_TRUE(de_atomic_load_u64(&tr[0].ops, DE_ATOMIC_RELAXED) > 1000u);
    EXPECT_EQ(de_audio_get_stats(a).commands_dropped, 0u);
    EXPECT_EQ(de_audio_get_stats(a).voices_real >= 64u, true);
    EXPECT_EQ(test_frames_mixed(a) >= 480000u + 4096u, true);
    de_audio_destroy(a);
}

TEST(audio_mixer, no_alloc_no_lock_realtime_threads) {
    de_audio *a = test_audio_realtime(128, 0);
    static test_source bed[64];
    for (u32 i = 0; i < 64; ++i) {
        test_src_init(&bed[i], TSRC_RAMP, 1e-6f, 4800 + i * 7, 1);
        ASSERT_TRUE(DE_HANDLE_VALID(test_play(a, &bed[i], 0, true)));
    }
    static traffic tr[2];
    de_thread *th[2];
    traffic_start(&tr[0], a, 3, &th[0]);
    traffic_start(&tr[1], a, 4, &th[1]);
    ASSERT_TRUE(traffic_ready(tr, 2));
    ASSERT_TRUE(test_wait(a, test_pred_mixed_at_least, 4800, 5000));

    const u64 hits0 = de__rt_guard_hits();
    const u64 mixed0 = test_frames_mixed(a);
    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    de_alloc_guard_begin(&g);
    /* About one second on the mixer thread and the null device thread. */
    for (u32 i = 0; i < 200; ++i) {
        de_sem_post(&tr[0].tick, 1);
        de_sem_post(&tr[1].tick, 1);
        de_sleep_ns(5000000ull);
        de_sem_wait(&tr[0].done);
        de_sem_wait(&tr[1].done);
    }
    de_alloc_guard_end(&g);
    traffic_stop(&tr[0], th[0]);
    traffic_stop(&tr[1], th[1]);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de__rt_guard_hits(), hits0);
    EXPECT_TRUE(test_frames_mixed(a) > mixed0 + 4800u);
    de_audio_destroy(a);
}
