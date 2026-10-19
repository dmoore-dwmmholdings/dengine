#ifndef DE_TEXT_STRING_TABLE_FORMAT_H
#define DE_TEXT_STRING_TABLE_FORMAT_H
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
typedef struct de__loc_table {
    const u8 *data;
    u64 size;
    u32 count, blob;
} de__loc_table;
static inline u32 de__loc_u32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static inline u64 de__loc_u64(const u8 *p) {
    return de__loc_u32(p) | (u64)de__loc_u32(p + 4) << 32;
}
static inline void de__loc_w32(u8 *p, u32 v) {
    for (u32 i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (i * 8));
}
static inline void de__loc_w64(u8 *p, u64 v) {
    de__loc_w32(p, (u32)v);
    de__loc_w32(p + 4, (u32)(v >> 32));
}
de_result de__loc_table_parse(const void *bytes, u64 size, de__loc_table *out);
de_str de__loc_table_lookup(const de__loc_table *table, de_name id);
typedef struct de__loc_pair {
    const char *key;
    const char *value;
    size_t length;
    u64 hash;
} de__loc_pair;
de_result de__loc_table_build(const de_allocator *a, de__loc_pair *pairs, u32 count, void **out,
                              u64 *bytes);
/* Cooker descriptor lives in the content-owned importer registry. */
#include <dengine/de_asset.h>
de_asset_type_desc de_string_table_import_type_desc(void);
#endif
