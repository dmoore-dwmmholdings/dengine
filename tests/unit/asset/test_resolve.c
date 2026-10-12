/*
 * test_resolve.c: manifest resolution and validation (DENG-139, SPEC §11.1, §11.2). CTest names
 * resolve.<test>.
 *
 * Most tests parse from memory (mem_read: a table of virtual path -> text). The example test
 * reads the shipped examples/assets.toml (mounted as `rex:/`) with the stub includes of
 * tests/unit/asset/data/example/ (mounted as `rdata:/`). The system tests write their manifests
 * into a writable scratch directory in the build tree (`rtmp:/`).
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef DE_ASSET_RESOLVE_TMP_DIR
#error "DE_ASSET_RESOLVE_TMP_DIR must be defined by tests/unit/asset/CMakeLists.txt"
#endif

/* ---- Helpers ----------------------------------------------------------------------------- */

typedef struct mem_file {
    const char *path;
    const char *text;
} mem_file;

static const mem_file *g_files;
static u32 g_file_count;

static de_result mem_read(void *user, const char *vpath, const de_allocator *a, void **data,
                          u64 *size) {
    (void)user;
    for (u32 i = 0; i < g_file_count; ++i) {
        if (strcmp(g_files[i].path, vpath) == 0) {
            size_t n = strlen(g_files[i].text);
            char *b = (char *)DE_ALLOC(a, n + 1, 1, DE_MEM_GENERAL);
            if (b == NULL)
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "mem_read");
            memcpy(b, g_files[i].text, n + 1);
            *data = b;
            *size = n;
            return DE_OK;
        }
    }
    return DE_FAIL(DE_ERR_NOT_FOUND, "%s: no such file", vpath);
}

static de__manifest *parse_mem(const mem_file *files, u32 count) {
    g_files = files;
    g_file_count = count;
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {.path = files[0].path, .read = mem_read};
    ASSERT_EQ(de__manifest_parse(&d, &m), DE_OK);
    return m;
}

static void dump_issues(const de__resolved *r) {
    for (u32 i = 0; i < r->issue_count; ++i)
        fprintf(stderr, "  issue %u: sev=%d %s:%u [%s] %s\n", i, (int)r->issues[i].severity,
                r->issues[i].file, r->issues[i].line, r->issues[i].key_path, r->issues[i].message);
}

/* The first issue of `sev` whose message contains `text`, or NULL. */
static const de__manifest_issue *find_issue(const de__resolved *r, de_asset_issue_severity sev,
                                            const char *text) {
    for (u32 i = 0; i < r->issue_count; ++i)
        if (r->issues[i].severity == sev && strstr(r->issues[i].message, text) != NULL)
            return &r->issues[i];
    return NULL;
}

typedef struct fixture {
    de__manifest *m;
    de__resolved *r;
} fixture;

static fixture resolve_files(const mem_file *files, u32 count, const de__resolve_desc *d) {
    fixture f;
    f.m = parse_mem(files, count);
    f.r = NULL;
    ASSERT_EQ(de__manifest_resolve(f.m, d, &f.r), DE_OK);
    ASSERT_TRUE(f.r != NULL);
    return f;
}

#define RESOLVE(files) resolve_files((files), (u32)(sizeof(files) / sizeof((files)[0])), NULL)

static void fixture_free(fixture *f) {
    de__resolved_destroy(f->r);
    de__manifest_destroy(f->m);
}

static de_asset_id id_of(const char *path) {
    return de_asset_id_from(path);
}

static bool has_member(const de__resolved_bundle *b, const char *path) {
    de_asset_id id = id_of(path);
    for (u32 i = 0; i < b->member_count; ++i)
        if (b->members[i].hash == id.hash)
            return true;
    return false;
}

