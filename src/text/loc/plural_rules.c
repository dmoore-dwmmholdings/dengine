#include "text/loc/loc_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
de__plural de__loc_plural(const char *locale, const char *number) {
    if (!locale || !number)
        return DE__PLURAL_OTHER;
    char *end = NULL;
    f64 n = strtod(number, &end);
    if (end == number || !isfinite(n))
        return DE__PLURAL_OTHER;
    de__plural_operands o = {0};
    o.n = fabs(n);
    o.i = floor(o.n);
    const char *dot = strchr(number, '.');
    if (dot && dot < end) {
        const char *p = dot + 1;
        f64 fraction = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            if (o.v < 18) {
                fraction = fraction * 10 + (*p - '0');
                ++o.v;
            }
            ++p;
        }
        o.f = o.t = fraction;
        o.w = o.v;
        while (o.w > 0 && fmod(o.t, 10) == 0) {
            o.t /= 10;
            --o.w;
        }
    }
    if (*end == 'c' || *end == 'e' || *end == 'E') {
        char *exponent_end = NULL;
        long exponent = strtol(end + 1, &exponent_end, 10);
        if (*exponent_end || exponent < 0 || exponent > 18)
            return DE__PLURAL_OTHER;
        o.e = exponent;
        if (*end == 'c') {
            o.n *= pow(10, exponent);
            o.i = floor(o.n);
        }
    } else if (*end)
        return DE__PLURAL_OTHER;
    return de__loc_plural_generated(locale, &o);
}
de__plural de__loc_plural_number(const char *locale, f64 number) {
    char text[64];
    snprintf(text, sizeof text, "%.15g", number);
    return de__loc_plural(locale, text);
}
const char *de__loc_plural_name(de__plural category) {
    static const char *names[] = {"zero", "one", "two", "few", "many", "other"};
    return names[category <= DE__PLURAL_OTHER ? category : DE__PLURAL_OTHER];
}
