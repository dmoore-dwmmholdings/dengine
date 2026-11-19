/* Fuzz target for the material graph generator (DENG-579): any bytes, no crash, and a result that
 * keeps its invariants (NUL-terminated diagnostics, source iff no errors, line map in range). */
#include "cook/importers/material_graph/material_graph.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static volatile uint32_t sink;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1u << 20))
        return 0;
    de__mg_result r;
    de_result rc = de__mg_generate(&(de__mg_desc){.graph = (const char *)data,
                                                  .graph_size = size,
                                                  .name = "fuzz",
                                                  .allocator = de_heap_allocator()},
                                   &r);
    if (rc != DE_OK && rc != DE_ERR_PARSE && rc != DE_ERR_OUT_OF_MEMORY)
        abort();
    if ((rc == DE_OK) != (r.source != NULL) || (rc == DE_ERR_PARSE && r.error_count == 0))
        abort();
    if (r.source && strlen(r.source) != r.source_size)
        abort();
    for (u32 i = 0; i < r.diag_count; ++i) {
        const de__mg_diag *d = &r.diags[i];
        if (!memchr(d->node, 0, sizeof d->node) || !memchr(d->pin, 0, sizeof d->pin) ||
            !memchr(d->message, 0, sizeof d->message) || d->message[0] == 0)
            abort();
    }
    for (u32 line = 0; r.source && line <= r.line_count_ + 1; ++line) {
        const char *node = de__mg_node_at_line(&r, line);
        if (node)
            sink ^= (uint32_t)strlen(node);
    }
    sink ^= r.param_count ^ r.features;
    de__mg_result_free(&r);
    if (r.source || r.diags || r.params)
        abort();
    return 0;
}
