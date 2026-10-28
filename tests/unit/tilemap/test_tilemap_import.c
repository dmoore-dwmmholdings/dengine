/*
 * test_tilemap_import.c: the tilemap importer (DENG-80; tools/cook/importers/tilemap/). CTest
 * names tilemap.<test>.
 *
 *   json_reader_strict         the JSON reader accepts RFC 8259 and refuses everything else
 *   xml_reader_subset          the XML reader: entities, CDATA, comments, DOCTYPE, errors
 *   ldtk_import_fixture        2 external levels, IntGrid + baked auto-layer, tiles, entities
 *   tiled_tmj_tmx_equivalent   the .tmj (external .tsj) and .tmx (external .tsx) fixtures cook
 *                              to the same bytes, without warnings; tilesets, animation, shapes
 *   tiled_layer_encodings      CSV, base64 plain / zlib / gzip / zstd, TMX <tile>: same cells;
 *                              an unknown compression fails naming the layer
 *   tilemap_cook_deterministic through de_cooker (verify on): same bytes twice, extra sources
 *   cook_check_accepts_tilemap `de-cook --check` accepts the `tilemap` entries when the sources
 *                              exist
 *   cooked_view_rejects        the runtime parser refuses truncated or damaged cooked bytes
 */
#include "tilemap_test_util.h"

#include "asset/manifest_internal.h"
#include "check.h"
#include "cook/importers/tilemap/tilemap_ir.h"
#include "cook/lib/de_cooker.h"

#include <dengine/de_asset.h>

#include <math.h>
#include <stdlib.h>

typedef struct imp_fx {
    de_tracking_allocator track;
    de_allocator alloc;
} imp_fx;

static void imp_fx_setup(imp_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    tm_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
}

static void imp_fx_teardown(imp_fx *f) {
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    de_log_shutdown();
}

/* ---- json_reader_strict ---------------------------------------------------------------------- */

static bool json_ok(tm_arena *ar, const char *text) {
    char err[160];
    return tmj_parse(ar, text, strlen(text), err, sizeof err) != NULL;
}

TEST_F(tilemap, json_reader_strict, imp_fx) {
    tm_arena ar;
    tm_arena_init(&ar, &f->alloc, 0);
    char err[160];
    const char *doc = " {\"a\": [1, -2.5e2, 0, true, false, null], \"s\": \"x\\u00e9\\ud83d\\ude00"
                      "\\n\\\"\", \"o\": {\"k\": {}} , \"e\": []} ";
    const tmj_val *v = tmj_parse(&ar, doc, strlen(doc), err, sizeof err);
    ASSERT_TRUE(v != NULL);
    ASSERT_EQ((int)v->kind, (int)TMJ_OBJ);
    const tmj_val *a = tmj_arr(v, "a");
    ASSERT_TRUE(a != NULL);
    ASSERT_EQ(a->count, 6u);
    EXPECT_FLOAT_EQ(a->u.items[1].u.n, -250.0, 0.0);
    EXPECT_EQ((int)a->u.items[5].kind, (int)TMJ_NULL);
    /* é (2 bytes), U+1F600 (4 bytes), newline, quote. */
    EXPECT_STREQ(tmj_str(v, "s", ""), "x\xc3\xa9\xf0\x9f\x98\x80\n\"");
    EXPECT_EQ(tmj_get(v, "s")->count, 9u);
    EXPECT_EQ((int)tmj_get(tmj_get(v, "o"), "k")->kind, (int)TMJ_OBJ);
    EXPECT_TRUE(json_ok(&ar, "\xEF\xBB\xBF[1]")); /* BOM */
    EXPECT_TRUE(json_ok(&ar, "0"));
    EXPECT_TRUE(json_ok(&ar, "\"\""));

    static const char *const bad[] = {
        "[1,]",          "{\"a\":1,}",     "// c\n{}",       "/* c */ {}",   "[01]",
        "[1.]",          "[.5]",           "[1e]",           "[+1]",         "[NaN]",
        "[Infinity]",    "['a']",          "{a:1}",          "[\"\x01\"]",   "[\"\\x\"]",
        "[\"\\ud800\"]", "[\"\\udc00\"]",  "[\"\\u12\"]",    "[\"\\u0000\"]", "[1] [2]",
        "",              "[",              "{\"a\"}",        "[tru]",        "[1e999]",
        "\"unterminated"};
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        if (json_ok(&ar, bad[i]))
            printf("  accepted: %s\n", bad[i]);
        EXPECT_FALSE(json_ok(&ar, bad[i]));
    }
    /* Depth: 64 levels are fine, 66 are not. */
    char deep[200];
    memset(deep, '[', 64);
    memset(deep + 64, ']', 64);
    deep[128] = '\0';
    EXPECT_TRUE(json_ok(&ar, deep));
    memset(deep, '[', 66);
    memset(deep + 66, ']', 66);
    deep[132] = '\0';
    EXPECT_FALSE(json_ok(&ar, deep));
    /* The message has a line and column. */
    EXPECT_TRUE(tmj_parse(&ar, "{\n  \"a\": x}", 11, err, sizeof err) == NULL);
    EXPECT_TRUE(strncmp(err, "2:8:", 4) == 0);
    tm_arena_free(&ar);
}

