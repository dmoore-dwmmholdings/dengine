/*
 * fuzz_str_parse: de_str_to_i64 and de_str_to_f64 (DENG-153) on untrusted text.
 *
 * The input is parsed whole, trimmed, and line by line. Besides "no crash, no sanitizer report",
 * every result is checked:
 *   - a failure returns DE_ERR_PARSE, sets the error context and leaves `*out` alone;
 *   - a parsed integer prints (decimal, and hex when non-negative) and parses back to itself;
 *   - a parsed double prints with %.17g and parses back to the same bits (any NaN to a NaN).
 * A broken check aborts, which libFuzzer and CTest report as a crash.
 */
#include "de_fuzz.h"

#include <dengine/de_base.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int ok, const char *what, de_str s) {
    if (ok)
        return;
    fprintf(stderr, "fuzz_str_parse: %s for input \"%.*s\"\n", what,
            (int)(s.len > 200 ? 200 : s.len), s.ptr != NULL ? s.ptr : "");
    abort();
}

static void check_failure(de_result r, de_str s) {
    check(r == DE_ERR_PARSE, "failure is not DE_ERR_PARSE", s);
    const de_error *e = de_error_last();
    check(e != NULL && e->result == DE_ERR_PARSE, "failure without error context", s);
    de_error_clear();
}

static void parse_i64(de_str s) {
    const i64 sentinel = 0x5A5A5A5A5A5A5A5All;
    i64 v = sentinel;
    de_result r = de_str_to_i64(s, &v);
    if (r != DE_OK) {
        check_failure(r, s);
        check(v == sentinel, "i64 output written on failure", s);
        return;
    }
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%" PRId64, v);
    i64 back = 0;
    check(de_str_to_i64((de_str){buf, (size_t)n}, &back) == DE_OK && back == v,
          "i64 decimal round trip", s);
    if (v >= 0) {
        n = snprintf(buf, sizeof buf, "0x%" PRIx64, (u64)v);
        check(de_str_to_i64((de_str){buf, (size_t)n}, &back) == DE_OK && back == v,
              "i64 hex round trip", s);
    }
}

static int is_nan(f64 v) {
    return v != v;
}

static void parse_f64(de_str s) {
    const f64 sentinel = 12345.5;
    f64 v = sentinel;
    de_result r = de_str_to_f64(s, &v);
    if (r != DE_OK) {
        check_failure(r, s);
        check(memcmp(&v, &sentinel, sizeof v) == 0, "f64 output written on failure", s);
        return;
    }
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%.17g", v);
    f64 back = 0;
    check(de_str_to_f64((de_str){buf, (size_t)n}, &back) == DE_OK, "f64 reparse failed", s);
    if (is_nan(v))
        check(is_nan(back), "NaN did not round trip", s);
    else
        check(memcmp(&v, &back, sizeof v) == 0, "f64 round trip changed the bits", s);
}

static void parse_both(de_str s) {
    parse_i64(s);
    parse_f64(s);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const de_str all = {(const char *)data, size};
    parse_both(all);
    parse_both(de_str_trim(all));
    de_str rest = all, line;
    while (de_str_split_next(&rest, '\n', &line))
        parse_both(line);
    de_error_clear();
    return 0;
}
