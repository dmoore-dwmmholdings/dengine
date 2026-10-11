/* Unit tests: de_hashmap (DENG-151). Hashes come from a local function, not DENG-153. */
#include "de_test.h"

#include "core/de_hashmap.h"
#include "test_counting_alloc.h"

#include <stdlib.h>
#include <string.h>

/* splitmix64 finaliser: a bijection on u64 with good mixing. */
static u64 hm_mix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* FNV-1a over bytes, then mixed: a trivial local hash for byte keys. */
static u64 hm_hash_bytes(const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    u64 h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return hm_mix(h);
}

static u64 hm_hash_u64(u64 k) {
    return hm_hash_bytes(&k, sizeof k);
}

static u64 hm_rng_next(u64 *s) {
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static void hm_check_clean(const test_counting_alloc *ca) {
    EXPECT_EQ(ca->live_blocks, 0u);
    EXPECT_EQ(ca->live_bytes, 0u);
    EXPECT_EQ(ca->size_errors, 0u);
    EXPECT_EQ(ca->align_errors, 0u);
}

/* ---- Randomized test against a reference -------------------------------------------- */

typedef struct hm_kv {
    u64 key;
    u64 value;
} hm_kv;

static int hm_kv_cmp(const void *a, const void *b) {
    const u64 x = ((const hm_kv *)a)->key, y = ((const hm_kv *)b)->key;
    return x < y ? -1 : x > y;
}

enum { HM_KEY_SPACE = 1 << 16 };

/* Key of reference slot `id`: a bijection, so ids map to distinct, scattered keys. */
static u64 hm_key_of(u32 id) {
    return hm_mix((u64)id * 0x632BE59BD9B4E019ull);
}

/* Compares the whole map with the reference: count, and the sorted (key, value) lists. */
static void hm_compare_sorted(const de_hashmap *m, const bool *present, const u64 *values,
                              hm_kv *scratch_a, hm_kv *scratch_b) {
    u32 n_ref = 0;
    for (u32 id = 0; id < HM_KEY_SPACE; ++id)
        if (present[id])
            scratch_a[n_ref++] = (hm_kv){hm_key_of(id), values[id]};
    ASSERT_EQ(de_hashmap_count(m), n_ref);
    u32 n_map = 0;
    for (de_hashmap_iter it = de_hashmap_iter_begin(m); de_hashmap_iter_next(&it);) {
        ASSERT_LT(n_map, n_ref);
        memcpy(&scratch_b[n_map].key, it.key, sizeof(u64));
        memcpy(&scratch_b[n_map].value, it.value, sizeof(u64));
        ++n_map;
    }
    ASSERT_EQ(n_map, n_ref);
    qsort(scratch_a, n_ref, sizeof(hm_kv), hm_kv_cmp);
    qsort(scratch_b, n_map, sizeof(hm_kv), hm_kv_cmp);
    ASSERT_MEMEQ(scratch_a, scratch_b, sizeof(hm_kv) * n_ref);
}

TEST(hashmap, random_vs_reference) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_hashmap m;
    ASSERT_EQ(DE_HASHMAP_INIT(&m, u64, u64, &ca.iface), DE_OK);
    EXPECT_EQ(test_counting_calls(&ca), 0u);

    bool *present = (bool *)calloc(HM_KEY_SPACE, sizeof(bool));
    u64 *values = (u64 *)calloc(HM_KEY_SPACE, sizeof(u64));
    hm_kv *sa = (hm_kv *)malloc(sizeof(hm_kv) * HM_KEY_SPACE);
    hm_kv *sb = (hm_kv *)malloc(sizeof(hm_kv) * HM_KEY_SPACE);
    ASSERT_TRUE(present && values && sa && sb);

    u64 rng = 12345;
    u32 live = 0;
    u64 removes = 0, reinserts = 0;
    bool *ever = (bool *)calloc(HM_KEY_SPACE, sizeof(bool));
    ASSERT_TRUE(ever != NULL);
    for (u32 op = 0; op < 1000000u; ++op) {
        const u64 r = hm_rng_next(&rng);
        /* Phases move the live size up and down so the map grows and churns. */
        const u32 phase = (op / 125000u) & 1u;
        /* The low id bits are reused across phases so removed keys come back. */
        const u32 id = (u32)(r >> 40) % HM_KEY_SPACE;
        const u64 key = hm_key_of(id);
        const u64 h = hm_hash_u64(key);
        const u32 kind = (u32)(r % 100u);
        const u32 put_pct = phase ? 30u : 60u;
        const u32 cap_before = de_hashmap_capacity(&m);
        if (kind < put_pct) {
            const u64 v = r ^ op;
            ASSERT_EQ(de_hashmap_put(&m, h, &key, &v), DE_OK);
            if (!present[id]) {
                ++live;
                if (ever[id])
                    ++reinserts;
            }
            present[id] = true;
            ever[id] = true;
            values[id] = v;
        } else if (kind < put_pct + 25u) {
            u64 out = 0;
            const bool removed = de_hashmap_remove(&m, h, &key, &out);
            ASSERT_EQ(removed, present[id]);
            if (removed) {
                ASSERT_EQ(out, values[id]);
                present[id] = false;
                --live;
                ++removes;
            }
            ASSERT_EQ(de_hashmap_capacity(&m), cap_before); /* remove never resizes */
        } else {
            const u64 *v = DE_HASHMAP_GET(&m, u64, h, &key);
            if (present[id]) {
                ASSERT_TRUE(v != NULL);
                ASSERT_EQ(*v, values[id]);
            } else {
                ASSERT_TRUE(v == NULL);
            }
            ASSERT_EQ(de_hashmap_contains(&m, h, &key), present[id]);
            ASSERT_EQ(de_hashmap_capacity(&m), cap_before); /* get never resizes */
        }
        ASSERT_EQ(de_hashmap_count(&m), live);
        /* Load invariant: live + tombstones <= 0.75 capacity. */
        ASSERT_LE((u64)m.count + m.tombstones, (u64)m.capacity * 3u / 4u);
        if (op % 100000u == 99999u)
            hm_compare_sorted(&m, present, values, sa, sb);
    }
    hm_compare_sorted(&m, present, values, sa, sb);
    EXPECT_GT(removes, 100000u);
    EXPECT_GT(reinserts, 10000u);

    de_hashmap_free(&m);
    EXPECT_EQ(de_hashmap_capacity(&m), 0u);
    hm_check_clean(&ca);
    free(present);
    free(values);
    free(ever);
    free(sa);
    free(sb);
}

