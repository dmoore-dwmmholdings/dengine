/*
 * test_manifest_diff.c: the per-asset digest of a manifest and the diff of two manifests that hot
 * reload uses (manifest_diff.c; DENG-214). Manifests are parsed from memory.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "asset/hot_reload_internal.h"
#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct md_parsed {
    de__manifest *m;
    de__resolved *r;
    de__manifest_digest d;
} md_parsed;

static const char *g_md_text;

static de_result md_read(void *user, const char *vpath, const de_allocator *a, void **data,
                         u64 *size) {
    (void)user;
    (void)vpath;
    size_t n = strlen(g_md_text);
    char *b = (char *)DE_ALLOC(a, n + 1, 1, DE_MEM_GENERAL);
    if (b == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "md_read");
    memcpy(b, g_md_text, n + 1);
    *data = b;
    *size = n;
    return DE_OK;
}

static void md_parse(md_parsed *p, const de_allocator *a, const char *text) {
    g_md_text = text;
    de__manifest_parse_desc d = {.path = "md:/assets.toml", .allocator = a, .read = md_read};
    ASSERT_EQ(de__manifest_parse(&d, &p->m), DE_OK);
    ASSERT_EQ(p->m->error_count, 0u);
    ASSERT_EQ(de__manifest_resolve(p->m, &(de__resolve_desc){.allocator = a}, &p->r), DE_OK);
    ASSERT_EQ(p->r->error_count, 0u);
    ASSERT_EQ(de__manifest_digest_build(p->m, p->r, a, &p->d), DE_OK);
}

static void md_free(md_parsed *p) {
    de__manifest_digest_free(&p->d);
    de__resolved_destroy(p->r);
    de__manifest_destroy(p->m);
}

static const de__manifest_change *find_change(const de__manifest_change *c, u32 n, const char *id) {
    de_asset_id want = de_asset_id_from(id);
    for (u32 i = 0; i < n; ++i)
        if (c[i].id.hash == want.hash)
            return &c[i];
    return NULL;
}

static const char k_old[] =
    "[project]\n"
    "asset_root = \"src\"\n"
    "[defaults.tex]\n"
    "srgb = true\n"
    "[assets]\n"
    "\"same\" = { type = \"tex\", source = \"same.png\", mips = 4 }\n"
    "\"reordered\" = { type = \"tex\", source = \"r.png\", a = 1, b = 2 }\n"
    "\"setting\" = { type = \"tex\", source = \"s.png\", mips = 4 }\n"
    "\"int_float\" = { type = \"tex\", source = \"f.png\", scale = 1 }\n"
    "\"source\" = { type = \"tex\", source = \"old.png\" }\n"
    "\"type\" = { type = \"tex\", source = \"t.png\" }\n"
    "\"removed\" = { type = \"tex\", source = \"gone.png\" }\n"
    "\"locale\" = { type = \"tex\", source = \"l.png\", locale = { ja = \"l_ja.png\" } }\n"
    "\"defaults\" = { type = \"tex\", source = \"d.png\" }\n"
    "[bundles.level]\n"
    "assets = [\"same\"]\n";

static const char k_new[] =
    "[project]\n"
    "asset_root = \"src\"\n"
    "[defaults.tex]\n"
    "srgb = true\n"
    "[defaults.snd]\n"
    "rate = 48000\n"
    "[assets]\n"
    "\"added\" = { type = \"tex\", source = \"new.png\" }\n"
    "\"same\" = { type = \"tex\", source = \"same.png\", mips = 4 }\n"
    "\"reordered\" = { type = \"tex\", b = 2, source = \"r.png\", a = 1 }\n"
    "\"setting\" = { type = \"tex\", source = \"s.png\", mips = 5 }\n"
    "\"int_float\" = { type = \"tex\", source = \"f.png\", scale = 1.0 }\n"
    "\"source\" = { type = \"tex\", source = \"new_src.png\" }\n"
    "\"type\" = { type = \"snd\", source = \"t.png\" }\n"
    "\"locale\" = { type = \"tex\", source = \"l.png\", locale = { ja = \"l_ja2.png\" } }\n"
    "\"defaults\" = { type = \"tex\", source = \"d.png\", srgb = false }\n"
    "[bundles.level]\n"
    "assets = [\"same\", \"added\"]\n";

TEST(manifest_diff, added_removed_changed) {
    de_tracking_allocator track;
    de_allocator a = de_tracking_allocator_init(&track, NULL, DE_MEM_ASSET);
    md_parsed o, n;
    memset(&o, 0, sizeof o);
    memset(&n, 0, sizeof n);
    md_parse(&o, &a, k_old);
    md_parse(&n, &a, k_new);
    EXPECT_EQ(o.d.count, 9u);
    EXPECT_EQ(n.d.count, 9u);
    for (u32 i = 1; i < n.d.count; ++i)
        EXPECT_LT(n.d.entries[i - 1u].id.hash, n.d.entries[i].id.hash); /* sorted by ID */

    de__manifest_change *c = NULL;
    u32 count = 0;
    ASSERT_EQ(de__manifest_diff(&o.d, &n.d, &a, &c, &count), DE_OK);
    /* added, removed, setting, int_float, source, type, locale, defaults. */
    EXPECT_EQ(count, 8u);
    for (u32 i = 1; i < count; ++i)
        EXPECT_LT(c[i - 1u].id.hash, c[i].id.hash);

    const de__manifest_change *x = find_change(c, count, "added");
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ((int)x->kind, (int)DE__MANIFEST_ADDED);
    x = find_change(c, count, "removed");
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ((int)x->kind, (int)DE__MANIFEST_REMOVED);
    x = find_change(c, count, "setting");
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ((int)x->kind, (int)DE__MANIFEST_CHANGED);
    EXPECT_EQ(x->what, (u32)DE__MANIFEST_CHANGED_SETTINGS);
    x = find_change(c, count, "int_float"); /* 1 and 1.0 are different settings */
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ(x->what, (u32)DE__MANIFEST_CHANGED_SETTINGS);
    x = find_change(c, count, "source");
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ(x->what, (u32)DE__MANIFEST_CHANGED_SOURCE);
    x = find_change(c, count, "type"); /* its settings change too: other defaults */
    ASSERT_TRUE(x != NULL);
    EXPECT_TRUE((x->what & DE__MANIFEST_CHANGED_TYPE) != 0);
    x = find_change(c, count, "locale");
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ(x->what, (u32)DE__MANIFEST_CHANGED_SOURCE);
    x = find_change(c, count, "defaults"); /* an override of a default */
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ(x->what, (u32)DE__MANIFEST_CHANGED_SETTINGS);
    /* Key order, formatting and bundle changes are no change. */
    EXPECT_TRUE(find_change(c, count, "same") == NULL);
    EXPECT_TRUE(find_change(c, count, "reordered") == NULL);
    DE_FREE(&a, c, sizeof(de__manifest_change) * count);

    /* The same manifest twice: nothing. An empty old digest: everything is added. */
    ASSERT_EQ(de__manifest_diff(&n.d, &n.d, &a, &c, &count), DE_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(c == NULL);
    de__manifest_digest empty;
    ASSERT_EQ(de__manifest_digest_build(NULL, NULL, &a, &empty), DE_OK);
    EXPECT_EQ(empty.count, 0u);
    ASSERT_EQ(de__manifest_diff(&empty, &n.d, &a, &c, &count), DE_OK);
    EXPECT_EQ(count, n.d.count);
    for (u32 i = 0; i < count; ++i)
        EXPECT_EQ((int)c[i].kind, (int)DE__MANIFEST_ADDED);
    DE_FREE(&a, c, sizeof(de__manifest_change) * count);
    de__manifest_digest_free(&empty);

    /* Another asset root changes every source. */
    md_parsed r;
    memset(&r, 0, sizeof r);
    static const char k_root[] = "[project]\n"
                                 "asset_root = \"other\"\n"
                                 "[defaults.tex]\n"
                                 "srgb = true\n"
                                 "[assets]\n"
                                 "\"same\" = { type = \"tex\", source = \"same.png\", mips = 4 }\n";
    md_parse(&r, &a, k_root);
    ASSERT_EQ(de__manifest_diff(&o.d, &r.d, &a, &c, &count), DE_OK);
    x = find_change(c, count, "same");
    ASSERT_TRUE(x != NULL);
    EXPECT_EQ(x->what, (u32)DE__MANIFEST_CHANGED_SOURCE);
    DE_FREE(&a, c, sizeof(de__manifest_change) * count);

    md_free(&r);
    md_free(&o);
    md_free(&n);
    EXPECT_EQ(de_tracking_allocator_stats(&track).bytes_live, 0u);
}