static void expect_sorted_unique(const de__resolved_bundle *b) {
    for (u32 i = 1; i < b->member_count; ++i)
        EXPECT_LT(b->members[i - 1].hash, b->members[i].hash);
}

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("rdata", DE_ASSET_TEST_DATA_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("rex", DE_ASSET_EXAMPLES_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("rtmp", DE_ASSET_RESOLVE_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    mounted = true;
}

/* rex:/ (examples/), falling back to rdata:/example/ for the includes the repo does not have. */
static de_result example_read(void *user, const char *vpath, const de_allocator *a, void **data,
                              u64 *size) {
    (void)user;
    de_result r = de_file_read_all(vpath, a, data, size);
    if (r == DE_ERR_NOT_FOUND && strncmp(vpath, "rex:/", 5) == 0) {
        char alt[DE_FS_PATH_MAX];
        snprintf(alt, sizeof alt, "rdata:/example/%s", vpath + 5);
        r = de_file_read_all(alt, a, data, size);
    }
    return r;
}

static de__manifest *parse_example(const de_allocator *a) {
    mount_once();
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {.path = "rex:/assets.toml", .allocator = a, .read = example_read};
    ASSERT_EQ(de__manifest_parse(&d, &m), DE_OK);
    ASSERT_EQ(m->error_count, 0u);
    return m;
}

static u32 bundle_index(const de__resolved *r, const char *name) {
    const de__resolved_bundle *b = de__resolved_find_bundle(r, name);
    ASSERT_TRUE(b != NULL);
    return (u32)(b - r->bundles);
}

/* ---- examples/assets.toml ---------------------------------------------------------------- */

TEST(resolve, example_bundles) {
    de__manifest *m = parse_example(NULL);
    de__resolved *r = NULL;
    ASSERT_EQ(de__manifest_resolve(m, NULL, &r), DE_OK);
    if (r->issue_count != 0)
        dump_issues(r);
    EXPECT_EQ(r->issue_count, 0u);
    EXPECT_EQ(r->error_count, 0u);

    /* Every asset has its ID; the table is sorted and complete. */
    ASSERT_EQ(r->id_count, m->asset_count);
    for (u32 i = 0; i < m->asset_count; ++i)
        EXPECT_EQ(r->asset_ids[i].hash, id_of(m->assets[i].id).hash);
    for (u32 i = 1; i < r->id_count; ++i)
        EXPECT_LT(r->ids[i - 1].id.hash, r->ids[i].id.hash);
    EXPECT_STREQ(de__resolved_path(r, id_of("env/castle")), "env/castle");
    EXPECT_STREQ(de__resolved_path(r, id_of("sfx/jump")), "sfx/jump"); /* from audio.toml */
    EXPECT_TRUE(de__resolved_path(r, id_of("env/castle#Tower")) == NULL);
    EXPECT_TRUE(de__resolved_path(r, (de_asset_id){0}) == NULL);
    const de__manifest_asset *logo = de__resolved_asset(r, id_of("ui/logo"));
    ASSERT_TRUE(logo != NULL);
    EXPECT_STREQ(logo->type, "texture");

    ASSERT_EQ(r->bundle_count, 4u);
    const de__resolved_bundle *level = de__resolved_find_bundle(r, "level_01");
    ASSERT_TRUE(level != NULL);
    EXPECT_STREQ(level->bundle->name, "level_01");
    /* depends closure = [common] */
    ASSERT_EQ(level->depends_count, 1u);
    EXPECT_STREQ(r->bundles[level->depends[0]].bundle->name, "common");
    /* Globs level01 (2), env (6), hero (1), plus shaders/hologram; sorted by ID. */
    EXPECT_EQ(level->member_count, 10u);
    EXPECT_TRUE(has_member(level, "env/castle"));
    EXPECT_TRUE(has_member(level, "env/castle_ruins"));
    EXPECT_TRUE(has_member(level, "env/stone"));
    EXPECT_TRUE(has_member(level, "hero/sprites"));
    EXPECT_TRUE(has_member(level, "shaders/hologram"));
    EXPECT_TRUE(has_member(level, "level01/tiles"));
    EXPECT_TRUE(has_member(level, "level01/scene"));
    EXPECT_FALSE(has_member(level, "ui/font")); /* common's member, not level_01's */
    expect_sorted_unique(level);

    const de__resolved_bundle *common = de__resolved_find_bundle(r, "common");
    ASSERT_TRUE(common != NULL);
    EXPECT_EQ(common->depends_count, 0u);
    /* The ui glob (font, font_pixel, strings, theme, logo) + sfx/click */
    EXPECT_EQ(common->member_count, 6u);
    EXPECT_TRUE(has_member(common, "ui/logo"));
    EXPECT_TRUE(has_member(common, "sfx/click"));
    EXPECT_FALSE(has_member(common, "sfx/jump"));
    expect_sorted_unique(common);

    const de__resolved_bundle *title = de__resolved_find_bundle(r, "title");
    ASSERT_TRUE(title != NULL);
    EXPECT_EQ(title->member_count, 2u);
    ASSERT_EQ(title->depends_count, 1u);
    EXPECT_EQ(title->depends[0], bundle_index(r, "common"));

    /* characters.toml: [bundles.player] with the player glob, depends = ["common"]. */
    const de__resolved_bundle *player = de__resolved_find_bundle(r, "player");
    ASSERT_TRUE(player != NULL);
    EXPECT_EQ(player->member_count, 2u);
    EXPECT_TRUE(has_member(player, "player/model"));
    EXPECT_TRUE(has_member(player, "player/albedo"));
    EXPECT_EQ(player->depends_count, 1u);
    EXPECT_TRUE(de__resolved_find_bundle(r, "nope") == NULL);

    de__resolved_destroy(r);
    de__manifest_destroy(m);
}

/* ---- Bundles ----------------------------------------------------------------------------- */

TEST(resolve, cycle) {
    static const mem_file files[] = {
        {"mem:/assets.toml",
         "[assets]\nx = { type = 't', source = 'x' }\n"
         "[bundles.a]\nassets = ['x']\ndepends = ['b']\n"
         "[bundles.b]\ndepends = ['c']\n"
         "[bundles.c]\ndepends = ['a']\n"
         "[bundles.d]\ndepends = ['a']\n" /* depends on the cycle; not part of it */
         "[bundles.self]\ndepends = ['self', 'self']\n"},
    };
    fixture f = RESOLVE(files);
    if (f.r->error_count != 2)
        dump_issues(f.r);
    ASSERT_EQ(f.r->error_count, 2u);
    EXPECT_EQ(f.r->warning_count, 0u);
    /* Reported once, with the full path, on the bundle whose `depends` closes the cycle. */
    const de__manifest_issue *is = find_issue(f.r, DE_ASSET_ISSUE_ERROR, "cycle");
    ASSERT_TRUE(is != NULL);
    EXPECT_STREQ(is->message, "bundle dependency cycle: a -> b -> c -> a");
    EXPECT_STREQ(is->key_path, "bundles.c.depends");
    EXPECT_STREQ(is->file, "mem:/assets.toml");
    EXPECT_EQ(is->line, 8u); /* the [bundles.c] header */
    EXPECT_EQ(is->code, DE_ERR_PARSE);
    const de__manifest_issue *self = find_issue(f.r, DE_ASSET_ISSUE_ERROR, "self -> self");
    ASSERT_TRUE(self != NULL);
    EXPECT_STREQ(self->message, "bundle dependency cycle: self -> self");

    /* The closing edge (c -> a) is left out; every other edge is kept. */
    const de__resolved_bundle *a = de__resolved_find_bundle(f.r, "a");
    const de__resolved_bundle *c = de__resolved_find_bundle(f.r, "c");
    const de__resolved_bundle *d = de__resolved_find_bundle(f.r, "d");
    ASSERT_EQ(a->depends_count, 2u); /* c, then b */
    EXPECT_EQ(a->depends[0], bundle_index(f.r, "c"));
    EXPECT_EQ(a->depends[1], bundle_index(f.r, "b"));
    EXPECT_EQ(c->depends_count, 0u);
    ASSERT_EQ(d->depends_count, 3u); /* c, b, a */
    EXPECT_EQ(d->depends[2], bundle_index(f.r, "a"));
    EXPECT_EQ(de__resolved_find_bundle(f.r, "self")->depends_count, 0u);
    fixture_free(&f);
}

TEST(resolve, closure_order) {
    /* x needs z and y; y needs w; v needs x and z. Declaration order: x, y, z, w, v.
     * Topological order with declaration-order ties: z, w, y, x, v. */
    static const mem_file files[] = {
        {"mem:/assets.toml", "[bundles.x]\ndepends = ['z', 'y']\n"
                             "[bundles.y]\ndepends = ['w']\n"
                             "[bundles.z]\n"
                             "[bundles.w]\n"
                             "[bundles.v]\ndepends = ['x', 'z', 'x']\n"},
    };
    fixture f = RESOLVE(files);
    EXPECT_EQ(f.r->issue_count, 0u);
    const de__resolved_bundle *x = de__resolved_find_bundle(f.r, "x");
    ASSERT_EQ(x->depends_count, 3u);
    EXPECT_STREQ(f.r->bundles[x->depends[0]].bundle->name, "z");
    EXPECT_STREQ(f.r->bundles[x->depends[1]].bundle->name, "w");
    EXPECT_STREQ(f.r->bundles[x->depends[2]].bundle->name, "y");
    const de__resolved_bundle *v = de__resolved_find_bundle(f.r, "v");
    ASSERT_EQ(v->depends_count, 4u); /* repeats in `depends` count once */
    EXPECT_STREQ(f.r->bundles[v->depends[0]].bundle->name, "z");
    EXPECT_STREQ(f.r->bundles[v->depends[1]].bundle->name, "w");
    EXPECT_STREQ(f.r->bundles[v->depends[2]].bundle->name, "y");
    EXPECT_STREQ(f.r->bundles[v->depends[3]].bundle->name, "x");
    EXPECT_EQ(de__resolved_find_bundle(f.r, "z")->depends_count, 0u);
    fixture_free(&f);
}

TEST(resolve, unknown_depends_and_entries) {
    static const mem_file files[] = {
        {"mem:/assets.toml",
         "[assets]\n'env/castle' = { type = 'model', source = 'castle.glb' }\n"
         "[bundles.'my level']\n"
         "assets = ['env/castle', 'env/castle', 'env/nope', 'env//bad', 'env/castle#Tower']\n"
         "depends = ['ghost']\n"},
    };
    fixture f = RESOLVE(files);
    if (f.r->error_count != 4)
        dump_issues(f.r);
    ASSERT_EQ(f.r->error_count, 4u);
    const de__manifest_issue *is;
    is = find_issue(f.r, DE_ASSET_ISSUE_ERROR, "\"env/nope\" is not declared");
    ASSERT_TRUE(is != NULL);
    EXPECT_STREQ(is->key_path, "bundles.\"my level\".assets");
    EXPECT_TRUE(strstr(is->message, "bundle \"my level\"") != NULL);
    is = find_issue(f.r, DE_ASSET_ISSUE_ERROR, "\"env//bad\" is not a valid asset ID");
    ASSERT_TRUE(is != NULL);
    is = find_issue(f.r, DE_ASSET_ISSUE_ERROR, "is a sub-asset; list its asset \"env/castle\"");
    ASSERT_TRUE(is != NULL);
    is = find_issue(f.r, DE_ASSET_ISSUE_ERROR, "unknown bundle \"ghost\"");
    ASSERT_TRUE(is != NULL);
    EXPECT_STREQ(is->key_path, "bundles.\"my level\".depends");
    /* Entries with errors are left out; the duplicate counts once. */
    const de__resolved_bundle *b = de__resolved_find_bundle(f.r, "my level");
    ASSERT_EQ(b->member_count, 1u);
    EXPECT_EQ(b->members[0].hash, id_of("env/castle").hash);
    EXPECT_EQ(b->depends_count, 0u);
    fixture_free(&f);
}

TEST(resolve, unmatched_glob) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[assets]\n'ui/font' = { type = 'font', source = 'f.ttf' }\n"
                             "'ui/a/b' = { type = 'font', source = 'g.ttf' }\n"
                             "[bundles.hud]\nassets = ['ui/*', 'hud/*', 'ui/font#*']\n"},
    };
    fixture f = RESOLVE(files);
    EXPECT_EQ(f.r->error_count, 0u);
    ASSERT_EQ(f.r->warning_count, 2u);
    const de__manifest_issue *is = find_issue(f.r, DE_ASSET_ISSUE_WARNING, "hud/*");
    ASSERT_TRUE(is != NULL);
    EXPECT_STREQ(is->message, "bundle \"hud\": glob \"hud/*\" matches no asset");
    EXPECT_STREQ(is->key_path, "bundles.hud.assets");
    EXPECT_EQ(is->line, 4u);
    EXPECT_TRUE(find_issue(f.r, DE_ASSET_ISSUE_WARNING, "\"ui/font#*\" matches no asset") != NULL);
    const de__resolved_bundle *b = de__resolved_find_bundle(f.r, "hud");
    ASSERT_EQ(b->member_count, 1u); /* the ui glob does not cross into ui/a/b */
    EXPECT_EQ(b->members[0].hash, id_of("ui/font").hash);
    fixture_free(&f);
}