/* ---- xml_reader_subset ----------------------------------------------------------------------- */

static bool xml_ok(tm_arena *ar, const char *text) {
    char err[160];
    return tmx_parse(ar, text, strlen(text), err, sizeof err) != NULL;
}

TEST_F(tilemap, xml_reader_subset, imp_fx) {
    tm_arena ar;
    tm_arena_init(&ar, &f->alloc, 0);
    char err[160];
    const char *doc = "<?xml version=\"1.0\"?>\n<!DOCTYPE map [ <!ENTITY x \"y\"> ]>\n"
                      "<!-- c --><map a='1' b=\"&lt;&amp;&#65;&#x42;&quot;\">"
                      "<layer name=\"l\"/><data>1,<![CDATA[2<,]]>3<!-- inner --></data></map>\n";
    const tmx_node *root = tmx_parse(&ar, doc, strlen(doc), err, sizeof err);
    ASSERT_TRUE(root != NULL);
    EXPECT_STREQ(root->name, "map");
    EXPECT_STREQ(tmx_attr_str(root, "a", ""), "1");
    EXPECT_STREQ(tmx_attr_str(root, "b", ""), "<&AB\"");
    EXPECT_FLOAT_EQ(tmx_attr_num(root, "a", 0), 1.0, 0.0);
    ASSERT_EQ(root->child_count, 2u);
    EXPECT_STREQ(tmx_attr_str(tmx_child(root, "layer"), "name", ""), "l");
    EXPECT_STREQ(tmx_child(root, "data")->text, "1,2<,3");

    static const char *const bad[] = {"<a>",       "<a></b>",  "<a b=1/>",   "<a b='1/>",
                                      "<a>&bogus;</a>", "<a>&#0;</a>", "<a/><b/>", "text",
                                      "<a b='<'/>", "<a\"/>",  "<!-- x",     "<a><![CDATA[x</a>"};
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        if (xml_ok(&ar, bad[i]))
            printf("  accepted: %s\n", bad[i]);
        EXPECT_FALSE(xml_ok(&ar, bad[i]));
    }
    char deep[400];
    size_t n = 0;
    for (u32 i = 0; i < 66; ++i)
        n += (size_t)snprintf(deep + n, sizeof deep - n, "<a>");
    for (u32 i = 0; i < 66 && n + 4 < sizeof deep; ++i)
        n += (size_t)snprintf(deep + n, sizeof deep - n, "</a>");
    EXPECT_FALSE(xml_ok(&ar, deep));
    tm_arena_free(&ar);
}

/* ---- ldtk_import_fixture ---------------------------------------------------------------------- */

static const char *const k_entities[] = {"Player=prefabs/player"};