/* ---- Adversarial hashes --------------------------------------------------------------- */

TEST(hashmap, collisions) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    /* Every key with the same hash; then the same home slot (low bits 0) but different
     * fingerprints (top 7 bits). */
    const u64 hash_kinds[1] = {0x1234567890ABCDEFull};
    const u32 missing = 0xFFFFFFFFu;
    for (int kind = 0; kind < 2; ++kind) {
        de_hashmap m;
        ASSERT_EQ(DE_HASHMAP_INIT(&m, u32, u32, &ca.iface), DE_OK);
        const u32 n = 3000;
#define HM_H(k) (kind == 0 ? hash_kinds[0] : ((u64)(k) << 50))
        for (u32 k = 0; k < n; ++k) {
            const u32 v = k * 3u;
            ASSERT_EQ(de_hashmap_put(&m, HM_H(k), &k, &v), DE_OK);
        }
        ASSERT_EQ(de_hashmap_count(&m), n);
        for (u32 k = 0; k < n; ++k) {
            const u32 *v = DE_HASHMAP_GET(&m, u32, HM_H(k), &k);
            ASSERT_TRUE(v != NULL);
            ASSERT_EQ(*v, k * 3u);
        }
        /* Missing keys with the colliding hash: the probe must end. */
        for (u32 k = n; k < n + 100u; ++k)
            ASSERT_TRUE(de_hashmap_get(&m, HM_H(k), &k) == NULL);
        for (u32 k = 0; k < n; k += 2)
            ASSERT_TRUE(de_hashmap_remove(&m, HM_H(k), &k, NULL));
        for (u32 k = 0; k < n; ++k)
            ASSERT_EQ(de_hashmap_contains(&m, HM_H(k), &k), (k & 1u) != 0);
        for (u32 k = 0; k < n; k += 2) {
            const u32 v = k + 7u;
            ASSERT_EQ(de_hashmap_put(&m, HM_H(k), &k, &v), DE_OK);
        }
        ASSERT_EQ(de_hashmap_count(&m), n);
        for (u32 k = 0; k < n; ++k) {
            const u32 *v = DE_HASHMAP_GET(&m, u32, HM_H(k), &k);
            ASSERT_TRUE(v != NULL);
            ASSERT_EQ(*v, (k & 1u) ? k * 3u : k + 7u);
        }
        /* Remove everything and churn: tombstones pile up on one probe path. */
        for (u32 k = 0; k < n; ++k)
            ASSERT_TRUE(de_hashmap_remove(&m, HM_H(k), &k, NULL));
        ASSERT_EQ(de_hashmap_count(&m), 0u);
        for (u32 round = 0; round < 20000u; ++round) {
            const u32 k = round, old = round - 10u;
            ASSERT_EQ(de_hashmap_put(&m, HM_H(k), &k, NULL), DE_OK);
            if (round >= 10u)
                ASSERT_TRUE(de_hashmap_remove(&m, HM_H(old), &old, NULL));
            ASSERT_TRUE(de_hashmap_get(&m, HM_H(missing), &missing) == NULL);
        }
        ASSERT_EQ(de_hashmap_count(&m), 10u);
#undef HM_H
        de_hashmap_free(&m);
    }
    hm_check_clean(&ca);
}

