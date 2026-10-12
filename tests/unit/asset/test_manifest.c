/*
 * test_manifest.c: assets.toml parser and the de_asset_system skeleton (DENG-138, SPEC §11.2).
 *
 * Most tests parse from memory through a reader callback (mem_read below: a table of virtual
 * path -> text). The example and VFS tests read real files: examples/assets.toml is mounted as
 * `aex:/`, tests/unit/asset/data/ as `adata:/` (read-only).
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"

#include <stdio.h>
#include <string.h>

/* ---- Helpers ----------------------------------------------------------------------------- */

typedef struct mem_file {
    const char *path;
    const char *text;
} mem_file;

static const mem_file *g_files;
static u32 g_file_count;
static u32 g_reads;

static de_result mem_read(void *user, const char *vpath, const de_allocator *a, void **data,
                          u64 *size) {
    (void)user;
    g_reads++;
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

/* Parses `root` from the in-memory files. */
static de__manifest *parse_mem(const mem_file *files, u32 count, const char *root,
                               const de_allocator *a) {
    g_files = files;
    g_file_count = count;
    g_reads = 0;
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {.path = root, .allocator = a, .read = mem_read};
    de_result r = de__manifest_parse(&d, &m);
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(m != NULL);
    return m;
}

#define PARSE(files, root)                                                                         \
    parse_mem((files), (u32)(sizeof(files) / sizeof((files)[0])), (root), NULL)

static void dump_issues(const de__manifest *m) {
    for (u32 i = 0; i < m->issue_count; ++i)
        fprintf(stderr, "  issue %u: sev=%d %s:%u [%s] %s\n", i, (int)m->issues[i].severity,
                m->issues[i].file, m->issues[i].line, m->issues[i].key_path, m->issues[i].message);
}

static const de__manifest_issue *find_issue(const de__manifest *m, de_asset_issue_severity sev,
                                            const char *key_path) {
    for (u32 i = 0; i < m->issue_count; ++i)
        if (m->issues[i].severity == sev && strcmp(m->issues[i].key_path, key_path) == 0)
            return &m->issues[i];
    return NULL;
}

static const de__manifest_kv *setting(const de__manifest_asset *a, const char *key) {
    return de__manifest_find_setting(a->settings, a->setting_count, key);
}

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("adata", DE_ASSET_TEST_DATA_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("aex", DE_ASSET_EXAMPLES_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    mounted = true;
}

/* Reads aex:/ (examples/) and falls back to adata:/example/ for the files the example includes
 * but the repo does not have (characters.toml, audio.toml). */
static de_result example_read(void *user, const char *vpath, const de_allocator *a, void **data,
                              u64 *size) {
    (void)user;
    de_result r = de_file_read_all(vpath, a, data, size);
    if (r == DE_ERR_NOT_FOUND && strncmp(vpath, "aex:/", 5) == 0) {
        char alt[DE_FS_PATH_MAX];
        snprintf(alt, sizeof alt, "adata:/example/%s", vpath + 5);
        r = de_file_read_all(alt, a, data, size);
    }
    return r;
}

static de__manifest *parse_example(const de_allocator *a) {
    mount_once();
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {.path = "aex:/assets.toml", .allocator = a, .read = example_read};
    ASSERT_EQ(de__manifest_parse(&d, &m), DE_OK);
    return m;
}

/* ---- examples/assets.toml ---------------------------------------------------------------- */

TEST(manifest, example_parses) {
    de__manifest *m = parse_example(NULL);
    if (m->issue_count != 0)
        dump_issues(m);
    EXPECT_EQ(m->error_count, 0u);
    EXPECT_EQ(m->warning_count, 0u);
    EXPECT_EQ(m->issue_count, 0u);

    ASSERT_EQ(m->file_count, 3u);
    EXPECT_STREQ(m->files[0], "aex:/assets.toml");
    EXPECT_STREQ(m->files[1], "aex:/characters.toml");
    EXPECT_STREQ(m->files[2], "aex:/audio.toml");
    EXPECT_STREQ(m->root, "aex:/assets.toml");
    EXPECT_STREQ(m->asset_root, "assets");
    EXPECT_TRUE(m->has_version);
    EXPECT_EQ(m->version, 1);

    /* 18 assets in the example, 2 in characters.toml, 2 in audio.toml; file order. */
    ASSERT_EQ(m->asset_count, 22u);
    EXPECT_STREQ(m->assets[0].id, "env/castle");
    EXPECT_STREQ(m->assets[17].id, "ui/logo");
    EXPECT_STREQ(m->assets[18].id, "player/model");
    EXPECT_STREQ(m->assets[18].file, "aex:/characters.toml");
    EXPECT_STREQ(m->assets[21].id, "music/credits");
    const de__manifest_asset *castle = de__manifest_find_asset(m, "env/castle");
    ASSERT_TRUE(castle != NULL);
    EXPECT_STREQ(castle->type, "model");
    EXPECT_STREQ(castle->source, "models/castle.glb");
    EXPECT_STREQ(castle->file, "aex:/assets.toml");
    EXPECT_GT(castle->line, 0u);

    /* 3 bundles in the example, 1 in characters.toml. */
    ASSERT_EQ(m->bundle_count, 4u);
    const de__manifest_bundle *common = de__manifest_find_bundle(m, "common");
    ASSERT_TRUE(common != NULL);
    ASSERT_EQ(common->asset_count, 2u);
    EXPECT_STREQ(common->assets[0], "ui/*");
    EXPECT_STREQ(common->assets[1], "sfx/click");
    EXPECT_EQ(common->depends_count, 0u);
    EXPECT_TRUE(common->resident);
    EXPECT_EQ(common->priority, DE__MANIFEST_PRIORITY_NORMAL);
    const de__manifest_bundle *level = de__manifest_find_bundle(m, "level_01");
    ASSERT_TRUE(level != NULL);
    EXPECT_EQ(level->asset_count, 4u);
    ASSERT_EQ(level->depends_count, 1u);
    EXPECT_STREQ(level->depends[0], "common");
    EXPECT_EQ(level->priority, DE__MANIFEST_PRIORITY_HIGH);
    EXPECT_FALSE(level->resident);
    EXPECT_TRUE(de__manifest_find_bundle(m, "player") != NULL);

    /* ui/logo locale overrides. */
    const de__manifest_asset *logo = de__manifest_find_asset(m, "ui/logo");
    ASSERT_TRUE(logo != NULL);
    EXPECT_STREQ(logo->type, "texture");
    EXPECT_STREQ(logo->source, "textures/logo_en.png");
    ASSERT_EQ(logo->locale_count, 2u);
    EXPECT_STREQ(logo->locales[0].locale, "ja");
    EXPECT_STREQ(logo->locales[0].source, "textures/logo_ja.png");
    EXPECT_STREQ(logo->locales[1].locale, "de");
    EXPECT_STREQ(logo->locales[1].source, "textures/logo_de.png");

    /* defaults.texture: compression, mips, srgb (sorted by key). defaults.sound from audio.toml
     * merged with the root's. */
    ASSERT_EQ(m->defaults_count, 3u); /* texture, model, sound */
    const de__manifest_defaults *tex = de__manifest_find_defaults(m, "texture");
    ASSERT_TRUE(tex != NULL);
    ASSERT_EQ(tex->setting_count, 3u);
    EXPECT_STREQ(tex->settings[0].key, "compression");
    EXPECT_EQ(tex->settings[0].value.type, DE__MANIFEST_STRING);
    EXPECT_STREQ(tex->settings[0].value.u.s, "auto");
    EXPECT_STREQ(tex->settings[1].key, "mips");
    EXPECT_EQ(tex->settings[1].value.type, DE__MANIFEST_BOOL);
    EXPECT_TRUE(tex->settings[1].value.u.b);
    EXPECT_STREQ(tex->settings[2].key, "srgb");
    EXPECT_TRUE(tex->settings[2].value.u.b);
    const de__manifest_defaults *model = de__manifest_find_defaults(m, "model");
    ASSERT_TRUE(model != NULL);
    const de__manifest_kv *lod =
        de__manifest_find_setting(model->settings, model->setting_count, "lod_error");
    ASSERT_TRUE(lod != NULL);
    EXPECT_EQ(lod->value.type, DE__MANIFEST_FLOAT);
    EXPECT_FLOAT_EQ(lod->value.u.f, 0.01, 1e-12);
    const de__manifest_defaults *sound = de__manifest_find_defaults(m, "sound");
    ASSERT_TRUE(sound != NULL);
    ASSERT_EQ(sound->setting_count, 2u);
    EXPECT_STREQ(sound->settings[0].key, "stream_above_seconds");
    EXPECT_EQ(sound->settings[0].value.type, DE__MANIFEST_INT);
    EXPECT_EQ(sound->settings[0].value.u.i, 10);
    EXPECT_STREQ(sound->settings[1].key, "volume");
    de__manifest_destroy(m);
}

/* ---- Settings overlay -------------------------------------------------------------------- */

TEST(manifest, defaults_overlay) {
    de__manifest *m = parse_example(NULL);
    ASSERT_EQ(m->error_count, 0u);

    const de__manifest_asset *n = de__manifest_find_asset(m, "env/stone_normal");
    ASSERT_TRUE(n != NULL);
    /* compression, mips (defaults), srgb (asset over defaults), usage (asset); sorted. */
    ASSERT_EQ(n->setting_count, 4u);
    EXPECT_STREQ(n->settings[0].key, "compression");
    EXPECT_STREQ(n->settings[1].key, "mips");
    EXPECT_STREQ(n->settings[2].key, "srgb");
    EXPECT_STREQ(n->settings[3].key, "usage");
    const de__manifest_kv *srgb = setting(n, "srgb");
    ASSERT_TRUE(srgb != NULL);
    EXPECT_EQ(srgb->value.type, DE__MANIFEST_BOOL);
    EXPECT_FALSE(srgb->value.u.b);
    const de__manifest_kv *comp = setting(n, "compression");
    ASSERT_TRUE(comp != NULL);
    EXPECT_EQ(comp->value.type, DE__MANIFEST_STRING);
    EXPECT_STREQ(comp->value.u.s, "auto");
    EXPECT_STREQ(setting(n, "usage")->value.u.s, "normal");
    EXPECT_TRUE(setting(n, "type") == NULL);
    EXPECT_TRUE(setting(n, "source") == NULL);

    /* An asset without own settings gets exactly the defaults. */
    const de__manifest_asset *alb = de__manifest_find_asset(m, "env/stone_albedo");
    ASSERT_TRUE(alb != NULL);
    EXPECT_EQ(alb->setting_count, 3u);
    EXPECT_TRUE(setting(alb, "srgb")->value.u.b);

    /* Float and bool settings of other types; defaults from an included file (audio.toml)
     * reach assets of the root file. */
    const de__manifest_asset *ruins = de__manifest_find_asset(m, "env/castle_ruins");
    ASSERT_TRUE(ruins != NULL);
    EXPECT_EQ(setting(ruins, "scale")->value.type, DE__MANIFEST_FLOAT);
    EXPECT_FLOAT_EQ(setting(ruins, "scale")->value.u.f, 0.01, 1e-12);
    EXPECT_TRUE(setting(ruins, "generate_lods")->value.u.b);
    const de__manifest_asset *music = de__manifest_find_asset(m, "music/title");
    ASSERT_TRUE(music != NULL);
    EXPECT_TRUE(setting(music, "stream")->value.u.b);
    EXPECT_EQ(setting(music, "stream_above_seconds")->value.u.i, 10);
    EXPECT_FLOAT_EQ(setting(music, "volume")->value.u.f, 1.0, 0.0);

    /* A type without defaults keeps only its own keys. */
    const de__manifest_asset *font = de__manifest_find_asset(m, "ui/font");
    ASSERT_TRUE(font != NULL);
    ASSERT_EQ(font->setting_count, 2u);
    EXPECT_STREQ(font->settings[0].key, "charset");
    EXPECT_STREQ(font->settings[1].key, "mode");
    de__manifest_destroy(m);
}

TEST(manifest, settings_types) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[assets.a]\n"
                             "type = 't'\nsource = 's'\n"
                             "tags = ['x', 'y']\nempty = []\nn = -7\nf = 2.5\nb = false\n"
                             "[assets.bad]\n"
                             "type = 't'\nsource = 's'\n"
                             "nested = { k = 1 }\n"
                             "[assets.bad2]\n"
                             "type = 't'\nsource = 's'\n"
                             "nums = [1, 2]\n"
                             "[assets.bad3]\n"
                             "type = 't'\nsource = 's'\n"
                             "when = 1979-05-27\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    const de__manifest_asset *a = de__manifest_find_asset(m, "a");
    ASSERT_TRUE(a != NULL);
    ASSERT_EQ(a->setting_count, 5u);
    const de__manifest_kv *tags = setting(a, "tags");
    ASSERT_TRUE(tags != NULL);
    ASSERT_EQ(tags->value.type, DE__MANIFEST_STRING_ARRAY);
    ASSERT_EQ(tags->value.u.arr.count, 2u);
    EXPECT_STREQ(tags->value.u.arr.items[0], "x");
    EXPECT_STREQ(tags->value.u.arr.items[1], "y");
    EXPECT_EQ(setting(a, "empty")->value.u.arr.count, 0u);
    EXPECT_EQ(setting(a, "n")->value.u.i, -7);
    EXPECT_FLOAT_EQ(setting(a, "f")->value.u.f, 2.5, 0.0);
    EXPECT_EQ(setting(a, "b")->value.type, DE__MANIFEST_BOOL);

    /* Tables, non-string arrays and dates are not settings: one error each, asset dropped. */
    EXPECT_EQ(m->error_count, 3u);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.bad.nested") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.bad2.nums") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.bad3.when") != NULL);
    EXPECT_TRUE(de__manifest_find_asset(m, "bad") == NULL);
    EXPECT_EQ(m->asset_count, 1u);
    de__manifest_destroy(m);
}

