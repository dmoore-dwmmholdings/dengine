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
python3 tools/vendor/vendor.py hash <url-or-file>   # sha256 for a new entry
```

Global options, which go before the command:

- `--root <dir>`: the repository root. The default is two levels above the script.
- `--config <file>`: the pin list. The default is `<root>/third_party/vendor.toml`.
- `--cache <dir>`: the archive cache. The default is `$DE_VENDOR_CACHE`, or
  `~/.cache/dengine-vendor` if that is not set.

Exit status: 0 means OK. 1 means a mismatch, a hash failure or a download failure. 2 means a bad
`vendor.toml` (or a policy violation) or a usage error.

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

**Config checks.** These run on every command. Every entry needs `name`, `version`, `url`,
`sha256`, `license` and `files`. The licence must be in the SPEC §22 allowlist
(`ALLOWED_LICENSES`). The tool refuses unknown keys, duplicate library names, and names or URLs
that look like proprietary SDKs.

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

## CI

CI (DENG-66) runs `python3 tools/vendor/vendor.py verify`. It points `DE_VENDOR_CACHE` at a
cached directory to avoid downloading on each run. ctest runs `vendor_selftest`
(`tests/selftest.py`, label `check`). This self-test runs offline: it exercises fetch, verify,
tamper detection, patches, git control files, git-ignored files (in a `git init` fake repository)
and the policy checks against local `file://` archives. The real `verify` needs the pinned
archives, so it is a separate CI step and not a ctest.
