/* Unit tests: de_hash64 (XXH3-64) reference vectors and de_name interning (DENG-153).
 *
 * This file is built twice: into test_core (the build type's own DE_NAME_LOOKUP) and, with
 * src/core/name.c, into test_core_name_shipping with DE_SHIPPING=1, which proves the Shipping
 * behaviour (no table, de_name_str returns NULL) in every build type. */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <setjmp.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Test hooks defined in src/core/name.c. */
typedef u64 (*de__name_hash_fn)(const void *data, size_t len);
void de__name_test_set_hash(de__name_hash_fn fn);
size_t de__name_test_count(void);

/* ---------- XXH3 reference vectors ---------- */

#define PRIME32 2654435761U
#define PRIME64 11400714785074694797ULL

/* The xxHash sanity buffer (cli/xsum_sanity_check.c): byte i is the top byte of
 * PRIME32 * PRIME64^i. The vectors below are XXH3_64bits_withSeed over its first `len` bytes. */
#define SANITY_LEN 2367
static unsigned char sanity_buf[SANITY_LEN];

static void fill_sanity_buffer(void) {
    u64 gen = PRIME32;
    for (size_t i = 0; i < SANITY_LEN; ++i) {
        sanity_buf[i] = (unsigned char)(gen >> 56);
        gen *= PRIME64;
    }
}

typedef struct xxh3_vector {
    size_t len;
    u64 seed;
    u64 hash;
} xxh3_vector;

/* From xxHash 0.8 cli/xsum_sanity_check.c (XSUM_XXH3_testdata). The lengths cover every XXH3
 * code path: empty, 1-3, 4-8, 9-16, 17-128, 129-240, and the long-input loop with and without a
 * partial stripe and block. */
static const xxh3_vector xxh3_vectors[] = {
    {0, 0, 0x2D06800538D394C2ULL},    {0, PRIME64, 0xA8A6B918B2F0364AULL},
    {1, 0, 0xC44BDFF4074EECDBULL},    {1, PRIME64, 0x032BE332DD766EF8ULL},
    {6, 0, 0x27B56A84CD2D7325ULL},    {6, PRIME64, 0x84589C116AB59AB9ULL},
    {12, 0, 0xA713DAF0DFBB77E7ULL},   {12, PRIME64, 0xE7303E1B2336DE0EULL},
    {24, 0, 0xA3FE70BF9D3510EBULL},   {24, PRIME64, 0x850E80FC35BDD690ULL},
    {48, 0, 0x397DA259ECBA1F11ULL},   {48, PRIME64, 0xADC2CBAA44ACC616ULL},
    {80, 0, 0xBCDEFBBB2C47C90AULL},   {80, PRIME64, 0xC6DD0CB699532E73ULL},
    {195, 0, 0xCD94217EE362EC3AULL},  {195, PRIME64, 0xBA68003D370CB3D9ULL},
    {403, 0, 0xCDEB804D65C6DEA4ULL},  {403, PRIME64, 0x6259F6ECFD6443FDULL},
    {512, 0, 0x617E49599013CB6BULL},  {512, PRIME64, 0x3CE457DE14C27708ULL},
    {2048, 0, 0xDD59E2C3A5F038E0ULL}, {2048, PRIME64, 0x66F81670669ABABCULL},
    {2240, 0, 0x6E73A90539CF2948ULL}, {2240, PRIME64, 0x757BA8487D1B5247ULL},
    {2367, 0, 0xCB37AEB9E5D361EDULL}, {2367, PRIME64, 0xD2DB3415B942B42AULL},
};

TEST(hash, xxh3_vectors) {
    fill_sanity_buffer();
    EXPECT_EQ(de_hash64("", 0, 0), 0x2D06800538D394C2ULL);
    EXPECT_EQ(de_hash64(NULL, 0, 0), 0x2D06800538D394C2ULL);
    for (size_t i = 0; i < sizeof xxh3_vectors / sizeof xxh3_vectors[0]; ++i) {
        const xxh3_vector *v = &xxh3_vectors[i];
        const u64 got = de_hash64(sanity_buf, v->len, v->seed);
        if (got != v->hash)
            printf("  vector len=%zu seed=0x%llx: got 0x%016llx want 0x%016llx\n", v->len,
                   (unsigned long long)v->seed, (unsigned long long)got,
                   (unsigned long long)v->hash);
        EXPECT_EQ(got, v->hash);
    }
}

/* The hash depends on the bytes only, never on their address. Every start offset in a 16-byte
 * window is used, so the SIMD loads of the long-input loop (> 240 bytes) see inputs that are not
 * vector-aligned. Under DE_SANITIZE=undefined this also catches a misaligned vector load
 * (DENG-358: xxHash's AArch64 GCC path dereferenced a uint64x2_t pointer). */
