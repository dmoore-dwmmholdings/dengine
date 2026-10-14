#!/usr/bin/env python3
"""Vendored third-party code manager for DEngine (SPEC §22, CONVENTIONS §6).

`third_party/vendor.toml` pins every vendored library: an archive URL, its sha256, the licence,
and the files taken from it. This tool downloads the archive, checks the hash, and writes the
selected files into `third_party/<name>/`. `verify` checks that the tree in the repository is
byte-identical to what the pinned archive gives, so a local edit to vendored code is detected.
Upstream `.gitignore` and `.gitattributes` files are never vendored, and `fetch` and `verify` fail
when git ignores a vendored file, so every vendored file is committed (DENG-362).

A `kind = "prebuilt"` entry (DENG-390) also pins one binary archive per host in
`[lib.hosts.<host>]` tables. Those archives are never committed: they stay in the cache, and
`prebuilt` extracts the host's files into a build directory at configure time
(cmake/DePrebuilt.cmake). The entry's own `url`/`files` are the committed part (headers, LICENSE).

Commands:
  fetch <name>... | --all   download, verify sha256, (re)write third_party/<name>/
  verify [<name>...]        check archive hashes and that each tree matches its archive
  prebuilt <name> --dest D  extract the host's prebuilt files of <name> into D
  list                      print the pinned libraries
  hash <url-or-file>        print the sha256 of an archive (to fill in a new vendor.toml entry)

`fetch` and `verify` check the prebuilt archive of this host only; `--all-hosts` checks every
host's archive, `--host` picks another host. Archives are cached in $DE_VENDOR_CACHE, else
~/.cache/dengine-vendor. `verify` and `prebuilt` download a missing archive unless --offline is
given. Exit status: 0 ok, 1 mismatch or download failure, 2 bad vendor.toml or usage, 3 (`prebuilt`
only) the entry has no archive for this host.

Standard library only; Python 3.10+ (tomllib from 3.11, else a minimal parser for the subset of
TOML that vendor.toml uses).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", ".."))

# Licences that SPEC §22 and the DENG-2 addendum allow, as SPDX expressions. An entry whose
# licence is not here is refused: add the library to §22 (or DENG-2) first, then extend this set.
ALLOWED_LICENSES = {
    "MIT",
    "MIT OR Unlicense",  # stb, dr_libs: MIT or public domain
    "MIT OR CC0-1.0",
    "BSD-2-Clause",
    "BSD-2-Clause OR CC0-1.0",  # Monocypher
    "BSD-3-Clause",
    "Zlib",
    "Apache-2.0",
    "Apache-2.0 WITH LLVM-exception",  # Slang
    "Unicode-3.0",  # CLDR data (DENG-2 addendum)
}

# Licences of binaries that the tests may download and run, but that may not be shared or
# redistributed. Allowed only on a `kind = "prebuilt"` entry, whose binaries are never committed;
# the library's CMakeLists.txt deploys them next to test executables only, never into a shipped or
# packaged target (third_party/README.md, policy rule 9).
TEST_ONLY_LICENSES = {
    "LicenseRef-Microsoft-Direct3D-WARP",  # d3d12_warp: "for testing purposes only" (DENG-405)
}

# Proprietary SDKs that are never vendored or downloaded (CONVENTIONS §6). Matched against the
# entry name and URL, case-insensitively. The D3D12 Agility SDK is not one of them: SPEC §22 lists
# it as a Microsoft redistributable, its headers are MIT, and its runtime DLLs are a prebuilt
# entry that is never committed (DENG-285).
FORBIDDEN_PATTERNS = re.compile(
    r"steamworks|steam_?sdk|windows_?sdk|winsdk|xwin|msvc_?crt|vcruntime", re.IGNORECASE
)

# Files in third_party/<name>/ that belong to the engine, not to the upstream archive, unless an
# entry sets its own `local` list.
DEFAULT_LOCAL = ["CMakeLists.txt", "de_*", "smoke/**", "patches/**"]

# Git control files in an archive are never vendored, at any depth, whatever `files` selects. An
# upstream `.gitignore` would stop git from committing vendored files (SDL ships one with `*.hlsl`
# next to its shaders, DENG-362), and an upstream `.gitattributes` would override the `-text` rule
# in third_party/.gitattributes. Neither is needed to build. A leftover one on disk fails `verify`
# as an unexpected file.
GIT_CONTROL_FILES = {".gitignore", ".gitattributes"}

REQUIRED_KEYS = ("name", "version", "url", "sha256", "license", "files")
KNOWN_KEYS = set(REQUIRED_KEYS) | {
    "license_file", "strip", "exclude", "local", "patches", "notes", "kind", "hosts",
}

# `kind`: "source" (the default) commits every selected file. "prebuilt" also pins per-host
# binary archives in `hosts`, which stay in the cache and are extracted into a build directory.
KINDS = ("source", "prebuilt")

# Keys of one [lib.hosts.<host>] table.
HOST_REQUIRED_KEYS = ("url", "sha256", "files")
HOST_KNOWN_KEYS = set(HOST_REQUIRED_KEYS) | {"strip", "exclude", "notes"}

# Host names: <os>-<arch>. v1 ships macos-arm64, linux-x64 and windows-x64 (DENG-2 Q3); the
# pattern also accepts the other two arm64/x64 combinations so a later pin needs no code change.
HOST_RE = re.compile(r"^(macos|linux|windows)-(arm64|x64)$")

# Written into a `prebuilt --dest` directory; when it matches, a re-configure extracts nothing.
PREBUILT_STAMP = ".de-prebuilt.json"
EXIT_NO_HOST = 3

NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_]*$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")


class ConfigError(Exception):
    """vendor.toml is missing, malformed or breaks the vendoring policy."""


class VendorError(Exception):
    """A download, hash or tree check failed."""


# --------------------------------------------------------------------------------------------
# Minimal TOML reader (Python 3.10 has no tomllib). Supports what vendor.toml uses: comments,
# [[array.of.tables]], [table], key = "string" | 'literal' | integer | boolean | array of those
# (arrays may span lines).


def _strip_comment(line: str) -> str:
    quote = None
    i = 0
    while i < len(line):
        c = line[i]
        if quote:
            if c == "\\" and quote == '"':
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == "#":
            return line[:i]
        i += 1
    return line


def _parse_value(text: str, where: str):
    text = text.strip()
    if text.startswith("["):
        if not text.endswith("]"):
            raise ConfigError(f"{where}: unterminated array")
        body = text[1:-1]
        items, cur, quote, depth = [], "", None, 0
        i = 0
        while i < len(body):
            c = body[i]
            if quote:
                cur += c
                if c == "\\" and quote == '"' and i + 1 < len(body):
                    cur += body[i + 1]
                    i += 2
                    continue
                if c == quote:
                    quote = None
            elif c in "\"'":
                quote = c
                cur += c
            elif c == "[":
                depth += 1
                cur += c
            elif c == "]":
                depth -= 1
                cur += c
            elif c == "," and depth == 0:
                if cur.strip():
                    items.append(_parse_value(cur, where))
                cur = ""
            else:
                cur += c
            i += 1
        if cur.strip():
            items.append(_parse_value(cur, where))
        return items
    if text.startswith('"') and text.endswith('"') and len(text) >= 2:
        escapes = {"\\": "\\", '"': '"', "n": "\n", "t": "\t", "r": "\r"}
        return re.sub(r"\\(.)", lambda m: escapes.get(m.group(1), m.group(0)), text[1:-1])
    if text.startswith("'") and text.endswith("'") and len(text) >= 2:
        return text[1:-1]
    if text in ("true", "false"):
        return text == "true"
    if re.fullmatch(r"[+-]?[0-9][0-9_]*", text):
        return int(text.replace("_", ""))
    raise ConfigError(f"{where}: unsupported value {text!r}")


def parse_toml_minimal(text: str, name: str = "<toml>") -> dict:
    root: dict = {}
    cur = root
    pending = ""
    pending_line = 0
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw).rstrip()
        if pending:
            pending += " " + line.strip()
            if pending.count("[") > pending.count("]"):
                continue
            key, _, val = pending.partition("=")
            cur[key.strip().strip('"')] = _parse_value(val, f"{name}:{pending_line}")
            pending = ""
            continue
        s = line.strip()
        if not s:
            continue
        m = re.fullmatch(r"\[\[\s*([A-Za-z0-9_.-]+)\s*\]\]", s)
        if m:
            parent = root
            parts = m.group(1).split(".")
            for p in parts[:-1]:
                parent = parent.setdefault(p, {})
            arr = parent.setdefault(parts[-1], [])
            if not isinstance(arr, list):
                raise ConfigError(f"{name}:{lineno}: {m.group(1)} is not an array of tables")
            cur = {}
            arr.append(cur)
            continue
        m = re.fullmatch(r"\[\s*([A-Za-z0-9_.-]+)\s*\]", s)
        if m:
            cur = root
            for p in m.group(1).split("."):
                cur = cur.setdefault(p, {})
                # [lib.hosts.x] after [[lib]] is a sub-table of the last [[lib]] element (TOML).
                if isinstance(cur, list):
                    if not cur:
                        raise ConfigError(f"{name}:{lineno}: {m.group(1)}: empty array of tables")
                    cur = cur[-1]
                if not isinstance(cur, dict):
                    raise ConfigError(f"{name}:{lineno}: {m.group(1)} is not a table")
            continue
        if "=" not in s:
            raise ConfigError(f"{name}:{lineno}: expected `key = value`")
        key, _, val = s.partition("=")
        key = key.strip().strip('"')
        if key in cur:
            raise ConfigError(f"{name}:{lineno}: duplicate key {key!r}")
        if val.strip().startswith("[") and val.count("[") > val.count("]"):
            pending, pending_line = s, lineno
            continue
        cur[key] = _parse_value(val, f"{name}:{lineno}")
    if pending:
        raise ConfigError(f"{name}:{pending_line}: unterminated array")
    return root


def load_toml(path: str, force_fallback: bool = False) -> dict:
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise ConfigError(f"cannot read {path}: {e}") from e
    if not force_fallback:
        try:
            import tomllib  # Python 3.11+
        except ImportError:
            tomllib = None
        if tomllib is not None:
            try:
                return tomllib.loads(data.decode("utf-8"))
            except tomllib.TOMLDecodeError as e:
                raise ConfigError(f"{path}: {e}") from e
    return parse_toml_minimal(data.decode("utf-8"), path)


# --------------------------------------------------------------------------------------------
# Globs: `*` and `?` stay inside one path component, `**` spans components ("a/**" matches
# everything under a/, "**/x.h" matches x.h at any depth).


def glob_to_regex(pattern: str) -> re.Pattern:
    out = ""
    i = 0
    while i < len(pattern):
        c = pattern[i]
        if pattern.startswith("**/", i):
            out += "(?:.*/)?"
            i += 3
        elif pattern.startswith("**", i):
            out += ".*"
            i += 2
        elif c == "*":
            out += "[^/]*"
            i += 1
        elif c == "?":
            out += "[^/]"
            i += 1
        else:
            out += re.escape(c)
            i += 1
    return re.compile(out + r"\Z")


def match_any(path: str, patterns: list) -> bool:
    return any(p.match(path) for p in patterns)


# --------------------------------------------------------------------------------------------
# Config


class Lib:
    def __init__(self, d: dict, where: str):
        unknown = set(d) - KNOWN_KEYS
        if unknown:
            raise ConfigError(f"{where}: unknown keys {sorted(unknown)}")
        for k in REQUIRED_KEYS:
            if k not in d or d[k] in ("", []):
                raise ConfigError(f"{where}: missing required key `{k}`")
        self.name: str = d["name"]
        where = f"{where} ({self.name})"
        if not isinstance(self.name, str) or not NAME_RE.match(self.name):
            raise ConfigError(f"{where}: `name` must be lower_snake_case")
        self.version: str = str(d["version"])
        self.url: str = d["url"]
        self.sha256: str = str(d["sha256"]).lower()
        if not SHA_RE.match(self.sha256):
            raise ConfigError(f"{where}: `sha256` must be 64 hex digits")
        self.license: str = d["license"]
        if self.license not in ALLOWED_LICENSES and self.license not in TEST_ONLY_LICENSES:
            raise ConfigError(
                f"{where}: licence {self.license!r} is not in the SPEC §22 allowlist "
                f"({', '.join(sorted(ALLOWED_LICENSES))})")
        if FORBIDDEN_PATTERNS.search(self.name) or FORBIDDEN_PATTERNS.search(self.url):
            raise ConfigError(
                f"{where}: proprietary SDKs are never vendored (CONVENTIONS §6); "
                "CMake finds them through a user-set path")
        self.license_file: str = d.get("license_file", "LICENSE")
        self.strip: int = d.get("strip", 1)
        if not isinstance(self.strip, int) or self.strip < 0:
            raise ConfigError(f"{where}: `strip` must be a non-negative integer")

        def strlist(key, default):
            v = d.get(key, default)
            if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
                raise ConfigError(f"{where}: `{key}` must be a list of strings")
            return v

        self.files = strlist("files", [])
        self.exclude = strlist("exclude", [])
        self.local = strlist("local", DEFAULT_LOCAL)
        self.patches = strlist("patches", [])
        self.files_re = [glob_to_regex(p) for p in self.files]
        self.exclude_re = [glob_to_regex(p) for p in self.exclude]
        self.local_re = [glob_to_regex(p) for p in self.local]
        for p in self.patches:
            if not match_any(p, self.local_re):
                raise ConfigError(f"{where}: patch {p!r} must match a `local` glob")

        self.kind: str = d.get("kind", "source")
        if self.kind not in KINDS:
            raise ConfigError(f"{where}: `kind` must be one of {', '.join(KINDS)}")
        hosts = d.get("hosts")
        if self.kind == "prebuilt":
            if not isinstance(hosts, dict) or not hosts:
                raise ConfigError(
                    f"{where}: a prebuilt entry needs at least one [lib.hosts.<host>] table")
        elif hosts is not None:
            raise ConfigError(f'{where}: `hosts` needs kind = "prebuilt"')
        if self.license in TEST_ONLY_LICENSES and self.kind != "prebuilt":
            raise ConfigError(
                f"{where}: licence {self.license!r} is test-only and forbids redistribution, so "
                'it needs kind = "prebuilt" (binaries never committed)')
        self.hosts: dict = {h: HostArchive(self, h, t, where)
                            for h, t in sorted((hosts or {}).items())}

    @property
    def archive_name(self) -> str:
        return cache_name(self.name, self.sha256, self.url)


def cache_name(name: str, sha256: str, url: str) -> str:
    """Archive file name in the cache. A host archive with the same URL and hash as the
    entry's committed part (the usual prebuilt case) shares one cached file."""
    base = posixpath.basename(urllib.parse.urlparse(url).path) or "archive"
    return f"{name}-{sha256[:16]}-{base}"