TEST_F(tilemap, ldtk_import_fixture, imp_fx) {
    tm_settings st = {k_entities, 1, NULL, 0, 0};
    de__tm_import_result r;
    ASSERT_EQ(tm_import_file(&f->alloc, "maps/fixture.ldtk", &st, &r), DE_OK);
    EXPECT_EQ(r.warnings, 0u);
    de__tm_view v;
    const char *why = NULL;
    ASSERT_TRUE(de__tm_view_parse(r.data, r.size, &v, &why));
    EXPECT_FLOAT_EQ(v.h.pixels_per_unit, 16.0, 0.0);
    ASSERT_EQ(v.h.tileset_count, 1u);
    EXPECT_EQ(de__tm_tileset_at(&v, 0).texture, de_asset_id_from("tiles/terrain").hash);
    EXPECT_STREQ(de__tm_str(&v, de__tm_tileset_at(&v, 0).name), "Terrain");
    EXPECT_EQ(v.h.tile_count, 8u);
    /* 2 levels x (Walls, Decor) in draw order; the Entities layers become spawns. */
    ASSERT_EQ(v.h.layer_count, 4u);
    const char *names[4] = {"Level_0/Walls", "Level_0/Decor", "Level_1/Walls", "Level_1/Decor"};
    for (u32 i = 0; i < 4; ++i)
        EXPECT_STREQ(de__tm_str(&v, de__tm_layer_at(&v, i).name), names[i]);
    de__tm_layer walls0 = de__tm_layer_at(&v, 0), walls1 = de__tm_layer_at(&v, 2);
    EXPECT_EQ(walls0.width, 24u);
    EXPECT_EQ(walls0.height, 16u);
    EXPECT_TRUE((walls0.flags & DE__TM_LAYER_INTGRID) != 0);
    EXPECT_FLOAT_EQ(walls0.origin_u, 0.0, 0.0);
    EXPECT_FLOAT_EQ(walls1.origin_u, 24.0, 0.0); /* worldX 384 px */
    EXPECT_FLOAT_EQ(walls1.cell_w, 1.0, 0.0);
    /* IntGrid: the two bottom rows and a wall column are solid; the auto-layer put grass (tile
     * 0) on the top surface and dirt (tile 4) below. */
    EXPECT_EQ(tm_view_cell(&v, 0, 0, 14), DE__TM_CELL_SOLID | 1u);
    EXPECT_EQ(tm_view_cell(&v, 0, 0, 15), DE__TM_CELL_SOLID | 5u);
    EXPECT_EQ(tm_view_cell(&v, 0, 5, 8), DE__TM_CELL_SOLID | 1u);
    EXPECT_EQ(tm_view_cell(&v, 0, 5, 9), DE__TM_CELL_SOLID | 5u);
    EXPECT_EQ(tm_view_cell(&v, 0, 6, 9), 0u);
    EXPECT_EQ(tm_view_cell(&v, 2, 18, 8), DE__TM_CELL_SOLID | 1u);
    /* Decor: flowers (tile 5) every 4 cells on row 13, every other one flipped in x. */
    EXPECT_EQ(tm_view_cell(&v, 1, 1, 13), DE__TM_CELL_FLIP_H | 6u);
    EXPECT_EQ(tm_view_cell(&v, 1, 5, 13), 6u);
    EXPECT_EQ(tm_view_cell(&v, 3, 10, 4), DE__TM_CELL_FLIP_V | 7u); /* brick, flipped in y */
    /* Spawns: Player (mapped) and Coin (not mapped). */
    ASSERT_EQ(v.h.spawn_count, 2u);
    de__tm_spawn p = de__tm_spawn_at(&v, 0), c = de__tm_spawn_at(&v, 1);
    EXPECT_STREQ(de__tm_str(&v, p.name), "Player");
    EXPECT_EQ(p.prefab, de_asset_id_from("prefabs/player").hash);
    EXPECT_STREQ(de__tm_str(&v, p.group), "Level_0/Entities");
    EXPECT_FLOAT_EQ(p.u, 2.5, 0.0);
    EXPECT_FLOAT_EQ(p.v, -14.0, 0.0);
    EXPECT_FLOAT_EQ(p.w, 1.0, 0.0);
    EXPECT_FLOAT_EQ(p.h, 1.5, 0.0);
    ASSERT_EQ(p.field_count, 3u); /* the null field is left out */
    de__tm_field f0 = de__tm_field_at(&v, p.field_first);
    EXPECT_STREQ(de__tm_str(&v, f0.path), "TmHealth.current");
    EXPECT_EQ(f0.kind, DE__TM_FIELD_FLOAT);
    EXPECT_FLOAT_EQ(f0.number, 75.0, 0.0);
    de__tm_field f2 = de__tm_field_at(&v, p.field_first + 2);
    EXPECT_EQ(f2.kind, DE__TM_FIELD_STRING);
    EXPECT_STREQ(de__tm_str(&v, f2.string), "ldtk");
    EXPECT_STREQ(de__tm_str(&v, c.name), "Coin");
    EXPECT_EQ(c.prefab, 0u);
    EXPECT_FLOAT_EQ(c.u, 24.0 + 200.0 / 16.0, 1e-6);
    /* Dependencies: the texture, then the prefab. */
    ASSERT_EQ(r.dependency_count, 2u);
    EXPECT_EQ(r.dependencies[0], de_asset_id_from("tiles/terrain").hash);
    EXPECT_EQ(r.dependencies[1], de_asset_id_from("prefabs/player").hash);
    de__tm_import_free(&f->alloc, &r);
}

