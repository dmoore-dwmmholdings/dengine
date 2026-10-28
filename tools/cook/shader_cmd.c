#define _CRT_SECURE_NO_WARNINGS
#include "shader_cmd.h"
#include "cook/importers/shader/slang_compile.h"
#include "gpu/shader/shader_format.h"
#include <dengine/de_platform.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern const de__slang_module de__shader_common_modules[];
extern const u32 de__shader_common_module_count;
#define FILE_LIMIT (16u * 1024u * 1024u)
#define READ_LIMIT (64u * 1024u * 1024u)
#define FILE_COUNT 256u
typedef struct shader_file {
    char requested[DE_FS_PATH_MAX], physical[DE_FS_PATH_MAX];
    char *bytes;
    u64 size;
} shader_file;
typedef struct shader_reader {
    const de_cook_shader_options *options;
    char directory[DE_FS_PATH_MAX];
    shader_file files[FILE_COUNT];
    u32 count;
    u64 total;
} shader_reader;

#if defined(_WIN32)
static bool wide_path(const char *path, wchar_t out[DE_FS_PATH_MAX]) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, DE_FS_PATH_MAX) != 0;
}
#endif
static FILE *native_open(const char *path, const char *mode) {
#if defined(_WIN32)
    wchar_t wpath[DE_FS_PATH_MAX], wmode[8];
    if (!wide_path(path, wpath))
        return NULL;
    size_t i = 0;
    for (; mode[i] && i < 7; ++i)
        wmode[i] = (wchar_t)mode[i];
    wmode[i] = 0;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}