/* The reverse index the reloads use: paths normalise, an asset's paths are replaced as a set,
 * and a lookup returns each asset that read the path once. */
static u32 g_ids_n;
static de_asset_id g_ids[8];

static void collect(de_asset_id id, void *user) {
    (void)user;
    if (g_ids_n < 8)
        g_ids[g_ids_n] = id;
    g_ids_n++;
}

TEST(manifest_diff, reverse_index) {
    de_tracking_allocator track;
    de_allocator a = de_tracking_allocator_init(&track, NULL, DE_MEM_ASSET);
    de__rindex ix;
    ASSERT_EQ(de__rindex_init(&ix, &a), DE_OK);
    de_asset_id x = de_asset_id_from("x"), y = de_asset_id_from("y");
    ASSERT_EQ(de__rindex_set(&ix, x, "dev:/a/./main.txt", "dev:/a/inc.txt\0dev:/a/b/../inc.txt", 2),
              DE_OK);
    ASSERT_EQ(de__rindex_set(&ix, y, "dev:/a/inc.txt", NULL, 0), DE_OK);
    EXPECT_EQ(ix.count, 3u); /* x: main, inc (once); y: inc */
    g_ids_n = 0;
    EXPECT_EQ(de__rindex_lookup(&ix, "dev:/a/inc.txt", collect, NULL), 2u);
    EXPECT_TRUE((g_ids[0].hash == x.hash && g_ids[1].hash == y.hash) ||
                (g_ids[0].hash == y.hash && g_ids[1].hash == x.hash));
    g_ids_n = 0;
    EXPECT_EQ(de__rindex_lookup(&ix, "dev:/a/main.txt", collect, NULL), 1u);
    EXPECT_EQ(g_ids[0].hash, x.hash);
    EXPECT_EQ(de__rindex_lookup(&ix, "dev:/a/none.txt", collect, NULL), 0u);
    /* x's paths are replaced: main.txt no longer leads to it. */
    ASSERT_EQ(de__rindex_set(&ix, x, "dev:/a/other.txt", NULL, 0), DE_OK);
    EXPECT_EQ(de__rindex_lookup(&ix, "dev:/a/main.txt", collect, NULL), 0u);
    g_ids_n = 0;
    EXPECT_EQ(de__rindex_each_id(&ix, collect, NULL), 2u);
    EXPECT_NE(g_ids[0].hash, g_ids[1].hash);
    de__rindex_remove(&ix, y);
    EXPECT_EQ(de__rindex_lookup(&ix, "dev:/a/inc.txt", collect, NULL), 0u);
    EXPECT_EQ(ix.count, 1u);
    /* Freed links are reused; many assets on one path, removed from the middle of its list. */
    de_asset_id many[50];
    for (u32 i = 0; i < 50; ++i) {
        char name[16];
        snprintf(name, sizeof name, "m%u", (unsigned)i);
        many[i] = de_asset_id_from(name);
        ASSERT_EQ(de__rindex_set(&ix, many[i], "dev:/shared.txt", NULL, 0), DE_OK);
    }
    for (u32 i = 0; i < 50; i += 2)
        de__rindex_remove(&ix, many[i]);
    EXPECT_EQ(de__rindex_lookup(&ix, "dev:/shared.txt", collect, NULL), 25u);
    for (u32 i = 1; i < 50; i += 2)
        de__rindex_remove(&ix, many[i]);
    EXPECT_EQ(de__rindex_lookup(&ix, "dev:/shared.txt", collect, NULL), 0u);
    EXPECT_EQ(ix.count, 1u);
    EXPECT_LE(ix.link_count, 51u);
    de__rindex_free(&ix);
    EXPECT_EQ(de_tracking_allocator_stats(&track).bytes_live, 0u);
}

TEST(manifest_diff, nested_numeric_range_change) {
    de_tracking_allocator track;
    de_allocator a = de_tracking_allocator_init(&track, NULL, DE_MEM_ASSET);
    md_parsed old = {0}, new = {0};
    md_parse(&old, &a, "[assets.a]\ntype='font'\nsource='font.ttf'\nranges=[[256,591]]\n");
    md_parse(&new, &a, "[assets.a]\ntype='font'\nsource='font.ttf'\nranges=[[256,592]]\n");
    de__manifest_change *changes = NULL;
    u32 count = 0;
    ASSERT_EQ(de__manifest_diff(&old.d, &new.d, &a, &changes, &count), DE_OK);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(changes[0].what, (u32)DE__MANIFEST_CHANGED_SETTINGS);
    DE_FREE(&a, changes, sizeof *changes * count);
    md_free(&old);
    md_free(&new);
    EXPECT_EQ(de_tracking_allocator_stats(&track).bytes_live, 0u);
}
