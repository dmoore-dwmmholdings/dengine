# tools/vendor: vendored-library manager

`vendor.py` manages `third_party/vendor.toml`, the pin list of every vendored library. The
policy is in `third_party/README.md`. The script uses only the Python standard library and needs
Python 3.10 or later. On 3.10 it uses a built-in TOML reader, because `tomllib` arrived in 3.11.

## Commands

```sh
python3 tools/vendor/vendor.py list                 # pinned libraries
python3 tools/vendor/vendor.py fetch <name>...      # (re)write third_party/<name>/
python3 tools/vendor/vendor.py fetch --all
python3 tools/vendor/vendor.py verify               # every tree matches its archive
python3 tools/vendor/vendor.py verify <name>...     # only these trees
python3 tools/vendor/vendor.py verify --offline     # never download; a missing archive fails
python3 tools/vendor/vendor.py verify --all-hosts   # also every host's prebuilt archive
python3 tools/vendor/vendor.py prebuilt <name> --dest <dir>   # extract this host's binaries
python3 tools/vendor/vendor.py hash <url-or-file>   # sha256 for a new entry
```

`fetch`, `verify` and `prebuilt` take `--host <host>` (`macos-arm64`, `linux-x64`,
`windows-x64`; the default is this machine) to pick the host of a prebuilt entry. `fetch` and
`verify` also take `--all-hosts`. `prebuilt` also takes `--offline`. These options change nothing
for a normal (`kind = "source"`) entry.

Global options, which go before the command:

- `--root <dir>`: the repository root. The default is two levels above the script.
- `--config <file>`: the pin list. The default is `<root>/third_party/vendor.toml`.
- `--cache <dir>`: the archive cache. The default is `$DE_VENDOR_CACHE`, or
  `~/.cache/dengine-vendor` if that is not set.

Exit status: 0 means OK. 1 means a mismatch, a hash failure or a download failure. 2 means a bad
`vendor.toml` (or a policy violation) or a usage error. 3 (`prebuilt` only) means the entry has no
archive for the host.

## What each command does

**fetch** downloads the archive into the cache, or uses the cached copy if its sha256 still
matches, and checks the sha256. It then removes every file in `third_party/<name>/` that does not
match a `local` glob, and writes the selected archive files and `LICENSE`. Engine-owned files
(`CMakeLists.txt`, `de_*`, `smoke/**`, `patches/**`) are never touched. When an entry lists
`patches`, `fetch` applies them in order with `git apply` before it writes the files.

`fetch` never writes the archive's git control files (`.gitignore` and `.gitattributes`, at any
depth), even when a `files` glob selects them. An upstream `.gitignore` would stop git from
committing vendored files, and an upstream `.gitattributes` would override
`third_party/.gitattributes`. SDL 3.4.18 ships `src/render/gpu/shaders/.gitignore` with `*.hlsl`
(DENG-362). After it writes the files, `fetch` fails if git ignores one of them (see below).

**verify** gets each archive the same way as `fetch` (it downloads a missing archive unless you
give `--offline`) and checks the sha256. It then compares the tree with the archive. The check
fails on:

- a changed byte in an upstream file;
- a missing upstream file;
- a file that is not from the archive and does not match a `local` glob;
- a `files` glob that matches nothing;
- a vendored file that git ignores. The check runs `git check-ignore --no-index`, so a tracked
  file is checked too, and the message names the ignore rule. It is skipped outside a git work
  tree. The last line of the root `.gitignore`, `!/third_party/**`, keeps the rules above it and
  a global excludes file away from `third_party/`;
- a directory under `third_party/` that `vendor.toml` does not list (full verify only).

For a prebuilt entry, `fetch` and `verify` also download this host's archive into the cache (if
it is not there), check its sha256, and check that each of its `files` globs matches something
and that each selected symlink's target is selected too. Nothing from it is written into
`third_party/`. With `--all-hosts` they check every host's archive. On a host that the entry does
not list, they print a `skip` note and check only the committed part.

**prebuilt** extracts the files of one host archive (`[lib.hosts.<host>]` `files`, `exclude`,
`strip`) into `--dest`, which it replaces as a whole. It keeps the file modes, so executables stay
executable, and it writes symlinks as symlinks (on Windows, as copies of their target). It
downloads the archive first on a cache miss, unless `--offline` is given. It writes a stamp file
(`.de-prebuilt.json`) with the pin and the file list. When the stamp matches and every file is
present, it does nothing, so a re-configure is cheap. `cmake/DePrebuilt.cmake` runs it at
configure time (see "Prebuilt entries" below).

**Config checks.** These run on every command. Every entry needs `name`, `version`, `url`,
`sha256`, `license` and `files`. The licence must be in the SPEC §22 allowlist
(`ALLOWED_LICENSES`). The tool refuses unknown keys, duplicate library names, and names or URLs
that look like proprietary SDKs. A prebuilt entry needs at least one host table, and each needs
`url`, `sha256` and `files`. A host name must be `<macos|linux|windows>-<arm64|x64>`. `hosts` on a
normal entry is an error.

## Entry format

The key reference is at the top of `third_party/vendor.toml`. Here is an example:

```toml
[[lib]]
name = "zstd"
version = "1.5.7"
url = "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"
sha256 = "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3"
license = "BSD-3-Clause"
files = ["lib/zstd.h", "lib/common/**", "lib/compress/**", "lib/decompress/**"]
exclude = ["lib/decompress/*.S"]
```

Archive paths lose their first component (`strip = 1`) before matching, so `zstd-1.5.7/lib/zstd.h`
becomes `lib/zstd.h` and is written to `third_party/zstd/lib/zstd.h`. In globs, `*` matches
inside one path component and `**` matches across components.

Prefer an upstream release asset (zstd publishes one) to a GitHub auto-generated archive. GitHub
generates `/archive/` tarballs on demand. They have been byte-stable in practice, but if one ever
changes, `fetch` reports a sha256 mismatch and refuses it. Re-check the source, then update the
hash.

## Prebuilt entries

A library that upstream ships only as large per-platform binaries (Slang, DENG-289) uses
`kind = "prebuilt"`. The policy is rule 9 in `third_party/README.md`. The entry keeps the normal
keys for the committed part (headers and `LICENSE`) and adds one table per host:

```toml
[[lib]]
name = "demo"
kind = "prebuilt"
version = "1.0"
url = "https://example.com/demo-1.0-linux-x86_64.tar.gz"   # committed part
sha256 = "<sha256>"
license = "MIT"
strip = 0
files = ["include/*.h"]

[lib.hosts.linux-x64]
url = "https://example.com/demo-1.0-linux-x86_64.tar.gz"   # same archive: one cached file
sha256 = "<sha256>"
strip = 0
files = ["bin/democ", "lib/libdemo*.so*"]

[lib.hosts.macos-arm64]
url = "https://example.com/demo-1.0-macos-aarch64.zip"
sha256 = "<sha256>"
files = ["bin/democ", "lib/libdemo*.dylib"]

[lib.hosts.windows-x64]
url = "https://example.com/demo-1.0-windows-x86_64.zip"
sha256 = "<sha256>"
files = ["bin/democ.exe", "bin/demo*.dll"]
```

The library's `third_party/<name>/CMakeLists.txt` imports the binaries at configure time:

```cmake
include(DePrebuilt)
de_prebuilt_import(demo)            # sets DE_PREBUILT_DEMO_DIR, _FOUND, _HOST
add_library(de_tp_demo SHARED IMPORTED GLOBAL)
# Linux shown. The file names differ per host; Windows also needs IMPORTED_IMPLIB (the .lib).
set_target_properties(de_tp_demo PROPERTIES
    IMPORTED_LOCATION "${DE_PREBUILT_DEMO_DIR}/lib/libdemo.so"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include")
```

`de_prebuilt_import(<name> [HOST <host>] [DEST <dir>] [ROOT <repo>] [OPTIONAL])` runs
`vendor.py prebuilt` into `<build>/prebuilt/<name>/`. The default host is the target platform
(`CMAKE_SYSTEM_NAME` and `CMAKE_SYSTEM_PROCESSOR`), so the `windows-xwin-dev` cross build on a Mac
imports `windows-x64`. With `OPTIONAL`, a host that the entry does not list sets
`DE_PREBUILT_<NAME>_FOUND` to `FALSE` instead of failing; a hash mismatch or a failed download
always fails the configure. `vendor.toml` becomes a configure dependency, so a new pin re-runs the
configure. Call it only when a target that is built needs the library (for example under
`DE_BUILD_TOOLS`), so that other builds download nothing. The header of
`cmake/DePrebuilt.cmake` has the full reference.

To add or bump a prebuilt entry: run `vendor.py hash <url>` for each host's archive, write the
entry, run `vendor.py fetch <name> --all-hosts`, then `vendor.py verify --all-hosts`. Pick the
smallest `files` set that the engine needs; the cache keeps the whole archive, but the build
directory gets only the selection.

## CI

The `Checks` workflow (DENG-66) runs `python tools/vendor/vendor.py verify --all-hosts`, with
the archive cache (`~/.cache/dengine-vendor`) restored by `actions/cache`, so a warm run
downloads nothing. The build jobs cache the same directory per OS (`setup-dengine`), for the
prebuilt archives a configure downloads. ctest runs `vendor_selftest`
(`tests/selftest.py`, label `check`) and `vendor_prebuilt_selftest` (`tests/prebuilt_selftest.py`,
label `check`). This self-test runs offline: it exercises fetch, verify,
tamper detection, patches, git control files, git-ignored files (in a `git init` fake repository)
and the policy checks against local `file://` archives. The prebuilt self-test builds fake
archives for the three hosts (a tar.gz without a top directory, and zips with unix modes and
symlinks) and checks the config errors with both TOML readers, `fetch`, `verify` and
`--all-hosts`, hash mismatches, extraction (modes, symlinks, the stamp), and a CMake configure
that imports the fake entry for the machine's own host through `cmake/DePrebuilt.cmake`. CI runs
it on macOS, Linux and Windows. The real `verify` needs the pinned archives, so it is a separate
CI step and not a ctest.