TEST(hash, unaligned_input) {
    fill_sanity_buffer();
    /* 16 bytes of slack, plus room to start at any offset of a 16-byte boundary. */
    static _Alignas(16) unsigned char shifted[SANITY_LEN + 16];
    for (size_t off = 0; off < 16; ++off) {
        memcpy(shifted + off, sanity_buf, SANITY_LEN);
        for (size_t i = 0; i < sizeof xxh3_vectors / sizeof xxh3_vectors[0]; ++i) {
            const xxh3_vector *v = &xxh3_vectors[i];
            const u64 got = de_hash64(shifted + off, v->len, v->seed);
            if (got != v->hash)
                printf("  offset=%zu len=%zu seed=0x%llx: got 0x%016llx want 0x%016llx\n", off,
                       v->len, (unsigned long long)v->seed, (unsigned long long)got,
                       (unsigned long long)v->hash);
            EXPECT_EQ(got, v->hash);
        }
    }
}

TEST(hash, str_helpers) {
    EXPECT_EQ(de_hash64_str(""), 0x2D06800538D394C2ULL);
    EXPECT_EQ(de_hash64_str(NULL), 0x2D06800538D394C2ULL);
    EXPECT_EQ(de_hash64_str("player/mesh"), de_hash64("player/mesh", 11, 0));
    /* Case and seed matter. */
    EXPECT_NE(de_hash64_str("Player/mesh"), de_hash64_str("player/mesh"));
    EXPECT_NE(de_hash64("player/mesh", 11, 1), de_hash64("player/mesh", 11, 0));
}

/* ---------- de_name ---------- */

TEST(name, hash_matches_de_hash64) {
    EXPECT_EQ(de_name_from("player/mesh").hash, de_hash64_str("player/mesh"));
    EXPECT_EQ(de_name_from("player/model#Body").hash, de_hash64_str("player/model#Body"));
    EXPECT_EQ(de_name_from("").hash, de_hash64_str(""));
    EXPECT_EQ(de_name_from(NULL).hash, de_hash64_str(""));
    /* A view hashes its bytes only, not what follows them. */
    const de_str view = {"player/mesh_extra", 11};
    EXPECT_EQ(de_name_from_view(view).hash, de_hash64_str("player/mesh"));
    const de_str null_view = {NULL, 0};
    EXPECT_EQ(de_name_from_view(null_view).hash, de_hash64_str(""));
}

#if DE_NAME_LOOKUP

TEST(name, roundtrip_dev) {
    const de_name n = de_name_from("player/mesh");
    const char *s = de_name_str(n);
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(s, "player/mesh");
    /* The table keeps its own copy. */
    char buf[32];
    snprintf(buf, sizeof buf, "level/%d", 7);
    const de_name m = de_name_from(buf);
    memset(buf, 'x', sizeof buf - 1);
    EXPECT_STREQ(de_name_str(m), "level/7");
    /* Interning again returns the same pointer and does not add an entry. */
    const size_t before = de__name_test_count();
    EXPECT_EQ(de_name_from("player/mesh").hash, n.hash);
    EXPECT_EQ(de__name_test_count(), before);
    EXPECT_TRUE(de_name_str(n) == s);
    /* View without NUL. */
    const de_str view = {"sound/step_and_more", 10};
    EXPECT_STREQ(de_name_str(de_name_from_view(view)), "sound/step");
    EXPECT_STREQ(de_name_str(de_name_from("")), "");
    /* Never interned. */
    const de_name unknown = {de_hash64_str("never/interned/name")};
    EXPECT_TRUE(de_name_str(unknown) == NULL);
}

TEST(name, many_names_and_long_strings) {
    /* Enough names to grow the table several times and fill several arena chunks. */
    const size_t before = de__name_test_count();
    char buf[64];
    for (int i = 0; i < 20000; ++i) {
        snprintf(buf, sizeof buf, "many/%d", i);
        de_name_from(buf);
    }
    EXPECT_EQ(de__name_test_count(), before + 20000);
    for (int i = 0; i < 20000; i += 997) {
        snprintf(buf, sizeof buf, "many/%d", i);
        EXPECT_STREQ(de_name_str(de_name_from(buf)), buf);
    }
    /* A string larger than an arena chunk. */
    static char big[100000];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    const de_name b = de_name_from(big);
    const char *s = de_name_str(b);
    ASSERT_TRUE(s != NULL);
    EXPECT_EQ(strlen(s), sizeof big - 1);
    EXPECT_STREQ(de_name_str(de_name_from("many/0")), "many/0");
}

/* 8 threads intern overlapping name sets at the same time. Each name must end up once in the
 * table, and every thread must read back its own strings. Run under DE_SANITIZE=thread. */
