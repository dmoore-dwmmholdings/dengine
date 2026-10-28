/* hints.c: inspector hints beside the type registry (de_reflect.h, DENG-624). A small table keyed
 * by type and field name; registration is main-thread, like the registry's. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include <stdio.h>
#include <string.h>

#define HINT_MAX 512u

typedef struct hint_entry {
    char type[64];
    char field[64];
    char group[64];
    char show_if[96];
    de_field_hints hints;
} hint_entry;

static hint_entry *g_hints;
static u32 g_hint_count;

static hint_entry *find(const char *type_name, const char *field) {
    for (u32 i = 0; i < g_hint_count; ++i)
        if (strcmp(g_hints[i].type, type_name) == 0 && strcmp(g_hints[i].field, field) == 0)
            return &g_hints[i];
    return NULL;
}

de_result de_field_hints_set(const char *type_name, const char *field, const de_field_hints *hints) {
    if (type_name == NULL || field == NULL || hints == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_field_hints_set: NULL argument");
    if (g_hints == NULL) {
        g_hints = (hint_entry *)DE_ALLOC(de_heap_allocator(), sizeof(hint_entry) * HINT_MAX, 8, DE_MEM_GENERAL);
        if (g_hints == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_field_hints_set");
    }
    hint_entry *e = find(type_name, field);
    if (e == NULL) {
        if (g_hint_count == HINT_MAX)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_field_hints_set: more than %u hinted fields", HINT_MAX);
        e = &g_hints[g_hint_count++];
        memset(e, 0, sizeof *e);
        snprintf(e->type, sizeof e->type, "%s", type_name);
        snprintf(e->field, sizeof e->field, "%s", field);
    }
    e->hints = *hints;
    snprintf(e->group, sizeof e->group, "%s", hints->group ? hints->group : "");
    snprintf(e->show_if, sizeof e->show_if, "%s", hints->show_if ? hints->show_if : "");
    e->hints.group = hints->group ? e->group : NULL;
    e->hints.show_if = hints->show_if ? e->show_if : NULL;
    return DE_OK;
}

const de_field_hints *de_field_hints_get(const char *type_name, const char *field) {
    if (type_name == NULL || field == NULL)
        return NULL;
    const hint_entry *e = find(type_name, field);
    return e != NULL ? &e->hints : NULL;
}
