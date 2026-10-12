/* Unit tests of the Chase-Lev deque (src/task/deque.c, DENG-291). */
#include "de_test.h"

#include <dengine/de_platform.h>

#include <stdint.h>
#include <stdlib.h>

#include "platform/de_atomic.h"
#include "task/deque.h"

#define ITEM(i)    ((void *)(uintptr_t)((u64)(i) + 1))
#define ITEM_ID(p) ((u64)(uintptr_t)(p) - 1)

TEST(deque, owner_lifo) {
    de__deque d;
    ASSERT_EQ(de__deque_init(&d, 0, NULL, 0), DE_OK);
    for (u32 i = 0; i < 100; ++i)
        ASSERT_TRUE(de__deque_push(&d, ITEM(i)));
    EXPECT_EQ(de__deque_size_approx(&d), 100u);
    for (u32 i = 100; i-- > 0;)
        ASSERT_EQ(ITEM_ID(de__deque_pop(&d)), (u64)i);
    EXPECT_TRUE(de__deque_pop(&d) == NULL);
    EXPECT_TRUE(de__deque_pop(&d) == NULL); /* popping an empty deque twice keeps it empty */
    void *x = NULL;
    EXPECT_EQ(de__deque_steal(&d, &x), DE__STEAL_EMPTY);
    EXPECT_EQ(de__deque_size_approx(&d), 0u);
    /* Still usable after running empty. */
    ASSERT_TRUE(de__deque_push(&d, ITEM(7)));
    EXPECT_EQ(ITEM_ID(de__deque_pop(&d)), 7u);
    de__deque_destroy(&d);
}

TEST(deque, steal_fifo) {
    de__deque d;
    ASSERT_EQ(de__deque_init(&d, 0, NULL, 0), DE_OK);
    for (u32 i = 0; i < 100; ++i)
        ASSERT_TRUE(de__deque_push(&d, ITEM(i)));
    for (u32 i = 0; i < 50; ++i) {
        void *x = NULL;
        ASSERT_EQ(de__deque_steal(&d, &x), DE__STEAL_OK);
        ASSERT_EQ(ITEM_ID(x), (u64)i);
    }
    /* The owner still pops from the other end. */
    EXPECT_EQ(ITEM_ID(de__deque_pop(&d)), 99u);
    void *x = NULL;
    EXPECT_EQ(de__deque_steal(&d, &x), DE__STEAL_OK);
    EXPECT_EQ(ITEM_ID(x), 50u);
    de__deque_destroy(&d);
}

TEST(deque, grow) {
    de__deque d;
    EXPECT_NE(de__deque_init(&d, 3, NULL, 0), DE_OK); /* not a power of two */
    de__deque_destroy(&d);
    ASSERT_EQ(de__deque_init(&d, 4, NULL, 0), DE_OK);
    EXPECT_EQ(de__deque_capacity(&d), 4u);
    /* Steal a few first so the live range wraps the ring before it grows. */
    for (u32 i = 0; i < 3; ++i)
        ASSERT_TRUE(de__deque_push(&d, ITEM(i)));
    void *x = NULL;
    for (u32 i = 0; i < 3; ++i)
        ASSERT_EQ(de__deque_steal(&d, &x), DE__STEAL_OK);
    for (u32 i = 3; i < 10003; ++i)
        ASSERT_TRUE(de__deque_push(&d, ITEM(i)));
    EXPECT_EQ(de__deque_capacity(&d), 16384u);
    EXPECT_EQ(de__deque_size_approx(&d), 10000u);
    /* Thieves see the oldest items in order, the owner the newest. */
    for (u32 i = 3; i < 5003; ++i) {
        ASSERT_EQ(de__deque_steal(&d, &x), DE__STEAL_OK);
        ASSERT_EQ(ITEM_ID(x), (u64)i);
    }
    for (u32 i = 10003; i-- > 5003;)
        ASSERT_EQ(ITEM_ID(de__deque_pop(&d)), (u64)i);
    EXPECT_TRUE(de__deque_pop(&d) == NULL);
    de__deque_destroy(&d); /* frees the retired rings too (ASan/leak check) */
}

/* ---------- concurrent_exactly_once ---------- */

#define DQ_ITEMS   1000000u
#define DQ_THIEVES 3u

/* The owner must not finish before the thieves run: on a loaded Windows CI runner a new thread
 * can wait a whole scheduling quantum (~190 ms on Windows Server) before it first runs, while
 * the owner's 1e6 pushes take about 40 ms in Release, so the thieves found the deque empty and
 * done (DENG-375). The owner therefore blocks until every thief has started (`started`, one post
 * per thief) and, before its final drain, until some thief has stolen (`first_steal`, posted by
 * each thief on its first steal). Both waits block in the OS, so the owner takes no core from
 * the thieves; both have a timeout, after which the `stolen > 0` check reports the failure. */
