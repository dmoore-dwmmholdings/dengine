# third_party: vendored libraries

DEngine builds from this repository alone. Every dependency is vendored here, pinned to an exact
upstream version, and ships with its licence (SPEC §22, CONVENTIONS §6).

## Policy

1. **Allowed libraries only.** A library must be listed in the SPEC §22 table, or added on the
   DENG-2 decisions issue. Its licence must be one that §22 lists. `tools/vendor/vendor.py` has the
   allowlist of SPDX expressions and refuses any other licence. To allow a new licence, first add
   the library to §22 or DENG-2, then extend `ALLOWED_LICENSES` in `vendor.py`.
2. **Never vendor or download proprietary SDKs.** This includes the Steamworks SDK, the Windows
   SDK and CRT, and the D3D12 Agility SDK. CMake finds them through a path that the user sets.
   `vendor.py` refuses entries with these names or URLs.
3. **One directory per library**: `third_party/<name>/`. `<name>` is lower_snake_case and is also
   the CMake target suffix: `de_tp_<name>`.
4. **Every library is in `vendor.toml`.** Each entry pins the archive URL (a release tarball, or a
   git archive at a tag or commit, never a branch), its sha256, the licence, and the files taken
   from the archive. `vendor.py verify` fails on a directory that `vendor.toml` does not list.
5. **One version per library.** To take more files from a library, extend the `files` list of its
   entry. Never add a second entry, and never vendor a second copy at a different version. For
   example, `stb_image_write.h` (S10) and `stb_truetype.h` (S28) extend the `stb` entry.
6. **LICENSE is copied verbatim.** `vendor.py` copies the upstream licence file to
   `third_party/<name>/LICENSE`. Do not edit it.
7. **No local edits to upstream files.** `vendor.py verify` compares each tree with its pinned
   archive byte for byte, so any edit fails CI. If a change is unavoidable (prefer none: configure
   with macros from the wrapper TU or the CMake target instead), put a patch file in
   `third_party/<name>/patches/`, list it in the entry's `patches`, and give the reason in a
   comment in `vendor.toml`.
8. **C++ libraries only behind a C interface inside one module** (SPEC §4.2, DENG-2 Q1). The
   `.cpp` shim lives in that module's `src/` directory. No third-party type appears in a public
   header.

## Layout of a library directory

```
third_party/<name>/
  LICENSE            upstream licence (written by vendor.py)
  <upstream files>   exactly the archive files selected by `files` (written by vendor.py)
  CMakeLists.txt     engine-owned: defines de_tp_<name>
  de_*.c, de_*.h     engine-owned: implementation TUs and thin include wrappers
  smoke/             engine-owned: smoke test (de_tp_smoke_<name>)
  patches/           engine-owned: local patches, if any
```

The engine-owned files match the entry's `local` globs (default `CMakeLists.txt`, `de_*`,
`smoke/**`, `patches/**`). `vendor.py` never touches them. Every other file must come from the
archive, except the archive's `.gitignore` and `.gitattributes` files, which are never vendored.
Every vendored file is committed: `vendor.py` fails when git ignores one (DENG-362). The
`third_party/.gitattributes` file turns off line-ending conversion here, so that checkouts stay
byte-identical on Windows too.

## CMake rules

`third_party/CMakeLists.txt` adds every `<name>/CMakeLists.txt` automatically. Each one defines:

- `de_tp_<name>`: a STATIC library. Header-only libraries get one implementation TU
  (`de_tp_<name>.c`), so that the implementation is compiled once.
- `SYSTEM` include directories, so warnings from vendored headers never fail `-Werror` in engine
  targets that include them.
- No `de_build_flags`. Instead, warnings are turned off for that target only (`-w`, or `/w` for
  MSVC and clang-cl).
- `POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}`, because the shared `dengine` library links it.
- A smoke test registered with `de_add_test(NAME de_tp_smoke_<name> ...)`, built with the engine's
  strict flags. This proves that the public headers compile cleanly in engine code.

Engine modules link these targets through `de_add_module(... LIBS de_tp_<name>)`.

## Libraries

| Library | Version | Licence | Target | Include | Notes |
|---|---|---|---|---|---|
| xxHash | 0.8.4 | BSD-2-Clause | `de_tp_xxhash` | `<xxhash.h>` | The library compiles `XXH_IMPLEMENTATION` once. For inlining, `#define XXH_INLINE_ALL` before the include. |
| zstd | 1.5.7 | BSD-3-Clause | `de_tp_zstd`, `de_tp_zstd_dict` | `<zstd.h>`, `<zdict.h>` | common + compress + decompress. No legacy formats, no multithreading, no asm. Internal xxHash is namespaced `ZSTD_`. The dictionary builder (`lib/dictBuilder`) is the separate tools-only target `de_tp_zstd_dict` (DENG-211); the runtime never links it. |
| tomlc17 | R261003 | MIT | `de_tp_tomlc17` | `<tomlc17.h>` | TOML parser. Its allocator hooks are process-global (`toml_set_option`). |
| stb | 2026-08-02-2c980bb | MIT OR Unlicense | `de_tp_stb` | `<stb_image.h>`, `<de_stb_vorbis.h>` | `stb_image.h` and `stb_vorbis.c`, with one implementation TU each. |
| SDL3 | 3.4.18 | Zlib | `de_tp_sdl3` | `<SDL3/SDL.h>` (only in `src/platform/sdl/`) | Built by SDL's own CMake from `sdl3/SDL3-3.4.18/` (`strip = 0`), static, without audio, GPU, renderer, camera, OpenGL, tray, tests and examples. Not built with `DE_HEADLESS`. |
| Tracy | 0.14.1 | BSD-3-Clause | `de_tp_tracy` | `<tracy/TracyC.h>` (only in `src/core/prof/prof_tracy.cpp`) | Profiler client, C++ behind the engine's C API (`de_debug.h`). `public/` only. Built from `public/TracyClient.cpp` with `TRACY_ON_DEMAND`, `TRACY_MANUAL_LIFETIME`, `TRACY_NO_FRAME_IMAGE`, `TRACY_NO_CRASH_HANDLER`; only for Dev and profiling Release builds, never Debug or Shipping. The profiler GUI must be 0.14.x. |
| Monocypher | 4.0.3 | BSD-2-Clause OR CC0-1.0 | `de_tp_monocypher` | `<monocypher.h>` (only in `src/net/crypto.c`) | Network crypto: XChaCha20-Poly1305, X25519, BLAKE2b (DENG-52). `src/monocypher.{c,h}` and upstream's TIS-CI test-vector subset `tests/tis-ci-vectors.h` (used by the net unit tests). The optional Ed25519 file is not vendored. |

## Adding or updating a library

See `tools/vendor/README.md`. In short:

```sh
python3 tools/vendor/vendor.py hash <archive-url>   # sha256 for the new entry
$EDITOR third_party/vendor.toml                     # append or edit the [[lib]] table
python3 tools/vendor/vendor.py fetch <name>         # write third_party/<name>/
python3 tools/vendor/vendor.py verify               # all trees match their archives
```

Then write `third_party/<name>/CMakeLists.txt` and a smoke test, and add a row to the table
above.
