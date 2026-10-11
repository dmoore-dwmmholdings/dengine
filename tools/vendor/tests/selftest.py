#!/usr/bin/env python3
"""Self-test for tools/vendor/vendor.py. Offline: archives are built locally and fetched through
file:// URLs into a temporary fake repository. Exit status 0 when every case passes."""

from __future__ import annotations

import hashlib
import io
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
VENDOR = os.path.normpath(os.path.join(HERE, "..", "vendor.py"))
sys.dont_write_bytecode = True  # keep tools/vendor/ free of __pycache__
sys.path.insert(0, os.path.dirname(VENDOR))
import vendor  # noqa: E402

FAILS: list = []

UPSTREAM = {
    "LICENSE": b"MIT License\n\nCopyright (c) demo\n",
    "include/demo.h": b"#pragma once\nint demo(void);\n",
    "src/demo.c": b"#include \"demo.h\"\nint demo(void) { return 42; }\n",
    "src/extra/deep.c": b"int deep;\n",
    "docs/readme.md": b"not vendored\n",
    "tests/t.c": b"not vendored\n",
}

PATCH = b"""--- a/src/demo.c
+++ b/src/demo.c
@@ -1,2 +1,2 @@
 #include "demo.h"
-int demo(void) { return 42; }
+int demo(void) { return 43; }
"""


def check(cond: bool, what: str) -> None:
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def make_tar(path: str, files: dict, top: str = "demo-1.0", symlink: str | None = None) -> str:
    with tarfile.open(path, "w:gz") as tf:
        for rel, data in sorted(files.items()):
            info = tarfile.TarInfo(f"{top}/{rel}")
            info.size = len(data)
            info.mtime = 0
            tf.addfile(info, io.BytesIO(data))
        if symlink:
            info = tarfile.TarInfo(f"{top}/{symlink}")
            info.type = tarfile.SYMTYPE
            info.linkname = "src/demo.c"
            tf.addfile(info)
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def make_zip(path: str, files: dict, top: str = "demo-1.0") -> str:
    with zipfile.ZipFile(path, "w") as z:
        for rel, data in sorted(files.items()):
            z.writestr(f"{top}/{rel}", data)
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def entry(name: str, url: str, sha: str, **kw) -> str:
    fields = {
        "name": f'"{name}"',
        "version": '"1.0"',
        "url": f'"{url}"',
        "sha256": f'"{sha}"',
        "license": '"MIT"',
        "files": '[\n    "include/*.h",\n    "src/**",  # comment inside an array\n]',
    }
    for k, v in kw.items():
        if v is None:
            fields.pop(k, None)
        else:
            fields[k] = v
    return "[[lib]]\n" + "".join(f"{k} = {v}\n" for k, v in fields.items()) + "\n"


class Repo:
    def __init__(self, base: str):
        self.root = os.path.join(base, "repo")
        self.cache = os.path.join(base, "cache")
        self.tp = os.path.join(self.root, "third_party")
        os.makedirs(self.tp, exist_ok=True)

    def config(self, text: str) -> None:
        pathlib.Path(self.tp, "vendor.toml").write_text("# test pin list\n" + text)

    def run(self, *args: str, fallback: bool = False) -> subprocess.CompletedProcess:
        cmd = [sys.executable, VENDOR, "--root", self.root, "--cache", self.cache]
        if fallback:
            cmd.append("--toml-fallback")
        return subprocess.run(cmd + list(args), capture_output=True, text=True)

    def snapshot(self, name: str) -> dict:
        d = pathlib.Path(self.tp, name)
        return {str(p.relative_to(d)): p.read_bytes() for p in sorted(d.rglob("*")) if p.is_file()}