/* ---- Asset IDs --------------------------------------------------------------------------- */

static u64 colliding_hash(const char *path, size_t len) {
    /* "a" and "b" collide; "zero" hashes to 0 (remapped to 1); the rest as usual. */
    if ((len == 1 && (path[0] == 'a' || path[0] == 'b')))
        return 42u;
    if (len == 4 && memcmp(path, "zero", 4) == 0)
        return 0u;
    return de_hash64(path, len, 0);
}

TEST(resolve, id_collision_and_zero_remap) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[assets]\na = { type = 't', source = 'a' }\n"
                             "c = { type = 't', source = 'c' }\n"
                             "b = { type = 't', source = 'b' }\n"
                             "zero = { type = 't', source = 'z' }\n"
                             "[bundles.k]\nassets = ['b', 'zero']\n"},
    };
    de__resolve_desc d = {.hash_override = colliding_hash};
    fixture f = resolve_files(files, 1, &d);
    if (f.r->error_count != 1)
        dump_issues(f.r);
    ASSERT_EQ(f.r->error_count, 1u);
    const de__manifest_issue *is = &f.r->issues[0];
    EXPECT_STREQ(is->key_path, "assets.b");
    EXPECT_EQ(is->line, 4u);
    EXPECT_TRUE(strstr(is->message, "\"b\" and \"a\"") != NULL);
    EXPECT_TRUE(strstr(is->message, "0x000000000000002a") != NULL);
    /* Both rows stay, adjacent, in declaration order; lookups find the first. */
    EXPECT_EQ(f.r->id_count, 4u);
    EXPECT_EQ(f.r->asset_ids[3].hash, 1u); /* "zero" -> 0 -> 1 */
    EXPECT_STREQ(de__resolved_path(f.r, (de_asset_id){42u}), "a");
    EXPECT_STREQ(de__resolved_path(f.r, (de_asset_id){1u}), "zero");
    /* Explicit entries compare the path, not just the ID. */
    const de__resolved_bundle *k = de__resolved_find_bundle(f.r, "k");
    ASSERT_EQ(k->member_count, 2u);
    EXPECT_EQ(k->members[0].hash, 1u);
    EXPECT_EQ(k->members[1].hash, 42u);
    fixture_free(&f);
}