TEST(manifest, duplicate_default_across_files) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[project]\ninclude = ['b.toml']\n[defaults.texture]\nmips = true\n"},
        {"mem:/b.toml", "[defaults.texture]\nmips = false\nsrgb = true\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    ASSERT_EQ(m->error_count, 1u);
    const de__manifest_issue *is = find_issue(m, DE_ASSET_ISSUE_ERROR, "defaults.texture.mips");
    ASSERT_TRUE(is != NULL);
    EXPECT_STREQ(is->file, "mem:/b.toml");
    EXPECT_TRUE(strstr(is->message, "mem:/assets.toml") != NULL);
    const de__manifest_defaults *d = de__manifest_find_defaults(m, "texture");
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(d->setting_count, 2u);
    EXPECT_TRUE(d->settings[0].value.u.b); /* mips from the first file */
    de__manifest_destroy(m);
}

/* ---- Locale overrides -------------------------------------------------------------------- */

TEST(manifest, locale_overrides) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[assets.\"ui/logo\"]\n"
                             "type = 'texture'\n"
                             "source = 'logo_en.png'\n"
                             "[assets.\"ui/logo\".locale]\n"
                             "ja = 'logo_ja.png'\n"
                             "pt = 'logo_pt.png'\n"
                             "pt-BR = 'logo_pt_br.png'\n"
                             "zh-Hant = 'logo_zh_hant.png'\n"
                             "[assets]\n"
                             "\"ui/plain\" = { type = 'texture', source = 'plain.png' }\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    ASSERT_EQ(m->issue_count, 0u);
    const de__manifest_asset *logo = de__manifest_find_asset(m, "ui/logo");
    ASSERT_TRUE(logo != NULL);
    ASSERT_EQ(logo->locale_count, 4u);
    EXPECT_STREQ(de__manifest_source_for(m, logo, NULL), "logo_en.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, ""), "logo_en.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "en"), "logo_en.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "ja"), "logo_ja.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "JA"), "logo_ja.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "ja-JP"), "logo_ja.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "pt-BR"), "logo_pt_br.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "pt_br"), "logo_pt_br.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "pt-PT"), "logo_pt.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "zh-Hant-TW"), "logo_zh_hant.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "zh"), "logo_en.png");
    EXPECT_STREQ(de__manifest_source_for(m, logo, "j"), "logo_en.png");
    const de__manifest_asset *plain = de__manifest_find_asset(m, "ui/plain");
    ASSERT_TRUE(plain != NULL);
    EXPECT_STREQ(de__manifest_source_for(m, plain, "ja"), "plain.png");
    /* `locale` is not a setting. */
    EXPECT_EQ(logo->setting_count, 0u);
    de__manifest_destroy(m);
}

