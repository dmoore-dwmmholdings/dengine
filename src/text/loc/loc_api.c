#include "text/text_internal.h"
#include "text/loc/loc_internal.h"
de_result de_loc_set_locale(de_text_system *t, const char *locale) {
    return de__loc_set_locale(t ? t->loc : NULL, locale);
}
de_result de_loc_set_default_locale(de_text_system *t, const char *locale) {
    return de__loc_set_default_locale(t ? t->loc : NULL, locale);
}
const char *de_loc_locale(const de_text_system *t) {
    return de__loc_locale(t ? t->loc : NULL);
}
u64 de_loc_generation(const de_text_system *t) {
    return de__loc_generation(t ? t->loc : NULL);
}
de_str de_loc(const de_text_system *t, de_name id) {
    return de__loc_lookup(t ? t->loc : NULL, id);
}
u32 de_loc_format(const de_text_system *t, de_name id, const de_loc_arg *args, u32 count, char *out,
                  u32 capacity) {
    return de__loc_format(t ? t->loc : NULL, id, args, count, out, capacity);
}
void de_loc_pseudo(de_text_system *t, bool enabled) {
    de__loc_pseudo(t ? t->loc : NULL, enabled);
}

u32 de_loc_format_pattern(const de_text_system *t, de_str pattern, const de_loc_arg *args,
                          u32 count, char *out, u32 capacity) {
    u32 needed = 0;
    if (de__loc_message(pattern.ptr, pattern.len, de_loc_locale(t), args, count, out, capacity,
                        &needed) != DE_OK)
        return 0;
    return needed;
}

/* DENG-781: editor preview and inspection. */
de_result de_loc_preview_locale(de_text_system *t, const char *locale) {
    return t ? de__loc_preview_locale(t->loc, locale) : DE_ERR_INVALID_ARGUMENT;
}
de_result de_loc_get_locale_state(const de_text_system *t, de_loc_locale_state *out) {
    return t ? de__loc_state(t->loc, out) : DE_ERR_INVALID_ARGUMENT;
}
de_result de_loc_list_locales(const de_text_system *t, de_loc_locale_info *out, u32 capacity,
                              u32 *count) {
    return t ? de__loc_list_locales(t->loc, out, capacity, count) : DE_ERR_INVALID_ARGUMENT;
}
de_result de_loc_read_strings(const de_text_system *t, de_loc_string *out, u32 capacity,
                              u32 *count) {
    return t ? de__loc_read_strings(t->loc, out, NULL, capacity, count) : DE_ERR_INVALID_ARGUMENT;
}
de_result de_loc_read_strings_with_tables(const de_text_system *t, de_loc_string *out,
                                          de_asset_id *table_assets, u32 capacity, u32 *count) {
    if (!t || (capacity && !table_assets))
        return DE_ERR_INVALID_ARGUMENT;
    return de__loc_read_strings(t->loc, out, table_assets, capacity, count);
}
