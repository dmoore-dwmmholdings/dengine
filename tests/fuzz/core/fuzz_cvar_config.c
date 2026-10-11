/*
 * fuzz_cvar_config: de_cvar_load_from_memory (DENG-154, config.toml) on untrusted text.
 *
 * Each input runs against a fresh registry: a fixed set of cvars of every type and flag is
 * registered, the input is loaded, then every value is read back and checked against its type's
 * rules (int and float ranges clamp, floats are never NaN, an enum is one of its names, and a
 * read-only cvar refuses the input once startup is locked). Keys the registry does not know stay
 * pending. de__cvar_test_reset (a test hook in src/core/cvar.c) frees everything afterwards, so
 * nothing survives between inputs.
 */
#include "de_fuzz.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void de__cvar_test_reset(void);

static const char *const k_filters[] = {"none", "pcf", "pcss"};

typedef struct fuzz_cvars {
    de_cvar vsync, quality, volume, name, filter, rate, god, dev;
} fuzz_cvars;

static void check(int ok, const char *what) {
    if (ok)
        return;
    fprintf(stderr, "fuzz_cvar_config: %s\n", what);
    abort();
}

static void register_all(fuzz_cvars *c) {
    c->vsync = de_cvar_register(&(const de_cvar_desc){
        .name = "r.vsync", .type = DE_CVAR_BOOL, .flags = DE_CVAR_SAVED, .default_number = 1});
    c->quality = de_cvar_register(&(const de_cvar_desc){
        .name = "r.shadows.quality", .type = DE_CVAR_INT, .default_number = 2, .min = 0, .max = 4});
    c->volume = de_cvar_register(&(const de_cvar_desc){
        .name = "volume", .type = DE_CVAR_FLOAT, .default_number = 0.5, .min = 0, .max = 1});
    c->name = de_cvar_register(&(const de_cvar_desc){
        .name = "player.name", .type = DE_CVAR_STRING, .default_string = "player"});
    c->filter = de_cvar_register(&(const de_cvar_desc){.name = "r.shadows.filter",
                                                       .type = DE_CVAR_ENUM,
                                                       .enum_values = k_filters,
                                                       .enum_count = 3,
                                                       .default_string = "pcf"});
    c->rate = de_cvar_register(&(const de_cvar_desc){
        .name = "net.rate", .type = DE_CVAR_INT, .flags = DE_CVAR_READONLY, .default_number = 30});
    c->god = de_cvar_register(
        &(const de_cvar_desc){.name = "cheat.god", .type = DE_CVAR_BOOL, .flags = DE_CVAR_CHEAT});
    c->dev = de_cvar_register(
        &(const de_cvar_desc){.name = "dev.only", .type = DE_CVAR_BOOL, .flags = DE_CVAR_DEV_ONLY});
    check(DE_HANDLE_VALID(c->vsync) && DE_HANDLE_VALID(c->quality) && DE_HANDLE_VALID(c->volume) &&
              DE_HANDLE_VALID(c->name) && DE_HANDLE_VALID(c->filter) && DE_HANDLE_VALID(c->rate) &&
              DE_HANDLE_VALID(c->god),
          "registration failed");
}

static void check_values(const fuzz_cvars *c) {
    (void)de_cvar_bool(c->vsync);
    const i64 q = de_cvar_int(c->quality);
    check(q >= 0 && q <= 4, "int cvar outside its range");
    const f64 v = de_cvar_float(c->volume);
    check(v == v && v >= 0.0 && v <= 1.0, "float cvar NaN or outside its range");
    const char *name = de_cvar_string(c->name);
    check(name != NULL, "string cvar is NULL");
    const char *filter = de_cvar_string(c->filter);
    check(filter != NULL, "enum cvar has no name");
    int known = 0;
    for (u32 i = 0; i < 3; ++i)
        known |= strcmp(filter, k_filters[i]) == 0;
    check(known, "enum cvar is not one of its names");
    check(de_cvar_int(c->filter) >= 0 && de_cvar_int(c->filter) < 3, "enum index out of range");
    (void)de_cvar_bool(c->god);
    (void)de_cvar_bool(c->dev);
}

static void visit(de_cvar cv, void *user) {
    (void)user;
    check(de_cvar_name(cv) != NULL, "visited cvar without a name");
    (void)de_cvar_help(cv);
    (void)de_cvar_get_type(cv);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Refused keys warn in category "core"; keep the replay quiet. */
    de_log_set_level(DE_LOG_CAT_CORE, DE_LOG_FATAL);

    fuzz_cvars c;
    register_all(&c);
    (void)de_cvar_load_from_memory((const char *)data, size);
    check_values(&c);
    de_cvar_foreach_flag(0, visit, NULL);

    /* After startup is locked, the read-only cvar refuses the same input. */
    const i64 rate = de_cvar_int(c.rate);
    de_cvar_lock_startup();
    (void)de_cvar_load_from_memory((const char *)data, size);
    check_values(&c);
    check(de_cvar_int(c.rate) == rate, "read-only cvar changed after de_cvar_lock_startup");

    de_error_clear();
    de__cvar_test_reset();
    return 0;
}