TEST(manifest, locale_errors) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[assets]\n"
                             "a = { type = 't', source = 's', locale = 'ja' }\n"
                             "b = { type = 't', source = 's', locale = { ja = 3 } }\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    EXPECT_EQ(m->error_count, 2u);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.a.locale") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.b.locale.ja") != NULL);
    EXPECT_EQ(m->asset_count, 0u);
    de__manifest_destroy(m);
}

/* ---- Duplicates and includes ------------------------------------------------------------- */

TEST(manifest, duplicate_id) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[project]\ninclude = ['a.toml', 'b.toml']\n"},
        {"mem:/a.toml", "[assets]\n'x' = { type = 'blob', source = 'one.bin' }\n"},
        {"mem:/b.toml", "[assets]\n'y' = { type = 'blob', source = 'y.bin' }\n"
                        "'x' = { type = 'blob', source = 'two.bin' }\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    if (m->error_count != 1)
        dump_issues(m);
    ASSERT_EQ(m->error_count, 1u);
    ASSERT_EQ(m->issue_count, 1u);
    const de__manifest_issue *is = &m->issues[0];
    EXPECT_EQ(is->severity, DE_ASSET_ISSUE_ERROR);
    EXPECT_EQ(is->code, DE_ERR_PARSE);
    EXPECT_STREQ(is->file, "mem:/b.toml");
    EXPECT_STREQ(is->key_path, "assets.x");
    EXPECT_EQ(is->line, 3u);
    EXPECT_TRUE(strstr(is->message, "mem:/a.toml") != NULL);
    EXPECT_TRUE(strstr(is->message, "mem:/b.toml") != NULL);
    /* The first declaration stays. */
    ASSERT_EQ(m->asset_count, 2u);
    EXPECT_STREQ(de__manifest_find_asset(m, "x")->source, "one.bin");
    de__manifest_destroy(m);
}