/* ---- tiled_tmj_tmx_equivalent ---------------------------------------------------------------- */

TEST_F(tilemap, tiled_tmj_tmx_equivalent, imp_fx) {
    tm_settings st = {k_entities, 1, NULL, 0, 0};
    de__tm_import_result a, b;
    ASSERT_EQ(tm_import_file(&f->alloc, "maps/fixture.tmj", &st, &a), DE_OK);
    ASSERT_EQ(tm_import_file(&f->alloc, "maps/fixture.tmx", &st, &b), DE_OK);
    EXPECT_EQ(a.warnings, 0u);
    EXPECT_EQ(b.warnings, 0u);
    ASSERT_EQ(a.size, b.size);
    EXPECT_MEMEQ(a.data, b.data, (size_t)a.size);

    de__tm_view v;
    ASSERT_TRUE(de__tm_view_parse(a.data, a.size, &v, NULL));
    ASSERT_EQ(v.h.layer_count, 3u);
    EXPECT_EQ(tm_view_layer(&v, "back"), 0u);
    EXPECT_EQ(tm_view_layer(&v, "ground"), 1u);
    EXPECT_EQ(tm_view_layer(&v, "deco"), 2u);
    de__tm_layer g = de__tm_layer_at(&v, 1);
    EXPECT_EQ(g.width, 48u);
    EXPECT_EQ(g.height, 40u);
    EXPECT_EQ(g.chunks_x, 2u);
    EXPECT_EQ(g.chunks_y, 2u);
    EXPECT_TRUE((g.flags & DE__TM_LAYER_COLLIDE) != 0); /* the `collision` property */
    EXPECT_TRUE((g.flags & DE__TM_LAYER_ANIMATED) != 0);
    EXPECT_EQ(v.h.chunk_count, 12u);
    /* Cells: grass at row 34, water (tile 1) at 10..15, slopes with their flips. */
    EXPECT_EQ(tm_view_cell(&v, 1, 0, 34), 1u);
    EXPECT_EQ(tm_view_cell(&v, 1, 0, 35), 5u);
    EXPECT_EQ(tm_view_cell(&v, 1, 12, 34), 2u);
    EXPECT_EQ(tm_view_cell(&v, 1, 31, 33), DE__TM_CELL_FLIP_H | 4u);
    EXPECT_EQ(tm_view_cell(&v, 1, 33, 33), DE__TM_CELL_FLIP_D | DE__TM_CELL_FLIP_V | 4u);
    EXPECT_EQ(tm_view_cell(&v, 2, 2, 33), DE__TM_CELL_FLIP_H | 6u);
    EXPECT_EQ(tm_view_cell(&v, 0, 3, 33), 6u);
    /* Tileset: texture from the image path; UVs of tile 5 (column 1, row 1). */
    ASSERT_EQ(v.h.tileset_count, 1u);
    EXPECT_EQ(de__tm_tileset_at(&v, 0).texture, de_asset_id_from("tiles/terrain").hash);
    de__tm_tile t5 = de__tm_tile_at(&v, 5);
    EXPECT_FLOAT_EQ(t5.u, 0.25, 0.0);
    EXPECT_FLOAT_EQ(t5.v, 0.5, 0.0);
    EXPECT_FLOAT_EQ(t5.du, 0.25, 0.0);
    EXPECT_FLOAT_EQ(t5.dv, 0.5, 0.0);
    EXPECT_FLOAT_EQ(t5.w, 1.0, 0.0);
    /* Animation: tile 1 -> frames 1 (200 ms), 2 (300 ms). */
    de__tm_tile t1 = de__tm_tile_at(&v, 1);
    ASSERT_EQ(t1.anim, 1u);
    de__tm_anim an = de__tm_anim_at(&v, 0);
    EXPECT_EQ(an.frame_count, 2u);
    EXPECT_EQ(an.total_ms, 500u);
    EXPECT_EQ(de__tm_frame_at(&v, an.frame_first + 1).tile, 2u);
    EXPECT_EQ(de__tm_frame_at(&v, an.frame_first + 1).duration_ms, 300u);
    /* Collision: tile 6 solid (full rectangle), tile 3 a polygon, tile 7 a 12-point polyline
     * split into chains of 8 and 5 points. */
    EXPECT_TRUE((de__tm_tile_at(&v, 6).flags & DE__TM_TILE_SOLID) != 0);
    EXPECT_EQ(de__tm_tile_at(&v, 6).shape_count, 0u);
    de__tm_tile t3 = de__tm_tile_at(&v, 3);
    ASSERT_EQ(t3.shape_count, 1u);
    de__tm_shape s3 = de__tm_shape_at(&v, t3.shape_first);
    EXPECT_EQ(s3.kind, DE__TM_SHAPE_POLYGON);
    EXPECT_EQ(s3.point_count, 3u);
    de__tm_point p0 = de__tm_point_at(&v, s3.point_first); /* (0, 16) px -> (0, 0) */
    EXPECT_FLOAT_EQ(p0.x, 0.0, 0.0);
    EXPECT_FLOAT_EQ(p0.y, 0.0, 0.0);
    de__tm_tile t7 = de__tm_tile_at(&v, 7);
    ASSERT_EQ(t7.shape_count, 2u);
    u32 total = 0;
    for (u32 i = 0; i < t7.shape_count; ++i) {
        de__tm_shape s = de__tm_shape_at(&v, t7.shape_first + i);
        EXPECT_EQ(s.kind, DE__TM_SHAPE_CHAIN);
        EXPECT_LE(s.point_count, 8u);
        total += s.point_count;
    }
    EXPECT_EQ(total, 13u); /* 12 points, the shared joint counted twice */
    /* Spawns: hero (Player, mapped) at its rectangle's center; coin not mapped. */
    ASSERT_EQ(v.h.spawn_count, 2u);
    de__tm_spawn hero = de__tm_spawn_at(&v, 0);
    EXPECT_STREQ(de__tm_str(&v, hero.name), "Player");
    EXPECT_STREQ(de__tm_str(&v, hero.group), "objects");
    EXPECT_EQ(hero.prefab, de_asset_id_from("prefabs/player").hash);
    EXPECT_FLOAT_EQ(hero.u, 3.0, 0.0);
    EXPECT_FLOAT_EQ(hero.v, -33.25, 0.0);
    ASSERT_EQ(hero.field_count, 4u);
    EXPECT_EQ(de__tm_field_at(&v, hero.field_first + 2).kind, DE__TM_FIELD_INT);
    EXPECT_FLOAT_EQ(de__tm_field_at(&v, hero.field_first + 2).number, 3.0, 0.0);
    EXPECT_EQ(de__tm_spawn_at(&v, 1).prefab, 0u);
    de__tm_import_free(&f->alloc, &a);
    de__tm_import_free(&f->alloc, &b);
}