TEST(resolve, invalid_asset_ids) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[assets]\n'ok/one' = { type = 't', source = 'a' }\n"
                             "'/abs' = { type = 't', source = 'b' }\n"
                             "'a/../b' = { type = 't', source = 'c' }\n"
                             "'m#x#y' = { type = 't', source = 'd' }\n"
                             "[bundles.all]\nassets = ['**']\n"},
    };
    fixture f = RESOLVE(files);
    ASSERT_EQ(f.r->error_count, 3u);
    EXPECT_STREQ(f.r->issues[0].key_path, "assets.\"/abs\"");
    EXPECT_STREQ(f.r->issues[0].message, "invalid asset ID \"/abs\": leading '/'");
    EXPECT_EQ(f.r->issues[0].line, 3u);
    EXPECT_STREQ(f.r->issues[1].key_path, "assets.\"a/../b\"");
    EXPECT_STREQ(f.r->issues[2].key_path, "assets.\"m#x#y\"");
    EXPECT_EQ(f.r->asset_ids[1].hash, 0u);
    EXPECT_EQ(f.r->id_count, 1u);
    /* Invalid IDs are never bundle members. */
    EXPECT_EQ(de__resolved_find_bundle(f.r, "all")->member_count, 1u);
    fixture_free(&f);
}

