#include "text/loc/string_table_format.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    de__loc_table table;
    if (de__loc_table_parse(data, size, &table) == DE_OK) {
        (void)de__loc_table_lookup(&table, (de_name){0});
        (void)de__loc_table_lookup(&table, (de_name){UINT64_MAX});
        for (u32 i = 0; i < table.count; ++i) {
            de_name id = {de__loc_u64(table.data + 32 + (u64)i * 16)};
            de_str value = de__loc_table_lookup(&table, id);
            if (!value.ptr || value.len >= table.size)
                abort();
        }
    }
    return 0;
}