TEST(manifest, duplicate_bundle) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[project]\ninclude = ['a.toml']\n[bundles.b]\nassets = ['x']\n"},
        {"mem:/a.toml", "[bundles.b]\nassets = ['y']\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    ASSERT_EQ(m->error_count, 1u);
    const de__manifest_issue *is = find_issue(m, DE_ASSET_ISSUE_ERROR, "bundles.b");
    ASSERT_TRUE(is != NULL);
    EXPECT_STREQ(is->file, "mem:/a.toml");
    EXPECT_TRUE(strstr(is->message, "mem:/assets.toml") != NULL);
    ASSERT_EQ(m->bundle_count, 1u);
    EXPECT_STREQ(m->bundles[0].assets[0], "x");
    de__manifest_destroy(m);
}

TEST(manifest, include_cycle) {
    static const mem_file files[] = {
        {"mem:/a.toml", "[project]\ninclude = ['b.toml']\n[assets]\na = {type='t', source='a'}\n"},
        {"mem:/b.toml", "[project]\ninclude = ['a.toml']\n[assets]\nb = {type='t', source='b'}\n"},
    };
    de__manifest *m = PARSE(files, "mem:/a.toml");
    if (m->error_count != 1)
        dump_issues(m);
    ASSERT_EQ(m->error_count, 1u);
    const de__manifest_issue *is = &m->issues[0];
    EXPECT_STREQ(is->file, "mem:/b.toml");
    EXPECT_STREQ(is->key_path, "project.include[0]");
    EXPECT_TRUE(strstr(is->message, "cycle") != NULL);
    EXPECT_TRUE(strstr(is->message, "mem:/a.toml -> mem:/b.toml -> mem:/a.toml") != NULL);
    EXPECT_EQ(g_reads, 2u); /* each file read once: the walk terminated */
    EXPECT_EQ(m->file_count, 2u);
    EXPECT_EQ(m->asset_count, 2u);
    de__manifest_destroy(m);

    /* A file that includes itself. */
    static const mem_file self[] = {{"mem:/s.toml", "[project]\ninclude = ['./s.toml']\n"}};
    m = PARSE(self, "mem:/s.toml");
    ASSERT_EQ(m->error_count, 1u);
    EXPECT_TRUE(strstr(m->issues[0].message, "cycle") != NULL);
    de__manifest_destroy(m);
}

