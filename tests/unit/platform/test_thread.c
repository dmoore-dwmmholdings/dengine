/* Tests for the platform thread layer: threads, mutex, rwlock, condvar, semaphore, TLS and
 * atomics (DENG-147). de_test.h checks may only run on the test's own thread, so the worker
 * threads record what they see and the test thread checks it after the join. */
#if defined(__linux__)
#define _GNU_SOURCE /* pthread_getname_np */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include "de_test.h"

#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define NS_PER_MS 1000000ull

/* ---------- helpers ---------- */

static void spawn_n(de_thread **threads, u32 n, de_thread_fn fn, void *user, size_t user_stride) {
    for (u32 i = 0; i < n; i++) {
        void *arg = (u8 *)user + (size_t)i * user_stride;
        ASSERT_EQ(de_thread_create(fn, arg, "de-test", DE_THREAD_NORMAL, 0, &threads[i]), DE_OK);
    }
}

static void join_n(de_thread **threads, u32 n) {
    for (u32 i = 0; i < n; i++)
        (void)de_thread_join(threads[i]);
}

/* Spin until *flag != 0 or the deadline passes. Returns true if the flag was seen. */
static bool wait_flag(de_atomic_u32 *flag, u64 timeout_ns) {
    u64 end = de_time_now_ns() + timeout_ns;
    while (de_atomic_load_u32(flag, DE_ATOMIC_ACQUIRE) == 0) {
        if (de_time_now_ns() > end)
            return false;
        de_cpu_pause();
    }
    return true;
}

/* ---------- create / join ---------- */

typedef struct ret_arg {
    i32 value;
    u32 tid;
} ret_arg;

static i32 return_value_fn(void *user) {
    ret_arg *a = (ret_arg *)user;
    a->tid = de_thread_current_id();
    return a->value;
}

TEST(thread, create_join_return_value) {
    enum { N = 8 };
    static const size_t stacks[N] = {0, 64u << 10, 100000, 1u << 20, 0, 0, 2u << 20, 12345};
    ret_arg args[N];
    de_thread *t[N];
    for (u32 i = 0; i < N; i++) {
        args[i].value = (i32)(1000 + i) * (i % 2 ? -1 : 1);
        args[i].tid = 0;
        de_thread_priority prio = (de_thread_priority)(i % 4);
        ASSERT_EQ(de_thread_create(return_value_fn, &args[i], NULL, prio, stacks[i], &t[i]), DE_OK);
        ASSERT_TRUE(t[i] != NULL);
    }
    for (u32 i = 0; i < N; i++)
        EXPECT_EQ(de_thread_join(t[i]), args[i].value);

    u32 main_id = de_thread_current_id();
    EXPECT_NE(main_id, 0u);
    EXPECT_EQ(de_thread_current_id(), main_id); /* stable on one thread */
    for (u32 i = 0; i < N; i++) {
        EXPECT_NE(args[i].tid, 0u);
        EXPECT_NE(args[i].tid, main_id);
    }

    de_thread *bad = (de_thread *)&args[0];
    EXPECT_EQ(de_thread_create(NULL, NULL, NULL, DE_THREAD_NORMAL, 0, &bad),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(bad == NULL);
    const de_error *err = de_error_last(); /* failures set the error context (DE_FAIL) */
    ASSERT_TRUE(err != NULL);
    EXPECT_EQ(err->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(err->message, "de_thread_create") != NULL);
    de_error_clear();
    EXPECT_EQ(de_thread_create(return_value_fn, &args[0], NULL, DE_THREAD_NORMAL, 0, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_thread_join(NULL), 0);
}

/* ---------- names ---------- */

typedef struct name_arg {
    const char *set_name; /* if non-NULL, the thread renames itself with de_thread_set_name */
    char seen[128];
} name_arg;

static void read_own_name(char *buf, size_t cap) {
    buf[0] = '\0';
#if defined(_WIN32)
    PWSTR w = NULL;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &w)) && w) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL);
        LocalFree(w);
    }
#else
    pthread_getname_np(pthread_self(), buf, cap);
#endif
}

