/*
 * ser_internal.h: helpers of the TOML text serializer (DENG-295): output staging, field paths for
 * messages, locale-independent float and ID formatting, and the reader/writer entry points that
 * the text document API shares with de_serialize_object / de_deserialize_object.
 *
 * Private to ser_text_writer.c, ser_text_reader.c and ser_common.c. The binary serializer
 * (DENG-296) does not include it.
 */
#ifndef DE_REFLECT_SER_INTERNAL_H
#define DE_REFLECT_SER_INTERNAL_H

#include "reflect_internal.h"

#include "core/de_toml_internal.h"

DE_EXTERN_C_BEGIN

/* ---- Output staging (ser_common.c) ------------------------------------------------------- */

/** Bytes staged before a de_writer call. */
#define DE__SER_OUT_CAP 4096u

/**
 * Text output in front of a de_writer: appends go to a fixed buffer, which is handed to the
 * writer when it fills and at the end. The first failure is kept (`err`) and later appends do
 * nothing, so callers check once.
 */
typedef struct de__ser_out {
    de_writer *w;
    de_result err;
    size_t len;
    char buf[DE__SER_OUT_CAP];
} de__ser_out;

void de__ser_out_init(de__ser_out *o, de_writer *w);
void de__ser_out_put(de__ser_out *o, const char *s, size_t n);
void de__ser_out_puts(de__ser_out *o, const char *s);
void de__ser_out_putf(de__ser_out *o, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
/** Hands the staged bytes to the writer. Returns the first failure, or DE_OK. */
de_result de__ser_out_flush(de__ser_out *o);

/* ---- Field paths for messages (ser_common.c) --------------------------------------------- */

/** Capacity of a field path ("inventory.items[2].count"); longer paths are cut with "...". */
#define DE__SER_PATH_CAP 256u

typedef struct de__ser_path {
    size_t len;
    char buf[DE__SER_PATH_CAP];
} de__ser_path;

/** Appends ".name" (or "name" at the start). Returns the length to restore with _pop. */
size_t de__ser_path_push_name(de__ser_path *p, const char *name);
/** Appends "[index]". Returns the length to restore with _pop. */
size_t de__ser_path_push_index(de__ser_path *p, u32 index);
void de__ser_path_pop(de__ser_path *p, size_t len);
/** The path, or "(object)" when empty. */
const char *de__ser_path_str(const de__ser_path *p);

/* ---- Numbers and IDs (ser_common.c) ------------------------------------------------------ */

/** Room for any de__ser_format_* result, NUL included. */
#define DE__SER_NUM_CAP 40u

/**
 * The shortest decimal text that reads back as exactly `v` (sign of zero included), in TOML
 * float syntax: always a '.' or an exponent, exponent without '+' or leading zeros ("1e-5"),
 * `nan`, `inf`, `-inf`. Independent of the C locale. Returns the length.
 */
size_t de__ser_format_f64(f64 v, char *buf);
/** Like de__ser_format_f64 for an f32: the shortest text whose f64 value converts to `v`. */
size_t de__ser_format_f32(f32 v, char *buf);
/**
 * Converts like a C cast, but defined for every input: a finite value beyond FLT_MAX that rounds
 * to FLT_MAX gives FLT_MAX; a larger finite value returns false (`*out` untouched).
 */
bool de__ser_f64_to_f32(f64 d, f32 *out);

/** "0x" and 16 lower-case hex digits. Returns the length (18). */
size_t de__ser_format_hex64(u64 v, char *buf);
/** Parses "0x" or "0X" followed by 1 to 16 hex digits, and nothing else. */
bool de__ser_parse_hex64(const char *s, size_t len, u64 *out);

/** The log category "reflect" (registered on first use). */
de_log_category de__ser_log_category(void);

/* ---- Writer (ser_text_writer.c) ---------------------------------------------------------- */

/**
 * Writes one object as TOML lines (the `__type` / `__version` header, then the selected fields)
 * to `o`. `opt` is never NULL. Does not flush.
 */
de_result de__ser_text_write_body(de__ser_out *o, const de_type_registry *r, de_type_id type,
                                  const void *obj, const de_ser_options *opt);

/* ---- Reader (ser_text_reader.c) ---------------------------------------------------------- */

/**
 * Reads one object from TOML table `tab` (a whole document, or one `[[key]]` entry) into `obj`.
 * `opt` is never NULL. `a` is used for the migrate scratch data. `source` names the text in
 * messages.
 */
de_result de__ser_text_read_table(const de_type_registry *r, de_type_id type, void *obj,
                                  const de_ser_options *opt, const toml_datum_t *tab,
                                  const de_allocator *a, const char *source);

DE_EXTERN_C_END

#endif /* DE_REFLECT_SER_INTERNAL_H */