class HostArchive:
    """One [lib.hosts.<host>] table of a prebuilt entry: a binary archive that is cached and
    extracted into a build directory, never committed."""

    def __init__(self, lib: Lib, host: str, d, where: str):
        where = f"{where} [hosts.{host}]"
        if not HOST_RE.match(host):
            raise ConfigError(f"{where}: host must be <macos|linux|windows>-<arm64|x64>")
        if not isinstance(d, dict):
            raise ConfigError(f"{where}: must be a table")
        unknown = set(d) - HOST_KNOWN_KEYS
        if unknown:
            raise ConfigError(f"{where}: unknown keys {sorted(unknown)}")
        for k in HOST_REQUIRED_KEYS:
            if k not in d or d[k] in ("", []):
                raise ConfigError(f"{where}: missing required key `{k}`")
        self.lib = lib
        self.host = host
        self.name = f"{lib.name} [{host}]"  # for messages
        self.version = lib.version
        self.url: str = d["url"]
        self.sha256: str = str(d["sha256"]).lower()
        if not SHA_RE.match(self.sha256):
            raise ConfigError(f"{where}: `sha256` must be 64 hex digits")
        if FORBIDDEN_PATTERNS.search(self.url):
            raise ConfigError(
                f"{where}: proprietary SDKs are never vendored (CONVENTIONS §6); "
                "CMake finds them through a user-set path")
        self.strip: int = d.get("strip", 1)
        if not isinstance(self.strip, int) or self.strip < 0:
            raise ConfigError(f"{where}: `strip` must be a non-negative integer")
        for key in ("files", "exclude"):
            v = d.get(key, [])
            if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
                raise ConfigError(f"{where}: `{key}` must be a list of strings")
        self.files: list = d["files"]
        self.exclude: list = d.get("exclude", [])
        self.files_re = [glob_to_regex(p) for p in self.files]
        self.exclude_re = [glob_to_regex(p) for p in self.exclude]

    @property
    def archive_name(self) -> str:
        return cache_name(self.lib.name, self.sha256, self.url)

    def stamp_key(self) -> dict:
        """What decides the extracted file set; a change re-extracts."""
        return {"name": self.lib.name, "version": self.version, "host": self.host,
                "url": self.url, "sha256": self.sha256, "strip": self.strip,
                "files": self.files, "exclude": self.exclude}