static i32 name_fn(void *user) {
    name_arg *a = (name_arg *)user;
    if (a->set_name)
        de_thread_set_name(a->set_name);
    read_own_name(a->seen, sizeof a->seen);
    return 0;
}

TEST(thread, names) {
#if defined(__linux__)
    const size_t visible = 15; /* TASK_COMM_LEN - 1 */
#else
    const size_t visible = DE_THREAD_NAME_MAX;
#endif
    static const char long_name[] =
        "de-a-very-long-thread-name-0123456789-0123456789-0123456789-0123456789-end";
    _Static_assert(sizeof long_name - 1 > DE_THREAD_NAME_MAX, "name must be longer than the max");

    /* UTF-8 'e' with acute accent (2 bytes) straddles both truncation points: bytes 14-15
     * (Linux keeps 15) and bytes 62-63 (others keep 63). Truncation backs off to 14 / 62. */
#define E_ACUTE "\xC3\xA9"
    static const char utf8_name[] =
        "aaaaaaaaaaaaaa" E_ACUTE "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" E_ACUTE "zz";
#undef E_ACUTE
#if defined(__linux__)
    const size_t utf8_kept = 14;
#else
    const size_t utf8_kept = 62;
#endif

    name_arg a[5];
    memset(a, 0, sizeof a);
    de_thread *t[5];
    ASSERT_EQ(de_thread_create(name_fn, &a[0], "de-io", DE_THREAD_NORMAL, 0, &t[0]), DE_OK);
    ASSERT_EQ(de_thread_create(name_fn, &a[1], long_name, DE_THREAD_HIGH, 0, &t[1]), DE_OK);
    a[2].set_name = "de-renamed";
    ASSERT_EQ(de_thread_create(name_fn, &a[2], "de-before", DE_THREAD_LOW, 0, &t[2]), DE_OK);
    ASSERT_EQ(de_thread_create(name_fn, &a[3], NULL, DE_THREAD_NORMAL, 0, &t[3]), DE_OK);
    ASSERT_EQ(de_thread_create(name_fn, &a[4], utf8_name, DE_THREAD_NORMAL, 0, &t[4]), DE_OK);
    join_n(t, 5);

    EXPECT_STREQ(a[0].seen, "de-io");
    EXPECT_EQ(strlen(a[1].seen), visible);
    EXPECT_EQ(strncmp(a[1].seen, long_name, visible), 0);
    EXPECT_STREQ(a[2].seen, "de-renamed");
    EXPECT_EQ(strlen(a[4].seen), utf8_kept);
    EXPECT_EQ(strncmp(a[4].seen, utf8_name, utf8_kept), 0);
#if defined(__linux__)
    /* Linux: a thread without a name keeps the comm name it inherited from its creator. */
    char main_name[128];
    read_own_name(main_name, sizeof main_name);
    EXPECT_STREQ(a[3].seen, main_name);
#else
    EXPECT_STREQ(a[3].seen, "");
#endif
}

/* ---------- mutex ---------- */

enum { CONTENTION_THREADS = 16, CONTENTION_ITERS = 100000 };

typedef struct counter_shared {
    de_mutex m;
    u64 value; /* plain, protected by m */
} counter_shared;

static i32 mutex_inc_fn(void *user) {
    counter_shared *s = (counter_shared *)user;
    for (u32 i = 0; i < CONTENTION_ITERS; i++) {
        de_mutex_lock(&s->m);
        s->value++;
        de_mutex_unlock(&s->m);
    }
    return 0;
}

TEST(thread, mutex_contention) {
    static counter_shared s;
    de_mutex_init(&s.m);
    s.value = 0;

    /* Non-recursive: try_lock fails while the lock is held, even by the caller. */
    de_mutex_lock(&s.m);
    EXPECT_FALSE(de_mutex_try_lock(&s.m));
    de_mutex_unlock(&s.m);
    EXPECT_TRUE(de_mutex_try_lock(&s.m));
    de_mutex_unlock(&s.m);

    de_thread *t[CONTENTION_THREADS];
    spawn_n(t, CONTENTION_THREADS, mutex_inc_fn, &s, 0);
    join_n(t, CONTENTION_THREADS);
    EXPECT_EQ(s.value, (u64)CONTENTION_THREADS * CONTENTION_ITERS);
    de_mutex_destroy(&s.m);
}