static void native_remove(const char *path) {
#if defined(_WIN32)
    wchar_t wpath[DE_FS_PATH_MAX];
    if (wide_path(path, wpath))
        (void)_wremove(wpath);
#else
    (void)remove(path);
#endif
}
static bool absolute_path(const char *s) {
    return s[0] == '/' || s[0] == '\\' || (s[0] && s[1] == ':');
}
static de_result load_file(shader_reader *r, const char *request, const char *path,
                           const void **data, u64 *size) {
    if (r->count == FILE_COUNT || strlen(path) >= DE_FS_PATH_MAX ||
        strlen(request) >= DE_FS_PATH_MAX)
        return DE_ERR_INVALID_ARGUMENT;
    FILE *f = native_open(path, "rb");
    if (!f)
        return errno == ENOENT || errno == ENOTDIR ? DE_ERR_NOT_FOUND : DE_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return DE_ERR_IO;
    }
    const long length = ftell(f);
    if (length < 0 || (u64)length > FILE_LIMIT || (u64)length > READ_LIMIT - r->total ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return DE_ERR_INVALID_ARGUMENT;
    }
    char *bytes = DE_ALLOC(de_heap_allocator(), (u64)length + 1u, 1, DE_MEM_ASSET);
    if (!bytes) {
        fclose(f);
        return DE_ERR_OUT_OF_MEMORY;
    }
    const bool read = fread(bytes, 1, (size_t)length, f) == (size_t)length && !ferror(f);
    const int closed = fclose(f);
    if (!read || closed) {
        DE_FREE(de_heap_allocator(), bytes, (u64)length + 1u);
        return DE_ERR_IO;
    }
    bytes[length] = '\0';
    shader_file *entry = &r->files[r->count++];
    snprintf(entry->requested, sizeof entry->requested, "%s", request);
    snprintf(entry->physical, sizeof entry->physical, "%s", path);
    entry->bytes = bytes;
    entry->size = (u64)length;
    r->total += (u64)length;
    *data = bytes;
    *size = (u64)length;
    return DE_OK;
}
static de_result read_import(void *user, const char *path, const void **data, u64 *size) {
    shader_reader *r = user;
    for (u32 i = 0; i < r->count; ++i)
        if (!strcmp(r->files[i].requested, path)) {
            *data = r->files[i].bytes;
            *size = r->files[i].size;
            return DE_OK;
        }
    if (absolute_path(path))
        return load_file(r, path, path, data, size);
    for (u32 i = 0; i <= r->options->include_count; ++i) {
        const char *root = i ? r->options->includes[i - 1] : r->directory;
        char joined[DE_FS_PATH_MAX];
        const int n = snprintf(joined, sizeof joined, "%s/%s", root, path);
        if (n < 0 || (size_t)n >= sizeof joined)
            return DE_ERR_INVALID_ARGUMENT;
        const de_result result = load_file(r, path, joined, data, size);
        if (result != DE_ERR_NOT_FOUND)
            return result;
    }
    return DE_ERR_NOT_FOUND;
}
/* JSON strings preserve valid UTF-8; hostile/invalid bytes become replacement characters. */
static void json_string(FILE *f, const char *text) {
    fputc('"', f);
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        const unsigned c = *p;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
            ++p;
        } else if (c < 32) {
            fprintf(f, "\\u%04x", c);
            ++p;
        } else if (c < 128)
            fputc(*p++, f);
        else {
            const u32 n = c >= 0xc2 && c <= 0xdf   ? 2
                          : c >= 0xe0 && c <= 0xef ? 3
                          : c >= 0xf0 && c <= 0xf4 ? 4
                                                   : 0;
            bool valid = n != 0;
            for (u32 i = 1; valid && i < n; ++i)
                valid = p[i] >= 0x80 && p[i] <= 0xbf;
            if (valid && n >= 3)
                valid = !(c == 0xe0 && p[1] < 0xa0) && !(c == 0xed && p[1] >= 0xa0);
            if (valid && n == 4)
                valid = !(c == 0xf0 && p[1] < 0x90) && !(c == 0xf4 && p[1] >= 0x90);
            if (valid) {
                fwrite(p, 1, n, f);
                p += n;
            } else {
                fputs("\\ufffd", f);
                ++p;
            }
        }
    }
    fputc('"', f);
}
static const char *physical_source(const shader_reader *r, const char *path) {
    for (u32 i = 0; i < r->count; ++i)
        if (!strcmp(r->files[i].requested, path))
            return r->files[i].physical;
    return path;
}
static bool write_report(FILE *f, const de_cook_shader_options *o, const shader_reader *r,
                         const char *status, const char *diagnostics, bool truncated) {
    fputs("{\"schema\":1,\"status\":", f);
    json_string(f, status);
    fputs(",\"source\":", f);
    json_string(f, o->source);
    fputs(",\"target\":", f);
    json_string(f, o->target);
    fputs(",\"compiler\":", f);
    json_string(f, de__slang_version());
    fputs(",\"rawDiagnostics\":", f);
    json_string(f, diagnostics);
    fprintf(f, ",\"diagnosticsTruncated\":%s", truncated ? "true" : "false");
    fputs(",\"diagnostics\":[", f);
    bool comma = false;
    const char *at = diagnostics;
    while (*at) {
        const char *end = strchr(at, '\n');
        char line[2048];
        /* Rich source positions follow the heading; only the heading becomes the message. */
        const size_t remaining = strlen(at);
        const size_t take = remaining < sizeof line - 1 ? remaining : sizeof line - 1;
        memcpy(line, at, take);
        line[take] = '\0';
        de_cook_shader_diagnostic d;
        if (de_cook_shader_diagnostic_parse(line, &d)) {
            char *message_end = strchr((char *)d.message, '\n');
            if (message_end)
                *message_end = '\0';
            if (comma)
                fputc(',', f);
            comma = true;
            fputs("{\"severity\":", f);
            json_string(f, d.severity);
            fputs(",\"message\":", f);
            json_string(f, d.message);
            fputs(",\"source\":", f);
            json_string(f, d.source[0] ? physical_source(r, d.source) : o->source);
            if (d.line)
                fprintf(f, ",\"line\":%u", d.line);
            if (d.column)
                fprintf(f, ",\"column\":%u", d.column);
            if (d.line) {
                const u32 column = d.column ? d.column : 1;
                const u32 end_column = d.end_column ? d.end_column : column;
                fprintf(f,
                        ",\"range\":{\"start\":{\"line\":%u,\"column\":%u},\"end\":{\"line\":%u,"
                        "\"column\":%u}}",
                        d.line, column, d.line, end_column);
            }
            fputc('}', f);
        }
        if (!end)
            break;
        at = end + 1;
    }
    if (!comma && strcmp(status, "ok")) {
        fputs("{\"severity\":\"error\",\"source\":", f);
        json_string(f, o->source);
        fputs(",\"message\":", f);
        json_string(f, diagnostics);
        fputc('}', f);
    }
    fputs("]}\n", f);
    return !ferror(f);
}
static bool write_outputs(const de_cook_shader_options *o, const de__slang_compile_result *result) {
    FILE *code = native_open(o->code, "wbx");
    if (!code)
        return false;
    FILE *reflection = native_open(o->reflection, "wbx");
    bool ok = reflection != NULL;
    if (ok) {
        fputs("{\"schema\":1,\"target\":", reflection);
        json_string(reflection, o->target);
        fputs(",\"entries\":[", reflection);
        for (u32 i = 0; i < result->entry_count; ++i) {
            const de__slang_entry_out *entry = &result->entries[i];
            fprintf(code, "%s entry: %s\n", !strcmp(o->target, "metal") ? "//" : ";",
                    result->entry_names[i]);
            fwrite(entry->code, 1, (size_t)entry->code_size, code);
            fputc('\n', code);
            if (i)
                fputc(',', reflection);
            fputs("{\"name\":", reflection);
            json_string(reflection, result->entry_names[i]);
            fprintf(reflection,
                    ",\"stage\":%u,\"threadgroup\":[%u,%u,%u],\"pushConstantBytes\":%u,\"layout\":",
                    entry->stage, entry->threadgroup[0], entry->threadgroup[1],
                    entry->threadgroup[2], entry->push_constant_bytes);
            fwrite(entry->reflection_json, 1, (size_t)entry->reflection_size, reflection);
            fputc('}', reflection);
        }
        fputs("]}\n", reflection);
        ok = !ferror(code) && !ferror(reflection);
    }
    if (fclose(code))
        ok = false;
    if (reflection && fclose(reflection))
        ok = false;
    if (!ok) {
        native_remove(o->code);
        if (reflection)
            native_remove(o->reflection);
    }
    return ok;
}
int de_cook_shader_main(int argc, const char *const *argv, const de_cook_io *io) {
    de_cook_shader_options options;
    char error[256];
    if (!de_cook_shader_parse(argc, argv, &options, error)) {
        io->write(io->user, DE_COOK_STDERR, error, strlen(error));
        io->write(io->user, DE_COOK_STDERR, "\n", 1);
        return DE_COOK_EXIT_USAGE;
    }
    if (options.help) {
        const char *help = "usage: de-cook shader <source> --target spirv|dxil|metal --code "
                           "<new-file> --reflection <new-file> --report <new-file> [-I <dir>] [-D "
                           "NAME[=VALUE]] [--entry <name>]\nOutputs must not exist. Exit: 0 "
                           "success, 1 compile error, 2 usage/I/O error, 3 unsupported.\n";
        io->write(io->user, DE_COOK_STDOUT, help, strlen(help));
        return 0;
    }
    FILE *report = native_open(options.report, "wbx");
    if (!report) {
        const char *message = "de-cook shader: report must be a new writable file\n";
        io->write(io->user, DE_COOK_STDERR, message, strlen(message));
        return 2;
    }
    shader_reader *reader =
        DE_ALLOC(de_heap_allocator(), sizeof *reader, DE_ALIGNOF(shader_reader), DE_MEM_ASSET);
    if (!reader) {
        fclose(report);
        native_remove(options.report);
        return 2;
    }
    memset(reader, 0, sizeof *reader);
    reader->options = &options;
    char source_path[DE_FS_PATH_MAX];
    snprintf(source_path, sizeof source_path, "%s", options.source);
    for (char *p = source_path; *p; ++p)
        if (*p == '\\')
            *p = '/';
    char *base = strrchr(source_path, '/');
    const char *source_name = base ? base + 1 : source_path;
    if (base) {
        const size_t n = (size_t)(base - source_path);
        memcpy(reader->directory, source_path, n);
        reader->directory[n] = '\0';
        if (!n)
            strcpy(reader->directory, "/");
    } else
        strcpy(reader->directory, ".");
    const void *source = NULL;
    u64 size = 0;
    const de_result read = load_file(reader, source_name, options.source, &source, &size);
    const char *status = "ioError";
    de__slang_compile_result result = {0};
    int rc = 2;
    if (read != DE_OK)
        snprintf(result.diagnostics, sizeof result.diagnostics,
                 "error: source could not be read within inspection limits");
    else {
        de__slang_entry entries[DE_COOK_SHADER_MAX_ARGS];
        for (u32 i = 0; i < options.entry_count; ++i)
            entries[i] = (de__slang_entry){options.entries[i], DE__SHADER_STAGE_NONE};
        const de__slang_compile_desc desc = {
            .target = !strcmp(options.target, "spirv")  ? DE__SLANG_SPIRV_ASM
                      : !strcmp(options.target, "dxil") ? DE__SLANG_DXIL_ASM
                                                        : DE__SLANG_METAL,
            .path = source_name,
            .source = source,
            .source_size = size,
            .defines = options.defines,
            .define_count = options.define_count,
            .modules = de__shader_common_modules,
            .module_count = de__shader_common_module_count,
            .preload_count = 1,
            .read = read_import,
            .read_user = reader,
            .entries = entries,
            .entry_count = options.entry_count,
            .allocator = de_heap_allocator(),
            .reflection_json = true,
            .max_entries = 64,
            .max_output_bytes = FILE_LIMIT};
        const de_result compiled = de__slang_compile(&desc, &result);
        status = compiled == DE_OK                ? "ok"
                 : compiled == DE_ERR_UNSUPPORTED ? "unsupported"
                                                  : "compileError";
        rc = compiled == DE_OK ? 0 : compiled == DE_ERR_UNSUPPORTED ? 3 : 1;
        if (compiled == DE_OK && !write_outputs(&options, &result)) {
            status = "ioError";
            rc = 2;
            snprintf(result.diagnostics, sizeof result.diagnostics,
                     "error: code/reflection outputs must be new writable files");
        }
    }
    if (result.diagnostics[0])
        io->write(io->user, rc ? DE_COOK_STDERR : DE_COOK_STDOUT, result.diagnostics,
                  strlen(result.diagnostics));
    const bool written = write_report(report, &options, reader, status, result.diagnostics,
                                      result.diagnostics_truncated);
    const int closed = fclose(report);
    if (!written || closed)
        rc = 2;
    de__slang_compile_result_free(&result);
    for (u32 i = 0; i < reader->count; ++i)
        DE_FREE(de_heap_allocator(), reader->files[i].bytes, reader->files[i].size + 1u);
    DE_FREE(de_heap_allocator(), reader, sizeof *reader);
    return rc;
}
