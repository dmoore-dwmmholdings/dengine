/*
 * test_settings_hash.c: the canonical settings hash of the cooker (DENG-209; de_cook_settings_hash
 * in tools/cook/lib/de_cooker.h). Manifests are parsed from memory. CTest names
 * cooker.settings_hash.<test>.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "asset/manifest_internal.h"
#include "cook/lib/de_cooker.h"

#include <stdio.h>
#include <string.h>

static const char *g_text;

static de_result mem_read(void *user, const char *vpath, const de_allocator *a, void **data,
                          u64 *size) {
    (void)user;
    (void)vpath;
    size_t n = strlen(g_text);
    char *b = (char *)DE_ALLOC(a, n + 1, 1, DE_MEM_GENERAL);
    if (b == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "mem_read");
    memcpy(b, g_text, n + 1);
    *data = b;
    *size = n;
    return DE_OK;
}

/* The settings hash of asset `id` in the manifest `text`. */
static u64 hash_of(const char *text, const char *id) {
    g_text = text;
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {.path = "mem:/assets.toml", .read = mem_read};
    ASSERT_EQ(de__manifest_parse(&d, &m), DE_OK);
    if (m->error_count != 0)
        for (u32 i = 0; i < m->issue_count; ++i)
            printf("  %s\n", m->issues[i].message);
    ASSERT_EQ(m->error_count, 0u);
    const de__manifest_asset *a = de__manifest_find_asset(m, id);
    ASSERT_TRUE(a != NULL);
    u64 h = de_cook_settings_hash(a->settings, a->setting_count);
    de__manifest_destroy(m);
    return h;
}

/* ---- cooker.settings_hash.order_independent ------------------------------------------------- */

TEST(settings_hash, order_independent) {
    u64 a = hash_of("[defaults.t]\n"
                    "mips = true\n"
                    "[assets]\n"
                    "\"a\" = { type = \"t\", source = \"a.png\", zeta = 1, alpha = \"s\", "
                    "list = [\"p\", \"q\"], ratio = 0.5 }\n",
                    "a");
    u64 b = hash_of("# the same settings, written another way\n"
                    "[assets.\"a\"]\n"
                    "ratio=0.5\n"
                    "list = [ \"p\",\n"
                    "   \"q\" ]   # comment\n"
                    "type = \"t\"\n"
                    "alpha   =   's'\n"
                    "source = \"a.png\"\n"
                    "mips = true\n"
                    "zeta=+1\n",
                    "a");
    EXPECT_EQ(a, b);
    /* Another source or type does not touch the settings hash (they are not settings). */
    u64 c = hash_of("[assets]\n"
                    "\"a\" = { type = \"u\", source = \"b.png\", zeta = 1, alpha = \"s\", "
                    "list = [\"p\", \"q\"], ratio = 0.5, mips = true }\n",
                    "a");
    EXPECT_EQ(a, c);

    /* The function sorts by itself: any order of the same list gives the same hash. */
    const char *const items[] = {"p", "q"};
    de__manifest_kv kv[] = {
        {"zeta", {.type = DE__MANIFEST_INT, .u.i = 1}},
        {"alpha", {.type = DE__MANIFEST_STRING, .u.s = "s"}},
        {"mips", {.type = DE__MANIFEST_BOOL, .u.b = true}},
        {"list", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {items, 2}}},
        {"ratio", {.type = DE__MANIFEST_FLOAT, .u.f = 0.5}},
    };
    EXPECT_EQ(de_cook_settings_hash(kv, 5), a);
    de__manifest_kv rev[5];
    for (u32 i = 0; i < 5; ++i)
        rev[i] = kv[4 - i];
    EXPECT_EQ(de_cook_settings_hash(rev, 5), a);
    EXPECT_EQ(de_cook_settings_hash(NULL, 0), de_cook_settings_hash(kv, 0));
}

/* ---- cooker.settings_hash.type_sensitive ---------------------------------------------------- */