/* ---- Key sizes ------------------------------------------------------------------------ */

static void hm_make_key(u8 *out, u32 key_size, u32 id) {
    /* The first 4 bytes are the id (unique); the rest is derived from it. */
    memcpy(out, &id, 4);
    u64 x = hm_mix(id);
    for (u32 i = 4; i < key_size; ++i) {
        out[i] = (u8)x;
        x = (x >> 8) | (x << 56);
    }
}

static bool hm_str_eq(const void *a, const void *b, u32 key_size, void *user) {
    (void)key_size;
    (void)user;
    const char *sa, *sb;
    memcpy(&sa, a, sizeof sa);
    memcpy(&sb, b, sizeof sb);
    return strcmp(sa, sb) == 0;
}

TEST(hashmap, key_sizes) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    const u32 sizes[4] = {4, 8, 16, 40};
    for (int s = 0; s < 4; ++s) {
        const u32 ks = sizes[s];
        de_hashmap m;
        de_hashmap_desc d = {0};
        d.allocator = &ca.iface;
        d.key_size = ks;
        d.value_size = sizeof(u32);
        ASSERT_EQ(de_hashmap_init(&m, &d), DE_OK);
        const u32 n = 20000;
        u8 key[40];
        for (u32 id = 0; id < n; ++id) {
            hm_make_key(key, ks, id);
            ASSERT_EQ(de_hashmap_put(&m, hm_hash_bytes(key, ks), key, &id), DE_OK);
        }
        ASSERT_EQ(de_hashmap_count(&m), n);
        for (u32 id = 0; id < n; id += 2) {
            hm_make_key(key, ks, id);
            u32 out = 0;
            ASSERT_TRUE(de_hashmap_remove(&m, hm_hash_bytes(key, ks), key, &out));
            ASSERT_EQ(out, id);
        }
        for (u32 id = 0; id < n; id += 2) {
            hm_make_key(key, ks, id);
            const u32 v = id + 1000000u;
            ASSERT_EQ(de_hashmap_put(&m, hm_hash_bytes(key, ks), key, &v), DE_OK);
        }
        for (u32 id = 0; id < n + 1000u; ++id) {
            hm_make_key(key, ks, id);
            const u32 *v = DE_HASHMAP_GET(&m, u32, hm_hash_bytes(key, ks), key);
            if (id >= n) {
                ASSERT_TRUE(v == NULL);
                continue;
            }
            ASSERT_TRUE(v != NULL);
            ASSERT_EQ(*v, (id & 1u) ? id : id + 1000000u);
            /* Keys and values keep their alignment. */
            ASSERT_EQ((uintptr_t)v % sizeof(u32), 0u);
        }
        /* Overwriting an existing key keeps the count. */
        hm_make_key(key, ks, 1);
        bool inserted = true;
        void *vp = NULL;
        ASSERT_EQ(de_hashmap_get_or_put(&m, hm_hash_bytes(key, ks), key, &vp, &inserted), DE_OK);
        EXPECT_FALSE(inserted);
        EXPECT_EQ(*(u32 *)vp, 1u);
        EXPECT_EQ(de_hashmap_count(&m), n);
        de_hashmap_free(&m);
    }

    /* A set (value_size 0) of string pointers with a custom compare. */
    de_hashmap set;
    de_hashmap_desc d = {0};
    d.allocator = &ca.iface;
    d.key_size = sizeof(const char *);
    d.key_eq = hm_str_eq;
    ASSERT_EQ(de_hashmap_init(&set, &d), DE_OK);
    static const char *const names[3] = {"alpha", "beta", "gamma"};
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(de_hashmap_put(&set, hm_hash_bytes(names[i], strlen(names[i])), &names[i], NULL),
                  DE_OK);
    char buf[8];
    memcpy(buf, "beta", 5);
    const char *probe = buf; /* a different pointer to an equal string */
    const char *const *stored =
        (const char *const *)de_hashmap_get(&set, hm_hash_bytes("beta", 4), &probe);
    ASSERT_TRUE(stored != NULL);
    EXPECT_TRUE(*stored == names[1]);
    EXPECT_TRUE(de_hashmap_remove(&set, hm_hash_bytes("beta", 4), &probe, NULL));
    EXPECT_EQ(de_hashmap_count(&set), 2u);
    de_hashmap_free(&set);

    /* Invalid descs. */
    de_hashmap bad;
    de_hashmap_desc z = {0};
    EXPECT_EQ(de_hashmap_init(&bad, &z), DE_ERR_INVALID_ARGUMENT);
    z.allocator = &ca.iface;
    EXPECT_EQ(de_hashmap_init(&bad, &z), DE_ERR_INVALID_ARGUMENT);
    z.key_size = 8;
    z.key_align = 3;
    EXPECT_EQ(de_hashmap_init(&bad, &z), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();

    /* A NULL allocator means the default heap. */
    de_hashmap heap_map;
    ASSERT_EQ(DE_HASHMAP_INIT(&heap_map, u64, u64, NULL), DE_OK);
    EXPECT_TRUE(heap_map.a == de_heap_allocator());
    for (u64 k = 0; k < 100; k++)
        ASSERT_EQ(de_hashmap_put(&heap_map, k * 0x9E3779B97F4A7C15ull, &k, &k), DE_OK);
    EXPECT_EQ(de_hashmap_count(&heap_map), 100u);
    de_hashmap_free(&heap_map);
    hm_check_clean(&ca);
}