typedef struct dq_shared {
    de__deque *d;
    de_atomic_u32 *hits;
    de_atomic_u32 done; /* the owner has pushed and popped everything it will */
    de_atomic_u64 taken;
    de_sem started;
    de_sem first_steal;
} dq_shared;

#define DQ_WAIT_NS (10ull * 1000 * 1000 * 1000)

static void dq_take(dq_shared *s, void *item) {
    de_atomic_fetch_add_u32(&s->hits[ITEM_ID(item)], 1, DE_ATOMIC_RELAXED);
}

static i32 dq_thief(void *p) {
    dq_shared *s = (dq_shared *)p;
    u64 n = 0;
    de_sem_post(&s->started, 1);
    for (;;) {
        void *x = NULL;
        const de__steal_result r = de__deque_steal(s->d, &x);
        if (r == DE__STEAL_OK) {
            dq_take(s, x);
            if (n++ == 0)
                de_sem_post(&s->first_steal, 1);
        } else if (r == DE__STEAL_EMPTY) {
            if (de_atomic_load_u32(&s->done, DE_ATOMIC_ACQUIRE) != 0 &&
                de__deque_size_approx(s->d) == 0)
                break;
            de_cpu_pause();
        }
    }
    de_atomic_fetch_add_u64(&s->taken, n, DE_ATOMIC_RELAXED);
    return 0;
}

TEST(deque, concurrent_exactly_once) {
    de__deque d;
    /* A small ring, so it may grow while the thieves are stealing (deque.grow covers growth
     * deterministically). */
    ASSERT_EQ(de__deque_init(&d, 64, NULL, 0), DE_OK);
    dq_shared s;
    s.d = &d;
    s.hits = (de_atomic_u32 *)calloc(DQ_ITEMS, sizeof(de_atomic_u32));
    ASSERT_TRUE(s.hits != NULL);
    de_atomic_init_u32(&s.done, 0);
    de_atomic_init_u64(&s.taken, 0);
    ASSERT_EQ(de_sem_init(&s.started, 0), DE_OK);
    ASSERT_EQ(de_sem_init(&s.first_steal, 0), DE_OK);

    de_thread *thieves[DQ_THIEVES];
    for (u32 i = 0; i < DQ_THIEVES; ++i)
        ASSERT_EQ(de_thread_create(dq_thief, &s, "dq-thief", DE_THREAD_NORMAL, 0, &thieves[i]),
                  DE_OK);
    u32 started = 0;
    for (u32 i = 0; i < DQ_THIEVES; ++i)
        started += de_sem_wait_timeout(&s.started, DQ_WAIT_NS) ? 1u : 0u;
    EXPECT_EQ(started, DQ_THIEVES);

    /* The owner pushes in bursts and pops some back, like a worker running nested work. */
    u64 owner_taken = 0;
    for (u32 i = 0; i < DQ_ITEMS; ++i) {
        ASSERT_TRUE(de__deque_push(&d, ITEM(i)));
        if (i % 3 == 2) {
            void *x = de__deque_pop(&d);
            if (x != NULL) {
                dq_take(&s, x);
                ++owner_taken;
            }
        }
    }
    /* Leave the rest to the thieves until one has stolen (see dq_shared). */
    const bool stole_before_drain = de_sem_wait_timeout(&s.first_steal, DQ_WAIT_NS);
    for (void *x; (x = de__deque_pop(&d)) != NULL;) {
        dq_take(&s, x);
        ++owner_taken;
    }
    de_atomic_store_u32(&s.done, 1, DE_ATOMIC_RELEASE);
    for (u32 i = 0; i < DQ_THIEVES; ++i)
        de_thread_join(thieves[i]);

    const u64 stolen = de_atomic_load_u64(&s.taken, DE_ATOMIC_RELAXED);
    EXPECT_EQ(owner_taken + stolen, (u64)DQ_ITEMS);
    u32 bad = 0;
    for (u32 i = 0; i < DQ_ITEMS; ++i)
        bad += de_atomic_load_u32(&s.hits[i], DE_ATOMIC_RELAXED) != 1u;
    EXPECT_EQ(bad, 0u);
    EXPECT_TRUE(stole_before_drain);
    EXPECT_GT(stolen, 0u); /* the thieves did take part */
    de_sem_destroy(&s.first_steal);
    de_sem_destroy(&s.started);
    free(s.hits);
    de__deque_destroy(&d);
}
