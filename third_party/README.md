# third_party: vendored libraries

DEngine builds from this repository alone. Every dependency is vendored here, pinned to an exact
upstream version, and ships with its licence (SPEC §22, CONVENTIONS §6).

**The one exception is prebuilt tool binaries** (policy rule 9). A library that ships only as
large per-platform binaries, such as the Slang shader compiler (about 38 MB per platform), is
pinned here by URL and sha256 for each host, but its binaries are not committed. The build
downloads the archive for its host once into the vendor cache, checks the hash, and extracts the
files into the build directory. The headers and the licence of such a library are still committed
here. See "Prebuilt binaries" below.

## Policy

1. **Allowed libraries only.** A library must be listed in the SPEC §22 table, or added on the
   DENG-2 decisions issue. Its licence must be one that §22 lists. `tools/vendor/vendor.py` has the
   allowlist of SPDX expressions and refuses any other licence. To allow a new licence, first add
   the library to §22 or DENG-2, then extend `ALLOWED_LICENSES` in `vendor.py`.
2. **Never vendor or download proprietary SDKs.** This includes the Steamworks SDK and the Windows
   SDK and CRT. CMake finds them through a path that the user sets. `vendor.py` refuses entries
   with these names or URLs. The D3D12 Agility SDK is not such an SDK: SPEC §22 lists it as a
   Microsoft redistributable. Its headers are MIT and committed, and its runtime DLLs are a
   prebuilt entry (rule 9) that is never committed (DENG-285). Microsoft Direct3D WARP is a
   test-only binary, not an SDK and not a redistributable (rule 10, DENG-405).
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
9. **Prebuilt binaries are pinned, not committed.** Use a `kind = "prebuilt"` entry only for a
   library on the §22 list that upstream ships as per-platform binaries and that the engine does
   not build from source (today: Slang, DENG-289, and the D3D12 Agility SDK, DENG-285). The entry
   pins one archive per host (`macos-arm64`, `linux-x64`, `windows-x64`) by URL and sha256. Its
   headers and `LICENSE` are committed like any other library; the binaries are never committed,
   and a binary under `third_party/<name>/` fails `vendor.py verify`. Rules 1, 2, 5 and 6 apply
   unchanged: one entry per library (all hosts in the same entry), one version, an allowed
   licence, no proprietary SDKs. An entry may list only the hosts that run the binaries (the
   Agility SDK: `windows-x64`).
10. **Test-only binaries stay in the tests.** A binary whose licence allows use for testing but
    forbids sharing or redistributing it may be pinned only as a `prebuilt` entry, with its
    licence in `TEST_ONLY_LICENSES` in `vendor.py` (`vendor.py` refuses such a licence on any
    other kind of entry). Only the licence text is committed. The binary is extracted only when
    the tests are built, and only test executables get a copy: never `src/host/`, a game
    (`cmake/DeAddGame.cmake`), a tool or a package. CI never uploads it as an artifact. Today
    this is Microsoft Direct3D WARP (`d3d12_warp`, DENG-405).

    *Decision (DENG-405):* the `Microsoft.Direct3D.WARP` licence (`d3d12_warp/LICENSE.TXT`) says
    "for testing purposes only" and lets you "install and use any number of copies of the software
    to develop and test your applications". Its §3(e) forbids to "share, publish, distribute, or
    lease the software". So, unlike the Agility SDK, WARP is not a redistributable. The engine
    uses it only to run the D3D12 tests on machines with no GPU (CI). Each build downloads it
    from nuget.org into its own vendor cache, so the repository never distributes it.

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

## Prebuilt binaries

A prebuilt entry is a normal `[[lib]]` table with `kind = "prebuilt"`, followed by one
`[lib.hosts.<host>]` table per host:

```toml
[[lib]]
name = "<name>"
kind = "prebuilt"
version = "<version>"
url = "<one host's archive>"        # the committed part: headers and LICENSE
sha256 = "<sha256>"
license = "<SPDX>"
strip = 0
files = ["include/*.h"]

[lib.hosts.linux-x64]
url = "<linux archive>"
sha256 = "<sha256>"
strip = 0
files = ["lib/lib<name>*.so*"]      # what is extracted into the build directory

[lib.hosts.macos-arm64]             # and windows-x64
...
```

- `vendor.py fetch <name>` writes the committed part into `third_party/<name>/` and downloads
  this host's archive into the vendor cache (`$DE_VENDOR_CACHE`, or `~/.cache/dengine-vendor`).
  `--all-hosts` downloads every host's archive.
- `vendor.py verify` checks the committed part byte for byte and this host's archive (sha256 and
  `files`). `verify --all-hosts`, which the `Checks` CI job runs, checks every host's archive.