/* ---- Tombstones, capacity policy ------------------------------------------------------- */

TEST(hashmap, tombstone_reuse) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_hashmap m;
    ASSERT_EQ(DE_HASHMAP_INIT(&m, u64, u64, &ca.iface), DE_OK);

    /* A tombstone on the probe path is reused by the next insert on that path. */
    const u64 h = 77; /* same hash for a, b, c: they share a probe path */
    const u64 a = 1, b = 2, c = 3;
    ASSERT_EQ(de_hashmap_put(&m, h, &a, &a), DE_OK);
    ASSERT_EQ(de_hashmap_put(&m, h, &b, &b), DE_OK);
    ASSERT_TRUE(de_hashmap_remove(&m, h, &a, NULL)); /* b follows a: a becomes a tombstone */
    EXPECT_EQ(m.tombstones, 1u);
    ASSERT_EQ(de_hashmap_put(&m, h, &c, &c), DE_OK);
    EXPECT_EQ(m.tombstones, 0u);
    ASSERT_TRUE(de_hashmap_remove(&m, h, &b, NULL));
    ASSERT_TRUE(de_hashmap_remove(&m, h, &c, NULL));
    EXPECT_EQ(de_hashmap_count(&m), 0u);

    /* reserve sizes once; filling, emptying and refilling to that size allocates nothing. */
    ASSERT_EQ(de_hashmap_reserve(&m, 1000), DE_OK);
    const u32 cap = de_hashmap_capacity(&m);
    EXPECT_GE((u64)cap * 3u / 4u, 1000u);
    const u64 calls = test_counting_calls(&ca);
    for (u64 k = 0; k < 1000; ++k)
        ASSERT_EQ(de_hashmap_put(&m, hm_hash_u64(k), &k, &k), DE_OK);
    for (u64 k = 0; k < 1000; ++k)
        ASSERT_TRUE(de_hashmap_remove(&m, hm_hash_u64(k), &k, NULL));
    EXPECT_GT(m.tombstones, 0u);
    for (u64 k = 5000; k < 6000; ++k)
        ASSERT_EQ(de_hashmap_put(&m, hm_hash_u64(k), &k, &k), DE_OK);
    EXPECT_EQ(de_hashmap_capacity(&m), cap);
    EXPECT_EQ(test_counting_calls(&ca), calls);

    /* Long churn at a small live size: tombstones are dropped in place, never by growing. */
    de_hashmap_clear(&m);
    EXPECT_EQ(de_hashmap_capacity(&m), cap); /* clear keeps the capacity */
    EXPECT_EQ(m.tombstones, 0u);
    for (u64 k = 0; k < 200000; ++k) {
        ASSERT_EQ(de_hashmap_put(&m, hm_hash_u64(k), &k, &k), DE_OK);
        if (k >= 64) {
            const u64 old = k - 64;
            ASSERT_TRUE(de_hashmap_remove(&m, hm_hash_u64(old), &old, NULL));
        }
    }
    EXPECT_EQ(de_hashmap_count(&m), 64u);
    EXPECT_EQ(de_hashmap_capacity(&m), cap);
    EXPECT_EQ(test_counting_calls(&ca), calls);
    for (u64 k = 200000 - 64; k < 200000; ++k) {
        const u64 *v = DE_HASHMAP_GET(&m, u64, hm_hash_u64(k), &k);
        ASSERT_TRUE(v != NULL);
        ASSERT_EQ(*v, k);
    }
    ASSERT_TRUE(de_hashmap_get(&m, hm_hash_u64(0), &(u64){0}) == NULL);

    /* reserve never shrinks; free gives everything back. */
    ASSERT_EQ(de_hashmap_reserve(&m, 4), DE_OK);
    EXPECT_EQ(de_hashmap_capacity(&m), cap);
    de_hashmap_free(&m);
    hm_check_clean(&ca);
}