def test_globs() -> None:
    g = vendor.glob_to_regex
    check(bool(g("src/**").match("src/a/b.c")), "glob: src/** matches nested files")
    check(not g("src/*").match("src/a/b.c"), "glob: src/* stays in one component")
    check(bool(g("**/x.h").match("x.h")) and bool(g("**/x.h").match("a/b/x.h")),
          "glob: **/x.h matches at any depth")
    check(bool(g("lib/decompress/*.S").match("lib/decompress/huf_decompress_amd64.S")),
          "glob: *.S")
    check(not g("de_*").match("smoke/de_x.c"), "glob: de_* is top-level only")


def test_toml_fallback() -> None:
    text = 'a = "x # not a comment"  # comment\nb = [\n  "p",\n  \'q\',\n]\nc = 3\nd = true\n' \
           "[[lib]]\nname = \"n\"\n[[lib]]\nname = \"m\"\n"
    d = vendor.parse_toml_minimal(text)
    check(d == {"a": "x # not a comment", "b": ["p", "q"], "c": 3, "d": True,
                "lib": [{"name": "n"}, {"name": "m"}]}, "minimal TOML reader")


def main() -> int:
    test_globs()
    test_toml_fallback()

    with tempfile.TemporaryDirectory(prefix="de-vendor-selftest-") as base:
        repo = Repo(base)
        arch = os.path.join(base, "demo-1.0.tar.gz")
        sha = make_tar(arch, UPSTREAM)
        url = pathlib.Path(arch).as_uri()
        repo.config(entry("demo", url, sha))

        for fb in (False, True):
            r = repo.run("list", fallback=fb)
            check(r.returncode == 0 and "demo" in r.stdout,
                  f"list ({'fallback' if fb else 'tomllib'} reader)")

        # fetch writes exactly the selected files plus LICENSE.
        r = repo.run("fetch", "--all")
        check(r.returncode == 0, "fetch --all")
        snap = repo.snapshot("demo")
        check(sorted(snap) == ["LICENSE", "include/demo.h", "src/demo.c", "src/extra/deep.c"],
              f"fetched file set {sorted(snap)}")
        check(repo.run("verify").returncode == 0, "verify passes after fetch")
        check(repo.run("verify", fallback=True).returncode == 0, "verify with fallback reader")
        check(repo.run("verify", "--offline").returncode == 0, "verify --offline with cached archive")

        # Local engine files are kept by fetch and ignored by verify.
        d = pathlib.Path(repo.tp, "demo")
        (d / "CMakeLists.txt").write_text("add_library(de_tp_demo STATIC src/demo.c)\n")
        (d / "smoke").mkdir()
        (d / "smoke" / "smoke_demo.c").write_text("int main(void) { return 0; }\n")
        (d / "de_tp_demo.c").write_text("/* wrapper */\n")
        check(repo.run("verify").returncode == 0, "verify ignores local files")

        # One changed byte fails verify; fetch repairs it.
        p = d / "src" / "demo.c"
        data = bytearray(p.read_bytes())
        data[0] ^= 1
        p.write_bytes(bytes(data))
        r = repo.run("verify")
        check(r.returncode == 1 and "src/demo.c differs" in r.stderr, "verify detects one edited byte")
        check(repo.run("fetch", "demo").returncode == 0, "fetch repairs the edit")
        check(repo.run("verify").returncode == 0, "verify passes after repair")
        check((d / "CMakeLists.txt").exists() and (d / "smoke" / "smoke_demo.c").exists(),
              "fetch keeps local files")

        # Unexpected and missing files fail.
        (d / "src" / "stray.c").write_text("int stray;\n")
        r = repo.run("verify")
        check(r.returncode == 1 and "unexpected file src/stray.c" in r.stderr,
              "verify detects an unexpected file")
        (d / "src" / "stray.c").unlink()
        (d / "include" / "demo.h").unlink()
        r = repo.run("verify")
        check(r.returncode == 1 and "missing include/demo.h" in r.stderr,
              "verify detects a missing file")

        # From an emptied directory, fetch reproduces a byte-identical tree.
        for q in sorted(d.rglob("*"), reverse=True):
            if q.is_file() and not vendor.match_any(str(q.relative_to(d)),
                                                    [vendor.glob_to_regex(x)
                                                     for x in vendor.DEFAULT_LOCAL]):
                q.unlink()
        repo.run("fetch", "--all")
        snap2 = {k: v for k, v in repo.snapshot("demo").items()
                 if k in snap}
        check(snap2 == snap, "fetch from empty reproduces a byte-identical tree")

        # A directory not in vendor.toml fails the full verify.
        (pathlib.Path(repo.tp) / "rogue").mkdir()
        r = repo.run("verify")
        check(r.returncode == 1 and "third_party/rogue/" in r.stderr, "verify flags unlisted dir")
        check(repo.run("verify", "demo").returncode == 0, "verify <name> checks only that lib")
        (pathlib.Path(repo.tp) / "rogue").rmdir()

        # Wrong sha256: download fails the check and nothing is cached.
        repo.config(entry("demo", url, "0" * 64))
        r = repo.run("fetch", "demo")
        check(r.returncode == 1 and "sha256 mismatch" in r.stderr, "fetch rejects a wrong sha256")
        r = repo.run("verify", "--offline")
        check(r.returncode == 1 and "not in cache" in r.stderr, "verify --offline needs the archive")

        # Policy errors are config errors (exit 2).
        cases = {
            "entry without license": entry("demo", url, sha, license=None),
            "licence outside §22": entry("demo", url, sha, license='"GPL-3.0-only"'),
            "proprietary SDK name": entry("steamworks", url, sha),
            "duplicate entry": entry("demo", url, sha) + entry("demo", url, sha),
            "unknown key": entry("demo", url, sha, branch='"main"'),
            "bad sha256": entry("demo", url, sha, sha256='"abc"'),
        }
        for what, text in cases.items():
            repo.config(text)
            r = repo.run("list")
            check(r.returncode == 2, f"refuses {what}")

        # A glob that selects nothing and a selected symlink are errors.
        repo.config(entry("demo", url, sha, files='["nothing/*.c"]'))
        r = repo.run("verify")
        check(r.returncode == 1 and "matches nothing" in r.stderr, "files glob matching nothing")
        arch2 = os.path.join(base, "link-1.0.tar.gz")
        sha2 = make_tar(arch2, UPSTREAM, symlink="src/link.c")
        repo.config(entry("demo", pathlib.Path(arch2).as_uri(), sha2))
        r = repo.run("fetch", "demo")
        check(r.returncode == 1 and "symlink" in r.stderr, "selected symlink is refused")

        # Exclude, strip=0 + license_file, zip archives and patches.
        arch3 = os.path.join(base, "demo-zip.zip")
        sha3 = make_zip(arch3, UPSTREAM, top="demo-1.0")
        (d / "patches").mkdir(exist_ok=True)
        (d / "patches" / "0001-answer.patch").write_bytes(PATCH)
        repo.config(entry("demo", pathlib.Path(arch3).as_uri(), sha3,
                          exclude='["src/extra/**"]',
                          patches='["patches/0001-answer.patch"]'))
        r = repo.run("fetch", "demo")
        check(r.returncode == 0, f"fetch zip with exclude and patch {r.stderr.strip()}")
        check(not (d / "src" / "extra").exists(), "exclude removes files and empty dirs")
        check(b"return 43" in (d / "src" / "demo.c").read_bytes(), "patch applied")
        check(repo.run("verify").returncode == 0, "verify passes on the patched tree")

        repo.config(entry("demo", url, sha, strip="0", files='["demo-1.0/include/*.h"]',
                          license_file='"demo-1.0/LICENSE"'))
        r = repo.run("fetch", "demo")
        check(r.returncode == 0 and (d / "demo-1.0" / "include" / "demo.h").exists()
              and (d / "LICENSE").exists(), "strip = 0 with license_file")

    if FAILS:
        print(f"\n{len(FAILS)} case(s) failed", file=sys.stderr)
        return 1
    print("\nvendor selftest: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