/* ---- tiled_layer_encodings ------------------------------------------------------------------- */

TEST_F(tilemap, tiled_layer_encodings, imp_fx) {
    de__tm_import_result ref;
    ASSERT_EQ(tm_import_file(&f->alloc, "maps/enc_csv.tmj", NULL, &ref), DE_OK);
    de__tm_view v;
    ASSERT_TRUE(de__tm_view_parse(ref.data, ref.size, &v, NULL));
    EXPECT_EQ(tm_view_cell(&v, 0, 3, 0), DE__TM_CELL_FLIP_H | 7u);
    EXPECT_EQ(tm_view_cell(&v, 0, 2, 1), DE__TM_CELL_FLIP_D | 3u);
    EXPECT_EQ(tm_view_cell(&v, 0, 0, 2), DE__TM_CELL_FLIP_H | DE__TM_CELL_FLIP_V | 1u);
    static const char *const others[] = {"maps/enc_gzip.tmj", "maps/enc_plain.tmx",
                                         "maps/enc_xml.tmx", "maps/enc_zstd.tmx"};
    for (u32 i = 0; i < sizeof others / sizeof others[0]; ++i) {
        de__tm_import_result r;
        ASSERT_EQ(tm_import_file(&f->alloc, others[i], NULL, &r), DE_OK);
        EXPECT_EQ(r.warnings, 0u);
        if (r.size != ref.size || memcmp(r.data, ref.data, (size_t)r.size) != 0)
            printf("  %s differs from enc_csv.tmj\n", others[i]);
        ASSERT_EQ(r.size, ref.size);
        EXPECT_MEMEQ(r.data, ref.data, (size_t)r.size);
        de__tm_import_free(&f->alloc, &r);
    }
    de__tm_import_result bad;
    EXPECT_EQ(tm_import_file(&f->alloc, "maps/enc_lz4.tmj", NULL, &bad), DE_ERR_PARSE);
    EXPECT_TRUE(strstr(bad.message, "broken_layer") != NULL);
    EXPECT_TRUE(strstr(bad.message, "unknown compression \"lz4\"") != NULL);
    EXPECT_TRUE(bad.data == NULL);
    de__tm_import_free(&f->alloc, &ref);
}