/* ---- Iteration, OOM ------------------------------------------------------------------- */

TEST(hashmap, iteration_counter) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_hashmap m;
    ASSERT_EQ(DE_HASHMAP_INIT(&m, u32, u32, &ca.iface), DE_OK);
    for (u32 k = 0; k < 500; ++k)
        ASSERT_EQ(de_hashmap_put(&m, hm_hash_u64(k), &k, &k), DE_OK);

    const u64 before = de_hashmap_iter_count();
    u32 seen = 0;
    u64 sum = 0;
    for (de_hashmap_iter it = de_hashmap_iter_begin(&m); de_hashmap_iter_next(&it);) {
        u32 k;
        memcpy(&k, it.key, sizeof k);
        ++seen;
        sum += k;
        /* Removing the current entry during the iteration is allowed. */
        if (k % 3u == 0)
            ASSERT_TRUE(de_hashmap_remove(&m, hm_hash_u64(k), &k, NULL));
    }
    EXPECT_EQ(seen, 500u);
    EXPECT_EQ(sum, 499u * 500u / 2u);
    EXPECT_EQ(de_hashmap_count(&m), 500u - 167u);
    EXPECT_EQ(de_hashmap_iter_count(), before + (DE_HASHMAP_ITER_TRACKING ? 1u : 0u));

    /* An empty map iterates nothing (and still counts). */
    de_hashmap empty;
    ASSERT_EQ(DE_HASHMAP_INIT(&empty, u32, u32, &ca.iface), DE_OK);
    de_hashmap_iter it = de_hashmap_iter_begin(&empty);
    EXPECT_FALSE(de_hashmap_iter_next(&it));
    EXPECT_EQ(de_hashmap_iter_count(), before + (DE_HASHMAP_ITER_TRACKING ? 2u : 0u));
    de_hashmap_free(&empty);
    de_hashmap_free(&m);
    hm_check_clean(&ca);
}

