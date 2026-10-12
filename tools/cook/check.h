/*
 * check.h: `de-cook --check`, the manifest validator (SPEC §11.2, §11.5; DENG-145). Tools only:
 * the runtime never links it.
 *
 *   de-cook --check <assets.toml> [--types a,b,c] [--no-sources] [--quiet] [-I <dir>]...
 *
 * It parses the manifest and its includes, resolves it and validates it (DENG-138, DENG-139:
 * de__manifest_parse, de__manifest_resolve, de__manifest_validate), then prints one line per
 * issue:
 *
 *   <file>:<line or 0>: <error|warning|note>: <message> [<key path>]
 *
 * sorted by file (virtual path, byte order), then line, then the order the issues were found;
 * then a summary line `de-cook --check: <n> errors, <n> warnings, <n> notes`. `[<key path>]` is
 * left out when the issue is about the whole file. `<file>` is the manifest argument's directory
 * as given on the command line, joined with the file's path below it, so the output does not
 * depend on the machine when the argument is relative.
 *
 * Exit codes: 0 no errors (warnings allowed), 1 errors found, 2 usage or I/O error.
 *
 * The code is a library (`de_cook_check`) so tests run it in-process; tools/cook/main.c is a thin
 * front end. S08 (DENG-213) adds the cook and pack commands next to it.
 */
#ifndef DE_COOK_CHECK_H
#define DE_COOK_CHECK_H

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Exit codes of `de-cook`. */
enum {
    DE_COOK_EXIT_OK = 0,     /**< no errors (warnings and notes allowed) */
    DE_COOK_EXIT_ERRORS = 1, /**< the manifest has at least one error issue */
    DE_COOK_EXIT_USAGE = 2   /**< bad arguments, or the manifest could not be read */
};

/** Output streams for de_cook_io.write. */
typedef enum de_cook_stream {
    DE_COOK_STDOUT = 1, /**< issue lines and the summary */
    DE_COOK_STDERR = 2  /**< usage text and fatal errors */
} de_cook_stream;

/** Where the tool writes. main.c writes to stdout and stderr; tests collect into a buffer. */
typedef struct de_cook_io {
    void (*write)(void *user, de_cook_stream stream, const char *text, size_t len);
    void *user;
} de_cook_io;

/** The engine asset type names `--check` knows before S08's importer registry exists. */
extern const char *const de_cook_known_types[];
extern const u32 de_cook_known_type_count;

/**
 * Runs `de-cook --check` with the arguments that follow `--check` (`argv[0]` is the manifest or
 * the first option; `argc` may be 0). Writes through `io`.
 * @return a DE_COOK_EXIT_* code.
 */
int de_cook_check_main(int argc, const char *const *argv, const de_cook_io *io);

/** Writes the usage text of `--check` to `stream`. */
void de_cook_check_usage(const de_cook_io *io, de_cook_stream stream);

DE_EXTERN_C_END

#endif /* DE_COOK_CHECK_H */