TEST(manifest, include_duplicate_and_nested_paths) {
    /* Diamond: root includes l/x.toml and r/y.toml, both include ../shared/z.toml. */
    static const mem_file files[] = {
        {"mem:/game/assets.toml", "[project]\ninclude = ['l/x.toml', 'r/y.toml']\n"},
        {"mem:/game/l/x.toml", "[project]\ninclude = ['../shared/z.toml']\n"},
        {"mem:/game/r/y.toml", "[project]\ninclude = ['../shared//./z.toml']\n"},
        {"mem:/game/shared/z.toml", "[assets]\nz = { type = 't', source = 'z' }\n"},
    };
    de__manifest *m = PARSE(files, "mem:/game/assets.toml");
    ASSERT_EQ(m->error_count, 1u);
    const de__manifest_issue *is = &m->issues[0];
    EXPECT_STREQ(is->file, "mem:/game/r/y.toml");
    EXPECT_TRUE(strstr(is->message, "mem:/game/shared/z.toml") != NULL);
    EXPECT_TRUE(strstr(is->message, "more than once") != NULL);
    ASSERT_EQ(m->file_count, 4u);
    EXPECT_STREQ(m->files[2], "mem:/game/shared/z.toml");
    EXPECT_EQ(m->asset_count, 1u);
    de__manifest_destroy(m);
}

TEST(manifest, include_errors) {
    static const mem_file files[] = {
        {"mem:/assets.toml",
         "[project]\n"
         "include = ['missing.toml', '../../outside.toml', 'dev:/other.toml', '', 3, "
         "'/abs.toml', 'ok.toml']\n"},
        {"mem:/ok.toml", "[assets]\nok = { type = 't', source = 'ok' }\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    if (m->error_count != 6)
        dump_issues(m);
    ASSERT_EQ(m->error_count, 6u);
    EXPECT_EQ(m->issues[0].code, DE_ERR_NOT_FOUND);
    EXPECT_STREQ(m->issues[0].key_path, "project.include[0]");
    EXPECT_TRUE(strstr(m->issues[0].message, "mem:/missing.toml") != NULL);
    for (u32 i = 1; i < 6; ++i) {
        EXPECT_EQ(m->issues[i].code, DE_ERR_PARSE);
        EXPECT_STREQ(m->issues[i].file, "mem:/assets.toml");
        EXPECT_EQ(m->issues[i].line, 2u);
    }
    EXPECT_STREQ(m->issues[4].key_path, "project.include[4]");
    /* The good include after the bad ones is still read. */
    EXPECT_EQ(m->asset_count, 1u);
    EXPECT_EQ(m->file_count, 2u);
    de__manifest_destroy(m);
}

TEST(manifest, include_depth_limit) {
    /* Every file includes "d/next.toml" relative to itself: an endless chain of new paths. */
    static char paths[40][DE_FS_PATH_MAX];
    static mem_file files[40];
    for (u32 i = 0; i < 40; ++i) {
        size_t n = (size_t)snprintf(paths[i], sizeof paths[i], "mem:/");
        for (u32 k = 0; k < i; ++k)
            n += (size_t)snprintf(paths[i] + n, sizeof paths[i] - n, "d/");
        snprintf(paths[i] + n, sizeof paths[i] - n, "next.toml");
        files[i] = (mem_file){paths[i], "[project]\ninclude = ['d/next.toml']\n"};
    }
    de__manifest *m = parse_mem(files, 40, "mem:/next.toml", NULL);
    ASSERT_EQ(m->error_count, 1u);
    EXPECT_TRUE(strstr(m->issues[0].message, "deeper") != NULL);
    EXPECT_EQ(m->file_count, DE__MANIFEST_MAX_INCLUDE_DEPTH + 1u);
    de__manifest_destroy(m);
}

/* ---- Required keys and bundle fields ----------------------------------------------------- */

TEST(manifest, missing_type) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[assets]\n"
                             "no_type = { source = 'a.png' }\n"
                             "no_source = { type = 'texture' }\n"
                             "empty_type = { type = '', source = 'b.png' }\n"
                             "not_table = 'c.png'\n"
                             "num_type = { type = 3, source = 'd.png' }\n"
                             "'' = { type = 'texture', source = 'e.png' }\n"
                             "good = { type = 'texture', source = 'f.png' }\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    if (m->error_count != 6)
        dump_issues(m);
    ASSERT_EQ(m->error_count, 6u);
    const de__manifest_issue *is = find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.no_type");
    ASSERT_TRUE(is != NULL);
    EXPECT_TRUE(strstr(is->message, "`type`") != NULL);
    EXPECT_EQ(is->line, 2u);
    is = find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.no_source");
    ASSERT_TRUE(is != NULL);
    EXPECT_TRUE(strstr(is->message, "`source`") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.empty_type.type") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.not_table") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.num_type.type") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets.\"\"") != NULL);
    /* Only the good asset is kept. Issues are in key order. */
    ASSERT_EQ(m->asset_count, 1u);
    EXPECT_STREQ(m->assets[0].id, "good");
    EXPECT_STREQ(m->issues[0].key_path, "assets.no_type");
    EXPECT_STREQ(m->issues[1].key_path, "assets.no_source");
    de__manifest_destroy(m);
}