def load_config(path: str, force_fallback: bool = False) -> list:
    data = load_toml(path, force_fallback)
    libs_raw = data.get("lib", [])
    if not isinstance(libs_raw, list):
        raise ConfigError(f"{path}: `lib` must be an array of tables ([[lib]])")
    libs = []
    seen = set()
    for i, d in enumerate(libs_raw):
        lib = Lib(d, f"{path}: [[lib]] #{i + 1}")
        if lib.name in seen:
            # One entry per library: later sprints extend `files`, never add a second version.
            raise ConfigError(f"{path}: library {lib.name!r} is listed twice")
        seen.add(lib.name)
        libs.append(lib)
    return libs


# --------------------------------------------------------------------------------------------
# Archives


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: str) -> None:
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".part-", dir=os.path.dirname(dest))
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "dengine-vendor/1"})
        with urllib.request.urlopen(req, timeout=120) as r, os.fdopen(fd, "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(tmp, dest)
    except Exception as e:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise VendorError(f"download failed: {url}: {e}") from e


def get_archive(lib: Lib, cache: str, offline: bool) -> str:
    """Return the path of the cached archive, downloading it if needed. Checks the sha256."""
    path = os.path.join(cache, lib.archive_name)
    if os.path.exists(path):
        got = sha256_file(path)
        if got == lib.sha256:
            return path
        if offline:
            raise VendorError(
                f"{lib.name}: cached archive {path} has sha256 {got}, expected {lib.sha256}")
        print(f"{lib.name}: cached archive has the wrong hash, downloading again")
    elif offline:
        raise VendorError(f"{lib.name}: archive not in cache ({path}) and --offline was given")
    print(f"{lib.name}: downloading {lib.url}")
    download(lib.url, path)
    got = sha256_file(path)
    if got != lib.sha256:
        os.unlink(path)
        raise VendorError(
            f"{lib.name}: sha256 mismatch for {lib.url}\n  expected {lib.sha256}\n  got      {got}")
    return path


def _safe_rel(name: str, strip: int):
    """Archive member name -> path relative to the library dir, or None if stripped away."""
    name = name.replace("\\", "/")
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if name.startswith("/") or ".." in parts:
        raise VendorError(f"unsafe path in archive: {name!r}")
    parts = parts[strip:]
    return "/".join(parts) if parts else None


def read_archive(path: str, strip: int) -> dict:
    """All regular files in the archive: {relative path: bytes}. Symlinks map to None."""
    out: dict = {}
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                rel = _safe_rel(info.filename, strip)
                if rel is None:
                    continue
                is_link = (info.external_attr >> 16) & 0o170000 == 0o120000
                out[rel] = None if is_link else z.read(info)
        return out
    try:
        tf = tarfile.open(path, "r:*")
    except tarfile.TarError as e:
        raise VendorError(f"{path}: not a tar or zip archive: {e}") from e
    with tf:
        for m in tf.getmembers():
            if m.isdir():
                continue
            rel = _safe_rel(m.name, strip)
            if rel is None:
                continue
            if m.isfile():
                f = tf.extractfile(m)
                out[rel] = f.read() if f else b""
            elif m.issym() or m.islnk():
                out[rel] = None
    return out


def apply_patches(lib: Lib, lib_dir: str, files: dict) -> dict:
    """Apply lib.patches (paths relative to third_party/<name>/) with `git apply` in a temp dir."""
    if not lib.patches:
        return files
    with tempfile.TemporaryDirectory(prefix=f"de-vendor-{lib.name}-") as tmp:
        for rel, data in files.items():
            p = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        for patch in lib.patches:
            ppath = os.path.join(lib_dir, patch)
            if not os.path.isfile(ppath):
                raise VendorError(f"{lib.name}: patch not found: {ppath}")
            r = subprocess.run(["git", "apply", "--whitespace=nowarn", ppath], cwd=tmp,
                               capture_output=True, text=True)
            if r.returncode != 0:
                raise VendorError(f"{lib.name}: patch {patch} does not apply:\n{r.stderr}")
        out = {}
        for dirpath, _, names in os.walk(tmp):
            for n in names:
                p = os.path.join(dirpath, n)
                rel = os.path.relpath(p, tmp).replace(os.sep, "/")
                with open(p, "rb") as f:
                    out[rel] = f.read()
        return out


def expected_tree(lib: Lib, archive: str, lib_dir: str) -> dict:
    """{relative path: bytes} that third_party/<name>/ must hold, local files excluded."""
    members = read_archive(archive, lib.strip)
    tree: dict = {}
    used = [False] * len(lib.files_re)
    for rel in sorted(members):
        if posixpath.basename(rel) in GIT_CONTROL_FILES:
            continue
        hits = [i for i, p in enumerate(lib.files_re) if p.match(rel)]
        if not hits or match_any(rel, lib.exclude_re):
            continue
        for i in hits:
            used[i] = True
        if members[rel] is None:
            raise VendorError(f"{lib.name}: selected file {rel} is a symlink; select its target")
        if match_any(rel, lib.local_re):
            raise VendorError(f"{lib.name}: upstream file {rel} collides with a `local` glob")
        tree[rel] = members[rel]
    for i, ok in enumerate(used):
        if not ok:
            raise VendorError(f"{lib.name}: `files` glob {lib.files[i]!r} matches nothing")
    lic = members.get(lib.license_file)
    if lic is None:
        raise VendorError(f"{lib.name}: licence file {lib.license_file!r} not in the archive")
    if "LICENSE" in tree and tree["LICENSE"] != lic:
        raise VendorError(f"{lib.name}: archive LICENSE differs from license_file")
    tree["LICENSE"] = lic
    return apply_patches(lib, lib_dir, tree)


def scan_dir(lib: Lib, lib_dir: str) -> tuple:
    """Files in third_party/<name>/: (managed {rel: abs path}, local [rel])."""
    managed, local = {}, []
    if not os.path.isdir(lib_dir):
        return managed, local
    for dirpath, dirs, names in os.walk(lib_dir):
        dirs.sort()
        for n in sorted(names):
            p = os.path.join(dirpath, n)
            rel = os.path.relpath(p, lib_dir).replace(os.sep, "/")
            if match_any(rel, lib.local_re):
                local.append(rel)
            else:
                managed[rel] = p
    return managed, local


def git_ignored(lib: Lib, root: str, lib_dir: str, rels) -> list:
    """Problems for vendored files that git ignores, so a commit would silently leave them out.

    Runs `git check-ignore --no-index` (tracked files are checked too) on every path that the
    archive gives. Returns [] when `root` is not inside a git work tree or git is not installed:
    a tarball export of the repository has nothing to commit.
    """
    try:
        r = subprocess.run(["git", "rev-parse", "--is-inside-work-tree"], cwd=root,
                           capture_output=True, text=True)
    except OSError:
        return []
    if r.returncode != 0 or r.stdout.strip() != "true":
        return []
    paths = [os.path.relpath(os.path.join(lib_dir, *rel.split("/")), root).replace(os.sep, "/")
             for rel in sorted(rels)]
    r = subprocess.run(["git", "check-ignore", "--no-index", "--verbose", "-z", "--stdin"],
                       cwd=root, input="\0".join(paths) + "\0", capture_output=True, text=True)
    if r.returncode not in (0, 1):  # 1: nothing ignored
        return [f"{lib.name}: git check-ignore failed: {r.stderr.strip()}"]
    # --verbose -z output: <source> NUL <line> NUL <pattern> NUL <path> NUL, per path whose last
    # matching rule is an ignore rule or a negation ("!..."). A negation means "not ignored".
    f = r.stdout.split("\0")
    problems = []
    for i in range(0, len(f) - 3, 4):
        source, line, pattern, path = f[i:i + 4]
        if pattern.startswith("!"):
            continue
        problems.append(
            f"{lib.name}: {path} is ignored by git ({source}:{line}: {pattern}); a commit would "
            "leave it out. Remove the rule, or add a negation to the root .gitignore")
    return problems


# --------------------------------------------------------------------------------------------
# Prebuilt archives (kind = "prebuilt"). Unlike read_archive(), these keep the file mode (an
# executable must stay executable) and symlink targets (Linux .so names are symlinks).


def current_host():
    """This machine as a host name (`macos-arm64`, `linux-x64`, ...), or None if unknown."""
    os_name = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system())
    arch = {"arm64": "arm64", "aarch64": "arm64", "x86_64": "x64", "amd64": "x64"}.get(
        platform.machine().lower())
    return f"{os_name}-{arch}" if os_name and arch else None