TEST(settings_hash, type_sensitive) {
    const char *variants[] = {
        "x = 1",
        "x = 1.0",
        "x = \"1\"",
        "x = true",
        "x = false",
        "x = 2",
        "x = [\"1\"]",
        "y = 1",
        "x = 1, y = 1",
        "x = -0.0",
        "x = 0.0",
        "x = [\"p\", \"q\"]",
        "x = [\"q\", \"p\"]",
        "x = [\"pq\"]",
        "x = [\"p\", \"\", \"q\"]",
        "x = \"\"",
    };
    enum { N = (int)(sizeof variants / sizeof variants[0]) };
    u64 h[N];
    for (int i = 0; i < N; ++i) {
        char text[256];
        snprintf(text, sizeof text, "[assets]\n\"a\" = { type = \"t\", source = \"a\", %s }\n",
                 variants[i]);
        h[i] = hash_of(text, "a");
    }
    for (int i = 0; i < N; ++i)
        for (int k = i + 1; k < N; ++k) {
            EXPECT_NE(h[i], h[k]);
            if (h[i] == h[k])
                printf("  same hash: \"%s\" and \"%s\"\n", variants[i], variants[k]);
        }
}

/* ---- cooker.settings_hash.encoding ----------------------------------------------------------
 * The documented encoding, written out by hand: the hash is part of the `.decache` key, so a
 * change to it must be deliberate (and bump the cache version). */

static size_t put32(u8 *p, u32 v) {
    for (u32 i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (8u * i));
    return 4;
}

static size_t put64(u8 *p, u64 v) {
    for (u32 i = 0; i < 8; ++i)
        p[i] = (u8)(v >> (8u * i));
    return 8;
}

static size_t putstr(u8 *p, const char *s) {
    size_t n = strlen(s);
    size_t k = put32(p, (u32)n);
    memcpy(p + k, s, n);
    return k + n;
}

TEST(settings_hash, encoding) {
    const char *const items[] = {"bc7", ""};
    f64 ratio = -2.25;
    const de__manifest_kv kv[] = {
        {"srgb", {.type = DE__MANIFEST_BOOL, .u.b = true}},
        {"mips", {.type = DE__MANIFEST_INT, .u.i = -3}},
        {"formats", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {items, 2}}},
        {"ratio", {.type = DE__MANIFEST_FLOAT, .u.f = ratio}},
        {"name", {.type = DE__MANIFEST_STRING, .u.s = "hero"}},
    };
    u8 buf[256];
    size_t n = 0;
    memcpy(buf, "DESH", 4);
    n = 4;
    n += put32(buf + n, 1); /* version */
    n += put32(buf + n, 5); /* count */
    n += putstr(buf + n, "formats");
    buf[n++] = 5;
    n += put32(buf + n, 2);
    n += putstr(buf + n, "bc7");
    n += putstr(buf + n, "");
    n += putstr(buf + n, "mips");
    buf[n++] = 2;
    n += put64(buf + n, (u64)(i64)-3);
    n += putstr(buf + n, "name");
    buf[n++] = 4;
    n += putstr(buf + n, "hero");
    n += putstr(buf + n, "ratio");
    buf[n++] = 3;
    u64 bits = 0;
    memcpy(&bits, &ratio, sizeof bits);
    n += put64(buf + n, bits);
    n += putstr(buf + n, "srgb");
    buf[n++] = 1;
    buf[n++] = 1;
    EXPECT_EQ(de_cook_settings_hash(kv, 5), de_hash64(buf, n, 0));

    u8 empty[12];
    memcpy(empty, "DESH", 4);
    put32(empty + 4, 1);
    put32(empty + 8, 0);
    EXPECT_EQ(de_cook_settings_hash(NULL, 0), de_hash64(empty, sizeof empty, 0));
}

TEST(settings_hash, nested_arrays_preserve_types_shape_and_order) {
    u64 h = hash_of("[assets.a]\ntype='t'\nsource='s'\nranges=[[256,591],[65,67]]\n", "a");
    EXPECT_EQ(
        h, hash_of("[assets.a]\ntype='t'\nsource='s'\nranges = [ [0x100,0x24f], [65,67] ]\n", "a"));
    const char *different[] = {"[[256,592],[65,67]]", "[[65,67],[256,591]]", "[256,591,65,67]",
                               "[[256.0,591],[65,67]]", "['256-591','65-67']"};
    for (u32 i = 0; i < (sizeof different / sizeof different[0]); ++i) {
        char text[256];
        snprintf(text, sizeof text, "[assets.a]\ntype='t'\nsource='s'\nranges=%s\n", different[i]);
        EXPECT_NE(h, hash_of(text, "a"));
    }
}