/* ---- tilemap_cook_deterministic ---------------------------------------------------------------- */

static de_result cook(de_cooker *c, const char *id, const char *source, const char *const *ents,
                      u32 ent_count, de_cook_result *out) {
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "tilemap";
    a.source = source;
    a.file = "tmd:/assets.toml";
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    if (ent_count > 0) {
        kv.key = "entities";
        kv.value.type = DE__MANIFEST_STRING_ARRAY;
        kv.value.u.arr.items = ents;
        kv.value.u.arr.count = ent_count;
        a.settings = &kv;
        a.setting_count = 1;
    }
    de_error_clear();
    return de_cooker_cook(c, &(de_cook_input){.asset = &a, .asset_root = "tmd:/"}, out);
}

static bool has_extra(const de_cook_result *r, const char *path) {
    for (u32 i = 0; i < r->extra_source_count; ++i)
        if (strcmp(r->extra_sources[i], path) == 0)
            return true;
    return false;
}

TEST_F(tilemap, tilemap_cook_deterministic, imp_fx) {
    de_cooker *c = NULL;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &c),
              DE_OK);
    de_asset_type_desc td;
    ASSERT_TRUE(de_cooker_builtin_importer("tilemap", &td));
    EXPECT_EQ(td.importer_version, DE_TILEMAP_IMPORTER_VERSION);
    ASSERT_EQ(de_cooker_register_type(c, &td), DE_OK);
    static const char *const srcs[] = {"maps/fixture.tmj", "maps/fixture.tmx", "maps/fixture.ldtk"};
    de_cook_result first[3];
    for (u32 i = 0; i < 3; ++i) {
        de_cook_result r1, r2;
        ASSERT_EQ(cook(c, "maps/x", srcs[i], k_entities, 1, &r1), DE_OK);
        ASSERT_EQ(cook(c, "maps/x", srcs[i], k_entities, 1, &r2), DE_OK);
        ASSERT_EQ(r1.size, r2.size);
        EXPECT_MEMEQ(r1.data, r2.data, (size_t)r1.size);
        EXPECT_EQ(r1.output_hash, r2.output_hash);
        de__tm_view v;
        EXPECT_TRUE(de__tm_view_parse(r1.data, r1.size, &v, NULL));
        de_cook_result_free(&r2);
        first[i] = r1;
    }
    /* The tmj and tmx cooks are the same bytes; external files are extra sources. */
    ASSERT_EQ(first[0].size, first[1].size);
    EXPECT_MEMEQ(first[0].data, first[1].data, (size_t)first[0].size);
    EXPECT_TRUE(has_extra(&first[0], "tiles/terrain.tsj"));
    EXPECT_TRUE(has_extra(&first[1], "tiles/terrain.tsx"));
    EXPECT_TRUE(has_extra(&first[2], "maps/fixture/Level_0.ldtkl"));
    EXPECT_TRUE(has_extra(&first[2], "maps/fixture/Level_1.ldtkl"));
    /* Settings change the bytes (the mapped prefab ID). */
    de_cook_result plain;
    ASSERT_EQ(cook(c, "maps/x", srcs[0], NULL, 0, &plain), DE_OK);
    EXPECT_TRUE(plain.size != first[0].size ||
                memcmp(plain.data, first[0].data, (size_t)plain.size) != 0);
    de_cook_result_free(&plain);
    /* A failing import names the layer through the cook error. */
    de__manifest_asset bad;
    memset(&bad, 0, sizeof bad);
    bad.id = "maps/bad";
    bad.type = "tilemap";
    bad.source = "maps/enc_lz4.tmj";
    bad.file = "tmd:/assets.toml";
    de_cook_result br;
    EXPECT_NE(de_cooker_cook(c, &(de_cook_input){.asset = &bad, .asset_root = "tmd:/"}, &br),
              DE_OK);
    EXPECT_TRUE(strstr(de_error_last()->message, "broken_layer") != NULL);
    de_error_clear();
    for (u32 i = 0; i < 3; ++i)
        de_cook_result_free(&first[i]);
    de_cooker_destroy(c);
}