/* ---- Validation -------------------------------------------------------------------------- */

TEST(resolve, unknown_type) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[assets]\nlogo = { type = 'texture', source = 'a.png' }\n"
                             "song = { type = 'sound', source = 'b.ogg' }\n"
                             "thing = { type = 'gizmo', source = 'c.giz' }\n"},
    };
    fixture f = RESOLVE(files);
    static const char *const known[] = {"texture", "sound"};
    de__validate_desc vd = {.known_types = known, .known_type_count = 2, .skip_sources = true};
    ASSERT_EQ(de__manifest_validate(f.r, &vd), DE_OK);
    ASSERT_EQ(f.r->error_count, 1u);
    const de__manifest_issue *is = &f.r->issues[0];
    EXPECT_STREQ(is->message, "asset \"thing\": unknown asset type \"gizmo\"");
    EXPECT_STREQ(is->key_path, "assets.thing.type");
    EXPECT_EQ(is->line, 4u);
    /* No list = no type check (the zero desc checks sources only). */
    de__validate_desc none = {.skip_sources = true};
    ASSERT_EQ(de__manifest_validate(f.r, &none), DE_OK);
    EXPECT_EQ(f.r->error_count, 1u);
    EXPECT_EQ(de__manifest_validate(NULL, &none), DE_ERR_INVALID_ARGUMENT);
    fixture_free(&f);
}

static bool fake_exists(void *user, const char *vpath) {
    const char *const *present = (const char *const *)user;
    for (; *present != NULL; ++present)
        if (strcmp(*present, vpath) == 0)
            return true;
    return false;
}