/* ---------- rwlock ---------- */

enum { RW_WRITERS = 8, RW_READERS = 8 };

typedef struct rw_shared {
    de_rwlock l;
    u64 a, b; /* writers keep a == b; readers check it */
    de_atomic_u32 writers_done;
    de_atomic_u64 reads, torn;
} rw_shared;

static i32 rw_writer_fn(void *user) {
    rw_shared *s = (rw_shared *)user;
    for (u32 i = 0; i < CONTENTION_ITERS; i++) {
        de_rwlock_write_lock(&s->l);
        s->a++;
        s->b++;
        de_rwlock_write_unlock(&s->l);
    }
    return 0;
}

static i32 rw_reader_fn(void *user) {
    rw_shared *s = (rw_shared *)user;
    u64 reads = 0, torn = 0;
    do {
        de_rwlock_read_lock(&s->l);
        if (s->a != s->b)
            torn++;
        de_rwlock_read_unlock(&s->l);
        reads++;
        de_cpu_pause(); /* leave the writers a gap */
    } while (de_atomic_load_u32(&s->writers_done, DE_ATOMIC_ACQUIRE) == 0);
    de_atomic_fetch_add_u64(&s->reads, reads, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&s->torn, torn, DE_ATOMIC_RELAXED);
    return 0;
}

typedef struct rw_shared_reader {
    de_rwlock *l;
    de_atomic_u32 got;
} rw_shared_reader;

static i32 rw_second_reader_fn(void *user) {
    rw_shared_reader *r = (rw_shared_reader *)user;
    de_rwlock_read_lock(r->l);
    de_atomic_store_u32(&r->got, 1, DE_ATOMIC_RELEASE);
    de_rwlock_read_unlock(r->l);
    return 0;
}

TEST(thread, rwlock) {
    static rw_shared s;
    memset(&s, 0, sizeof s);
    de_rwlock_init(&s.l);
    de_atomic_init_u32(&s.writers_done, 0);
    de_atomic_init_u64(&s.reads, 0);
    de_atomic_init_u64(&s.torn, 0);

    /* Readers share the lock: a second reader gets in while this thread holds a read lock. */
    rw_shared_reader r = {&s.l, DE_ATOMIC_INIT(0)};
    de_thread *second = NULL;
    de_rwlock_read_lock(&s.l);
    ASSERT_EQ(de_thread_create(rw_second_reader_fn, &r, "de-reader", DE_THREAD_NORMAL, 0, &second),
              DE_OK);
    bool shared = wait_flag(&r.got, 5000 * NS_PER_MS);
    de_rwlock_read_unlock(&s.l);
    (void)de_thread_join(second);
    EXPECT_TRUE(shared);

    de_thread *w[RW_WRITERS], *rd[RW_READERS];
    spawn_n(rd, RW_READERS, rw_reader_fn, &s, 0);
    spawn_n(w, RW_WRITERS, rw_writer_fn, &s, 0);
    join_n(w, RW_WRITERS);
    de_atomic_store_u32(&s.writers_done, 1, DE_ATOMIC_RELEASE);
    join_n(rd, RW_READERS);

    EXPECT_EQ(s.a, (u64)RW_WRITERS * CONTENTION_ITERS);
    EXPECT_EQ(s.b, (u64)RW_WRITERS * CONTENTION_ITERS);
    EXPECT_EQ(de_atomic_load_u64(&s.torn, DE_ATOMIC_RELAXED), 0u);
    EXPECT_GE(de_atomic_load_u64(&s.reads, DE_ATOMIC_RELAXED), (u64)RW_READERS);
    de_rwlock_destroy(&s.l);
}

/* ---------- condition variable ---------- */

enum { PINGPONG_ITERS = 1000 };

typedef struct pingpong {
    de_mutex m;
    de_cond c;
    u32 turn;  /* 0 = main thread's turn, 1 = worker's turn */
    u32 value; /* written by main, checked by the worker */
    u32 out_of_order;
} pingpong;