TEST(hashmap, oom) {
    de_hashmap m;
    ASSERT_EQ(DE_HASHMAP_INIT(&m, u64, u64, de_allocator_null()), DE_OK);
    const u64 k = 1;
    de_error_clear();
    EXPECT_EQ(de_hashmap_put(&m, hm_hash_u64(k), &k, &k), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_hashmap_reserve(&m, 10), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_hashmap_count(&m), 0u);
    EXPECT_TRUE(de_hashmap_get(&m, hm_hash_u64(k), &k) == NULL);
    EXPECT_FALSE(de_hashmap_remove(&m, hm_hash_u64(k), &k, NULL));
    de_hashmap_free(&m);

    /* A failed grow leaves every entry in place. */
    test_counting_alloc ca;
    test_counting_init(&ca);
    ASSERT_EQ(DE_HASHMAP_INIT(&m, u64, u64, &ca.iface), DE_OK);
    ca.fail_after = 2;
    u64 n = 0;
    de_result r;
    while ((r = de_hashmap_put(&m, hm_hash_u64(n), &n, &n)) == DE_OK)
        ++n;
    EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_hashmap_count(&m), (u32)n);
    for (u64 i = 0; i < n; ++i) {
        const u64 *v = DE_HASHMAP_GET(&m, u64, hm_hash_u64(i), &i);
        ASSERT_TRUE(v != NULL);
        ASSERT_EQ(*v, i);
    }
    /* Overwrites and removes still work while the allocator fails. */
    const u64 z = 0, nine = 9;
    EXPECT_EQ(de_hashmap_put(&m, hm_hash_u64(z), &z, &nine), DE_OK);
    EXPECT_EQ(*DE_HASHMAP_GET(&m, u64, hm_hash_u64(z), &z), 9u);
    de_hashmap_free(&m);
    hm_check_clean(&ca);
    de_error_clear();
}