TEST(resolve, missing_source) {
    static const mem_file files[] = {
        {"mem:/game/assets.toml", "[project]\nasset_root = 'assets'\n"
                                  "[assets]\nhere = { type = 't', source = 'textures/here.png' }\n"
                                  "gone = { type = 't', source = 'textures/gone.png' }\n"
                                  "out = { type = 't', source = '../../../etc/passwd' }\n"
                                  "[assets.logo]\ntype = 't'\nsource = 'logo_en.png'\n"
                                  "[assets.logo.locale]\nja = 'logo_ja.png'\nde = 'logo_de.png'\n"},
    };
    fixture f = RESOLVE(files);
    static const char *const present[] = {"mem:/game/assets/textures/here.png",
                                          "mem:/game/assets/logo_en.png",
                                          "mem:/game/assets/logo_ja.png", NULL};
    de__validate_desc vd = {.exists = fake_exists, .exists_user = (void *)present};
    ASSERT_EQ(de__manifest_validate(f.r, &vd), DE_OK);
    if (f.r->error_count != 3)
        dump_issues(f.r);
    ASSERT_EQ(f.r->error_count, 3u);
    EXPECT_STREQ(f.r->issues[0].key_path, "assets.gone.source");
    EXPECT_STREQ(f.r->issues[0].message,
                 "asset \"gone\": source file mem:/game/assets/textures/gone.png does not exist");
    EXPECT_STREQ(f.r->issues[1].key_path, "assets.out.source");
    EXPECT_TRUE(strstr(f.r->issues[1].message, "is not a valid path") != NULL);
    EXPECT_STREQ(f.r->issues[2].key_path, "assets.logo.locale.de"); /* locale sources too */
    EXPECT_TRUE(strstr(f.r->issues[2].message, "logo_de.png does not exist") != NULL);
    fixture_free(&f);

    /* A given source directory replaces asset_root; the default test is de_path_exists. */
    mount_once();
    static const mem_file files2[] = {
        {"mem:/assets.toml", "[assets]\nm = { type = 't', source = 'vfs/assets.toml' }\n"
                             "n = { type = 't', source = 'vfs/nope.toml' }\n"},
    };
    f = RESOLVE(files2);
    de__validate_desc vd2 = {.source_dir = "rdata:/"};
    ASSERT_EQ(de__manifest_validate(f.r, &vd2), DE_OK);
    ASSERT_EQ(f.r->error_count, 1u);
    EXPECT_STREQ(f.r->issues[0].key_path, "assets.n.source");
    fixture_free(&f);
}

/* ---- Determinism and memory -------------------------------------------------------------- */

static bool same_resolution(const de__resolved *a, const de__resolved *b) {
    if (a->id_count != b->id_count || a->bundle_count != b->bundle_count ||
        a->issue_count != b->issue_count)
        return false;
    for (u32 i = 0; i < a->id_count; ++i)
        if (a->ids[i].id.hash != b->ids[i].id.hash || a->ids[i].asset != b->ids[i].asset)
            return false;
    for (u32 i = 0; i < a->bundle_count; ++i) {
        const de__resolved_bundle *x = &a->bundles[i], *y = &b->bundles[i];
        if (x->member_count != y->member_count || x->depends_count != y->depends_count)
            return false;
        if (x->member_count != 0 &&
            memcmp(x->members, y->members, sizeof *x->members * x->member_count) != 0)
            return false;
        if (x->depends_count != 0 &&
            memcmp(x->depends, y->depends, sizeof *x->depends * x->depends_count) != 0)
            return false;
    }
    for (u32 i = 0; i < a->issue_count; ++i)
        if (strcmp(a->issues[i].message, b->issues[i].message) != 0 ||
            strcmp(a->issues[i].key_path, b->issues[i].key_path) != 0)
            return false;
    return true;
}

