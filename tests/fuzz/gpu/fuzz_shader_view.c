/*
 * fuzz_shader_view.c: fuzz target fuzz_shader_view (DENG-409): de__shader_view_init, the check of
 * a cooked `shader` asset (DSHD, src/gpu/shader/shader_format.h) that the runtime type runs on
 * every load, and the view accessors the loader and de__shader_blob use on a checked blob.
 *
 * When the check accepts an input, its promises are tested, and a broken one aborts: every entry
 * name and permutation string ends inside the string table; every record's code lies inside the
 * input, after the strings, 16-byte aligned; records are sorted and de__shader_view_find returns
 * each one's own index; push constants fit 128 bytes; indices and enums are in range. A refusal
 * must be DE_ERR_PARSE or DE_ERR_VERSION_MISMATCH and leave the view zeroed.
 *
 * Seeds (corpus/shader_view/): blobs cooked by `de-cook cook` (SPIR-V + metallib, SPIR-V only,
 * permutations, several entry points) and small hand-made blobs at the edges of the format.
 */
#include "de_fuzz.h"

#include <dengine/de_base.h>

#include "gpu/shader/shader_format.h"
#include "gpu/shader/shader_internal.h"

#include <stdlib.h>
#include <string.h>

static void check_string(const de__shader_view *v, const char *s) {
    const char *table = v->strings, *end = v->strings + v->header.strings_size;
    if (s < table || s >= end)
        abort();
    const char *nul = (const char *)memchr(s, 0, (size_t)(end - s));
    if (nul == NULL)
        abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    de__shader_view v;
    const de_result r = de__shader_view_init(data, size, &v);
    if (r != DE_OK) {
        if (r != DE_ERR_PARSE && r != DE_ERR_VERSION_MISMATCH)
            abort();
        static const de__shader_view zero;
        if (memcmp(&v, &zero, sizeof v) != 0)
            abort(); /* a refused blob leaves nothing behind */
        return 0;
    }
    const de__shader_header *h = &v.header;
    if (v.base != data || v.size != size || h->magic != DE__SHADER_MAGIC ||
        h->version != DE__SHADER_VERSION || h->entry_count == 0 || h->permutation_count == 0)
        abort();
    if ((const u8 *)v.strings + h->strings_size > data + size)
        abort();

    for (u32 i = 0; i < h->entry_count; ++i) {
        const char *name = NULL;
        de__shader_entry e = de__shader_view_entry(&v, i, &name);
        if (e.stage >= DE__SHADER_STAGE_COUNT_)
            abort();
        check_string(&v, name);
    }
    for (u32 i = 0; i < h->permutation_count; ++i)
        check_string(&v, de__shader_view_permutation(&v, i));

    u32 code_sum = 0;
    for (u32 i = 0; i < h->record_count; ++i) {
        de__shader_record rec = de__shader_view_record(&v, i);
        if (rec.target == DE__SHADER_TARGET_NONE || rec.target >= DE__SHADER_TARGET_COUNT_ ||
            rec.permutation >= h->permutation_count || rec.entry >= h->entry_count ||
            rec.stage >= DE__SHADER_STAGE_COUNT_ ||
            rec.push_constant_bytes > DE__SHADER_MAX_PUSH_CONSTANT_BYTES ||
            rec.code_offset % DE__SHADER_CODE_ALIGN != 0 ||
            (u64)rec.code_offset < (u64)h->strings_offset + h->strings_size ||
            (u64)rec.code_offset + rec.code_size > size)
            abort();
        /* Touch the code range, so ASan sees any read past the input. */
        if (rec.code_size != 0)
            code_sum += data[rec.code_offset] + data[rec.code_offset + rec.code_size - 1u];
        if (de__shader_view_find(&v, rec.target, rec.permutation, rec.entry) != (i32)i)
            abort();
    }
    /* Keys nobody can have: no record of target NONE, or past the tables. */
    if (de__shader_view_find(&v, DE__SHADER_TARGET_NONE, 0, 0) != -1 ||
        de__shader_view_find(&v, DE__SHADER_TARGET_SPIRV, h->permutation_count, 0) != -1 ||
        de__shader_view_find(&v, DE__SHADER_TARGET_METALLIB, 0, h->entry_count) != -1)
        abort();
    (void)code_sum;
    return 0;
}
