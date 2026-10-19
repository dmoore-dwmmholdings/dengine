#include "text/loc/loc_internal.h"
#include <string.h>
u32 de__loc_pseudo_text(const char *input, size_t bytes, char *out, u32 capacity) {
    static const char *lower[] = {"á", "ƀ", "ç", "ď", "ë", "ƒ", "ğ", "ħ", "ï", "ĵ", "ķ", "ľ", "ɱ",
                                  "ñ", "ö", "þ", "ɋ", "ř", "š", "ť", "ü", "ṽ", "ŵ", "ẋ", "ÿ", "ž"};
    static const char *upper[] = {"Á", "Ɓ", "Ç", "Ď", "Ë", "Ƒ", "Ğ", "Ħ", "Ï", "Ĵ", "Ķ", "Ľ", "Ṁ",
                                  "Ñ", "Ö", "Þ", "Ɋ", "Ř", "Š", "Ť", "Ü", "Ṽ", "Ŵ", "Ẋ", "Ÿ", "Ž"};
    de__loc_writer w = {out, capacity, 0};
    de__loc_write(&w, "[!! ", 4);
    u32 letters = 0;
    for (size_t i = 0; i < bytes;) {
        char c = input[i];
        if (c == '{' || c == '<' || c == '[') {
            char open = c, close = c == '{' ? '}' : c == '<' ? '>' : ']';
            size_t start = i++;
            u32 depth = 1;
            while (i < bytes && depth) {
                if (input[i] == open)
                    ++depth;
                else if (input[i] == close)
                    --depth;
                ++i;
            }
            de__loc_write(&w, input + start, i - start);
            continue;
        }
        const char *replacement = c >= 'a' && c <= 'z'   ? lower[c - 'a']
                                  : c >= 'A' && c <= 'Z' ? upper[c - 'A']
                                                         : NULL;
        if (replacement) {
            de__loc_write(&w, replacement, strlen(replacement));
            ++letters;
        } else
            de__loc_write(&w, input + i, 1);
        ++i;
    }
    for (u32 i = 0; i < (letters * 35 + 99) / 100; ++i)
        de__loc_write(&w, "~", 1);
    de__loc_write(&w, " !!]", 4);
    return de__loc_finish(&w);
}