/* ---- cook_check_accepts_tilemap --------------------------------------------------------------- */

typedef struct check_out {
    char text[8192];
    size_t len;
} check_out;
static void check_write(void *user, de_cook_stream s, const char *text, size_t len) {
    (void)s;
    check_out *o = (check_out *)user;
    size_t k = len < sizeof o->text - 1 - o->len ? len : sizeof o->text - 1 - o->len;
    memcpy(o->text + o->len, text, k);
    o->len += k;
    o->text[o->len] = '\0';
}

TEST_F(tilemap, cook_check_accepts_tilemap, imp_fx) {
    (void)f;
    static check_out out;
    memset(&out, 0, sizeof out);
    de_cook_io io = {check_write, &out};
    const char *args[] = {DE_TILEMAP_TEST_DATA_DIR "/assets.toml"};
    int rc = de_cook_check_main(1, args, &io);
    if (rc != DE_COOK_EXIT_OK)
        printf("%s\n", out.text);
    EXPECT_EQ(rc, DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(out.text, "0 errors") != NULL);
    /* `tilemap` is a known engine type with a built-in importer. */
    EXPECT_TRUE(de_cooker_is_engine_type("tilemap"));
}

/* ---- cooked_view_rejects ---------------------------------------------------------------------- */

TEST_F(tilemap, cooked_view_rejects, imp_fx) {
    de__tm_import_result r;
    ASSERT_EQ(tm_import_file(&f->alloc, "maps/fixture.tmj", NULL, &r), DE_OK);
    de__tm_view v;
    const char *why = NULL;
    ASSERT_TRUE(de__tm_view_parse(r.data, r.size, &v, &why));
    /* Every truncation fails. */
    for (u64 n = 0; n < r.size; n += 97)
        EXPECT_FALSE(de__tm_view_parse(r.data, n, &v, &why));
    EXPECT_FALSE(de__tm_view_parse(r.data, r.size - 1, &v, &why));
    u8 *copy = (u8 *)malloc((size_t)r.size);
    ASSERT_TRUE(copy != NULL);
    /* A cell naming a tile out of range. */
    memcpy(copy, r.data, (size_t)r.size);
    ASSERT_TRUE(de__tm_view_parse(copy, r.size, &v, &why));
    size_t cell0 = (size_t)(v.cells - copy);
    de__tm_wr32(copy + cell0, 9999u);
    EXPECT_FALSE(de__tm_view_parse(copy, r.size, &v, &why));
    /* A bad magic, a bad version. */
    memcpy(copy, r.data, (size_t)r.size);
    copy[0] ^= 1;
    EXPECT_FALSE(de__tm_view_parse(copy, r.size, &v, &why));
    memcpy(copy, r.data, (size_t)r.size);
    copy[4] = 9;
    EXPECT_FALSE(de__tm_view_parse(copy, r.size, &v, &why));
    free(copy);
    de__tm_import_free(&f->alloc, &r);
}
