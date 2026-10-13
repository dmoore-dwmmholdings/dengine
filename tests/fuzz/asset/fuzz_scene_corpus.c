/*
 * fuzz_scene_corpus.c: the DSCN seed corpus of fuzz_scene_dscn (DENG-403), corpus/scene_dscn/.
 *
 * Each DSCN seed is the cook of a TOML seed of corpus/scene_toml/ (fuzz_scene_cook.c), so the
 * fuzzer starts from bytes that pass every check of de__scene_view_parse and instantiate, and
 * mutates from there. migrate_v1.dscn is cooked where THealth is version 1 ({ hp }), so
 * instantiating it runs the stored-schema path and the migrate hook.
 *
 * CTest scene_corpus.seeds cooks each TOML seed again and compares the bytes with its DSCN seed,
 * then checks that the seed parses, has the feature it is there for, and instantiates. A format or
 * importer change that turns the seeds into bytes the parser stops at fails here, not silently in
 * the fuzzer. To write the seeds again: DE_SCENE_CORPUS_UPDATE=1 ctest -R scene_corpus.seeds.
 * The test skips where de_asset has no cooker (Release with tools).
 */
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dengine/de_base.h>

#include "asset/scene/scene_format.h"
#include "de_test.h"
#include "fuzz_scene_harness.h"

#if DE_HAS_COOKER
typedef struct sc_seed {
    const char *toml; /* in corpus/scene_toml/ */
    bool prefab;
    fz_scene_reg reg;
    const char *dscn; /* in corpus/scene_dscn/ */
    const char *type; /* a type the seed must hold, at `version` */
    u32 version;
    u32 entities;
} sc_seed;

static const sc_seed k_seeds[] = {
    {"level.scene.toml", false, FZ_SCENE_REG_CURRENT, "level.dscn", "TTarget", 1, 3},
    {"unknown.scene.toml", false, FZ_SCENE_REG_CURRENT, "unknown.dscn", "THealth", 2, 2},
    {"crate.prefab.toml", true, FZ_SCENE_REG_CURRENT, "crate.dscn", "THealth", 2, 2},
    {"misc.scene.toml", false, FZ_SCENE_REG_CURRENT, "misc.dscn", "TMisc", 1, 3},
    {"misc.scene.toml", false, FZ_SCENE_REG_CURRENT, "misc.dscn", "TMarker", 1, 3},
    {"misc.scene.toml", false, FZ_SCENE_REG_CURRENT, "misc.dscn", "DeTransform", 1, 3},
    {"hierarchy.prefab.toml", true, FZ_SCENE_REG_CURRENT, "hierarchy.dscn", "TTarget", 1, 6},
    {"versioned.scene.toml", false, FZ_SCENE_REG_CURRENT, "versioned.dscn", "THealth", 2, 2},
    {"empty.scene.toml", false, FZ_SCENE_REG_CURRENT, "empty.dscn", NULL, 0, 0},
    {"migrate.scene.toml", false, FZ_SCENE_REG_HEALTH_V1, "migrate_v1.dscn", "THealth", 1, 1},
};

/* ---- Hand-built seeds -------------------------------------------------------------------------
 */

/* A component of a hand-built seed: a type of fz_scene_types() and an object of it, whose entity
 * fields hold scene IDs of the file (written raw, as the importer writes them). */
typedef struct sc_comp {
    u32 type_index;
    const void *obj;
} sc_comp;

typedef struct sc_entity {
    u64 id;
    u32 parent; /* index, or DE__SCENE_NO_PARENT */
    u32 comp_count;
    sc_comp comps[2];
} sc_entity;

