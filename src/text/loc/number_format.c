#include "text/loc/loc_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void pattern_number(de__loc_writer *w, const char *pattern, const char *number,
                           const char *sign) {
    bool replaced = false, quote = false;
    for (size_t i = 0; pattern[i] && pattern[i] != ';';) {
        char c = pattern[i];
        if (c == '\'') {
            quote = !quote;
            ++i;
            continue;
        }
        if (!quote && (c == '#' || c == '0' || c == ',' || c == '.')) {
            if (!replaced) {
                de__loc_write(w, sign, strlen(sign));
                de__loc_write(w, number, strlen(number));
                replaced = true;
            }
            do {
                c = pattern[++i];
            } while (c == '#' || c == '0' || c == ',' || c == '.');
        } else {
            de__loc_write(w, pattern + i, 1);
            ++i;
        }
    }
}
u32 de__loc_number(const char *locale, f64 value, const char *style, char *out, u32 capacity) {
    de__loc_writer writer = {out, capacity, 0};
    const de__loc_number_data *data = de__loc_number_data_get(locale ? locale : "en");
    bool percent = style && strcmp(style, "percent") == 0;
    bool integer = style && strcmp(style, "integer") == 0;
    if (percent)
        value *= 100;
    if (!isfinite(value)) {
        const char *special = isnan(value) ? "NaN" : signbit(value) ? "-∞" : "∞";
        de__loc_write(&writer, special, strlen(special));
        return de__loc_finish(&writer);
    }
    char raw[384], formatted[768];
    bool negative = value < 0;
    value = fabs(value);
    if (value >= 1e21)
        snprintf(raw, sizeof raw, "%.15g", value);
    else if (integer)
        snprintf(raw, sizeof raw, "%.0f", floor(value + 0.5));
    else
        snprintf(raw, sizeof raw, "%.9f", value);
    char *dot = strchr(raw, '.');
    if (dot && !strchr(raw, 'e')) {
        char *tail = raw + strlen(raw);
        while (tail > dot + 1 && tail[-1] == '0')
            *--tail = 0;
        if (tail == dot + 1)
            *dot = 0;
    }
    de__loc_writer number = {formatted, sizeof formatted, 0};
    size_t whole = strcspn(raw, ".e"), n = strlen(raw);
    for (size_t i = 0; i < whole; ++i) {
        size_t right = whole - i;
        if (i && data->primary &&
            (right == data->primary || (right > data->primary && data->secondary &&
                                        (right - data->primary) % data->secondary == 0)))
            de__loc_write(&number, data->group, strlen(data->group));
        de__loc_write(&number, raw + i, 1);
    }
    if (whole < n && raw[whole] == '.') {
        de__loc_write(&number, data->decimal, strlen(data->decimal));
        de__loc_write(&number, raw + whole + 1, n - whole - 1);
    } else if (whole < n)
        de__loc_write(&number, raw + whole, n - whole);
    de__loc_finish(&number);
    if (percent)
        pattern_number(&writer, data->percent, formatted, negative ? "-" : "");
    else {
        if (negative)
            de__loc_write(&writer, "-", 1);
        de__loc_write(&writer, formatted, strlen(formatted));
    }
    return de__loc_finish(&writer);
}
static void digits(de__loc_writer *w, i32 value, u32 width) {
    char text[24];
    int n = snprintf(text, sizeof text, "%0*d", (int)width, value);
    if (n > 0)
        de__loc_write(w, text, (size_t)n);
}
u32 de__loc_date(const char *locale, i64 timestamp, const char *style, char *out, u32 capacity) {
    de__loc_writer writer = {out, capacity, 0};
    if (timestamp < -62135596800ll || timestamp > 253402300799ll)
        return de__loc_finish(&writer);
    i64 days = timestamp / 86400;
    if (timestamp % 86400 < 0)
        --days;
    i64 z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    u32 doe = (u32)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i32 year = (i32)yoe + (i32)era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    u32 day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    year += month <= 2;
    const de__loc_number_data *data = de__loc_number_data_get(locale ? locale : "en");
    const char *p = style && strcmp(style, "medium") == 0 ? data->medium_date : data->short_date;
    bool quote = false;
    for (size_t i = 0; p[i];) {
        char c = p[i];
        if (c == '\'') {
            if (p[i + 1] == '\'') {
                de__loc_write(&writer, "'", 1);
                i += 2;
            } else {
                quote = !quote;
                ++i;
            }
            continue;
        }
        if (!quote && (c == 'y' || c == 'M' || c == 'L' || c == 'd')) {
            size_t start = i;
            while (p[i] == c)
                ++i;
            u32 count = (u32)(i - start);
            if (c == 'y')
                digits(&writer, count == 2 ? year % 100 : year, count == 2 ? 2 : 1);
            else if (c == 'd')
                digits(&writer, (i32)day, count == 2 ? 2 : 1);
            else if (count >= 3)
                de__loc_write(&writer, data->months[month - 1], strlen(data->months[month - 1]));
            else
                digits(&writer, (i32)month, count == 2 ? 2 : 1);
        } else {
            de__loc_write(&writer, p + i, 1);
            ++i;
        }
    }
    return de__loc_finish(&writer);
}