static i32 pong_fn(void *user) {
    pingpong *p = (pingpong *)user;
    for (u32 i = 0; i < PINGPONG_ITERS; i++) {
        de_mutex_lock(&p->m);
        while (p->turn != 1) /* spurious-wakeup-safe: re-check the predicate */
            de_cond_wait(&p->c, &p->m);
        if (p->value != i)
            p->out_of_order++;
        p->turn = 0;
        de_cond_signal(&p->c);
        de_mutex_unlock(&p->m);
    }
    return 0;
}

enum { PC_CAP = 8, PC_PRODUCERS = 2, PC_CONSUMERS = 2, PC_ITEMS = 1000 };

typedef struct prodcons {
    de_mutex m;
    de_cond not_full, not_empty;
    u32 ring[PC_CAP];
    u32 head, count, produced_done;
    u64 consumed_sum, consumed_n;
} prodcons;

static i32 producer_fn(void *user) {
    prodcons *q = (prodcons *)user;
    for (u32 i = 1; i <= PC_ITEMS; i++) {
        de_mutex_lock(&q->m);
        while (q->count == PC_CAP)
            de_cond_wait(&q->not_full, &q->m);
        q->ring[(q->head + q->count) % PC_CAP] = i;
        q->count++;
        de_cond_signal(&q->not_empty);
        de_mutex_unlock(&q->m);
    }
    de_mutex_lock(&q->m);
    q->produced_done++;
    de_cond_broadcast(&q->not_empty); /* consumers re-check the exit condition */
    de_mutex_unlock(&q->m);
    return 0;
}

static i32 consumer_fn(void *user) {
    prodcons *q = (prodcons *)user;
    for (;;) {
        de_mutex_lock(&q->m);
        while (q->count == 0 && q->produced_done < PC_PRODUCERS)
            de_cond_wait(&q->not_empty, &q->m);
        if (q->count == 0) {
            de_mutex_unlock(&q->m);
            return 0;
        }
        u32 v = q->ring[q->head];
        q->head = (q->head + 1) % PC_CAP;
        q->count--;
        q->consumed_sum += v;
        q->consumed_n++;
        de_cond_signal(&q->not_full);
        de_mutex_unlock(&q->m);
    }
}

TEST(thread, cond_pingpong) {
    static pingpong p;
    memset(&p, 0, sizeof p);
    de_mutex_init(&p.m);
    de_cond_init(&p.c);

    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(pong_fn, &p, "de-pong", DE_THREAD_NORMAL, 0, &t), DE_OK);
    u32 rounds = 0;
    for (u32 i = 0; i < PINGPONG_ITERS; i++) {
        de_mutex_lock(&p.m);
        p.value = i;
        p.turn = 1;
        de_cond_signal(&p.c);
        while (p.turn != 0)
            de_cond_wait(&p.c, &p.m);
        de_mutex_unlock(&p.m);
        rounds++;
    }
    (void)de_thread_join(t);
    EXPECT_EQ(rounds, (u32)PINGPONG_ITERS);
    EXPECT_EQ(p.out_of_order, 0u);

    /* Bounded producer/consumer queue with two condvars. */
    static prodcons q;
    memset(&q, 0, sizeof q);
    de_mutex_init(&q.m);
    de_cond_init(&q.not_full);
    de_cond_init(&q.not_empty);
    de_thread *prod[PC_PRODUCERS], *cons[PC_CONSUMERS];
    spawn_n(cons, PC_CONSUMERS, consumer_fn, &q, 0);
    spawn_n(prod, PC_PRODUCERS, producer_fn, &q, 0);
    join_n(prod, PC_PRODUCERS);
    join_n(cons, PC_CONSUMERS);
    EXPECT_EQ(q.consumed_n, (u64)PC_PRODUCERS * PC_ITEMS);
    EXPECT_EQ(q.consumed_sum, (u64)PC_PRODUCERS * PC_ITEMS * (PC_ITEMS + 1) / 2);

    de_cond_destroy(&q.not_full);
    de_cond_destroy(&q.not_empty);
    de_mutex_destroy(&q.m);
    de_cond_destroy(&p.c);
    de_mutex_destroy(&p.m);
}