/* Assembles a DSCN (format of scene_format.h) from sorted types and sorted entities. */
static void sc_build(de__scene_kind kind, const char *const *types, u32 type_count,
                     const sc_entity *ents, u32 ent_count, fz_scene_cooked *out) {
    const de_type_registry *reg = fz_scene_types();
    de_mem_writer data;
    de_mem_writer_init(&data, NULL);
    u32 comp_count = 0, roots = 0, string_bytes = 0;
    for (u32 i = 0; i < ent_count; ++i) {
        comp_count += ents[i].comp_count;
        roots += ents[i].parent == DE__SCENE_NO_PARENT ? 1u : 0u;
    }
    for (u32 t = 0; t < type_count; ++t)
        string_bytes += (u32)strlen(types[t]) + 1u;
    u64 head = DE__SCENE_HEADER_SIZE + (u64)type_count * DE__SCENE_TYPE_SIZE +
               (u64)ent_count * DE__SCENE_ENTITY_SIZE + (u64)comp_count * DE__SCENE_COMP_SIZE +
               string_bytes;
    u8 *b = (u8 *)calloc(1, (size_t)head);
    ASSERT_TRUE(b != NULL);
    u8 *p = b + DE__SCENE_HEADER_SIZE;
    u32 str_off = 0;
    for (u32 t = 0; t < type_count; ++t, p += DE__SCENE_TYPE_SIZE) {
        de_type_id id = de_type_find(reg, types[t]);
        const de_type_desc *td = de_type_get(reg, id);
        ASSERT_TRUE(td != NULL);
        de__scene_put_u32(p, str_off);
        de__scene_put_u32(p + 4, (u32)strlen(types[t]));
        de__scene_put_u32(p + 8, td->version);
        de__scene_put_u64(p + 16, de_type_schema_hash(reg, id));
        str_off += (u32)strlen(types[t]) + 1u;
    }
    u32 first = 0;
    for (u32 i = 0; i < ent_count; ++i, p += DE__SCENE_ENTITY_SIZE) {
        de__scene_put_u64(p, ents[i].id);
        de__scene_put_u32(p + 8, ents[i].parent);
        de__scene_put_u32(p + 12, first);
        de__scene_put_u32(p + 16, ents[i].comp_count);
        first += ents[i].comp_count;
    }
    for (u32 i = 0; i < ent_count; ++i) {
        for (u32 k = 0; k < ents[i].comp_count; ++k, p += DE__SCENE_COMP_SIZE) {
            const sc_comp *c = &ents[i].comps[k];
            de_type_id id = de_type_find(reg, types[c->type_index]);
            size_t before = data.size;
            de_ser_options opt = {DE_SER_BINARY, DE_FIELD_SERIALIZE, NULL};
            de_ser_binary_options bin = {DE_SER_BINARY_SELF_DESCRIBING, NULL, false};
            de_writer w = de_writer_from_mem(&data);
            ASSERT_EQ(de_serialize_binary(reg, id, c->obj, &opt, &bin, &w), DE_OK);
            de__scene_put_u32(p, c->type_index);
            de__scene_put_u32(p + 4, (u32)(data.size - before));
            de__scene_put_u64(p + 8, (u64)before);
        }
    }
    for (u32 t = 0; t < type_count; ++t) {
        size_t n = strlen(types[t]) + 1u;
        memcpy(p, types[t], n);
        p += n;
    }
    ASSERT_EQ((u64)(p - b), head);
    memcpy(b, DE__SCENE_MAGIC, 4);
    de__scene_put_u32(b + 4, DE__SCENE_VERSION);
    de__scene_put_u32(b + 8, (u32)kind);
    de__scene_put_u32(b + 12, type_count);
    de__scene_put_u32(b + 16, ent_count);
    de__scene_put_u32(b + 20, comp_count);
    de__scene_put_u32(b + 24, 0);
    de__scene_put_u32(b + 28, string_bytes);
    de__scene_put_u64(b + 32, (u64)data.size);
    de__scene_put_u32(b + 40, roots);
    memset(out, 0, sizeof *out);
    out->size = head + data.size;
    out->data = (u8 *)malloc((size_t)out->size);
    out->deps = (u64 *)malloc(sizeof(u64));
    ASSERT_TRUE(out->data != NULL && out->deps != NULL);
    memcpy(out->data, b, (size_t)head);
    if (data.size != 0)
        memcpy(out->data + head, data.data, data.size);
    free(b);
    de_mem_writer_free(&data);
}

/* builtins_hier.dscn: the hierarchy components DeChildOf and DeChildren stored in a scene (the
 * importer never writes them; a hostile or foreign file can), with links that disagree with the
 * `parent` records: 0x2 is a child of 0x1 by its record, but its DeChildOf names 0x3. */
static void sc_builtins_hier(fz_scene_cooked *out) {
    static const char *const types[] = {"DeChildOf", "DeChildren"};
    static const DeChildOf of2 = {{0x3}, {0x1}, {0x2}}, of3 = {{0x2}, {0x3}, {0}};
    static const DeChildren ch1 = {{0x3}, {0x3}, 5}, ch3 = {{0x1}, {0x2}, 2};
    sc_entity ents[3];
    memset(ents, 0, sizeof ents);
    ents[0].id = 0x1;
    ents[0].parent = DE__SCENE_NO_PARENT;
    ents[0].comp_count = 1;
    ents[0].comps[0] = (sc_comp){1, &ch1};
    ents[1].id = 0x2;
    ents[1].parent = 0;
    ents[1].comp_count = 1;
    ents[1].comps[0] = (sc_comp){0, &of2};
    ents[2].id = 0x3;
    ents[2].parent = DE__SCENE_NO_PARENT;
    ents[2].comp_count = 2;
    ents[2].comps[0] = (sc_comp){0, &of3};
    ents[2].comps[1] = (sc_comp){1, &ch3};
    sc_build(DE__SCENE_KIND_SCENE, types, 2, ents, 3, out);
}

typedef struct sc_built {
    const char *dscn;
    void (*build)(fz_scene_cooked *out);
    u32 entities;
} sc_built;

static const sc_built k_built[] = {
    {"builtins_hier.dscn", sc_builtins_hier, 3},
};