TEST(manifest, bundle_fields) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[bundles.ok]\n"
                             "priority = 'background'\n"
                             "[bundles.empty]\n"
                             "[bundles.bad_priority]\n"
                             "priority = 'urgent'\n"
                             "[bundles.bad_resident]\n"
                             "resident = 'yes'\n"
                             "[bundles.bad_assets]\n"
                             "assets = 'ui/*'\n"
                             "[bundles.bad_depends]\n"
                             "depends = ['a', 2]\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    ASSERT_EQ(m->error_count, 4u);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "bundles.bad_priority.priority") != NULL);
    EXPECT_TRUE(
        strstr(find_issue(m, DE_ASSET_ISSUE_ERROR, "bundles.bad_priority.priority")->message,
               "\"urgent\"") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "bundles.bad_resident.resident") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "bundles.bad_assets.assets") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "bundles.bad_depends.depends") != NULL);
    ASSERT_EQ(m->bundle_count, 2u);
    EXPECT_STREQ(m->bundles[0].name, "ok");
    EXPECT_EQ(m->bundles[0].priority, DE__MANIFEST_PRIORITY_BACKGROUND);
    EXPECT_STREQ(m->bundles[1].name, "empty");
    EXPECT_EQ(m->bundles[1].priority, DE__MANIFEST_PRIORITY_NORMAL);
    EXPECT_EQ(m->bundles[1].asset_count, 0u);
    EXPECT_STREQ(de__manifest_priority_name(DE__MANIFEST_PRIORITY_CRITICAL), "critical");
    de__manifest_destroy(m);
}

/* ---- Unknown keys and variants ----------------------------------------------------------- */

TEST(manifest, unknown_key_warns) {
    static const mem_file files[] = {
        {"mem:/assets.toml", "[project]\n"
                             "asset_root = 'assets'\n"
                             "colour = 'blue'\n"
                             "include = ['inc.toml']\n"
                             "[bundles.common]\n"
                             "assets = ['ui/*']\n"
                             "evict = false\n"
                             "[extra]\n"
                             "x = 1\n"},
        {"mem:/inc.toml", "[project]\nasset_root = 'other'\n[bundles.\"a b\"]\nfoo = 1\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    if (m->warning_count != 5)
        dump_issues(m);
    EXPECT_EQ(m->error_count, 0u);
    ASSERT_EQ(m->warning_count, 5u);
    /* File order, then key order. */
    EXPECT_STREQ(m->issues[0].key_path, "project.colour");
    EXPECT_STREQ(m->issues[0].file, "mem:/assets.toml");
    EXPECT_EQ(m->issues[0].line, 3u);
    EXPECT_STREQ(m->issues[1].key_path, "bundles.common.evict");
    EXPECT_EQ(m->issues[1].line, 7u);
    EXPECT_STREQ(m->issues[2].key_path, "extra");
    EXPECT_STREQ(m->issues[3].key_path, "project.asset_root");
    EXPECT_STREQ(m->issues[3].file, "mem:/inc.toml");
    EXPECT_STREQ(m->issues[4].key_path, "bundles.\"a b\".foo");
    EXPECT_STREQ(m->issues[4].file, "mem:/inc.toml");
    /* Warnings ignore the input: the root's asset_root stays, the bundles are kept. */
    EXPECT_STREQ(m->asset_root, "assets");
    EXPECT_EQ(m->bundle_count, 2u);
    de__manifest_destroy(m);
}

TEST(manifest, variants_ignored) {
    static const mem_file files[] = {
        {"mem:/assets.toml",
         "variants = ['pc']\n"
         "[project]\n"
         "variants = ['low', 'high']\n"
         "[defaults.texture]\n"
         "variants = { low = { mips = false } }\n"
         "mips = true\n"
         "[assets]\n"
         "a = { type = 'texture', source = 'a.png', variants = { low = 'a_low.png' } }\n"
         "[bundles.b]\n"
         "assets = ['a']\n"
         "variants = ['console']\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    if (m->info_count != 5)
        dump_issues(m);
    EXPECT_EQ(m->error_count, 0u);
    EXPECT_EQ(m->warning_count, 0u);
    ASSERT_EQ(m->info_count, 5u);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_INFO, "variants") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_INFO, "project.variants") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_INFO, "defaults.texture.variants") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_INFO, "assets.a.variants") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_INFO, "bundles.b.variants") != NULL);
    const de__manifest_asset *a = de__manifest_find_asset(m, "a");
    ASSERT_TRUE(a != NULL);
    ASSERT_EQ(a->setting_count, 1u);
    EXPECT_STREQ(a->settings[0].key, "mips");
    EXPECT_EQ(m->bundle_count, 1u);
    de__manifest_destroy(m);
}

/* ---- Malformed input --------------------------------------------------------------------- */

TEST(manifest, malformed_toml) {
    static const mem_file files[] = {
        {"mem:/assets.toml",
         "[project]\ninclude = ['bad.toml']\n[assets]\nok = {type='t', source='s'}\n"},
        {"mem:/bad.toml", "# comment\n[assets]\n'x' = { type = 'blob' source = 'x' }\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    ASSERT_EQ(m->error_count, 1u);
    const de__manifest_issue *is = &m->issues[0];
    EXPECT_EQ(is->code, DE_ERR_PARSE);
    EXPECT_STREQ(is->file, "mem:/bad.toml");
    EXPECT_STREQ(is->key_path, "");
    EXPECT_EQ(is->line, 3u);
    EXPECT_TRUE(strstr(is->message, "invalid TOML") != NULL);
    EXPECT_EQ(m->asset_count, 1u);
    de__manifest_destroy(m);

    /* Through the system and the VFS: DE_ERR_PARSE, issues kept for tools. */
    mount_once();
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &s), DE_OK);
    EXPECT_EQ(de_asset_manifest_load(s, "adata:/malformed.toml"), DE_ERR_PARSE);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(strstr(e->message, "adata:/malformed.toml:5") != NULL);
    u32 n = 0;
    const de_asset_manifest_issue *issues = de_asset_manifest_issues(s, &n);
    ASSERT_EQ(n, 1u);
    EXPECT_STREQ(issues[0].file, "adata:/malformed.toml");
    EXPECT_EQ(issues[0].line, 5u);
    EXPECT_TRUE(s->manifest == NULL);
    de_asset_system_destroy(s);
}