TEST(thread, cond_wait_timeout) {
    de_mutex m;
    de_cond c;
    de_mutex_init(&m);
    de_cond_init(&c);
    const u64 timeout = 20 * NS_PER_MS;

    de_mutex_lock(&m);
    bool timed_out = false;
    u64 waited = 0;
    /* Nobody signals, so the wait times out; a spurious wakeup (true) just retries. */
    for (u32 tries = 0; tries < 10 && !timed_out; tries++) {
        u64 t0 = de_time_now_ns();
        timed_out = !de_cond_wait_timeout(&c, &m, timeout);
        waited = de_time_now_ns() - t0;
    }
    de_mutex_unlock(&m);
    EXPECT_TRUE(timed_out);
    EXPECT_GE(waited, timeout - NS_PER_MS); /* 1 ms slack for clock granularity */

    de_cond_destroy(&c);
    de_mutex_destroy(&m);
}

/* ---------- semaphore ---------- */

enum { SEM_POSTERS = 4, SEM_WAITERS = 4, SEM_PER_THREAD = 10000 };

typedef struct sem_shared {
    de_sem s;
    de_atomic_u64 posted, taken;
} sem_shared;

static i32 sem_poster_fn(void *user) {
    sem_shared *s = (sem_shared *)user;
    /* Mix single posts and batch posts of 4 (de_sem_post count > 1). */
    u32 left = SEM_PER_THREAD;
    while (left > 0) {
        u32 n = left >= 4 && left % 3 == 0 ? 4 : 1;
        de_sem_post(&s->s, n);
        de_atomic_fetch_add_u64(&s->posted, n, DE_ATOMIC_RELAXED);
        left -= n;
    }
    return 0;
}

static i32 sem_waiter_fn(void *user) {
    sem_shared *s = (sem_shared *)user;
    for (u32 i = 0; i < SEM_PER_THREAD; i++) {
        if (i % 2 == 0)
            de_sem_wait(&s->s);
        else
            while (!de_sem_wait_timeout(&s->s, 1000 * NS_PER_MS)) {
            }
        de_atomic_fetch_add_u64(&s->taken, 1, DE_ATOMIC_RELAXED);
    }
    return 0;
}

TEST(thread, semaphore) {
    /* Initial count. */
    de_sem s0;
    ASSERT_EQ(de_sem_init(&s0, 3), DE_OK);
    EXPECT_TRUE(de_sem_try_wait(&s0));
    EXPECT_TRUE(de_sem_try_wait(&s0));
    EXPECT_TRUE(de_sem_try_wait(&s0));
    EXPECT_FALSE(de_sem_try_wait(&s0));
    EXPECT_FALSE(de_sem_wait_timeout(&s0, 5 * NS_PER_MS));
    de_sem_post(&s0, 2);
    EXPECT_TRUE(de_sem_wait_timeout(&s0, 5 * NS_PER_MS));
    de_sem_wait(&s0);
    EXPECT_FALSE(de_sem_try_wait(&s0));
    de_sem_post(&s0, 0); /* no-op */
    EXPECT_FALSE(de_sem_try_wait(&s0));
    de_sem_destroy(&s0);

    /* Posts and waits balance across 8 threads. */
    static sem_shared s;
    ASSERT_EQ(de_sem_init(&s.s, 0), DE_OK);
    de_atomic_init_u64(&s.posted, 0);
    de_atomic_init_u64(&s.taken, 0);
    de_thread *t[SEM_POSTERS + SEM_WAITERS];
    spawn_n(t, SEM_WAITERS, sem_waiter_fn, &s, 0);
    spawn_n(t + SEM_WAITERS, SEM_POSTERS, sem_poster_fn, &s, 0);
    join_n(t, SEM_POSTERS + SEM_WAITERS);
    EXPECT_EQ(de_atomic_load_u64(&s.posted, DE_ATOMIC_RELAXED), (u64)SEM_POSTERS * SEM_PER_THREAD);
    EXPECT_EQ(de_atomic_load_u64(&s.taken, DE_ATOMIC_RELAXED), (u64)SEM_WAITERS * SEM_PER_THREAD);
    EXPECT_FALSE(de_sem_try_wait(&s.s)); /* count is back to zero */
    de_sem_destroy(&s.s);
}

