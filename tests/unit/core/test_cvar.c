/* Tests of the cvar registry (DENG-154, SPEC §5.8): types, flags, command line, config.toml.
 *
 * Built twice: in test_core (the build's own flags) and in test_core_cvar_shipping, which
 * compiles cvar.c and cvar_toml.c with DE_SHIPPING=1, so both sides of DEV_ONLY and of the cheat
 * gate run on every build. Each test starts with a registry reset, so the tests do not depend on
 * each other even when one process runs them all.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#endif

#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "de_test.h"

/* Test hooks in src/core/cvar.c. */
void de__cvar_test_reset(void);
void de__cvar_test_lock(void);
void de__cvar_test_unlock(void);

#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define CHEATS_ALWAYS 1
#else
#define CHEATS_ALWAYS 0
#endif
#if defined(DE_SHIPPING)
#define SHIPPING 1
#else
#define SHIPPING 0
#endif

#ifndef DE_CVAR_TEST_DATA_DIR
#error "DE_CVAR_TEST_DATA_DIR must be defined by tests/unit/core/CMakeLists.txt"
#endif
#ifndef DE_CVAR_TEST_OUT_PREFIX
#error "DE_CVAR_TEST_OUT_PREFIX must be defined by tests/unit/core/CMakeLists.txt"
#endif

static void reset(void) {
    de__cvar_test_reset();
    de_error_clear();
}

static bool error_mentions(const char *needle) {
    const de_error *e = de_error_last();
    return e != NULL && strstr(e->message, needle) != NULL;
}

/* Reads a whole file into a malloc'ed, NUL-terminated buffer. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    for (;;) {
        if (buf == NULL)
            break;
        if (cap - len < 2) {
            char *grown = (char *)realloc(buf, cap * 2);
            if (grown == NULL) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0)
            break;
    }
    fclose(f);
    if (buf != NULL) {
        buf[len] = '\0';
        if (out_len != NULL)
            *out_len = len;
    }
    return buf;
}

static de_cvar reg(const char *name, de_cvar_type type, u32 flags) {
    return de_cvar_register(&(de_cvar_desc){.name = name, .type = type, .flags = flags});
}

/* ---- types ----------------------------------------------------------------------------- */