TEST(manifest, wrong_section_types) {
    static const mem_file files[] = {
        {"mem:/assets.toml",
         "project = 1\ndefaults = { texture = 3 }\nassets = []\nbundles = 'x'\n"},
        {"mem:/b.toml", "[project]\nasset_root = 1\nversion = 'one'\ninclude = 'x.toml'\n"},
    };
    de__manifest *m = PARSE(files, "mem:/assets.toml");
    EXPECT_EQ(m->error_count, 4u);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "project") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "defaults.texture") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "assets") != NULL);
    EXPECT_TRUE(find_issue(m, DE_ASSET_ISSUE_ERROR, "bundles") != NULL);
    de__manifest_destroy(m);

    m = PARSE(files, "mem:/b.toml");
    EXPECT_EQ(m->error_count, 3u);
    EXPECT_FALSE(m->has_version);
    EXPECT_STREQ(m->asset_root, "");
    de__manifest_destroy(m);
}

TEST(manifest, root_errors) {
    static const mem_file files[] = {{"mem:/assets.toml", ""}};
    de__manifest *m = PARSE(files, "mem:/missing.toml");
    ASSERT_EQ(m->error_count, 1u);
    EXPECT_EQ(m->issues[0].code, DE_ERR_NOT_FOUND);
    EXPECT_STREQ(m->issues[0].file, "mem:/missing.toml");
    EXPECT_EQ(m->file_count, 0u);
    de__manifest_destroy(m);

    /* An empty manifest is valid. */
    m = PARSE(files, "mem:/assets.toml");
    EXPECT_EQ(m->issue_count, 0u);
    EXPECT_EQ(m->asset_count, 0u);
    de__manifest_destroy(m);

    /* Not a virtual path. */
    de__manifest_parse_desc d = {.path = "/etc/assets.toml", .read = mem_read};
    de__manifest *out = (de__manifest *)(void *)&d;
    EXPECT_EQ(de__manifest_parse(&d, &out), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(out == NULL);
    EXPECT_EQ(de__manifest_parse(NULL, &out), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__manifest_parse(&d, NULL), DE_ERR_INVALID_ARGUMENT);

    /* Through the system: a missing root is DE_ERR_NOT_FOUND. */
    mount_once();
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &s), DE_OK);
    EXPECT_EQ(de_asset_manifest_load(s, "adata:/nope.toml"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_asset_manifest_load(s, "C:\\assets.toml"), DE_ERR_INVALID_ARGUMENT);
    de_asset_system_destroy(s);
}

/* ---- Allocation failures ----------------------------------------------------------------- */

TEST(manifest, oom_sweep) {
    /* Every allocation failure gives DE_ERR_OUT_OF_MEMORY and leaks nothing; with enough
     * allocations the parse succeeds. Runs over the example (tomlc17, includes, overlay). */
    mount_once();
    de_tracking_allocator t;
    const de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_ASSET);
    bool done = false;
    i64 n = 0;
    for (; n < 100000 && !done; ++n) {
        de_tracking_allocator_fail_after(&t, n);
        de__manifest *m = NULL;
        de__manifest_parse_desc d = {
            .path = "aex:/assets.toml", .allocator = &a, .read = example_read};
        de_result r = de__manifest_parse(&d, &m);
        if (r == DE_OK) {
            ASSERT_TRUE(m != NULL);
            if (m->error_count != 0)
                dump_issues(m);
            EXPECT_EQ(m->error_count, 0u);
            EXPECT_EQ(m->asset_count, 22u);
            de__manifest_destroy(m);
            done = true;
        } else {
            ASSERT_EQ(r, DE_ERR_OUT_OF_MEMORY);
            ASSERT_TRUE(m == NULL);
        }
        ASSERT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    }
    EXPECT_TRUE(done);
    /* Many allocation points were failed (chunks, temporaries, tomlc17 blocks, file bytes). */
    EXPECT_GT(n, 50);
}

/* ---- de_asset_system --------------------------------------------------------------------- */

TEST(asset_system, create_zero_desc) {
    de_mem_stats before = de_mem_stats_for_tag(DE_MEM_ASSET);
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){0}, &s), DE_OK);
    ASSERT_TRUE(s != NULL);
    EXPECT_TRUE(s->alloc == de_heap_allocator());
    EXPECT_TRUE(s->tasks == NULL);
    EXPECT_EQ(s->desc.mode, DE_ASSET_MODE_SOURCE);
    EXPECT_STREQ(s->desc.cache_dir, "cache:/decache");
    EXPECT_EQ(s->desc.io_threads, 2u);
    EXPECT_EQ(s->desc.staging_ring_bytes, (u64)128 * 1024 * 1024);
    EXPECT_TRUE(de__asset_locale(s) == NULL);
    EXPECT_TRUE(s->manifest == NULL);
    u32 n = 99;
    EXPECT_TRUE(de_asset_manifest_issues(s, &n) == NULL);
    EXPECT_EQ(n, 0u);
    de_asset_system_destroy(s);
    de_asset_system_destroy(NULL);
    /* The heap reports blocks to the tag stats in Debug and Dev builds. */
    de_mem_stats after = de_mem_stats_for_tag(DE_MEM_ASSET);
    if (DE_MEM_TRACKING)
        EXPECT_EQ(after.bytes_live, before.bytes_live);

    s = NULL;
    EXPECT_EQ(de_asset_system_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_asset_system_create(NULL, &s), DE_OK); /* NULL desc = zeroed desc */
    de_asset_system_destroy(s);
}