def read_archive_members(path: str, strip: int) -> dict:
    """{relative path: (data | None, mode, link target | None)} for files and symlinks."""
    out: dict = {}
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                rel = _safe_rel(info.filename, strip)
                if rel is None:
                    continue
                mode = (info.external_attr >> 16) & 0o177777
                if mode & 0o170000 == 0o120000:
                    out[rel] = (None, 0o777, z.read(info).decode("utf-8"))
                else:
                    out[rel] = (z.read(info), (mode & 0o777) or 0o644, None)
        return out
    try:
        tf = tarfile.open(path, "r:*")
    except tarfile.TarError as e:
        raise VendorError(f"{path}: not a tar or zip archive: {e}") from e
    with tf:
        for m in tf.getmembers():
            if m.isdir():
                continue
            rel = _safe_rel(m.name, strip)
            if rel is None:
                continue
            if m.issym():
                out[rel] = (None, 0o777, m.linkname)
            elif m.isfile() or m.islnk():  # extractfile() resolves a hard link to its target
                f = tf.extractfile(m)
                out[rel] = (f.read() if f else b"", (m.mode & 0o777) or 0o644, None)
    return out


def select_prebuilt(ha: HostArchive, archive: str) -> dict:
    """The host archive's selected members: {rel: (data | None, mode, link | None)}.

    Fails when a `files` glob matches nothing, or when a selected symlink points outside the
    selection (the extracted link would dangle)."""
    members = read_archive_members(archive, ha.strip)
    sel: dict = {}
    used = [False] * len(ha.files_re)
    for rel in sorted(members):
        if posixpath.basename(rel) in GIT_CONTROL_FILES:
            continue
        hits = [i for i, p in enumerate(ha.files_re) if p.match(rel)]
        if not hits or match_any(rel, ha.exclude_re):
            continue
        for i in hits:
            used[i] = True
        sel[rel] = members[rel]
    for i, ok in enumerate(used):
        if not ok:
            raise VendorError(f"{ha.name}: `files` glob {ha.files[i]!r} matches nothing")
    for rel, (_, _, link) in sel.items():
        if link is not None:
            _resolve_link(ha, sel, rel)
    return sel