TEST(cvar, types_roundtrip) {
    reset();
    static const char *const modes[] = {"off", "fast", "fancy"};
    de_cvar b = de_cvar_register(&(de_cvar_desc){.name = "t.bool", .default_number = 1});
    de_cvar i = de_cvar_register(
        &(de_cvar_desc){.name = "t.int", .type = DE_CVAR_INT, .default_number = -7});
    de_cvar f = de_cvar_register(
        &(de_cvar_desc){.name = "t.float", .type = DE_CVAR_FLOAT, .default_number = 0.25});
    de_cvar s = de_cvar_register(
        &(de_cvar_desc){.name = "t.string", .type = DE_CVAR_STRING, .default_string = "hello"});
    de_cvar e = de_cvar_register(&(de_cvar_desc){.name = "t.enum",
                                                 .type = DE_CVAR_ENUM,
                                                 .enum_values = modes,
                                                 .enum_count = 3,
                                                 .default_string = "fast"});
    de_cvar empty = de_cvar_register(&(de_cvar_desc){.name = "t.empty", .type = DE_CVAR_STRING});
    ASSERT_TRUE(DE_HANDLE_VALID(b) && DE_HANDLE_VALID(i) && DE_HANDLE_VALID(f));
    ASSERT_TRUE(DE_HANDLE_VALID(s) && DE_HANDLE_VALID(e) && DE_HANDLE_VALID(empty));

    /* Defaults. */
    EXPECT_TRUE(de_cvar_bool(b));
    EXPECT_EQ(de_cvar_int(i), -7);
    EXPECT_FLOAT_EQ(de_cvar_float(f), 0.25, 0.0);
    EXPECT_STREQ(de_cvar_string(s), "hello");
    EXPECT_STREQ(de_cvar_string(empty), "");
    EXPECT_EQ(de_cvar_int(e), 1);
    EXPECT_STREQ(de_cvar_string(e), "fast");

    /* Console text, per type. */
    static const char *const bool_true[] = {"1", "true", "TRUE", "on", "Yes", " on "};
    static const char *const bool_false[] = {"0", "false", "Off", "no", "\tNO\n"};
    for (size_t k = 0; k < sizeof bool_true / sizeof bool_true[0]; ++k) {
        ASSERT_EQ(de_cvar_set(b, "0"), DE_OK);
        ASSERT_EQ(de_cvar_set(b, bool_true[k]), DE_OK);
        EXPECT_TRUE(de_cvar_bool(b));
    }
    for (size_t k = 0; k < sizeof bool_false / sizeof bool_false[0]; ++k) {
        ASSERT_EQ(de_cvar_set(b, "1"), DE_OK);
        ASSERT_EQ(de_cvar_set(b, bool_false[k]), DE_OK);
        EXPECT_FALSE(de_cvar_bool(b));
    }
    EXPECT_EQ(de_cvar_set(b, "2"), DE_ERR_PARSE);
    EXPECT_TRUE(error_mentions("t.bool"));

    EXPECT_EQ(de_cvar_set(i, "123"), DE_OK);
    EXPECT_EQ(de_cvar_int(i), 123);
    EXPECT_EQ(de_cvar_set(i, " -0x10 "), DE_OK);
    EXPECT_EQ(de_cvar_int(i), -16);
    EXPECT_EQ(de_cvar_set(i, "9223372036854775807"), DE_OK);
    EXPECT_EQ(de_cvar_int(i), INT64_MAX);
    EXPECT_EQ(de_cvar_set(i, "1.5"), DE_ERR_PARSE);
    EXPECT_EQ(de_cvar_set(i, "99999999999999999999"), DE_ERR_PARSE);
    EXPECT_EQ(de_cvar_int(i), INT64_MAX); /* unchanged after a failure */

    EXPECT_EQ(de_cvar_set(f, "0.1"), DE_OK);
    EXPECT_TRUE(de_cvar_float(f) == 0.1);
    EXPECT_EQ(de_cvar_set(f, "-2.5e3"), DE_OK);
    EXPECT_TRUE(de_cvar_float(f) == -2500.0);
    EXPECT_EQ(de_cvar_int(f), -2500);
    EXPECT_EQ(de_cvar_set(f, "abc"), DE_ERR_PARSE);
    EXPECT_EQ(de_cvar_set(f, "nan"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(de_cvar_float(f) == -2500.0);

    EXPECT_EQ(de_cvar_set(s, "  spaced value "), DE_OK);
    EXPECT_STREQ(de_cvar_string(s), "  spaced value "); /* strings are not trimmed */
    const char *old = de_cvar_string(s);
    EXPECT_EQ(de_cvar_set(s, "next"), DE_OK);
    EXPECT_STREQ(de_cvar_string(s), "next");
    EXPECT_STREQ(old, "  spaced value "); /* earlier pointers stay valid */
    EXPECT_EQ(de_cvar_set(s, "  spaced value "), DE_OK);
    EXPECT_EQ(de_cvar_string(s), old); /* the same text reuses its copy */

    EXPECT_EQ(de_cvar_set(e, "fancy"), DE_OK);
    EXPECT_EQ(de_cvar_int(e), 2);
    EXPECT_STREQ(de_cvar_string(e), "fancy");

    /* Cross-type getters. */
    EXPECT_EQ(de_cvar_int(b), 0);
    EXPECT_FLOAT_EQ(de_cvar_float(e), 2.0, 0.0);
    EXPECT_TRUE(de_cvar_string(i) == NULL);
    EXPECT_EQ(de_cvar_int(s), 0);
    EXPECT_FALSE(de_cvar_bool(s));

    /* Command line: every type through `+name value`. */
    char *argv[] = {"game",     "+t.bool", "yes",       "+t.int",    "0b101",   "-unrelated",
                    "+t.float", "1e-3",    "+t.string", "from args", "+t.enum", "0"};
    ASSERT_EQ(de_cvar_apply_args((int)(sizeof argv / sizeof argv[0]), argv), DE_OK);
    EXPECT_TRUE(de_cvar_bool(b));
    EXPECT_EQ(de_cvar_int(i), 5);
    EXPECT_TRUE(de_cvar_float(f) == 1e-3);
    EXPECT_STREQ(de_cvar_string(s), "from args");
    EXPECT_STREQ(de_cvar_string(e), "off");

    /* Queries. */
    EXPECT_STREQ(de_cvar_name(f), "t.float");
    EXPECT_STREQ(de_cvar_help(f), "");
    EXPECT_EQ(de_cvar_get_type(e), DE_CVAR_ENUM);
    EXPECT_EQ(de_cvar_get_flags(e), 0u);
    EXPECT_EQ(de_cvar_find("t.enum").id, e.id);
    EXPECT_EQ(de_cvar_find("t.nope").id, 0u);
    EXPECT_EQ(de_cvar_find(NULL).id, 0u);
}

TEST(cvar, handles_and_names) {
    reset();
    de_cvar a = reg("a.b", DE_CVAR_INT, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(a));

    /* Invalid names. */
    static const char *const bad[] = {
        "", "A.b", "a..b", ".a", "a.", "a-b", "a b", "a/b", "p1.p2.p3.p4.p5.p6.p7.p8.p9.p10.p11"};
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; ++k) {
        de_error_clear();
        EXPECT_EQ(reg(bad[k], DE_CVAR_INT, 0).id, 0u);
        ASSERT_TRUE(de_error_last() != NULL);
        EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    }
    char long_name[DE_CVAR_NAME_MAX + 2];
    memset(long_name, 'x', sizeof long_name);
    long_name[DE_CVAR_NAME_MAX + 1] = '\0';
    EXPECT_EQ(reg(long_name, DE_CVAR_INT, 0).id, 0u);
    long_name[DE_CVAR_NAME_MAX] = '\0';
    EXPECT_TRUE(DE_HANDLE_VALID(reg(long_name, DE_CVAR_INT, 0)));
    de_cvar ten = reg("p1.p2.p3.p4.p5.p6.p7.p8.p9.p10", DE_CVAR_INT, 0);
    EXPECT_TRUE(DE_HANDLE_VALID(ten));
    static const char ten_toml[] = "p1.p2.p3.p4.p5.p6.p7.p8.p9.p10 = 5\n"; /* the parser's limit */
    EXPECT_EQ(de_cvar_load_from_memory(ten_toml, sizeof ten_toml - 1), DE_OK);
    EXPECT_EQ(de_cvar_int(ten), 5);
    EXPECT_EQ(de_cvar_register(NULL).id, 0u);
    EXPECT_EQ(de_cvar_register(&(de_cvar_desc){.name = "bad.type", .type = (de_cvar_type)99}).id,
              0u);

    /* A name cannot be both a value and a table. */
    de_error_clear();
    EXPECT_EQ(reg("a", DE_CVAR_INT, 0).id, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(reg("a.b.c", DE_CVAR_INT, 0).id, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(DE_HANDLE_VALID(reg("a.c", DE_CVAR_INT, 0)));

    /* Registering again: same type gives the same handle, another type fails. */
    EXPECT_EQ(reg("a.b", DE_CVAR_INT, 0).id, a.id);
    de_error_clear();
    EXPECT_EQ(reg("a.b", DE_CVAR_FLOAT, 0).id, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_ALREADY_EXISTS);

    /* Invalid and stale handles. */
    ASSERT_EQ(de_cvar_set(a, "5"), DE_OK);
    de_cvar invalid = {0};
    EXPECT_EQ(de_cvar_int(invalid), 0);
    EXPECT_TRUE(de_cvar_name(invalid) == NULL);
    EXPECT_EQ(de_cvar_set(invalid, "1"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_cvar_set(a, NULL), DE_ERR_INVALID_ARGUMENT);
    de_cvar beyond = {a.id + 1000u};
    EXPECT_EQ(de_cvar_int(beyond), 0);
    reset();
    EXPECT_EQ(de_cvar_int(a), 0); /* the generation changed */
    EXPECT_EQ(de_cvar_set(a, "1"), DE_ERR_INVALID_ARGUMENT);
    de_cvar again = reg("a.b", DE_CVAR_INT, 0);
    EXPECT_TRUE(DE_HANDLE_VALID(again));
    EXPECT_NE(again.id, a.id);

    /* The built-in cheat gate exists after every reset. */
    de_cvar cheats = de_cvar_find("cheats");
    ASSERT_TRUE(DE_HANDLE_VALID(cheats));
    EXPECT_EQ(de_cvar_get_type(cheats), DE_CVAR_BOOL);
    EXPECT_FALSE(de_cvar_bool(cheats));
    EXPECT_EQ(de_cvar_get_flags(cheats), (u32)DE_CVAR_REPLICATED);
}

/* ---- clamp and enum -------------------------------------------------------------------- */

TEST(cvar, clamp) {
    reset();
    de_cvar i = de_cvar_register(&(de_cvar_desc){
        .name = "c.int", .type = DE_CVAR_INT, .default_number = 100, .min = 0, .max = 10});
    de_cvar f = de_cvar_register(&(de_cvar_desc){
        .name = "c.float", .type = DE_CVAR_FLOAT, .default_number = -1, .min = 0.5, .max = 2});
    de_cvar frac = de_cvar_register(
        &(de_cvar_desc){.name = "c.frac", .type = DE_CVAR_INT, .min = 0.5, .max = 2.5});
    de_cvar neg = de_cvar_register(
        &(de_cvar_desc){.name = "c.neg", .type = DE_CVAR_INT, .min = -2.5, .max = -0.5});
    de_cvar open = de_cvar_register(&(de_cvar_desc){.name = "c.open", .type = DE_CVAR_FLOAT});
    ASSERT_TRUE(DE_HANDLE_VALID(i) && DE_HANDLE_VALID(f) && DE_HANDLE_VALID(frac));
    ASSERT_TRUE(DE_HANDLE_VALID(neg) && DE_HANDLE_VALID(open));

    EXPECT_EQ(de_cvar_int(i), 10); /* defaults clamp too */
    EXPECT_FLOAT_EQ(de_cvar_float(f), 0.5, 0.0);
    EXPECT_EQ(de_cvar_int(frac), 1); /* ceil(0.5) */

    EXPECT_EQ(de_cvar_set(i, "50"), DE_OK);
    EXPECT_EQ(de_cvar_int(i), 10);
    EXPECT_EQ(de_cvar_set(i, "-5"), DE_OK);
    EXPECT_EQ(de_cvar_int(i), 0);
    EXPECT_EQ(de_cvar_set(i, "7"), DE_OK);
    EXPECT_EQ(de_cvar_int(i), 7);
    EXPECT_EQ(de_cvar_set(i, "-9223372036854775808"), DE_OK);
    EXPECT_EQ(de_cvar_int(i), 0);

    EXPECT_EQ(de_cvar_set(f, "3"), DE_OK);
    EXPECT_FLOAT_EQ(de_cvar_float(f), 2.0, 0.0);
    EXPECT_EQ(de_cvar_set(f, "0"), DE_OK);
    EXPECT_FLOAT_EQ(de_cvar_float(f), 0.5, 0.0);
    EXPECT_EQ(de_cvar_set(f, "inf"), DE_OK);
    EXPECT_FLOAT_EQ(de_cvar_float(f), 2.0, 0.0);
    EXPECT_EQ(de_cvar_set(f, "-infinity"), DE_OK);
    EXPECT_FLOAT_EQ(de_cvar_float(f), 0.5, 0.0);
    EXPECT_EQ(de_cvar_set(f, "1.25"), DE_OK);
    EXPECT_FLOAT_EQ(de_cvar_float(f), 1.25, 0.0);

    EXPECT_EQ(de_cvar_set(frac, "3"), DE_OK);
    EXPECT_EQ(de_cvar_int(frac), 2); /* floor(2.5) */
    EXPECT_EQ(de_cvar_set(neg, "0"), DE_OK);
    EXPECT_EQ(de_cvar_int(neg), -1); /* floor(-0.5) */
    EXPECT_EQ(de_cvar_set(neg, "-10"), DE_OK);
    EXPECT_EQ(de_cvar_int(neg), -2); /* ceil(-2.5) */

    /* min == max (a zeroed desc): no range. */
    EXPECT_EQ(de_cvar_set(open, "1e300"), DE_OK);
    EXPECT_TRUE(de_cvar_float(open) == 1e300);

    /* Startup sources clamp the same way. */
    char *argv[] = {"game", "+c.int", "11"};
    EXPECT_EQ(de_cvar_apply_args(3, argv), DE_OK);
    EXPECT_EQ(de_cvar_int(i), 10);
    static const char toml[] = "[c]\nint = -3\nfloat = 9.5\n";
    EXPECT_EQ(de_cvar_load_from_memory(toml, sizeof toml - 1), DE_OK);
    EXPECT_EQ(de_cvar_int(i), 0);
    EXPECT_FLOAT_EQ(de_cvar_float(f), 2.0, 0.0);
}

TEST(cvar, enum_parse) {
    reset();
    static const char *const q[] = {"low", "medium", "high"};
    de_cvar e = de_cvar_register(&(de_cvar_desc){
        .name = "r.quality", .type = DE_CVAR_ENUM, .enum_values = q, .enum_count = 3});
    ASSERT_TRUE(DE_HANDLE_VALID(e));
    EXPECT_STREQ(de_cvar_string(e), "low"); /* default index 0 */

    EXPECT_EQ(de_cvar_set(e, "high"), DE_OK);
    EXPECT_EQ(de_cvar_int(e), 2);
    EXPECT_EQ(de_cvar_set(e, "1"), DE_OK);
    EXPECT_STREQ(de_cvar_string(e), "medium");
    EXPECT_EQ(de_cvar_set(e, " low\n"), DE_OK);
    EXPECT_EQ(de_cvar_int(e), 0);

    de_error_clear();
    EXPECT_EQ(de_cvar_set(e, "ultra"), DE_ERR_PARSE);
    EXPECT_TRUE(error_mentions("r.quality"));
    EXPECT_TRUE(error_mentions("\"ultra\""));
    EXPECT_TRUE(error_mentions("low, medium, high"));
    EXPECT_EQ(de_cvar_set(e, "HIGH"), DE_ERR_PARSE); /* names are exact */
    EXPECT_EQ(de_cvar_set(e, "3"), DE_ERR_PARSE);
    EXPECT_EQ(de_cvar_set(e, "-1"), DE_ERR_PARSE);
    EXPECT_EQ(de_cvar_set(e, ""), DE_ERR_PARSE);
    EXPECT_EQ(de_cvar_int(e), 0);

    /* From config.toml, as a name or an index. */
    static const char by_name[] = "r.quality = \"high\"\n";
    EXPECT_EQ(de_cvar_load_from_memory(by_name, sizeof by_name - 1), DE_OK);
    EXPECT_EQ(de_cvar_int(e), 2);
    static const char by_index[] = "[r]\nquality = 1\n";
    EXPECT_EQ(de_cvar_load_from_memory(by_index, sizeof by_index - 1), DE_OK);
    EXPECT_EQ(de_cvar_int(e), 1);
    static const char bad[] = "[r]\nquality = \"epic\"\n";
    de_error_clear();
    EXPECT_EQ(de_cvar_load_from_memory(bad, sizeof bad - 1), DE_ERR_PARSE);
    EXPECT_TRUE(error_mentions("r.quality"));

    /* Bad enum descs. */
    static const char *const with_null[] = {"a", NULL};
    EXPECT_EQ(de_cvar_register(&(de_cvar_desc){.name = "e.none", .type = DE_CVAR_ENUM}).id, 0u);
    EXPECT_EQ(de_cvar_register(&(de_cvar_desc){.name = "e.null",
                                               .type = DE_CVAR_ENUM,
                                               .enum_values = with_null,
                                               .enum_count = 2})
                  .id,
              0u);
    EXPECT_EQ(de_cvar_register(&(de_cvar_desc){.name = "e.def",
                                               .type = DE_CVAR_ENUM,
                                               .enum_values = q,
                                               .enum_count = 3,
                                               .default_string = "nope"})
                  .id,
              0u);
    EXPECT_EQ(de_cvar_register(&(de_cvar_desc){.name = "e.idx",
                                               .type = DE_CVAR_ENUM,
                                               .enum_values = q,
                                               .enum_count = 3,
                                               .default_number = 3})
                  .id,
              0u);
    de_cvar by_idx = de_cvar_register(&(de_cvar_desc){.name = "e.ok",
                                                      .type = DE_CVAR_ENUM,
                                                      .enum_values = q,
                                                      .enum_count = 3,
                                                      .default_number = 2});
    EXPECT_STREQ(de_cvar_string(by_idx), "high");
}

/* ---- flags ----------------------------------------------------------------------------- */

TEST(cvar, readonly_startup_only) {
    reset();
    de_cvar ro = reg("ro.value", DE_CVAR_INT, DE_CVAR_READONLY);
    ASSERT_TRUE(DE_HANDLE_VALID(ro));

    de_error_clear();
    EXPECT_EQ(de_cvar_set(ro, "1"), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_mentions("ro.value"));
    EXPECT_EQ(de_cvar_int(ro), 0);

    char *argv[] = {"game", "+ro.value", "5", "+ro.late", "8"};
    EXPECT_EQ(de_cvar_apply_args(5, argv), DE_OK);
    EXPECT_EQ(de_cvar_int(ro), 5);
    static const char toml[] = "[ro]\nvalue = 6\n";
    EXPECT_EQ(de_cvar_load_from_memory(toml, sizeof toml - 1), DE_OK);
    EXPECT_EQ(de_cvar_int(ro), 6);
    EXPECT_EQ(de_cvar_set(ro, "7"), DE_ERR_UNSUPPORTED); /* the console never may */

    /* A pending startup value reaches a read-only cvar registered before the lock... */
    de_cvar late = reg("ro.late", DE_CVAR_INT, DE_CVAR_READONLY);
    EXPECT_EQ(de_cvar_int(late), 8);

    de_cvar_lock_startup();
    de_error_clear();
    EXPECT_EQ(de_cvar_apply_args(3, argv), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_mentions("ro.value"));
    EXPECT_EQ(de_cvar_load_from_memory(toml, sizeof toml - 1), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_cvar_int(ro), 6);

    /* ...but not to one registered after it. */
    char *argv2[] = {"game", "+ro.after", "9"};
    EXPECT_EQ(de_cvar_apply_args(3, argv2), DE_OK); /* pending: nobody registered it yet */
    de_cvar after = reg("ro.after", DE_CVAR_INT, DE_CVAR_READONLY);
    ASSERT_TRUE(DE_HANDLE_VALID(after));
    EXPECT_EQ(de_cvar_int(after), 0);

    /* Other cvars are not affected by the lock. */
    de_cvar rw = reg("rw.value", DE_CVAR_INT, 0);
    char *argv3[] = {"game", "+rw.value", "3"};
    EXPECT_EQ(de_cvar_apply_args(3, argv3), DE_OK);
    EXPECT_EQ(de_cvar_int(rw), 3);
}

TEST(cvar, dev_only_shipping) {
    reset();
    de_error_clear();
    de_cvar d = reg("dev.tool", DE_CVAR_BOOL, DE_CVAR_DEV_ONLY);
    if (SHIPPING) {
        EXPECT_EQ(d.id, 0u);
        EXPECT_TRUE(de_error_last() == NULL); /* silently */
        EXPECT_EQ(de_cvar_find("dev.tool").id, 0u);
        /* Startup sources keep its value pending without an error. */
        char *argv[] = {"game", "+dev.tool", "1"};
        EXPECT_EQ(de_cvar_apply_args(3, argv), DE_OK);
        EXPECT_EQ(reg("dev.tool", DE_CVAR_BOOL, DE_CVAR_DEV_ONLY).id, 0u);
    } else {
        ASSERT_TRUE(DE_HANDLE_VALID(d));
        EXPECT_EQ(de_cvar_find("dev.tool").id, d.id);
        EXPECT_EQ(de_cvar_set(d, "1"), DE_OK);
        EXPECT_TRUE(de_cvar_bool(d));
        EXPECT_EQ(de_cvar_get_flags(d), (u32)DE_CVAR_DEV_ONLY);
    }
    /* A cvar without the flag registers in every build. */
    EXPECT_TRUE(DE_HANDLE_VALID(reg("dev.not", DE_CVAR_BOOL, 0)));
}

TEST(cvar, cheat_gate) {
    reset();
    de_cvar god = reg("player.god", DE_CVAR_BOOL, DE_CVAR_CHEAT);
    de_cvar cheats = de_cvar_find("cheats");
    ASSERT_TRUE(DE_HANDLE_VALID(god) && DE_HANDLE_VALID(cheats));
    char *argv[] = {"game", "+player.god", "1"};
    static const char toml[] = "player.god = true\n";

    if (CHEATS_ALWAYS) {
        EXPECT_EQ(de_cvar_set(god, "1"), DE_OK);
        EXPECT_TRUE(de_cvar_bool(god));
        return;
    }
    de_error_clear();
    EXPECT_EQ(de_cvar_set(god, "1"), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_mentions("player.god"));
    EXPECT_EQ(de_cvar_apply_args(3, argv), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_cvar_load_from_memory(toml, sizeof toml - 1), DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(de_cvar_bool(god));

    EXPECT_EQ(de_cvar_set(cheats, "1"), DE_OK);
    EXPECT_EQ(de_cvar_set(god, "1"), DE_OK);
    EXPECT_TRUE(de_cvar_bool(god));
    EXPECT_EQ(de_cvar_set(god, "0"), DE_OK);
    EXPECT_EQ(de_cvar_apply_args(3, argv), DE_OK);
    EXPECT_TRUE(de_cvar_bool(god));

    EXPECT_EQ(de_cvar_set(cheats, "0"), DE_OK);
    EXPECT_EQ(de_cvar_set(god, "0"), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(de_cvar_bool(god));

    /* `+cheats 1` earlier on the same command line opens the gate for later pairs. */
    char *argv2[] = {"game", "+cheats", "1", "+player.god", "0"};
    EXPECT_EQ(de_cvar_apply_args(5, argv2), DE_OK);
    EXPECT_FALSE(de_cvar_bool(god));
}

static void count_calls(de_cvar cv, void *user) {
    (void)cv;
    ++*(int *)user;
}

TEST(cvar, args_pending) {
    reset();
    int calls = 0;
    char *argv[] = {"game", "-windowed",  "+late.value", "1", "+late.name", "bob", "+late.value",
                    "42",   "+bad..name", "x",           "+", "+late.flag", "on",  "+late.missing"};
    de_error_clear();
    de_result r = de_cvar_apply_args((int)(sizeof argv / sizeof argv[0]), argv);
    EXPECT_EQ(r, DE_ERR_INVALID_ARGUMENT); /* the first failure: the bad name */
    EXPECT_TRUE(error_mentions("bad..name"));
    EXPECT_TRUE(error_mentions("1 more")); /* "+late.missing" has no value */

    /* Registered later: the pending values apply, the last one for a name wins, and on_change
     * is not called for them. */
    de_cvar v = de_cvar_register(&(de_cvar_desc){
        .name = "late.value", .type = DE_CVAR_INT, .on_change = count_calls, .user = &calls});
    de_cvar n = de_cvar_register(
        &(de_cvar_desc){.name = "late.name", .type = DE_CVAR_STRING, .default_string = "x"});
    de_cvar f = reg("late.flag", DE_CVAR_BOOL, 0);
    EXPECT_EQ(de_cvar_int(v), 42);
    EXPECT_STREQ(de_cvar_string(n), "bob");
    EXPECT_TRUE(de_cvar_bool(f));

    /* A pending value that does not parse is dropped at registration. */
    char *argv2[] = {"game", "+late.typed", "not-a-number"};
    EXPECT_EQ(de_cvar_apply_args(3, argv2), DE_OK);
    de_error_clear();
    de_cvar typed = reg("late.typed", DE_CVAR_INT, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(typed)); /* the bad pending value is dropped... */
    EXPECT_EQ(de_cvar_int(typed), 0);
    EXPECT_TRUE(error_mentions("late.typed")); /* ...and the error context says why */

    EXPECT_EQ(calls, 0);

    /* Known names apply at once (and call on_change). */
    char *argv3[] = {"game", "+late.value", "7"};
    EXPECT_EQ(de_cvar_apply_args(3, argv3), DE_OK);
    EXPECT_EQ(de_cvar_int(v), 7);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(de_cvar_apply_args(0, NULL), DE_OK);
}

/* ---- on_change ------------------------------------------------------------------------- */

typedef struct change_log {
    int calls;
    de_cvar last;
    i64 seen_value;
    de_cvar mirror; /* set from inside the callback */
} change_log;

static void on_change_record(de_cvar cv, void *user) {
    change_log *log = (change_log *)user;
    log->calls++;
    log->last = cv;
    log->seen_value = de_cvar_int(cv);
    if (DE_HANDLE_VALID(log->mirror)) {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld", (long long)log->seen_value * 2);
        (void)de_cvar_set(log->mirror, buf); /* no lock is held: this must not deadlock */
    }
}

static void on_change_other(de_cvar cv, void *user) {
    (void)cv;
    ++*(int *)user;
}

TEST(cvar, on_change) {
    reset();
    change_log log = {0};
    log.mirror = reg("cb.mirror", DE_CVAR_INT, 0);
    de_cvar cv = de_cvar_register(&(de_cvar_desc){
        .name = "cb.value", .type = DE_CVAR_INT, .on_change = on_change_record, .user = &log});
    ASSERT_TRUE(DE_HANDLE_VALID(cv));
    EXPECT_EQ(log.calls, 0); /* not called for the default */

    EXPECT_EQ(de_cvar_set(cv, "3"), DE_OK);
    EXPECT_EQ(log.calls, 1);
    EXPECT_EQ(log.last.id, cv.id);
    EXPECT_EQ(log.seen_value, 3); /* the new value is visible in the callback */
    EXPECT_EQ(de_cvar_int(log.mirror), 6);

    EXPECT_EQ(de_cvar_set(cv, "3"), DE_OK); /* no change, no call */
    EXPECT_EQ(log.calls, 1);
    EXPECT_EQ(de_cvar_set(cv, "x"), DE_ERR_PARSE); /* failure, no call */
    EXPECT_EQ(log.calls, 1);

    char *argv[] = {"game", "+cb.value", "4"};
    EXPECT_EQ(de_cvar_apply_args(3, argv), DE_OK);
    EXPECT_EQ(log.calls, 2);
    static const char toml[] = "[cb]\nvalue = 5\n";
    EXPECT_EQ(de_cvar_load_from_memory(toml, sizeof toml - 1), DE_OK);
    EXPECT_EQ(log.calls, 3);
    EXPECT_EQ(de_cvar_int(log.mirror), 10);

    /* Registering again replaces the callback (a reloaded game library). */
    int other = 0;
    de_cvar again = de_cvar_register(&(de_cvar_desc){
        .name = "cb.value", .type = DE_CVAR_INT, .on_change = on_change_other, .user = &other});
    EXPECT_EQ(again.id, cv.id);
    EXPECT_EQ(de_cvar_int(cv), 5); /* the value stays */
    EXPECT_EQ(de_cvar_set(cv, "6"), DE_OK);
    EXPECT_EQ(other, 1);
    EXPECT_EQ(log.calls, 3);
}

/* ---- foreach --------------------------------------------------------------------------- */

typedef struct visit_log {
    u32 count;
    de_cvar seen[8];
} visit_log;

static void visit_record(de_cvar cv, void *user) {
    visit_log *v = (visit_log *)user;
    if (v->count < 8)
        v->seen[v->count] = cv;
    v->count++;
}

TEST(cvar, foreach_flag) {
    reset();
    de_cvar a = reg("rep.a", DE_CVAR_INT, DE_CVAR_REPLICATED);
    (void)reg("rep.b", DE_CVAR_INT, 0);
    de_cvar c = reg("rep.c", DE_CVAR_FLOAT, DE_CVAR_REPLICATED | DE_CVAR_SAVED);
    visit_log v = {0};
    de_cvar_foreach_flag(DE_CVAR_REPLICATED, visit_record, &v);
    ASSERT_EQ(v.count, 3u); /* the built-in `cheats` first, in registration order */
    EXPECT_EQ(v.seen[0].id, de_cvar_find("cheats").id);
    EXPECT_EQ(v.seen[1].id, a.id);
    EXPECT_EQ(v.seen[2].id, c.id);
    visit_log both = {0};
    de_cvar_foreach_flag(DE_CVAR_REPLICATED | DE_CVAR_SAVED, visit_record, &both);
    ASSERT_EQ(both.count, 1u);
    EXPECT_EQ(both.seen[0].id, c.id);
    visit_log all = {0};
    de_cvar_foreach_flag(0, visit_record, &all);
    EXPECT_EQ(all.count, 4u);
    de_cvar_foreach_flag(0, NULL, NULL);
}

/* ---- config.toml ----------------------------------------------------------------------- */

/* The cvars of cvars_basic.toml, registered in an order that interleaves tables. */
static void register_basic(void) {
    static const char *const filters[] = {"none", "pcf", "pcss"};
    const u32 S = DE_CVAR_SAVED;
    ASSERT_TRUE(DE_HANDLE_VALID(de_cvar_register(&(de_cvar_desc){.name = "volume",
                                                                 .help = "Master volume.",
                                                                 .type = DE_CVAR_FLOAT,
                                                                 .flags = S,
                                                                 .default_number = 1})));
    ASSERT_TRUE(DE_HANDLE_VALID(de_cvar_register(&(de_cvar_desc){.name = "r.shadows.quality",
                                                                 .help = "Shadow quality, 0-4.",
                                                                 .type = DE_CVAR_INT,
                                                                 .flags = S,
                                                                 .default_number = 2,
                                                                 .min = 0,
                                                                 .max = 4})));
    ASSERT_TRUE(DE_HANDLE_VALID(de_cvar_register(
        &(de_cvar_desc){.name = "net.rate", .help = "Not saved.", .type = DE_CVAR_INT})));
    ASSERT_TRUE(DE_HANDLE_VALID(de_cvar_register(&(de_cvar_desc){
        .name = "r.vsync", .type = DE_CVAR_BOOL, .flags = S, .default_number = 1})));
    ASSERT_TRUE(
        DE_HANDLE_VALID(de_cvar_register(&(de_cvar_desc){.name = "player.name",
                                                         .help = "Player name.\nShown to others.",
                                                         .type = DE_CVAR_STRING,
                                                         .flags = S,
                                                         .default_string = "Player"})));
    ASSERT_TRUE(DE_HANDLE_VALID(de_cvar_register(&(de_cvar_desc){.name = "r.shadows.filter",
                                                                 .type = DE_CVAR_ENUM,
                                                                 .flags = S,
                                                                 .enum_values = filters,
                                                                 .enum_count = 3})));
    ASSERT_TRUE(DE_HANDLE_VALID(de_cvar_register(&(de_cvar_desc){.name = "ui.scale",
                                                                 .type = DE_CVAR_FLOAT,
                                                                 .flags = S | DE_CVAR_REPLICATED,
                                                                 .default_number = 1})));
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_cvar_register(&(de_cvar_desc){.name = "zz", .type = DE_CVAR_BOOL, .flags = S})));
}

static const char k_basic_saved[] =
    "# DEngine config, written by de_cvar_save. Keys are cvar names; a [table] holds the\n"
    "# cvars whose names start with its name.\n"
    "\n"
    "# Master volume.\n"
    "volume = 0.1\n"
    "zz = true\n"
    "\n"
    "[r.shadows]\n"
    "# Shadow quality, 0-4.\n"
    "quality = 3\n"
    "filter = \"pcf\"\n"
    "\n"
    "[r]\n"
    "vsync = false\n"
    "\n"
    "[player]\n"
    "# Player name.\n"
    "# Shown to others.\n"
    "name = \"Ann \\\"the\\\" Tester\\t\xc3\xa9\\\\\"\n"
    "\n"
    "[ui]\n"
    "scale = 1.25\n";

TEST(cvar, toml_roundtrip) {
    reset();
    register_basic();
    ASSERT_EQ(de_cvar_load(DE_CVAR_TEST_DATA_DIR "/cvars_basic.toml"), DE_OK);
    EXPECT_TRUE(de_cvar_float(de_cvar_find("volume")) == 0.1);
    EXPECT_EQ(de_cvar_int(de_cvar_find("r.shadows.quality")), 3);
    EXPECT_EQ(de_cvar_int(de_cvar_find("net.rate")), -30);
    EXPECT_FALSE(de_cvar_bool(de_cvar_find("r.vsync")));
    EXPECT_STREQ(de_cvar_string(de_cvar_find("player.name")), "Ann \"the\" Tester\t\xc3\xa9\\");
    EXPECT_STREQ(de_cvar_string(de_cvar_find("r.shadows.filter")), "pcf");
    EXPECT_TRUE(de_cvar_float(de_cvar_find("ui.scale")) == 1.25);
    EXPECT_TRUE(de_cvar_bool(de_cvar_find("zz")));

    /* Save: SAVED cvars only, in registration order, grouped into tables. */
    const char *path_a = DE_CVAR_TEST_OUT_PREFIX "roundtrip_a.toml";
    const char *path_b = DE_CVAR_TEST_OUT_PREFIX "roundtrip_b.toml";
    const char *path_c = DE_CVAR_TEST_OUT_PREFIX "roundtrip_c.toml";
    ASSERT_EQ(de_cvar_save(path_a), DE_OK);
    size_t len_a = 0;
    char *a = read_file(path_a, &len_a);
    ASSERT_TRUE(a != NULL);
    EXPECT_STREQ(a, k_basic_saved);
    EXPECT_TRUE(strstr(a, "rate") == NULL);

    /* load -> save -> load -> save: byte-identical, starting from fresh defaults. */
    reset();
    register_basic();
    ASSERT_EQ(de_cvar_load(path_a), DE_OK);
    ASSERT_EQ(de_cvar_save(path_b), DE_OK);
    reset();
    register_basic();
    ASSERT_EQ(de_cvar_load(path_b), DE_OK);
    ASSERT_EQ(de_cvar_save(path_c), DE_OK);
    size_t len_b = 0, len_c = 0;
    char *b = read_file(path_b, &len_b);
    char *c = read_file(path_c, &len_c);
    ASSERT_TRUE(b != NULL && c != NULL);
    EXPECT_EQ(len_b, len_a);
    EXPECT_EQ(len_c, len_a);
    EXPECT_STREQ(b, a);
    EXPECT_STREQ(c, a);
    free(a);
    free(b);
    free(c);
    remove(path_a);
    remove(path_b);
    remove(path_c);
}

TEST(cvar, toml_values) {
    reset();
    /* Every type, and float values that need care to round-trip. */
    static const char *const m[] = {"a", "b"};
    const u32 S = DE_CVAR_SAVED;
    de_cvar f[6];
    static const char *const fnames[] = {"f.tenth",   "f.big",   "f.tiny",
                                         "f.negzero", "f.whole", "f.inf"};
    for (int k = 0; k < 6; ++k)
        f[k] = reg(fnames[k], DE_CVAR_FLOAT, S);
    de_cvar ctl = reg("s.ctl", DE_CVAR_STRING, S);
    de_cvar en = de_cvar_register(&(de_cvar_desc){
        .name = "e.m", .type = DE_CVAR_ENUM, .flags = S, .enum_values = m, .enum_count = 2});
    de_cvar big = reg("i.min", DE_CVAR_INT, S);
    ASSERT_EQ(de_cvar_set(f[0], "0.1"), DE_OK);
    ASSERT_EQ(de_cvar_set(f[1], "1.7976931348623157e308"), DE_OK);
    ASSERT_EQ(de_cvar_set(f[2], "4.9406564584124654e-324"), DE_OK);
    ASSERT_EQ(de_cvar_set(f[3], "-0.0"), DE_OK);
    ASSERT_EQ(de_cvar_set(f[4], "2"), DE_OK);
    ASSERT_EQ(de_cvar_set(f[5], "-inf"), DE_OK);
    ASSERT_EQ(de_cvar_set(ctl, "a\x01"
                               "b\x7f"
                               "c\r\n\"q\""),
              DE_OK);
    ASSERT_EQ(de_cvar_set(en, "b"), DE_OK);
    ASSERT_EQ(de_cvar_set(big, "-9223372036854775808"), DE_OK);

    const char *path = DE_CVAR_TEST_OUT_PREFIX "values.toml";
    ASSERT_EQ(de_cvar_save(path), DE_OK);
    char *text = read_file(path, NULL);
    ASSERT_TRUE(text != NULL);
    EXPECT_TRUE(strstr(text, "tenth = 0.1\n") != NULL);
    EXPECT_TRUE(strstr(text, "whole = 2.0\n") != NULL);
    EXPECT_TRUE(strstr(text, "negzero = -0.0\n") != NULL);
    EXPECT_TRUE(strstr(text, "inf = -inf\n") != NULL);
    EXPECT_TRUE(strstr(text, "ctl = \"a\\u0001b\\u007Fc\\r\\n\\\"q\\\"\"\n") != NULL);
    EXPECT_TRUE(strstr(text, "m = \"b\"\n") != NULL);
    EXPECT_TRUE(strstr(text, "min = -9223372036854775808\n") != NULL);

    /* Back to defaults, then load: every value is bit-exact again. */
    reset();
    for (int k = 0; k < 6; ++k)
        f[k] = reg(fnames[k], DE_CVAR_FLOAT, S);
    ctl = reg("s.ctl", DE_CVAR_STRING, S);
    en = de_cvar_register(&(de_cvar_desc){
        .name = "e.m", .type = DE_CVAR_ENUM, .flags = S, .enum_values = m, .enum_count = 2});
    big = reg("i.min", DE_CVAR_INT, S);
    ASSERT_EQ(de_cvar_load(path), DE_OK);
    EXPECT_TRUE(de_cvar_float(f[0]) == 0.1);
    EXPECT_TRUE(de_cvar_float(f[1]) == 1.7976931348623157e308);
    EXPECT_TRUE(de_cvar_float(f[2]) == 4.9406564584124654e-324);
    f64 nz = de_cvar_float(f[3]);
    u64 nz_bits;
    memcpy(&nz_bits, &nz, sizeof nz_bits);
    EXPECT_EQ(nz_bits, (u64)1 << 63);
    EXPECT_TRUE(de_cvar_float(f[4]) == 2.0);
    EXPECT_TRUE(de_cvar_float(f[5]) < -1.7976931348623157e308);
    EXPECT_STREQ(de_cvar_string(ctl), "a\x01"
                                      "b\x7f"
                                      "c\r\n\"q\"");
    EXPECT_EQ(de_cvar_int(en), 1);
    EXPECT_EQ(de_cvar_int(big), INT64_MIN);
    free(text);
    remove(path);
}

TEST(cvar, toml_unknown_pending) {
    reset();
    de_cvar known = reg("known", DE_CVAR_INT, 0);
    ASSERT_EQ(de_cvar_load(DE_CVAR_TEST_DATA_DIR "/cvars_unknown.toml"), DE_OK);
    EXPECT_EQ(de_cvar_int(known), 7);
    de_cvar speed = reg("future.module.speed", DE_CVAR_FLOAT, 0);
    de_cvar mode = reg("future.module.mode", DE_CVAR_STRING, 0);
    de_cvar enabled = reg("future.enabled", DE_CVAR_BOOL, 0);
    de_cvar count = reg("future.count", DE_CVAR_INT, 0);
    EXPECT_TRUE(de_cvar_float(speed) == 2.5);
    EXPECT_STREQ(de_cvar_string(mode), "fast");
    EXPECT_TRUE(de_cvar_bool(enabled));
    EXPECT_EQ(de_cvar_int(count), 12); /* a TOML string is parsed like console text */
}

TEST(cvar, toml_errors) {
    reset();
    de_cvar a = reg("err.a", DE_CVAR_INT, 0);
    de_cvar b = reg("err.b", DE_CVAR_INT, 0);

    /* Not TOML: nothing is applied. */
    static const char broken[] = "[err]\na = 1\nb = = 2\n";
    de_error_clear();
    EXPECT_EQ(de_cvar_load_from_memory(broken, sizeof broken - 1), DE_ERR_PARSE);
    EXPECT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_cvar_int(a), 0);

    /* Per-key failures: the other keys still apply; the first failure is returned. */
    static const char mixed[] = "[err]\na = [1, 2]\nb = 5\nc = 1979-05-27\n";
    de_error_clear();
    EXPECT_EQ(de_cvar_load_from_memory(mixed, sizeof mixed - 1), DE_ERR_PARSE);
    EXPECT_TRUE(error_mentions("err.a"));
    EXPECT_TRUE(error_mentions("1 more"));
    EXPECT_EQ(de_cvar_int(b), 5);

    static const char badname[] = "\"Upper\" = 1\n";
    EXPECT_EQ(de_cvar_load_from_memory(badname, sizeof badname - 1), DE_ERR_INVALID_ARGUMENT);
    static const char badvalue[] = "err.b = \"x\"\n";
    EXPECT_EQ(de_cvar_load_from_memory(badvalue, sizeof badvalue - 1), DE_ERR_PARSE);
    EXPECT_EQ(de_cvar_int(b), 5);

    /* A key path longer than a cvar name: 8 parts of 20 bytes. */
    char deep[400];
    size_t n = 0;
    for (int k = 0; k < 8; ++k) {
        memset(deep + n, 'k', 20);
        n += 20;
        deep[n++] = '.';
    }
    memcpy(deep + n, "x = 1\n", 7);
    EXPECT_EQ(de_cvar_load_from_memory(deep, strlen(deep)), DE_ERR_INVALID_ARGUMENT);

    EXPECT_EQ(de_cvar_load_from_memory("", 0), DE_OK);
    EXPECT_EQ(de_cvar_load_from_memory(NULL, 0), DE_OK);
    EXPECT_EQ(de_cvar_load_from_memory(NULL, 3), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_cvar_load(DE_CVAR_TEST_DATA_DIR "/does_not_exist.toml"), DE_ERR_IO);
    EXPECT_EQ(de_cvar_load(NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_cvar_save(NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_cvar_save(DE_CVAR_TEST_DATA_DIR "/no_such_dir/out.toml"), DE_ERR_IO);
}

/* ---- threads --------------------------------------------------------------------------- */

/* STRESS_STRINGS distinct string values, so new string copies are made (and published) while the
 * readers run: that is the path TSan has to see as ordered. */
enum { STRESS_SETS = 50000, STRESS_READERS = 3, STRESS_STRINGS = 500 };

/* True if `s` is exactly what the setter writes for some n: "value <n>". */
static bool stress_string_ok(const char *s) {
    if (s == NULL || strncmp(s, "value ", 6) != 0)
        return false;
    int n = atoi(s + 6);
    char expect[32];
    snprintf(expect, sizeof expect, "value %d", n);
    return n >= 0 && n < STRESS_STRINGS && strcmp(s, expect) == 0;
}

typedef struct stress_ctx {
    de_cvar i, f, s, e;
    de_atomic_u32 started;
    de_atomic_u32 done;
    u64 reads[STRESS_READERS];
    u64 bad[STRESS_READERS];
} stress_ctx;

typedef struct reader_arg {
    stress_ctx *ctx;
    u32 index;
} reader_arg;

static i32 stress_reader(void *user) {
    reader_arg *ra = (reader_arg *)user;
    stress_ctx *c = ra->ctx;
    u64 reads = 0, bad = 0;
    de_atomic_fetch_add_u32(&c->started, 1, DE_ATOMIC_RELEASE);
    while (de_atomic_load_u32(&c->done, DE_ATOMIC_ACQUIRE) == 0 || reads < 1000) {
        i64 iv = de_cvar_int(c->i);
        f64 fv = de_cvar_float(c->f);
        const char *sv = de_cvar_string(c->s);
        const char *ev = de_cvar_string(c->e);
        if (iv < 0 || iv >= 1000)
            ++bad;
        if (!(fv >= 0.0 && fv < 500.0) || fv != (f64)(i64)(fv * 2.0) / 2.0)
            ++bad;
        if (!stress_string_ok(sv))
            ++bad;
        if (ev == NULL || (strcmp(ev, "x") != 0 && strcmp(ev, "y") != 0))
            ++bad;
        ++reads;
    }
    c->reads[ra->index] = reads;
    c->bad[ra->index] = bad;
    return 0;
}

TEST(cvar, concurrent_get_set) {
    reset();
    static const char *const xy[] = {"x", "y"};
    stress_ctx c;
    memset(&c, 0, sizeof c);
    de_atomic_init_u32(&c.started, 0);
    de_atomic_init_u32(&c.done, 0);
    c.i = reg("mt.int", DE_CVAR_INT, 0);
    c.f = reg("mt.float", DE_CVAR_FLOAT, 0);
    c.s = de_cvar_register(
        &(de_cvar_desc){.name = "mt.string", .type = DE_CVAR_STRING, .default_string = "value 0"});
    c.e = de_cvar_register(&(de_cvar_desc){
        .name = "mt.enum", .type = DE_CVAR_ENUM, .enum_values = xy, .enum_count = 2});
    ASSERT_TRUE(DE_HANDLE_VALID(c.i) && DE_HANDLE_VALID(c.f));
    ASSERT_TRUE(DE_HANDLE_VALID(c.s) && DE_HANDLE_VALID(c.e));

    de_thread *t[STRESS_READERS];
    reader_arg args[STRESS_READERS];
    for (u32 k = 0; k < STRESS_READERS; ++k) {
        args[k] = (reader_arg){&c, k};
        ASSERT_EQ(
            de_thread_create(stress_reader, &args[k], "cvar_reader", DE_THREAD_NORMAL, 0, &t[k]),
            DE_OK);
    }
    while (de_atomic_load_u32(&c.started, DE_ATOMIC_ACQUIRE) < STRESS_READERS)
        de_cpu_pause(); /* the readers run before the first new string is published */
    char buf[32];
    u32 failures = 0;
    for (int n = 0; n < STRESS_SETS; ++n) {
        snprintf(buf, sizeof buf, "%d", n % 1000);
        failures += de_cvar_set(c.i, buf) != DE_OK;
        snprintf(buf, sizeof buf, "%.1f", (n % 1000) * 0.5);
        failures += de_cvar_set(c.f, buf) != DE_OK;
        snprintf(buf, sizeof buf, "value %d", (n * 7) % STRESS_STRINGS);
        failures += de_cvar_set(c.s, buf) != DE_OK;
        failures += de_cvar_set(c.e, xy[n % 2]) != DE_OK;
        /* Registration runs concurrently with the readers too. */
        if (n % 1000 == 0) {
            snprintf(buf, sizeof buf, "mt.reg.r%d", n / 1000);
            failures += !DE_HANDLE_VALID(reg(buf, DE_CVAR_INT, 0));
        }
    }
    de_atomic_store_u32(&c.done, 1, DE_ATOMIC_RELEASE);
    for (u32 k = 0; k < STRESS_READERS; ++k)
        de_thread_join(t[k]);
    EXPECT_EQ(failures, 0u);
    for (u32 k = 0; k < STRESS_READERS; ++k) {
        EXPECT_GE(c.reads[k], 1000u);
        EXPECT_EQ(c.bad[k], 0u);
    }
    EXPECT_EQ(de_cvar_int(c.i), (STRESS_SETS - 1) % 1000);
}

typedef struct lockfree_ctx {
    de_cvar i, s;
    de_atomic_u32 finished;
    i64 sum;
} lockfree_ctx;

static i32 lockfree_reader(void *user) {
    lockfree_ctx *c = (lockfree_ctx *)user;
    i64 sum = 0;
    for (int n = 0; n < 1000; ++n) {
        sum += de_cvar_int(c->i);
        sum += (i64)strlen(de_cvar_string(c->s));
        sum += de_cvar_bool(c->i) ? 1 : 0;
        sum += (i64)de_cvar_float(c->i);
        sum += (i64)de_cvar_get_flags(c->i) + (i64)strlen(de_cvar_name(c->i));
    }
    c->sum = sum;
    de_atomic_store_u32(&c->finished, 1, DE_ATOMIC_RELEASE);
    return 0;
}

TEST(cvar, getters_lock_free) {
    reset();
    lockfree_ctx c;
    memset(&c, 0, sizeof c);
    de_atomic_init_u32(&c.finished, 0);
    c.i = de_cvar_register(
        &(de_cvar_desc){.name = "lf.int", .type = DE_CVAR_INT, .default_number = 2});
    c.s = de_cvar_register(
        &(de_cvar_desc){.name = "lf.str", .type = DE_CVAR_STRING, .default_string = "abc"});
    ASSERT_TRUE(DE_HANDLE_VALID(c.i) && DE_HANDLE_VALID(c.s));

    /* While this thread holds the registry mutex, another thread's getters must finish. */
    de__cvar_test_lock();
    de_thread *t = NULL;
    de_result r = de_thread_create(lockfree_reader, &c, "cvar_lockfree", DE_THREAD_NORMAL, 0, &t);
    bool finished = false;
    u64 deadline = de_time_now_ns() + 10ull * 1000000000ull; /* 10 s */
    while (r == DE_OK && !finished && de_time_now_ns() < deadline) {
        finished = de_atomic_load_u32(&c.finished, DE_ATOMIC_ACQUIRE) != 0;
        de_cpu_pause();
    }
    de__cvar_test_unlock();
    ASSERT_EQ(r, DE_OK);
    de_thread_join(t);
    EXPECT_TRUE(finished);
    EXPECT_EQ(c.sum, 1000 * (2 + 3 + 1 + 2 + 0 + 6));
}