TEST(asset_system, destroy_frees_everything) {
    mount_once();
    de_tracking_allocator t;
    const de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_ASSET);
    char cache[] = "cache:/mine";
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(
                  &(de_asset_system_desc){.allocator = &a, .cache_dir = cache, .locale = "ja"}, &s),
              DE_OK);
    cache[0] = 'X'; /* the desc string was copied */
    EXPECT_STREQ(s->desc.cache_dir, "cache:/mine");
    EXPECT_STREQ(de__asset_locale(s), "ja");

    /* A good load, a failed one (keeps the good manifest active), another good one. */
    ASSERT_EQ(de_asset_manifest_load(s, "adata:/vfs/assets.toml"), DE_OK);
    de__manifest *first = s->manifest;
    ASSERT_TRUE(first != NULL);
    EXPECT_EQ(de_asset_manifest_load(s, "adata:/malformed.toml"), DE_ERR_PARSE);
    EXPECT_TRUE(s->manifest == first);
    ASSERT_EQ(de_asset_manifest_load(s, "adata:/vfs/assets.toml"), DE_OK);
    EXPECT_TRUE(s->failed_manifest == NULL);
    EXPECT_GT(de_tracking_allocator_stats(&t).bytes_live, 0u);
    de_asset_system_destroy(s);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&t).blocks_live, 0u);
}

TEST(asset_system, manifest_load_vfs) {
    /* The default reader (de_file_read_all) and includes relative to the including file:
     * vfs/assets.toml -> sub/more.toml -> ../shared.toml. */
    mount_once();
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &s), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(s, "adata:/vfs/assets.toml"), DE_OK);
    const de__manifest *m = s->manifest;
    ASSERT_TRUE(m != NULL);
    ASSERT_EQ(m->file_count, 3u);
    EXPECT_STREQ(m->files[1], "adata:/vfs/sub/more.toml");
    EXPECT_STREQ(m->files[2], "adata:/vfs/shared.toml");
    EXPECT_EQ(m->version, 2);
    ASSERT_EQ(m->asset_count, 2u);
    const de__manifest_asset *banner = de__manifest_find_asset(m, "ui/banner");
    ASSERT_TRUE(banner != NULL);
    EXPECT_STREQ(banner->file, "adata:/vfs/sub/more.toml");
    EXPECT_FALSE(setting(banner, "srgb")->value.u.b);
    EXPECT_TRUE(setting(de__manifest_find_asset(m, "ui/icon"), "srgb")->value.u.b);
    const de__manifest_bundle *ui = de__manifest_find_bundle(m, "ui");
    ASSERT_TRUE(ui != NULL);
    EXPECT_EQ(ui->priority, DE__MANIFEST_PRIORITY_CRITICAL);
    EXPECT_TRUE(ui->resident);
    u32 n = 99;
    de_asset_manifest_issues(s, &n);
    EXPECT_EQ(n, 0u);
    de_asset_system_destroy(s);
}

TEST(asset_system, set_locale) {
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &s), DE_OK);
    EXPECT_TRUE(de__asset_locale(s) == NULL);
    de_asset_set_locale(s, "de-AT");
    EXPECT_STREQ(de__asset_locale(s), "de-AT");
    /* Too long: refused, the previous locale stays. */
    de_asset_set_locale(s, "a-very-long-locale-tag-that-is-not-a-real-bcp47-tag");
    EXPECT_STREQ(de__asset_locale(s), "de-AT");
    de_asset_set_locale(s, "");
    EXPECT_TRUE(de__asset_locale(s) == NULL);
    de_asset_set_locale(s, "ja");
    de_asset_set_locale(s, NULL);
    EXPECT_TRUE(de__asset_locale(s) == NULL);
    de_asset_set_locale(NULL, "ja");

    /* The locale picks the source through de__manifest_source_for. */
    mount_once();
    de__manifest *m = parse_example(NULL);
    const de__manifest_asset *logo = de__manifest_find_asset(m, "ui/logo");
    ASSERT_TRUE(logo != NULL);
    de_asset_set_locale(s, "de-AT");
    EXPECT_STREQ(de__manifest_source_for(m, logo, de__asset_locale(s)), "textures/logo_de.png");
    de_asset_set_locale(s, "fr");
    EXPECT_STREQ(de__manifest_source_for(m, logo, de__asset_locale(s)), "textures/logo_en.png");
    de__manifest_destroy(m);
    de_asset_system_destroy(s);

    /* A too-long desc locale fails create. */
    s = NULL;
    EXPECT_EQ(de_asset_system_create(
                  &(de_asset_system_desc){.locale = "x-0123456789-0123456789-0123456789-0123"}, &s),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(s == NULL);
}