def _resolve_link(ha: HostArchive, sel: dict, rel: str) -> str:
    """The selected regular file that symlink `rel` finally points to."""
    seen = set()
    cur = rel
    while sel[cur][2] is not None:
        if cur in seen:
            raise VendorError(f"{ha.name}: symlink loop at {rel}")
        seen.add(cur)
        link = sel[cur][2]
        target = posixpath.normpath(posixpath.join(posixpath.dirname(cur), link))
        if link.startswith("/") or target.startswith("../") or target == "..":
            raise VendorError(f"{ha.name}: symlink {cur} -> {link} leaves the archive")
        if target not in sel:
            raise VendorError(
                f"{ha.name}: symlink {cur} -> {link}: the target is not selected; add it to "
                "`files`")
        cur = target
    return cur


def write_prebuilt(ha: HostArchive, sel: dict, dest: str) -> None:
    """Write the selection into `dest` (replaced as a whole) and the stamp file."""
    parent = os.path.dirname(os.path.abspath(dest)) or "."
    os.makedirs(parent, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix=".de-prebuilt-", dir=parent)
    try:
        links = []
        for rel, (data, mode, link) in sorted(sel.items()):
            p = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            if link is not None:
                links.append((rel, p, link))
                continue
            with open(p, "wb") as f:
                f.write(data)
            os.chmod(p, mode | 0o600)
        for rel, p, link in links:
            try:
                if os.name == "nt":
                    raise OSError("no symlinks on Windows without developer mode")
                os.symlink(link, p)
            except OSError:
                # Copy the file it points to (Windows; or a file system without symlinks).
                data, mode, _ = sel[_resolve_link(ha, sel, rel)]
                with open(p, "wb") as f:
                    f.write(data)
                os.chmod(p, mode | 0o600)
        with open(os.path.join(tmp, PREBUILT_STAMP), "w", encoding="utf-8") as f:
            json.dump({"key": ha.stamp_key(), "files": sorted(sel)}, f, indent=1)
            f.write("\n")
        if os.path.isdir(dest) and not os.path.islink(dest):
            shutil.rmtree(dest)
        elif os.path.lexists(dest):
            os.unlink(dest)
        os.replace(tmp, dest)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise


