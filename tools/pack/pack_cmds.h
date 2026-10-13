/*
 * pack_cmds.h: the `de-pack` command line (SPEC §11.7; DENG-213). Tools only.
 *
 *   de-pack list <pak> [--json]               one line (or JSON object) per entry, then bundles
 *   de-pack extract <pak> <sel> [-o <dir>]    writes the raw cooked bytes of matching entries
 *   de-pack verify <pak>                      checks a pack completely
 *
 * `<pak>` is a native path. Packs of every platform are accepted. The pack reader is the runtime
 * one (src/asset/dpak/dpak_reader.h, DENG-212), which checks every offset, count and range at
 * open; `verify` adds the content hash, the decoding of every chunk, the name hashes and the
 * dependency check.
 *
 * `extract` selectors: `0x<hex>` (an asset ID), a glob on asset paths (`*` any run without `/`,
 * `**` any run, `?` one character but `/`; needs a pack with names), or an asset path (its ID is
 * hashed, so it works on stripped packs too). Files are named by the asset path (sub-assets keep
 * their `#`), or by the 16-hex-digit ID when the pack has no name for the entry.
 *
 * Exit codes: 0 ok, 1 problems (verify failed, nothing matched, a pack that does not open),
 * 2 usage or I/O error (bad arguments, a file that cannot be read or written).
 */
#ifndef DE_PACK_CMDS_H
#define DE_PACK_CMDS_H

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Exit codes of `de-pack`. */
enum { DE_PACK_EXIT_OK = 0, DE_PACK_EXIT_PROBLEMS = 1, DE_PACK_EXIT_USAGE = 2 };

/** Output streams for de_pack_io.write. */
typedef enum de_pack_stream {
    DE_PACK_STDOUT = 1, /**< listings, verify results, extracted files */
    DE_PACK_STDERR = 2  /**< usage text and fatal errors */
} de_pack_stream;

/** Where the tool writes. main.c writes to stdout and stderr; tests collect into buffers. */
typedef struct de_pack_io {
    void (*write)(void *user, de_pack_stream stream, const char *text, size_t len);
    void *user;
} de_pack_io;

/**
 * Runs `de-pack` with its arguments: `argv[0]` is the command (`list`, `extract`, `verify`,
 * `-h`/`--help`); `argc` may be 0 (usage error). Writes through `io`. Mounts the VFS roots
 * `depak` and `depakout` for the run (not reentrant).
 * @return a DE_PACK_EXIT_* code.
 */
int de_pack_main(int argc, const char *const *argv, const de_pack_io *io);

/** Writes the usage text to `stream`. */
void de_pack_usage(const de_pack_io *io, de_pack_stream stream);

DE_EXTERN_C_END

#endif /* DE_PACK_CMDS_H */
