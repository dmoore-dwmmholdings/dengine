#include "text_internal.h"
#include <string.h>
de_result de_text_create(const de_allocator *a, de_asset_system *assets, de_text_system **out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "text output is NULL");
    *out = NULL;
    if (!a)
        a = de_heap_allocator();
    de_text_system *t = DE_NEW(a, de_text_system, DE_MEM_UI);
    if (!t)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "text system allocation");
    memset(t, 0, sizeof *t);
    t->alloc = a;
    t->assets = assets;
    *out = t;
    return DE_OK;
}
void de_text_destroy(de_text_system *t) {
    if (t)
        DE_FREE(t->alloc, t, sizeof *t);
}