def prebuilt_up_to_date(ha: HostArchive, dest: str) -> bool:
    """True when `dest` holds this exact pin's extraction (stamp matches, every file present)."""
    try:
        with open(os.path.join(dest, PREBUILT_STAMP), encoding="utf-8") as f:
            stamp = json.load(f)
    except (OSError, ValueError):
        return False
    if not isinstance(stamp, dict) or stamp.get("key") != ha.stamp_key():
        return False
    files = stamp.get("files")
    return isinstance(files, list) and all(
        isinstance(rel, str) and os.path.lexists(os.path.join(dest, *rel.split("/")))
        for rel in files)


def hosts_to_check(lib: Lib, host, all_hosts: bool) -> tuple:
    """(host archives to check, note for a host the entry does not list or None)."""
    if lib.kind != "prebuilt":
        return [], None
    if all_hosts:
        return list(lib.hosts.values()), None
    if host in lib.hosts:
        return [lib.hosts[host]], None
    listed = ", ".join(lib.hosts)
    return [], (f"skip {lib.name} {lib.version}: no prebuilt archive for host {host or 'unknown'} "
                f"(listed: {listed}; --all-hosts checks them all)")


def check_host_archive(ha: HostArchive, cache: str, offline: bool) -> list:
    """Problems with one host archive (hash, `files` selection); prints `ok` when none."""
    try:
        sel = select_prebuilt(ha, get_archive(ha, cache, offline))
    except VendorError as e:
        return [str(e)]
    print(f"ok   {ha.name} {ha.version} ({len(sel)} prebuilt files, cached)")
    return []