TEST(resolve, deterministic_order) {
    /* The same input resolves to the same bytes, run after run, whatever the allocator state
     * (hash maps are never iterated; everything is sorted or in manifest order). */
    static const mem_file files[] = {
        {"mem:/assets.toml",
         "[assets]\n"
         "'z/1' = { type = 't', source = 's' }\n'a/1' = { type = 't', source = 's' }\n"
         "'m/1' = { type = 't', source = 's' }\n'a/2' = { type = 't', source = 's' }\n"
         "'z/2' = { type = 't', source = 's' }\n'm/2' = { type = 't', source = 's' }\n"
         "[bundles.top]\nassets = ['*/2', 'a/*', 'nothing/*']\ndepends = ['mid', 'low', 'x']\n"
         "[bundles.mid]\nassets = ['m/*']\ndepends = ['low', 'top']\n"
         "[bundles.low]\nassets = ['z/1', 'z/2', 'q']\n"},
    };
    fixture first = RESOLVE(files);
    EXPECT_EQ(first.r->error_count, 3u);   /* unknown x, undeclared q, cycle top -> mid -> top */
    EXPECT_EQ(first.r->warning_count, 1u); /* the `nothing` glob */
    for (u32 run = 0; run < 8; ++run) {
        /* Disturb the heap between runs so addresses and map capacities differ. */
        void *junk = DE_ALLOC(de_heap_allocator(), 4096u * (run + 1), 16, DE_MEM_GENERAL);
        fixture again = RESOLVE(files);
        EXPECT_TRUE(same_resolution(first.r, again.r));
        fixture_free(&again);
        DE_FREE(de_heap_allocator(), junk, 4096u * (run + 1));
    }
    fixture_free(&first);

    /* The example too, through two independent parses. */
    de__manifest *m1 = parse_example(NULL), *m2 = parse_example(NULL);
    de__resolved *r1 = NULL, *r2 = NULL;
    ASSERT_EQ(de__manifest_resolve(m1, NULL, &r1), DE_OK);
    ASSERT_EQ(de__manifest_resolve(m2, NULL, &r2), DE_OK);
    EXPECT_TRUE(same_resolution(r1, r2));
    de__resolved_destroy(r1);
    de__resolved_destroy(r2);
    de__manifest_destroy(m1);
    de__manifest_destroy(m2);
}