/* ---------- thread-local storage ---------- */

enum { TLS_THREADS = 8 };

typedef struct tls_shared {
    de_tls_key key;
    de_atomic_u32 dtor_calls;
    de_atomic_u64 dtor_value_sum;
} tls_shared;

static tls_shared g_tls; /* the destructor has no user pointer, so it reaches this */

static void tls_dtor(void *value) {
    u32 *slot = (u32 *)value;
    de_atomic_fetch_add_u32(&g_tls.dtor_calls, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&g_tls.dtor_value_sum, *slot, DE_ATOMIC_RELAXED);
}

typedef struct tls_arg {
    u32 id;   /* the value this thread stores (via a pointer to it) */
    u32 mode; /* 0 = set and keep, 1 = never set, 2 = set then clear */
    bool initial_null, own_value_seen;
} tls_arg;

static i32 tls_fn(void *user) {
    tls_arg *a = (tls_arg *)user;
    a->initial_null = de_tls_get(g_tls.key) == NULL;
    if (a->mode == 1) {
        a->own_value_seen = true;
        return 0;
    }
    if (de_tls_set(g_tls.key, &a->id) != DE_OK)
        return 1;
    bool ok = true;
    for (u32 i = 0; i < 1000; i++) { /* other threads set their values meanwhile */
        ok = ok && de_tls_get(g_tls.key) == &a->id;
        de_cpu_pause();
    }
    a->own_value_seen = ok;
    if (a->mode == 2)
        (void)de_tls_set(g_tls.key, NULL);
    return 0;
}

TEST(thread, tls_per_thread) {
    de_tls_key zero = {0};
    EXPECT_TRUE(de_tls_get(zero) == NULL);
    EXPECT_EQ(de_tls_set(zero, &zero), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_tls_key_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);

    memset(&g_tls, 0, sizeof g_tls);
    de_atomic_init_u32(&g_tls.dtor_calls, 0);
    de_atomic_init_u64(&g_tls.dtor_value_sum, 0);
    ASSERT_EQ(de_tls_key_create(&g_tls.key, tls_dtor), DE_OK);
    ASSERT_NE(g_tls.key.id, 0u);

    /* Main thread: starts NULL, keeps its own value. Cleared before destroy (no dtor here). */
    u32 main_value = 7;
    EXPECT_TRUE(de_tls_get(g_tls.key) == NULL);
    ASSERT_EQ(de_tls_set(g_tls.key, &main_value), DE_OK);
    EXPECT_TRUE(de_tls_get(g_tls.key) == &main_value);

    tls_arg args[TLS_THREADS + 2];
    de_thread *t[TLS_THREADS + 2];
    u64 expected_sum = 0;
    for (u32 i = 0; i < TLS_THREADS + 2; i++) {
        args[i].id = 100 + i;
        args[i].mode = i < TLS_THREADS ? 0u : i - TLS_THREADS + 1; /* last two: modes 1, 2 */
        args[i].initial_null = args[i].own_value_seen = false;
        if (args[i].mode == 0)
            expected_sum += args[i].id;
    }
    spawn_n(t, TLS_THREADS + 2, tls_fn, args, sizeof args[0]);
    for (u32 i = 0; i < TLS_THREADS + 2; i++)
        EXPECT_EQ(de_thread_join(t[i]), 0);

    for (u32 i = 0; i < TLS_THREADS + 2; i++) {
        EXPECT_TRUE(args[i].initial_null);
        EXPECT_TRUE(args[i].own_value_seen);
    }
    /* Destructors ran on thread exit, before the join returned, for non-NULL values only. */
    EXPECT_EQ(de_atomic_load_u32(&g_tls.dtor_calls, DE_ATOMIC_RELAXED), (u32)TLS_THREADS);
    EXPECT_EQ(de_atomic_load_u64(&g_tls.dtor_value_sum, DE_ATOMIC_RELAXED), expected_sum);
    EXPECT_TRUE(de_tls_get(g_tls.key) == &main_value); /* untouched by the other threads */

    ASSERT_EQ(de_tls_set(g_tls.key, NULL), DE_OK);
    de_tls_key_destroy(g_tls.key);

    /* A key without destructor. */
    de_tls_key k2;
    ASSERT_EQ(de_tls_key_create(&k2, NULL), DE_OK);
    EXPECT_TRUE(de_tls_get(k2) == NULL);
    ASSERT_EQ(de_tls_set(k2, &k2), DE_OK);
    EXPECT_TRUE(de_tls_get(k2) == &k2);
    ASSERT_EQ(de_tls_set(k2, NULL), DE_OK);
    de_tls_key_destroy(k2);
}