- At configure time, `de_prebuilt_import(<name>)` (`cmake/DePrebuilt.cmake`) extracts the target
  host's files into `<build>/prebuilt/<name>/`. It downloads the archive first if the cache does
  not have it, and fails the configure on a hash mismatch. It extracts again only when the pin
  changes or a file is missing. The library's `CMakeLists.txt` calls it only when a target that
  is built needs the library, so other builds (Shipping, for example) download nothing.
- A configure never writes into `third_party/`, also for prebuilt entries.

The commands and the details are in `tools/vendor/README.md`.

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
| cgltf | 1.15 | MIT | `de_tp_cgltf` | `<cgltf.h>` (only in `tools/cook/importers/model/`) | glTF 2.0 parser of the model importer (DENG-101). Tools only: linked into the cooker library, never into Release or Shipping. `cgltf.h` only (no `cgltf_write.h`); one implementation TU. |
| MikkTSpace | 2020-03-25-3e895b4 | Zlib | `de_tp_mikktspace` | `<mikktspace.h>` (only in `tools/cook/importers/model/`) | Tangent generation of the model importer (DENG-101). Tools only. Upstream has no releases: pinned to its last commit. The archive has no licence file; the zlib licence is the header comment of `mikktspace.h`, which is copied to `LICENSE`. Not in the SPEC §22 table: added by DENG-101 (zlib, tools only); see "SPEC §22 additions" below. |
| Slang | 2026.19 | Apache-2.0 WITH LLVM-exception | `de_tp_slang` (INTERFACE) | `<slang.h>` (only in `tools/cook/importers/shader/slang_compile.cpp`) | Shader compiler of the shader importer (DENG-289). A **prebuilt** entry: the headers and `LICENSE` are committed; the host's compiler and glslang libraries are extracted into `<build>/prebuilt/slang/` at configure time (`DE_BUILD_TOOLS` only). The importer opens the library at run time (`DE_SLANG_LIBRARY`); nothing links it. C++ behind a C shim. |
| ufbx | 0.23.1 | MIT OR Unlicense | `de_tp_ufbx` | `<ufbx.h>` (only in `tools/cook/importers/model/fbx_*.c`) | FBX loader of the model importer (DENG-102). Tools only: linked into the cooker library, never into Release or Shipping. `ufbx.c` + `ufbx.h`, upstream's default configuration (double precision, libc math, no threads). Loads from memory only; the importer forces the FBX format, so ufbx's OBJ reader is never used. |
| tinyexr | 1.0.13 | BSD-3-Clause | `de_tp_tinyexr` | `<tinyexr.h>` (only in `tools/cook/importers/texture/`) | OpenEXR decoder of the texture importer (DENG-98). Tools only, like cgltf. C++ implementation in one TU (`de_tp_tinyexr.cc`); the declaration part of `tinyexr.h` is C, so the importer calls it from C (DENG-2 Q1). `tinyexr.h`, `exr_reader.hh`, `streamreader.hh` only: inflate goes through stb_image (`TINYEXR_USE_STB_ZLIB`), so miniz is not vendored, and the zlib compressor is a failing stub (writing EXR is not supported). No threads (`TINYEXR_USE_THREAD=0`). SPEC §22 listed it as a candidate (`?`); DENG-98 adopted it. |
| Basis Universal | 2_50 | Apache-2.0 | `de_tp_basisu` | `<basisu_transcoder.h>` (C++, only in `tools/cook/importers/texture/texture_basis.cpp`) | The transcoder only (`transcoder/`, 26 files, plus `NOTICE`): the texture importer transcodes KTX2 ETC1S and UASTC payloads at cook time (DENG-98). Tools only. Chosen over KTX-Software/libktx, which bundles the same transcoder; the importer reads KTX2 containers itself. KTX2 zstd supercompression uses the vendored zstd through the engine-owned forwarder `zstd/zstd.h`. Built with `-fno-strict-aliasing` (upstream requires it) and without the mobile targets (PVRTC, ATC, FXT1, ETC2 EAC). |
| meshoptimizer | 1.3 | MIT | `de_tp_meshoptimizer` | `<meshoptimizer.h>` (only in `tools/cook/importers/model/`) | Mesh optimization, meshlets and LOD simplification of the model importer (DENG-103). C++ (`src/*.cpp`) called only through its C API (DENG-2 Q1). Tools only: linked into the cooker library, never into Release or Shipping. `src/` only (no gltfpack, demo or js); release tarball. Built without FMA contraction. Its temporary memory comes from `operator new` (`meshopt_setAllocator` is process-wide, so the engine leaves it alone). |
| Vulkan-Headers | vulkan-sdk-1.4.363.0 | Apache-2.0 (dual Apache-2.0 OR MIT; we take Apache-2.0) | `de_tp_vulkan_headers` (INTERFACE) | `<vulkan/vulkan.h>` (only in `src/gpu/vulkan/`) | Khronos C headers for the Vulkan RHI backend (DENG-283), with `VK_NO_PROTOTYPES`: nothing links a loader. The backend opens `libvulkan` at run time (`src/gpu/vulkan/vk_loader.c`), so no Vulkan SDK is needed to build. `include/vulkan/*.h` and `include/vk_video/*.h`; not the C++ bindings or the registry. |
| D3D12 Agility SDK | 1.619.6 | MIT (headers); Microsoft DirectX terms (DLLs, `LICENSE.txt`) | `de_tp_d3d12_agility` (INTERFACE) | `<d3d12.h>` (only in `src/gpu/d3d12/`) | The D3D12 runtime (SPEC §3.1, §22: Microsoft redistributable), DENG-285. A **prebuilt** entry (`d3d12_agility`, the NuGet package): the four headers the backend uses, `LICENSE` and `LICENSE.txt` are committed; `D3D12Core.dll` and `d3d12SDKLayers.dll` (host `windows-x64` only) are extracted at configure time when the D3D12 backend is built, and `de_d3d12_agility_deploy(<target>)` copies them into `<exe dir>/D3D12/`. Executables that create D3D12 devices include `src/gpu/d3d12/d3d12_agility_exports.h` once. |
| bc7enc_rdo | 2026-07-31-b943862 | MIT OR Unlicense | `de_tp_bc7enc_rdo` | `<bc7enc.h>`, `<rgbcx.h>`, `<ert.h>`, `<bc7decomp.h>` (C++, only in `tools/cook/importers/texture/texture_bc_enc.cpp`) | BC1/BC3/BC4/BC5/BC7 encoders of the texture importer (DENG-99): bc7enc (BC7), rgbcx (BC1 to BC5), ert (the `rdo_quality` pass) and bc7decomp (the BC7 unpacker ert needs). Tools only: linked into the cooker library, never into Release or Shipping. 11 files, about 480 KB: no command-line tool, `rdo_bc_encoder`, `utils.cpp`, lodepng, miniz, ISPC kernel or `ispc.exe`, and rgbcx's small ordering table only (`RGBCX_USE_SMALLER_TABLES=1`, PUBLIC). Upstream has no tags: pinned to a commit. Built without FMA contraction. Two local patches: a UBSan fix in `ert::hash_hsieh` (misaligned loads, a negative shift; hash values unchanged), and `<stdint.h>` includes in `ert.h` and `utils.h` (they did not compile on Linux). BC6H is not here: the importer uses the Basis Universal transcoder's BC6H encoder (`de_tp_basisu`). |
| Direct3D WARP | 1.0.21 | Microsoft Direct3D WARP terms (`LICENSE.TXT`; test-only, no redistribution) | none (`de_d3d12_warp_deploy(<target>)`) | – | **Test-only** software rasterizer for the D3D12 tests (DENG-405). A **prebuilt** entry (`d3d12_warp`, the NuGet package `Microsoft.Direct3D.WARP`). Only the licence text is committed. `d3d10warp.dll` (host `windows-x64` only) is extracted at configure time when the tests and the D3D12 backend are built and `DE_D3D12_TEST_WARP` is ON (the default). `de_d3d12_warp_deploy()` copies it next to `test_gpu_d3d12`, so the D3D12 runtime loads it instead of the WARP of Windows. The windows-2022 runners' own WARP has shader model 6.2. This one has the Base tier (SM 6.6), enhanced barriers and mesh shaders, so the D3D12 pipeline tests run on CI. Policy rule 10. |
| bcdec | 2026-09-02-80859ed | MIT OR Unlicense | `de_tp_bcdec` | `<bcdec.h>` (only in `tests/unit/import/`) | **Test-only** reference BC1 to BC7 decoder (DENG-99): the BC tests decode the cooked blocks with it. Only test executables link it; `de_cooker`, `de-cook` and the runtime never do. `bcdec.h` only, one implementation TU (`de_tp_bcdec.c`). Upstream has no tags: pinned to a commit. One local patch (UBSan: an uninitialized BC7 endpoint is shifted; decoded texels unchanged). Not in the SPEC §22 table; see below. |

## SPEC §22 additions

Libraries vendored here that the SPEC §22 table does not list (policy 1: a DENG-2 decision or a
task that adds them records them here):

| Library | Licence | Use | Added by |
|---|---|---|---|
| MikkTSpace | Zlib | tangent space generation for imported models (tools only) | DENG-101 |
| bcdec | MIT OR Unlicense | reference BC decoder for the texture importer's tests (test-only) | DENG-99 |
| Microsoft Direct3D WARP | Microsoft Direct3D WARP terms (test-only, no redistribution) | software D3D12 adapter for the D3D12 tests on CI (test-only, never shipped; SPEC §19 GPU tests on WARP) | DENG-405 |

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