# --------------------------------------------------------------------------------------------
# Commands


def check_tree(lib: Lib, lib_dir: str, tree: dict) -> list:
    problems = []
    if not os.path.isdir(lib_dir):
        return [f"{lib.name}: {lib_dir} does not exist (run `vendor.py fetch {lib.name}`)"]
    on_disk, _ = scan_dir(lib, lib_dir)
    for rel in sorted(set(tree) | set(on_disk)):
        if rel not in on_disk:
            problems.append(f"{lib.name}: missing {rel}")
        elif rel not in tree:
            problems.append(
                f"{lib.name}: unexpected file {rel} (not in the pinned archive and not `local`)")
        else:
            with open(on_disk[rel], "rb") as f:
                if f.read() != tree[rel]:
                    problems.append(f"{lib.name}: {rel} differs from the pinned archive")
    return problems


def cmd_fetch(libs: list, root: str, cache: str, host=None, all_hosts: bool = False) -> int:
    for lib in libs:
        lib_dir = os.path.join(root, "third_party", lib.name)
        archive = get_archive(lib, cache, offline=False)
        tree = expected_tree(lib, archive, lib_dir)
        on_disk, local = scan_dir(lib, lib_dir)
        for p in on_disk.values():
            os.unlink(p)
        for rel, data in tree.items():
            p = os.path.join(lib_dir, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        # Remove directories left empty by deleted upstream files.
        for dirpath, _, _ in sorted(os.walk(lib_dir), key=lambda t: -len(t[0])):
            if dirpath != lib_dir and not os.listdir(dirpath):
                os.rmdir(dirpath)
        print(f"{lib.name} {lib.version}: wrote {len(tree)} files "
              f"({len(local)} local files kept)")
        ignored = git_ignored(lib, root, lib_dir, tree)
        for p in ignored:
            print(f"FAIL {p}", file=sys.stderr)
        if ignored:
            return 1
        # A prebuilt entry: also download and check the host archive(s) into the cache. They are
        # never written into third_party/.
        archives, note = hosts_to_check(lib, host, all_hosts)
        if note:
            print(note)
        problems = []
        for ha in archives:
            problems += check_host_archive(ha, cache, offline=False)
        for p in problems:
            print(f"FAIL {p}", file=sys.stderr)
        if problems:
            return 1
    return 0


def verify_lib(lib: Lib, root: str, cache: str, offline: bool) -> list:
    """Problems found for one library; prints `ok` when there are none."""
    lib_dir = os.path.join(root, "third_party", lib.name)
    try:
        tree = expected_tree(lib, get_archive(lib, cache, offline), lib_dir)
    except VendorError as e:
        return [str(e)]
    problems = check_tree(lib, lib_dir, tree)
    if os.path.isdir(lib_dir):
        problems += git_ignored(lib, root, lib_dir, tree)
    if not problems:
        print(f"ok   {lib.name} {lib.version} ({len(tree)} files)")
    return problems


def cmd_verify(libs: list, root: str, cache: str, offline: bool, scan_unlisted: bool,
               host=None, all_hosts: bool = False) -> int:
    problems = []
    for lib in libs:
        problems += verify_lib(lib, root, cache, offline)
        archives, note = hosts_to_check(lib, host, all_hosts)
        if note:
            print(note)
        for ha in archives:
            problems += check_host_archive(ha, cache, offline)
    # A directory under third_party/ that vendor.toml does not know breaks the policy.
    tp = os.path.join(root, "third_party")
    names = {lib.name for lib in libs}
    if scan_unlisted and os.path.isdir(tp):
        for d in sorted(os.listdir(tp)):
            if os.path.isdir(os.path.join(tp, d)) and not d.startswith(".") and d not in names:
                problems.append(f"third_party/{d}/ is not listed in vendor.toml")
    for p in problems:
        print(f"FAIL {p}", file=sys.stderr)
    return 1 if problems else 0


def cmd_list(libs: list) -> int:
    w = max([len(lib.name) for lib in libs] + [4])
    v = max([len(lib.version) for lib in libs] + [7])
    print(f"{'name':<{w}}  {'version':<{v}}  {'license':<24}  url")
    for lib in libs:
        print(f"{lib.name:<{w}}  {lib.version:<{v}}  {lib.license:<24}  {lib.url}")
        for ha in lib.hosts.values():
            print(f"{'':<{w}}  {'':<{v}}  {'prebuilt ' + ha.host:<24}  {ha.url}")
    return 0


def cmd_prebuilt(lib: Lib, host, cache: str, dest: str, offline: bool) -> int:
    """Extract the host's prebuilt files of `lib` into `dest` (cmake/DePrebuilt.cmake)."""
    if lib.kind != "prebuilt":
        raise ConfigError(f'{lib.name} is not a prebuilt entry (kind = "{lib.kind}")')
    if host not in lib.hosts:
        print(f"vendor.py: {lib.name} {lib.version} has no prebuilt archive for host "
              f"{host or 'unknown'} (listed: {', '.join(lib.hosts)})", file=sys.stderr)
        return EXIT_NO_HOST
    ha = lib.hosts[host]
    if prebuilt_up_to_date(ha, dest):
        print(f"{ha.name} {ha.version}: {dest} is up to date")
        return 0
    sel = select_prebuilt(ha, get_archive(ha, cache, offline))
    write_prebuilt(ha, sel, dest)
    print(f"{ha.name} {ha.version}: extracted {len(sel)} files into {dest}")
    return 0


def cmd_hash(src: str) -> int:
    if os.path.isfile(src):
        print(sha256_file(src))
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        dest = os.path.join(tmp, "archive")
        download(src, dest)
        print(sha256_file(dest))
    return 0


def default_cache() -> str:
    env = os.environ.get("DE_VENDOR_CACHE")
    if env:
        return os.path.abspath(os.path.expanduser(env))
    return os.path.join(os.path.expanduser("~"), ".cache", "dengine-vendor")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=DEFAULT_ROOT, help="repository root (default: %(default)s)")
    ap.add_argument("--config", help="pin list (default: <root>/third_party/vendor.toml)")
    ap.add_argument("--cache", default=None,
                    help="archive cache (default: $DE_VENDOR_CACHE or ~/.cache/dengine-vendor)")
    ap.add_argument("--toml-fallback", action="store_true",
                    help="use the built-in TOML reader even if tomllib exists (self-test)")
    def host_args(p, all_hosts: bool) -> None:
        p.add_argument("--host", help="prebuilt host, <macos|linux|windows>-<arm64|x64> "
                       "(default: this machine)")
        if all_hosts:
            p.add_argument("--all-hosts", action="store_true",
                           help="check the prebuilt archive of every host")

    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="download and (re)write third_party/<name>/")
    f.add_argument("names", nargs="*")
    f.add_argument("--all", action="store_true", help="every library in vendor.toml")
    host_args(f, True)
    v = sub.add_parser("verify", help="check hashes and that the trees match their archives")
    v.add_argument("names", nargs="*")
    v.add_argument("--offline", action="store_true", help="never download; a missing archive fails")
    host_args(v, True)
    pb = sub.add_parser("prebuilt", help="extract the host's prebuilt files into --dest")
    pb.add_argument("name")
    pb.add_argument("--dest", required=True, help="output directory (replaced as a whole)")
    pb.add_argument("--offline", action="store_true", help="never download; a missing archive fails")
    host_args(pb, False)
    sub.add_parser("list", help="print the pinned libraries")
    h = sub.add_parser("hash", help="print the sha256 of an archive (URL or local file)")
    h.add_argument("src")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    config = args.config or os.path.join(root, "third_party", "vendor.toml")
    cache = os.path.abspath(args.cache) if args.cache else default_cache()
    host = getattr(args, "host", None) or current_host()
    if getattr(args, "host", None) and not HOST_RE.match(args.host):
        ap.error(f"--host {args.host!r}: expected <macos|linux|windows>-<arm64|x64>")

    try:
        if args.cmd == "hash":
            return cmd_hash(args.src)
        libs = load_config(config, args.toml_fallback)
        if args.cmd == "list":
            return cmd_list(libs)
        names = getattr(args, "names", None)
        if args.cmd == "prebuilt":
            names = [args.name]
        by_name = {lib.name: lib for lib in libs}
        for n in names:
            if n not in by_name:
                raise ConfigError(f"unknown library {n!r} (see `vendor.py list`)")
        selected = [by_name[n] for n in names] if names else libs
        if args.cmd == "prebuilt":
            return cmd_prebuilt(selected[0], host, cache, os.path.abspath(args.dest),
                                args.offline)
        if args.cmd == "fetch":
            if not names and not args.all:
                ap.error("fetch needs library names or --all")
            return cmd_fetch(selected, root, cache, host, args.all_hosts)
        if args.cmd == "verify":
            # With names, only those trees are checked (no scan for unlisted directories).
            return cmd_verify(selected, root, cache, args.offline, scan_unlisted=not names,
                              host=host, all_hosts=args.all_hosts)
    except ConfigError as e:
        print(f"vendor.py: config error: {e}", file=sys.stderr)
        return 2
    except VendorError as e:
        print(f"vendor.py: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