TEST(hashmap, inplace_rehash_wraparound) {
    /* Drives the in-place tombstone drop through its swap case: entries whose probe path wraps
     * from the last slot to slot 0. Hashes pick the home slot directly (low 4 bits). */
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_hashmap m;
    ASSERT_EQ(DE_HASHMAP_INIT(&m, u64, u64, &ca.iface), DE_OK);
    ASSERT_EQ(de_hashmap_reserve(&m, 12), DE_OK);
    ASSERT_EQ(de_hashmap_capacity(&m), 16u);
#define HM_HOME(k, home) (((u64)(k) << 32) | (home))
    /* a, b, c live in slots 15, 0, 1 (all home 15); fillers 100..108 in slots 2..10. */
    for (u64 k = 1; k <= 3; ++k)
        ASSERT_EQ(de_hashmap_put(&m, HM_HOME(k, 15), &k, &k), DE_OK);
    for (u64 k = 100; k < 109; ++k)
        ASSERT_EQ(de_hashmap_put(&m, HM_HOME(k, k - 98), &k, &k), DE_OK);
    ASSERT_EQ(de_hashmap_count(&m), 12u);
    /* Removing the fillers front to back leaves 8 tombstones (the last one becomes EMPTY). */
    for (u64 k = 100; k < 109; ++k)
        ASSERT_TRUE(de_hashmap_remove(&m, HM_HOME(k, k - 98), &k, NULL));
    ASSERT_EQ(m.tombstones, 8u);
    const u64 calls = test_counting_calls(&ca);
    const u64 d = 4, e = 5;
    ASSERT_EQ(de_hashmap_put(&m, HM_HOME(d, 11), &d, &d), DE_OK); /* fills the load to 12 */
    ASSERT_EQ(m.tombstones, 8u);
    ASSERT_EQ(de_hashmap_put(&m, HM_HOME(e, 12), &e, &e), DE_OK); /* over: rehash in place */
    EXPECT_EQ(m.tombstones, 0u);
    EXPECT_EQ(de_hashmap_capacity(&m), 16u);
    EXPECT_EQ(test_counting_calls(&ca), calls);
    const u64 homes[6] = {0, 15, 15, 15, 11, 12};
    for (u64 k = 1; k <= 5; ++k) {
        const u64 *v = DE_HASHMAP_GET(&m, u64, HM_HOME(k, homes[k]), &k);
        ASSERT_TRUE(v != NULL);
        ASSERT_EQ(*v, k);
    }
    for (u64 k = 100; k < 109; ++k)
        ASSERT_FALSE(de_hashmap_contains(&m, HM_HOME(k, k - 98), &k));
    /* Every entry sits on its probe path: home, then FULL slots up to it. */
    for (u32 i = 0; i < 16; ++i) {
        if (!(m.ctrl[i] & 0x80u))
            continue;
        for (u32 s = (u32)m.hashes[i] & 15u; s != i; s = (s + 1u) & 15u)
            ASSERT_TRUE((m.ctrl[s] & 0x80u) != 0);
    }
#undef HM_HOME
    de_hashmap_free(&m);
    hm_check_clean(&ca);
}

TEST(hashmap, churn_wrapping_clusters) {
    /* Random churn on small tables where every home slot is near the end, so clusters wrap to
     * slot 0 and in-place tombstone drops hit every case. Checked against a reference. */
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_hashmap m;
    ASSERT_EQ(DE_HASHMAP_INIT(&m, u32, u32, &ca.iface), DE_OK);
    enum { KEYS = 24 };
    bool present[KEYS] = {false};
    u32 values[KEYS] = {0};
    u32 live = 0;
    u64 rng = 99;
    for (u32 op = 0; op < 300000u; ++op) {
        const u64 r = hm_rng_next(&rng);
        const u32 k = (u32)(r >> 33) % KEYS;
        /* Bits 1..4 set: home slot 14/15 at capacity 16, 30/31 at 32, 30/31 at 64. */
        const u64 h = hm_mix(k) | 0x1Eu;
        if ((r & 3u) != 0 && (r & 4u) != 0) {
            const u32 v = (u32)r;
            ASSERT_EQ(de_hashmap_put(&m, h, &k, &v), DE_OK);
            live += present[k] ? 0u : 1u;
            present[k] = true;
            values[k] = v;
        } else {
            ASSERT_EQ(de_hashmap_remove(&m, h, &k, NULL), present[k]);
            live -= present[k] ? 1u : 0u;
            present[k] = false;
        }
        ASSERT_EQ(de_hashmap_count(&m), live);
        if ((op & 15u) == 0) {
            for (u32 q = 0; q < KEYS; ++q) {
                const u32 *v = DE_HASHMAP_GET(&m, u32, hm_mix(q) | 0x1Eu, &q);
                ASSERT_EQ(v != NULL, present[q]);
                if (v)
                    ASSERT_EQ(*v, values[q]);
            }
        }
    }
    EXPECT_LE(de_hashmap_capacity(&m), 64u);
    de_hashmap_free(&m);
    hm_check_clean(&ca);
}
