/* DENG-664: single-source shader inspection. Tools only; no project cook or source writes. */
#ifndef DE_COOK_SHADER_CMD_H
#define DE_COOK_SHADER_CMD_H
#include "check.h"

DE_EXTERN_C_BEGIN

#define DE_COOK_SHADER_MAX_ARGS 32u
typedef struct de_cook_shader_options {
    const char *source, *target, *code, *reflection, *report;
    const char *includes[DE_COOK_SHADER_MAX_ARGS];
    const char *defines[DE_COOK_SHADER_MAX_ARGS];
    const char *entries[DE_COOK_SHADER_MAX_ARGS];
    u32 include_count, define_count, entry_count;
    bool help;
} de_cook_shader_options;

/* Pure parsers, also exercised by the fuzz harness. argv strings remain caller-owned. */
bool de_cook_shader_parse(int argc, const char *const *argv, de_cook_shader_options *out,
                          char error[256]);
typedef struct de_cook_shader_diagnostic {
    char source[2048];
    u32 line, column; /**< 1-based compiler positions; 0 means unavailable. */
    u32 end_column;   /**< Exclusive underline end, or 0 when the compiler supplies only a point. */
    const char *severity, *message; /**< severity is static; message borrows the input line. */
} de_cook_shader_diagnostic;
bool de_cook_shader_diagnostic_parse(const char *line, de_cook_shader_diagnostic *out);

/** `de-cook shader <source> --target spirv|dxil|metal --code <new-file>
 * --reflection <new-file> --report <new-file> [-I <dir>] [-D NAME[=VALUE]] [--entry <name>].
 * Outputs must not exist. Code is disassembly (SPIR-V/DXIL) or MSL; reflection is full Slang JSON
 * per entry. Report schema 1 records status and bounded raw/structured diagnostics on compile
 * success/failure. Locations/ranges use 1-based compiler coordinates, with exclusive ends;
 * where no underline extent is available, a range is a point. Inputs: 16 MiB per file,
 * 256 files/64 MiB total; code plus reflection: 16 MiB, at most 64 entry points.
 * Exit 0 success, 1 compile error, 2 usage/I/O error, 3 unsupported target. */
int de_cook_shader_main(int argc, const char *const *argv, const de_cook_io *io);

DE_EXTERN_C_END
#endif
