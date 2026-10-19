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