TEST(resolve, keeps_manifest_issues) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[project]\ncolour = 'red'\n"      /* manifest warning */
                             "[assets]\nbad = { type = 't' }\n" /* manifest error */
                             "[bundles.b]\nassets = ['none/*']\n"},
    };
    fixture f = RESOLVE(files);
    ASSERT_EQ(f.r->issue_count, f.m->issue_count + 1u);
    for (u32 i = 0; i < f.m->issue_count; ++i)
        EXPECT_STREQ(f.r->issues[i].message, f.m->issues[i].message);
    EXPECT_EQ(f.r->error_count, f.m->error_count);
    EXPECT_EQ(f.r->warning_count, f.m->warning_count + 1u);
    de__resolved *none = (de__resolved *)&f; /* any non-NULL value: it must be cleared */
    EXPECT_EQ(de__manifest_resolve(NULL, NULL, &none), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(none == NULL);
    EXPECT_EQ(de__manifest_resolve(f.m, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    fixture_free(&f);
}

TEST(resolve, oom_sweep) {
    /* Every allocation failure gives DE_ERR_OUT_OF_MEMORY and leaks nothing, in resolve and in
     * validate; with enough allocations both succeed. */
    de__manifest *m = parse_example(NULL);
    de_tracking_allocator t;
    const de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_ASSET);
    static const char *const known[] = {"model",      "texture", "material", "shader",
                                        "aseprite",   "tilemap", "font",     "strings",
                                        "stylesheet", "sound",   "video",    "scene"};
    bool done = false;
    i64 n = 0;
    for (; n < 10000 && !done; ++n) {
        de_tracking_allocator_fail_after(&t, n);
        de__resolved *r = NULL;
        de__resolve_desc d = {.allocator = &a};
        de_result res = de__manifest_resolve(m, &d, &r);
        if (res != DE_OK) {
            ASSERT_EQ(res, DE_ERR_OUT_OF_MEMORY);
            ASSERT_TRUE(r == NULL);
        } else {
            de__validate_desc vd = {.known_types = known,
                                    .known_type_count = 12,
                                    .skip_sources = false,
                                    .source_dir = "rdata:/"};
            res = de__manifest_validate(r, &vd);
            if (res == DE_OK) {
                EXPECT_EQ(r->warning_count, 0u);
                /* No source exists under rdata:/: one error per source and locale source. */
                u32 sources = 0;
                for (u32 i = 0; i < m->asset_count; ++i)
                    sources += 1u + m->assets[i].locale_count;
                EXPECT_EQ(r->error_count, sources);
                done = true;
            } else {
                ASSERT_EQ(res, DE_ERR_OUT_OF_MEMORY);
            }
            de__resolved_destroy(r);
        }
        ASSERT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    }
    EXPECT_TRUE(done);
    EXPECT_GT(n, 3);
    de__manifest_destroy(m);
}

/* ---- Through de_asset_system ------------------------------------------------------------- */

static void write_file(const char *vpath, const char *text) {
    ASSERT_EQ(de_file_write_all_atomic(vpath, text, strlen(text)), DE_OK);
}

TEST(resolve, system_load) {
    mount_once();
    write_file("rtmp:/good.toml", "[assets]\n'ui/icon' = { type = 't', source = 'i.png' }\n"
                                  "'ui/bar' = { type = 't', source = 'b.png' }\n"
                                  "[bundles.ui]\nassets = ['ui/*', 'none/*']\n");
    write_file("rtmp:/cycle.toml", "[assets]\nx = { type = 't', source = 'x' }\n"
                                   "[bundles.a]\ndepends = ['b']\n[bundles.b]\ndepends = ['a']\n");
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &s), DE_OK);
    EXPECT_TRUE(de_asset_id_path(s, id_of("ui/icon")) == NULL); /* no manifest yet */

    ASSERT_EQ(de_asset_manifest_load(s, "rtmp:/good.toml"), DE_OK);
    ASSERT_TRUE(s->resolved != NULL);
    EXPECT_TRUE(s->resolved->manifest == s->manifest);
    EXPECT_STREQ(de_asset_id_path(s, id_of("ui/icon")), "ui/icon");
    EXPECT_TRUE(de_asset_id_path(s, id_of("ui/nope")) == NULL);
    EXPECT_TRUE(de_asset_id_path(NULL, id_of("ui/icon")) == NULL);
    u32 n = 0;
    const de_asset_manifest_issue *issues = de_asset_manifest_issues(s, &n);
    ASSERT_EQ(n, 1u); /* the unmatched glob warning (resolution issues are included) */
    EXPECT_EQ(issues[0].severity, DE_ASSET_ISSUE_WARNING);
    EXPECT_EQ(de__resolved_find_bundle(s->resolved, "ui")->member_count, 2u);

    /* A resolution error fails the load; the previous manifest stays active. */
    EXPECT_EQ(de_asset_manifest_load(s, "rtmp:/cycle.toml"), DE_ERR_PARSE);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(strstr(e->message, "a -> b -> a") != NULL);
    issues = de_asset_manifest_issues(s, &n);
    ASSERT_EQ(n, 1u);
    EXPECT_STREQ(issues[0].message, "bundle dependency cycle: a -> b -> a");
    EXPECT_STREQ(de_asset_id_path(s, id_of("ui/icon")), "ui/icon");
    ASSERT_TRUE(s->failed_resolved != NULL);

    /* A parse error: not resolved; its issues are the manifest's. */
    EXPECT_EQ(de_asset_manifest_load(s, "rdata:/malformed.toml"), DE_ERR_PARSE);
    EXPECT_TRUE(s->failed_resolved == NULL);
    issues = de_asset_manifest_issues(s, &n);
    ASSERT_EQ(n, 1u);
    EXPECT_STREQ(issues[0].file, "rdata:/malformed.toml");

    /* Loading the good one again replaces everything. */
    ASSERT_EQ(de_asset_manifest_load(s, "rtmp:/good.toml"), DE_OK);
    EXPECT_TRUE(s->failed_manifest == NULL && s->failed_resolved == NULL);
    de_asset_system_destroy(s);
}

TEST(resolve, system_destroy_frees_everything) {
    mount_once();
    write_file("rtmp:/free_good.toml", "[assets]\nx = { type = 't', source = 'x' }\n");
    write_file("rtmp:/free.toml", "[assets]\nx = { type = 't', source = 'x' }\n"
                                  "[bundles.a]\nassets = ['x']\ndepends = ['b']\n"
                                  "[bundles.b]\ndepends = ['a']\n");
    de_tracking_allocator t;
    const de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_ASSET);
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &a}, &s), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(s, "rtmp:/free_good.toml"), DE_OK);
    EXPECT_EQ(de_asset_manifest_load(s, "rtmp:/free.toml"), DE_ERR_PARSE);
    ASSERT_EQ(de_asset_manifest_load(s, "rtmp:/free_good.toml"), DE_OK);
    EXPECT_EQ(de_asset_manifest_load(s, "rtmp:/free.toml"), DE_ERR_PARSE);
    EXPECT_GT(de_tracking_allocator_stats(&t).bytes_live, 0u);
    de_asset_system_destroy(s);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&t).blocks_live, 0u);
}