enum { THREADS = 8, SHARED_NAMES = 2000, OWN_NAMES = 500 };

typedef struct name_thread_arg {
    int id;
    int failures;
} name_thread_arg;

static i32 name_thread(void *user) {
    name_thread_arg *a = (name_thread_arg *)user;
    char buf[64];
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < SHARED_NAMES; ++i) {
            /* Different start points, so threads race on the same names. */
            const int k = (i + a->id * 250) % SHARED_NAMES;
            snprintf(buf, sizeof buf, "threads/shared/%d", k);
            const de_name n = de_name_from(buf);
            const char *s = de_name_str(n);
            if (n.hash != de_hash64_str(buf) || s == NULL || strcmp(s, buf) != 0)
                ++a->failures;
        }
        for (int i = 0; i < OWN_NAMES; ++i) {
            snprintf(buf, sizeof buf, "threads/own/%d/%d", a->id, i);
            const char *s = de_name_str(de_name_from(buf));
            if (s == NULL || strcmp(s, buf) != 0)
                ++a->failures;
        }
    }
    return 0;
}

TEST(name, threads) {
    const size_t before = de__name_test_count();
    de_thread *t[THREADS];
    name_thread_arg args[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        args[i].id = i;
        args[i].failures = 0;
        ASSERT_EQ(de_thread_create(name_thread, &args[i], "name_test", DE_THREAD_NORMAL, 0, &t[i]),
                  DE_OK);
    }
    for (int i = 0; i < THREADS; ++i) {
        de_thread_join(t[i]);
        EXPECT_EQ(args[i].failures, 0);
    }
    /* No duplicates: exactly one entry per distinct string. */
    EXPECT_EQ(de__name_test_count(), before + SHARED_NAMES + THREADS * OWN_NAMES);
}

/* A hash function that makes every string collide. */
static u64 colliding_hash(const void *data, size_t len) {
    (void)data;
    (void)len;
    return 0x00C0111DE00C0111DULL;
}

static void intern_colliding_pair(void) {
    de__name_test_set_hash(colliding_hash);
    de_name_from("collide/first");
    de_name_from("collide/first"); /* same string: fine */
    de_name_from("collide/second");
}

#if !defined(_WIN32)

static void abort_to_exit(int sig) {
    (void)sig;
    _exit(42);
}

/* Interning two different strings with the same hash panics. Forks, so the abort happens in the
 * child; SIGABRT becomes exit code 42 and stderr comes back through a pipe. */
TEST(name, collision_panics) {
    int err_pipe[2];
    ASSERT_EQ(pipe(err_pipe), 0);
    fflush(NULL);
    const pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        signal(SIGABRT, abort_to_exit);
        dup2(err_pipe[1], 2);
        close(err_pipe[0]);
        intern_colliding_pair();
        _exit(0); /* no panic: the parent sees exit code 0 */
    }
    close(err_pipe[1]);
    char err[1024];
    size_t got = 0;
    for (;;) {
        const ssize_t n = read(err_pipe[0], err + got, sizeof err - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    err[got] = '\0';
    close(err_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 42);
    EXPECT_TRUE(strstr(err, "hash collision") != NULL);
    EXPECT_TRUE(strstr(err, "\"collide/first\"") != NULL);
    EXPECT_TRUE(strstr(err, "\"collide/second\"") != NULL);
}

#else /* _WIN32: no fork. The panic hook longjmps out (name.c unlocks before it panics). */

static jmp_buf collision_jmp;
static char collision_message[512];

static void collision_hook(const de_panic_info *info, void *user) {
    (void)user;
    snprintf(collision_message, sizeof collision_message, "%s", info->message);
    longjmp(collision_jmp, 1);
}

TEST(name, collision_panics) {
    de_panic_set_hook(collision_hook, NULL);
    if (setjmp(collision_jmp) == 0)
        intern_colliding_pair();
    de_panic_set_hook(NULL, NULL);
    de__name_test_set_hash(NULL);
    EXPECT_TRUE(strstr(collision_message, "hash collision") != NULL);
    /* The table is still usable. */
    EXPECT_STREQ(de_name_str(de_name_from("after/collision")), "after/collision");
}

#endif /* _WIN32 */

#else /* !DE_NAME_LOOKUP */

TEST(name, shipping_returns_null) {
    const de_name n = de_name_from("player/mesh");
    EXPECT_EQ(n.hash, de_hash64_str("player/mesh"));
    EXPECT_TRUE(de_name_str(n) == NULL);
    EXPECT_TRUE(de_name_str(de_name_from("")) == NULL);
    EXPECT_EQ(de__name_test_count(), 0u);
}

#endif /* DE_NAME_LOOKUP */
