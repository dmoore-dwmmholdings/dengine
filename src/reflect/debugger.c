/* Debugger output reads the same current descriptions as serialization and the inspector. */
#include "ser_internal.h"
#include <stdlib.h>
#include <string.h>

#if !defined(DE_SHIPPING)
#include "debugger_lldb.inc"

static int by_name(const void *a, const void *b) {
    const de__type_entry *const *x = a, *const *y = b;
    return strcmp((*x)->name, (*y)->name);
}

static void name(de__ser_out *o, const char *s) {
    /* Registry names are validated C identifiers: safe in XML text and Python quotes. */
    de__ser_out_puts(o, s);
}

static void xml_type(de__ser_out *o, const de_type_desc *d) {
    de__ser_out_puts(o, "  <Type Name=\"");
    name(o, d->name);
    de__ser_out_puts(o, "\">\n    <DisplayString Condition=\"sizeof(*this) != ");
    de__ser_out_putf(o, "%u", d->size);
    de__ser_out_puts(o, "\">reflection layout changed; regenerate visualizers</DisplayString>\n"
                        "    <DisplayString>");
    name(o, d->name);
    de__ser_out_puts(o, " {{");
    u32 rows = !strcmp(d->name, "de_mat3") || !strcmp(d->name, "de_affine") ? 3 : 4;
    u32 columns = !strcmp(d->name, "de_mat3") ? 3 : 4;
    bool matrix = (!strcmp(d->name, "de_mat3") || !strcmp(d->name, "de_mat4") ||
                   !strcmp(d->name, "de_affine")) &&
                  d->field_count == 1 && !strcmp(d->fields[0].name, "m") &&
                  d->fields[0].kind == DE_FK_FIXED_ARRAY &&
                  d->fields[0].attrs.array_count == rows * columns;
    if (matrix) {
        for (u32 row = 0; row < rows; ++row) {
            de__ser_out_puts(o, row ? " (" : "(");
            for (u32 col = 0; col < columns; ++col) {
                if (col)
                    de__ser_out_puts(o, ", ");
                de__ser_out_putf(o, "{m[%u]}", col * rows + row);
            }
            de__ser_out_puts(o, ")");
        }
    }
    for (u32 i = 0; !matrix && i < d->field_count && i < 6; ++i) {
        if (i)
            de__ser_out_puts(o, ", ");
        name(o, d->fields[i].name);
        de__ser_out_puts(o, "={");
        name(o, d->fields[i].name);
        de__ser_out_puts(o, "}");
    }
    if (!matrix && d->field_count > 6)
        de__ser_out_puts(o, ", ...");
    de__ser_out_puts(o, "}}</DisplayString>\n    <Expand>\n");
    for (u32 i = 0; i < d->field_count; ++i) {
        de__ser_out_puts(o, "      <Item Name=\"");
        name(o, d->fields[i].name);
        de__ser_out_puts(o, "\">");
        name(o, d->fields[i].name);
        de__ser_out_puts(o, "</Item>\n");
    }
    de__ser_out_puts(o, "    </Expand>\n  </Type>\n");
}
#endif

de_result de_debug_visualizers_write(const de_type_registry *r, de_writer *natvis,
                                     de_writer *lldb) {
#if defined(DE_SHIPPING)
    (void)r;
    (void)natvis;
    (void)lldb;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "debug visualizers are unavailable in Shipping");
#else
    if (!r || (!natvis && !lldb) || (natvis && !natvis->write) || (lldb && !lldb->write))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "debug visualizers: registry/writers required");
    size_t bytes = (size_t)r->entries.count * sizeof(de__type_entry *);
    const de__type_entry **entries =
        bytes ? DE_ALLOC(r->a, bytes, DE_ALIGNOF(void *), DE_MEM_GENERAL) : NULL;
    if (bytes && !entries)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "debug visualizers: sorted types");
    u32 count = 0;
    for (u32 i = 0; i < r->entries.count; ++i) {
        const de__type_entry *e = &((const de__type_entry *)r->entries.data)[i];
        if (e->kind == DE__ENTRY_STRUCT)
            entries[count++] = e;
    }
    if (count > 1)
        qsort(entries, count, sizeof *entries, by_name);
    de_result result = DE_OK;
    if (natvis) {
        de__ser_out o;
        de__ser_out_init(&o, natvis);
        de__ser_out_puts(&o,
                         "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                         "<!-- Generated from DEngine reflection; do not edit. -->\n"
                         "<AutoVisualizer "
                         "xmlns=\"http://schemas.microsoft.com/vstudio/debugger/natvis/2010\">\n");
        for (u32 i = 0; i < count; ++i) {
            const char *n = entries[i]->name;
            if (strcmp(n, "de_str") && strcmp(n, "de_array") && strcmp(n, "de_hashmap") &&
                strcmp(n, "de_entity"))
                xml_type(&o, &entries[i]->cur->desc);
        }
        de__ser_out_puts(
            &o, "  <Type Name=\"de_str\"><DisplayString Condition=\"len == 0\">\"\"</DisplayString>"
                "<DisplayString Condition=\"ptr == 0\">invalid string</DisplayString>"
                "<DisplayString>{ptr,[len &lt; 4096 ? len : 4096]s8}</DisplayString></Type>\n"
                "  <Type Name=\"de_array\"><DisplayString>{count} elements, capacity "
                "{capacity}</DisplayString></Type>\n"
                "  <Type Name=\"de_hashmap\"><DisplayString>{count} elements, capacity "
                "{capacity}</DisplayString></Type>\n"
                "  <Type Name=\"de_entity\"><DisplayString Condition=\"bits == 0\">null "
                "entity</DisplayString>"
                "<DisplayString>entity {(unsigned int)(bits &amp; 0xffffffff),d} gen {(unsigned "
                "int)(bits &gt;&gt; 32),d}</DisplayString></Type>\n"
                "</AutoVisualizer>\n");
        result = de__ser_out_flush(&o);
    }
    if (lldb && result == DE_OK) {
        de__ser_out o;
        de__ser_out_init(&o, lldb);
        for (size_t i = 0; i < sizeof de_debugger_python / sizeof *de_debugger_python; ++i)
            de__ser_out_puts(&o, de_debugger_python[i]);
        de__ser_out_puts(&o, "TYPES = {\n");
        for (u32 i = 0; i < count; ++i) {
            const de_type_desc *d = &entries[i]->cur->desc;
            de__ser_out_puts(&o, "    '");
            name(&o, d->name);
            de__ser_out_putf(&o, "': (%u, [", d->size);
            for (u32 j = 0; j < d->field_count; ++j) {
                if (j)
                    de__ser_out_puts(&o, ", ");
                de__ser_out_puts(&o, "'");
                name(&o, d->fields[j].name);
                de__ser_out_puts(&o, "'");
            }
            de__ser_out_puts(&o, "]),\n");
        }
        de__ser_out_puts(&o, "}\n");
        result = de__ser_out_flush(&o);
    }
    if (entries)
        DE_FREE(r->a, entries, bytes);
    return result;
#endif
}