/* ---------- atomics (src/platform/de_atomic.h) ---------- */

typedef struct atomic_shared {
    de_atomic_u32 add32;
    de_atomic_u64 cas64;
    de_atomic_u32 bits;
    de_atomic_ptr last;
} atomic_shared;

typedef struct atomic_arg {
    atomic_shared *s;
    u32 index;
} atomic_arg;

static i32 atomic_fn(void *user) {
    atomic_arg *a = (atomic_arg *)user;
    for (u32 i = 0; i < CONTENTION_ITERS; i++) {
        de_atomic_fetch_add_u32(&a->s->add32, 1, DE_ATOMIC_RELAXED);
        u64 cur = de_atomic_load_u64(&a->s->cas64, DE_ATOMIC_RELAXED);
        while (!de_atomic_cas_weak_u64(&a->s->cas64, &cur, cur + 2, DE_ATOMIC_ACQ_REL,
                                       DE_ATOMIC_RELAXED))
            de_cpu_pause();
    }
    de_atomic_fetch_or_u32(&a->s->bits, 1u << a->index, DE_ATOMIC_RELEASE);
    (void)de_atomic_exchange_ptr(&a->s->last, a, DE_ATOMIC_ACQ_REL);
    return 0;
}

TEST(thread, atomics) {
    static atomic_shared s;
    de_atomic_init_u32(&s.add32, 0);
    de_atomic_init_u64(&s.cas64, 0);
    de_atomic_init_u32(&s.bits, 0);
    de_atomic_init_ptr(&s.last, NULL);

    atomic_arg args[CONTENTION_THREADS];
    for (u32 i = 0; i < CONTENTION_THREADS; i++) {
        args[i].s = &s;
        args[i].index = i;
    }
    de_thread *t[CONTENTION_THREADS];
    spawn_n(t, CONTENTION_THREADS, atomic_fn, args, sizeof args[0]);
    join_n(t, CONTENTION_THREADS);

    EXPECT_EQ(de_atomic_load_u32(&s.add32, DE_ATOMIC_SEQ_CST),
              (u32)CONTENTION_THREADS * CONTENTION_ITERS);
    EXPECT_EQ(de_atomic_load_u64(&s.cas64, DE_ATOMIC_SEQ_CST),
              (u64)CONTENTION_THREADS * CONTENTION_ITERS * 2);
    EXPECT_EQ(de_atomic_load_u32(&s.bits, DE_ATOMIC_ACQUIRE), (1u << CONTENTION_THREADS) - 1);
    atomic_arg *last = (atomic_arg *)de_atomic_load_ptr(&s.last, DE_ATOMIC_ACQUIRE);
    EXPECT_TRUE(last >= &args[0] && last < &args[CONTENTION_THREADS]);

    /* Single-thread semantics of the remaining wrappers. */
    de_atomic_u32 x = DE_ATOMIC_INIT(5);
    u32 exp = 4;
    EXPECT_FALSE(de_atomic_cas_u32(&x, &exp, 9, DE_ATOMIC_SEQ_CST, DE_ATOMIC_SEQ_CST));
    EXPECT_EQ(exp, 5u);
    EXPECT_TRUE(de_atomic_cas_u32(&x, &exp, 9, DE_ATOMIC_SEQ_CST, DE_ATOMIC_SEQ_CST));
    EXPECT_EQ(de_atomic_exchange_u32(&x, 12, DE_ATOMIC_SEQ_CST), 9u);
    EXPECT_EQ(de_atomic_fetch_sub_u32(&x, 2, DE_ATOMIC_SEQ_CST), 12u);
    EXPECT_EQ(de_atomic_fetch_and_u32(&x, 0x8, DE_ATOMIC_SEQ_CST), 10u);
    de_atomic_store_u32(&x, 1, DE_ATOMIC_RELEASE);
    EXPECT_EQ(de_atomic_load_u32(&x, DE_ATOMIC_ACQUIRE), 1u);

    de_atomic_u64 y = DE_ATOMIC_INIT(0xffffffff00000000ull);
    EXPECT_EQ(de_atomic_fetch_add_u64(&y, 1, DE_ATOMIC_RELAXED), 0xffffffff00000000ull);
    EXPECT_EQ(de_atomic_fetch_or_u64(&y, 2, DE_ATOMIC_RELAXED), 0xffffffff00000001ull);
    EXPECT_EQ(de_atomic_fetch_and_u64(&y, 0xffull, DE_ATOMIC_RELAXED), 0xffffffff00000003ull);
    EXPECT_EQ(de_atomic_fetch_sub_u64(&y, 3, DE_ATOMIC_RELAXED), 3u);
    EXPECT_EQ(de_atomic_exchange_u64(&y, 42, DE_ATOMIC_RELAXED), 0u);
    u64 e64 = 42;
    EXPECT_TRUE(de_atomic_cas_u64(&y, &e64, 43, DE_ATOMIC_SEQ_CST, DE_ATOMIC_RELAXED));
    de_atomic_store_u64(&y, 7, DE_ATOMIC_SEQ_CST);
    EXPECT_EQ(de_atomic_load_u64(&y, DE_ATOMIC_SEQ_CST), 7u);

    int target = 0;
    de_atomic_ptr p = DE_ATOMIC_INIT(NULL);
    void *ep = NULL;
    EXPECT_TRUE(de_atomic_cas_ptr(&p, &ep, &target, DE_ATOMIC_SEQ_CST, DE_ATOMIC_SEQ_CST));
    ep = NULL;
    EXPECT_FALSE(de_atomic_cas_weak_ptr(&p, &ep, NULL, DE_ATOMIC_SEQ_CST, DE_ATOMIC_SEQ_CST));
    EXPECT_TRUE(ep == &target);
    de_atomic_store_ptr(&p, NULL, DE_ATOMIC_RELEASE);
    EXPECT_TRUE(de_atomic_load_ptr(&p, DE_ATOMIC_ACQUIRE) == NULL);
    de_atomic_thread_fence(DE_ATOMIC_SEQ_CST);
    de_atomic_signal_fence(DE_ATOMIC_SEQ_CST);
}

