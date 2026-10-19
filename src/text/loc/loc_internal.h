#ifndef DE_TEXT_LOC_INTERNAL_H
#define DE_TEXT_LOC_INTERNAL_H
#include <dengine/de_text.h>
#include <dengine/de_memory.h>
#include "platform/de_atomic.h"
typedef struct de__loc de__loc;
de_result de__loc_create(const de_allocator *a, de_asset_system *assets, de__loc **out);
void de__loc_destroy(de__loc *loc);
void de__loc_update(de__loc *loc);
de_result de__loc_set_locale(de__loc *loc, const char *locale);
de_result de__loc_set_default_locale(de__loc *loc, const char *locale);
const char *de__loc_locale(const de__loc *loc);
u64 de__loc_generation(const de__loc *loc);
de_str de__loc_lookup(const de__loc *loc, de_name id);
u32 de__loc_format(const de__loc *loc, de_name id, const de_loc_arg *args, u32 count, char *out,
                   u32 capacity);
void de__loc_pseudo(de__loc *loc, bool enabled);
de_result de__loc_register(de_asset_system *assets);
bool de__loc_valid_locale(const char *locale);
/* All formatting and validation are bounded, reentrant and allocation-free. */
typedef struct de__loc_writer {
    char *out;
    u32 capacity;
    u64 needed;
} de__loc_writer;
void de__loc_write(de__loc_writer *w, const char *s, size_t n);
u32 de__loc_finish(de__loc_writer *w);
de_result de__loc_message(const char *pattern, size_t bytes, const char *locale,
                          const de_loc_arg *args, u32 count, char *out, u32 capacity, u32 *needed);
de_result de__loc_validate(const char *pattern, size_t bytes, size_t *error_offset);
typedef enum de__plural {
    DE__PLURAL_ZERO,
    DE__PLURAL_ONE,
    DE__PLURAL_TWO,
    DE__PLURAL_FEW,
    DE__PLURAL_MANY,
    DE__PLURAL_OTHER
} de__plural;
typedef struct de__plural_operands {
    f64 n, i, v, w, f, t, e;
} de__plural_operands;
de__plural de__loc_plural(const char *locale, const char *number);
de__plural de__loc_plural_number(const char *locale, f64 number);
de__plural de__loc_plural_generated(const char *locale, const de__plural_operands *o);
const char *de__loc_plural_name(de__plural category);
u32 de__loc_number(const char *locale, f64 value, const char *style, char *out, u32 capacity);
u32 de__loc_date(const char *locale, i64 timestamp, const char *style, char *out, u32 capacity);
u32 de__loc_pseudo_text(const char *input, size_t bytes, char *out, u32 capacity);
typedef struct de__loc_number_data {
    const char *locale, *decimal, *group, *percent, *short_date, *medium_date;
    const char *months[12];
    u32 primary, secondary;
} de__loc_number_data;
const de__loc_number_data *de__loc_number_data_get(const char *locale);
#endif