/* Reads a whole file; NULL if it does not exist. `*size` gets the size. */
static u8 *sc_read(const char *path, size_t *size) {
    *size = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    size_t cap = 4096, n = 0;
    u8 *buf = (u8 *)malloc(cap);
    ASSERT_TRUE(buf != NULL);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            u8 *grown = (u8 *)realloc(buf, cap);
            ASSERT_TRUE(grown != NULL);
            buf = grown;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    fclose(f);
    *size = n;
    return buf;
}

/* Compares `c` with the seed file `dscn` (writes it first when `write`), then checks that the seed
 * parses as `kind` with `entities` entities, holds `type` at `version` (unless NULL), keeps the
 * parse's promises and instantiates. Frees `c`. */
static void sc_check_seed(const char *dscn, fz_scene_cooked *c, bool write, const char *from,
                          de__scene_kind kind, u32 entities, const char *type, u32 version) {
    char path[1024];
    snprintf(path, sizeof path, "%s/scene_dscn/%s", DE_SCENE_CORPUS_DIR, dscn);
    if (write) {
        FILE *f = fopen(path, "wb");
        ASSERT_TRUE(f != NULL);
        ASSERT_EQ(fwrite(c->data, 1, (size_t)c->size, f), (size_t)c->size);
        fclose(f);
    }
    size_t size = 0;
    u8 *seed = sc_read(path, &size);
    if (seed == NULL)
        printf("  missing seed %s (DE_SCENE_CORPUS_UPDATE=1 writes it)\n", path);
    ASSERT_TRUE(seed != NULL);
    if (size != c->size || memcmp(seed, c->data, size) != 0)
        printf("  %s is not what %s gives now (DE_SCENE_CORPUS_UPDATE=1 writes it again)\n", dscn,
               from);
    ASSERT_EQ(size, (size_t)c->size);
    EXPECT_MEMEQ(seed, c->data, size);
    fz_scene_cooked_free(c);

    de__scene_view v;
    char err[256] = "";
    de_result pr = de__scene_view_parse(seed, size, &v, err, sizeof err);
    if (pr != DE_OK)
        printf("  %s: %s\n", dscn, err);
    ASSERT_EQ(pr, DE_OK);
    EXPECT_EQ(v.kind, kind);
    EXPECT_EQ(v.entity_count, entities);
    if (type != NULL) {
        bool found = false;
        for (u32 t = 0; t < v.type_count; ++t) {
            de__scene_type_rec tr = de__scene_type_at(&v, t);
            if (strcmp(tr.name, type) == 0) {
                found = true;
                EXPECT_EQ(tr.version, version);
            }
        }
        if (!found)
            printf("  %s has no type %s\n", dscn, type);
        EXPECT_TRUE(found);
    }
    fz_scene_check_view("scene_corpus", &v);
    EXPECT_EQ(fz_scene_instantiate("scene_corpus", seed, size), DE_OK);
    free(seed);
}

static bool sc_update(void) {
    const char *update = getenv("DE_SCENE_CORPUS_UPDATE");
    return update != NULL && update[0] == '1';
}
#endif

#define NO_COOKER_MSG                                                                              \
    "this build does not link the cooker (DE_HAS_COOKER: Debug and Dev with tools)"

/* The cooked seeds: the cook of each TOML seed is its DSCN seed. */
TEST(scene_corpus, seeds) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    bool write = sc_update();
    for (size_t i = 0; i < sizeof k_seeds / sizeof k_seeds[0]; ++i) {
        const sc_seed *sd = &k_seeds[i];
        char path[1024];
        snprintf(path, sizeof path, "%s/scene_toml/%s", DE_SCENE_CORPUS_DIR, sd->toml);
        size_t toml_size = 0;
        u8 *toml = sc_read(path, &toml_size);
        if (toml == NULL)
            printf("  missing TOML seed %s\n", path);
        ASSERT_TRUE(toml != NULL);
        fz_scene_cooked c;
        char msg[256];
        de_result r = fz_scene_cook(toml, toml_size, sd->prefab, sd->reg, &c, msg, sizeof msg);
        free(toml);
        if (r != DE_OK)
            printf("  %s: %s\n", sd->toml, msg);
        ASSERT_EQ(r, DE_OK);
        sc_check_seed(sd->dscn, &c, write, sd->toml,
                      sd->prefab ? DE__SCENE_KIND_PREFAB : DE__SCENE_KIND_SCENE, sd->entities,
                      sd->type, sd->version);
    }
#endif
}

/* The hand-built seeds: what the importer never writes, but a damaged or foreign file can hold. */
TEST(scene_corpus, built) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    bool write = sc_update();
    for (size_t i = 0; i < sizeof k_built / sizeof k_built[0]; ++i) {
        fz_scene_cooked c;
        k_built[i].build(&c);
        sc_check_seed(k_built[i].dscn, &c, write, "its builder", DE__SCENE_KIND_SCENE,
                      k_built[i].entities, NULL, 0);
    }
#endif
}