/* ---------- cores and affinity ---------- */

static i32 wait_sem_fn(void *user) {
    de_sem_wait((de_sem *)user);
    return 3;
}

TEST(thread, cores_and_affinity) {
    u32 cores = de_cpu_logical_cores();
    EXPECT_GE(cores, 1u);
    EXPECT_LE(cores, 4096u);

    de_sem go;
    ASSERT_EQ(de_sem_init(&go, 0), DE_OK);
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(wait_sem_fn, &go, "de-affinity", DE_THREAD_NORMAL, 0, &t), DE_OK);
    EXPECT_EQ(de_thread_set_affinity(t, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_thread_set_affinity(NULL, 1), DE_ERR_INVALID_ARGUMENT);
    /* Every core we may run on is in the mask, so the call is valid even inside a cpuset. */
    de_result r = de_thread_set_affinity(t, ~0ull);
#if defined(__APPLE__) && defined(__aarch64__)
    EXPECT_EQ(r, DE_ERR_UNSUPPORTED); /* Apple Silicon has no affinity policy */
#elif defined(__APPLE__)
    EXPECT_TRUE(r == DE_OK || r == DE_ERR_UNSUPPORTED);
#else
    EXPECT_EQ(r, DE_OK);
#endif
    de_sem_post(&go, 1);
    EXPECT_EQ(de_thread_join(t), 3);
    de_sem_destroy(&go);
}
